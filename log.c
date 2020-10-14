#include <stdio.h>
#include <stdarg.h>

static int DBG = 0;

void enable_debug(void)
{
	DBG = 1;
}

void print_debug(const char* fmt, ...)
{
	if (DBG) {
		va_list args;
		va_start(args, fmt);
		vfprintf(stdout, fmt, args);
		va_end(args);
	}

}
