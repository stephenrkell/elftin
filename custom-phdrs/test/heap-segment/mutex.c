#include <stddef.h>
#include <stdatomic.h>
#ifdef __THREAD_START_H
#include __THREAD_START_H
#endif

#ifndef CAS
#define CAS(location, oldval, newval) \
        atomic_compare_exchange_strong(location, &oldval, newval)
#endif

static _Atomic(int) locked;

static void enter_cr(void)
{
	int oldval = 0;
	while (CAS(&locked, oldval, 1));
}

static void exit_cr(void)
{
	locked = 0;
}

void f(int tid) {
  enter_cr();
  exit_cr();
  enter_cr();
  exit_cr();
}

int main(void)
{
  thread_start(f);
  enter_cr();
  exit_cr();
  enter_cr();
  exit_cr();
  return 0;
}
