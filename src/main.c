#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "mic_inmp441.h"
#include "audio_segment.h"
#include "audio_app.h"
#include "vad.h"
void app_main() 
{
    mic_init(); // 初始化I2S
    ESP_ERROR_CHECK(audio_segment_init(16000, 5, 3));
    xTaskCreate(audio_app_task, "audio_app_task", 4096, NULL, 5, NULL); //创建解码任务
    xTaskCreate(seg_consumer_task, "seg_consumer", 4096, NULL, 4, NULL);
}