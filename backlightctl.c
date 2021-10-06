#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <time.h>
#include <iio.h>
#include "log.h"
#include "libbacklight.h"

#define xstr(a) str(a)
#define str(a) #a

#define DEFAULT_ON_TIME_SEC 30
#define DEFAULT_MIN_LUX 10
#define DEFAULT_MAX_LUX 600

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
	printf("    Turn off backlight after --time inactivity\n");
	printf("    Expects gpio edge property already is configured\n");
	printf("    See kernel documentation Documentation/gpio/sysfs.txt\n");
	printf("  -t, --time     Time in seconds to wait for interrupt before disabling backlight\n");
	printf("    Default: %d\n", DEFAULT_ON_TIME_SEC);
	printf("  -s, --sensor   Sensor input\n");
	printf("    iio device and channel in format dev:chan\n");
	printf("    For example: vcnl4000:illuminance\n");
	printf("    Control backlight based on sensor input\n");
	printf("  --lmin         Lux value where backlight it set to 1\n");
	printf("    Default: %d\n", DEFAULT_MIN_LUX);
	printf("  --lmax         Lux value where backlight it set to max\n");
	printf("    Default: %d\n", DEFAULT_MAX_LUX);
	printf("  -p, --prox     Proximity input\n");
	printf("    iio device and channel in format dev:chan\n");
	printf("    For example: vcnl4000:proximity\n");
	printf("    Turn off backlight after --time inactivity\n");
	printf("    If input below iio attribute nearlevel then backlight is kept enabled\n");
	printf("  -n, --near     Proximity near level override\n");
	printf("    Default to 0 if no \"nearlevel\" iio attribute for proximity input channel\n");
	printf("\n");

	printf("Return values:\n");
	printf("  0 if ok\n");
	printf("  errno for error\n");
	printf("\n");
}

struct devices {
	char *backlight_path;
	char *backlight_brightness;
	char *backlight_actual_brightness;
	char *backlight_max_brightness;
	char *interrupt_path;
	char *interrupt_value;
	char *sensor;
	struct iio_channel *sensor_ch;
	char *prox;
	struct iio_channel *prox_ch;
	long long prox_nearlevel;
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
	struct iio_context* ctx = NULL;
	if (!ctx && d->sensor_ch) {
		ctx = (struct iio_context*) iio_device_get_context(iio_channel_get_device(d->sensor_ch));
		d->sensor_ch = NULL;
	}
	if (!ctx && d->prox_ch) {
		ctx = (struct iio_context*) iio_device_get_context(iio_channel_get_device(d->prox_ch));
		d->prox_ch = NULL;
	}
	if (ctx)
		iio_context_destroy(ctx);
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

struct iio_channel* init_iio_ch(const struct iio_context* ctx, const char* device)
{
	struct iio_channel *ch = NULL;
	char *token = NULL;
	char *tmp = malloc(strlen(device) + 1);
	if (!tmp) {
		pr_err("Failed allocating memory for iio sensor name and channel\n");
		goto exit;
	}
	char *rest = tmp;
	memcpy(tmp, device, strlen(device) + 1);

	if ((token = strtok_r(rest, ":", &rest)) == NULL) {
		pr_err("Failed extracting sensor device from name\n");
		goto exit;
	}
	const struct iio_device *dev = iio_context_find_device(ctx, token);
	if (!dev) {
		pr_err("Failed finding iio device: %s\n", token);
		goto exit;
	}

	if ((token = strtok_r(rest, ":", &rest)) == NULL) {
		pr_err("Failed extracting sensor channel from name\n");
		goto exit;
	}
	ch = iio_device_find_channel(dev, token, 0);
	if (!ch)
		pr_err("Failed finding iio channel %s\n", token);

exit:
	if (tmp)
		free(tmp);

