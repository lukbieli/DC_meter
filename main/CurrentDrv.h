#ifndef CURRENTDRV_H
#define CURRENTDRV_H

#ifdef __cplusplus
extern "C" {
#endif


#include <freertos/queue.h>
#include <esp_timer.h>
#include "ina219.h"

//pragma pack aligned to 1 byte struct with ina results
#pragma pack(1)
typedef struct {
    float voltage;      //!< Bus voltage, V
    float current;          //!< Current, A
    float power;            //!< Power, W
    float shunt_voltage;    //!< Shunt voltage, V
} ina219_data_raw_t;

typedef struct{
    ina219_data_raw_t ch1;
    ina219_data_raw_t ch2;
    ina219_data_raw_t ch3;
}ina219_full_data_t;
#pragma pack()

typedef struct CurrentDrv_ChannelCfg {
    bool voltage_enabled;  //!< Enable voltage measurement
    bool current_enabled;  //!< Enable current measurement
    bool power_enabled;    //!< Enable power measurement
    ina219_gain_t gain;  //!< Gain setting for current measurement
    ina219_bus_voltage_range_t bus_voltage_range;  //!< Bus voltage range setting
    ina219_resolution_t bus_voltage_resolution;  //!< Bus voltage resolution setting
    ina219_resolution_t shunt_voltage_resolution;  //!< Shunt voltage resolution setting
    uint8_t i2c_addr;  //!< I2C address of the INA219 device
} CurrentDrv_ChannelCfg_t;

typedef struct CurrentDrv_Cfg {
    CurrentDrv_ChannelCfg_t ch1;  //!< Channel 1 configuration
    CurrentDrv_ChannelCfg_t ch2;  //!< Channel 2 configuration
    CurrentDrv_ChannelCfg_t ch3;  //!< Channel 3 configuration
} CurrentDrv_Config_t;


void CurrentDrv_Init(void);
void CurrentDrv_Task(void *pvParameters);
QueueHandle_t* CurrentDrv_GetQueue(void);
esp_timer_handle_t* CurrentDrv_GetPeriodicTimer(void);
void CurrentDrv_SetChannelConfig(CurrentDrv_Config_t *config);
void CurrentDrv_GetChannelConfig(CurrentDrv_Config_t* config);
void CurrentDrv_SetTimerPeriod(uint32_t period_us);
uint32_t CurrentDrv_GetTimerPeriod(void);
void CurrentDrv_StopTimer(void);
void CurrentDrv_StartTimer(bool auto_cfg_en);

#ifdef __cplusplus
}
#endif

#endif // CURRENTDRV_H
