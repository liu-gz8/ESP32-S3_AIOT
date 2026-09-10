/*麦克风应用层*/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_app.h"
#include "audio_format.h"
#include "mic_inmp441.h"
#include "vad.h"

//打印 解码 音频 统计
void audio_app_task(void *arg)
{
    static uint32_t block_id = 0;   /* 循环外定义 */
    
    int32_t *raw = (int32_t *)calloc(1, RX_BUFFER_BYTES);
    int16_t *pcm = (int16_t *)calloc(1, RX_PCM_BUFFER_BYTES);
    vad_enent_t vad_event = 0;
    block_rms_t st = {};
    //int printf_cnt = 0;

    vad_t* vad = (vad_t *)malloc(sizeof(vad_t));
    assert(vad);
    vad_init(vad);

    
    while(1)
    {
        if(mic_read_frame(raw, RX_BUFFER_BYTES) != ESP_OK)
        {
            continue;
        }

        audio_convert_to_s16(raw, RX_FRAME_COUNT, pcm);
        st = audio_rms(pcm, RX_FRAME_COUNT);
        vad_event = vad_process(vad, st.rms_l, st.rms_r);
        //6.25fps
        //if(++printf_cnt % 10 == 0)
        block_id++;
        printf("blk=%lu state=%d db=%.1f floor=%.1f event=%d frame=%ld\n",
            (unsigned long)block_id, vad->state, vad->voice_db,
            vad->noise_db, vad_event, vad->speech_frames);
    }
    
    free(raw);
    free(pcm);
    free(vad);
    vTaskDelete(NULL);
}

