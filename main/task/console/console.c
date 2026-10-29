#include "console.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/param.h>

#define CONSOLE_LINE_SIZE 1024U

/* Guarded by the stdout lock. */
static char line_buffer[CONSOLE_LINE_SIZE];
static bool log_line_open;
static bool data_line_open;

static void console_put_log(const char *text, size_t len) {
  for (size_t i = 0U; i < len; i++) {
    if (!log_line_open) {
      fputs(CONFIG_LOG_LINE_PREFIX, stdout);
      log_line_open = true;
    }

    fputc(text[i], stdout);

    if (text[i] == '\n') {
      log_line_open = false;
    }
  }
}

static int console_log_vprintf(const char *format, va_list args) {
  flockfile(stdout);

  int len = vsnprintf(line_buffer, sizeof(line_buffer), format, args);

  if (len > 0) {
    bool truncated = ((size_t)len >= sizeof(line_buffer));

    if (data_line_open) {
      fputc('\n', stdout);
      data_line_open = false;
    }

    console_put_log(line_buffer, MIN((size_t)len, sizeof(line_buffer) - 1U));

    if (truncated && log_line_open) {
      fputc('\n', stdout);
      log_line_open = false;
    }

    fflush(stdout);
  }

  funlockfile(stdout);
  return len;
}

void console_init(void) { esp_log_set_vprintf(console_log_vprintf); }

void console_write_data(const uint8_t *data, size_t len) {
  if ((data == NULL) || (len == 0U)) {
    return;
  }

  flockfile(stdout);

  if (log_line_open) {
    fputc('\n', stdout);
    log_line_open = false;
  }

  fwrite(data, 1, len, stdout);
  fflush(stdout);
  data_line_open = (data[len - 1U] != '\n');

  funlockfile(stdout);
}
