#include <stdio.h>

#define N 100

int main()
{
	int a = 0,
		b = 1,
		i; 
	
	for (i = 0; i < N; i++) { 
		int t = a + b;
		a = b;
		b = t;
	} 
	printf("%d\n", a);
}
