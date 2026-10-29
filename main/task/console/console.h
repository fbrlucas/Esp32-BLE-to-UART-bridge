#ifndef H_CONSOLE_
#define H_CONSOLE_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* H_CONSOLE_ */
