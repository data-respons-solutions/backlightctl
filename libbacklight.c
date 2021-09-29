#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "ringbuf.h"
#include "libbacklight.h"

struct libbacklight_ctrl {
	struct libbacklight_conf conf;
	struct timespec last_trigger;	// Last time trigger received
	struct ringbuf *sensor_ring;	// Ringbuffer for sensor readings
	uint64_t sensor_sum;			// Sum of ringbuffer values
	uint32_t brightness_step; 		// Brightness now
};

struct libbacklight_ctrl* create_libbacklight(const struct timespec* ts, const struct libbacklight_conf* conf)
{
	struct libbacklight_ctrl *bctl = (struct libbacklight_ctrl*) malloc(sizeof(struct libbacklight_ctrl));
	if (!bctl)
		goto error_exit;

	memset(bctl, 0, sizeof(struct libbacklight_ctrl));

	if (conf->max_brightness_step == 0)
		goto error_exit;

	if (conf->enable_trigger) {
		if (conf->trigger_timeout.tv_sec == 0 && conf->trigger_timeout.tv_nsec == 0)
			goto error_exit;
		memcpy(&bctl->last_trigger, ts, sizeof(struct timespec));
	}

	if (conf->enable_sensor) {
		bctl->sensor_ring = create_ringbuf(10);
		if (!bctl->sensor_ring)
			goto error_exit;
	}

	memcpy(&bctl->conf, conf, sizeof(struct libbacklight_conf));

	bctl->brightness_step = conf->initial_brightness_step;

	return bctl;

error_exit:
	destroy_libbacklight(&bctl);
	return NULL;
}

void destroy_libbacklight(struct libbacklight_ctrl** bctl)
{
	if (*bctl) {
		if ((*bctl)->sensor_ring)
			destroy_ringbuf(&(*bctl)->sensor_ring);
		free(*bctl);
		*bctl = NULL;
	}
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
		if (ts1->tv_nsec == ts2->tv_nsec)
			return 0;
		return ts1->tv_nsec > ts2->tv_nsec ? 1 : -1;
	}
	return ts1->tv_sec > ts2->tv_sec ? 1 : -1;
}

enum libbacklight_action libbacklight_operate(struct libbacklight_ctrl* bctl, const struct timespec* ts, int triggered, uint32_t lux)
{
	enum libbacklight_action ac = LIBBACKLIGHT_NONE;

	if (bctl->conf.enable_trigger) {
		if (triggered) {
			memcpy(&bctl->last_trigger, ts, sizeof(struct timespec));
			if (bctl->brightness_step == 0) {
				bctl->brightness_step = bctl->conf.initial_brightness_step;
				ac = LIBBACKLIGHT_BRIGHTNESS;
			}
		}
		else
		if (bctl->brightness_step > 0) {
			const struct timespec since_last = timespec_sub(&bctl->last_trigger, ts);
			if (timespec_cmp(&bctl->conf.trigger_timeout, &since_last) <= 0) {
				bctl->brightness_step = 0;
				ac = LIBBACKLIGHT_BRIGHTNESS;
			}
		}
	}

	(void) lux;

	return ac;
}

uint32_t libbacklight_brightness(const struct libbacklight_ctrl* bctl)
{
	return bctl->brightness_step;
}
