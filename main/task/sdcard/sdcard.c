#include "sdcard.h"

#include "battery.h"
#include "console.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SDCARD_MOUNT_POINT "/sdcard"
#define SDCARD_MAX_LOG_FILES 1000U
#define SDCARD_STREAM_SIZE 16384U
#define SDCARD_CHUNK_SIZE 4096U
#define SDCARD_STDIO_BUFFER_SIZE 4096U
#define SDCARD_LINE_SIZE 256U
#define SDCARD_TRIGGER_LEVEL 512U
#define SDCARD_RECEIVE_TIMEOUT_MS 200U
#define SDCARD_ENQUEUE_TIMEOUT_MS 100U
#define SDCARD_FLUSH_PERIOD_US 30000000
#define SDCARD_TIMESTAMP_LAYOUT "00-00 00:00:00"
#define SDCARD_RTC_SET_LAYOUT "0000/00/00 00:00:00"
#define SDCARD_NAME_SIZE 32U
#define SDCARD_PATH_SIZE 64U
#define SDCARD_MAX_OPEN_FILES 3
#define SDCARD_ALLOCATION_UNIT_SIZE (16U * 1024U)
#define SDCARD_TASK_STACK_SIZE 4096
#define SDCARD_TASK_PRIORITY 9

typedef struct {
  unsigned day;
  unsigned month;
  unsigned hour;
  unsigned minute;
  unsigned second;
} sdcard_stamp_t;

static const char *TAG = "sdcard";

static sdmmc_card_t *card;
static spi_host_device_t spi_host;
static FILE *log_file;
static StreamBufferHandle_t stream;
static StaticStreamBuffer_t stream_control;
static uint8_t stream_storage[SDCARD_STREAM_SIZE + 1U];
static SemaphoreHandle_t writer_done;
static StaticSemaphore_t writer_done_control;
static uint8_t write_chunk[SDCARD_CHUNK_SIZE];
static char stdio_buffer[SDCARD_STDIO_BUFFER_SIZE];
static char line[SDCARD_LINE_SIZE];
static size_t line_len;
static char log_path[SDCARD_PATH_SIZE];
static uint32_t first_stamp_s;
static uint32_t last_stamp_s;
static bool stamp_seen;
static bool name_final;
static volatile bool card_present;

static bool sdcard_handle_line(void);

static bool sdcard_battery_low(void) {
#if CONFIG_ENABLE_BATTERY_MONITOR
  return battery_is_low();
#else
  return false;
#endif
}

static void sdcard_abort_logging(void) {
  card_present = false;

  if (log_file != NULL) {
    if (fclose(log_file) != 0) {
      ESP_LOGE(TAG, "close failed, buffered data may be lost");
    }
    log_file = NULL;
  }

  esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, card);
  card = NULL;
  spi_bus_free(spi_host);
}

static bool sdcard_flush_line(void) {
  if (line_len == 0U) {
    return true;
  }

  line[line_len] = '\0';
  bool ok = sdcard_handle_line();
  line_len = 0U;
  return ok;
}

static bool sdcard_feed(const uint8_t *data, size_t len) {
  for (size_t i = 0U; i < len; i++) {
    line[line_len++] = (char)data[i];

    if ((data[i] == (uint8_t)'\n') || (line_len == (sizeof(line) - 1U))) {
      if (!sdcard_flush_line()) {
        return false;
      }
    }
  }

  return true;
}

static void sdcard_stop_logging(void) {
  card_present = false;

  size_t len;
  bool ok = true;

  while (ok && ((len = xStreamBufferReceive(stream, write_chunk,
                                            sizeof(write_chunk), 0)) > 0U)) {
    ok = sdcard_feed(write_chunk, len);
  }

  if (ok) {
    (void)sdcard_flush_line();
  }

  sdcard_abort_logging();
  ESP_LOGI(TAG, "card unmounted, safe to power off");
}

/* Printable ASCII plus TAB, LF, CR and ESC. The RX line noise seen while the
 * device powers up or down is mostly 0x00 and 0xFF. */
