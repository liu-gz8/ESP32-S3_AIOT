#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "mic_inmp441.h"
#include "audio_segment.h"
#include "audio_app.h"
#include "vad.h"
#include "audio_out.h"
#include "storage.h"

void app_main() 
{
    mic_init();             
    audio_out_init();
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(audio_segment_init(16000, 5, 3));   
    audio_out_play_file("/spiffs/wake.pcm");
    xTaskCreate(audio_app_task, "audio_app_task", 4096, NULL, 5, NULL);
    xTaskCreate(seg_consumer_task, "seg_consumer", 4096, NULL, 4, NULL);
}
