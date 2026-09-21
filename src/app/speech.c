#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"

#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "speech.h"
#include "audio_out.h"
#include "mic_inmp441.h"
#include "audio_segment.h"

static srmodel_list_t *s_models = NULL;  /*模型分区模型列表*/
static const esp_afe_sr_iface_t *s_afe = NULL; /*AFE框架接口*/
static esp_afe_sr_data_t *s_afe_data = NULL;/*AFE框架实例*/
static esp_mn_iface_t *s_mn = NULL;/*MultiNet接口*/
static model_iface_data_t *s_mn_data = NULL;/*MultiNet实例*/

static int64_t s_cmd_deadline_us = 0; /*">0"表示处于命令窗口*/
static int64_t s_last_wake_us = 0; /*设备启动时间 */
static int s_feed_chunk = 0;  /*每次feed的点数*/
static int s_feed_channels = 0; /*feed的通道数(双通道立体声)*/
static int s_fetch_chunk = 0; /*每次fetch输出的点数*/
static int s_fetch_per_feed = 0;/*匹配值*/
static int mn_chunk = 0;

static char *speech_pick_wakenet(srmodel_list_t* models)
{
    for(int i = 0; i < models->num; i++)
        if(strcmp(models->model_name[i], SPEECH_WAKENET_NAME) == 0)
            return models->model_name[i];
    ESP_LOGW(TAG, "模型包里没有 '%s'，退回第一个唤醒词模型", SPEECH_WAKENET_NAME);
    return esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
}

/*将PCM缓冲区内容喂给AFE框架*/
esp_err_t speech_feed_pcm(const int16_t *pcm, size_t frames)
{
    if (s_afe == NULL || s_afe_data == NULL) return ESP_ERR_INVALID_STATE;
    /*将PCM喂给AFE框架*/
    s_afe->feed(s_afe_data, pcm);
    return ESP_OK;
}

void speech_fetch_task(void *arg)
{
    while(1)
    {   
        /*从AFE中获取结果*/
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if(res == NULL)
            continue;

        if(res->wakeup_state == WAKENET_DETECTED)
        {
            int64_t now = esp_timer_get_time();
            if(now - s_last_wake_us > 2000000)
            {
                s_last_wake_us = now;
                speech_set_wakenet(false);
                audio_out_play_file("/spiffs/wake.pcm");

                s_mn->clean(s_mn_data);
                s_cmd_deadline_us = esp_timer_get_time() + 5000 * 1000;
            }
            
            ESP_LOGI(TAG, "唤醒命中:word = %d , model = %d, len = %d",
                    res->wake_word_index,
                    res->wakenet_model_index,
                    res->wake_word_length);
        }
            /*创建命令窗口(5s)*/
        if(s_cmd_deadline_us > 0)
        {
            if(esp_timer_get_time() > s_cmd_deadline_us)
            {
                ESP_LOGI(TAG, "命令窗口超时,未识别到命令");
                s_cmd_deadline_us = 0;
                speech_set_wakenet(true);
            }
            else if(res->data != NULL && res->data_size > 0)
            {
                /*MultiNet识别*/
                esp_mn_state_t st = s_mn->detect(s_mn_data, res->data);
                if(st == ESP_MN_STATE_DETECTED)
                {
                    esp_mn_results_t *r = s_mn->get_results(s_mn_data);
                    
                    ESP_LOGI(TAG, "识别到命令: id = %d, 文本 = %s",
                            r->command_id[0],
                            r->string);
                    s_mn->clean(s_mn_data);  /*清理状态 */
                    s_cmd_deadline_us = 0;
                    audio_segment_mark_cmd(r->command_id[0]);
                    speech_set_wakenet(true);
                }
            }

        }
        
        if(res->data && res->data_size > 0)
        {
            bool is_speech = (res->vad_state != VAD_SILENCE);
            audio_segment_feed(res->data,
                            res->data_size / sizeof(int16_t),
                            is_speech,
                            (const int16_t *)res->vad_cache,     
                            res->vad_cache_size / sizeof(int16_t)); 
        }
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
    cfg->wakenet_model_name_2 = NULL; 


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

    /*创建multinet*/
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if(mn_name == NULL)
    {
        ESP_LOGE(TAG, "模型包里面没有中文命令词！");
        return ESP_FAIL;
    }

    s_mn = esp_mn_handle_from_name(mn_name);
    if(s_mn == NULL)
    {
        ESP_LOGE(TAG, "multinet 句柄获取失败");
        return ESP_FAIL;
    }

    s_mn_data = s_mn->create(mn_name, 5000);
    if(s_mn_data == NULL)
    {
        ESP_LOGE(TAG, "multinet 实例创建失败(PSRAM)");
        return ESP_FAIL;
    }

    mn_chunk = s_mn->get_samp_chunksize(s_mn_data);
    ESP_LOGI(TAG, "MultiNet: %s, 每次 %d 点, 采样率 %d Hz",
            mn_name,
            mn_chunk,
            s_mn->get_samp_rate(s_mn_data));

    esp_mn_commands_alloc(s_mn, s_mn_data);
    esp_mn_commands_add(1, "da kai dian deng");
    esp_mn_commands_add(2, "da kai fang men");
    esp_mn_error_t *err = esp_mn_commands_update(); 
    if(err != NULL && err->num > 0)
    {
        for(int i = 0; i < err->num; i++)
        {
            ESP_LOGE(TAG, "命令词未加入(拼音有问题): %s", err->phrases[i]->string);
        }
    }
    esp_mn_commands_print();

    /*查参数，分配缓冲*/
    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    s_feed_channels = s_afe->get_feed_channel_num(s_afe_data);
    s_fetch_chunk = s_afe->get_fetch_chunksize(s_afe_data);
    s_fetch_per_feed = (s_feed_chunk + s_fetch_chunk - 1) / s_fetch_chunk; 

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

void speech_set_wakenet(bool on)
{

    if(s_afe == NULL || s_afe_data == NULL)
    {
        return;
    }

    if(on)
    {
        s_afe->enable_wakenet(s_afe_data);
        ESP_LOGI(TAG, "唤醒已开启");

    }
    else
    {
        s_afe->disable_wakenet(s_afe_data);
        s_afe->reset_buffer(s_afe_data);
        ESP_LOGI(TAG, "唤醒已关闭（半双工）");
    }
}

