/*--- myadd.c
 *   Simple adding subroutine thrown in to allow subroutine
 *   calls/returns to be factored in as part of the benchmark.
*/

const int N = 3;

void myadd(float *sum, float *addend) {
    for (int i=0; i < N; i++)
      *sum = *sum + *addend;
}


