#ifndef H_BLESPPSERVER_
#define H_BLESPPSERVER_
#endif


#include "driver/gpio.h"
#include <assert.h>
#include "esp_log.h"
#include "esp_peripheral.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "hal/gpio_types.h"
#include "hal/uart_types.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "nvs_flash.h"
#include "task/battery/battery.h"
#include "task/ble/ble.h"
#include "task/led/led.h"
#include "task/sdcard/sdcard.h"
/* BLE */
#include "driver/uart.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "soc/gpio_num.h"
#include "esp_bt.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "esp_assert.h"  
#include "sys/param.h"  
#include <stdbool.h>
#include <stdint.h>
#include "freertos/idf_additions.h"
#include "hal/uart_types.h"
#include "nimble/ble.h"
#include "soc/gpio_num.h"
#include "sdkconfig.h"
#include "esp_bt.h"

#ifdef __cplusplus
extern "C" {
#endif
#define UART_IDLE_TIMEOUT_US 5000

/* 16 Bit SPP Service UUID */
#define BLE_SVC_SPP_UUID16_SECOND                           0xABE0
/* 16 Bit SPP Service Characteristic UUID */

#define BLE_SVC_SPP_CHR_UUID16_WRITE_UART_0                              0xABE1
#define BLE_SVC_SPP_CHR_UUID16_READ_UART_0                               0xABE2

#define BLE_DEFAULT_MTU                                                  20
#define BLE_PREFERRED_MTU                                                517
#define BLE_HEADER_SIZE                                                  3

#define BLE_ADVERTISING_NAME                                                 "UART-to-BLE-Bridge"


#define BLE_ADVERTISING_POWER                                           ESP_PWR_LVL_P3  //3dm
#define BLE_DEFAULT_POWER                                                ESP_PWR_LVL_P3 //3dbm

struct ble_hs_cfg;
struct ble_gatt_register_ctxt;

struct uart_connection_attributes
{
    QueueHandle_t Uart_queue_handle; 
    uart_port_t Uart_port;
    gpio_num_t rx;
    gpio_num_t tx;


} typedef UartConnection ;