static bool sdcard_is_text(uint8_t c) {
  return ((c >= 0x20U) && (c <= 0x7EU)) || (c == (uint8_t)'\t') ||
         (c == (uint8_t)'\n') || (c == (uint8_t)'\r') || (c == 0x1BU);
}

static bool sdcard_line_is_text(void) {
  for (size_t i = 0U; i < line_len; i++) {
    if (!sdcard_is_text((uint8_t)line[i])) {
      return false;
    }
  }

  return true;
}

/* The line may hold NUL bytes, so the search is bounded by line_len. */
static const char *sdcard_line_find(const char *pattern) {
  size_t len = strlen(pattern);

  for (size_t i = 0U; (i + len) <= line_len; i++) {
    if (memcmp(&line[i], pattern, len) == 0) {
      return &line[i];
    }
  }

  return NULL;
}

static void sdcard_line_skip(size_t count) {
  line_len -= count;
  memmove(line, &line[count], line_len + 1U);
}

static bool sdcard_is_digit(char c) { return (c >= '0') && (c <= '9'); }

static unsigned sdcard_two_digits(const char *p) {
  return ((unsigned)(p[0] - '0') * 10U) + (unsigned)(p[1] - '0');
}

/* '0' in the layout matches any digit, other characters match literally. The
 * text must be NUL terminated. */
static bool sdcard_match_layout(const char *p, const char *layout) {
  for (size_t k = 0U; layout[k] != '\0'; k++) {
    if (!((layout[k] == '0') ? sdcard_is_digit(p[k]) : (p[k] == layout[k]))) {
      return false;
    }
  }

  return true;
}

static bool sdcard_stamp_in_range(const sdcard_stamp_t *stamp) {
  return (stamp->day >= 1U) && (stamp->day <= 31U) && (stamp->month >= 1U) &&
         (stamp->month <= 12U) && (stamp->hour <= 23U) &&
         (stamp->minute <= 59U) && (stamp->second <= 59U);
}

static bool sdcard_stamp_is_unset(const sdcard_stamp_t *stamp) {
  return (stamp->day == 1U) && (stamp->month == 1U) &&
         (((stamp->hour * 60U) + stamp->minute) <
          (unsigned)CONFIG_SDCARD_RTC_UNSET_WINDOW_MIN);
}

/* Seconds since 01-01 00:00:00 of a non leap year, where the device clock
 * starts while its RTC is unset. */
static uint32_t sdcard_stamp_seconds(const sdcard_stamp_t *stamp) {
  static const uint16_t days_before[12] = {0U,   31U,  59U,  90U,
                                           120U, 151U, 181U, 212U,
                                           243U, 273U, 304U, 334U};
  uint32_t days = (uint32_t)days_before[stamp->month - 1U] + stamp->day - 1U;

  return (days * 86400U) + (stamp->hour * 3600U) + (stamp->minute * 60U) +
         stamp->second;
}

/* Matches the device's "MM-DD HH:MM:SS" not preceded by a digit. */
static bool sdcard_find_timestamp(sdcard_stamp_t *stamp) {
  for (size_t i = 0U; i < line_len; i++) {
    const char *p = &line[i];

    if (((i > 0U) && sdcard_is_digit(p[-1])) ||
        !sdcard_match_layout(p, SDCARD_TIMESTAMP_LAYOUT)) {
      continue;
    }

    sdcard_stamp_t found = {
        .day = sdcard_two_digits(&p[3]),
        .month = sdcard_two_digits(&p[0]),
        .hour = sdcard_two_digits(&p[6]),
        .minute = sdcard_two_digits(&p[9]),
        .second = sdcard_two_digits(&p[12]),
    };

    if (sdcard_stamp_in_range(&found)) {
      *stamp = found;
      return true;
    }
  }

  return false;
}

