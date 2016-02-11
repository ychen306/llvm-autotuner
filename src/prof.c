#include <time.h>
#include <sys/time.h>
#include <stdint.h> 
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <math.h>

#include "common.h"

#define PROF_FLAT_OUT "loop-prof.flat.csv" 
#define PROF_GRAPH_OUT "loop-prof.graph.csv"

// sample every 1ms
#define SAMPLING_INTERVAL 1000

void _prof_init() __attribute__ ((constructor));
void _prof_dump() __attribute__ ((destructor));

struct loop_data { 
	char *func;
	int32_t header_id;
	int64_t runs;
};

extern uint32_t _prof_num_loops;

extern struct loop_data _prof_loops[];

extern uint32_t _prof_loops_running[];

static struct timespec begin;

struct fraction { 
	size_t a, b; 
}; 

// a profile is an array of fraction (of time) spent in a loop
//
// e.g. if A spends 50% of the time calling B, A is run
// 25% of the sample, and C is called by B 25% of the time
// then A's profile will be [1/4(A), 1/2(B)]
// (note that the entry of a loop itself is absolute and the 
// rest is relative)
typedef struct fraction *profile_t;

static profile_t *profiles;

static inline float frac2num(struct fraction *frac)
{ 
	return ((float) frac->a) / frac->b;
}

// record that `caller` called `called_idx`
static inline void record_hit(profile_t caller, size_t callee_idx)
{ 
	struct fraction *frac = &caller[callee_idx];
	frac->a++;
	frac->b++;
}

// record that `caller` didn't call `called_idx`
static inline void record_miss(profile_t caller, size_t callee_idx)
{
	caller[callee_idx].b++;
}

static void create_profiles()
{
	profiles = malloc(sizeof (profile_t) * _prof_num_loops);
	size_t i;
	for (i = 0; i < _prof_num_loops; i++) 
		profiles[i] = calloc(sizeof (struct fraction), _prof_num_loops);
}

// exponential distribution with lambda = 1
// note that this means the expected value is also one (E[X] = 1/lambda)
// see
// https://en.wikipedia.org/wiki/Exponential_distribution#Generating_exponential_variates
static inline float rand_exp()
{ 
	float r = (float)rand() / (float)RAND_MAX,
		  x = -logf(r);
	return x;
}

static void collect_sample(int signo);

// setup a timer that fires in T microseconds
// T is a exponential random variable with expectation of `SAMPLING_INTERVAL` microseconds
static void setup_timer()
{
	struct itimerval timerspec;

	timerspec.it_interval.tv_sec = 0;
	timerspec.it_interval.tv_usec = 0;
	timerspec.it_value.tv_sec = 0;
	timerspec.it_value.tv_usec = SAMPLING_INTERVAL * rand_exp(); 

	if (signal(SIGPROF, collect_sample) == SIG_ERR) {
		perror("Unable to catch SIGPROF");
		exit(1);
	}

	setitimer(ITIMER_PROF, &timerspec, NULL);
}

static void collect_sample(int signo)
{ 
	// update profiles of all the loops given a new sample
	size_t i, j;
	for (i = 0; i < _prof_num_loops; i++) {
		if (_prof_loops_running[i]) record_hit(profiles[i], i);
		else record_miss(profiles[i], i);

		for (j = i+1; j < _prof_num_loops; j++) { 
			if (_prof_loops_running[i] && _prof_loops_running[j]) { 
				if (_prof_loops_running[i] < _prof_loops_running[j]) {
					// i called j 
					record_hit(profiles[i], j);
					record_miss(profiles[j], i);
				} else {
					// j called i
					record_hit(profiles[j], i);
					record_miss(profiles[i], j);
				}
			} else if (_prof_loops_running[i]) {
				record_miss(profiles[i], j);
			} else if (_prof_loops_running[j]) {
				record_miss(profiles[j], i);
			}
		}
	}
	setup_timer();
}

void _prof_init() 
{ 
	srand(time(NULL));
	setup_timer();
	create_profiles();
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &begin);
}

void _prof_dump()
{ 
	// disarm timer
	struct itimerval timerspec;
	memset(&timerspec, 0, sizeof timerspec);
	setitimer(SIGPROF, &timerspec, NULL);

	// time the process in ms
	struct timespec end;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
	long long int elapsed = (end.tv_sec - begin.tv_sec)*1e3 +
		(end.tv_nsec - begin.tv_nsec) / 1e6;

	FILE *flat_out = fopen(PROF_FLAT_OUT, "wb"),
		 *graph_out = fopen(PROF_GRAPH_OUT, "wb");

	fprintf(flat_out, "function,header-id,runs,time(pct),time(ms)\n");
	size_t i, j;
	for (i = 0; i < _prof_num_loops; i++) { 
		struct loop_data *loop = &_prof_loops[i];
		float pct = frac2num(&profiles[i][i]); 
		fprintf(flat_out, "%s,%d,%lld,%.4f,%.4f\n",
				loop->func,
				loop->header_id,
				loop->runs,
				100*pct,
				elapsed*pct);
	}

	for (i = 0; i < _prof_num_loops; i++) {
		for (j = 0; j < _prof_num_loops; j++) {
			if (i == j) fprintf(graph_out, "_");
			else fprintf(graph_out, "%.4f", frac2num(&profiles[i][j]) * 100);

			if (j != _prof_num_loops-1) fprintf(graph_out, "\t");
		}
		fprintf(graph_out, "\n");
	}

	fclose(flat_out);
	fclose(graph_out);
}
