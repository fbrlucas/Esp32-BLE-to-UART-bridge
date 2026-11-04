#include "console.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "version.h"

#if CONFIG_ENABLE_BATTERY_MONITOR
#include "battery.h"
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/param.h>

#define CONSOLE_LINE_SIZE 1024U
#define CONSOLE_USB_POLL_MS 100U
#define CONSOLE_USB_IDLE_POLLS 5U
#define CONSOLE_TASK_STACK_SIZE 3072
#define CONSOLE_TASK_PRIORITY 1

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

size_t console_format_status(char *buf, size_t size) {
  uint32_t millivolts = 0U;

#if CONFIG_ENABLE_BATTERY_MONITOR
  millivolts = battery_get_millivolts();
#endif

  int len;

  if (millivolts > 0U) {
    len = snprintf(buf, size,
                   CONFIG_LOG_LINE_PREFIX
                   "battery %u.%03u V, firmware " FIRMWARE_VERSION "\n",
                   (unsigned)(millivolts / 1000U),
                   (unsigned)(millivolts % 1000U));
  } else {
    len = snprintf(buf, size,
                   CONFIG_LOG_LINE_PREFIX
                   "battery n/a, firmware " FIRMWARE_VERSION "\n");
  }

  return (len > 0) ? MIN((size_t)len, size - 1U) : 0U;
}

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void console_write_status(void) {
  char status[CONSOLE_STATUS_SIZE];
  size_t len = console_format_status(status, sizeof(status));

  flockfile(stdout);

  if (log_line_open || data_line_open) {
    fputc('\n', stdout);
    log_line_open = false;
    data_line_open = false;
  }

  fwrite(status, 1, len, stdout);
  fflush(stdout);

  funlockfile(stdout);
}

/* The C3 USB Serial/JTAG does not expose DTR/RTS. The host polls the CDC data
 * IN endpoint only while a terminal holds the port open, so IN tokens on EP1
 * tell an open port from a closed one. The flag latches, the poll period only
 * sets the detection latency. */
static void console_usb_task(void *pvParameters) {
  (void)pvParameters;

  bool port_open = false;
  unsigned idle_polls = 0U;

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(CONSOLE_USB_POLL_MS));

    if ((usb_serial_jtag_ll_get_intraw_mask() &
         USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1) != 0U) {
      usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);
      idle_polls = 0U;

      if (!port_open) {
        port_open = true;
        console_write_status();
      }
    } else if (port_open && (++idle_polls >= CONSOLE_USB_IDLE_POLLS)) {
      port_open = false;
    }
  }
}
#endif

void console_start_status_monitor(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  /* Drops tokens latched before boot, e.g. by a terminal closed since. */
  usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);

  if (xTaskCreate(console_usb_task, "consTask", CONSOLE_TASK_STACK_SIZE, NULL,
                  CONSOLE_TASK_PRIORITY, NULL) != pdPASS) {
    ESP_LOGE("console", "unable to create USB monitor task");
  }
#endif
}
