set(srcs "main.c")

if(NOT "${IDF_TARGET}" STREQUAL "esp32c3")
    message(FATAL_ERROR "This project only supports the ESP32-C3 (IDF_TARGET is '${IDF_TARGET}')")
endif()

idf_component_register(SRCS "${srcs}" "task/led/led.c" "task/ble/ble.c"
                              "task/sdcard/sdcard.c" "task/battery/battery.c"
                              "task/console/console.c"
                       INCLUDE_DIRS "." "task/led" "task/ble" "task/sdcard"
                                    "task/battery" "task/console")
