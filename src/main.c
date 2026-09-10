#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_inmp441.h"

#include "audio_app.h"
#include "vad.h"
void app_main() 
{
    mic_init(); // 初始化I2S

    xTaskCreate(audio_app_task, "audio_app_task", 4096, NULL, 5, NULL); //创建解码任务

}