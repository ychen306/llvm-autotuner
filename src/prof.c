#include <time.h>
#include <stdint.h> 
#include <stdlib.h>
#include <stdio.h>

#define PROF_OUT "prof.out.csv" 

void _prof_dump() __attribute__ ((destructor));

//
// TODO
// make this work with threads
//
// time unit is nano seconds
struct loop_data {
	int64_t total_elapsed;
	int64_t cur_begin_sec;
	int64_t cur_begin_nsec;
    uint32_t running;
	uint32_t id;
    int32_t iterations;
	char *fn_name;
};

extern struct loop_data *_prof_loops[];
extern uint32_t _prof_num_loops;

void _prof_begin(struct loop_data *loop)
{
	if (loop->running) return;

	loop->running = 1; 
    struct timespec begin;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &begin);
    loop->cur_begin_sec = begin.tv_sec;
    loop->cur_begin_nsec = begin.tv_nsec;
}

void _prof_end(struct loop_data *loop)
{
	if (!loop->running) return;

    struct timespec end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
    uint64_t elapsed = 1e9 * (end.tv_sec - loop->cur_begin_sec) +
        (end.tv_nsec - loop->cur_begin_nsec);

	loop->running = 0;
	loop->total_elapsed += elapsed;
    loop->iterations += 1;
}

void _prof_dump()
{ 
	unsigned i;
	FILE *out = fopen(PROF_OUT, "wb");

	fprintf(out, "function,loop id,avg time spent,times run\n");
	for (i = 0; i < _prof_num_loops; i++) {
		struct loop_data *loop = _prof_loops[i]; 
		fprintf(out, "%s,%d,%ld,%d\n",
				loop->fn_name,
				(int) loop->id,
				loop->total_elapsed/loop->iterations,
                loop->iterations);
	}

	fclose(out);
}
