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
 * Each device boot, delimited by CONFIG_SDCARD_BOOT_MARKER, goes to its own
 * file named DD-MM_HH-MM-SS.TXT (DD-MM_HH-MM-SS_N.TXT when taken) after the
 * real time of its first line. The device prints "TAG HH:MM:SS " at the start
 * of its lines and "---- YYYY-MM-DD ----" at boot, at midnight and when its
 * RTC is set:
 *  - first time dated and valid: used right away;
 *  - first time within CONFIG_SDCARD_RTC_UNSET_WINDOW_MIN of 01-01 00:00:00:
 *    the file is LOGnnn.TXT until the first CONFIG_SDCARD_RTC_SET_MARKER
 *    "YYYY/MM/DD HH:MM:SS";
 *  - first time without a date, the logger having started after the device
 *    booted: the file is LOGnnn.TXT until the first dated time or RTC SET.
 * The provisional file is then renamed to that time minus the device clock
 * elapsed since its first time.
 * A file is only opened by a printable ASCII line holding the boot marker, a
 * date or a time, so RX noise with the device off creates none. Once open,
 * every byte is written as received. When the battery monitor reports low
 * battery, the writer task flushes and closes the file and unmounts the card.
 * Must be called once, after battery_init() and before the UART tasks are
 * started.
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

/**
 * Waits for the writer task to close the log file and unmount the card, which
 * it does on its own once the battery monitor reports low battery.
 *
 * @return true when no card is mounted, also when none ever was.
 */
bool sdcard_wait_unmounted(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* H_SDCARD_ */
