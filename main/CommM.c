// CommM.c
// Implementation of communication module

// -----------------------------------------------------------------------------
// Includes
// -----------------------------------------------------------------------------
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <freertos/queue.h>

#include <driver/uart.h>
#include "CommM.h"
#include "CurrentDrv.h"

// -----------------------------------------------------------------------------
// Macros and Definitions
// -----------------------------------------------------------------------------
#define EX_UART_NUM UART_NUM_0
#define PATTERN_CHR_NUM    (3)         /*!< Set the number of consecutive and identical characters received by receiver which defines a UART pattern*/

#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)

typedef enum CommandType {
    CMD_NONE = 0,
    CMD_MODE = 0x0A, // Mode command
    CMD_SET_PERIOD = 0x0B, // Set period command
    CMD_PRINT_MODE = 0x0C, // Print mode command
    CMD_CHANNEL_CFG = 0x0D, // Channel configuration command
    CMD_STATUS = 0x0E, // Status command
    CMD_MAX
} CommandType_t;

typedef enum PrintMode {
    PRINT_MODE_RAW = 0,
    PRINT_MODE_ASCII = 0x01, // ASCII print mode
    PRINT_MODE_MAX
} PrintMode_t;
// -----------------------------------------------------------------------------
// Static (Private) Variables
// -----------------------------------------------------------------------------

static QueueHandle_t uart0_queue;
static const char *TAG = "CommM";

static PrintMode_t current_print_mode = PRINT_MODE_RAW; // Default print mode


// -----------------------------------------------------------------------------
// Static (Private) Function Declarations
// -----------------------------------------------------------------------------
static void uart_event_task(void *pvParameters);
static void uart_command_handler(const uint8_t *command, uint8_t size);
static uint8_t uart_prepare_message(uint8_t *buffer, const ina219_full_data_t *data, const CurrentDrv_Config_t* const config);
static uint8_t uart_fill_channel_data(uint8_t *buffer, const ina219_data_raw_t *data, const CurrentDrv_ChannelCfg_t* const config);

// -----------------------------------------------------------------------------
// Public Function Implementations
// -----------------------------------------------------------------------------

void CommM_Init(void) {
    // Initialization code here
}

void CommM_Task(void *pvParameters)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = 256000,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    //Install UART driver, and get the queue.
    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);

    //Set UART log level
    esp_log_level_set(TAG, ESP_LOG_INFO);
    //Set UART pins (using UART0 default pins ie no changes.)
    uart_set_pin(EX_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    //Set uart pattern detect function.
    uart_enable_pattern_det_baud_intr(EX_UART_NUM, '+', PATTERN_CHR_NUM, 9, 0, 0);
    //Reset the pattern queue length to record at most 20 pattern positions.
    uart_pattern_queue_reset(EX_UART_NUM, 20);

    //Create a task to handler UART event from ISR
    xTaskCreate(uart_event_task, "uart_event_task", 2048, NULL, 12, NULL);

    ina219_full_data_t received_data;
    uint8_t dtmp[sizeof(ina219_full_data_t)+3] = {0};
    while (1) {
        if (xQueueReceive(*CurrentDrv_GetQueue(), &received_data, portMAX_DELAY) == pdTRUE) { // Receive data from the queue
            // ESP_LOGI(TAG, "Received ina219\n");
            // uart_write_bytes(EX_UART_NUM, (const char*) &received_data, sizeof(received_data));
            // Prepare data to send over UART
            
            // dtmp[0] = 0xAA; // Start byte
            // dtmp[1] = 0xBB; // Second byte
            // dtmp[2] = sizeof(ina219_full_data_t); // Size of the data
            // memcpy(&dtmp[3], &received_data, sizeof(ina219_full_data_t));
            // uart_write_bytes(EX_UART_NUM, (const char*) &dtmp, sizeof(dtmp));

            //depends on mode printf or uart_prepare_message
            if(current_print_mode ==  PRINT_MODE_ASCII)
            {
            //print recieved data from all channels
            printf("%.03f, %.03f, %.03f, %.03f, %.03f, %.03f, %.03f, %.03f, %.03f\n",
                received_data.ch1.voltage, received_data.ch1.current, received_data.ch1.power,
                received_data.ch2.voltage, received_data.ch2.current, received_data.ch2.power,
                received_data.ch3.voltage, received_data.ch3.current, received_data.ch3.power);
            }
            else if(current_print_mode == PRINT_MODE_RAW)
            {
                //uart_prepare_message(dtmp, &received_data, CurrentDrv_GetChannelConfig());
                //uart_write_bytes(EX_UART_NUM, (const char*) dtmp, sizeof(dtmp));
                CurrentDrv_Config_t config = {0};
                CurrentDrv_GetChannelConfig(&config);
                uint8_t size = uart_prepare_message(dtmp, &received_data, &config);
                uart_write_bytes(EX_UART_NUM, (const char*) dtmp, size);
            }
            
        }
    }
}

