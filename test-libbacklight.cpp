#include <cstring>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include "libbacklight.h"

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

TEST_CASE("create_libbacklight simple") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	struct timespec ts = {0,0};
	struct libbacklight_ctrl *bctl = create_libbacklight(&ts, &conf);
	REQUIRE(bctl);

	destroy_libbacklight(&bctl);
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

	destroy_libbacklight(&bctl);
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

	destroy_libbacklight(&bctl);
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
			REQUIRE(libbacklight_operate(bctl, &ts, 0, 0) == LIBBACKLIGHT_NONE);
			REQUIRE(libbacklight_brightness(bctl) == conf.initial_brightness_step);
		}
	}

	SECTION("Timeout") {
		struct timespec ts {10, 0};
		REQUIRE(libbacklight_operate(bctl, &ts, 0, 0) == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == 0);
	}

	SECTION("Timeout - Trigger - Timeout") {
		struct timespec ts {10, 0};
		REQUIRE(libbacklight_operate(bctl, &ts, 0, 0) == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == 0);

		REQUIRE(libbacklight_operate(bctl, &ts, 1, 0) == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == conf.initial_brightness_step);

		ts.tv_sec = 21;
		REQUIRE(libbacklight_operate(bctl, &ts, 0, 0) == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == 0);
	}

	destroy_libbacklight(&bctl);
}

TEST_CASE("Test sensor") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	conf.enable_sensor = 1;
	conf.min_lux = 42;
	conf.max_lux = 600;
	const struct timespec start = {0,0};
	struct libbacklight_ctrl *bctl = create_libbacklight(&start, &conf);
	REQUIRE(bctl);

	SECTION("Stable") {
		bool change = false;
		for (int i = 0; i < 100; ++i) {
			if (libbacklight_operate(bctl, &start, 0, 290) == LIBBACKLIGHT_BRIGHTNESS)
				change = true;
		}
		REQUIRE(!change);
		REQUIRE(libbacklight_brightness(bctl) == 5);
	}

	SECTION("Min value") {
		bool change = true;
		for (int i = 0; i < 100; ++i) {
			if (libbacklight_operate(bctl, &start, 0, 42) == LIBBACKLIGHT_BRIGHTNESS)
				change = true;
		}
		REQUIRE(change);
		REQUIRE(libbacklight_brightness(bctl) == 1);
	}

	SECTION("Max value") {
		bool change = true;
		for (int i = 0; i < 100; ++i) {
			if (libbacklight_operate(bctl, &start, 0, 600) == LIBBACKLIGHT_BRIGHTNESS)
				change = true;
		}
		REQUIRE(change);
		REQUIRE(libbacklight_brightness(bctl) == 10);
	}

	destroy_libbacklight(&bctl);
}

TEST_CASE("Test sensor small lux gap") {
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 23;
	conf.initial_brightness_step = 23;
	conf.enable_sensor = 1;
	conf.min_lux = 10;
	conf.max_lux = 200;
	const struct timespec start = {0,0};
	struct libbacklight_ctrl *bctl = create_libbacklight(&start, &conf);
	REQUIRE(bctl);

	SECTION("No backlight max overflow") {
		struct timespec ts {0, 0};
		for (int i = 0; i < 100; ++i) {
			libbacklight_operate(bctl, &ts, 0, 220);
			REQUIRE(libbacklight_brightness(bctl) <= conf.max_brightness_step);
		}
	}

	destroy_libbacklight(&bctl);
}

TEST_CASE("Sensor and Trigger")
{
	struct libbacklight_conf conf;
	memset(&conf, 0, sizeof(conf));
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;
	conf.enable_sensor = 1;
	conf.min_lux = 42;
	conf.max_lux = 600;
	conf.enable_trigger = 1;
	conf.trigger_timeout.tv_sec = 10;
	const struct timespec start = {0,0};
	struct libbacklight_ctrl *bctl = create_libbacklight(&start, &conf);
	REQUIRE(bctl);

	SECTION("Stable") {
		struct timespec ts {0, 0};
		for (int i = 0; i < 100; ++i) {
			REQUIRE(libbacklight_operate(bctl, &ts, 0, 290) == LIBBACKLIGHT_NONE);
			REQUIRE(libbacklight_brightness(bctl) == conf.initial_brightness_step);
		}
	}

	SECTION("Min value") {
		bool change = true;
		for (int i = 0; i < 100; ++i) {
			if (libbacklight_operate(bctl, &start, 0, 42) == LIBBACKLIGHT_BRIGHTNESS)
				change = true;
		}
		REQUIRE(change);
		REQUIRE(libbacklight_brightness(bctl) == 1);
	}

	SECTION("Max value") {
		bool change = true;
		for (int i = 0; i < 100; ++i) {
			if (libbacklight_operate(bctl, &start, 0, 600) == LIBBACKLIGHT_BRIGHTNESS)
				change = true;
		}
		REQUIRE(change);
		REQUIRE(libbacklight_brightness(bctl) == 10);
	}

	SECTION("Timeout and don't increment due to lux, set max after trigger") {
		struct timespec ts = {10,0};
		REQUIRE(libbacklight_operate(bctl, &ts, 0, 290) == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == 0);
		for (int i = 0; i < 100; ++i) {
			REQUIRE(libbacklight_operate(bctl, &ts, 0, 600) == LIBBACKLIGHT_NONE);
			REQUIRE(libbacklight_brightness(bctl) == 0);
		}

		REQUIRE(libbacklight_operate(bctl, &ts, 1, 600) == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(libbacklight_brightness(bctl) == 10);
	}

	destroy_libbacklight(&bctl);
}
