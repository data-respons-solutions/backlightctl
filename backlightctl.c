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

void sighandler(int sig)
{
	(void) sig;
	EXIT = 1;
}

struct devices {
	char *backlight_path;
	char *backlight_brightness;
	char *backlight_actual_brightness;
	char *backlight_max_brightness;
	char *interrupt_path;
	char *interrupt_value;
};

static void free_devices(struct devices* d)
{
	if (d->backlight_brightness) {
		free(d->backlight_brightness);
		d->backlight_brightness = NULL;
	}
	if (d->backlight_actual_brightness) {
		free(d->backlight_actual_brightness);
		d->backlight_actual_brightness = NULL;
	}
	if (d->backlight_max_brightness) {
		free(d->backlight_max_brightness);
		d->backlight_max_brightness = NULL;
	}
	if (d->interrupt_value) {
		free(d->interrupt_value);
		d->interrupt_value = NULL;
	}
}

static char* join_path(const char* base, const char* add)
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


/*
 *  Fills backlight_* and interrupt_* if respective path available.
 */
static int init_devices(struct devices* d)
{
	if (d->backlight_path) {
		pr_dbg("backlight: path: %s\n", d->backlight_path);

		if (!(d->backlight_brightness = join_path(d->backlight_path, "brightness")))
			goto error_exit;
		if (!(d->backlight_actual_brightness = join_path(d->backlight_path, "actual_brightness")))
			goto error_exit;
		if (!(d->backlight_max_brightness = join_path(d->backlight_path, "max_brightness")))
			goto error_exit;
	}

	if (d->interrupt_path) {
		pr_dbg("interrupt: path: %s\n", d->interrupt_path);

		if (!(d->interrupt_value = join_path(d->interrupt_path, "value")))
			goto error_exit;
	}

	return 0;

error_exit:
	free_devices(d);
	return -ENOMEM;
}

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

static int init_conf(struct libbacklight_conf* conf, const struct devices* d)
{
	int r = read_u32(d->backlight_max_brightness, &conf->max_brightness_step);
	if (r)
		return r;
	r = read_u32(d->backlight_actual_brightness, &conf->initial_brightness_step);
	if (r)
		return r;

	pr_dbg("backlight: max: %" PRIu32 ": initial: %" PRIu32 "\n",
			conf->max_brightness_step, conf->initial_brightness_step);

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

static int timestamp(struct timespec* ts)
{
	if (clock_gettime(CLOCK_MONOTONIC, ts)) {
		pr_err("Failed getting CLOCK_MONOTONIC [%d]: %s\n", errno, strerror(errno));
		return -errno;
	}
	return 0;
}

static int control_loop(struct libbacklight_ctrl* bctl, const struct devices* d)
{
	const int delay_ms = 100;
	struct timespec ts = {0,0};
	int r = 0;

	while (!EXIT) {
		int trigger = 0;
		r = wait_int(d->interrupt_value, delay_ms);
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
			r = write_u32(d->backlight_brightness, libbacklight_brightness(bctl));
			if (r)
				goto exit;
		}
	}

	r = 0;

exit:
	write_u32(d->backlight_brightness, libbacklight_get_conf(bctl)->initial_brightness_step);

	return r;
}

int main(int argc, char** argv)
{
	struct libbacklight_ctrl *bctl = NULL;
	struct devices devices;
	memset(&devices, 0, sizeof(devices));
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
			devices.interrupt_path = argv[i];
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
			if (!devices.backlight_path) {
				devices.backlight_path = argv[i];
			}
			else {
				fprintf(stderr, "invalid argument: %s\n", argv[i]);
				return 1;
			}
		}
	}

	if (!devices.backlight_path) {
		pr_err("mandatory argument PATH missing\n");
		return 1;
	}
	if (!devices.interrupt_path) {
		pr_err("mandatory argument -i/--int missing\n");
		return 1;
	}

	struct timespec start = {0,0};
	int r = init_devices(&devices);
	if (r) {
		pr_err("Memory allocation failed for file paths\n");
		goto exit;
	}

	r = init_conf(&conf, &devices);
	if (r)
		goto exit;

	r = timestamp(&start);
	if (r)
		goto exit;

	if ((bctl = create_libbacklight(&start, &conf)) == NULL) {
		r = -EFAULT;
		pr_err("Failed initializing control logic");
		goto exit;
	}

	r = control_loop(bctl, &devices);
exit:
	free_devices(&devices);
	destroy_libbacklight(&bctl);
	return r;
}
