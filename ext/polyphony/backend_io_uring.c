#ifdef POLYPHONY_BACKEND_LIBURING

#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdnoreturn.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <errno.h>

#include "polyphony.h"
#include "liburing.h"
#include "backend_io_uring_context.h"
#include "ruby/thread.h"
#include "ruby/io.h"
#include "backend_common.h"

VALUE SYM_io_uring;
VALUE SYM_send;
VALUE SYM_splice;
VALUE SYM_write;

VALUE eArgumentError;

#ifdef POLYPHONY_UNSET_NONBLOCK
#define io_unset_nonblock(fptr, io) io_verify_blocking_mode(fptr, io, Qtrue)
#else
#define io_unset_nonblock(fptr, io)
#endif

typedef struct Backend_t {
  struct Backend_base base;

  // implementation-specific fields
  struct io_uring     ring;
  op_context_store_t  store;
  unsigned int        pending_sqes;
  unsigned int        prepared_limit;
  int                 ring_initialized;

  int                 event_fd;
  op_context_t        *event_fd_ctx;
} Backend_t;

static void Backend_mark(void *ptr) {
  Backend_t *backend = ptr;
  backend_base_mark(&backend->base);
  context_store_mark_taken_buffers(&backend->store);
}

static void Backend_free(void *ptr) {
  Backend_t *backend = ptr;
  backend_base_finalize(&backend->base);
}

static size_t Backend_size(const void *ptr) {
  return sizeof(Backend_t);
}

static const rb_data_type_t Backend_type = {
    "IOUringBackend",
    {Backend_mark, Backend_free, Backend_size,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE Backend_allocate(VALUE klass) {
  Backend_t *backend = ALLOC(Backend_t);

  return TypedData_Wrap_Struct(klass, &Backend_type, backend);
}

#define GetBackend(obj, backend) \
  TypedData_Get_Struct((obj), Backend_t, &Backend_type, (backend))

static VALUE Backend_initialize(VALUE self) {
  Backend_t *backend;
  GetBackend(self, backend);

  backend_base_initialize(&backend->base);
  backend->pending_sqes = 0;
  backend->ring_initialized = 0;
  backend->event_fd = -1;
  backend->event_fd_ctx = NULL;

  context_store_initialize(&backend->store);

  backend->prepared_limit = 1024;
  int flags = 0;
  #ifdef HAVE_IORING_SETUP_SUBMIT_ALL
  flags |= IORING_SETUP_SUBMIT_ALL;
  #endif
  #ifdef HAVE_IORING_SETUP_COOP_TASKRUN
  flags |= IORING_SETUP_COOP_TASKRUN;
  #endif

  while (1) {
    int ret = io_uring_queue_init(backend->prepared_limit, &backend->ring, flags);
    if (!ret) break;

    // if ENOMEM is returned, use a smaller limit
    if (ret == -ENOMEM && backend->prepared_limit > 64)
      backend->prepared_limit = backend->prepared_limit / 2;
    else
      rb_syserr_fail(-ret, strerror(-ret));
  }
  backend->ring_initialized = 1;

  return self;
}

VALUE Backend_finalize(VALUE self) {
  Backend_t *backend;
  GetBackend(self, backend);

  if (backend->ring_initialized) io_uring_queue_exit(&backend->ring);
  if (backend->event_fd != -1) close(backend->event_fd);
  context_store_free(&backend->store);
  return self;
}

VALUE Backend_post_fork(VALUE self) {
  Backend_t *backend;
  GetBackend(self, backend);

  io_uring_queue_exit(&backend->ring);
  io_uring_queue_init(backend->prepared_limit, &backend->ring, 0);
  context_store_free(&backend->store);
  backend_base_reset(&backend->base);

  return self;
}

typedef struct poll_context {
  struct io_uring     *ring;
  struct io_uring_cqe *cqe;
  int                 result;
} poll_context_t;

extern int __sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags, sigset_t *sig);

void *io_uring_backend_poll_without_gvl(void *ptr) {
  poll_context_t *ctx = (poll_context_t *)ptr;
  ctx->result = io_uring_wait_cqe(ctx->ring, &ctx->cqe);
  return NULL;
}

// copied from liburing/queue.c
static inline bool cq_ring_needs_flush(struct io_uring *ring) {
  return IO_URING_READ_ONCE(*ring->sq.kflags) & IORING_SQ_CQ_OVERFLOW;
}

static void handle_multishot_accept_completion(op_context_t *ctx, struct io_uring_cqe *cqe, Backend_t *backend) {
  // printf("handle_multishot_accept_completion result: %d\n", ctx->result);
  if (ctx->result == -ECANCELED) {
    context_store_release(&backend->store, ctx);
    rb_ivar_set(ctx->resume_value, ID_ivar_multishot_accept_queue, Qnil);
  }
  else {
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
      context_store_release(&backend->store, ctx);
    }
    VALUE queue = rb_ivar_get(ctx->resume_value, ID_ivar_multishot_accept_queue);
    if (queue != Qnil)
      Queue_push(queue, INT2NUM(ctx->result));
  }
}

static void handle_multishot_completion(op_context_t *ctx, struct io_uring_cqe *cqe, Backend_t *backend) {
  switch (ctx->type) {
    case OP_MULTISHOT_ACCEPT:
      return handle_multishot_accept_completion(ctx, cqe, backend);
    default:
      printf("Unexpected multishot completion for op type %d\n", ctx->type);
  }
}

static inline void io_uring_backend_handle_completion(struct io_uring_cqe *cqe, Backend_t *backend) {
  op_context_t *ctx = io_uring_cqe_get_data(cqe);
  if (!ctx) return;

  // printf("cqe ctx %p id: %d result: %d (%s, ref_count: %d)\n", ctx, ctx->id, cqe->res, op_type_to_str(ctx->type), ctx->ref_count);
  ctx->result = cqe->res;
  if (ctx->ref_count == MULTISHOT_REFCOUNT) {
    handle_multishot_completion(ctx, cqe, backend);
  }
  else {
    if (ctx->ref_count == 2 && ctx->result != -ECANCELED && ctx->fiber)
      Fiber_make_runnable(ctx->fiber, ctx->resume_value);
    context_store_release(&backend->store, ctx);
  }
}

// adapted from io_uring_peek_batch_cqe in liburing/queue.c
// this peeks at cqes and handles each available cqe
void io_uring_backend_handle_ready_cqes(Backend_t *backend) {
  struct io_uring *ring = &backend->ring;
  bool overflow_checked = false;
  struct io_uring_cqe *cqe;
  unsigned head;
  unsigned cqe_count;

again:
  cqe_count = 0;
  io_uring_for_each_cqe(ring, head, cqe) {
    ++cqe_count;
    io_uring_backend_handle_completion(cqe, backend);
  }
  io_uring_cq_advance(ring, cqe_count);

  if (overflow_checked) goto done;

  if (cq_ring_needs_flush(ring)) {
    __sys_io_uring_enter(ring->ring_fd, 0, 0, IORING_ENTER_GETEVENTS, NULL);
    overflow_checked = true;
    goto again;
  }

done:
  return;
}

inline void io_uring_backend_immediate_submit(Backend_t *backend) {
  backend->pending_sqes = 0;
  io_uring_submit(&backend->ring);
}

inline void io_uring_backend_defer_submit(Backend_t *backend) {
  backend->pending_sqes += 1;
  if (backend->pending_sqes >= backend->prepared_limit)
    io_uring_backend_immediate_submit(backend);
}

