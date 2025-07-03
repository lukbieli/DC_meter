#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <freertos/queue.h>

#include "CurrentDrv.h"
#include "ina219.h"
#include <driver/gpio.h>
#include <esp_timer.h>

#define I2C_PORT 0
#define I2C_ADDR CONFIG_EXAMPLE_I2C_ADDR

#define TEST_PIN GPIO_NUM_4

static const char *TAG = "CurrentDrv";



static QueueHandle_t ina219_queue = NULL;

static ina219_t dev1;
static ina219_t dev2;
static ina219_t dev3;

static TaskHandle_t task_ina219_handle = NULL;

static esp_timer_handle_t periodic_timer;
static uint32_t timer_period_us = 32000; // Default timer period in microseconds

static CurrentDrv_Config_t currentDrv_config = {
    .ch1 = {
        .voltage_enabled = true,
        .current_enabled = true,
        .power_enabled = true
    },
    .ch2 = {
        .voltage_enabled = true,
        .current_enabled = true,
        .power_enabled = true
    },
    .ch3 = {
        .voltage_enabled = true,
        .current_enabled = true,
        .power_enabled = true
    }
};

static void read_ina219_data(ina219_t *dev, ina219_data_raw_t *data, CurrentDrv_ChannelCfg_t* ch_cfg);
static void ina_init(ina219_t* dev, uint8_t i2c_addr);
void IRAM_ATTR timer_notify_task_ina219(void* arg);

void CurrentDrv_Init(void)
{
    ESP_LOGI(TAG, "Initializing Current Driver...");


}

void CurrentDrv_Task(void *pvParameters)
{
    //test pin output
    gpio_set_direction(TEST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(TEST_PIN, 0);

    //create queue
    ina219_queue = xQueueCreate(10, sizeof(ina219_full_data_t));
    if(ina219_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue");
        return;
    }

    ina_init(&dev1, 0x41);
    ina_init(&dev2, 0x40);
    ina_init(&dev3, 0x44);

    // lcd_init(); //commented out for now. In future led display should used separately I2C port

    // Create periodic esp_timer
    const esp_timer_create_args_t timer_args = {
        .callback = &timer_notify_task_ina219,
        .arg = NULL,
        .name = "ina219_timer"
    };
    
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 32 * 1000)); // 500ms

    ESP_LOGI(TAG, "Starting the loop");
    task_ina219_handle = xTaskGetCurrentTaskHandle();

    static ina219_full_data_t data = {0};
    while (1)
    {
        // Wait for notification from timer
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        gpio_set_level(TEST_PIN, 1);
        read_ina219_data(&dev1, &data.ch1, &currentDrv_config.ch1);
        gpio_set_level(TEST_PIN, 0);
        read_ina219_data(&dev2, &data.ch2, &currentDrv_config.ch2);
        gpio_set_level(TEST_PIN, 1);
        read_ina219_data(&dev3, &data.ch3, &currentDrv_config.ch3);
        gpio_set_level(TEST_PIN, 0);
        /* Using float in printf() requires non-default configuration in
         * sdkconfig. See sdkconfig.defaults.esp32 and
         * sdkconfig.defaults.esp8266  */
        // printf("VBUS: %.04f V, VSHUNT: %.04f mV, IBUS: %.04f mA, PBUS: %.04f mW\n",
        //         data.bus_voltage, data.shunt_voltage * 1000, data.current * 1000, data.power * 1000);
        // add to queue
        if (xQueueSend(ina219_queue, &data, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send data to queue");
        }

        // gpio_set_level(TEST_PIN, 1);
        // vTaskDelay(pdMS_TO_TICKS(500));
    }
}

QueueHandle_t* CurrentDrv_GetQueue(void)
{
    return &ina219_queue;
}

esp_timer_handle_t* CurrentDrv_GetPeriodicTimer(void)
{
    return &periodic_timer;
}

void CurrentDrv_SetChannelConfig(CurrentDrv_Config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid channel configuration");
        return;
    }
    memcpy(&currentDrv_config, config, sizeof(CurrentDrv_Config_t));
    ESP_LOGI(TAG, "Channel configuration updated");
}

