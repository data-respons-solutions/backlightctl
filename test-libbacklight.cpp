#include <cstring>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <cstdio>

#include "ringbuf.h"

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

struct libbacklight_conf {
	uint32_t max_brightness_step;		// Total number of steps available.
	uint32_t initial_brightness_step;	// Step we're starting from.
	int enable_sensor;					// Calculate brightness based on min/max_lux.
	uint32_t min_lux;					// This value corresponds to brightness step 1.
	uint32_t max_lux;					// This value corresponds to max_brightness_step.
	int enable_trigger;					// Enable backlight after trigger received.
										// Will set backlight to initial_brightness_step,
										// unless enable_sensor is set, then the value is adjusted
										// based on sensor input.
	struct timespec trigger_timeout;	// Time without any trigger until backlight is turned off, step 0.
};

struct libbacklight_ctrl {
	struct libbacklight_conf conf;
	struct timespec last_trigger;	// Last time trigger received
	struct ringbuf *sensor_ring;	// Ringbuffer for sensor readings
	uint64_t sensor_sum;			// Sum of ringbuffer values
	uint32_t brightness_step; 		// Brightness now
};

void destroy_libbacklight(struct libbacklight_ctrl** ctrl);

struct libbacklight_ctrl* create_libbacklight(const struct timespec* ts, const struct libbacklight_conf* conf)
{
	struct libbacklight_ctrl *ctrl = (struct libbacklight_ctrl*) malloc(sizeof(struct libbacklight_ctrl));
	if (!ctrl)
		goto error_exit;

	memset(ctrl, 0, sizeof(struct libbacklight_ctrl));

	if (conf->max_brightness_step == 0)
		goto error_exit;

	if (conf->enable_trigger) {
		if (conf->trigger_timeout.tv_sec == 0 && conf->trigger_timeout.tv_nsec == 0)
			goto error_exit;
		memcpy(&ctrl->last_trigger, ts, sizeof(struct timespec));
	}

	if (conf->enable_sensor) {
		ctrl->sensor_ring = create_ringbuf(10);
		if (!ctrl->sensor_ring)
			goto error_exit;
	}

	memcpy(&ctrl->conf, conf, sizeof(struct libbacklight_conf));

	ctrl->brightness_step = conf->initial_brightness_step;

	return ctrl;

error_exit:
	destroy_libbacklight(&ctrl);
	return NULL;
}

void destroy_libbacklight(struct libbacklight_ctrl** ctrl)
{
	if (*ctrl) {
		if ((*ctrl)->sensor_ring)
			destroy_ringbuf(&(*ctrl)->sensor_ring);
		free(*ctrl);
		*ctrl = NULL;
	}
}

enum libbacklight_action {
	LIBBACKLIGHT_NONE,
	LIBBACKLIGHT_BRIGHTNESS,
};

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
		if (ts1->tv_nsec == ts2->tv_nsec)
			return 0;
		return ts1->tv_nsec > ts2->tv_nsec ? 1 : -1;
	}
	return ts1->tv_sec > ts2->tv_sec ? 1 : -1;
}

enum libbacklight_action libbacklight_operate(struct libbacklight_ctrl* ctrl, const struct timespec* ts, int triggered, uint32_t lux)
{
	enum libbacklight_action ac = LIBBACKLIGHT_NONE;

	if (ctrl->conf.enable_trigger) {
		if (triggered) {
			memcpy(&ctrl->last_trigger, ts, sizeof(struct timespec));
			if (ctrl->brightness_step == 0) {
				ctrl->brightness_step = ctrl->conf.initial_brightness_step;
				ac = LIBBACKLIGHT_BRIGHTNESS;
			}
		}
		else
		if (ctrl->brightness_step > 0) {
			const struct timespec since_last = timespec_sub(&ctrl->last_trigger, ts);
			if (timespec_cmp(&ctrl->conf.trigger_timeout, &since_last) <= 0) {
				ctrl->brightness_step = 0;
				ac = LIBBACKLIGHT_BRIGHTNESS;
			}
		}
	}

	(void) lux;

	return ac;
}

uint32_t libbacklight_brightness(const struct libbacklight_ctrl* ctrl)
{
	return ctrl->brightness_step;
}

TEST_CASE("create_libbacklight simple") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	struct timespec ts = {0,0};
	struct libbacklight_ctrl *bctl = create_libbacklight(&ts, &conf);
	REQUIRE(bctl);
}

TEST_CASE("create_libbacklight trigger") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	conf.enable_trigger = 1;
	conf.trigger_timeout.tv_sec = 10;
	struct timespec ts = {0,0};
	struct libbacklight_ctrl *bctl = create_libbacklight(&ts, &conf);
	REQUIRE(bctl);
}

TEST_CASE("create_libbacklight sensor") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	conf.enable_sensor = 1;
	conf.min_lux = 10;
	conf.max_lux = 600;
	struct timespec ts = {0,0};
	struct libbacklight_ctrl *bctl = create_libbacklight(&ts, &conf);
	REQUIRE(bctl);
}

TEST_CASE("Test trigger") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	conf.enable_trigger = 1;
	conf.trigger_timeout.tv_sec = 10;
	const struct timespec start = {0,0};
	struct libbacklight_ctrl *bctl = create_libbacklight(&start, &conf);
	REQUIRE(bctl);

	SECTION("No action") {
		for (int i = 0; i < 10; ++i) {
			struct timespec ts {i, 0};
			enum libbacklight_action ac = libbacklight_operate(bctl, &ts, 0, 0);
			REQUIRE(ac == LIBBACKLIGHT_NONE);
			REQUIRE(libbacklight_brightness(bctl) == conf.initial_brightness_step);
		}
	}

	SECTION("Timeout") {
		struct timespec ts {10, 0};
		enum libbacklight_action ac = libbacklight_operate(bctl, &ts, 0, 0);
		REQUIRE(ac == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == 0);
	}

	SECTION("Timeout - Trigger - Timeout") {
		struct timespec ts {10, 0};
		enum libbacklight_action ac = libbacklight_operate(bctl, &ts, 0, 0);
		REQUIRE(ac == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == 0);

		ac = libbacklight_operate(bctl, &ts, 1, 0);
		REQUIRE(ac == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == conf.initial_brightness_step);

		ts.tv_sec = 21;
		ac = libbacklight_operate(bctl, &ts, 0, 0);
		REQUIRE(ac == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == 0);
	}
}

