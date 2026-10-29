#include "sdcard.h"

#include "battery.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SDCARD_MOUNT_POINT "/sdcard"
#define SDCARD_MAX_LOG_FILES 1000U
#define SDCARD_STREAM_SIZE 16384U
#define SDCARD_CHUNK_SIZE 4096U
#define SDCARD_STDIO_BUFFER_SIZE 4096U
#define SDCARD_TRIGGER_LEVEL 512U
#define SDCARD_RECEIVE_TIMEOUT_MS 200U
#define SDCARD_ENQUEUE_TIMEOUT_MS 100U
#define SDCARD_FLUSH_PERIOD_US 2000000
#define SDCARD_TIMESTAMP_LAYOUT "00-00 00:00:00"
#define SDCARD_TIMESTAMP_LEN (sizeof(SDCARD_TIMESTAMP_LAYOUT) - 1U)
#define SDCARD_TIMESTAMP_WAIT_US 5000000
#define SDCARD_PATH_SIZE 64U
#define SDCARD_MAX_OPEN_FILES 3
#define SDCARD_ALLOCATION_UNIT_SIZE (16U * 1024U)
#define SDCARD_TASK_STACK_SIZE 4096
#define SDCARD_TASK_PRIORITY 9

static const char *TAG = "sdcard";

static sdmmc_card_t *card;
static spi_host_device_t spi_host;
static FILE *log_file;
static StreamBufferHandle_t stream;
static StaticStreamBuffer_t stream_control;
static uint8_t stream_storage[SDCARD_STREAM_SIZE + 1U];
static uint8_t write_chunk[SDCARD_CHUNK_SIZE];
static char stdio_buffer[SDCARD_STDIO_BUFFER_SIZE];
static size_t pending_len;
static int64_t pending_since;
static volatile bool card_present;

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

static void sdcard_stop_logging(void) {
  card_present = false;

  if (log_file != NULL) {
    size_t len;

    while ((len = xStreamBufferReceive(stream, write_chunk,
                                       sizeof(write_chunk), 0)) > 0U) {
      if (fwrite(write_chunk, 1, len, log_file) != len) {
        break;
      }
    }
  }

  sdcard_abort_logging();
  ESP_LOGI(TAG, "card unmounted, safe to power off");
}

static bool sdcard_is_digit(uint8_t c) { return (c >= '0') && (c <= '9'); }

static unsigned sdcard_two_digits(const uint8_t *p) {
  return ((unsigned)(p[0] - '0') * 10U) + (unsigned)(p[1] - '0');
}

/* Matches "DD-MM HH:MM:SS" and emits "DD-MM_HH-MM-SS": ':' is invalid in FAT
 * file names. */
static bool sdcard_find_timestamp(const uint8_t *data, size_t len, char *stamp,
                                  size_t stamp_size) {
  static const char layout[] = SDCARD_TIMESTAMP_LAYOUT;

  for (size_t i = 0U; (i + SDCARD_TIMESTAMP_LEN) <= len; i++) {
    const uint8_t *p = &data[i];
    size_t k = 0U;

    if ((i > 0U) && sdcard_is_digit(data[i - 1U])) {
      continue;
    }

    while ((k < SDCARD_TIMESTAMP_LEN) &&
           ((layout[k] == '0') ? sdcard_is_digit(p[k])
                               : (p[k] == (uint8_t)layout[k]))) {
      k++;
    }

    if (k < SDCARD_TIMESTAMP_LEN) {
      continue;
    }

    unsigned day = sdcard_two_digits(&p[0]);
    unsigned month = sdcard_two_digits(&p[3]);
    unsigned hour = sdcard_two_digits(&p[6]);
    unsigned minute = sdcard_two_digits(&p[9]);
    unsigned second = sdcard_two_digits(&p[12]);

    if ((day < 1U) || (day > 31U) || (month < 1U) || (month > 12U) ||
        (hour > 23U) || (minute > 59U) || (second > 59U)) {
      continue;
    }

    snprintf(stamp, stamp_size, "%02u-%02u_%02u-%02u-%02u", day, month, hour,
             minute, second);
    return true;
  }

  return false;
}

