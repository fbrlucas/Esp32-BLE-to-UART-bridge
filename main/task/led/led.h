#ifndef H_LED_
#define H_LED_

#define LED_LOW_BATTERY_LIT_MS 200
#define LED_LOW_BATTERY_DARK_MS 1800
#define LED_IDLE_POLL_MS 500

/* Open-drain against an active-low LED: pulling the pin down lights it. */
#define LED_ON_LEVEL 0
#define LED_OFF_LEVEL 1

void led_task(void *pvParameters);

#endif /* H_LED_ */