	return ch;
}

/*
 *  Fills backlight_* and interrupt_* if respective path available.
 */
static int init_devices(struct devices* d)
{
	struct iio_context *ctx = NULL;
	int r = -ENOMEM;

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

	if (d->sensor || d->prox) {
		pr_dbg("sensor [device:channel]: %s\n", d->sensor);
		ctx = iio_create_local_context();
		if (!ctx) {
			r = -errno;
			pr_err("Failed creating iio context: %s\n", strerror(-r));
			goto error_exit;
		}
		if (d->sensor && (d->sensor_ch = init_iio_ch(ctx, d->sensor)) == NULL) {
			r = -ENODEV;
			goto error_exit;
		}
		if (d->prox && (d->prox_ch = init_iio_ch(ctx, d->prox)) == NULL) {
			r = -ENODEV;
			goto error_exit;
		}
	}

	return 0;
error_exit:
	if (r == -ENOMEM)
		pr_err("Memory allocation failed for file paths\n");
	if (ctx)
		iio_context_destroy(ctx);
	d->sensor_ch = NULL;
	d->prox_ch = NULL;
	free_devices(d);
	return r;
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

static int init_conf(struct libbacklight_conf* conf, struct devices* d)
{
	int r = read_u32(d->backlight_max_brightness, &conf->max_brightness_step);
	if (r)
		return r;
	r = read_u32(d->backlight_actual_brightness, &conf->initial_brightness_step);
	if (r)
		return r;

	pr_dbg("backlight: max: %" PRIu32 ": initial: %" PRIu32 "\n",
			conf->max_brightness_step, conf->initial_brightness_step);

	if (conf->enable_sensor)
		pr_dbg("sensor: max: %" PRIu32 ": min: %" PRIu32"\n", conf->max_lux, conf->min_lux);

	if (d->prox_ch) {
		if (iio_channel_find_attr(d->prox_ch, "nearlevel") != NULL) {
			r = iio_channel_attr_read_longlong(d->prox_ch, "nearlevel", &d->prox_nearlevel);
			if (r) {
				pr_err("prox: nearlevel reading failed: %s\n", strerror(-r));
				return -EIO;
			}
		}
		pr_dbg("prox: nearlevel: %" PRIu32 "\n", d->prox_nearlevel);
	}

	return 0;
}

// Return 0 for OK and signal state in *interrupt
// Negative errno for error
static int wait_fd(struct pollfd* fds, int timeout_ms, int* interrupt)
{
	int r = poll(fds, 1, timeout_ms);
	if (r < 0) {
		r = -errno;
		pr_err("poll [%d]: %s\n", -r, strerror(-r));
	}
	else
	if (r > 0 && fds->revents > 0) {
		*interrupt = 1;
		fds->revents = 0;
		r = 0;
	}
	else {
		*interrupt = 0;
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

static int get_interrupt(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fd = -errno;
	}
	else {
		// clear value before polling
		char value;
		if (read(fd, &value, 1) < 0) {
			close(fd);
			fd = -errno;
	    }
	}
	return fd;
}

static int get_signalfd(void)
{
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	int r = -1;

	if (sigprocmask(SIG_BLOCK, &mask, NULL) != -1)
		r = signalfd(-1, &mask, 0);
	return r;
}

static int read_sensor(const struct iio_channel* ch, uint32_t* lux)
{
	long long val = 0LL;
	int r = iio_channel_attr_read_longlong(ch, "raw", &val);
	if (r) {
		pr_err("Failed reading sensor: %s\n", strerror(-r));
		return r;
	}

	const struct iio_data_format *fmt = iio_channel_get_data_format(ch);
	val = fmt->with_scale ? (long long) (val * fmt->scale + 0.5) : val;
	if (val > UINT32_MAX || val < 0) {
		pr_err("Sensor reading invalid: value: %lld\n", val);
		return -EIO;
	}

	*lux = val;
	return 0;
}

static int read_prox(const struct iio_channel* ch, long long nearlevel, long long* prev, int* trigger)
{
	long long val = 0LL;
	int r = iio_channel_attr_read_longlong(ch, "raw", &val);
	if (r) {
		pr_err("Failed reading prox: %s\n", strerror(-r));
		return r;
	}

	*trigger = (val <= nearlevel || val != *prev) ? 1 : 0;
	*prev = val;
	return 0;
}

static int control_loop(struct libbacklight_ctrl* bctl, const struct devices* d)
{
	const int delay_ms = 100;
	struct timespec ts = {0,0};
	int r = 0;
	uint32_t lux = 0;
	struct pollfd signalfd;
	signalfd.fd = get_signalfd();
	if (signalfd.fd < 0) {
		pr_err("Failed installing signal handler\n");
		return -EFAULT;
	}
	signalfd.events = POLLIN;
	signalfd.revents = 0;

	struct pollfd interruptfd;
	interruptfd.fd = -1;
	if (d->interrupt_value) {
		interruptfd.fd = get_interrupt(d->interrupt_value);
		if (interruptfd.fd < 0) {
			r = -errno;
			pr_err("%s [%d]: %s\n", d->interrupt_value, -r, strerror(-r));
			return r;
		}
		interruptfd.events = POLLPRI;
		interruptfd.revents = 0;
	}
	long long prox_previous = 0LL;

	while (1) {
		int trigger = 0;

		if (!trigger && interruptfd.fd >= 0) {
			r = wait_fd(&interruptfd, 0, &trigger);
			if (r)
				goto exit;
			if (trigger != 0)
				pr_dbg("interrupt: %d\n", trigger);
		}

		if (!trigger && d->prox_ch) {
			r = read_prox(d->prox_ch, d->prox_nearlevel, &prox_previous, &trigger);
			if (r)
				goto exit;
			if (trigger != 0)
				pr_dbg("prox: %d: raw %lld\n", trigger, prox_previous);
		}

		if (d->sensor_ch) {
			r = read_sensor(d->sensor_ch, &lux);
			if (r)
				goto exit;
		}

		r = timestamp(&ts);
		if (r)
			goto exit;

		if (libbacklight_operate(bctl, &ts, trigger, lux) == LIBBACKLIGHT_BRIGHTNESS) {
			pr_dbg("backlight: brightness -> %" PRIu32 ": lux: %" PRIu32 "\n", libbacklight_brightness(bctl), lux);
			r = write_u32(d->backlight_brightness, libbacklight_brightness(bctl));
			if (r)
				goto exit;
		}

		r = wait_fd(&signalfd, delay_ms, &trigger);
		if (r)
			goto exit;
		if (trigger != 0)
			break;
	}

	r = 0;

exit:
	write_u32(d->backlight_brightness, libbacklight_get_conf(bctl)->initial_brightness_step);
	if (interruptfd.fd >= 0)
		close(interruptfd.fd);
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
	conf.min_lux = DEFAULT_MIN_LUX;
	conf.max_lux = DEFAULT_MAX_LUX;

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
		if (!strcmp("--sensor", argv[i]) || !strcmp("-s", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid -s/--sensor\n");
				return 1;
			}
			devices.sensor = argv[i];
			conf.enable_sensor = 1;
		}
		else
		if (!strcmp("--lmin", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid --lmin\n");
				return 1;
			}
			conf.min_lux = atoi(argv[i]);
		}
		else
		if (!strcmp("--lmax", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid --lmax\n");
				return 1;
			}
			conf.max_lux = atoi(argv[i]);
		}
		else
		if (!strcmp("--prox", argv[i]) || !strcmp("-p", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid -s/--sensor\n");
				return 1;
			}
			devices.prox = argv[i];
			conf.enable_trigger = 1;
		}
		else
		if (!strcmp("--near", argv[i]) || !strcmp("-n", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid -s/--sensor\n");
				return 1;
			}
			devices.prox_nearlevel = atoi(argv[i]);
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
	if (!devices.interrupt_path && !devices.sensor && !devices.prox) {
		pr_err("Control source missing (interrupt/sensor/proxmitity) -- see help\n");
		return 1;
	}

	struct timespec start = {0,0};
	int r = init_devices(&devices);
	if (r)
		goto exit;

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
