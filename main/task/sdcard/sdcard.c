#include "sdcard.h"

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
#define SDCARD_MAX_OPEN_FILES 3
#define SDCARD_ALLOCATION_UNIT_SIZE (16U * 1024U)
#define SDCARD_TASK_STACK_SIZE 4096
#define SDCARD_TASK_PRIORITY 9

static const char *TAG = "sdcard";

static sdmmc_card_t *card;
static FILE *log_file;
static StreamBufferHandle_t stream;
static StaticStreamBuffer_t stream_control;
static uint8_t stream_storage[SDCARD_STREAM_SIZE + 1U];
static uint8_t write_chunk[SDCARD_CHUNK_SIZE];
static char stdio_buffer[SDCARD_STDIO_BUFFER_SIZE];
static volatile bool card_present;

static void sdcard_abort_logging(void) {
  card_present = false;

  if (log_file != NULL) {
    fclose(log_file);
    log_file = NULL;
  }
}

static void sdcard_writer_task(void *pvParameters) {
  (void)pvParameters;

  int64_t last_flush = esp_timer_get_time();

  while (card_present) {
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

static bool sdcard_open_log_file(void) {
  char path[32];

  for (unsigned index = 0U; index < SDCARD_MAX_LOG_FILES; index++) {
    struct stat entry;

    snprintf(path, sizeof(path), SDCARD_MOUNT_POINT "/LOG%03u.TXT", index);

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

bool sdcard_init(void) {
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

  ESP_LOGI(TAG, "card %s, %llu MB, %u kHz", card->cid.name,
           ((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024),
           card->max_freq_khz);

  if (!sdcard_open_log_file()) {
    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, card);
    card = NULL;
    spi_bus_free(host.slot);
    return false;
  }

  stream = xStreamBufferCreateStatic(sizeof(stream_storage) - 1U,
                                     SDCARD_TRIGGER_LEVEL, stream_storage,
                                     &stream_control);
  card_present = true;

  if (xTaskCreate(sdcard_writer_task, "sdTask", SDCARD_TASK_STACK_SIZE, NULL,
                  SDCARD_TASK_PRIORITY, NULL) != pdPASS) {
    ESP_LOGE(TAG, "unable to create writer task");
    sdcard_abort_logging();
    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, card);
    card = NULL;
    spi_bus_free(host.slot);
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