void io_uring_backend_poll(Backend_t *backend) {
  poll_context_t poll_ctx;
  poll_ctx.ring = &backend->ring;
  if (backend->pending_sqes) io_uring_backend_immediate_submit(backend);

wait_cqe:
  backend->base.currently_polling = 1;
  rb_thread_call_without_gvl(io_uring_backend_poll_without_gvl, (void *)&poll_ctx, RUBY_UBF_IO, 0);
  backend->base.currently_polling = 0;
  if (poll_ctx.result < 0) {
    if (poll_ctx.result == -EINTR && runqueue_empty_p(&backend->base.runqueue)) goto wait_cqe;
    return;
  }

  io_uring_backend_handle_completion(poll_ctx.cqe, backend);
  io_uring_cqe_seen(&backend->ring, poll_ctx.cqe);
}

inline VALUE Backend_poll(VALUE self, VALUE blocking) {
  int is_blocking = blocking == Qtrue;
  Backend_t *backend;
  GetBackend(self, backend);

  backend->base.poll_count++;

  if (!is_blocking && backend->pending_sqes) io_uring_backend_immediate_submit(backend);

  COND_TRACE(&backend->base, 2, SYM_enter_poll, rb_fiber_current());

  if (is_blocking) io_uring_backend_poll(backend);
  io_uring_backend_handle_ready_cqes(backend);

  COND_TRACE(&backend->base, 2, SYM_leave_poll, rb_fiber_current());

  return self;
}

inline void Backend_schedule_fiber(VALUE thread, VALUE self, VALUE fiber, VALUE value, int prioritize) {
  Backend_t *backend;
  GetBackend(self, backend);

  backend_base_schedule_fiber(thread, self, &backend->base, fiber, value, prioritize);
}

inline void Backend_unschedule_fiber(VALUE self, VALUE fiber) {
  Backend_t *backend;
  GetBackend(self, backend);

  runqueue_delete(&backend->base.runqueue, fiber);
}

inline VALUE Backend_switch_fiber(VALUE self) {
  Backend_t *backend;
  GetBackend(self, backend);

  return backend_base_switch_fiber(self, &backend->base);
}

inline struct backend_stats backend_get_stats(VALUE self) {
  Backend_t *backend;
  GetBackend(self, backend);

  return backend_base_stats(&backend->base);
}

static inline struct io_uring_sqe *io_uring_backend_get_sqe(Backend_t *backend) {
  struct io_uring_sqe *sqe;
  sqe = io_uring_get_sqe(&backend->ring);
  if (sqe) goto done;

  if (backend->pending_sqes)
    io_uring_backend_immediate_submit(backend);
  else {
    VALUE resume_value = backend_snooze(&backend->base);
    RAISE_IF_EXCEPTION(resume_value);
  }
done:
  return sqe;
}

VALUE Backend_wakeup(VALUE self) {
  Backend_t *backend;
  GetBackend(self, backend);

  if (backend->base.currently_polling) {
    // Since we're currently blocking while waiting for a completion, we add a
    // NOP which would cause the io_uring_enter syscall to return
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_backend_immediate_submit(backend);

    return Qtrue;
  }

  return Qnil;
}

int io_uring_backend_defer_submit_and_await(
  Backend_t *backend,
  struct io_uring_sqe *sqe,
  op_context_t *ctx,
  VALUE *value_ptr
)
{
  VALUE switchpoint_result = Qnil;

  backend->base.op_count++;
  if (sqe) io_uring_sqe_set_data(sqe, ctx);
  io_uring_backend_defer_submit(backend);

  switchpoint_result = backend_await((struct Backend_base *)backend);

  if (ctx->ref_count > 1) {
    struct io_uring_sqe *sqe;

    // op was not completed (an exception was raised), so we need to cancel it
    ctx->result = -ECANCELED;
    sqe = io_uring_backend_get_sqe(backend);
    io_uring_prep_cancel(sqe, ctx, 0);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_backend_immediate_submit(backend);
  }

  if (value_ptr) (*value_ptr) = switchpoint_result;
  RB_GC_GUARD(switchpoint_result);
  RB_GC_GUARD(ctx->fiber);
  return ctx->result;
}

VALUE io_uring_backend_wait_fd(Backend_t *backend, int fd, int write) {
  op_context_t *ctx = context_store_acquire(&backend->store, OP_POLL);
  VALUE resumed_value = Qnil;

  struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
  io_uring_prep_poll_add(sqe, fd, write ? POLLOUT : POLLIN);

  io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resumed_value);
  context_store_release(&backend->store, ctx);

  RB_GC_GUARD(resumed_value);
  return resumed_value;
}

static inline int fd_from_io(VALUE io, rb_io_t **fptr, int write_mode, int rectify_file_pos) {
  if (rb_obj_class(io) == cPipe) {
    *fptr = NULL;
    return Pipe_get_fd(io, write_mode);
  }
  else {
    VALUE underlying_io = rb_ivar_get(io, ID_ivar_io);
    if (underlying_io != Qnil) io = underlying_io;

    GetOpenFile(io, *fptr);
    io_unset_nonblock(*fptr, io);
    if (rectify_file_pos) rectify_io_file_pos(*fptr);

    return (*fptr)->fd;
  }
}

VALUE Backend_read(VALUE self, VALUE io, VALUE buffer, VALUE length, VALUE to_eof, VALUE pos) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  struct backend_buffer_spec buffer_spec = backend_get_buffer_spec(buffer, 0);
  long total = 0;
  int read_to_eof = RTEST(to_eof);

  GetBackend(self, backend);
  backend_prepare_read_buffer(buffer, length, &buffer_spec, FIX2INT(pos));
  fd = fd_from_io(io, &fptr, 0, 1);

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_READ);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_read(sqe, fd, buffer_spec.ptr, buffer_spec.len, -1);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else if (!result)
      break; // EOF
    else {
      total += result;
      if (!read_to_eof) break;

      if (result == buffer_spec.len) {
        if (buffer_spec.expandable)
          backend_grow_string_buffer(buffer, &buffer_spec, total);
        else
          break;
      }
      else {
        buffer_spec.ptr += result;
        buffer_spec.len -= result;
        if (!buffer_spec.len) break;
      }
    }
  }

  if (!total) return Qnil;

  if (!buffer_spec.raw) backend_finalize_string_buffer(buffer, &buffer_spec, total, fptr);
  return buffer_spec.raw ? INT2FIX(total) : buffer;
}

VALUE Backend_read_loop(VALUE self, VALUE io, VALUE maxlen) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  VALUE buffer;
  long total;
  char *ptr;
  long len = FIX2INT(maxlen);
  int shrinkable;

  READ_LOOP_PREPARE_STR();

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, 0, 1);

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_READ);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    ssize_t result;
    int completed;

    io_uring_prep_read(sqe, fd, ptr, len, -1);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else if (!result)
      break; // EOF
    else {
      total = result;
      READ_LOOP_YIELD_STR();
    }
  }

  RB_GC_GUARD(buffer);

  return io;
}

VALUE Backend_feed_loop(VALUE self, VALUE io, VALUE receiver, VALUE method) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  VALUE buffer;
  long total;
  char *ptr;
  long len = 8192;
  int shrinkable;
  ID method_id = SYM2ID(method);

  READ_LOOP_PREPARE_STR();

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, 0, 1);

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_READ);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    ssize_t result;
    int completed;

    io_uring_prep_read(sqe, fd, ptr, len, -1);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else if (!result)
      break; // EOF
    else {
      total = result;
      READ_LOOP_PASS_STR_TO_RECEIVER(receiver, method_id);
    }
  }

  RB_GC_GUARD(buffer);

  return io;
}

VALUE Backend_write(VALUE self, VALUE io, VALUE buffer) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;

  struct backend_buffer_spec buffer_spec = backend_get_buffer_spec(buffer, 1);
  long left = buffer_spec.len;

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, 1, 0);

  while (left > 0) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_WRITE);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_write(sqe, fd, buffer_spec.ptr, left, -1);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else {
      buffer_spec.ptr += result;
      left -= result;
    }
  }

  return INT2FIX(buffer_spec.len);
}

