#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "log.h"

int EXIT = 0;

#define xstr(a) str(a)
#define str(a) #a
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static void print_usage(void)
{
	printf("backlightctl, automatic backlight control, Data Respons Solutions AB\n");
	printf("Version:   %s\n", xstr(SRC_VERSION));
	printf("\n");

	printf("Usage:   backlightctl [OPTION] PATH\n");
	printf("\n");

	printf("PATH: Path to backlight sysfs device\n");
	printf("  For example: /sys/class/backlight/backlight-lvds\n");
	printf("  Required properties:\n");
	printf("    brightness\n");
	printf("    actual_brightness\n");
	printf("    max_brightness\n");
	printf("\n");

	printf("Options:\n");
	printf("  -d, --debug    enable debug output\n");
	printf("  -i, --int      interrupt input\n");
	printf("    path to gpio interrupt input\n");
	printf("    For example: /sys/class/gpio/gpio12\n");
	printf("    Expects gpio edge property already is configured\n");
	printf("    See kernel documentation Documentation/gpio/sysfs.txt\n");
	printf("\n");

	printf("Return values:\n");
	printf("  0 if ok\n");
	printf("  errno for error\n");
	printf("\n");
}

char* join_path(const char* base, const char* add)
{
	const char *fmt = "%s/%s";
	int sz = snprintf(NULL, 0, fmt, base, add) + 1;
	if (sz < 0) {
		return NULL;
	}
	char *path = malloc(sz);
	if (snprintf(path, sz, fmt, base, add) == sz - 1) {
		return path;
	}

	free(path);
	return NULL;
}

void sighandler(int sig)
{
	(void) sig;
	EXIT = 1;
}

struct backlight {
	char *path;
	char *brightness;
	char *actual_brightness;
	char *max_brightness;
	char on_value[64];
	char off_value[64];
	struct timespec on_time; // unless new interrupt backlight will turn off after this time
	int enabled;
};

struct interrupt {
	char *path;
	char* value;
};

// read null terminated line into buf with buf_len from file. Return 0 for ok or negative errno for error
static int read_line(char* buf, int buf_len, const char* file)
{
	FILE *fp = fopen(file, "r");
	if (!fp) {
		int e = errno;
		pr_err("%s [%d] open: %s\n", file, e, strerror(e));
		return -e;
	}
	int r = 0;
	if (fgets(buf, buf_len, fp) == NULL) {
		r = -errno;
		pr_err("%s [%d] read: %s\n", file, -r, strerror(-r));
	}
	if (fclose(fp)) {
		if (!r) {
			r = -errno;
			pr_err("%s [%d] close: %s\n", file, -r, strerror(-r));
		}
	}
	return r;
}

// write null terminated line to file. Return 0 for ok or negative errno for error
static int write_line(const char* line, const char* file)
{
	FILE *fp = fopen(file, "w");
	if (!fp) {
		int e = errno;
		pr_err("%s [%d] open: %s\n", file, e, strerror(e));
		return -e;
	}
	int r = 0;
	if (fputs(line, fp) == EOF) {
		r = -errno;
		pr_err("%s [%d] write: %s\n", file, -r, strerror(-r));
	}
	if (fclose(fp)) {
		if (!r) {
			r = -errno;
			pr_err("%s [%d] close: %s\n", file, -r, strerror(-r));
		}
	}
	return r;
}

// return 0 for interrupt, 1 for timeout, negative errno for error
static int wait_int(const char* path, int timeout_ms)
{
	struct pollfd fds;
	fds.fd = open(path, O_RDONLY);
	if (fds.fd < 0) {
		int e = errno;
		pr_err("%s [%d] open: %s\n", path, e, strerror(e));
		return -e;
	}
	fds.events = POLLPRI;
	fds.revents = 0;

	int r = 0;
	switch(poll(&fds, 1, timeout_ms)) {
	case -1:
		r = -errno;
		pr_err("%s [%d] poll: %s\n", path, -r, strerror(-r));
		break;
	case 0:
		// timeout
		r = 1;
		break;
	default:
		if ((fds.revents & POLLPRI) != POLLPRI) {
			// unknown revent
			r = -EINTR;
		}
		break;
	}

	if (close(fds.fd)) {
		if (!r) {
			r = -errno;
			pr_err("%s [%d] close: %s\n", path, -r, strerror(-r));
		}
	}

	return r;
}

