#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_wn_models.h"

#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "speech.h"

#define TAG "speech"
#define SPEECH_PARTITION "model"    /*模型存放分区名*/
#define AFE_INPUT_FORMAT "MM"     /*两麦、无参考通道*/
#define SPEECH_WAKENET_NAME "wn10_nihaoxiaozhi"
#define SPEECH_PCM_BLOCK_FRAMES 256
#define SPEECH_PCM_QUEUE_DEPTH 8

static QueueHandle_t s_pcm_q = NULL;
static srmodel_list_t *s_models = NULL;  /*模型分区模型列表*/
static const esp_afe_sr_iface_t *s_afe = NULL; /*AFE框架接口*/
static esp_afe_sr_data_t *s_afe_data = NULL;/*AFE框架实例*/

static int s_feed_chunk = 0;  /*每次feed的点数*/
static int s_feed_channels = 0; /*feed的通道数(双通道立体声)*/
static int s_fetch_chunk = 0; /*每次fetch输出的点数*/
static int s_fetch_per_feed = 0;/*匹配值*/

static int16_t *s_accum = NULL; /*攒帧缓冲区*/
static size_t s_accum_frame = 0; /*已攒帧数*/
static int16_t *s_out = NULL; /*最近一帧输出*/
static size_t s_out_frames = 0;

static char *speech_pick_wakenet(srmodel_list_t* models)
{
    for(int i = 0; i < models->num; i++)
        if(strcmp(models->model_name[i], SPEECH_WAKENET_NAME) == 0)
            return models->model_name[i];
    ESP_LOGW(TAG, "模型包里没有 '%s'，退回第一个唤醒词模型", SPEECH_WAKENET_NAME);
    return esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
}

static void speech_task(void *arg)
{
    int16_t buf[SPEECH_PCM_BLOCK_FRAMES * 2];
    uint32_t cnt = 0;
    while(1)
    {   
        if(xQueueReceive(s_pcm_q, buf, portMAX_DELAY) != pdTRUE)
            continue;

        size_t copied = 0;
        while(copied < SPEECH_PCM_BLOCK_FRAMES)
        {
            size_t need = (size_t)s_feed_chunk - s_accum_frame;
            size_t take = (SPEECH_PCM_BLOCK_FRAMES - copied) < need 
            ?(SPEECH_PCM_BLOCK_FRAMES - copied) 
            :need;

            memcpy(&s_accum[s_accum_frame * s_feed_channels],
                &buf[copied * 2],
                take * 2 * sizeof(int16_t));

            s_accum_frame += take;
            copied += take;

            if(s_accum_frame < (size_t)s_feed_chunk)
                continue;
            
            s_accum_frame = 0;
            s_afe->feed(s_afe_data, s_accum);


            
            for(size_t i = 0; i < (size_t)s_fetch_per_feed; i++)
            {
                afe_fetch_result_t *res = s_afe->fetch(s_afe_data);

                if(res == NULL)
                {
                    ESP_LOGW(TAG, "fetch返回kong");
                    continue;
                }

                if(res->data != NULL && res->data_size > 0)
                {
                    size_t n = (size_t)res->data_size / sizeof(int16_t);
                    if(n > (size_t)s_fetch_chunk)
                    {
                        n = (size_t)s_fetch_chunk;
                    }
                    
                    memcpy(s_out, res->data, n * sizeof(int16_t));
                    s_out_frames = n;
                }

                if(res->wakeup_state == WAKENET_DETECTED)
                {
                    ESP_LOGI(TAG, "唤醒命中:word = %d , model = %d, len = %d",
                            res->wake_word_index,
                            res->wakenet_model_index,
                            res->wake_word_length);
                }


            }

        }

        
        
        if((++cnt % 50) == 0)
            ESP_LOGI(TAG, "收到 %u 块", (unsigned)cnt);
    }
}


