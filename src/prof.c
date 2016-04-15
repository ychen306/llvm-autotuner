#include <time.h>
#include <sys/time.h>
#include <stdint.h> 
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <errno.h>

#include "common.h"

#define PROF_FLAT_OUT "loop-prof.flat.csv" 
#define PROF_GRAPH_OUT "loop-prof.graph.csv"
#define PROF_DUMP "loop_prof.out"

// sample every 100 us
#define SAMPLING_INTERVAL 100

static uint32_t _prof_num_loops_tot = 0;

void _prof_init() __attribute__ ((constructor));
void _prof_dump() __attribute__ ((destructor));

// Profile data about a single loop
struct loop_data { 
	char *func;
	int32_t header_id; // > 0 if it's loop, = 0 if it's a function
	int64_t runs;
};

// Create a linked list of descriptors, one per linked module.
// 
typedef struct module_desc_t {
  uint32_t _prof_num_loops;
  struct loop_data* _prof_loops_p;
  uint32_t* _prof_loops_running_p;
  struct module_desc_t* next;
} module_desc;

module_desc* module_desc_list_head = NULL;
module_desc* module_desc_list_tail = NULL;

static module_desc* get_new_desc_list_node()
{
  module_desc* new_entry = calloc(1, sizeof(module_desc));
  if (new_entry == NULL) {
    perror("append_new_desc_list_node(): ");
    exit(1);
  }
  if (module_desc_list_tail == NULL)
    module_desc_list_head = new_entry;
  else {
    assert(module_desc_list_tail->next == NULL && "Incorrect list tail");
    module_desc_list_tail->next = new_entry;
  }
  module_desc_list_tail = new_entry;
  return new_entry;
}

void add_module_desc(int32_t* _numloops,
		     struct loop_data* _p_l,
		     int32_t* _p_l_r)
{
  int32_t numloops = *_numloops;
  assert(numloops >= 0 && "Unexpected negative value for _numloops");
  _prof_num_loops_tot += (uint32_t) numloops;
  
  module_desc* new_entry = get_new_desc_list_node();
  new_entry->_prof_num_loops = (uint32_t) numloops;
  new_entry->_prof_loops_p = _p_l;
  new_entry->_prof_loops_running_p = (uint32_t*) _p_l_r;
  new_entry->next = NULL;
#ifndef NDEBUG
  printf("Registering one module desc!\n");
#endif
}

static struct timespec begin;

FILE *dumpfile;
size_t num_sampled;

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

// Allocate an NxN array of fraction objects, where N = _prof_num_loops_tot
// Initialize each one to zero by using calloc.
//
static void create_profiles()
{
        if (_prof_num_loops_tot == 0)
                return;
	profiles = malloc(sizeof (profile_t) * _prof_num_loops_tot);
	size_t i;
	for (i = 0; i < _prof_num_loops_tot; i++) {
		profiles[i] = calloc(_prof_num_loops_tot,
				     sizeof(struct fraction));
	}
}

// exponential distribution with lambda = 1
// note that this means the expected value is also one (E[X] = 1/lambda)
//
// stolen from:
// https://en.wikipedia.org/wiki/Exponential_distribution#Generating_exponential_variates
static inline float rand_exp()
{ 
	float r = (float)rand() / (float)RAND_MAX,
		  x = -logf(r);
	return x;
}

static void dump_sample(int signo);

// setup a timer that fires in T microseconds
// T is a exponential random variable with expectation of `SAMPLING_INTERVAL` microseconds
static void setup_timer()
{
	struct itimerval timerspec;

	timerspec.it_interval.tv_sec = 0;
	timerspec.it_interval.tv_usec = 0;
	timerspec.it_value.tv_sec = 0;
	timerspec.it_value.tv_usec = SAMPLING_INTERVAL * (rand_exp() + 0.5); 

	if (signal(SIGPROF, dump_sample) == SIG_ERR) {
		perror("Unable to catch SIGPROF");
		exit(1);
	}

	setitimer(ITIMER_PROF, &timerspec, NULL);
}

// running_instance[] is an array of integers recording which loops were
// running at the time of the sample.  Use those to accumulate an NxN
// matrix of profile entries, where profiles[i][j] is for loops i and j,
// and each profile entry is as defined above.
// 
static void collect_sample_impl(uint32_t *running_instance)
{
	size_t i, j;
	for (i = 0; i < _prof_num_loops_tot; i++) {
		if (running_instance[i]) record_hit(profiles[i], i);
		else record_miss(profiles[i], i);

		for (j = i+1; j < _prof_num_loops_tot; j++) { 
			if (running_instance[i] && running_instance[j]) { 
				if (running_instance[i] < running_instance[j]) {
					// i called j 
					record_hit(profiles[i], j);
					record_miss(profiles[j], i);
				} else {
					// j called i
					record_hit(profiles[j], i);
					record_miss(profiles[i], j);
				}
			} else if (running_instance[i]) {
				record_miss(profiles[i], j);
			} else if (running_instance[j]) {
				record_miss(profiles[j], i);
			}
		}
	}
}

