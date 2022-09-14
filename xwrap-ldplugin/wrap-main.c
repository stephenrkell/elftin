#include <stdio.h>

extern int __real_main(int argc, char **argv);
int __wrap_main(int argc, char **argv)
{
	printf("Hello, before world!\n");
	return __real_main(argc, argv);
}
