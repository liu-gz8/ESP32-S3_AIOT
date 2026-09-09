/*麦克风应用层*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_app.h"
#include "audio_format.h"
#include "mic_inmp441.h"

//打印 解码 音频 统计
void audio_app_task(void *arg)
{
    int32_t *raw = (int32_t *)calloc(1, RX_BUFFER_BYTES);
    int16_t *pcm = (int16_t *)calloc(1, RX_PCM_BUFFER_BYTES);
    int printf_cnt = 0;
    while(1)
    {
        if(mic_read_frame(raw, RX_BUFFER_BYTES) != ESP_OK)
        {
            continue;
        }
        
        audio_convert_to_s16(raw, RX_FRAME_COUNT, pcm);
        block_rms_t st = audio_rms(pcm, RX_FRAME_COUNT);
        
        if(++printf_cnt % 10 == 0)
        printf("L %.2f  R %.2f\n",st.rms_l, st.rms_r);
    }
    
    free(raw);
    free(pcm);

}

