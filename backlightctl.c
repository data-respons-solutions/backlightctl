#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <math.h>
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
	printf("    If input above iio attribute nearlevel then backlight is kept enabled\n");
	printf("  -n, --near     Proximity near level override\n");
	printf("    Default to 0 if no \"nearlevel\" iio attribute for proximity input channel\n");
	printf("\n");

	printf("Return values:\n");
	printf("  0 if ok\n");
	printf("  errno for error\n");
	printf("\n");
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

/* device is a null terminated string in format "device:channel" */
static int init_iio_ch(struct iio_channel** channel, const struct iio_context* ctx, const char* device)
{
	char *token = NULL;
	char *tmp = strdup(device);
	if (!tmp)
		return -ENOMEM;
	char *rest = tmp;

	int r = -EINVAL;
	if ((token = strtok_r(rest, ":", &rest)) == NULL)
		goto exit;
	const struct iio_device *dev = iio_context_find_device(ctx, token);
	if (!dev) {
		r = -ENODEV;
		goto exit;
	}

	if ((token = strtok_r(rest, ":", &rest)) == NULL)
		goto exit;
	*channel = iio_device_find_channel(dev, token, 0);
	if (!*channel) {
		r = -ENODEV;
		goto exit;
	}

	r = 0;
exit:
	if (tmp)
		free(tmp);
	return r;
}

struct sensor {
	struct iio_channel *channel;
};

static int sensor_init(struct sensor* sensor, const struct iio_context* ctx, const char* device)
{
	if (!sensor || !ctx || !device)
		return -EINVAL;
	pr_info("sensor [device:channel]: %s\n", device);
	return init_iio_ch(&sensor->channel, ctx, device);
}

static int sensor_get(const struct sensor* sensor, uint32_t* lux)
{
	if (!sensor || !sensor->channel || !lux)
		return -EINVAL;

	long long val = 0LL;
	const int r = iio_channel_attr_read_longlong(sensor->channel, "raw", &val);
	if (r)
		return -r;

	const struct iio_data_format *fmt = iio_channel_get_data_format(sensor->channel);
	val = fmt->with_scale ? (long long) round(val * fmt->scale) : val;
	if (val > UINT32_MAX || val < 0)
		return -EIO;

	*lux = val;
	return 0;
}

struct proximity {
	struct iio_channel *channel;
	long long nearlevel;
};

/* nearlevel will override iio provided attribute.
 * nearlevel with negative value means not set and must be provided by iio device. */
static int proximity_init(struct proximity* proximity, const struct iio_context* ctx, const char* device, long long nearlevel)
{
	if (!proximity || !ctx || !device)
		return -EINVAL;
	pr_info("proximity [device:channel]: %s\n", device);

	int r = init_iio_ch(&proximity->channel, ctx, device);
	if (r)
		return r;

	proximity->nearlevel = nearlevel;
	if (proximity->nearlevel < 0) {
		if (iio_channel_find_attr(proximity->channel, "nearlevel") == NULL)
			return -ENODEV;
		r = iio_channel_attr_read_longlong(proximity->channel, "nearlevel", &proximity->nearlevel);
		if (r)
			return -r;
	}
	pr_info("proximity nearlevel: %lld\n", proximity->nearlevel);
	return 0;
}

static int proximity_get(const struct proximity* proximity, int* trigger)
{
	if (!proximity || !proximity->channel || !trigger)
		return -EINVAL;

	long long val = 0LL;
	int r = iio_channel_attr_read_longlong(proximity->channel, "raw", &val);
	if (r)
		return -r;
	*trigger = val >= proximity->nearlevel ? 1 : 0;
	return 0;
}

struct interrupt {
	char* value;
	int fd;
	int fd_set;
};

static void interrupt_free(struct interrupt* interrupt)
{
	if (!interrupt)
		return;
	if (interrupt->value) {
		free(interrupt->value);
		interrupt->value = NULL;
	}
	if (interrupt->fd_set) {
		close(interrupt->fd);
		interrupt->fd = -1;
		interrupt->fd_set = 0;
	}
}

static int interrupt_init(struct interrupt* interrupt, const char* device)
{
	if (!interrupt || !device)
		return -EINVAL;
	pr_info("interrupt: device: %s\n", device);

	int r = 0;
	char value = 0;
	interrupt->value = join_path(device, "value");
	if (!interrupt->value) {
		r = -ENOMEM;
		goto exit;
	}

	interrupt->fd = open(interrupt->value, O_RDONLY);
	if (interrupt->fd < 0){
		r = -errno;
		goto exit;
	}
	interrupt->fd_set = 1;

	/* Clear any value before using fd for polling*/
	if (read(interrupt->fd, &value, 1) < 0) {
		r = -errno;
		goto exit;
	}

	r = 0;
exit:
	if (r)
		interrupt_free(interrupt);
	return r;
}

static int interrupt_fd(const struct interrupt* interrupt, int* fd)
{
	if (!interrupt)
		return -EINVAL;
	if (!interrupt->fd_set || interrupt->fd < 0)
		return -EBADF;
	*fd = interrupt->fd;
	return 0;
}

