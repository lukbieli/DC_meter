/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "math.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "DisplayM.h"
#include "CurrentDrv.h"

static const char *TAG = "DisplayM";

#if CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
#include "esp_lcd_sh1107.h"
#else
#include "esp_lcd_panel_vendor.h"
#endif

#define I2C_BUS_PORT  1

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ    (400 * 1000)
#define EXAMPLE_PIN_NUM_SDA           18
#define EXAMPLE_PIN_NUM_SCL           19
#define EXAMPLE_PIN_NUM_RST           -1
#define EXAMPLE_I2C_HW_ADDR           0x3C

// The pixel number in horizontal and vertical
#if CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306
#define EXAMPLE_LCD_H_RES              128
#define EXAMPLE_LCD_V_RES              CONFIG_EXAMPLE_SSD1306_HEIGHT
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
#define EXAMPLE_LCD_H_RES              64
#define EXAMPLE_LCD_V_RES              128
#endif
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS           8
#define EXAMPLE_LCD_PARAM_BITS         8

#define BUTTON_GPIO 34

#define UPDATE_SCREEN_PERIOD_MS 200 // Update screen every 200ms
#define SLEEP_BLINK_COUNT 5 // Number of blinks before going to sleep

typedef struct eye_obj_t {
    lv_obj_t *eyelid;
    lv_anim_t eye_anim;
    lv_obj_t *pupil;
    lv_obj_t *eye_border;
    int pos_x;
    int pos_y;
    int pupil_pos_x;
} eye_obj_t;

//struct for normal screen with voltage, current and power labels
typedef struct normal_screen_t {
    lv_obj_t *voltage_label;
    lv_obj_t *current_label;
    lv_obj_t *power_label;
    lv_obj_t *channelName_label; // Label for channel name
    int channel; // Channel number for this screen
} normal_screen_t;


// static lv_obj_t *eyelid_left = NULL;
// static lv_obj_t *eyelid_right = NULL;
// static lv_anim_t eye_anim_left;
// static lv_anim_t eye_anim_right;
static lv_timer_t *blink_timer = NULL;
static lv_obj_t *smile_obj = NULL;
static lv_obj_t *mouthO_obj = NULL;
static volatile bool button_pressed = false;

static TaskHandle_t *button_task_handle = NULL;

static bool demo_mode_enabled = true;
static int sleep_blink_counter = 0;
static bool sleep_state = false; // Flag to indicate if the display is in sleep mode

static lv_disp_t *current_disp = NULL;

static lv_obj_t *normal_label = NULL;

eye_obj_t eye_left_obj = {0};
eye_obj_t eye_right_obj = {0};

normal_screen_t normal_screen = {
    .channel = 1,
    .voltage_label = NULL,
    .current_label = NULL,
    .power_label = NULL
};


static void display_eye(lv_disp_t *disp,int pos_x, int pos_y, eye_obj_t *eye_obj);
static void eye_start_anim(eye_obj_t *eye_obj);
static void eye_stop_anim(eye_obj_t *eye_obj);
// static void display_eye(lv_disp_t *disp,int pos_x, int pos_y, lv_obj_t **eyelid_out, lv_anim_t *eye_anim_out);
static void display_smile(lv_disp_t *disp, int pos_x, int pos_y);
static void eyelid_anim_cb(void * obj, int32_t v);
static void blink_eyelids(lv_timer_t * timer);
static void open_eyelids_cb(lv_timer_t * t);
static void open_eyelids(void);
static void update_smile(lv_disp_t *disp, int pos_x, int pos_y);
static uint32_t get_random_blink_period();
static void button_task(void *arg);
static void IRAM_ATTR button_isr_handler(void* arg);

static void run_demo(lv_disp_t *disp);
static void stop_demo(lv_disp_t *disp);
static void run_normal_mode(lv_disp_t *disp);
static void stop_normal_mode(lv_disp_t *disp);

static void update_screen(lv_timer_t * timer);
static lv_timer_t *update_screen_timer = NULL;
static void switch_to_normal_cb(lv_timer_t * t);