/* Matches CONFIG_SDCARD_RTC_SET_MARKER followed by "YYYY/MM/DD HH:MM:SS". */
static bool sdcard_find_rtc_set(time_t *set_time) {
  const char *p = sdcard_line_find(CONFIG_SDCARD_RTC_SET_MARKER);

  if (p == NULL) {
    return false;
  }

  p += sizeof(CONFIG_SDCARD_RTC_SET_MARKER) - 1U;

  if (!sdcard_match_layout(p, SDCARD_RTC_SET_LAYOUT)) {
    return false;
  }

  sdcard_stamp_t found = {
      .day = sdcard_two_digits(&p[8]),
      .month = sdcard_two_digits(&p[5]),
      .hour = sdcard_two_digits(&p[11]),
      .minute = sdcard_two_digits(&p[14]),
      .second = sdcard_two_digits(&p[17]),
  };

  if (!sdcard_stamp_in_range(&found)) {
    return false;
  }

  struct tm fields = {
      .tm_year = (int)((sdcard_two_digits(&p[0]) * 100U) +
                       sdcard_two_digits(&p[2])) -
                 1900,
      .tm_mon = (int)found.month - 1,
      .tm_mday = (int)found.day,
      .tm_hour = (int)found.hour,
      .tm_min = (int)found.minute,
      .tm_sec = (int)found.second,
      .tm_isdst = 0,
  };

  *set_time = mktime(&fields);
  return *set_time != (time_t)-1;
}

static void sdcard_stamp_from_time(time_t t, sdcard_stamp_t *stamp) {
  struct tm fields;

  localtime_r(&t, &fields);
  stamp->day = (unsigned)fields.tm_mday;
  stamp->month = (unsigned)fields.tm_mon + 1U;
  stamp->hour = (unsigned)fields.tm_hour;
  stamp->minute = (unsigned)fields.tm_min;
  stamp->second = (unsigned)fields.tm_sec;
}

/* "DD-MM_HH-MM-SS": ':' is invalid in FAT file names. */
static void sdcard_format_name(const sdcard_stamp_t *stamp, char *name,
                               size_t name_size) {
  snprintf(name, name_size, "%02u-%02u_%02u-%02u-%02u", stamp->day,
           stamp->month, stamp->hour, stamp->minute, stamp->second);
}

static bool sdcard_free_path(const char *name, char *path, size_t path_size) {
  for (unsigned index = 0U; index < SDCARD_MAX_LOG_FILES; index++) {
    struct stat entry;

    if (name == NULL) {
      snprintf(path, path_size, SDCARD_MOUNT_POINT "/LOG%03u.TXT", index);
    } else if (index == 0U) {
      snprintf(path, path_size, SDCARD_MOUNT_POINT "/%s.TXT", name);
    } else {
      snprintf(path, path_size, SDCARD_MOUNT_POINT "/%s_%u.TXT", name, index);
    }

    if (stat(path, &entry) != 0) {
      return true;
    }
  }

  ESP_LOGE(TAG, "no free log file slot (limit %u)", SDCARD_MAX_LOG_FILES);
  return false;
}

static bool sdcard_fopen(const char *mode) {
  log_file = fopen(log_path, mode);

  if (log_file == NULL) {
    ESP_LOGE(TAG, "unable to open %s", log_path);
    return false;
  }

  setvbuf(log_file, stdio_buffer, _IOFBF, sizeof(stdio_buffer));
  return true;
}

static bool sdcard_fwrite(const char *data, size_t len) {
  if (fwrite(data, 1, len, log_file) != len) {
    ESP_LOGE(TAG, "write failed, stopping SD logging");
    return false;
  }

  return true;
}

/* A NULL name opens a provisional LOGnnn.TXT, renamed once the RTC is known.
 * The file starts with the logger status line. */
static bool sdcard_open_session(const char *name) {
  char status[CONSOLE_STATUS_SIZE];

  if (!sdcard_free_path(name, log_path, sizeof(log_path)) ||
      !sdcard_fopen("w") ||
      !sdcard_fwrite(status, console_format_status(status, sizeof(status)))) {
    return false;
  }

  stamp_seen = false;
  name_final = (name != NULL);
  ESP_LOGI(TAG, "logging UART0 data to %s", log_path);
  return true;
}

static void sdcard_close_session(void) {
  if (fclose(log_file) != 0) {
    ESP_LOGE(TAG, "close failed, buffered data may be lost");
  }

  log_file = NULL;
  ESP_LOGI(TAG, "device reboot, closed %s", log_path);
}

