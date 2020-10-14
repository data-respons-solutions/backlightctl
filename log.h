#ifndef _LOG_H_
#define _LOG_H_

#define pr_dbg(fmt, ...) \
		print_debug("dbg: " fmt, ##__VA_ARGS__);

#define pr_err(fmt, ...) \
		fprintf(stderr, "error: " fmt, ##__VA_ARGS__);

void enable_debug(void);
void print_debug(const char* fmt, ...);

#endif // _LOG_H_
