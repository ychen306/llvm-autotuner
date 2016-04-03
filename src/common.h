#ifndef _COMMON_H_ 
#define _COMMON_H_

// mach dosen't have `clock_gettime`
#ifdef __MACH__ 
#include <mach/clock.h>
#include <mach/mach.h>
#define CLOCK_MONOTONIC SYSTEM_CLOCK

int clock_gettime(clock_id_t clk_id, struct timespec *ts)
{ 
	clock_serv_t cclock;
	mach_timespec_t mts;
	host_get_clock_service(mach_host_self(), clk_id, &cclock);
	int ret = clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;
	return ret;
}
#endif

float diff_time(struct timespec *end, struct timespec *begin)
{
  return (float) ((end->tv_sec-begin->tv_sec) * 1e9 +
                  (end->tv_nsec - begin->tv_nsec));
}

#endif