static bool sdcard_open_log_file(const char *stamp) {
  char path[SDCARD_PATH_SIZE];

  for (unsigned index = 0U; index < SDCARD_MAX_LOG_FILES; index++) {
    struct stat entry;

    if (stamp == NULL) {
      snprintf(path, sizeof(path), SDCARD_MOUNT_POINT "/LOG%03u.TXT", index);
    } else if (index == 0U) {
      snprintf(path, sizeof(path), SDCARD_MOUNT_POINT "/%s.TXT", stamp);
    } else {
      snprintf(path, sizeof(path), SDCARD_MOUNT_POINT "/%s_%u.TXT", stamp,
               index);
    }

    if (stat(path, &entry) == 0) {
      continue;
    }

    log_file = fopen(path, "w");

    if (log_file == NULL) {
      ESP_LOGE(TAG, "unable to create %s", path);
      return false;
    }

    setvbuf(log_file, stdio_buffer, _IOFBF, sizeof(stdio_buffer));
    ESP_LOGI(TAG, "logging UART0 data to %s", path);
    return true;
  }

  ESP_LOGE(TAG, "no free log file slot (limit %u)", SDCARD_MAX_LOG_FILES);
  return false;
}

static bool sdcard_open_on_timestamp(void) {
  size_t len = xStreamBufferReceive(stream, &write_chunk[pending_len],
                                    sizeof(write_chunk) - pending_len,
                                    pdMS_TO_TICKS(SDCARD_RECEIVE_TIMEOUT_MS));

  if ((len > 0U) && (pending_len == 0U)) {
    pending_since = esp_timer_get_time();
  }

  pending_len += len;

  if (pending_len == 0U) {
    return true;
  }

  char stamp[32];
  bool found =
      sdcard_find_timestamp(write_chunk, pending_len, stamp, sizeof(stamp));

  if (!found && (pending_len < sizeof(write_chunk)) &&
      ((esp_timer_get_time() - pending_since) < SDCARD_TIMESTAMP_WAIT_US)) {
    return true;
  }

  if (!found) {
    ESP_LOGW(TAG, "no timestamp in the first %u bytes, using indexed name",
             (unsigned)pending_len);
  }

  if (!sdcard_open_log_file(found ? stamp : NULL)) {
    return false;
  }

  if (fwrite(write_chunk, 1, pending_len, log_file) != pending_len) {
    ESP_LOGE(TAG, "write failed, stopping SD logging");
    return false;
  }

  pending_len = 0U;
  return true;
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

    if (log_file == NULL) {
      if (!sdcard_open_on_timestamp()) {
        sdcard_abort_logging();
        break;
      }
      continue;
    }

    size_t len =
        xStreamBufferReceive(stream, write_chunk, sizeof(write_chunk),
                             pdMS_TO_TICKS(SDCARD_RECEIVE_TIMEOUT_MS));

    if (len > 0U) {
      if (fwrite(write_chunk, 1, len, log_file) != len) {
        ESP_LOGE(TAG, "write failed, stopping SD logging");
        sdcard_abort_logging();
        break;
      }
    }

    int64_t now = esp_timer_get_time();

    if ((now - last_flush) >= SDCARD_FLUSH_PERIOD_US) {
      if (fflush(log_file) != 0) {
        ESP_LOGE(TAG, "flush failed, stopping SD logging");
        sdcard_abort_logging();
        break;
      }
      fsync(fileno(log_file));
      last_flush = now;
    }
  }

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
  card_present = true;

  if (xTaskCreate(sdcard_writer_task, "sdTask", SDCARD_TASK_STACK_SIZE, NULL,
                  SDCARD_TASK_PRIORITY, NULL) != pdPASS) {
    ESP_LOGE(TAG, "unable to create writer task");
    sdcard_abort_logging();
    return false;
  }

  return true;
}

bool sdcard_is_present(void) { return card_present; }

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