static void collect_sample(uint32_t *running_instance)
{ 
        // Nothing to do if there are zero loops
        if (_prof_num_loops_tot == 0) return;

        // Otherwise, if this is the first sample, allocate the profiles array
        if (profiles == NULL) create_profiles();

// Debug infinite loop
static int inCollect = 0;
printf("In collect_sample %d\n", ++inCollect);
	
	// update profiles of all the loops given a new sample
	collect_sample_impl(running_instance);
}

// Sample what loops and functions are running and write sample data to a file
static int dump_one_sample(module_desc* desc)
{
	// Write the sample data for each module
	size_t num_written = 0;
	do {
		ssize_t bytes = fwrite(desc->_prof_loops_running_p+num_written, sizeof (uint32_t), desc->_prof_num_loops-num_written, dumpfile);
		num_written += bytes;
	} while (num_written < desc->_prof_num_loops);
	return num_written;
}

// Sample what loops and functions are running and write sample data to a file
static void dump_sample(int signo)
{
  for (module_desc *desc= module_desc_list_head; desc !=NULL; desc=desc->next) {
    size_t num_written = dump_one_sample(desc);
    assert(num_written == desc->_prof_num_loops && "Invalid dump");
  }
  num_sampled++;
  setup_timer();
}

void _prof_init() 
{ 
	dumpfile = fopen(PROF_DUMP, "w+");
	
	srand(time(NULL));
	//create_profiles();
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &begin);
	setup_timer();
}

void _prof_dump()
{ 
	// disarm timer
	struct itimerval timerspec;
	memset(&timerspec, 0, sizeof timerspec);
	setitimer(SIGPROF, &timerspec, NULL);
	signal(SIGPROF, SIG_IGN);

	// read sample from the dump
	uint32_t *buf = malloc(sizeof (uint32_t) * _prof_num_loops_tot);
	fflush(dumpfile);
	rewind(dumpfile);
	for (size_t i = 0, N = num_sampled; i < N; i++) {
		size_t num_read = 0;
		do {
			size_t n = fread(buf+num_read, sizeof (uint32_t), _prof_num_loops_tot-num_read, dumpfile); 
			num_read += n;
			if (feof(dumpfile) && num_read < _prof_num_loops_tot) {
			  perror("Unexpected EOF in dump file");
			  assert(0);
			}
		} while (num_read < _prof_num_loops_tot);
		collect_sample(buf);
	}
	free(buf);
	fclose(dumpfile);

	// time the process in ms
	struct timespec end;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
	long long int elapsed = (end.tv_sec - begin.tv_sec)*1e3 +
		(end.tv_nsec - begin.tv_nsec) / 1e6;

	FILE *flat_out = fopen(PROF_FLAT_OUT, "wb"),
		 *graph_out = fopen(PROF_GRAPH_OUT, "wb");

	fprintf(flat_out, "function,header-id,runs,time(pct),time(ms)\n");
	uint32_t loop_idx = 0;
	for (module_desc *desc = module_desc_list_head; desc != NULL;
	     desc= desc->next) {
	   struct loop_data* prof_loops = desc->_prof_loops_p;
	   for (uint32_t i = 0; i < desc->_prof_num_loops; i++) { 
		struct loop_data *loop = &prof_loops[i];
		float pct = frac2num(&profiles[loop_idx][loop_idx]);
		fprintf(flat_out, "%s,%d,%ld,%.4f,%.4f\n",
				loop->func,
				loop->header_id,
				loop->runs,
				100*pct,
				elapsed*pct);
	        ++loop_idx;
	  }
	}

	for (size_t i = 0; i < _prof_num_loops_tot; i++) {
		for (size_t j = 0; j < _prof_num_loops_tot; j++) {
			if (i == j) fprintf(graph_out, "-1");
			else fprintf(graph_out, "%.4f", frac2num(&profiles[i][j]) * 100);

			if (j != _prof_num_loops_tot-1) fprintf(graph_out,"\t");
		}
		fprintf(graph_out, "\n");
	}

	fclose(flat_out);
	fclose(graph_out);
}
