#include <time.h>
#include <stdint.h> 
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#define PROF_OUT "loop-prof.out.csv" 

void _prof_init() __attribute__ ((constructor));
void _prof_dump() __attribute__ ((destructor));

struct loop_profile { 
    char *func;
    int32_t header_id;
    int32_t running;
    int64_t runs;
    int64_t sampled;
};

// sample every 1ms
#define SAMPLING_INTERVAL 1000

extern uint32_t _prof_num_loops;

extern struct loop_profile *_prof_loops[];

static timer_t timer_id;

int64_t total_sampled;

static void sample(int sig, siginfo_t *si, void *uc)
{ 
    if (si->si_value.sival_ptr != &timer_id) return;

    uint32_t i;
    for (i = 0; i < _prof_num_loops; i++) {
        struct loop_profile *loop = _prof_loops[i]; 
        if (loop->running) loop->sampled++;
    } 

    total_sampled += 1;
}
 
void _prof_init() 
{ 
    struct itimerspec timerspec;
    struct sigaction sa;
    struct sigevent sev;

    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = sample;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    sev.sigev_value.sival_ptr = &timer_id;
    timer_create(CLOCK_PROCESS_CPUTIME_ID, &sev, &timer_id); 

    timerspec.it_interval.tv_sec = 0;
    timerspec.it_interval.tv_nsec = SAMPLING_INTERVAL;
    timerspec.it_value.tv_sec = 0;
    timerspec.it_value.tv_nsec = SAMPLING_INTERVAL;

    timer_settime(&timer_id, 0, &timerspec, NULL);
}

void _prof_dump()
{ 
	uint32_t i;
	FILE *out = fopen(PROF_OUT, "wb"); 

    fprintf(out, "function,header-id,runs,%% time\n");
    for (i = 0; i < _prof_num_loops; i++) {
        struct loop_profile *loop = _prof_loops[i];
        fprintf(out, "%s,%d,%ld,%.4f\n",
                loop->func,
                loop->header_id,
                loop->runs,
                ((double)loop->sampled)/total_sampled);
    }

	fclose(out);
}
