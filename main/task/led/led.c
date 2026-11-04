#include "led.h"
#include "../battery/battery.h"
#include "driver/gpio.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

#include <stdbool.h>

static volatile bool boot_blink_pending = true;

void led_task(void *pvParameters) {
  (void)pvParameters;

  gpio_reset_pin(CONFIG_STATUS_LED_GPIO);
  gpio_set_direction(CONFIG_STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(CONFIG_STATUS_LED_GPIO, LED_ON_LEVEL);
  gpio_hold_dis(CONFIG_STATUS_LED_GPIO);

  vTaskDelay(pdMS_TO_TICKS(LED_BOOT_LIT_MS));
  gpio_set_level(CONFIG_STATUS_LED_GPIO, LED_OFF_LEVEL);
  boot_blink_pending = false;

  while (1) {
    if (battery_is_low()) {
      gpio_set_level(CONFIG_STATUS_LED_GPIO, LED_ON_LEVEL);
      vTaskDelay(pdMS_TO_TICKS(LED_LOW_BATTERY_LIT_MS));

      gpio_set_level(CONFIG_STATUS_LED_GPIO, LED_OFF_LEVEL);
      vTaskDelay(pdMS_TO_TICKS(LED_LOW_BATTERY_DARK_MS));
    } else {
      gpio_set_level(CONFIG_STATUS_LED_GPIO, LED_OFF_LEVEL);
      vTaskDelay(pdMS_TO_TICKS(LED_IDLE_POLL_MS));
    }
  }
}

void led_hold_off(void) {
  /* A low battery wake up sleeps again right after boot, which would cut the
   * boot blink short. */
  for (unsigned waited = 0U;
       boot_blink_pending && (waited < (2U * LED_BOOT_LIT_MS));
       waited += LED_WAIT_STEP_MS) {
    vTaskDelay(pdMS_TO_TICKS(LED_WAIT_STEP_MS));
  }

  gpio_set_level(CONFIG_STATUS_LED_GPIO, LED_OFF_LEVEL);
  gpio_hold_en(CONFIG_STATUS_LED_GPIO);
  gpio_deep_sleep_hold_en();
}
