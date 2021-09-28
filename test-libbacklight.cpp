#include <cstring>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <cstdio>

#include "circular_buf.h"

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
	uint32_t brightness_step; 		// Brightness now
	struct timespec last_trigger;   // Last time trigger received
	circular_buf_t lux;
};

int libbacklight_init(struct libbacklight_ctrl* ctrl, const struct timespec* ts, const struct libbacklight_conf* conf)
{
	memset(ctrl, 0, sizeof(struct libbacklight_ctrl));

	if (conf->max_brightness_step == 0)
		return -EINVAL;

	if (conf->enable_trigger) {
		if (conf->trigger_timeout.tv_sec == 0 && conf->trigger_timeout.tv_nsec == 0)
			return -EINVAL;
		memcpy(&ctrl->last_trigger, ts, sizeof(struct timespec));
	}

	memcpy(&ctrl->conf, conf, sizeof(struct libbacklight_conf));

	ctrl->brightness_step = conf->initial_brightness_step;

	return 0;
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

	(void) triggered;
	(void) lux;

	return ac;
}

TEST_CASE("libbacklight_init simple") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;

	struct libbacklight_ctrl ctrl;
	struct timespec ts = {0,0};
	int r = libbacklight_init(&ctrl, &ts, &conf);
	REQUIRE(r == 0);
}

TEST_CASE("libbacklight_init trigger") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	conf.enable_trigger = 1;
	conf.trigger_timeout.tv_sec = 10;
	struct libbacklight_ctrl ctrl;
	struct timespec ts = {0,0};
	int r = libbacklight_init(&ctrl, &ts, &conf);
	REQUIRE(r == 0);
}

TEST_CASE("libbacklight_init sensor") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	conf.enable_sensor = 1;
	conf.lux_min = 10;
	conf.lux_max = 600;
	struct libbacklight_ctrl ctrl;
	struct timespec ts = {0,0};
	int r = libbacklight_init(&ctrl, &ts, &conf);
	REQUIRE(r == 0);
}

TEST_CASE("Test trigger") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	conf.enable_trigger = 1;
	conf.trigger_timeout.tv_sec = 10;
	struct libbacklight_ctrl ctrl;
	struct timespec ts = {0,0};
	int r = libbacklight_init(&ctrl, &ts, &conf);
	REQUIRE(r == 0);

	SECTION("Timeout") {
		enum libbacklight_action ac = LIBBACKLIGHT_NONE;
		for (int i = 0; i < 10; ++i) {
			ts.tv_sec = i;
			ac = libbacklight_operate(&ctrl, &ts, 0, 0);
			REQUIRE(ac == LIBBACKLIGHT_NONE);
		}
		ts.tv_sec = 10;
		ac = libbacklight_operate(&ctrl, &ts, 0, 0);
		REQUIRE(ac == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(ctrl.brightness_step == 0);

		SECTION("Trigger") {
			ac = libbacklight_operate(&ctrl, &ts, 1, 0);
			REQUIRE(ac == LIBBACKLIGHT_BRIGHTNESS);
			REQUIRE(ctrl.brightness_step == conf.initial_brightness_step);

			SECTION("Timeout again")
				ts.tv_sec = 21;
				ac = libbacklight_operate(&ctrl, &ts, 0, 0);
				REQUIRE(ac == LIBBACKLIGHT_BRIGHTNESS);
				REQUIRE(ctrl.brightness_step == 0);
		}
	}
}

