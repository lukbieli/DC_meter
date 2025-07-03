#ifndef CURRENTDRV_H
#define CURRENTDRV_H

#ifdef __cplusplus
extern "C" {
#endif

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

void CurrentDrv_Init(void);
void CurrentDrv_Task(void *pvParameters);
QueueHandle_t CurrentDrv_GetQueue(void);

#ifdef __cplusplus
}
#endif

#endif // CURRENTDRV_H
