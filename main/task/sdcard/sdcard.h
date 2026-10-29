#ifndef H_SDCARD_
#define H_SDCARD_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialises the SPI bus, mounts the SD card and starts the writer task.
 * The log file is created on the first "DD-MM HH:MM:SS" timestamp seen in the
 * UART stream and named DD-MM_HH-MM-SS.TXT (DD-MM_HH-MM-SS_N.TXT when taken).
 * Without a timestamp in the first SDCARD_CHUNK_SIZE bytes or
 * SDCARD_TIMESTAMP_WAIT_US, it falls back to LOGnnn.TXT. When the battery
 * monitor reports low battery, the writer task flushes and closes the file and
 * unmounts the card. Must be called once, after battery_init() and before the
 * UART tasks are started.
 *
 * @return true when a card was mounted and logging is active, false when no
 *         card is present or the battery is already low (the application must
 *         keep the BLE bridge flow).
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