void DisplayM_Init(void)
{
    // Configure button GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);


    ESP_LOGI(TAG, "Initialize I2C bus");
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = EXAMPLE_PIN_NUM_SDA,
        .scl_io_num = EXAMPLE_PIN_NUM_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = EXAMPLE_I2C_HW_ADDR,
        .scl_speed_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
        .control_phase_bytes = 1,               // According to SSD1306 datasheet
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,   // According to SSD1306 datasheet
        .lcd_param_bits = EXAMPLE_LCD_CMD_BITS, // According to SSD1306 datasheet
#if CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306
        .dc_bit_offset = 6,                     // According to SSD1306 datasheet
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
        .dc_bit_offset = 0,                     // According to SH1107 datasheet
        .flags =
        {
            .disable_control_phase = 1,
        }
#endif
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install SSD1306 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = EXAMPLE_PIN_NUM_RST,
    };
#if CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = EXAMPLE_LCD_V_RES,
    };
    panel_config.vendor_config = &ssd1306_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1107(io_handle, &panel_config, &panel_handle));
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
#endif

    ESP_LOGI(TAG, "Initialize LVGL");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES,
        .double_buffer = true,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .monochrome = true,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        }
    };
    current_disp = lvgl_port_add_disp(&disp_cfg);

    /* Rotation of the screen */
    lv_disp_set_rotation(current_disp, LV_DISP_ROT_180);



    ESP_LOGI(TAG, "Display Mode: %s", demo_mode_enabled ? "Demo" : "Normal");
    // Lock the mutex due to the LVGL APIs are not thread-safe
    if (lvgl_port_lock(0)) {
        if(demo_mode_enabled) {
            run_demo(current_disp);
            // Create a timer to switch to normal mode after 10 seconds
            lv_timer_t *switch_timer = lv_timer_create_basic();
            lv_timer_set_period(switch_timer, 10000);
            lv_timer_set_repeat_count(switch_timer, 1);
            lv_timer_set_cb(switch_timer, switch_to_normal_cb);
        } else {
            run_normal_mode(current_disp);
        }
        // Release the mutex
        lvgl_port_unlock();
    }

    
    // Start button monitor task
    xTaskCreate(button_task, "button_task", 2048, current_disp, 5, button_task_handle);

}

void DisplayM_EnableDemoMode(bool enable)
{
    // Fix: update demo_mode_enabled before switching screens
    if(enable != demo_mode_enabled) {
        demo_mode_enabled = enable; // <-- update the flag first!
        if (lvgl_port_lock(0)) {
            if (enable) {
                stop_normal_mode(current_disp);
                run_demo(current_disp);
            } else {
                stop_demo(current_disp);
                run_normal_mode(current_disp);
            }
            // Release the mutex
            lvgl_port_unlock();
        }
    }
}


static uint32_t get_random_blink_period()
{
    // Random period between 1200ms and 3500ms
    return 800 + (rand() % 6000);
}

static void IRAM_ATTR button_isr_handler(void* arg) {
    // Read pin state directly in ISR
    int level = gpio_get_level(BUTTON_GPIO);
    button_pressed = (level == 1); // Assuming active low button
}

