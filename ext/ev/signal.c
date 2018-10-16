#include "ev.h"

struct EV_Signal {
  VALUE   self;
  int     signum;
  int     active;
  int     free_in_callback;
  struct  ev_signal ev_signal;
  VALUE   callback;
};

static VALUE mEV = Qnil;
static VALUE cEV_Signal = Qnil;

/* Allocator/deallocator */
static VALUE EV_Signal_allocate(VALUE klass);
static void EV_Signal_mark(struct EV_Signal *signal);
static void EV_Signal_free(struct EV_Signal *signal);

/* Methods */
static VALUE EV_Signal_initialize(VALUE self, VALUE sig);

static VALUE EV_Signal_start(VALUE self);
static VALUE EV_Signal_stop(VALUE self);

void EV_Signal_callback(ev_loop *ev_loop, struct ev_signal *signal, int revents);

static ID ID_call = Qnil;

/* Signal encapsulates a signal watcher */
void Init_EV_Signal() {
  mEV = rb_define_module("EV");
  cEV_Signal = rb_define_class_under(mEV, "Signal", rb_cObject);
  rb_define_alloc_func(cEV_Signal, EV_Signal_allocate);

  rb_define_method(cEV_Signal, "initialize", EV_Signal_initialize, 1);
  rb_define_method(cEV_Signal, "start", EV_Signal_start, 0);
  rb_define_method(cEV_Signal, "stop", EV_Signal_stop, 0);

  ID_call = rb_intern("call");
}

static VALUE EV_Signal_allocate(VALUE klass) {
  struct EV_Signal *signal = (struct EV_Signal *)xmalloc(sizeof(struct EV_Signal));

  return Data_Wrap_Struct(klass, EV_Signal_mark, EV_Signal_free, signal);
}

static void EV_Signal_mark(struct EV_Signal *signal) {
  rb_gc_mark(signal->callback);
}

static void EV_Signal_free(struct EV_Signal *signal) {
  ev_signal_stop(EV_DEFAULT, &signal->ev_signal);

  if ev_is_pending(&signal->ev_signal) {
    signal->free_in_callback = 1;
  }
  else {
    xfree(signal);
  }
}

static VALUE EV_Signal_initialize(VALUE self, VALUE sig) {
  struct EV_Signal *signal;

  Data_Get_Struct(self, struct EV_Signal, signal);

  VALUE signum = sig;

  signal->signum = NUM2INT(signum);

  if (rb_block_given_p()) {
    signal->callback = rb_block_proc();
  }

  ev_signal_init(&signal->ev_signal, EV_Signal_callback, signal->signum);

  signal->ev_signal.data = (void *)signal;
  signal->active = 1;
  signal->free_in_callback = 0;

  ev_signal_start(EV_DEFAULT, &signal->ev_signal);
  EV_add_watcher_ref(self);

  return Qnil;
}

void EV_Signal_callback(ev_loop *ev_loop, struct ev_signal *ev_signal, int revents) {
  struct EV_Signal *signal = (struct EV_Signal *)ev_signal->data;

  if (signal->free_in_callback) {
    xfree(signal);
    return;
  }

  rb_funcall(signal->callback, ID_call, 1, INT2NUM(signal->signum));
}

static VALUE EV_Signal_start(VALUE self) {
  struct EV_Signal *signal;
  Data_Get_Struct(self, struct EV_Signal, signal);

  if (!signal->active) {
    ev_signal_start(EV_DEFAULT, &signal->ev_signal);
    EV_add_watcher_ref(self);
    signal->active = 1;
  }

  return Qnil;
}

static VALUE EV_Signal_stop(VALUE self) {
  struct EV_Signal *signal;
  Data_Get_Struct(self, struct EV_Signal, signal);

  if (signal->active) {
    ev_signal_stop(EV_DEFAULT, &signal->ev_signal);
    EV_del_watcher_ref(self);
    signal->active = 0;
  }

  return Qnil;
}