static int fill_backlight(struct backlight* backlight)
{
	pr_dbg("backlight: path: %s\n", backlight->path);

	backlight->brightness = join_path(backlight->path, "brightness");
	if (!backlight->brightness) {
		return -ENOMEM;
	}
	pr_dbg("backlight: brightness: %s\n", backlight->brightness);
	backlight->actual_brightness = join_path(backlight->path, "actual_brightness");
	if (!backlight->actual_brightness) {
		return -ENOMEM;
	}
	pr_dbg("backlight: actual_brightness: %s\n", backlight->actual_brightness);
	backlight->max_brightness = join_path(backlight->path, "max_brightness");
	if (!backlight->max_brightness) {
		return -ENOMEM;
	}
	pr_dbg("backlight: max_brightness: %s\n", backlight->max_brightness);
	int r = read_line(backlight->on_value, ARRAY_SIZE(backlight->on_value), backlight->actual_brightness);
	if (r) {
		return r;
	}
	pr_dbg("backlight: on_value: %s", backlight->on_value);

	_Static_assert(ARRAY_SIZE(backlight->off_value) > 2, "backlight->off_value min size is 3");
	backlight->off_value[0] = '0';
	backlight->off_value[1] = '\n';
	backlight->off_value[2] = 0;
	pr_dbg("backlight: off_value: %s", backlight->off_value);

	backlight->on_time.tv_sec = 1;
	backlight->on_time.tv_nsec = 0;
	pr_dbg("backlight: on_time: %llds\n", (long long) backlight->on_time.tv_sec);

	backlight->enabled = 1;
	pr_dbg("backlight: enabled: %d\n", backlight->enabled);

	return 0;
}

static int fill_interrupt(struct interrupt* interrupt)
{
	pr_dbg("interrupt: path: %s\n", interrupt->path);

	interrupt->value = join_path(interrupt->path, "value");
	if (!interrupt->value) {
		return -ENOMEM;
	}
	pr_dbg("interrupt: value: %s\n", interrupt->value);

	return 0;
}

static int timestamp(struct timespec* ts)
{
	if (clock_gettime(CLOCK_MONOTONIC, ts)) {
		pr_err("Failed getting CLOCK_MONOTONIC [%d]: %s\n", errno, strerror(errno));
		return -errno;
	}
	return 0;
}

static struct timespec timespec_sub(const struct timespec* ts1, const struct timespec* ts2)
{
	struct timespec ts;
	ts.tv_sec = ts1->tv_sec > ts2->tv_sec ? ts1->tv_sec - ts2->tv_sec : ts2->tv_sec - ts1->tv_sec;
	ts.tv_nsec = ts1->tv_nsec > ts2->tv_nsec ? ts1->tv_nsec - ts2->tv_nsec : ts2->tv_nsec - ts1->tv_nsec;
	return ts;
}

// return 1 if ts1 > ts2
// return -1 i ts2 > ts1
// return 0 if equal
static int timespec_cmp(const struct timespec* ts1, const struct timespec* ts2)
{
	if (ts1->tv_sec == ts2->tv_sec) {
		if (ts1->tv_nsec == ts2->tv_nsec) {
			return 0;
		}
		return ts1->tv_nsec > ts2->tv_nsec ? 1 : -1;
	}
	return ts1->tv_sec > ts2->tv_sec ? 1 : -1;
}

static int set_backlight(struct backlight* backlight, int on)
{
	pr_dbg("backlight: set brightness: %s", on ? backlight->on_value : backlight->off_value);
	int r = write_line(on ? backlight->on_value : backlight->off_value, backlight->brightness);
	if (r) {
		return r;
	}
	backlight->enabled = on ? 1 : 0;
	return r;
}

