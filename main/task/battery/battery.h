#ifndef H_BATTERY_
#define H_BATTERY_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configures ADC1 on the battery sense pin and starts the monitor task.
 *
 * @return true when the monitor is running.
 */
bool battery_init(void);

/** @return true while the last average was below CONFIG_BATTERY_LOW_MV. */
bool battery_is_low(void);

/** @return last averaged battery voltage in mV, 0 before the first sample. */
uint32_t battery_get_millivolts(void);

#ifdef __cplusplus
}
#endif

#endif /* H_BATTERY_ */
