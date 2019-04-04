/* baidu translation device example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_http_client.h"
#include "sdkconfig.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_hal.h"
#include "esp_peripherals.h"
#include "periph_button.h"
#include "periph_wifi.h"
#include "periph_led.h"

#include "baidu_asr.h"
#include "board.h"
#include "filter_resample.h"
#include "zl38063.h"

static const char *TAG = "BAIDU_TRANSLATION_EXAMPLE";

#define EXAMPLE_RECORD_PLAYBACK_SAMPLE_RATE (16000)

esp_periph_handle_t led_handle = NULL;
void baidu_asr_begin(baidu_asr_handle_t asr)
{
    // if (led_handle) {
    //     periph_led_blink(led_handle, GPIO_LED_GREEN, 500, 500, true, -1);
    // }
    ESP_LOGW(TAG, "Start speaking now");
}

void asr_task(void *pv)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    tcpip_adapter_init();

    ESP_LOGI(TAG, "[ 1 ] Initialize Buttons & Connect to Wi-Fi network, ssid=%s", CONFIG_WIFI_SSID);
    // Initialize peripherals management
    esp_periph_config_t periph_cfg = { 0 };
    esp_periph_init(&periph_cfg);

    periph_wifi_cfg_t wifi_cfg = {
        .ssid = CONFIG_WIFI_SSID,
        .password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);

    // Initialize Button peripheral
    periph_button_cfg_t btn_cfg = {
        .gpio_mask = GPIO_SEL_0,
    };
    esp_periph_handle_t button_handle = periph_button_init(&btn_cfg);

    // periph_led_cfg_t led_cfg = {
    //     .led_speed_mode = LEDC_LOW_SPEED_MODE,
    //     .led_duty_resolution = LEDC_TIMER_10_BIT,
    //     .led_timer_num = LEDC_TIMER_0,
    //     .led_freq_hz = 5000,
    // };
    // led_handle = periph_led_init(&led_cfg);

    // Start wifi & button peripheral
    esp_periph_start(button_handle);
    esp_periph_start(wifi_handle);
    // esp_periph_start(led_handle);

    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
#if (CONFIG_ESP_LYRAT_V4_3_BOARD || CONFIG_ESP_LYRAT_V4_2_BOARD)
    audio_hal_codec_config_t audio_hal_codec_cfg = AUDIO_HAL_ES8388_DEFAULT();
    audio_hal_codec_cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
    audio_hal_handle_t hal = audio_hal_init(&audio_hal_codec_cfg, 0);
#endif
#if (CONFIG_ESP_LYRATD_MSC_V2_1_BOARD || CONFIG_ESP_LYRATD_MSC_V2_2_BOARD)
    audio_hal_codec_config_t audio_hal_codec_cfg = AUDIO_HAL_ZL38063_DEFAULT();
    audio_hal_codec_cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
    audio_hal_handle_t hal = audio_hal_init(&audio_hal_codec_cfg, 2);
#endif
    audio_hal_ctrl_codec(hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    baidu_asr_config_t asr_config = {
        .access_key = CONFIG_BAIDU_ACCESS_KEY,
        .secret_key = CONFIG_BAIDU_SECRET_KEY,
        .format = "pcm",
        .record_sample_rates = 16000,
        .channel = 1,
        .cuid = "ESP32",
        .dev_pid = 1536,
        .on_begin = baidu_asr_begin,
    };
    baidu_asr_handle_t asr = baidu_asr_init(&asr_config);

    ESP_LOGI(TAG, "[ 4 ] Setup event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from the pipeline");
    baidu_asr_set_listener(asr, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_get_event_iface(), evt);

    ESP_LOGI(TAG, "[ 5 ] Listen for all pipeline events");
    while (1) 
    {
        audio_event_iface_msg_t msg;
        if (audio_event_iface_listen(evt, &msg, portMAX_DELAY) != ESP_OK) 
        {
            ESP_LOGW(TAG, "[ * ] Event process failed: src_type:%d, source:%p cmd:%d, data:%p, data_len:%d",
                 msg.source_type, msg.source, msg.cmd, msg.data, msg.data_len);
            continue;
        }

        ESP_LOGI(TAG, "[ * ] Event received: src_type:%d, source:%p cmd:%d, data:%p, data_len:%d",
                 msg.source_type, msg.source, msg.cmd, msg.data, msg.data_len);


        if (msg.source_type != PERIPH_ID_BUTTON) {
            continue;
        }

        // // It's MODE button
        // if ((int)msg.data == GPIO_MODE) {
        //     break;
        // }

        // if ((int)msg.data != GPIO_REC) {
        //     continue;
        // }

        if (msg.cmd == PERIPH_BUTTON_PRESSED) 
        {
            ESP_LOGI(TAG, "[ * ] Resuming pipeline");
            baidu_asr_start(asr);
        } 
        else if (msg.cmd == PERIPH_BUTTON_RELEASE || msg.cmd == PERIPH_BUTTON_LONG_RELEASE) 
        {
            ESP_LOGI(TAG, "[ * ] Stop pipeline");

            char *original_text = baidu_asr_stop(asr);
            if (original_text == NULL) {
                continue;
            }
            ESP_LOGI(TAG, "Original text = %s", original_text);
        }

    }
    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
    baidu_asr_destroy(asr);

    /* Stop all periph before removing the listener */
    esp_periph_stop_all();
    audio_event_iface_remove_listener(esp_periph_get_event_iface(), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);
    esp_periph_destroy();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    xTaskCreate(asr_task, "asr_task", 2 * 4096, NULL, 5, NULL);
}