// return 0 for ok, 1 for exceeded, negative errno for error
static int time_exceeded(const struct timespec* start, const struct timespec* on_time)
{
	struct timespec now;
	int r = timestamp(&now);
	if (r) {
		return r;
	}
	struct timespec since_start = timespec_sub(&now, start);
	if (timespec_cmp(on_time, &since_start) <= 0) {
		return 1;
	}
	return 0;
}

static int control_loop(struct backlight* backlight, struct interrupt* interrupt)
{
	struct timespec start;
	int r = timestamp(&start);
	if (r) {
		return r;
	}
	const int delay_ms = 100;
	while (!EXIT) {
		r = wait_int(interrupt->value, delay_ms);
		switch (r) {
		case 0:
			// interrupt, reset timer
			r = timestamp(&start);
			if (r) {
				return r;
			}
			if (!backlight->enabled) {
				r = set_backlight(backlight, 1);
				if (r) {
					return r;
				}
			}
			usleep(delay_ms * 1000);
			break;
		case 1:
			// timeout
			if (backlight->enabled) {
				r = time_exceeded(&start, &backlight->on_time);
				switch (r) {
				case 0:
					break;
				case 1:
					r = set_backlight(backlight, 0);
					if (r) {
						return r;
					}
					break;
				default:
					return r;
				}
				break;
			}
			break;
		default:
			return r;
		}
	}

	return 0;
}

int main(int argc, char** argv)
{
	struct backlight backlight;
	memset(&backlight, 0, sizeof(backlight));
	struct interrupt interrupt;
	memset(&interrupt, 0, sizeof(interrupt));

	if (argc < 2) {
		print_usage();
		return 1;
	}

	for (int i = 1; i < argc; i++) {
		if (!strcmp("--debug", argv[i]) || !strcmp("-d", argv[i])) {
			enable_debug();
		}
		else
		if (!strcmp("--int", argv[i]) || !strcmp("-i", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid -i/--int\n");
				return 1;
			}
			interrupt.path = argv[i];
		}
		else
		if (!strcmp("--help", argv[i]) || !strcmp("-h", argv[i])) {
			print_usage();
			return 1;
		}
		else
		if ('-' == argv[i][0]) {
			fprintf(stderr, "invalid option: %s\n", argv[i]);
			return 1;
		}
		else {
			if (!backlight.path) {
				backlight.path = argv[i];
			}
			else {
				fprintf(stderr, "invalid argument: %s\n", argv[i]);
				return 1;
			}
		}
	}

	if (!backlight.path) {
		fprintf(stderr, "mandatory argument PATH missing\n");
		return 1;
	}
	if (!interrupt.path) {
		fprintf(stderr, "mandatory argument -i/--int missing\n");
		return 1;
	}

	int r = 0;

	r = fill_backlight(&backlight);
	if (r) {
		pr_err("Failed initializing backlight [%d]: %s\n", -r, strerror(-r));
		goto exit;
	}

	r = fill_interrupt(&interrupt);
	if (r) {
		pr_err("Failed initializing interrupt [%d]: %s\n", -r, strerror(-r));
		goto exit;
	}

	if (signal(SIGINT, sighandler) == SIG_ERR) {
		r = -errno;
		pr_err("Failed initializing signal handler [%d]: %s\n", -r, strerror(-r));
		goto exit;
	}

	r = control_loop(&backlight, &interrupt);

exit:
	if (backlight.brightness) {
		free(backlight.brightness);
		backlight.brightness = NULL;
	}
	if (backlight.actual_brightness) {
		free(backlight.actual_brightness);
		backlight.actual_brightness = NULL;
	}
	if (backlight.max_brightness) {
		free(backlight.max_brightness);
		backlight.max_brightness = NULL;
	}
	if (interrupt.value) {
		free(interrupt.value);
		interrupt.value = NULL;
	}

	return r;
}
