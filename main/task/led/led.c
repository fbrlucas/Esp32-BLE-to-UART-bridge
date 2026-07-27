#include "led.h"
#include "../ble/ble.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "soc/gpio_num.h"
#include <stdint.h>

static blinkParameters_t default_blink_parameter = {CONFIG_STATUS_LED_GPIO, 50,
                                                    50};

blinkParameters_t get_current_ble_state_config(BleState_e *current_state) {
  BleState_e state = (BleState_e)*current_state;
  switch (state) {
  case CONNECTED:
    return (blinkParameters_t){default_blink_parameter.pin, 0, 5000};
  case DISCONNECTED:
    return default_blink_parameter;
  case WAITING:
    return (blinkParameters_t){default_blink_parameter.pin, 500, 500};
  default:
    return default_blink_parameter;
  }
}

void led_task(void *pvParameters) {
  BleState_e *led_state = (BleState_e *)pvParameters;
  blinkParameters_t state = get_current_ble_state_config(led_state);
  uint8_t number_of_connections;
  gpio_reset_pin(state.pin);
  gpio_set_direction(state.pin, GPIO_MODE_OUTPUT_OD);
  int64_t starting_time = esp_timer_get_time();
  int64_t time_difference;
  int64_t current_time;
  while (1) {
    current_time = esp_timer_get_time();
    time_difference = current_time - starting_time;
    if (time_difference >= LED_CONNECTION_UPDATE_TIME) {
      starting_time = current_time;
      number_of_connections = get_number_of_connections();
      if (number_of_connections == 0) {
        *led_state = WAITING;
      } else {
        *led_state = CONNECTED;
      }
    } else {
      state = get_current_ble_state_config(led_state);
      gpio_set_level(state.pin, 1);
      vTaskDelay(pdMS_TO_TICKS(state.time_on_ms));

      gpio_set_level(state.pin, 0);
      vTaskDelay(pdMS_TO_TICKS(state.time_off_ms));
    }
  }
}