#include <stdio.h>

#define N 1024

int main()
{
	int i;
	int a[N], b[N], c[N];

	for(i = 0; i < N; i++)
	{
		a[i] = i;
		b[i] = 2*i+i;
	}

	for(i = 0; i < N; i++)
		c[i] = a[i] + b[i];
	for(i = 0; i < N; i++)
		printf("%d, %d + %d = %d\n", i, a[i], b[i], c[i]);

	return 0;
}