VALUE Backend_writev(VALUE self, VALUE io, int argc, VALUE *argv) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  long total_length = 0;
  long total_written = 0;
  struct iovec *iov = 0;
  struct iovec *iov_ptr = 0;
  int iov_count = argc;

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, 1, 0);

  iov = malloc(iov_count * sizeof(struct iovec));
  for (int i = 0; i < argc; i++) {
    VALUE buffer = argv[i];
    iov[i].iov_base = StringValuePtr(buffer);
    iov[i].iov_len = RSTRING_LEN(buffer);
    total_length += iov[i].iov_len;
  }
  iov_ptr = iov;

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_WRITEV);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_writev(sqe, fd, iov_ptr, iov_count, -1);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      free(iov);
      context_attach_buffers(ctx, argc, argv);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0) {
      free(iov);
      rb_syserr_fail(-result, strerror(-result));
    }
    else {
      total_written += result;
      if (total_written >= total_length) break;

      while (result > 0) {
        if ((size_t) result < iov_ptr[0].iov_len) {
          iov_ptr[0].iov_base = (char *) iov_ptr[0].iov_base + result;
          iov_ptr[0].iov_len -= result;
          result = 0;
        }
        else {
          result -= iov_ptr[0].iov_len;
          iov_ptr += 1;
          iov_count -= 1;
        }
      }
    }
  }

  free(iov);
  return INT2FIX(total_written);
}

VALUE Backend_write_m(int argc, VALUE *argv, VALUE self) {
  if (argc < 2)
    rb_raise(eArgumentError, "(wrong number of arguments (expected 2 or more))");

  return (argc == 2) ?
    Backend_write(self, argv[0], argv[1]) :
    Backend_writev(self, argv[0], argc - 1, argv + 1);
}

VALUE Backend_recv(VALUE self, VALUE io, VALUE buffer, VALUE length, VALUE pos) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  struct backend_buffer_spec buffer_spec = backend_get_buffer_spec(buffer, 0);
  long total = 0;

  GetBackend(self, backend);
  backend_prepare_read_buffer(buffer, length, &buffer_spec, FIX2INT(pos));
  fd = fd_from_io(io, &fptr, 0, 0);

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_RECV);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_recv(sqe, fd, buffer_spec.ptr, buffer_spec.len, 0);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else {
      total += result;
      break;
    }
  }

  if (!total) return Qnil;

  if (!buffer_spec.raw) backend_finalize_string_buffer(buffer, &buffer_spec, total, fptr);
  return buffer_spec.raw ? INT2FIX(total) : buffer;
}

VALUE Backend_recvmsg(VALUE self, VALUE io, VALUE buffer, VALUE maxlen, VALUE pos, VALUE flags, VALUE maxcontrollen, VALUE opts) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  struct backend_buffer_spec buffer_spec = backend_get_buffer_spec(buffer, 0);
  long total = 0;

  GetBackend(self, backend);
  backend_prepare_read_buffer(buffer, maxlen, &buffer_spec, FIX2INT(pos));
  fd = fd_from_io(io, &fptr, 0, 0);

  char addr_buffer[64];
  struct iovec iov;
  struct msghdr msg;

  iov.iov_base = StringValuePtr(buffer);
  iov.iov_len = maxlen;

  msg.msg_name = addr_buffer;
  msg.msg_namelen = sizeof(addr_buffer);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = 0;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_RECVMSG);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_recvmsg(sqe, fd, &msg, NUM2INT(flags));

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else {
      total += result;
      break;
    }
  }

  if (!total) return Qnil;

  if (!buffer_spec.raw) backend_finalize_string_buffer(buffer, &buffer_spec, total, fptr);
  VALUE addr = name_to_addrinfo(msg.msg_name, msg.msg_namelen);
  VALUE rflags = INT2NUM(msg.msg_flags);
  return rb_ary_new_from_args(3, buffer, addr, rflags);
  RB_GC_GUARD(addr);
}

VALUE Backend_recv_loop(VALUE self, VALUE io, VALUE maxlen) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  VALUE buffer;
  long total;
  char *ptr;
  long len = FIX2INT(maxlen);
  int shrinkable;

  READ_LOOP_PREPARE_STR();

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, 0, 0);

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_RECV);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_recv(sqe, fd, ptr, len, 0);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else if (!result)
      break; // EOF
    else {
      total = result;
      READ_LOOP_YIELD_STR();
    }
  }

  RB_GC_GUARD(buffer);
  return io;
}

VALUE Backend_recv_feed_loop(VALUE self, VALUE io, VALUE receiver, VALUE method) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  VALUE buffer;
  long total;
  char *ptr;
  long len = 8192;
  int shrinkable;
  ID method_id = SYM2ID(method);

  READ_LOOP_PREPARE_STR();

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, 0, 0);

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_RECV);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_recv(sqe, fd, ptr, len, 0);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else if (!result)
      break; // EOF
    else {
      total = result;
      READ_LOOP_PASS_STR_TO_RECEIVER(receiver, method_id);
    }
  }

  RB_GC_GUARD(buffer);
  return io;
}

VALUE Backend_send(VALUE self, VALUE io, VALUE buffer, VALUE flags) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;

  struct backend_buffer_spec buffer_spec = backend_get_buffer_spec(buffer, 1);
  long left = buffer_spec.len;
  int flags_int = FIX2INT(flags);

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, 1, 0);

  while (left > 0) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_SEND);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_send(sqe, fd, buffer_spec.ptr, left, flags_int);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else {
      buffer_spec.ptr += result;
      left -= result;
    }
  }

  return INT2FIX(buffer_spec.len);
}

VALUE Backend_sendmsg(VALUE self, VALUE io, VALUE buffer, VALUE flags, VALUE dest_sockaddr, VALUE controls) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;

  struct backend_buffer_spec buffer_spec = backend_get_buffer_spec(buffer, 1);
  long left = buffer_spec.len;
  int flags_int = FIX2INT(flags);

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, 1, 0);

  struct iovec iov;
  struct msghdr msg;

  iov.iov_base = buffer_spec.ptr;
  iov.iov_len = buffer_spec.len;

  if (dest_sockaddr != Qnil) {
    msg.msg_name = RSTRING_PTR(dest_sockaddr);
    msg.msg_namelen = RSTRING_LEN(dest_sockaddr);
  }
  else {
    msg.msg_name = 0;
    msg.msg_namelen = 0;
  }
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = 0;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

  while (left > 0) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_SENDMSG);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_sendmsg(sqe, fd, &msg, flags_int);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    if (!completed) {
      context_attach_buffers(ctx, 1, &buffer);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }
    RB_GC_GUARD(resume_value);

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));
    else {
      iov.iov_base += result;
      iov.iov_len -= result;
      left -= result;
    }
  }

  return INT2FIX(buffer_spec.len);
}

VALUE io_uring_backend_accept(Backend_t *backend, VALUE server_socket, VALUE socket_class, int loop) {
  int server_fd;
  rb_io_t *server_fptr;
  struct sockaddr addr;
  socklen_t len = (socklen_t)sizeof addr;
  VALUE socket = Qnil;

  server_fd = fd_from_io(server_socket, &server_fptr, 0, 0);

  while (1) {
    VALUE resume_value = Qnil;
    op_context_t *ctx = context_store_acquire(&backend->store, OP_ACCEPT);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int fd;
    int completed;

    io_uring_prep_accept(sqe, server_fd, &addr, &len, 0);

    fd = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    RAISE_IF_EXCEPTION(resume_value);
    if (!completed) return resume_value;
    RB_GC_GUARD(resume_value);

    if (fd < 0)
      rb_syserr_fail(-fd, strerror(-fd));
    else {
      rb_io_t *fp;

      socket = rb_obj_alloc(socket_class);
      MakeOpenFile(socket, fp);
      rb_update_max_fd(fd);
      fp->fd = fd;
      fp->mode = FMODE_READWRITE | FMODE_DUPLEX;
      rb_io_ascii8bit_binmode(socket);
      rb_io_synchronized(fp);

      // if (rsock_do_not_reverse_lookup) {
      //   fp->mode |= FMODE_NOREVLOOKUP;
      // }
      if (loop) {
        rb_yield(socket);
        socket = Qnil;
      }
      else
        return socket;
    }
  }
  RB_GC_GUARD(socket);
  return Qnil;
}