/* FATFS cannot rename an open file. */
static bool sdcard_rename_session(const char *name) {
  char path[SDCARD_PATH_SIZE];
  int ret = fclose(log_file);

  log_file = NULL;
  name_final = true;

  if (ret != 0) {
    ESP_LOGE(TAG, "close failed, stopping SD logging");
    return false;
  }

  if (sdcard_free_path(name, path, sizeof(path)) &&
      (rename(log_path, path) == 0)) {
    ESP_LOGI(TAG, "renamed %s to %s", log_path, path);
    memcpy(log_path, path, sizeof(log_path));
  } else {
    ESP_LOGW(TAG, "unable to rename %s", log_path);
  }

  return sdcard_fopen("a");
}

/* Only a printable ASCII line holding the boot marker or a timestamp opens a
 * file. What comes before it, RX noise while the device powers up included,
 * has no file to go to. */
static bool sdcard_try_open(bool is_boot) {
  sdcard_stamp_t stamp;
  char name[SDCARD_NAME_SIZE];
  size_t skip = 0U;

  if (is_boot) {
    skip = (size_t)(sdcard_line_find(CONFIG_SDCARD_BOOT_MARKER) - line);
  } else {
    while ((skip < line_len) && !sdcard_is_text((uint8_t)line[skip])) {
      skip++;
    }
  }

  sdcard_line_skip(skip);

  bool has_stamp = sdcard_find_timestamp(&stamp);

  if ((!is_boot && !has_stamp) || !sdcard_line_is_text()) {
    skip += line_len;
    line_len = 0U;
  }

  if (skip > 0U) {
    ESP_LOGW(TAG, "dropped %u bytes before the first device message",
             (unsigned)skip);
  }

  if (line_len == 0U) {
    return true;
  }

  bool stamp_valid = has_stamp && !sdcard_stamp_is_unset(&stamp);

  if (stamp_valid) {
    sdcard_format_name(&stamp, name, sizeof(name));
  }

  return sdcard_open_session(stamp_valid ? name : NULL);
}

/* Every file holds one device boot and is named after the real time of its
 * first line. While the device RTC is unset that time is unknown, so the file
 * stays LOGnnn.TXT until RTC SET arrives and the start time is derived from
 * the device clock elapsed until then. Once a file is open every byte is
 * written as received. */
static bool sdcard_handle_line(void) {
  const char *marker = sdcard_line_find(CONFIG_SDCARD_BOOT_MARKER);
  bool is_boot = (marker != NULL);

  if (is_boot && (log_file != NULL)) {
    size_t head = (size_t)(marker - line);

    /* What precedes the marker ends the previous boot. */
    if (!sdcard_fwrite(line, head)) {
      return false;
    }

    sdcard_close_session();
    sdcard_line_skip(head);
  }

  if ((log_file == NULL) && !sdcard_try_open(is_boot)) {
    return false;
  }

  if (log_file == NULL) {
    return true;
  }

  sdcard_stamp_t stamp = {0};
  bool has_stamp = sdcard_find_timestamp(&stamp);
  bool stamp_valid = has_stamp && !sdcard_stamp_is_unset(&stamp);
  uint32_t stamp_s = has_stamp ? sdcard_stamp_seconds(&stamp) : 0U;

  if (!name_final) {
    const sdcard_stamp_t *start = (stamp_valid && !stamp_seen) ? &stamp : NULL;
    sdcard_stamp_t derived;
    time_t set_time;

    if (sdcard_find_rtc_set(&set_time)) {
      /* The device may print this line with its old clock or the new one. */
      uint32_t set_s = (has_stamp && !stamp_valid) ? stamp_s : last_stamp_s;
      time_t elapsed = (stamp_seen && (set_s > first_stamp_s))
                           ? (time_t)(set_s - first_stamp_s)
                           : 0;

      sdcard_stamp_from_time(set_time - elapsed, &derived);
      start = &derived;
    }

    if (start != NULL) {
      char name[SDCARD_NAME_SIZE];

      sdcard_format_name(start, name, sizeof(name));

      if (!sdcard_rename_session(name)) {
        return false;
      }
    }
  }

  if (has_stamp) {
    if (!stamp_seen) {
      first_stamp_s = stamp_s;
    }

    last_stamp_s = stamp_s;
    stamp_seen = true;
  }

  return sdcard_fwrite(line, line_len);
}