esp_err_t speech_init(void)
{
    /*获取剩余PSRAM堆空间*/
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /*挂载模型分区*/
    s_models = esp_srmodel_init(SPEECH_PARTITION);
    if(s_models == NULL)
    {
        ESP_LOGE(TAG, "模型分区'%s'挂载失败", SPEECH_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGE(TAG, "模型分区 '%s' 共有 '%d' 个模型", 
            SPEECH_PARTITION, 
            s_models->num);
    for(int i = 0; i < s_models->num; i++)
    {
        ESP_LOGE(TAG, "[%d]%s", i, s_models->model_name[i]);
    }

    /*生成默认配置*/
    afe_config_t *cfg = afe_config_init(AFE_INPUT_FORMAT,
                                        s_models, 
                                        AFE_TYPE_SR, 
                                        AFE_MODE_LOW_COST);             
    if(cfg == NULL)
    {
        ESP_LOGE(TAG, "afe_config_init 失 败");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    /*按本项目需求覆盖默认值*/
    cfg->aec_init  = false;  /*半双工*/
    cfg->se_init = true;  /*双麦增强*/
    cfg->ns_init = false;  /*降噪*/
    cfg->vad_init = true;   /*vad_model_name使用NULL,使用webRTC VAD*/
    cfg->wakenet_init = true;
    cfg->afe_perferred_core = 1; /*AFE内部任务绑定1*/
    cfg->afe_perferred_priority = 5; /*任务优先级*/
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    cfg->afe_ringbuf_size = 8;
    cfg->wakenet_model_name = speech_pick_wakenet(s_models); /*按名字挑模型 */

    if(cfg->wakenet_model_name == NULL)
    {
        ESP_LOGI(TAG, "模型分区找不到唤醒词模型");
        afe_config_free(cfg);
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    /*创建AFE实例*/
    s_afe = esp_afe_handle_from_config(cfg);
    if(s_afe == NULL)
    {
        ESP_LOGE(TAG, "esp_afe_handle_from_config 失败");
        afe_config_free(cfg);
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    
    s_afe_data = s_afe->create_from_config(cfg);
    if(s_afe_data == NULL)
    {
        ESP_LOGE(TAG, "AFE实例创建失败(PSRAM不足)");
        afe_config_free(cfg);
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    afe_config_free(cfg);
    cfg = NULL;

    /*查参数，分配缓冲*/
    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    s_feed_channels = s_afe->get_feed_channel_num(s_afe_data);
    s_fetch_chunk = s_afe->get_fetch_chunksize(s_afe_data);
    s_fetch_per_feed = (s_feed_chunk + s_fetch_chunk - 1) / s_fetch_chunk; 


    s_accum = (int16_t *)calloc(1, sizeof(int16_t) * s_feed_chunk * s_feed_channels);
    s_out = (int16_t *)calloc(1, sizeof(int16_t) * s_fetch_chunk);

    if(s_accum == NULL || s_out == NULL)
    {
        ESP_LOGE(TAG, "音频缓冲分配失败");
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
        afe_config_free(cfg);
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    /*打印实际生效的流水线*/
    s_afe->print_pipeline(s_afe_data);

    ESP_LOGI(TAG, "feed=%d 点/通道 * %d 通道， fetch = %d 点，采样率 = %dHz", 
                s_feed_chunk, 
                s_feed_channels, 
                s_fetch_chunk, 
                s_afe->get_samp_rate(s_afe_data));
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM: %u KB -> %u KB(AFE占用 %u KB)",
            (unsigned)(psram_before / 1024),
            (unsigned)(psram_after /1024),
            (unsigned)((psram_before - psram_after) /1024));

    return ESP_OK;
}

const int16_t *speech_last_frame(size_t *frames)
{
    if(frames != NULL)
    {
        *frames = s_out_frames;
    }
    if(s_out == NULL || s_out_frames == 0)
    {
        return NULL;
    }
    return s_out;
}

void speech_set_wakenet(bool on)
{

    if(s_afe == NULL || s_afe_data == NULL)
    {
        return;
    }

    //xQueueSend(s_pcm_q, s_accum, 0);

    if(on)
    {
        s_accum_frame = 0;
        s_afe->reset_buffer(s_afe_data);
        s_afe->enable_wakenet(s_afe_data);
        ESP_LOGI(TAG, "唤醒已开启");

    }
    else
    {
        s_afe->disable_wakenet(s_afe_data);
        ESP_LOGI(TAG, "唤醒已关闭（半双工）");
    }
}

/*创建PCM 队列，捆绑speech任务到内核1，优先级4*/
esp_err_t speech_start(void)
{
    s_pcm_q = xQueueCreate(SPEECH_PCM_QUEUE_DEPTH,
                            SPEECH_PCM_BLOCK_FRAMES * 2 * sizeof(int16_t));
    if(!s_pcm_q)
    {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(speech_task, "speech", 8192, NULL, 4, NULL, 1);
    return ESP_OK;
}

/*从PCM队列取出frames帧，存放到pcm_stereo*/
esp_err_t speech_push_pcm(const int16_t *pcm_stereo, size_t frames)
{
    if(!s_pcm_q || frames != SPEECH_PCM_BLOCK_FRAMES)
        return ESP_ERR_INVALID_ARG;
    return (xQueueSend(s_pcm_q, pcm_stereo, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}