#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_inmp441.h"

void app_main() 
{
    i2s_init_std_simplex(); // 初始化I2S

    xTaskCreate(i2s_read_task, "i2s_read_task", 4096, NULL, 5, NULL); // 创建I2S读取任务
    xTaskCreate(i2s_write_task, "i2s_write_task", 4096, NULL, 5, NULL); // 创建I2S写入任务

}