// -----------------------------------------------------------------------------
// Static (Private) Function Implementations
// -----------------------------------------------------------------------------


static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    size_t buffered_size;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
    for(;;) {
        //Waiting for UART event.
        if(xQueueReceive(uart0_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE);
            ESP_LOGI(TAG, "uart[%d] event:", EX_UART_NUM);
            switch(event.type) {
                //Event of UART receving data
                /*We'd better handler data event fast, there would be much more data events than
                other types of events. If we take too much time on data event, the queue might
                be full.*/
                case UART_DATA:
                    ESP_LOGI(TAG, "[UART DATA]: %d", event.size);
                    uart_read_bytes(EX_UART_NUM, dtmp, event.size, portMAX_DELAY);
                    // ESP_LOGI(TAG, "[DATA EVT]:");
                    // uart_write_bytes(EX_UART_NUM, (const char*) dtmp, event.size);

                    uart_command_handler(dtmp, event.size);
                    // if(dtmp[0] > 0)
                    // {
                    //     //set timer period to value received from UART
                    //     int timer_period = (int)dtmp[0];
                    //     ESP_LOGI(TAG, "Setting timer period to %d ms", timer_period);
                    //     esp_timer_handle_t* timerPtr = CurrentDrv_GetPeriodicTimer();
                    //     esp_timer_stop(*timerPtr);
                    //     esp_timer_start_periodic(*timerPtr, timer_period * 1000); // Convert ms to us
                    // }
                    break;
                //Event of HW FIFO overflow detected
                case UART_FIFO_OVF:
                    ESP_LOGI(TAG, "hw fifo overflow");
                    // If fifo overflow happened, you should consider adding flow control for your application.
                    // The ISR has already reset the rx FIFO,
                    // As an example, we directly flush the rx buffer here in order to read more data.
                    uart_flush_input(EX_UART_NUM);
                    xQueueReset(uart0_queue);
                    break;
                //Event of UART ring buffer full
                case UART_BUFFER_FULL:
                    ESP_LOGI(TAG, "ring buffer full");
                    // If buffer full happened, you should consider encreasing your buffer size
                    // As an example, we directly flush the rx buffer here in order to read more data.
                    uart_flush_input(EX_UART_NUM);
                    xQueueReset(uart0_queue);
                    break;
                //Event of UART RX break detected
                case UART_BREAK:
                    ESP_LOGI(TAG, "uart rx break");
                    break;
                //Event of UART parity check error
                case UART_PARITY_ERR:
                    ESP_LOGI(TAG, "uart parity error");
                    break;
                //Event of UART frame error
                case UART_FRAME_ERR:
                    ESP_LOGI(TAG, "uart frame error");
                    break;
                //UART_PATTERN_DET
                case UART_PATTERN_DET:
                    uart_get_buffered_data_len(EX_UART_NUM, &buffered_size);
                    int pos = uart_pattern_pop_pos(EX_UART_NUM);
                    ESP_LOGI(TAG, "[UART PATTERN DETECTED] pos: %d, buffered size: %d", pos, buffered_size);
                    if (pos == -1) {
                        // There used to be a UART_PATTERN_DET event, but the pattern position queue is full so that it can not
                        // record the position. We should set a larger queue size.
                        // As an example, we directly flush the rx buffer here.
                        uart_flush_input(EX_UART_NUM);
                    } else {
                        uart_read_bytes(EX_UART_NUM, dtmp, pos, 100 / portTICK_PERIOD_MS);
                        uint8_t pat[PATTERN_CHR_NUM + 1];
                        memset(pat, 0, sizeof(pat));
                        uart_read_bytes(EX_UART_NUM, pat, PATTERN_CHR_NUM, 100 / portTICK_PERIOD_MS);
                        ESP_LOGI(TAG, "read data: %s", dtmp);
                        ESP_LOGI(TAG, "read pat : %s", pat);
                    }
                    break;
                //Others
                default:
                    ESP_LOGI(TAG, "uart event type: %d", event.type);
                    break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

static bool pasrse_command_to_config(const uint8_t *command, uint8_t size, CurrentDrv_Config_t *config)
{
    if (size != 7 || command == NULL || config == NULL) {
        ESP_LOGE(TAG, "Invalid command or config");
        return false;
    }

    // Parse the command to fill the CurrentDrv_Config_t structure
    // Channel 1
    config->ch1.voltage_enabled = (command[1] & 0x01) != 0;
    config->ch1.current_enabled = (command[1] & 0x02) != 0;
    config->ch1.power_enabled = (command[1] & 0x04) != 0;

    config->ch1.gain = (ina219_gain_t)(command[2] & 0x03);
    config->ch1.bus_voltage_range = (ina219_bus_voltage_range_t)((command[2] >> 2) & 0x01);
    config->ch1.bus_voltage_resolution = (ina219_resolution_t)((command[2] >> 4) & 0x04);
    config->ch1.shunt_voltage_resolution = config->ch1.bus_voltage_resolution;

    // Channel 2
    config->ch2.voltage_enabled = (command[3] & 0x01) != 0;
    config->ch2.current_enabled = (command[3] & 0x02) != 0;
    config->ch2.power_enabled = (command[3] & 0x04) != 0;

    config->ch2.gain = (ina219_gain_t)(command[4] & 0x03);
    config->ch2.bus_voltage_range = (ina219_bus_voltage_range_t)((command[4] >> 2) & 0x01);
    config->ch2.bus_voltage_resolution = (ina219_resolution_t)((command[4] >> 4) & 0x04);
    config->ch2.shunt_voltage_resolution = config->ch2.bus_voltage_resolution;

    // Channel 3
    config->ch3.voltage_enabled = (command[5] & 0x01) != 0;
    config->ch3.current_enabled = (command[5] & 0x02) != 0;
    config->ch3.power_enabled = (command[5] & 0x04) != 0;

    config->ch3.gain = (ina219_gain_t)(command[6] & 0x03);
    config->ch3.bus_voltage_range = (ina219_bus_voltage_range_t)((command[6] >> 2) & 0x01);
    config->ch3.bus_voltage_resolution = (ina219_resolution_t)((command[6] >> 4) & 0x04);
    config->ch3.shunt_voltage_resolution = config->ch3.bus_voltage_resolution;

    // ESP_LOGI(TAG, "Channel configuration updated");
    return true;
}

static void uart_command_handler(const uint8_t *command, uint8_t size)
{
    if(size == 0 || command == NULL) {
        ESP_LOGE(TAG, "Invalid command received");
        return;
    }

    CommandType_t cmd_type = command[0];

    switch(cmd_type) {
        case CMD_MODE:
            if(size < 2) {
                ESP_LOGE(TAG, "Invalid mode command size");
                return;
            }
            // Handle mode command
            // ESP_LOGI(TAG, "Mode command received: %d", command[1]);
            if(command[1] == 0x01)
            {
                CurrentDrv_StartTimer(true); // Start the timer with auto-configuration enabled
                ESP_LOGI(TAG, "START mode activated");
            }
            else if(command[1] == 0x00)
            {
                CurrentDrv_StopTimer();
                ESP_LOGI(TAG, "STOP mode activated");
            }
            else if (command[1] == 0x02)
            {
                CurrentDrv_StartTimer(false); // Start the timer without auto-configuration
                ESP_LOGI(TAG, "START mode without auto-configuration activated");
            }
            else
            {
                ESP_LOGE(TAG, "Unknown mode command: %d", command[1]);
            }
            break;

        case CMD_SET_PERIOD:
            if(size < 2) {
                ESP_LOGE(TAG, "Invalid set period command size");
                return;
            }
            // Handle set period command
            // ESP_LOGI(TAG, "Set period command received: %d", command[1]);
            uint32_t period_us = (uint32_t)command[1];
            period_us += (uint32_t)command[2] << 8;
            period_us += (uint32_t)command[3] << 16;
            period_us += (uint32_t)command[4] << 24;

            CurrentDrv_SetTimerPeriod(period_us);
            ESP_LOGI(TAG, "Timer period set to %lu ms", period_us);
            break;

        case CMD_PRINT_MODE:
            // Handle print mode command
            // ESP_LOGI(TAG, "Print mode command received");
            if(size < 2) {
                ESP_LOGE(TAG, "Invalid print mode command size");
                return;
            }
            
            if(command[1] == PRINT_MODE_RAW) {
                current_print_mode = PRINT_MODE_RAW;
                ESP_LOGI(TAG, "Print mode set to RAW");
            } else if(command[1] == PRINT_MODE_ASCII) {
                current_print_mode = PRINT_MODE_ASCII;
                ESP_LOGI(TAG, "Print mode set to ASCII");
            } else {
                ESP_LOGE(TAG, "Unknown print mode: %d", command[1]);
            }
            break;

        case CMD_CHANNEL_CFG:
            // Handle channel configuration command
            // ESP_LOGI(TAG, "Channel configuration command received");

            CurrentDrv_Config_t channel_config = {
                .ch1 = {0},
                .ch2 = {0},
                .ch3 = {0}
            };

            if(pasrse_command_to_config(command, size, &channel_config)) {
                CurrentDrv_SetChannelConfig(&channel_config);
                ESP_LOGI(TAG, "Channel configuration updated");
            } else {
                ESP_LOGE(TAG, "Failed to parse channel configuration command");
            }

            break;

        case CMD_STATUS:
            // Handle status command
            // ESP_LOGI(TAG, "Status command received");

            ESP_LOGI(TAG, "Current Print Mode: %d", current_print_mode);
            CurrentDrv_Config_t config = {0};
            CurrentDrv_GetChannelConfig(&config);
            ESP_LOGI(TAG, "Channel 1 - Voltage: %s, Current: %s, Power: %s",
                     config.ch1.voltage_enabled ? "Enabled" : "Disabled",
                     config.ch1.current_enabled ? "Enabled" : "Disabled",
                     config.ch1.power_enabled ? "Enabled" : "Disabled");
            ESP_LOGI(TAG, "Channel 1 Gain: %d, Bus Voltage Range: %d, Bus Voltage Resolution: %d, Shunt Voltage Resolution: %d",
                     config.ch1.gain, config.ch1.bus_voltage_range, config.ch1.bus_voltage_resolution, config.ch1.shunt_voltage_resolution);

            ESP_LOGI(TAG, "Channel 2 - Voltage: %s, Current: %s, Power: %s",
                     config.ch2.voltage_enabled ? "Enabled" : "Disabled",
                     config.ch2.current_enabled ? "Enabled" : "Disabled",
                     config.ch2.power_enabled ? "Enabled" : "Disabled");
            ESP_LOGI(TAG, "Channel 2 Gain: %d, Bus Voltage Range: %d, Bus Voltage Resolution: %d, Shunt Voltage Resolution: %d",
                     config.ch2.gain, config.ch2.bus_voltage_range, config.ch2.bus_voltage_resolution, config.ch2.shunt_voltage_resolution);

            ESP_LOGI(TAG, "Channel 3 - Voltage: %s, Current: %s, Power: %s",
                     config.ch3.voltage_enabled ? "Enabled" : "Disabled",
                        config.ch3.current_enabled ? "Enabled" : "Disabled",
                        config.ch3.power_enabled ? "Enabled" : "Disabled");
            ESP_LOGI(TAG, "Channel 3 Gain: %d, Bus Voltage Range: %d, Bus Voltage Resolution: %d, Shunt Voltage Resolution: %d",
                     config.ch3.gain, config.ch3.bus_voltage_range, config.ch3.bus_voltage_resolution, config.ch3.shunt_voltage_resolution);

            //print command in hex for configuration
            uint8_t cmd_hex[7] = {0};
            cmd_hex[0] = CMD_CHANNEL_CFG;
            cmd_hex[1] = (config.ch1.voltage_enabled ? 0x01 : 0x00) |
                         (config.ch1.current_enabled ? 0x02 : 0x00) |
                         (config.ch1.power_enabled ? 0x04 : 0x00);
            cmd_hex[2] = (config.ch1.gain & 0x03) |
                         ((config.ch1.bus_voltage_range & 0x01) << 2) |
                         ((config.ch1.bus_voltage_resolution & 0x0f) << 4);
            cmd_hex[3] = (config.ch2.voltage_enabled ? 0x01 : 0x00) |
                         (config.ch2.current_enabled ? 0x02 : 0x00) |
                            (config.ch2.power_enabled ? 0x04 : 0x00);
            cmd_hex[4] = (config.ch2.gain & 0x03) |
                         ((config.ch2.bus_voltage_range & 0x01) << 2) |
                         ((config.ch2.bus_voltage_resolution & 0x0f) << 4);
            cmd_hex[5] = (config.ch3.voltage_enabled ? 0x01 : 0x00) |
                         (config.ch3.current_enabled ? 0x02 : 0x00) |
                            (config.ch3.power_enabled ? 0x04 : 0x00);
            cmd_hex[6] = (config.ch3.gain & 0x03) |
                         ((config.ch3.bus_voltage_range & 0x01) << 2) |
                         ((config.ch3.bus_voltage_resolution & 0x0f) << 4);
            ESP_LOGI(TAG, "Channel configuration command in hex:");
            ESP_LOGI(TAG, "0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X",
                     cmd_hex[0], cmd_hex[1], cmd_hex[2], cmd_hex[3], cmd_hex[4], cmd_hex[5], cmd_hex[6]);

            //queue status
            if (CurrentDrv_GetQueue() != NULL) {
                ESP_LOGI(TAG, "CurrentDrv Queue Status: %d items", uxQueueMessagesWaiting(*CurrentDrv_GetQueue()));
            } else {
                ESP_LOGE(TAG, "CurrentDrv Queue is NULL");
            }

            ESP_LOGI(TAG, "Timer Period: %lu ms", CurrentDrv_GetTimerPeriod());
            ESP_LOGI(TAG, "Current Timer Status: %s", (esp_timer_is_active(*CurrentDrv_GetPeriodicTimer()) ? "Running" : "Stopped"));
            if (current_print_mode == PRINT_MODE_RAW) {
                ESP_LOGI(TAG, "Current Print Mode: RAW");
            } else if (current_print_mode == PRINT_MODE_ASCII) {
                ESP_LOGI(TAG, "Current Print Mode: ASCII");
            } else {
                ESP_LOGE(TAG, "Unknown print mode: %d", current_print_mode);
            }
            
            break;

        default:
            ESP_LOGE(TAG, "Unknown command type: %d", cmd_type);
            ESP_LOGI(TAG, "Supported commands:");
            ESP_LOGI(TAG, "Commands are sent as binary messages. The first byte is the command type, followed by command-specific data.\n");
            ESP_LOGI(TAG, "| Command Name         | Code (hex) | Payload Format                  | Description                                 |");
            ESP_LOGI(TAG, "|----------------------|------------|---------------------------------|---------------------------------------------|");
            ESP_LOGI(TAG, "| Start/Stop           | 0x0A       | [0x0A, {0x01, 0x00, 0x02}]      | Start (0x01) or stop (0x00) measurement     |");
            ESP_LOGI(TAG, "|                      |            |                                 | (0x02) to start without autoconfiguration   |");
            ESP_LOGI(TAG, "| Set Timer Period     | 0x0B       | [0x0B, <u8>, <u8>, <u8>, <u8>]  | Set period in microseconds (little-endian)  |");
            ESP_LOGI(TAG, "| Set Print Mode       | 0x0C       | [0x0C, 0x00] or [0x0C, 0x01]    | Raw (0x00) or ASCII (0x01) output           |");
            ESP_LOGI(TAG, "| Channel Config       | 0x0D       | [0x0D, <ch1>, <ch2>, <ch3>]     | Set channel configuration. Details in readme|");
            ESP_LOGI(TAG, "| Status               | 0x0E       | [0x0E]                          | Query current status/config                 |");
            break;
    }
}

static uint8_t uart_fill_channel_data(uint8_t *buffer, const ina219_data_raw_t *data, const CurrentDrv_ChannelCfg_t* const config)
{
    if (buffer == NULL || data == NULL || config == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for uart_fill_channel_data");
        return 0;
    }

    uint8_t size = 0;

    // Fill the buffer with channel data based on configuration
    if (config->voltage_enabled) {
        memcpy(&buffer[size], &data->voltage, sizeof(data->voltage));
        size += sizeof(data->voltage);
    }
    if (config->current_enabled) {
        memcpy(&buffer[size], &data->current, sizeof(data->current));
        size += sizeof(data->current);
    }
    if (config->power_enabled) {
        memcpy(&buffer[size], &data->power, sizeof(data->power));
        size += sizeof(data->power);
    }

    return size;
}

static uint8_t uart_prepare_message(uint8_t *buffer, const ina219_full_data_t *data, const CurrentDrv_Config_t* const config)
{
    if (buffer == NULL || data == NULL || config == NULL) {
        ESP_LOGE(TAG, "Invalid parameters for uart_prepare_message");
        return 0;
    }

    uint8_t size = 3;
    // Prepare the message in the buffer
    buffer[0] = 0xAA; // Start byte
    buffer[1] = 0xBB; // Second byte

    // Fill the buffer with channel data
    size += uart_fill_channel_data(&buffer[size], &data->ch1, &config->ch1);    
    size += uart_fill_channel_data(&buffer[size], &data->ch2, &config->ch2);    
    size += uart_fill_channel_data(&buffer[size], &data->ch3, &config->ch3);

    buffer[2] = size-3; // Size of the data

    return size; // Return the size of the prepared message
}
