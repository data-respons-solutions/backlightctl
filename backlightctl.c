#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "log.h"
#include "libbacklight.h"

int EXIT = 0;

#define xstr(a) str(a)
#define str(a) #a

#define DEFAULT_ON_TIME_SEC 30

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
	printf("  Will toggle between actual_brightness and 0\n");
	printf("\n");

	printf("Options:\n");
	printf("  -d, --debug    enable debug output\n");
	printf("  -i, --int      interrupt input\n");
	printf("    path to gpio interrupt input\n");
	printf("    For example: /sys/class/gpio/gpio12\n");
	printf("    Expects gpio edge property already is configured\n");
	printf("    See kernel documentation Documentation/gpio/sysfs.txt\n");
	printf("  -t, --time     Time in seconds to wait for interrupt before disabling backlight\n");
	printf("    Default: %d\n", DEFAULT_ON_TIME_SEC);
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
};

struct interrupt {
	char *path;
	char* value;
};

static int read_u32(const char* path, uint32_t* value)
{
	int r = 0;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		r = -errno;
		pr_err("%s [%d] open: %s\n", path, -r, strerror(-r));
		return r;
	}

	const size_t buf_size = 64;
	char buf[buf_size];
	const ssize_t bytes = read(fd, buf, buf_size - 1);
	const int read_errno = errno;
	if (close(fd) != 0) {
		r = -errno;
		pr_err("%s [%d] close: %s\n", path, -r, strerror(-r));
		return r;
	}
	if (bytes < 0) {
		r = -read_errno;
		pr_err("%s [%d] read: %s\n", path, -r, strerror(-r));
		return r;
	}
	buf[bytes + 1] = '\0';
	if (sscanf(buf, "%" PRIu32 "", value) != 1) {
		r = -EFAULT;
		pr_err("%s [%d]: sscanf: %s\n", path, -r, strerror(-r));
		return r;
	}

	return 0;
}

static int write_u32(const char* path, uint32_t value)
{
	const int buf_size = 64;
	char buf[buf_size];
	int r = 0;
	const int count = snprintf(buf, buf_size, "%" PRIu32"\n", value);
	if (count < 0)
		r = -errno;
	if (count >= buf_size)
		r = -EINVAL;
	if (r) {
		pr_err("%s [%d]: snprintf: %s\n", path, -r, strerror(-r));
		return r;
	}

	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		r = -errno;
		pr_err("%s [%d] open: %s\n", path, -r, strerror(-r));
		return r;
	}

	const ssize_t bytes = write(fd, buf, count);
	const int write_errno = errno;
	if (close(fd) != 0) {
		r = -errno;
		pr_err("%s [%d] close: %s\n", path, -r, strerror(-r));
		return r;
	}
	if (bytes < 0) {
		r = -write_errno;
		pr_err("%s [%d] write: %s\n", path, -r, strerror(-r));
		return r;
	}

	return 0;
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

	// clear value before poll
	char value;
	if (read(fds.fd, &value, 1) < 0) {
		r = -errno;
		pr_err("%s [%d] read: %s\n", path, -r, strerror(-r));
		goto exit;
    }

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
			pr_err("%s [%d] poll: %s\n", path, -r, strerror(-r));
		}
		break;
	}

exit:
	if (close(fds.fd)) {
		if (!r) {
			r = -errno;
			pr_err("%s [%d] close: %s\n", path, -r, strerror(-r));
		}
	}

	return r;
}

static int fill_backlight(struct backlight* backlight, struct libbacklight_conf* conf)
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

	int r = read_u32(backlight->max_brightness, &conf->max_brightness_step);
	if (r)
		return r;
	r = read_u32(backlight->actual_brightness, &conf->initial_brightness_step);
	if (r)
		return r;

	pr_dbg("backlight: max: %" PRIu32 ": initial: %" PRIu32 "\n",
			conf->max_brightness_step, conf->initial_brightness_step);
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

static int control_loop(const struct libbacklight_conf* conf, const struct backlight* backlight, const struct interrupt* interrupt)
{
	struct libbacklight_ctrl *bctl = NULL;
	struct timespec ts = {0,0};
	int r = timestamp(&ts);
	if (!r)
		bctl = create_libbacklight(&ts, conf);
	if (r || !bctl) {
		pr_err("Failed creating libbacklight\n");
		r = -EINVAL;
		goto exit;
	}

	const int delay_ms = 100;
	while (!EXIT) {
		int trigger = 0;
		r = wait_int(interrupt->value, delay_ms);
		switch(r) {
		case 0: // interrupt
			trigger = 1;
			break;
		case 1: // timeout
			break;
		default: // error
			goto exit;
		}

		r = timestamp(&ts);
		if (r)
			goto exit;

		if (libbacklight_operate(bctl, &ts, trigger, 0) == LIBBACKLIGHT_BRIGHTNESS) {
			r = write_u32(backlight->brightness, libbacklight_brightness(bctl));
			if (r)
				goto exit;
		}
	}

	r = 0;

exit:
	// restore initial brightness
	if (bctl) {
		write_u32(backlight->brightness, libbacklight_get_conf(bctl)->initial_brightness_step);
		destroy_libbacklight(&bctl);
	}

	return r;
}

int main(int argc, char** argv)
{
	struct backlight backlight;
	memset(&backlight, 0, sizeof(backlight));
	struct interrupt interrupt;
	memset(&interrupt, 0, sizeof(interrupt));
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.trigger_timeout.tv_sec = DEFAULT_ON_TIME_SEC;

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
			conf.enable_trigger = 1;
		}
		else
		if (!strcmp("--time", argv[i]) || !strcmp("-t", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid -t/--time\n");
				return 1;
			}
			conf.trigger_timeout.tv_sec = atoi(argv[i]);
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
	r = fill_backlight(&backlight, &conf);
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

	r = control_loop(&conf, &backlight, &interrupt);

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
