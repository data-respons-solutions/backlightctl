#ifndef LIBBACKLIGHT__H__
#define LIBBACKLIGHT__H__

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct libbacklight_conf {
	uint32_t max_brightness_step;		// Total number of steps available.
	uint32_t initial_brightness_step;	// Step we're starting from. Value between 1 and max_brightness_step.
	int enable_sensor;					// Calculate brightness based on min/max_lux.
	uint32_t min_lux;					// This value corresponds to brightness step 1.
	uint32_t max_lux;					// This value corresponds to max_brightness_step.
	int enable_trigger;					// Enable backlight after trigger received.
										// Will set backlight to initial_brightness_step,
										// unless enable_sensor is set, then the value is adjusted
										// based on sensor input.
	struct timespec trigger_timeout;	// Time without any trigger until backlight is turned off, step 0.
};

struct libbacklight_ctrl;

struct libbacklight_ctrl* create_libbacklight(const struct timespec* ts, const struct libbacklight_conf* conf);
void destroy_libbacklight(struct libbacklight_ctrl** bctl);

/* Operate on state machine.
 * If trigger is disabled, argument trigger is ignored.
 * If sensor is disabled, argument lux is ignored.
 *
 * Returns what action callier is expected to take.
*/

enum libbacklight_action {
	LIBBACKLIGHT_NONE,
	LIBBACKLIGHT_BRIGHTNESS, // Adjust brightness to value returned by libbacklight_brightness()
};

enum libbacklight_action libbacklight_operate(struct libbacklight_ctrl* bctl, const struct timespec* ts, int triggered, uint32_t lux);

/* Return current brightness step
 */
uint32_t libbacklight_brightness(const struct libbacklight_ctrl* bctl);

/* Return current configuration
 */
const struct libbacklight_conf* libbacklight_get_conf(const struct libbacklight_ctrl* bctl);

#ifdef __cplusplus
}
#endif

#endif /* LIBBACKLIGHT__H__ */
