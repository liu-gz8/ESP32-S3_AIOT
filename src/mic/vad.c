#include "vad.h"
#include "mic_inmp441.h"
#include <math.h>

void vad_init(vad_t *vad)
{
    vad->noise_db = VAD_NOISE_FLOOR_INIT_DB;
    vad->state = VAD_STATE_SILENCE;
    vad->hit_cnt = 0;
    vad->low_cnt = 0;
    vad->voice_db = 0;
    vad->speech_frames = 0;
}

vad_enent_t vad_process(vad_t *vad, float rms_l, float rms_r)
{
    vad->voice_db = fmax((20 * log10(rms_l + 1)),
                        (20 * log10(rms_r + 1)));
    vad_enent_t vad_event = VAD_EVENT_NONE;
    bool above_start = (vad->voice_db >= vad->noise_db + VAD_THRESHOLD);
    bool above_keep = (vad->voice_db >= vad->noise_db + VAD_THRESHOLD / 2);

    if(vad->state == VAD_STATE_SILENCE)
    {
        if(above_start)
        {
            if(++vad->hit_cnt >= VAD_ONSET_FRAME)
            {
               
                vad->state = VAD_STATE_SPEEK;
                vad->hit_cnt = 0;
                vad->low_cnt = 0;
                vad_event = VAD_EVENT_START;
            }
            else
            {
                vad->speech_frames++;
            }
        }
        else
        {
            //1.连续断帧
            //.2.动态更新底噪
            vad->speech_frames = 0;
            vad->hit_cnt = 0;
            vad->noise_db = 
            VAD_NOISE_ALPHA * vad->noise_db + (1 - VAD_NOISE_ALPHA) * vad->voice_db;
        }
    }
    else //说话
    {
        if(above_keep)  //有声
        {
            vad->speech_frames++;
            vad->low_cnt = 0;
        }
        else if(++vad->low_cnt >= VAD_OFFSET_FRAME) //结束帧判定
        {
            vad->state = VAD_STATE_SILENCE;
            vad->low_cnt = 0;
            vad->hit_cnt = 0;
            if(vad->speech_frames >= VAD_MIN_SPEECH_FRAMES)
            {
                vad_event = VAD_EVENT_END;
                vad->speech_frames = 0;
            }
            else
            {
                vad_event = VAD_EVENT_NONE;
            }    
        }
        else
        {
            vad->speech_frames++;
        }
    }

    //超时返回结束帧
    if(vad->speech_frames >= VAD_MAX_SPEECH_FRAMES)
    {
        vad->state = VAD_STATE_SILENCE;
        vad->hit_cnt = 0;
        vad->low_cnt = 0;
        vad->speech_frames = 0;
        vad_event = VAD_EVENT_END;
    }
    return vad_event;
}
