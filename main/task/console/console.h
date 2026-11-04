#ifndef H_CONSOLE_
#define H_CONSOLE_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONSOLE_STATUS_SIZE 96U

/**
 * Routes ESP_LOG output through a writer that starts every line with
 * CONFIG_LOG_LINE_PREFIX. Call it first in app_main so later logs are tagged.
 */
void console_init(void);

/**
 * Writes bytes received from the target to the USB console without a prefix.
 * Shares the stdout lock with the log writer, so a log line never lands in
 * the middle of a burst and always starts on its own line.
 */
void console_write_data(const uint8_t *data, size_t len);

/**
 * Formats the status line sent to every BLE client that subscribes and
 * written at the top of every new SD log file:
 * "<CONFIG_LOG_LINE_PREFIX>battery X.XXX V, firmware X.Y.Z\n".
 *
 * @param buf  Destination, CONSOLE_STATUS_SIZE bytes fit any prefix up to
 *             about 50 characters.
 * @param size Size of buf, must not be 0.
 * @return Length written, without the terminating NUL.
 */
size_t console_format_status(char *buf, size_t size);

/**
 * Starts a low priority task that writes the status line to the USB console
 * every time a terminal opens the port. Call it after battery_init() so the
 * first line already carries a reading.
 */
void console_start_status_monitor(void);

#ifdef __cplusplus
}
#endif

#endif /* H_CONSOLE_ */