static void sdcard_writer_task(void *pvParameters) {
  (void)pvParameters;

  int64_t last_flush = esp_timer_get_time();

  while (card_present) {
    if (sdcard_battery_low()) {
      ESP_LOGW(TAG, "battery low, closing log file");
      sdcard_stop_logging();
      break;
    }

    size_t len =
        xStreamBufferReceive(stream, write_chunk, sizeof(write_chunk),
                             pdMS_TO_TICKS(SDCARD_RECEIVE_TIMEOUT_MS));

    /* An idle stream releases a partial line, e.g. a prompt without '\n'. */
    if (!((len > 0U) ? sdcard_feed(write_chunk, len) : sdcard_flush_line())) {
      sdcard_abort_logging();
      break;
    }

    int64_t now = esp_timer_get_time();

    if ((log_file != NULL) && ((now - last_flush) >= SDCARD_FLUSH_PERIOD_US)) {
      if (fflush(log_file) != 0) {
        ESP_LOGE(TAG, "flush failed, stopping SD logging");
        sdcard_abort_logging();
        break;
      }
      fsync(fileno(log_file));
      last_flush = now;
    }
  }

  xSemaphoreGive(writer_done);
  vTaskDelete(NULL);
}

bool sdcard_init(void) {
  if (sdcard_battery_low()) {
    ESP_LOGW(TAG, "battery low, SD logging disabled");
    return false;
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = SDCARD_MAX_OPEN_FILES,
      .allocation_unit_size = SDCARD_ALLOCATION_UNIT_SIZE,
  };

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = CONFIG_SDCARD_SPI_FREQ_KHZ;

  spi_bus_config_t bus_config = {
      .mosi_io_num = CONFIG_SDCARD_PIN_MOSI,
      .miso_io_num = CONFIG_SDCARD_PIN_MISO,
      .sclk_io_num = CONFIG_SDCARD_PIN_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = SDCARD_CHUNK_SIZE,
  };

  esp_err_t ret = spi_bus_initialize(host.slot, &bus_config, SDSPI_DEFAULT_DMA);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
    return false;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = CONFIG_SDCARD_PIN_CS;
  slot_config.host_id = host.slot;

  ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot_config,
                                &mount_config, &card);

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "no SD card mounted (%s), running as BLE bridge",
             esp_err_to_name(ret));
    spi_bus_free(host.slot);
    return false;
  }

  spi_host = (spi_host_device_t)host.slot;

  ESP_LOGI(TAG, "card %s, %llu MB, %u kHz", card->cid.name,
           ((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024),
           card->max_freq_khz);

  stream = xStreamBufferCreateStatic(sizeof(stream_storage) - 1U,
                                     SDCARD_TRIGGER_LEVEL, stream_storage,
                                     &stream_control);
  writer_done = xSemaphoreCreateBinaryStatic(&writer_done_control);
  card_present = true;

  if (xTaskCreate(sdcard_writer_task, "sdTask", SDCARD_TASK_STACK_SIZE, NULL,
                  SDCARD_TASK_PRIORITY, NULL) != pdPASS) {
    ESP_LOGE(TAG, "unable to create writer task");
    sdcard_abort_logging();
    xSemaphoreGive(writer_done);
    return false;
  }

  return true;
}

bool sdcard_is_present(void) { return card_present; }

bool sdcard_wait_unmounted(uint32_t timeout_ms) {
  if (writer_done == NULL) {
    return true;
  }

  if (xSemaphoreTake(writer_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return false;
  }

  /* Keeps later calls returning at once. */
  xSemaphoreGive(writer_done);
  return true;
}

bool sdcard_write(const uint8_t *data, size_t len) {
  if (!card_present || data == NULL || len == 0U) {
    return false;
  }

  size_t sent = xStreamBufferSend(stream, data, len,
                                  pdMS_TO_TICKS(SDCARD_ENQUEUE_TIMEOUT_MS));

  if (sent != len) {
    ESP_LOGW(TAG, "dropped %u bytes, SD card cannot keep up",
             (unsigned)(len - sent));
    return false;
  }

  return true;
}
