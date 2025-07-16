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
#define AVG_SAMPLE_COUNT 10 // Number of samples for averaging

typedef struct ChannelInstance {
    ina219_t dev;
    CurrentDrv_ChannelCfg_t config;
    ina219_data_raw_t data; // Raw data read from INA219
    ina219_data_raw_t avg_data; // Average data for smoothing
    ina219_data_raw_t hist_data[AVG_SAMPLE_COUNT]; // Historical data for smoothing
    uint32_t sample_count; // Number of samples taken for averaging
} ChannelInstance_t;

static const char *TAG = "CurrentDrv";

static bool auto_config_enabled = true; // Flag to enable/disable auto-configuration

static QueueHandle_t ina219_queue = NULL;

static ChannelInstance_t channel_instances[3] = {
    {
        .config = {
            .voltage_enabled = true,
            .current_enabled = true,
            .power_enabled = true,
            .gain = INA219_GAIN_0_125, // Default gain setting
            .bus_voltage_range = INA219_BUS_RANGE_32V, // Default bus voltage range
            .bus_voltage_resolution = INA219_RES_12BIT_1S, // Default bus voltage resolution
            .shunt_voltage_resolution = INA219_RES_12BIT_1S, // Default shunt voltage resolution
            .i2c_addr = 0x41 // Default I2C address for channel 1
        },
        .data = {0},
        .avg_data = {0},
        .sample_count = 0
    },
    {
        .config = {
            .voltage_enabled = true,
            .current_enabled = true,
            .power_enabled = true,
            .gain = INA219_GAIN_0_125, // Default gain setting
            .bus_voltage_range = INA219_BUS_RANGE_32V, // Default bus voltage range
            .bus_voltage_resolution = INA219_RES_12BIT_1S, // Default bus voltage resolution
            .shunt_voltage_resolution = INA219_RES_12BIT_1S, // Default shunt voltage resolution
            .i2c_addr = 0x40 // Default I2C address for channel 2
        },
        .data = {0},
        .avg_data = {0},
        .sample_count = 0
    },
    {
        .config = {
            .voltage_enabled = true,
            .current_enabled = true,
            .power_enabled = true,
            .gain = INA219_GAIN_0_125, // Default gain setting
            .bus_voltage_range = INA219_BUS_RANGE_32V, // Default bus voltage range
            .bus_voltage_resolution = INA219_RES_12BIT_1S, // Default bus voltage resolution
            .shunt_voltage_resolution = INA219_RES_12BIT_1S, // Default shunt voltage resolution
            .i2c_addr = 0x44 // Default I2C address for channel 3
        },
        .data = {0},
        .avg_data = {0},
        .sample_count = 0
    }
};

// static ina219_t dev1;
// static ina219_t dev2;
// static ina219_t dev3;

static TaskHandle_t task_ina219_handle = NULL;

static esp_timer_handle_t periodic_timer;
static uint32_t timer_period_us = 32000; // Default timer period in microseconds

// static CurrentDrv_Config_t currentDrv_config = {
//     .ch1 = {
//         .voltage_enabled = true,
//         .current_enabled = true,
//         .power_enabled = true,
//         .gain = INA219_GAIN_0_125, // Default gain setting
//         .bus_voltage_range = INA219_BUS_RANGE_32V, // Default bus voltage range
//         .bus_voltage_resolution = INA219_RES_12BIT_1S, // Default bus voltage resolution
//         .shunt_voltage_resolution = INA219_RES_12BIT_1S, // Default shunt voltage resolution
//         .i2c_addr = 0x41 // Default I2C address for channel 1
//     },
//     .ch2 = {
//         .voltage_enabled = true,
//         .current_enabled = true,
//         .power_enabled = true,
//         .gain = INA219_GAIN_0_125, // Default gain setting
//         .bus_voltage_range = INA219_BUS_RANGE_32V, // Default bus voltage range
//         .bus_voltage_resolution = INA219_RES_12BIT_1S, // Default bus voltage resolution
//         .shunt_voltage_resolution = INA219_RES_12BIT_1S, // Default shunt voltage resolution
//         .i2c_addr = 0x40 // Default I2C address for channel 2
//     },
//     .ch3 = {
//         .voltage_enabled = true,
//         .current_enabled = true,
//         .power_enabled = true,
//         .gain = INA219_GAIN_0_125, // Default gain setting
//         .bus_voltage_range = INA219_BUS_RANGE_32V, // Default bus voltage range
//         .bus_voltage_resolution = INA219_RES_12BIT_1S, // Default bus voltage resolution
//         .shunt_voltage_resolution = INA219_RES_12BIT_1S, // Default shunt voltage resolution
//         .i2c_addr = 0x44 // Default I2C address for channel 3
//     }
// };

