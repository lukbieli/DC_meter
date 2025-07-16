#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <ina219.h>
#include <string.h>
#include <esp_log.h>
#include <assert.h>

#include <driver/gpio.h>
#include <driver/uart.h>

#include <esp_timer.h>

#include "CommM.h"
#include "CurrentDrv.h"
#include "DisplayM.h"


const static char *TAG = "DC_Meter";


void app_main()
{


    ESP_ERROR_CHECK(i2cdev_init());
    xTaskCreate(CurrentDrv_Task, "CurrentDrv_Task", configMINIMAL_STACK_SIZE * 8, NULL, 5, NULL);

    xTaskCreate(CommM_Task, "CommM_Task", configMINIMAL_STACK_SIZE * 8, NULL, 4, NULL);

    DisplayM_Init();
}
