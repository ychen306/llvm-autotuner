#include <stdio.h>
#include <time.h>

#include "common.h"

void _invos_init() __attribute__ ((constructor));
void _invos_dump() __attribute__ ((destructor));

#define OUT_FILENAME "invocations.txt"

FILE *out;

static __thread struct timespec begin;

void _invos_begin()
{
	clock_gettime(CLOCK_MONOTONIC, &begin);
}

void _invos_end()
{
	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	fprintf(out, "%f ", diff_time(&end, &begin));
}

void _invos_init()
{
	out = fopen(OUT_FILENAME, "w");
}

void _invos_dump()
{
	fclose(out);
}