static int interrupt_events(const struct interrupt* interrupt)
{
	(void) interrupt;
	return POLLPRI | POLLERR;
}

static int interrupt_get(const struct interrupt* interrupt, int* trigger)
{
	if (!interrupt || !trigger)
		return -EINVAL;
	if (!interrupt->fd_set || interrupt->fd < 0)
		return -EBADF;

	if (lseek(interrupt->fd, 0, SEEK_SET) != 0)
		return -errno;

	char value = 0;
	const ssize_t r = read(interrupt->fd, &value, 1);
	if (r < 0)
		return -errno;
	if (r < 1)
		return -EIO;

	*trigger = value > 0 ? 1 : 0;
	return 0;
}

struct backlight {
	char *brightness;
	char *actual_brightness;
	char *max_brightness;
};

static void backlight_free(struct backlight* backlight)
{
	if (!backlight)
		return;
	if (backlight->brightness) {
		free(backlight->brightness);
		backlight->brightness = NULL;
	}
	if (backlight->actual_brightness) {
		free(backlight->actual_brightness);
		backlight->actual_brightness = NULL;
	}
	if (backlight->max_brightness) {
		free(backlight->max_brightness);
		backlight->max_brightness = NULL;
	}
}

static int backlight_init(struct backlight* backlight, const char* device)
{
	if (!device)
		return -EINVAL;

	pr_info("backlight: device: %s\n", device);

	int r = -ENOMEM;

	backlight->brightness = join_path(device, "brightness");
	if (!backlight->brightness)
		goto exit;
	backlight->actual_brightness = join_path(device, "actual_brightness");
	if (!backlight->actual_brightness)
		goto exit;
	backlight->max_brightness = join_path(device, "max_brightness");
	if (!backlight->max_brightness)
		goto exit;

	r = 0;
exit:
	if (r)
		backlight_free(backlight);
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

static int backlight_get(struct backlight* backlight, uint32_t* value)
{
	if (!backlight || !value || !backlight->actual_brightness)
		return -EINVAL;
	return read_u32(backlight->actual_brightness, value);
}

static int backlight_set(struct backlight* backlight, uint32_t value)
{
	if (!backlight || !backlight->brightness)
		return -EINVAL;
	return write_u32(backlight->brightness, value);
}

static int backlight_max(struct backlight* backlight, uint32_t* value)
{
	if (!backlight || !value || !backlight->max_brightness)
		return -EINVAL;
	return read_u32(backlight->max_brightness, value);
}

static int timestamp(struct timespec* ts)
{
	if (clock_gettime(CLOCK_MONOTONIC, ts)) {
		pr_err("Failed getting CLOCK_MONOTONIC [%d]: %s\n", errno, strerror(errno));
		return -errno;
	}
	return 0;
}

/* Used for array indexing, be careful */
enum FDS {
	FDS_SIGNAL = 0,
	FDS_INTERRUPT,
	FDS_LENGTH, /* Number of array entries */
};

int main(int argc, char** argv)
{
	char *backlight_device = NULL;
	char *sensor_device = NULL;
	char *proximity_device = NULL;
	long long proximity_nearlevel = -1;
	char *interrupt_device = NULL;
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
			interrupt_device = argv[i];
		}
		else
		if (!strcmp("--sensor", argv[i]) || !strcmp("-s", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid -s/--sensor\n");
				return 1;
			}
			sensor_device = argv[i];
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
				fprintf(stderr, "invalid -p/--prox\n");
				return 1;
			}
			proximity_device = argv[i];
		}
		else
		if (!strcmp("--near", argv[i]) || !strcmp("-n", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "invalid -n/--near\n");
				return 1;
			}
			proximity_nearlevel = atoi(argv[i]);
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
			if (!backlight_device) {
				backlight_device = argv[i];
			}
			else {
				fprintf(stderr, "invalid argument: %s\n", argv[i]);
				return 1;
			}
		}
	}

	if (!backlight_device) {
		pr_err("mandatory argument PATH missing\n");
		return 1;
	}
	if (!interrupt_device && !sensor_device && !proximity_device) {
		pr_err("Control source missing (interrupt/sensor/proxmitity) -- see help\n");
		return 1;
	}

	struct iio_context *ctx = NULL;
	struct libbacklight_ctrl *bctl = NULL;
	struct backlight backlight;
	memset(&backlight, 0, sizeof(backlight));
	struct sensor sensor;
	memset(&sensor, 0, sizeof(sensor));
	struct proximity proximity;
	memset(&proximity, 0, sizeof(proximity));
	struct interrupt interrupt;
	memset(&interrupt, 0, sizeof(interrupt));
	struct timespec start = {0,0};
	struct timespec now = {0,0};
	struct pollfd fds[FDS_LENGTH];
	fds[FDS_SIGNAL].fd = -1;
	fds[FDS_INTERRUPT].fd = -1;
	sigset_t mask;
	int r = 0;

	if (sensor_device || proximity_device) {
		ctx = iio_create_local_context();
		if (!ctx) {
			r = -errno;
			pr_err("Failed creating iio context: %s\n", strerror(-r));
			goto exit;
		}
	}
	if (sensor_device) {
		r = sensor_init(&sensor, ctx, sensor_device);
		if (r) {
			pr_err("Failed initializing sensor [%d]: %s\n", -r, strerror(-r));
			goto exit;
		}
		conf.enable_sensor = 1;
		pr_info("sensor: max: %" PRIu32 ": min: %" PRIu32"\n", conf.max_lux, conf.min_lux);
	}
	if (proximity_device) {
		r = proximity_init(&proximity, ctx, proximity_device, proximity_nearlevel);
		if (r) {
			pr_err("Failed initializing proximity [%d]: %s\n", -r, strerror(-r));
			goto exit;
		}
		conf.enable_trigger = 1;
	}
	if (interrupt_device) {
		r = interrupt_init(&interrupt, interrupt_device);
		if (r) {
			pr_err("Failed initializing interrupt [%d]: %s\n", -r, strerror(-r));
			goto exit;
		}
	}
	r = backlight_init(&backlight, backlight_device);
	if (r) {
		pr_err("Failed initializing backlight [%d]: %s\n", -r, strerror(-r));
		goto exit;
	}

	r = backlight_get(&backlight, &conf.initial_brightness_step);
	if (r) {
		pr_err("Failed reading actual backlight [%d]: %s\n", -r, strerror(-r));
		goto exit;
	}

	r = backlight_max(&backlight, &conf.max_brightness_step);
	if (r) {
		pr_err("Failed reading max backlight [%d]: %s\n", -r, strerror(-r));
		goto exit;
	}

	pr_info("backlight: max: %" PRIu32 ": initial: %" PRIu32 "\n",
			conf.max_brightness_step, conf.initial_brightness_step);


	r = timestamp(&start);
	if (r)
		goto exit;

	if ((bctl = create_libbacklight(&start, &conf)) == NULL) {
		r = -EFAULT;
		pr_err("Failed initializing control logic");
		goto exit;
	}

	/* Install signal handler */
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
		r = -errno;
		pr_err("Failed blocking signals [%d]: %s\n", -r, strerror(-r));
		goto exit;
	}
	fds[FDS_SIGNAL].events = POLLIN;
	fds[FDS_SIGNAL].fd = signalfd(-1, &mask, 0);
	if (fds[FDS_SIGNAL].fd < 0) {
		r = -errno;
		pr_err("Failed installing signal handler [%d]: %s\n", -r, strerror(-r));
		goto exit;
	}

	if (interrupt_device) {
		fds[FDS_INTERRUPT].events = interrupt_events(&interrupt);
		r = interrupt_fd(&interrupt, &fds[FDS_INTERRUPT].fd);
		if (r) {
			pr_err ("Failed getting interrupt fd [%d]: %s\n", -r, strerror(-r));
			goto exit;
		}
	}

	r = 0;
	while (1) {
		const int delay_ms = 100;
		int detect_interrupt = 0;
		int trigger = 0;
		uint32_t lux = 0;

		/* poll for events */
		r = poll(fds, FDS_LENGTH, delay_ms);
		if (r < 0) {
			r = -errno;
			pr_err("Failed polling [%d]: %s\n", -r, strerror(-r));
			break;
		}

		/* Exit due to signal */
		if (fds[FDS_SIGNAL].revents != 0)
			break;

		/* Check for triggered interrupts */
		if ((fds[FDS_INTERRUPT].revents & (POLLPRI | POLLERR)) != 0) {
			r = interrupt_get(&interrupt, &trigger);
			if (r) {
				pr_err("interrupt: failed reading [%d]: %s\n", -r, strerror(-r));
				break;
			}
			if (trigger)
				pr_dbg("interrupt: yes\n");
			detect_interrupt |= trigger;
		}

		if (sensor_device) {
			r = sensor_get(&sensor, &lux);
			if (r) {
				pr_err("sensor: failed reading [%d]: %s\n", -r, strerror(-r));
				break;
			}
		}
		if (proximity_device) {
			r = proximity_get(&proximity, &trigger);
			if (r) {
				pr_err("proximity: failed reading [%d]: %s\n", -r, strerror(-r));
				break;
			}
			if (trigger)
				pr_dbg("proximity: yes\n");
			detect_interrupt |= trigger;
		}

		r = timestamp(&now);
		if (r)
			break;

		if (libbacklight_operate(bctl, &now, detect_interrupt, lux) == LIBBACKLIGHT_BRIGHTNESS) {
			pr_dbg("backlight: brightness -> %" PRIu32 ": lux: %" PRIu32 "\n", libbacklight_brightness(bctl), lux);
			r = backlight_set(&backlight, libbacklight_brightness(bctl));
			if (r)
				break;
		}
	}
	/* Restore backlight setting */
	backlight_set(&backlight, libbacklight_get_conf(bctl)->initial_brightness_step);
exit:

	backlight_free(&backlight);
	interrupt_free(&interrupt);
	if (ctx)
		iio_context_destroy(ctx);
	destroy_libbacklight(&bctl);
	return -r;
}