static void read_ina219_data(ChannelInstance_t* channel, ina219_data_raw_t *data);
static void ina_init(ChannelInstance_t* channel);
void IRAM_ATTR timer_notify_task_ina219(void* arg);
static void autoconfig_ina219(ChannelInstance_t* channel, ina219_data_raw_t *data);
static void calculate_avg(ChannelInstance_t* channel, ina219_data_raw_t *data);
static void update_channel_config(ChannelInstance_t* channel, CurrentDrv_ChannelCfg_t *new_cfg);


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

    ina_init(&channel_instances[0]);
    ina_init(&channel_instances[1]);
    ina_init(&channel_instances[2]);

    // lcd_init(); //commented out for now. In future led display should used separately I2C port

    // Create periodic esp_timer
    const esp_timer_create_args_t timer_args = {
        .callback = &timer_notify_task_ina219,
        .arg = NULL,
        .name = "ina219_timer"
    };
    
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, timer_period_us)); // 32 ms

    ESP_LOGI(TAG, "Starting the loop");
    task_ina219_handle = xTaskGetCurrentTaskHandle();

    static ina219_full_data_t data = {0};
    while (1)
    {
        // Wait for notification from timer
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        gpio_set_level(TEST_PIN, 1);
        read_ina219_data(&channel_instances[0], &data.ch1);
        gpio_set_level(TEST_PIN, 0);
        read_ina219_data(&channel_instances[1], &data.ch2);
        gpio_set_level(TEST_PIN, 1);
        read_ina219_data(&channel_instances[2], &data.ch3);
        gpio_set_level(TEST_PIN, 0);

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
        ESP_LOGE(TAG, "Invalid channel configuration pointer");
        return;
    }
    update_channel_config(&channel_instances[0], &config->ch1);
    update_channel_config(&channel_instances[1], &config->ch2);
    update_channel_config(&channel_instances[2], &config->ch3);
    ESP_LOGI(TAG, "Channel configuration updated");
}

void CurrentDrv_GetChannelConfig(CurrentDrv_Config_t* config)
{
    if(config == NULL) {
        ESP_LOGE(TAG, "Invalid channel configuration pointer");
        return;
    }
    memcpy((void*)&config->ch1, (void*)&channel_instances[0].config, sizeof(CurrentDrv_ChannelCfg_t));
    memcpy((void*)&config->ch2, (void*)&channel_instances[1].config, sizeof(CurrentDrv_ChannelCfg_t));
    memcpy((void*)&config->ch3, (void*)&channel_instances[2].config, sizeof(CurrentDrv_ChannelCfg_t));
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

void CurrentDrv_StartTimer(bool auto_cfg_en)
{
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, timer_period_us)); // Convert ms to us
    ESP_LOGI(TAG, "Timer started with period %lu ms", CurrentDrv_GetTimerPeriod());
    auto_config_enabled = auto_cfg_en; // Set the flag for auto-configuration
}

