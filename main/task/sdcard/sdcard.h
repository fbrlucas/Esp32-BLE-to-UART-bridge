#ifndef H_SDCARD_
#define H_SDCARD_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialises the SPI bus, mounts the SD card and opens a new log file.
 * Must be called once, before the UART tasks are started.
 *
 * @return true when a card was mounted and logging is active, false when no
 *         card is present (the application must keep the BLE bridge flow).
 */
bool sdcard_init(void);

/** @return true while UART data must be redirected to the SD card. */
bool sdcard_is_present(void);

/**
 * Queues data for the writer task. Never blocks longer than
 * SDCARD_ENQUEUE_TIMEOUT_MS so the UART task cannot stall on card latency.
 *
 * @return true when every byte was queued.
 */
bool sdcard_write(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* H_SDCARD_ */