const CurrentDrv_Config_t* const CurrentDrv_GetChannelConfig(void)
{
    return (const CurrentDrv_Config_t* const)&currentDrv_config;
}

void CurrentDrv_SetTimerPeriod(uint32_t period_us)
{
    if (period_us == 0) {
        ESP_LOGE(TAG, "Invalid timer period");
        return;
    }
    timer_period_us = period_us;
    ESP_ERROR_CHECK(esp_timer_stop(periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, timer_period_us)); // Convert ms to us
    ESP_LOGI(TAG, "Timer period set to %lu us", timer_period_us);
}

uint32_t CurrentDrv_GetTimerPeriod(void)
{
    return timer_period_us; // Convert us to ms
}

void CurrentDrv_StopTimer(void)
{
    ESP_ERROR_CHECK(esp_timer_stop(periodic_timer));
    ESP_LOGI(TAG, "Timer stopped");
}

void CurrentDrv_StartTimer(void)
{
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, timer_period_us)); // Convert ms to us
    ESP_LOGI(TAG, "Timer started with period %lu ms", CurrentDrv_GetTimerPeriod());
}

// Timer callback to notify task_ina219
void IRAM_ATTR timer_notify_task_ina219(void* arg) {
    if (task_ina219_handle) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(task_ina219_handle, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void ina_init(ina219_t* dev, uint8_t i2c_addr)
{
    memset(dev, 0, sizeof(ina219_t));

    assert(CONFIG_EXAMPLE_SHUNT_RESISTOR_MILLI_OHM > 0);
    ESP_ERROR_CHECK(ina219_init_desc(dev, i2c_addr, I2C_PORT, CONFIG_EXAMPLE_I2C_MASTER_SDA, CONFIG_EXAMPLE_I2C_MASTER_SCL));
    ESP_LOGI(TAG, "Initializing INA219");
    ESP_ERROR_CHECK(ina219_init(dev));

    ESP_LOGI(TAG, "Configuring INA219");
    ESP_ERROR_CHECK(ina219_configure(dev, INA219_BUS_RANGE_16V, INA219_GAIN_0_125,
            INA219_RES_12BIT_1S, INA219_RES_12BIT_1S, INA219_MODE_CONT_SHUNT_BUS));

    ESP_LOGI(TAG, "Calibrating INA219");

    ESP_ERROR_CHECK(ina219_calibrate(dev, (float)CONFIG_EXAMPLE_SHUNT_RESISTOR_MILLI_OHM / 1000.0f));  
}

static void read_ina219_data(ina219_t *dev, ina219_data_raw_t *data, CurrentDrv_ChannelCfg_t* ch_cfg)
{
    ina219_data_t raw_data = {0};
    // ina219_get_data(dev, &raw_data);

    if(dev == NULL || data == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for read_ina219_data");
        return;
    }

    if(ch_cfg->voltage_enabled == true)
    {
        if (ina219_get_bus_voltage(dev, &raw_data.bus_voltage) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read bus voltage");
            return;
        }
        if (ina219_get_shunt_voltage(dev, &raw_data.shunt_voltage) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read shunt voltage");
            return;
        }

        data->voltage = raw_data.bus_voltage + (raw_data.shunt_voltage / 1000.0f); // Convert shunt voltage to V and add to bus voltage
    } 

    if(ch_cfg->current_enabled == true)
    {
        if (ina219_get_current(dev, &raw_data.current) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read current");
            return;
        }
        data->current = raw_data.current;
    }

    if(ch_cfg->power_enabled == true)
    {
        if (ina219_get_power(dev, &raw_data.power) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read power");
            return;
        }
        data->power = raw_data.power;
    }

    // data->voltage += 0.1;
    // data->current += 0.1;
    // data->power += 0.1;

    // //limit values
    // if (data->voltage > 16.0f) {
    //     data->voltage = 0.0f;
    // }
    // if (data->current > 40.0f) {
    //     data->current = 0.0f;
    // }
    // if (data->power > 100.0f) {
    //     data->power = 0.0f;
    // }
}
