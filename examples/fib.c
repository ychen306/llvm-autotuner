#include <stdio.h>

#define N 40

int get(int n)
{
	return n;
}

int main()
{
	int a = 0,
		b = 1,
		i; 
	
	for (i = 0; i < N; i++) { 
		int t = get(a) + get(b);
		a = get(b);
		b = get(t);
	} 
	printf("%d\n", a);
}