VALUE Backend_accept(VALUE self, VALUE server_socket, VALUE socket_class) {
#ifdef HAVE_IO_URING_PREP_MULTISHOT_ACCEPT
  VALUE accept_queue = rb_ivar_get(server_socket, ID_ivar_multishot_accept_queue);
  if (accept_queue != Qnil) {
    VALUE next = Queue_shift(0, 0, accept_queue);
    int fd = NUM2INT(next);
    if (fd < 0)
      rb_syserr_fail(-fd, strerror(-fd));
    else {
      rb_io_t *fp;

      VALUE socket = rb_obj_alloc(socket_class);
      MakeOpenFile(socket, fp);
      rb_update_max_fd(fd);
      fp->fd = fd;
      fp->mode = FMODE_READWRITE | FMODE_DUPLEX;
      rb_io_ascii8bit_binmode(socket);
      rb_io_synchronized(fp);
      return socket;
    }
  }
#endif

  Backend_t *backend;
  GetBackend(self, backend);
  return io_uring_backend_accept(backend, server_socket, socket_class, 0);
}

#ifdef HAVE_IO_URING_PREP_MULTISHOT_ACCEPT

struct multishot_accept_ctx {
  Backend_t *backend;
  VALUE server_socket;
  op_context_t *op_ctx;
};

VALUE multishot_accept_start(struct multishot_accept_ctx *ctx) {
  int server_fd;
  rb_io_t *server_fptr;
  server_fd = fd_from_io(ctx->server_socket, &server_fptr, 0, 0);
  VALUE accept_queue = rb_funcall(cQueue, ID_new, 0);
  rb_ivar_set(ctx->server_socket, ID_ivar_multishot_accept_queue, accept_queue);

  ctx->op_ctx = context_store_acquire(&ctx->backend->store, OP_MULTISHOT_ACCEPT);
  ctx->op_ctx->ref_count = -1;
  ctx->op_ctx->resume_value = ctx->server_socket;
  struct io_uring_sqe *sqe = io_uring_backend_get_sqe(ctx->backend);
  io_uring_prep_multishot_accept(sqe, server_fd, 0, 0, 0);
  io_uring_sqe_set_data(sqe, ctx->op_ctx);
  io_uring_backend_defer_submit(ctx->backend);

  rb_yield(ctx->server_socket);

  return Qnil;
}

VALUE multishot_accept_cleanup(struct multishot_accept_ctx *ctx) {
  struct io_uring_sqe *sqe = io_uring_backend_get_sqe(ctx->backend);
  io_uring_prep_cancel(sqe, ctx->op_ctx, 0);
  io_uring_sqe_set_data(sqe, NULL);
  io_uring_backend_defer_submit(ctx->backend);

  rb_ivar_set(ctx->server_socket, ID_ivar_multishot_accept_queue, Qnil);

  return Qnil;
}

VALUE Backend_multishot_accept(VALUE self, VALUE server_socket) {
  Backend_t *backend;
  GetBackend(self, backend);

  struct multishot_accept_ctx ctx;
  ctx.backend = backend;
  ctx.server_socket = server_socket;

  return rb_ensure(
    SAFE(multishot_accept_start), (VALUE)&ctx,
    SAFE(multishot_accept_cleanup), (VALUE)&ctx
  );
}

#endif

VALUE Backend_accept_loop(VALUE self, VALUE server_socket, VALUE socket_class) {
#ifdef HAVE_IO_URING_PREP_MULTISHOT_ACCEPT
  VALUE accept_queue = rb_ivar_get(server_socket, ID_ivar_multishot_accept_queue);
  if (accept_queue != Qnil) {
    while (true) {
      VALUE next = Queue_shift(0, 0, accept_queue);
      int fd = NUM2INT(next);
      if (fd < 0)
        rb_syserr_fail(-fd, strerror(-fd));
      else {
        rb_io_t *fp;

        VALUE socket = rb_obj_alloc(socket_class);
        MakeOpenFile(socket, fp);
        rb_update_max_fd(fd);
        fp->fd = fd;
        fp->mode = FMODE_READWRITE | FMODE_DUPLEX;
        rb_io_ascii8bit_binmode(socket);
        rb_io_synchronized(fp);
        rb_yield(socket);
      }
    }
    return self;
  }
#endif

  Backend_t *backend;
  GetBackend(self, backend);
  io_uring_backend_accept(backend, server_socket, socket_class, 1);
  return self;
}

VALUE io_uring_backend_splice(Backend_t *backend, VALUE src, VALUE dest, int maxlen) {
  int src_fd;
  int dest_fd;
  rb_io_t *src_fptr;
  rb_io_t *dest_fptr;
  int total = 0;
  VALUE resume_value = Qnil;
  int splice_to_eof = maxlen < 0;
  if (splice_to_eof) maxlen = -maxlen;

  src_fd = fd_from_io(src, &src_fptr, 0, 0);
  dest_fd = fd_from_io(dest, &dest_fptr, 1, 0);

  while (1) {
    op_context_t *ctx = context_store_acquire(&backend->store, OP_SPLICE);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_splice(sqe, src_fd, -1, dest_fd, -1, maxlen, 0);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    RAISE_IF_EXCEPTION(resume_value);
    if (!completed) return resume_value;

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));

    total += result;
    if (!result || !splice_to_eof) return INT2FIX(total);
  }

  RB_GC_GUARD(resume_value);
}

VALUE Backend_splice(VALUE self, VALUE src, VALUE dest, VALUE maxlen) {
  Backend_t *backend;
  GetBackend(self, backend);
  return io_uring_backend_splice(backend, src, dest, FIX2INT(maxlen));
}

struct double_splice_ctx {
  Backend_t *backend;
  VALUE src;
  VALUE dest;
  int pipefd[2];
};

#define DOUBLE_SPLICE_MAXLEN (1 << 16)

static inline op_context_t *prepare_double_splice_ctx(Backend_t *backend, int src_fd, int dest_fd) {
  op_context_t *ctx = context_store_acquire(&backend->store, OP_SPLICE);
  struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
  io_uring_prep_splice(sqe, src_fd, -1, dest_fd, -1, DOUBLE_SPLICE_MAXLEN, 0);
  io_uring_sqe_set_data(sqe, ctx);
  backend->base.op_count += 1;
  backend->pending_sqes += 1;

  return ctx;
}

static inline void io_uring_backend_cancel(Backend_t *backend, op_context_t *ctx) {
  struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
  ctx->result = -ECANCELED;
  io_uring_prep_cancel(sqe, ctx, 0);
  io_uring_sqe_set_data(sqe, NULL);
}

