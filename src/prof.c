#include <time.h>
#include <stdint.h> 
#include <stdlib.h>
#include <stdio.h>

#define PROF_OUT "prof.out.csv" 

/*
 * TODO
 * make this work with threads
 *
 * note that the time unit is clock ticks, not seconds
 */
struct loop_data {
	double total_elapsed;
	double cur_begin;
	uint32_t running;
	uint32_t id;
	char *fn_name;
};

extern struct loop_data *_prof_loops[];
extern uint32_t _prof_num_loops;

void _prof_begin(struct loop_data *loop)
{
	if (loop->running) return;

	loop->running = 1; 
	loop->cur_begin = (double)clock();
}

void _prof_end(struct loop_data *loop)
{
	if (!loop->running) return;

	loop->running = 0;
	loop->total_elapsed += ((double)clock()) - loop->cur_begin;
}

void _prof_dump()
{ 
	unsigned i;
	FILE *out = fopen(PROF_OUT, "wb");

	fprintf(out, "function,loop id,time spent\n");
	for (i = 0; i < _prof_num_loops; i++) {
		struct loop_data *loop = _prof_loops[i]; 
		fprintf(out, "%s,%d,%f\n",
				loop->fn_name,
				(int) loop->id,
				loop->total_elapsed);
	}

	fclose(out);
}
