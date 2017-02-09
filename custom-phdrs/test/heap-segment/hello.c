#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	char *out;
	int x = asprintf(&out, "Hello, world!\n");
	if (x > 0)
	{
		printf("Stored at %p: %s\n", out, out);
		free(out);
	}
	return !(x > 0);
}