VALUE double_splice_safe(struct double_splice_ctx *ctx) {
  int src_fd;
  int dest_fd;
  rb_io_t *src_fptr;
  rb_io_t *dest_fptr;
  int total = 0;
  VALUE resume_value = Qnil;

  src_fd = fd_from_io(ctx->src, &src_fptr, 0, 0);
  dest_fd = fd_from_io(ctx->dest, &dest_fptr, 1, 0);

  op_context_t *ctx_src = prepare_double_splice_ctx(ctx->backend, src_fd, ctx->pipefd[1]);
  op_context_t *ctx_dest = prepare_double_splice_ctx(ctx->backend, ctx->pipefd[0], dest_fd);

  if (ctx->backend->pending_sqes >= ctx->backend->prepared_limit)
    io_uring_backend_immediate_submit(ctx->backend);

  while (1) {
    resume_value = backend_await((struct Backend_base *)ctx->backend);

    if ((ctx_src && ctx_src->ref_count == 2 && ctx_dest && ctx_dest->ref_count == 2) || TEST_EXCEPTION(resume_value)) {
      if (ctx_src) {
        context_store_release(&ctx->backend->store, ctx_src);
        io_uring_backend_cancel(ctx->backend, ctx_src);
      }
      if (ctx_dest) {
        context_store_release(&ctx->backend->store, ctx_dest);
        io_uring_backend_cancel(ctx->backend, ctx_dest);
      }
      io_uring_backend_immediate_submit(ctx->backend);
      RAISE_IF_EXCEPTION(resume_value);
      return resume_value;
    }

    if (ctx_src && ctx_src->ref_count == 1) {
      context_store_release(&ctx->backend->store, ctx_src);
      if (ctx_src->result == 0) {
        // close write end of pipe
        close(ctx->pipefd[1]);
        ctx_src = NULL;
      }
      else {
        ctx_src = prepare_double_splice_ctx(ctx->backend, src_fd, ctx->pipefd[1]);
      }
    }
    if (ctx_dest && ctx_dest->ref_count == 1) {
      context_store_release(&ctx->backend->store, ctx_dest);
      if (ctx_dest->result == 0)
        break;
      else {
        total += ctx_dest->result;
        ctx_dest = prepare_double_splice_ctx(ctx->backend, ctx->pipefd[0], dest_fd);
      }
    }

    if (ctx->backend->pending_sqes >= ctx->backend->prepared_limit)
      io_uring_backend_immediate_submit(ctx->backend);
  }
  RB_GC_GUARD(resume_value);
  return INT2FIX(total);
}

VALUE double_splice_cleanup(struct double_splice_ctx *ctx) {
  if (ctx->pipefd[0]) close(ctx->pipefd[0]);
  if (ctx->pipefd[1]) close(ctx->pipefd[1]);
  return Qnil;
}

VALUE Backend_double_splice(VALUE self, VALUE src, VALUE dest) {
  struct double_splice_ctx ctx = { NULL, src, dest, {0, 0} };
  GetBackend(self, ctx.backend);
  if (pipe(ctx.pipefd) == -1) rb_syserr_fail(errno, strerror(errno));

  return rb_ensure(
    SAFE(double_splice_safe), (VALUE)&ctx,
    SAFE(double_splice_cleanup), (VALUE)&ctx
  );
}

VALUE Backend_tee(VALUE self, VALUE src, VALUE dest, VALUE maxlen) {
  Backend_t *backend;
  GetBackend(self, backend);

  int src_fd;
  int dest_fd;
  rb_io_t *src_fptr;
  rb_io_t *dest_fptr;
  VALUE resume_value = Qnil;

  src_fd = fd_from_io(src, &src_fptr, 0, 0);
  dest_fd = fd_from_io(dest, &dest_fptr, 1, 0);

  while (1) {
    op_context_t *ctx = context_store_acquire(&backend->store, OP_SPLICE);
    struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
    int result;
    int completed;

    io_uring_prep_tee(sqe, src_fd, dest_fd, FIX2INT(maxlen), 0);

    result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
    completed = context_store_release(&backend->store, ctx);
    RAISE_IF_EXCEPTION(resume_value);
    if (!completed) return resume_value;

    if (result < 0)
      rb_syserr_fail(-result, strerror(-result));

    return INT2FIX(result);
  }

  RB_GC_GUARD(resume_value);
}

VALUE Backend_connect(VALUE self, VALUE sock, VALUE host, VALUE port) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  struct sockaddr *ai_addr;
  int ai_addrlen;
  VALUE resume_value = Qnil;
  op_context_t *ctx;
  struct io_uring_sqe *sqe;
  int result;
  int completed;

  ai_addrlen = backend_getaddrinfo(host, port, &ai_addr);

  GetBackend(self, backend);
  fd = fd_from_io(sock, &fptr, 1, 0);
  ctx = context_store_acquire(&backend->store, OP_CONNECT);
  sqe = io_uring_backend_get_sqe(backend);
  io_uring_prep_connect(sqe, fd, ai_addr, ai_addrlen);
  result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
  completed = context_store_release(&backend->store, ctx);
  RAISE_IF_EXCEPTION(resume_value);
  if (!completed) return resume_value;
  RB_GC_GUARD(resume_value);

  if (result < 0) rb_syserr_fail(-result, strerror(-result));
  return sock;
}

VALUE Backend_wait_io(VALUE self, VALUE io, VALUE write) {
  Backend_t *backend;
  int fd;
  rb_io_t *fptr;
  VALUE resume_value;
  int write_mode = RTEST(write);

  GetBackend(self, backend);
  fd = fd_from_io(io, &fptr, write_mode, 0);
  resume_value = io_uring_backend_wait_fd(backend, fd, write_mode);

  RAISE_IF_EXCEPTION(resume_value);
  RB_GC_GUARD(resume_value);
  return self;
}

// VALUE Backend_close(VALUE self, VALUE io) {
//   Backend_t *backend;
//   rb_io_t *fptr;
//   VALUE resume_value = Qnil;
//   op_context_t *ctx;
//   struct io_uring_sqe *sqe;
//   int result;
//   int completed;

//   if (fd < 0) return Qnil;

//   io_unset_nonblock(fptr, io);

//   ctx = context_store_acquire(&backend->store, OP_CLOSE);
//   sqe = io_uring_backend_get_sqe(backend);
//   io_uring_prep_close(sqe, fd);
//   result = io_uring_backend_defer_submit_and_await(backend, sqe, ctx, &resume_value);
//   completed = context_store_release(&backend->store, ctx);
//   RAISE_IF_EXCEPTION(resume_value);
//   if (!completed) return resume_value;
//   RB_GC_GUARD(resume_value);

//   if (result < 0) rb_syserr_fail(-result, strerror(-result));

//   fptr_finalize(fptr);
//   // fd = -1;
//   return io;
// }

inline struct __kernel_timespec double_to_timespec(double duration) {
  double duration_integral;
  double duration_fraction = modf(duration, &duration_integral);
  struct __kernel_timespec ts;
  ts.tv_sec = duration_integral;
  ts.tv_nsec = floor(duration_fraction * 1000000000);
  return ts;
}

inline struct __kernel_timespec duration_to_timespec(VALUE duration) {
  return double_to_timespec(NUM2DBL(duration));
}

// returns true if completed, 0 otherwise
int io_uring_backend_submit_timeout_and_await(Backend_t *backend, double duration, VALUE *resume_value) {
  struct __kernel_timespec ts = double_to_timespec(duration);
  struct io_uring_sqe *sqe = io_uring_backend_get_sqe(backend);
  op_context_t *ctx = context_store_acquire(&backend->store, OP_TIMEOUT);

  io_uring_prep_timeout(sqe, &ts, 0, 0);
  io_uring_backend_defer_submit_and_await(backend, sqe, ctx, resume_value);
  return context_store_release(&backend->store, ctx);
}

VALUE Backend_sleep(VALUE self, VALUE duration) {
  VALUE resume_value = Qnil;
  Backend_t *backend;
  GetBackend(self, backend);

  io_uring_backend_submit_timeout_and_await(backend, NUM2DBL(duration), &resume_value);
  RAISE_IF_EXCEPTION(resume_value);
  RB_GC_GUARD(resume_value);
  return resume_value;
}