void CurrentDrv_GetAvgData(ina219_full_data_t *data)
{
    if (data == NULL) {
        ESP_LOGE(TAG, "Invalid data pointer for average data");
        return;
    }

    // Copy the average data from each channel instance
    memcpy(&data->ch1, &channel_instances[0].avg_data, sizeof(ina219_data_raw_t));
    memcpy(&data->ch2, &channel_instances[1].avg_data, sizeof(ina219_data_raw_t));
    memcpy(&data->ch3, &channel_instances[2].avg_data, sizeof(ina219_data_raw_t));
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

static void ina_init(ChannelInstance_t* channel)
{
    memset(&channel->dev, 0, sizeof(ina219_t));

    assert(CONFIG_EXAMPLE_SHUNT_RESISTOR_MILLI_OHM > 0);
    ESP_ERROR_CHECK(ina219_init_desc(&channel->dev, channel->config.i2c_addr, I2C_PORT, CONFIG_EXAMPLE_I2C_MASTER_SDA, CONFIG_EXAMPLE_I2C_MASTER_SCL));
    ESP_LOGI(TAG, "Initializing INA219");
    ESP_ERROR_CHECK(ina219_init(&channel->dev));

    ESP_LOGI(TAG, "Configuring INA219");
    ESP_ERROR_CHECK(ina219_configure(&channel->dev, channel->config.bus_voltage_range, channel->config.gain,
            channel->config.bus_voltage_resolution, channel->config.shunt_voltage_resolution, INA219_MODE_CONT_SHUNT_BUS));

    ESP_LOGI(TAG, "Calibrating INA219");

    ESP_ERROR_CHECK(ina219_calibrate(&channel->dev, (float)CONFIG_EXAMPLE_SHUNT_RESISTOR_MILLI_OHM / 1000.0f));  
}

static void read_ina219_data(ChannelInstance_t* channel, ina219_data_raw_t *data)
{
    ina219_data_t raw_data = {0};
    // ina219_get_data(dev, &raw_data);

    if(channel == NULL || data == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for read_ina219_data");
        return;
    }

    if(channel->config.voltage_enabled == true)
    {
        if (ina219_get_bus_voltage(&channel->dev, &raw_data.bus_voltage) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read bus voltage");
            return;
        }
        if (ina219_get_shunt_voltage(&channel->dev, &raw_data.shunt_voltage) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read shunt voltage");
            return;
        }

        data->voltage = raw_data.bus_voltage + raw_data.shunt_voltage;
        data->shunt_voltage = raw_data.shunt_voltage;
    } 

    if(channel->config.current_enabled == true)
    {
        if (ina219_get_current(&channel->dev, &raw_data.current) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read current");
            return;
        }
        data->current = raw_data.current;
    }

    if(channel->config.power_enabled == true)
    {
        if (ina219_get_power(&channel->dev, &raw_data.power) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read power");
            return;
        }
        data->power = raw_data.power;
    }

    //calculate average
    calculate_avg(channel, data);

    //autoconfigure based on data
    if(auto_config_enabled == true)
    {
        autoconfig_ina219(channel, data);
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

static void autoconfig_ina219(ChannelInstance_t* channel, ina219_data_raw_t *data)
{
    //depends on voltage and current and if enabled change range
    if (channel == NULL || data == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for autoconfig_ina219");
        return;
    }
    bool changed = false;

    if (channel->config.voltage_enabled) {
        if (channel->avg_data.voltage > 16.0f ) {
            if(channel->config.bus_voltage_range != INA219_BUS_RANGE_32V) {
                ESP_LOGI(TAG, "Changing bus voltage range to 32V");
                channel->config.bus_voltage_range = INA219_BUS_RANGE_32V;
                changed = true;
            }
        } else {
            if(channel->config.bus_voltage_range != INA219_BUS_RANGE_16V) {
                ESP_LOGI(TAG, "Changing bus voltage range to 16V");
                channel->config.bus_voltage_range = INA219_BUS_RANGE_16V;
                changed = true;
            }
        }
    }

    if (channel->config.current_enabled) {
        if (channel->avg_data.current > 1.6f) {
            if(channel->config.gain != INA219_GAIN_0_125) {
                ESP_LOGI(TAG, "Current over 0.4A, changing gain to 0.125");
                channel->config.gain = INA219_GAIN_0_125;
                changed = true;
            }
        } else if (channel->avg_data.current > 0.8f) {
            if(channel->config.gain != INA219_GAIN_0_25) {
                ESP_LOGI(TAG, "Current over 0.4A, changing gain to 0.25");
                channel->config.gain = INA219_GAIN_0_25;
                changed = true;
            }
        } else if (channel->avg_data.current > 0.4f) {
            if(channel->config.gain != INA219_GAIN_0_5) {
                ESP_LOGI(TAG, "Current over 0.4A, changing gain to 0.5");
                channel->config.gain = INA219_GAIN_0_5;
                changed = true;
            }
        } else {
            if(channel->config.gain != INA219_GAIN_1) {
                ESP_LOGI(TAG, "Changing gain to 1");
                channel->config.gain = INA219_GAIN_1;
                changed = true;
            }
        }
    }

    if(changed) {
        // Reinitialize INA219 with the new configuration

        ESP_ERROR_CHECK(ina219_configure(&channel->dev, channel->config.bus_voltage_range, channel->config.gain,
            channel->config.bus_voltage_resolution, channel->config.shunt_voltage_resolution, INA219_MODE_CONT_SHUNT_BUS));
        ESP_LOGI(TAG, "INA219 reinitialized with new configuration");
    }
}

static void calculate_avg(ChannelInstance_t* channel, ina219_data_raw_t *data)
{
    if (channel == NULL || data == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for calculate_avg");
        return;
    }

    // Shift historical data
    for (int i = AVG_SAMPLE_COUNT - 1; i > 0; i--) {
        channel->hist_data[i] = channel->hist_data[i - 1];
    }

    // Add new data to history
    channel->hist_data[0] = *data;

    // Calculate average
    channel->avg_data.voltage = 0.0f;
    channel->avg_data.current = 0.0f;
    channel->avg_data.power = 0.0f;
    channel->avg_data.shunt_voltage = 0.0f;

    for (int i = 0; i < AVG_SAMPLE_COUNT; i++) {
        channel->avg_data.voltage += channel->hist_data[i].voltage;
        channel->avg_data.current += channel->hist_data[i].current;
        channel->avg_data.power += channel->hist_data[i].power;
        channel->avg_data.shunt_voltage += channel->hist_data[i].shunt_voltage;
    }

    // Divide by number of samples
    channel->avg_data.voltage /= AVG_SAMPLE_COUNT;
    channel->avg_data.current /= AVG_SAMPLE_COUNT;
    channel->avg_data.power /= AVG_SAMPLE_COUNT;
    channel->avg_data.shunt_voltage /= AVG_SAMPLE_COUNT;

}

static void update_channel_config(ChannelInstance_t* channel, CurrentDrv_ChannelCfg_t *new_cfg)
{
    if (channel == NULL || new_cfg == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for update_channel_config");
        return;
    }

    // Update channel configuration
    channel->config.voltage_enabled = new_cfg->voltage_enabled;
    channel->config.current_enabled = new_cfg->current_enabled;
    channel->config.power_enabled = new_cfg->power_enabled;
    channel->config.gain = new_cfg->gain;
    channel->config.bus_voltage_range = new_cfg->bus_voltage_range;
    channel->config.bus_voltage_resolution = new_cfg->bus_voltage_resolution;
    channel->config.shunt_voltage_resolution = new_cfg->shunt_voltage_resolution;
    // channel->config.i2c_addr = new_cfg->i2c_addr;

    // Reinitialize INA219 with the new configuration
    ina219_configure(&channel->dev, channel->config.bus_voltage_range, channel->config.gain,
        channel->config.bus_voltage_resolution, channel->config.shunt_voltage_resolution, INA219_MODE_CONT_SHUNT_BUS);
}
