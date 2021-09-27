#include <cstring>
#include <cstdint>
#include <cerrno>
#include <ctime>

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

// bitwise operators for enum class flags
template <typename Enumeration, typename = typename std::enable_if<std::is_enum<Enumeration>::value, Enumeration>::type>
Enumeration operator&(Enumeration a, Enumeration b)
{return static_cast<Enumeration>(static_cast<typename std::underlying_type<Enumeration>::type>(a) & static_cast<typename std::underlying_type<Enumeration>::type>(b));}
template <typename Enumeration, typename = typename std::enable_if<std::is_enum<Enumeration>::value, Enumeration>::type>
Enumeration operator|(Enumeration a, Enumeration b)
{return static_cast<Enumeration>(static_cast<typename std::underlying_type<Enumeration>::type>(a) | static_cast<typename std::underlying_type<Enumeration>::type>(b));}
template <typename Enumeration, typename = typename std::enable_if<std::is_enum<Enumeration>::value, Enumeration>::type>
Enumeration& operator&=(Enumeration& a, Enumeration b)
{return a = a & b;}
template <typename Enumeration, typename = typename std::enable_if<std::is_enum<Enumeration>::value, Enumeration>::type>
Enumeration& operator|=(Enumeration& a, Enumeration b)
{return a = a | b;}
template <typename Enumeration, typename = typename std::enable_if<std::is_enum<Enumeration>::value, Enumeration>::type>
Enumeration& operator~(Enumeration a)
{return ~static_cast<Enumeration>(static_cast<typename std::underlying_type<Enumeration>::type>(a));}

struct libbacklight_conf {
	uint32_t max_brightness_step;
	uint32_t initial_brightness_step;
	int enable_trigger;
	struct timespec trigger_timeout;
};

struct libbacklight_ctrl {
	struct libbacklight_conf conf;
	uint32_t brightness_step; // Brightness now
	struct timespec last_trigger;
};

int libbacklight_init(struct libbacklight_ctrl* ctrl, const struct timespec* ts, const struct libbacklight_conf* conf)
{
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
	LIBBACKLIGHT_NONE =			0,
	LIBBACKLIGHT_BRIGHTNESS = 	1 << 0,
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
	printf("ts1: %d: ts2: %d\n", ts1->tv_sec, ts2->tv_sec);
	return ts1->tv_sec > ts2->tv_sec ? 1 : -1;
}

enum libbacklight_action libbacklight_operate(struct libbacklight_ctrl* ctrl, const struct timespec* ts, int triggered, uint32_t lux)
{
	enum libbacklight_action ac = LIBBACKLIGHT_NONE;

	if (ctrl->conf.enable_trigger && ctrl->brightness_step > 0) {
		const struct timespec since_last = timespec_sub(&ctrl->last_trigger, ts);
		if (timespec_cmp(&ctrl->conf.trigger_timeout, &since_last) <= 0) {
			ac |= LIBBACKLIGHT_BRIGHTNESS;
			ctrl->brightness_step = 0;
		}
	}
	(void) ts;
	(void) triggered;
	(void) lux;
	(void) ctrl;

	return ac;
}

TEST_CASE("libbacklight_init simple") {

	struct libbacklight_conf conf;
	conf.max_brightness_step = 10;
	conf.initial_brightness_step = 5;

	struct libbacklight_ctrl ctrl;
	struct timespec ts;
	int r = libbacklight_init(&ctrl, &ts, &conf);
	REQUIRE(r == 0);
}

TEST_CASE("Test trigger") {
	SECTION("Timeout") {
		struct libbacklight_conf conf;
		conf.max_brightness_step = 10;
		conf.initial_brightness_step = 5;
		conf.enable_trigger = 1;
		conf.trigger_timeout.tv_sec = 10;
		struct libbacklight_ctrl ctrl;
		struct timespec ts;
		int r = libbacklight_init(&ctrl, &ts, &conf);
		REQUIRE(r == 0);

		enum libbacklight_action ac = LIBBACKLIGHT_NONE;
		for (int i = 0; i < 9; ++i) {
			ts.tv_sec = i;
			ac = libbacklight_operate(&ctrl, &ts, 0, 0);
			REQUIRE(ac == LIBBACKLIGHT_NONE);
		}
		ts.tv_sec = 10;
		ac = libbacklight_operate(&ctrl, &ts, 0, 0);
		REQUIRE(ac == LIBBACKLIGHT_BRIGHTNESS);
		REQUIRE(ctrl.brightness_step == 0);

		// Section -> Test re-enable backlight
	}
}