VALUE Backend_timer_loop(VALUE self, VALUE interval) {
  Backend_t *backend;
  uint64_t interval_ns = NUM2DBL(interval) * 1e9;
  uint64_t next_time_ns = 0;
  VALUE resume_value = Qnil;

  GetBackend(self, backend);

  while (1) {
    double now_ns = current_time_ns();
    if (next_time_ns == 0) next_time_ns = now_ns + interval_ns;
    if (next_time_ns > now_ns) {
      double sleep_duration = ((double)(next_time_ns - now_ns))/1e9;
      int completed = io_uring_backend_submit_timeout_and_await(backend, sleep_duration, &resume_value);
      RAISE_IF_EXCEPTION(resume_value);
      if (!completed) return resume_value;
    }
    else {
      resume_value = backend_snooze(&backend->base);
      RAISE_IF_EXCEPTION(resume_value);
    }

    rb_yield(Qnil);

    while (1) {
      next_time_ns += interval_ns;
      if (next_time_ns > now_ns) break;
    }
  }
  RB_GC_GUARD(resume_value);
}

struct Backend_timeout_ctx {
  Backend_t *backend;
  op_context_t *ctx;
};

VALUE Backend_timeout_ensure(VALUE arg) {
  struct Backend_timeout_ctx *timeout_ctx = (struct Backend_timeout_ctx *)arg;
  if (timeout_ctx->ctx->ref_count) {
    struct io_uring_sqe *sqe;

    timeout_ctx->ctx->result = -ECANCELED;
    // op was not completed, so we need to cancel it
    sqe = io_uring_get_sqe(&timeout_ctx->backend->ring);
    io_uring_prep_cancel(sqe, timeout_ctx->ctx, 0);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_backend_immediate_submit(timeout_ctx->backend);
  }
  context_store_release(&timeout_ctx->backend->store, timeout_ctx->ctx);
  return Qnil;
}

VALUE Backend_timeout(int argc, VALUE *argv, VALUE self) {
  VALUE duration;
  VALUE exception;
  VALUE move_on_value = Qnil;
  struct Backend_timeout_ctx timeout_ctx;
  op_context_t *ctx;
  struct io_uring_sqe *sqe;
  Backend_t *backend;
  struct __kernel_timespec ts;
  VALUE result = Qnil;
  VALUE timeout;

  rb_scan_args(argc, argv, "21", &duration, &exception, &move_on_value);

  ts = duration_to_timespec(duration);
  GetBackend(self, backend);
  timeout = rb_funcall(cTimeoutException, ID_new, 0);

  sqe = io_uring_backend_get_sqe(backend);
  ctx = context_store_acquire(&backend->store, OP_TIMEOUT);
  ctx->resume_value = timeout;
  io_uring_prep_timeout(sqe, &ts, 0, 0);
  io_uring_sqe_set_data(sqe, ctx);
  io_uring_backend_defer_submit(backend);
  backend->base.op_count++;

  timeout_ctx.backend = backend;
  timeout_ctx.ctx = ctx;
  result = rb_ensure(Backend_timeout_ensure_safe, Qnil, Backend_timeout_ensure, (VALUE)&timeout_ctx);

  if (result == timeout) {
    if (exception == Qnil) return move_on_value;
    RAISE_EXCEPTION(backend_timeout_exception(exception));
  }

  RAISE_IF_EXCEPTION(result);
  RB_GC_GUARD(result);
  RB_GC_GUARD(timeout);
  return result;
}

VALUE Backend_waitpid(VALUE self, VALUE pid) {
  int pid_int = FIX2INT(pid);
  int fd = pidfd_open(pid_int, 0);
  int status;
  pid_t ret;

  if (fd >= 0) {
    VALUE resume_value;
    Backend_t *backend;
    GetBackend(self, backend);

    resume_value = io_uring_backend_wait_fd(backend, fd, 0);
    close(fd);
    RAISE_IF_EXCEPTION(resume_value);
    RB_GC_GUARD(resume_value);
  }

  ret = waitpid(pid_int, &status, WNOHANG);
  if (ret < 0) {
    int e = errno;
    if (e == ECHILD)
      ret = pid_int;
    else
      rb_syserr_fail(e, strerror(e));
  }
  return rb_ary_new_from_args(2, INT2FIX(ret), INT2FIX(WEXITSTATUS(status)));
}

/*
Blocks a fiber indefinitely. This is accomplished by using an eventfd that will
never be signalled. The eventfd is needed so we could do a blocking polling for
completions even when no other I/O operations are pending.

The eventfd is refcounted in order to allow multiple fibers to be blocked.
*/
VALUE Backend_wait_event(VALUE self, VALUE raise) {
  Backend_t *backend;
  VALUE resume_value;

  GetBackend(self, backend);

  if (backend->event_fd == -1) {
    backend->event_fd = eventfd(0, 0);
    if (backend->event_fd == -1) {
      int n = errno;
      rb_syserr_fail(n, strerror(n));
    }
  }

  if (!backend->event_fd_ctx) {
    struct io_uring_sqe *sqe;

    backend->event_fd_ctx = context_store_acquire(&backend->store, OP_POLL);
    sqe = io_uring_backend_get_sqe(backend);
    io_uring_prep_poll_add(sqe, backend->event_fd, POLLIN);
    backend->base.op_count++;
    io_uring_sqe_set_data(sqe, backend->event_fd_ctx);
    io_uring_backend_defer_submit(backend);
  }
  else
    backend->event_fd_ctx->ref_count += 1;

  resume_value = backend_await((struct Backend_base *)backend);
  context_store_release(&backend->store, backend->event_fd_ctx);

  if (backend->event_fd_ctx->ref_count == 1) {

    // last fiber to use the eventfd, so we cancel the ongoing poll
    struct io_uring_sqe *sqe;
    sqe = io_uring_backend_get_sqe(backend);
    io_uring_prep_cancel(sqe, backend->event_fd_ctx, 0);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_backend_immediate_submit(backend);
    backend->event_fd_ctx = NULL;
  }

  if (RTEST(raise)) RAISE_IF_EXCEPTION(resume_value);
  RB_GC_GUARD(resume_value);
  return resume_value;
}

VALUE Backend_kind(VALUE self) {
  return SYM_io_uring;
}

struct io_uring_sqe *Backend_chain_prepare_write(Backend_t *backend, VALUE io, VALUE buffer) {
  int fd;
  rb_io_t *fptr;
  struct io_uring_sqe *sqe;

  fd = fd_from_io(io, &fptr, 1, 0);
  sqe = io_uring_backend_get_sqe(backend);
  io_uring_prep_write(sqe, fd, StringValuePtr(buffer), RSTRING_LEN(buffer), 0);
  return sqe;
}

struct io_uring_sqe *Backend_chain_prepare_send(Backend_t *backend, VALUE io, VALUE buffer, VALUE flags) {
  int fd;
  rb_io_t *fptr;
  struct io_uring_sqe *sqe;

  fd = fd_from_io(io, &fptr, 1, 0);

  sqe = io_uring_backend_get_sqe(backend);
  io_uring_prep_send(sqe, fd, StringValuePtr(buffer), RSTRING_LEN(buffer), FIX2INT(flags));
  return sqe;
}

struct io_uring_sqe *Backend_chain_prepare_splice(Backend_t *backend, VALUE src, VALUE dest, VALUE maxlen) {
  int src_fd;
  int dest_fd;
  rb_io_t *src_fptr;
  rb_io_t *dest_fptr;
  struct io_uring_sqe *sqe;

  src_fd = fd_from_io(src, &src_fptr, 0, 0);
  dest_fd = fd_from_io(dest, &dest_fptr, 1, 0);
  sqe = io_uring_backend_get_sqe(backend);
  io_uring_prep_splice(sqe, src_fd, -1, dest_fd, -1, FIX2INT(maxlen), 0);
  return sqe;
}

void Backend_chain_ctx_attach_buffers(op_context_t *ctx, int argc, VALUE *argv) {
  int count = 0;
  if (argc > 1) ctx->buffers = malloc(sizeof(VALUE) * (argc - 1));

  for (int i = 0; i < argc; i++) {
    VALUE op = argv[i];
    VALUE op_type = RARRAY_AREF(op, 0);

    if (op_type == SYM_write || op_type == SYM_send) {
      if (!count) ctx->buffer0 = RARRAY_AREF(op, 2);
      else        ctx->buffers[count - 1] = RARRAY_AREF(op, 2);
      count++;
    }
  }
  ctx->buffer_count = count;
}


