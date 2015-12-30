#include <time.h>
#include <stdint.h> 
#include <stdlib.h>
#include <stdio.h>

#define PROF_OUT "prof.out.tsv"

/*
 * note that the time unit is clock ticks, not seconds
 */
struct loop_data {
	double total_elapsed;
	double cur_begin;
	uint32_t running;
	uint32_t id;
	char *fn_name;
};

extern struct loop_data **loops;
extern unsigned num_loops;

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
	loop->total_elapsed += (double)clock() - loop->cur_begin;
}

void _prof_dump()
{ 
	unsigned i;
	FILE *out = fopen(PROF_OUT, "wb");

	fprintf(out, "function\tloop id\ttime spend");
	for (i = 0; i < num_loops; i++) {
		struct loop_data *loop = loops[i]; 
		fprintf(out, "%s\t%du\t%f",
				loop->fn_name,
				loop->id,
				loop->total_elapsed);
	}

	fclose(out);
}
