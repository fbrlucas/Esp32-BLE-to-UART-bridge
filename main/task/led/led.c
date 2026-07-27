#include "led.h"
#include "../battery/battery.h"
#include "driver/gpio.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

void led_task(void *pvParameters) {
  (void)pvParameters;

  gpio_reset_pin(CONFIG_STATUS_LED_GPIO);
  gpio_set_direction(CONFIG_STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(CONFIG_STATUS_LED_GPIO, LED_OFF_LEVEL);

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
