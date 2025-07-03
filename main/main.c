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



#define I2C_PORT 0
#define I2C_ADDR CONFIG_EXAMPLE_I2C_ADDR

const static char *TAG = "INA219_example";

#define TEST_PIN GPIO_NUM_4

//pragma pack aligned to 1 byte struct with ina results
#pragma pack(1)
typedef struct {
    float voltage;      //!< Bus voltage, V
    float current;          //!< Current, A
    float power;            //!< Power, W
} ina219_data_raw_t;

typedef struct{
    ina219_data_raw_t ch1;
    ina219_data_raw_t ch2;
    ina219_data_raw_t ch3;
}ina219_full_data_t;
#pragma pack()

QueueHandle_t ina219_queue = NULL;

ina219_t dev1;
ina219_t dev2;
ina219_t dev3;

#define EX_UART_NUM UART_NUM_0
#define PATTERN_CHR_NUM    (3)         /*!< Set the number of consecutive and identical characters received by receiver which defines a UART pattern*/

#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)
static QueueHandle_t uart0_queue;

static TaskHandle_t task_ina219_handle = NULL;

esp_timer_handle_t periodic_timer;

// Timer callback to notify task_ina219
static void IRAM_ATTR timer_notify_task_ina219(void* arg) {
    if (task_ina219_handle) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(task_ina219_handle, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}


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
                    if(dtmp[0] > 0)
                    {
                        //set timer period to value received from UART
                        int timer_period = (int)dtmp[0];
                        ESP_LOGI(TAG, "Setting timer period to %d ms", timer_period);
                        esp_timer_stop(periodic_timer);
                        esp_timer_start_periodic(periodic_timer, timer_period * 1000); // Convert ms to us
                    }
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


void ina_init(ina219_t* dev, uint8_t i2c_addr)
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

static void read_ina219_data(ina219_t *dev, ina219_data_raw_t *data)
{
    ina219_data_t raw_data = {0};
    ina219_get_data(dev, &raw_data);
    //convert ina219 data to 3 floats
    data->voltage = raw_data.bus_voltage + (raw_data.shunt_voltage / 1000.0f); // Convert shunt voltage to V and add to bus voltage
    data->current = raw_data.current;
    data->power = raw_data.power;
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


void task_ina219(void *pvParameters)
{
    //test pin output
    gpio_set_direction(TEST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(TEST_PIN, 0);



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
        read_ina219_data(&dev1, &data.ch1);
        gpio_set_level(TEST_PIN, 0);
        read_ina219_data(&dev2, &data.ch2);
        gpio_set_level(TEST_PIN, 1);
        read_ina219_data(&dev3, &data.ch3);
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


void task_uart(void *pvParameters)
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
        if (xQueueReceive(ina219_queue, &received_data, portMAX_DELAY) == pdTRUE) { // Receive data from the queue
            // ESP_LOGI(TAG, "Received ina219\n");
            // uart_write_bytes(EX_UART_NUM, (const char*) &received_data, sizeof(received_data));
            // Prepare data to send over UART
            dtmp[0] = 0xAA; // Start byte
            dtmp[1] = 0xBB; // Second byte
            dtmp[2] = sizeof(ina219_full_data_t); // Size of the data
            memcpy(&dtmp[3], &received_data, sizeof(ina219_full_data_t));
            uart_write_bytes(EX_UART_NUM, (const char*) &dtmp, sizeof(dtmp));
            //print recieved data from all channels
            // printf("%.03f, %.03f, %.03f, %.03f, %.03f, %.03f, %.03f, %.03f, %.03f\n",
            // received_data.ch1.voltage, received_data.ch1.current, received_data.ch1.power,
            // received_data.ch2.voltage, received_data.ch2.current, received_data.ch2.power,
            // received_data.ch3.voltage, received_data.ch3.current, received_data.ch3.power);
            
        }
    }
}

void app_main()
{
    //create queueu
    ina219_queue = xQueueCreate(10, sizeof(ina219_full_data_t));
    if(ina219_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue");
        return;
    }

    ESP_ERROR_CHECK(i2cdev_init());
    xTaskCreate(task_ina219, "task_ina219", configMINIMAL_STACK_SIZE * 8, NULL, 5, NULL);

    xTaskCreate(task_uart, "task_uart", configMINIMAL_STACK_SIZE * 8, NULL, 4, NULL);
}
