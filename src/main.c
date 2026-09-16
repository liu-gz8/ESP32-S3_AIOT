#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "mic_inmp441.h"
#include "audio_segment.h"
#include "audio_app.h"
#include "vad.h"
#include "audio_out.h"
#include "storage.h"
#include "speech.h"

void app_main(void)
{
    mic_init();
    audio_out_init();
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(audio_segment_init(16000, 5, 3));

    /*开机提示音:此时 AFE 还没起来,不用担心它听到提示音*/
    audio_out_play_file("/spiffs/wake.pcm");

    /*AFE + WakeNet(约占用 1~2MB PSRAM,失败会打印错误并返回)*/
    ESP_ERROR_CHECK(speech_init());

    xTaskCreate(audio_app_task, "audio_app_task", 4096, NULL, 5, NULL);
    xTaskCreate(seg_consumer_task, "seg_consumer", 4096, NULL, 4, NULL);
}
