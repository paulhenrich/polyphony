#include "gyro.h"

VALUE mGyro;

ID ID_call;
ID ID_caller;
ID ID_clear;
ID ID_each;
ID ID_inspect;
ID ID_new;
ID ID_raise;
ID ID_ivar_running;
ID ID_runnable;
ID ID_runnable_value;
ID ID_size;
ID ID_signal_bang;
ID ID_switch_fiber;
ID ID_transfer;
ID ID_R;
ID ID_W;
ID ID_RW;

ID ID_empty;
ID ID_pop;
ID ID_push;

VALUE SYM_dead;
VALUE SYM_running;
VALUE SYM_runnable;
VALUE SYM_waiting;

// static VALUE Gyro_break_set(VALUE self) {
//   break_flag = 1;
//   ev_break(EV_DEFAULT, EVBREAK_ALL);
//   return Qnil;
// }

// static VALUE Gyro_break_get(VALUE self) {
//   return (break_flag == 0) ? Qfalse : Qtrue;
// }

VALUE Gyro_snooze(VALUE self) {
  VALUE fiber = rb_fiber_current();
  Gyro_schedule_fiber(fiber, Qnil);

  VALUE ret = Thread_switch_fiber(rb_thread_current());
  if (RTEST(rb_obj_is_kind_of(ret, rb_eException)))
    return rb_funcall(rb_mKernel, ID_raise, 1, ret);
  else
    return ret;
}

static VALUE Gyro_post_fork(VALUE self) {
  Thread_post_fork(rb_thread_current());
  return Qnil;
}

static VALUE Gyro_ref(VALUE self) {
  return Thread_ref(rb_thread_current());
}

static VALUE Gyro_unref(VALUE self) {
  return Thread_unref(rb_thread_current());
}

static VALUE Gyro_suspend(VALUE self) {
  rb_ivar_set(self, ID_runnable_value, Qnil);
  VALUE ret = Thread_switch_fiber(rb_thread_current());
  
  if (RTEST(rb_obj_is_kind_of(ret, rb_eException))) {
    return rb_funcall(rb_mKernel, ID_raise, 1, ret);
  }
  else {
    return ret;
  }
}

static VALUE Fiber_safe_transfer(int argc, VALUE *argv, VALUE self) {
  VALUE arg = (argc == 0) ? Qnil : argv[0];
  VALUE ret = rb_funcall(self, ID_transfer, 1, arg);

  // fiber is resumed, check if resumed value is an exception
  return RTEST(rb_obj_is_kind_of(ret, rb_eException)) ? 
    rb_funcall(rb_mKernel, ID_raise, 1, ret) : ret;
}

static VALUE Fiber_schedule(int argc, VALUE *argv, VALUE self) {
  VALUE value = (argc == 0) ? Qnil : argv[0];
  Gyro_schedule_fiber(self, value);
  return self;
}

static VALUE Fiber_state(VALUE self) {
  if (!rb_fiber_alive_p(self) || (rb_ivar_get(self, ID_ivar_running) == Qfalse))
    return SYM_dead;
  if (rb_fiber_current() == self) return SYM_running;
  if (rb_ivar_get(self, ID_runnable) != Qnil) return SYM_runnable;
  
  return SYM_waiting;
}

inline void Gyro_schedule_fiber(VALUE fiber, VALUE value) {
  Thread_schedule_fiber(rb_thread_current(), fiber, value);
}

void Init_Gyro() {
  mGyro = rb_define_module("Gyro");

  rb_define_singleton_method(mGyro, "post_fork", Gyro_post_fork, 0);
  rb_define_singleton_method(mGyro, "ref", Gyro_ref, 0);
  rb_define_singleton_method(mGyro, "unref", Gyro_unref, 0);

  // rb_define_singleton_method(mGyro, "break!", Gyro_break_set, 0);
  // rb_define_singleton_method(mGyro, "break?", Gyro_break_get, 0);

  rb_define_global_function("snooze", Gyro_snooze, 0);
  rb_define_global_function("suspend", Gyro_suspend, 0);

  VALUE cFiber = rb_const_get(rb_cObject, rb_intern("Fiber"));
  rb_define_method(cFiber, "safe_transfer", Fiber_safe_transfer, -1);
  rb_define_method(cFiber, "schedule", Fiber_schedule, -1);
  rb_define_method(cFiber, "state", Fiber_state, 0);

  ID_call             = rb_intern("call");
  ID_caller           = rb_intern("caller");
  ID_clear            = rb_intern("clear");
  ID_each             = rb_intern("each");
  ID_inspect          = rb_intern("inspect");
  ID_new              = rb_intern("new");
  ID_raise            = rb_intern("raise");
  ID_ivar_running     = rb_intern("@running");
  ID_runnable         = rb_intern("runnable");
  ID_runnable_value   = rb_intern("runnable_value");
  ID_size             = rb_intern("size");
  ID_signal_bang      = rb_intern("signal!");
  ID_switch_fiber     = rb_intern("switch_fiber");
  ID_transfer         = rb_intern("transfer");
  ID_R                = rb_intern("r");
  ID_W                = rb_intern("w");
  ID_RW               = rb_intern("rw");

  ID_empty            = rb_intern("empty?");
  ID_pop              = rb_intern("pop");
  ID_push             = rb_intern("push");

  SYM_dead = ID2SYM(rb_intern("dead"));
  SYM_running = ID2SYM(rb_intern("running"));
  SYM_runnable = ID2SYM(rb_intern("runnable"));
  SYM_waiting = ID2SYM(rb_intern("waiting"));
  rb_global_variable(&SYM_dead);
  rb_global_variable(&SYM_running);
  rb_global_variable(&SYM_runnable);
  rb_global_variable(&SYM_waiting);
}