VALUE Backend_chain(int argc,VALUE *argv, VALUE self) {
  VALUE resume_value = Qnil;
  unsigned int sqe_count = 0;
  struct io_uring_sqe *last_sqe = 0;
  Backend_t *backend;
  int result;
  int completed;
  op_context_t *ctx;

  GetBackend(self, backend);
  if (argc == 0) return resume_value;

  ctx = context_store_acquire(&backend->store, OP_CHAIN);
  for (int i = 0; i < argc; i++) {
    VALUE op = argv[i];
    VALUE op_type = RARRAY_AREF(op, 0);
    VALUE op_len = RARRAY_LEN(op);
    unsigned int flags;

    if (op_type == SYM_write && op_len == 3) {
      last_sqe = Backend_chain_prepare_write(backend, RARRAY_AREF(op, 1), RARRAY_AREF(op, 2));
    }
    else if (op_type == SYM_send && op_len == 4)
      last_sqe = Backend_chain_prepare_send(backend, RARRAY_AREF(op, 1), RARRAY_AREF(op, 2), RARRAY_AREF(op, 3));
    else if (op_type == SYM_splice && op_len == 4)
      last_sqe = Backend_chain_prepare_splice(backend, RARRAY_AREF(op, 1), RARRAY_AREF(op, 2), RARRAY_AREF(op, 3));
    else {

      if (sqe_count) {
        struct io_uring_sqe *sqe;
        io_uring_sqe_set_data(last_sqe, ctx);

        ctx->ref_count = sqe_count;
        ctx->result = -ECANCELED;
        sqe = io_uring_backend_get_sqe(backend);
        io_uring_prep_cancel(sqe, ctx, 0);
        io_uring_sqe_set_data(sqe, NULL);
        io_uring_backend_immediate_submit(backend);
      }
      else {
        ctx->ref_count = 1;
        context_store_release(&backend->store, ctx);
      }
      rb_raise(rb_eRuntimeError, "Invalid op specified or bad op arity");
    }

    io_uring_sqe_set_data(last_sqe, ctx);
    flags = (i == (argc - 1)) ? 0 : IOSQE_IO_LINK;
    io_uring_sqe_set_flags(last_sqe, flags);
    sqe_count++;
  }

  backend->base.op_count += sqe_count;
  ctx->ref_count = sqe_count + 1;
  io_uring_backend_defer_submit(backend);
  resume_value = backend_await((struct Backend_base *)backend);
  result = ctx->result;
  completed = context_store_release(&backend->store, ctx);
  if (!completed) {
    struct io_uring_sqe *sqe;

    Backend_chain_ctx_attach_buffers(ctx, argc, argv);

    // op was not completed (an exception was raised), so we need to cancel it
    ctx->result = -ECANCELED;
    sqe = io_uring_backend_get_sqe(backend);
    io_uring_prep_cancel(sqe, ctx, 0);
    io_uring_sqe_set_data(sqe, NULL);
    io_uring_backend_immediate_submit(backend);
    RAISE_IF_EXCEPTION(resume_value);
    return resume_value;
  }

  RB_GC_GUARD(resume_value);
  return INT2FIX(result);
}

VALUE Backend_idle_gc_period_set(VALUE self, VALUE period) {
  Backend_t *backend;
  GetBackend(self, backend);
  backend->base.idle_gc_period = NUM2DBL(period);
  backend->base.idle_gc_last_time = current_time();
  return self;
}

VALUE Backend_idle_proc_set(VALUE self, VALUE block) {
  Backend_t *backend;
  GetBackend(self, backend);
  backend->base.idle_proc = block;
  return self;
}

inline VALUE Backend_run_idle_tasks(VALUE self) {
  Backend_t *backend;
  GetBackend(self, backend);
  backend_run_idle_tasks(&backend->base);
  return self;
}

static inline void splice_chunks_prep_write(op_context_t *ctx, struct io_uring_sqe *sqe, int fd, VALUE buffer) {
  char *buf = RSTRING_PTR(buffer);
  int len = RSTRING_LEN(buffer);
  io_uring_prep_write(sqe, fd, buf, len, 0);
  // io_uring_prep_send(sqe, fd, buf, len, 0);
  io_uring_sqe_set_data(sqe, ctx);
}

static inline void splice_chunks_prep_splice(op_context_t *ctx, struct io_uring_sqe *sqe, int src, int dest, int maxlen) {
  io_uring_prep_splice(sqe, src, -1, dest, -1, maxlen, 0);
  io_uring_sqe_set_data(sqe, ctx);
}

static inline void splice_chunks_get_sqe(
  Backend_t *backend,
  op_context_t **ctx,
  struct io_uring_sqe **sqe,
  enum op_type type
)
{
  if (*ctx) {
    if (*sqe) (*sqe)->flags |= IOSQE_IO_LINK;
    (*ctx)->ref_count++;
  }
  else
    *ctx = context_store_acquire(&backend->store, type);
  (*sqe) = io_uring_backend_get_sqe(backend);
}

static inline void splice_chunks_cancel(Backend_t *backend, op_context_t *ctx) {
  struct io_uring_sqe *sqe;

  ctx->result = -ECANCELED;
  sqe = io_uring_backend_get_sqe(backend);
  io_uring_prep_cancel(sqe, ctx, 0);
  io_uring_sqe_set_data(sqe, NULL);
  io_uring_backend_immediate_submit(backend);
}

static inline int splice_chunks_await_ops(
  Backend_t *backend,
  op_context_t **ctx,
  int *result,
  VALUE *switchpoint_result
)
{
  int completed;
  int res = io_uring_backend_defer_submit_and_await(backend, 0, *ctx, switchpoint_result);

  if (result) (*result) = res;
  completed = context_store_release(&backend->store, *ctx);
  if (!completed) {
    splice_chunks_cancel(backend, *ctx);
    if (TEST_EXCEPTION(*switchpoint_result)) return 1;
  }
  *ctx = 0;
  return 0;
}

#define SPLICE_CHUNKS_AWAIT_OPS(backend, ctx, result, switchpoint_result) \
  if (splice_chunks_await_ops(backend, ctx, result, switchpoint_result)) goto error;