static void button_task(void *arg) {
    lv_disp_t *disp = (lv_disp_t *)arg;
    bool last_state = false;
    while (1) {
        if (button_pressed != last_state) {
            last_state = button_pressed;
            
            if(demo_mode_enabled)
            {
                if (lvgl_port_lock(0)) {
                    update_smile(disp, 0, 55);
                    sleep_blink_counter = 0; // Reset the sleep blink counter
                    if(sleep_state)
                    {
                        //wakeup - open eyelids
                        if (button_pressed) {
                            sleep_state = false;
                            open_eyelids();

                            //start blinking timer with random period
                            if (blink_timer) {
                                lv_timer_del(blink_timer);
                            }
                            blink_timer = lv_timer_create(blink_eyelids, get_random_blink_period(), NULL);
                        }
                    }
                    // Release the mutex
                    lvgl_port_unlock();
                }
            }
            else
            {
                if(button_pressed == true)
                {
                    if (++normal_screen.channel > 3) {
                        normal_screen.channel = 1; // Reset to channel 1 if it exceeds 3
                    }
                    ESP_LOGI(TAG, "Button pressed - Channel: %d", normal_screen.channel);
                    // Update the screen with the new channel information
                    if (lvgl_port_lock(0)) {
                        update_screen(update_screen_timer);
                        // Release the mutex
                        lvgl_port_unlock();
                    }
                }
            }
        }
    

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void update_smile(lv_disp_t *disp, int pos_x, int pos_y) {

    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    if (button_pressed) {
        //hide the smile
        if (smile_obj) {
            lv_obj_set_style_line_opa(smile_obj, LV_OPA_TRANSP, LV_STATE_DEFAULT);
        }
        // Show mouthO_obj
        if(mouthO_obj)
        {
            lv_obj_set_style_bg_opa(mouthO_obj, LV_OPA_COVER, LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(mouthO_obj, LV_OPA_COVER, LV_STATE_DEFAULT);
        }
        //pause eye_anim_left and eye_anim_right
        eye_stop_anim(&eye_left_obj);
        eye_stop_anim(&eye_right_obj);
    } else {
        //hide the mouthO_obj
        if(mouthO_obj)
        {
            lv_obj_set_style_bg_opa(mouthO_obj, LV_OPA_TRANSP, LV_STATE_DISABLED);
            lv_obj_set_style_border_opa(mouthO_obj, LV_OPA_TRANSP, LV_STATE_DEFAULT);

        }
        // Show the smile
        if (smile_obj) {
            lv_obj_set_style_line_opa(smile_obj, LV_OPA_COVER, LV_STATE_DEFAULT);
        }
        //start eye_anim_left and eye_anim_right
        eye_start_anim(&eye_left_obj);
        eye_start_anim(&eye_right_obj);
    }
}

void run_demo(lv_disp_t *disp)
{
    lv_obj_t *scr = lv_disp_get_scr_act(disp);    

    display_eye(disp, -16, 20, &eye_left_obj);
    display_eye(disp, 16, 20, &eye_right_obj);

    display_smile(disp, 0, 55);

    // Start blinking timer with random period
    blink_timer = lv_timer_create(blink_eyelids, get_random_blink_period(), NULL);

    


}

static void stop_demo(lv_disp_t *disp)
{
    // Stop the blinking timer
    if (blink_timer) {
        lv_timer_del(blink_timer);
        blink_timer = NULL;
    }

    // Delete the smile object
    if (smile_obj) {
        lv_obj_del(smile_obj);
        smile_obj = NULL;
    }

    // Delete the mouthO_obj
    if (mouthO_obj) {
        lv_obj_del(mouthO_obj);
        mouthO_obj = NULL;
    }

    // Delete the eye objects
    eye_stop_anim(&eye_left_obj);
    eye_stop_anim(&eye_right_obj);
    
    lv_obj_del(eye_left_obj.eye_border);
    lv_obj_del(eye_left_obj.pupil);
    lv_obj_del(eye_left_obj.eyelid);

    lv_obj_del(eye_right_obj.eye_border);
    lv_obj_del(eye_right_obj.pupil);
    lv_obj_del(eye_right_obj.eyelid);

}

static void run_normal_mode(lv_disp_t *disp)
{
    //display text "Normal mode" on the screen
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    // normal_label = lv_label_create(scr);
    // lv_label_set_text(normal_label, "Normal mode");
    // lv_obj_align(normal_label, LV_ALIGN_TOP_MID, 0, 0);

    normal_screen.channelName_label = lv_label_create(scr);
    // lv_label_set_long_mode(normal_screen.channelName_label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    lv_label_set_text(normal_screen.channelName_label, "Channel1\n\n00.00V\n00.00mA\n00.00mW");
    //change font size to 32
    lv_obj_set_style_text_font(normal_screen.channelName_label, &lv_font_unscii_8, LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(normal_screen.channelName_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    // lv_obj_set_style_text_color(normal_label, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    // lv_obj_set_style_text_opa(normal_screen.channelName_label, LV_OPA_COVER, LV_STATE_DEFAULT);
    // lv_obj_set_style_bg_color(normal_label, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    // lv_obj_set_style_bg_opa(normal_screen.channelName_label, LV_OPA_COVER, LV_STATE_DEFAULT);
    /* Size of the screen (if you use rotation 90 or 270, please set disp->driver->ver_res) */
    // lv_obj_set_width(normal_screen.channelName_label, disp->driver->hor_res);
    lv_obj_align(normal_screen.channelName_label, LV_ALIGN_TOP_MID, 0, 20); 

    // EXP_LOGI(TAG, "Running normal mode on display: %p", disp);

    // create timer to update screen
    if (update_screen_timer == NULL) {
        update_screen_timer = lv_timer_create(update_screen, UPDATE_SCREEN_PERIOD_MS, disp);
        // ESP_LOGI(TAG, "Created update screen timer: %p", update_screen_timer);
    } else {
        lv_timer_set_period(update_screen_timer, UPDATE_SCREEN_PERIOD_MS);
    }   
    // Initial update of the screen
    update_screen(update_screen_timer);
}

static void stop_normal_mode(lv_disp_t *disp)
{
    // Remove the label from the screen
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    if (normal_screen.channelName_label) {
        lv_obj_del(normal_screen.channelName_label);
        normal_screen.channelName_label = NULL;
    }

    if (normal_screen.voltage_label) {
        lv_obj_del(normal_screen.voltage_label);
        normal_screen.voltage_label = NULL;
    }

    if (normal_screen.current_label) {
        lv_obj_del(normal_screen.current_label);
        normal_screen.current_label = NULL;
    }

    if (normal_screen.power_label) {
        lv_obj_del(normal_screen.power_label);
        normal_screen.power_label = NULL;
    }

    //delete timer if it exists
    if (update_screen_timer) {
        lv_timer_del(update_screen_timer);
        update_screen_timer = NULL;
    }

    // Reset channel to 1
    normal_screen.channel = 1;




    // Stop any animations or timers related to normal mode
    // Currently, there are no specific animations or timers in normal mode
}

static void update_screen(lv_timer_t * timer)
{
    // lv_disp_t *disp = (lv_disp_t *)timer->user_data;
    // lv_obj_t *scr = lv_disp_get_scr_act(disp);

    ina219_full_data_t full_data;
    CurrentDrv_GetAvgData(&full_data);
    ina219_data_raw_t* data = &full_data.ch1;
    if(normal_screen.channel == 1)
    {
        data = &full_data.ch1;
    }
    else if(normal_screen.channel == 2)
    {
        data = &full_data.ch2;
    }
    else if(normal_screen.channel == 3)
    {
        data = &full_data.ch3;
    }
    else
    {
        ESP_LOGE(TAG, "Invalid channel number: %d", normal_screen.channel);
        return;
    }

    char text[64] = {0};
    //fixed number of digits for each value
    // snprintf();
    snprintf(text, sizeof(text), "Channel%d\n\n%05.2f V\n%06.3fmA\n%06.2fmW",
             normal_screen.channel,
             data->voltage, // Convert to V
             data->current * 1000.0f, // Convert to mA
             data->power * 1000.0f); // Convert to mW

    // ESP_LOGI(TAG, "Updating screen with text: %s", text);

    // Update voltage, current and power labels
    if (normal_screen.channelName_label != NULL) {
        lv_label_set_text(normal_screen.channelName_label, text);
    }
}

static void display_eye(lv_disp_t *disp,int pos_x, int pos_y, eye_obj_t *eye_obj)
// static void display_eye(lv_disp_t *disp,int pos_x, int pos_y, lv_obj_t **eyelid_out, lv_anim_t *eye_anim)
{
    eye_obj->pos_x = pos_x;
    eye_obj->pos_y = pos_y;
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    // display circle with white border and black fill
    eye_obj->eye_border = lv_obj_create(scr);
    lv_obj_set_size(eye_obj->eye_border, 32, 32); // size of the circle
    lv_obj_set_style_bg_color(eye_obj->eye_border, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT); // black fill
    lv_obj_set_style_bg_opa(eye_obj->eye_border, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(eye_obj->eye_border, 16, LV_STATE_DEFAULT); // radius for circle
    lv_obj_set_style_border_color(eye_obj->eye_border, lv_color_hex(0x000000), LV_STATE_DEFAULT); // white border
    lv_obj_set_style_border_width(eye_obj->eye_border, 1, LV_STATE_DEFAULT); // width of the border
    lv_obj_align(eye_obj->eye_border, LV_ALIGN_TOP_MID, pos_x, pos_y);
    
    //white circle inside that circle
    eye_obj->pupil = lv_obj_create(scr);
    lv_obj_set_size(eye_obj->pupil, 14, 14); // size of the inner circle
    lv_obj_set_style_bg_color(eye_obj->pupil, lv_color_hex(0x000000), LV_STATE_DEFAULT); // white inner circle
    lv_obj_set_style_bg_opa(eye_obj->pupil, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(eye_obj->pupil, 4, LV_STATE_DEFAULT); // radius for inner circle
    lv_obj_align(eye_obj->pupil, LV_ALIGN_TOP_MID, pos_x, pos_y+8);

    // //animate that inner circle moves left right inside eye_border
    // lv_anim_init(&eye_obj->eye_anim);
    // lv_anim_set_var(&eye_obj->eye_anim, eye_obj->pupil);
    // lv_anim_set_exec_cb(&eye_obj->eye_anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    // lv_anim_set_values(&eye_obj->eye_anim, pos_x-8, pos_x + 8); // move from -16 to 16
    // lv_anim_set_time(&eye_obj->eye_anim, 2000); // duration of the animation
    // lv_anim_set_repeat_count(&eye_obj->eye_anim, LV_ANIM_REPEAT_INFINITE); // repeat infinitely
    // lv_anim_set_playback_time(&eye_obj->eye_anim, 2000); // playback time
    // lv_anim_set_path_cb(&eye_obj->eye_anim, lv_anim_path_linear); // linear path
    // lv_anim_start(&eye_obj->eye_anim);

    eye_start_anim(eye_obj);

    // Eyelid (rectangle, initially hidden)
    eye_obj->eyelid = lv_obj_create(scr);
    lv_obj_set_size(eye_obj->eyelid, 32, 0); // height 0 = open
    lv_obj_set_style_bg_color(eye_obj->eyelid, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(eye_obj->eyelid, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(eye_obj->eyelid, 0, LV_STATE_DEFAULT);
    lv_obj_align(eye_obj->eyelid, LV_ALIGN_TOP_MID, pos_x, pos_y);

    // if (eyelid_out) *eyelid_out = eyelid;
}

static void eye_start_anim(eye_obj_t *eye_obj)
{
    //animate that inner circle moves left right inside eye_border
    lv_anim_init(&eye_obj->eye_anim);
    lv_anim_set_var(&eye_obj->eye_anim, eye_obj->pupil);
    lv_anim_set_exec_cb(&eye_obj->eye_anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&eye_obj->eye_anim, eye_obj->pos_x-8, eye_obj->pos_x + 8); // move from -16 to 16
    lv_anim_set_time(&eye_obj->eye_anim, 2000); // duration of the animation
    lv_anim_set_repeat_count(&eye_obj->eye_anim, LV_ANIM_REPEAT_INFINITE); // repeat infinitely
    lv_anim_set_playback_time(&eye_obj->eye_anim, 2000); // playback time
    lv_anim_set_path_cb(&eye_obj->eye_anim, lv_anim_path_linear); // linear path
    lv_anim_start(&eye_obj->eye_anim);
}
static void eye_stop_anim(eye_obj_t *eye_obj)
{
    lv_anim_del(eye_obj->pupil, (lv_anim_exec_xcb_t)lv_obj_set_x);
}

static void display_smile(lv_disp_t *disp, int pos_x, int pos_y)
{
    // lv_obj_t *scr = lv_disp_get_scr_act(disp);
    // // display smile - arc 64x32
    // lv_obj_t *smile = lv_arc_create(scr);
    // lv_arc_set_angles(smile, 0, 180); // arc from 0 to 180 degrees
    // lv_obj_set_size(smile, 60, 20); // size of the arc
    // lv_obj_set_style_bg_color(smile, lv_color_hex(0x000000), LV_STATE_DEFAULT); // white arc
    // lv_obj_set_style_bg_opa(smile, LV_OPA_COVER, LV_STATE_DEFAULT);
    // lv_obj_set_style_radius(smile, 12, LV_STATE_DEFAULT); // radius for arc
    // lv_obj_align(smile, LV_ALIGN_TOP_MID, pos_x, pos_y);

    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    mouthO_obj = lv_obj_create(scr);
    lv_obj_set_size(mouthO_obj, 14, 14);
    lv_obj_set_style_bg_color(mouthO_obj, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(mouthO_obj, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(mouthO_obj, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(mouthO_obj, 6, LV_STATE_DEFAULT); 
    lv_obj_align(mouthO_obj, LV_ALIGN_TOP_MID, pos_x, pos_y-5);
    //hide the mouthO_obj initially
    lv_obj_set_style_bg_opa(mouthO_obj, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(mouthO_obj, LV_OPA_TRANSP, LV_STATE_DEFAULT);


    // Parameters for the ellipse
    int center_x = pos_x;
    int center_y = pos_y;
    int a = 24; // horizontal radius (width/2)
    int b = 8;  // vertical radius (height/2)
    int start_angle = 20; // degrees
    int end_angle = 180;   // degrees

    // Generate points for the ellipse arc
    static lv_point_t points[36];
    int idx = 0;
    for (int angle = start_angle; angle <= end_angle; angle += 5) {
        float rad = angle * 3.14159f / 180.0f;
        points[idx].x = center_x + (int)(a * cosf(rad));
        points[idx].y = center_y + (int)(b * sinf(rad));
        idx++;
    }

    smile_obj = lv_line_create(scr);
    lv_line_set_points(smile_obj, points, idx);
    lv_obj_set_style_line_width(smile_obj, 3, LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(smile_obj, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_align(smile_obj, LV_ALIGN_TOP_MID, 0, 0);
    // Hide the smile initially
    // lv_obj_set_style_line_opa(smile_obj, LV_OPA_TRANSP, LV_STATE_DEFAULT);
}

static void eyelid_anim_cb(void * obj, int32_t v) {
    lv_obj_set_height((lv_obj_t *)obj, v);
}

static void blink_eyelids(lv_timer_t * timer) {
    // Animate eyelids down (close)
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eye_left_obj.eyelid);
    lv_anim_set_exec_cb(&a, eyelid_anim_cb);
    lv_anim_set_values(&a, 0, 32); // 0=open, 32=closed
    lv_anim_set_time(&a, 120);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, eye_right_obj.eyelid);
    lv_anim_set_exec_cb(&b, eyelid_anim_cb);
    lv_anim_set_values(&b, 0, 32);
    lv_anim_set_time(&b, 120);
    lv_anim_set_path_cb(&b, lv_anim_path_linear);
    lv_anim_start(&b);

    if(++sleep_blink_counter >= SLEEP_BLINK_COUNT) {
        // If sleep blink count reached, don't open eyelids and stop blinking
        sleep_state = true; // Set sleep state
        sleep_blink_counter = 0; // Reset counter
        ESP_LOGI(TAG, "Entering sleep mode after %d blinks", SLEEP_BLINK_COUNT);
        // Stop the blinking timer
        if (blink_timer) {
            lv_timer_del(blink_timer);
            blink_timer = NULL;
        }
    } else {
        ESP_LOGI(TAG, "Blink count: %d", sleep_blink_counter);
        // After short delay, animate eyelids up (open)
        lv_timer_t *open_timer = lv_timer_create_basic();
        lv_timer_set_period(open_timer, 200);
        lv_timer_set_repeat_count(open_timer, 1);
        lv_timer_set_cb(open_timer, open_eyelids_cb);
        // Set next blink to a random period
        if (blink_timer) {
            lv_timer_set_period(blink_timer, get_random_blink_period());
        }
    }

}

static void open_eyelids(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eye_left_obj.eyelid);
    lv_anim_set_exec_cb(&a, eyelid_anim_cb);
    lv_anim_set_values(&a, 32, 0);
    lv_anim_set_time(&a, 120);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, eye_right_obj.eyelid);
    lv_anim_set_exec_cb(&b, eyelid_anim_cb);
    lv_anim_set_values(&b, 32, 0);
    lv_anim_set_time(&b, 120);
    lv_anim_set_path_cb(&b, lv_anim_path_linear);
    lv_anim_start(&b);   
}

static void open_eyelids_cb(lv_timer_t * t) {
    open_eyelids();

    lv_timer_del(t); // delete this one-shot timer
}


static void switch_to_normal_cb(lv_timer_t * t)
{
    demo_mode_enabled = false; // Switch to normal mode
    lv_timer_del(t); // delete this one-shot timer
    if (lvgl_port_lock(0)) {
        stop_demo(current_disp);
        run_normal_mode(current_disp);
        // Release the mutex
        lvgl_port_unlock();
    }
}
