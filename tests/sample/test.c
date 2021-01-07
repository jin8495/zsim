#include <stdio.h>

#define N 4096

int main()
{
	int i;
	int a[N], b[N], c[N];

	for(i = 0; i < N; i++)
	{
		a[i] = 8008;
		b[i] = 110;
	}

	for(i = 0; i < N; i++)
		c[i] = a[i] + b[i];
//	for(i = 0; i < N; i++)
//		printf("%d, %d + %d = %d\n", i, a[i], b[i], c[i]);

	return 0;
}