VALUE Backend_splice_chunks(VALUE self, VALUE src, VALUE dest, VALUE prefix, VALUE postfix, VALUE chunk_prefix, VALUE chunk_postfix, VALUE chunk_size) {
  Backend_t *backend;
  int total = 0;
  int err = 0;
  VALUE switchpoint_result = Qnil;
  op_context_t *ctx = 0;
  struct io_uring_sqe *sqe = 0;
  int maxlen;
  VALUE chunk_len_value = Qnil;
  int src_fd;
  int dest_fd;
  rb_io_t *src_fptr;
  rb_io_t *dest_fptr;
  int pipefd[2] = { -1, -1 };

  GetBackend(self, backend);
  src_fd = fd_from_io(src, &src_fptr, 0, 0);
  dest_fd = fd_from_io(dest, &dest_fptr, 1, 0);

  maxlen = FIX2INT(chunk_size);

  if (pipe(pipefd) == -1) {
    err = errno;
    goto syscallerror;
  }

  if (prefix != Qnil) {
    splice_chunks_get_sqe(backend, &ctx, &sqe, OP_WRITE);
    splice_chunks_prep_write(ctx, sqe, dest_fd, prefix);
    backend->base.op_count++;
  }

  while (1) {
    int chunk_len;
    VALUE chunk_prefix_str = Qnil;
    VALUE chunk_postfix_str = Qnil;

    splice_chunks_get_sqe(backend, &ctx, &sqe, OP_SPLICE);
    splice_chunks_prep_splice(ctx, sqe, src_fd, pipefd[1], maxlen);
    backend->base.op_count++;

    SPLICE_CHUNKS_AWAIT_OPS(backend, &ctx, &chunk_len, &switchpoint_result);
    if (chunk_len == 0) break;

    total += chunk_len;
    chunk_len_value = INT2FIX(chunk_len);


    if (chunk_prefix != Qnil) {
      chunk_prefix_str = (TYPE(chunk_prefix) == T_STRING) ? chunk_prefix : rb_funcall(chunk_prefix, ID_call, 1, chunk_len_value);
      splice_chunks_get_sqe(backend, &ctx, &sqe, OP_WRITE);
      splice_chunks_prep_write(ctx, sqe, dest_fd, chunk_prefix_str);
      backend->base.op_count++;
    }

    splice_chunks_get_sqe(backend, &ctx, &sqe, OP_SPLICE);
    splice_chunks_prep_splice(ctx, sqe, pipefd[0], dest_fd, chunk_len);
    backend->base.op_count++;

    if (chunk_postfix != Qnil) {
      chunk_postfix_str = (TYPE(chunk_postfix) == T_STRING) ? chunk_postfix : rb_funcall(chunk_postfix, ID_call, 1, chunk_len_value);
      splice_chunks_get_sqe(backend, &ctx, &sqe, OP_WRITE);
      splice_chunks_prep_write(ctx, sqe, dest_fd, chunk_postfix_str);
      backend->base.op_count++;
    }

    RB_GC_GUARD(chunk_prefix_str);
    RB_GC_GUARD(chunk_postfix_str);
  }

  if (postfix != Qnil) {
    splice_chunks_get_sqe(backend, &ctx, &sqe, OP_WRITE);
    splice_chunks_prep_write(ctx, sqe, dest_fd, postfix);
    backend->base.op_count++;
  }
  if (ctx) {
    SPLICE_CHUNKS_AWAIT_OPS(backend, &ctx, 0, &switchpoint_result);
  }

  RB_GC_GUARD(chunk_len_value);
  RB_GC_GUARD(switchpoint_result);
  if (pipefd[0] != -1) close(pipefd[0]);
  if (pipefd[1] != -1) close(pipefd[1]);
  return INT2FIX(total);
syscallerror:
  if (pipefd[0] != -1) close(pipefd[0]);
  if (pipefd[1] != -1) close(pipefd[1]);
  rb_syserr_fail(err, strerror(err));
error:
  context_attach_buffers_v(ctx, 4, prefix, postfix, chunk_prefix, chunk_postfix);
  if (pipefd[0] != -1) close(pipefd[0]);
  if (pipefd[1] != -1) close(pipefd[1]);
  return RAISE_EXCEPTION(switchpoint_result);
}

VALUE Backend_trace(int argc, VALUE *argv, VALUE self) {
  Backend_t *backend;
  GetBackend(self, backend);
  backend_trace(&backend->base, argc, argv);
  return self;
}

VALUE Backend_trace_proc_set(VALUE self, VALUE block) {
  Backend_t *backend;
  GetBackend(self, backend);

  backend->base.trace_proc = block;
  return self;
}

VALUE Backend_snooze(VALUE self) {
  VALUE ret;
  VALUE fiber = rb_fiber_current();
  Backend_t *backend;
  GetBackend(self, backend);

  Fiber_make_runnable(fiber, Qnil);
  ret = backend_base_switch_fiber(self, &backend->base);

  COND_TRACE(&backend->base, 4, SYM_unblock, rb_fiber_current(), ret, CALLER());

  RAISE_IF_EXCEPTION(ret);
  RB_GC_GUARD(ret);
  return ret;
}

void Backend_park_fiber(VALUE self, VALUE fiber) {
  Backend_t *backend;
  GetBackend(self, backend);

  backend_base_park_fiber(&backend->base, fiber);
}

void Backend_unpark_fiber(VALUE self, VALUE fiber) {
  Backend_t *backend;
  GetBackend(self, backend);

  backend_base_unpark_fiber(&backend->base, fiber);
}

void Init_Backend(void) {
  VALUE cBackend = rb_define_class_under(mPolyphony, "Backend", rb_cObject);
  rb_define_alloc_func(cBackend, Backend_allocate);

  rb_define_method(cBackend, "initialize", Backend_initialize, 0);
  rb_define_method(cBackend, "finalize", Backend_finalize, 0);
  rb_define_method(cBackend, "post_fork", Backend_post_fork, 0);
  rb_define_method(cBackend, "trace", Backend_trace, -1);
  rb_define_method(cBackend, "trace_proc=", Backend_trace_proc_set, 1);
  rb_define_method(cBackend, "stats", Backend_stats, 0);

  rb_define_method(cBackend, "poll", Backend_poll, 1);
  rb_define_method(cBackend, "break", Backend_wakeup, 0);
  rb_define_method(cBackend, "kind", Backend_kind, 0);
  rb_define_method(cBackend, "chain", Backend_chain, -1);
  rb_define_method(cBackend, "idle_gc_period=", Backend_idle_gc_period_set, 1);
  rb_define_method(cBackend, "idle_proc=", Backend_idle_proc_set, 1);
  rb_define_method(cBackend, "splice_chunks", Backend_splice_chunks, 7);

  rb_define_method(cBackend, "accept", Backend_accept, 2);
  rb_define_method(cBackend, "accept_loop", Backend_accept_loop, 2);
  rb_define_method(cBackend, "connect", Backend_connect, 3);
  rb_define_method(cBackend, "feed_loop", Backend_feed_loop, 3);

  #ifdef HAVE_IO_URING_PREP_MULTISHOT_ACCEPT
  rb_define_method(cBackend, "multishot_accept", Backend_multishot_accept, 1);
  #endif

  rb_define_method(cBackend, "read", Backend_read, 5);
  rb_define_method(cBackend, "read_loop", Backend_read_loop, 2);
  rb_define_method(cBackend, "recv", Backend_recv, 4);
  rb_define_method(cBackend, "recvmsg", Backend_recvmsg, 7);
  rb_define_method(cBackend, "recv_feed_loop", Backend_recv_feed_loop, 3);
  rb_define_method(cBackend, "recv_loop", Backend_recv_loop, 2);
  rb_define_method(cBackend, "send", Backend_send, 3);
  rb_define_method(cBackend, "sendmsg", Backend_sendmsg, 5);
  rb_define_method(cBackend, "sendv", Backend_sendv, 3);
  rb_define_method(cBackend, "sleep", Backend_sleep, 1);

  rb_define_method(cBackend, "splice", Backend_splice, 3);
  rb_define_method(cBackend, "double_splice", Backend_double_splice, 2);
  rb_define_method(cBackend, "tee", Backend_tee, 3);

  rb_define_method(cBackend, "timeout", Backend_timeout, -1);
  rb_define_method(cBackend, "timer_loop", Backend_timer_loop, 1);
  rb_define_method(cBackend, "wait_event", Backend_wait_event, 1);
  rb_define_method(cBackend, "wait_io", Backend_wait_io, 2);
  rb_define_method(cBackend, "waitpid", Backend_waitpid, 1);
  rb_define_method(cBackend, "write", Backend_write_m, -1);
  // rb_define_method(cBackend, "close", Backend_close, 1);

  SYM_io_uring = ID2SYM(rb_intern("io_uring"));
  SYM_send = ID2SYM(rb_intern("send"));
  SYM_splice = ID2SYM(rb_intern("splice"));
  SYM_write = ID2SYM(rb_intern("write"));

  backend_setup_stats_symbols();

  eArgumentError = rb_const_get(rb_cObject, rb_intern("ArgumentError"));
}

#endif // POLYPHONY_BACKEND_LIBURING
