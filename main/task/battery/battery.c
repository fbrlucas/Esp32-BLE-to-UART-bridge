#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "sdkconfig.h"

#if CONFIG_ENABLE_SDCARD
#include "sdcard.h"
#endif

#define BATTERY_ADC_UNIT ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_2 /* GPIO2 on the ESP32-C3 */
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT

#define BATTERY_UNCAL_FULL_SCALE_MV 2800
#define BATTERY_UNCAL_FULL_SCALE_RAW 4095

#define BATTERY_TASK_STACK_SIZE 3072
#define BATTERY_TASK_PRIORITY 3
#define BATTERY_SD_CLOSE_TIMEOUT_MS 10000U

_Static_assert(CONFIG_BATTERY_RESUME_MV > CONFIG_BATTERY_LOW_MV,
               "resume threshold must be above the low threshold");

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t cali_handle;
static volatile bool low_battery;
static volatile uint32_t battery_mv;
static uint32_t low_threshold_mv = CONFIG_BATTERY_LOW_MV;

static bool battery_calibration_init(void) {
  adc_cali_curve_fitting_config_t cali_config = {
      .unit_id = BATTERY_ADC_UNIT,
      .chan = BATTERY_ADC_CHANNEL,
      .atten = BATTERY_ADC_ATTEN,
      .bitwidth = BATTERY_ADC_BITWIDTH,
  };

  esp_err_t ret =
      adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle);

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "no eFuse calibration (%s), using nominal full scale",
             esp_err_to_name(ret));
    cali_handle = NULL;
    return false;
  }

  return true;
}

static uint32_t battery_read_average_mv(void) {
  uint32_t sum = 0;
  unsigned taken = 0;

  for (unsigned i = 0U; i < CONFIG_BATTERY_SAMPLES; i++) {
    int raw = 0;

    if (adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
      continue;
    }

    int millivolts = 0;

    if (cali_handle != NULL) {
      if (adc_cali_raw_to_voltage(cali_handle, raw, &millivolts) != ESP_OK) {
        continue;
      }
    } else {
      millivolts = (raw * BATTERY_UNCAL_FULL_SCALE_MV) /
                   BATTERY_UNCAL_FULL_SCALE_RAW;
    }

    sum += (uint32_t)millivolts;
    taken++;
  }

  if (taken == 0U) {
    return 0U;
  }

  return (sum / taken) * CONFIG_BATTERY_DIVIDER_RATIO;
}

static void battery_update(void) {
  uint32_t millivolts = battery_read_average_mv();

  if (millivolts > 0U) {
    bool was_low = low_battery;

    battery_mv = millivolts;
    low_battery = (millivolts < low_threshold_mv);

    if (low_battery != was_low) {
      ESP_LOGW(TAG, "battery %s: %u.%03u V", low_battery ? "low" : "recovered",
               (unsigned)(millivolts / 1000U), (unsigned)(millivolts % 1000U));
    } else {
      ESP_LOGI(TAG, "battery %u.%03u V", (unsigned)(millivolts / 1000U),
               (unsigned)(millivolts % 1000U));
    }
  } else {
    ESP_LOGE(TAG, "no valid ADC sample");
  }
}

/* Wakes up every CONFIG_BATTERY_WAKE_PERIOD_S to check whether the pack was
 * recharged, otherwise only a reset brings the firmware back. */
static void battery_deep_sleep(void) {
  ESP_LOGW(TAG, "entering deep sleep");

#if CONFIG_BATTERY_WAKE_PERIOD_S > 0
  esp_sleep_enable_timer_wakeup((uint64_t)CONFIG_BATTERY_WAKE_PERIOD_S *
                                1000000ULL);
#endif

  led_hold_off();
  esp_deep_sleep_start();
}

static void battery_task(void *pvParameters) {
  (void)pvParameters;

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(CONFIG_BATTERY_PERIOD_MS));
    battery_update();

    if (low_battery) {
#if CONFIG_ENABLE_SDCARD
      if (!sdcard_wait_unmounted(BATTERY_SD_CLOSE_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "SD card still mounted, sleeping anyway");
      }
#endif
      battery_deep_sleep();
    }
  }
}

bool battery_init(void) {
  adc_oneshot_unit_init_cfg_t unit_config = {
      .unit_id = BATTERY_ADC_UNIT,
  };

  esp_err_t ret = adc_oneshot_new_unit(&unit_config, &adc_handle);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
    return false;
  }

  adc_oneshot_chan_cfg_t channel_config = {
      .atten = BATTERY_ADC_ATTEN,
      .bitwidth = BATTERY_ADC_BITWIDTH,
  };

  ret = adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL,
                                   &channel_config);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s",
             esp_err_to_name(ret));
    adc_oneshot_del_unit(adc_handle);
    adc_handle = NULL;
    return false;
  }

  battery_calibration_init();

  /* The pack recovers some voltage once unloaded, so after a low battery
   * sleep only a recharge, not that rebound, resumes logging. */
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    low_threshold_mv = CONFIG_BATTERY_RESUME_MV;
  }

  battery_update();
  low_threshold_mv = CONFIG_BATTERY_LOW_MV;

  if (low_battery) {
    battery_deep_sleep();
  }

  /* A low reading at boot is already reported by battery_update(). */
  if ((battery_mv > 0U) && !low_battery) {
    ESP_LOGW(TAG, "battery at boot: %u.%03u V", (unsigned)(battery_mv / 1000U),
             (unsigned)(battery_mv % 1000U));
  }

  if (xTaskCreate(battery_task, "battTask", BATTERY_TASK_STACK_SIZE, NULL,
                  BATTERY_TASK_PRIORITY, NULL) != pdPASS) {
    ESP_LOGE(TAG, "unable to create monitor task");
    adc_oneshot_del_unit(adc_handle);
    adc_handle = NULL;
    return false;
  }

  return true;
}

bool battery_is_low(void) { return low_battery; }

uint32_t battery_get_millivolts(void) { return battery_mv; }
