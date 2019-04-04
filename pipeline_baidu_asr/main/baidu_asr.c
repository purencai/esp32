/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mbedtls/base64.h"

#include "esp_http_client.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_hal.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "baidu_asr.h"

#include "json_utils.h"
#include "baidu_access_token.h"

static const char *TAG = "baidu_asr";

#define BAIDU_ASR_ENDPOINT         "http://vop.baidu.com/server_api?"
#define BAIDU_ASR_BEGIN            "{\"dev_pid\":%d,\"rate\":%d,\"speech\":\""
#define BAIDU_ASR_END              "\",\"len\":%d,\"format\":\"%s\",\"cuid\":\"%s\",\"token\":\"%s\",\"channel\":%d}"
#define BAIDU_ASR_TASK_STACK       (8*1024)

typedef struct baidu_asr {
    audio_pipeline_handle_t pipeline;
    int                     remain_len;
    int                    sr_total_raw;
    int                     sr_total_write;
    bool                    is_begin;
    char                    *buffer;
    char                    *b64_buffer;
    int                     buffer_size;
    audio_element_handle_t  i2s_reader;
    audio_element_handle_t  http_stream_writer;
    char                    *access_key;
    char                    *secret_key;
    char                    *format;                       /*!< Speech-to-Text language code */
    int                     record_sample_rates;            /*!< Audio recording sample rate */
    int                     channel;                        /*!< Audio encoding */
    char                    *cuid;                           /*!< Processing buffer size */
    char                    *token;
    int                     dev_pid;
    char                    *lan;
    char                    *speech;
    int                     len;
    char                    *response_text;
    baidu_asr_event_handle_t on_begin;
} baidu_asr_t;


static int _http_write_chunk(esp_http_client_handle_t http, const char *buffer, int len)
{
    char header_chunk_buffer[16];
    int header_chunk_len = sprintf(header_chunk_buffer, "%x\r\n", len);
    if (esp_http_client_write(http, header_chunk_buffer, header_chunk_len) <= 0) {
        return ESP_FAIL;
    }
    int write_len = esp_http_client_write(http, buffer, len);
    if (write_len <= 0) {
        ESP_LOGE(TAG, "Error write chunked content");
        return ESP_FAIL;
    }
    if (esp_http_client_write(http, "\r\n", 2) <= 0) {
        return ESP_FAIL;
    }
    return write_len;
}

static esp_err_t _http_stream_writer_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    baidu_asr_t *asr = (baidu_asr_t *)msg->user_data;

    int write_len;
    size_t need_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) 
    {

        if (asr->token == NULL) 
        {
            // Must freed `baidu_access_token` after used
            asr->token = baidu_get_access_token(asr->access_key, asr->secret_key);
        }

        if (asr->token == NULL) 
        {
            ESP_LOGE(TAG, "Error issuing access token");
            return ESP_FAIL;
        }
        
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d", msg->buffer_len);
        asr->sr_total_write = 0;
        asr->sr_total_raw = 0;
        asr->is_begin = true;
        asr->remain_len = 0;
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        esp_http_client_set_post_field(http, NULL, -1); // Chunk content
        esp_http_client_set_header(http, "Content-Type", "application/json");
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST) 
    {
        // ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_ON_REQUEST, lenght=%d, begin=%d", msg->buffer_len, sr->is_begin);
        /* Write first chunk */
        if (asr->is_begin) 
        {
            asr->is_begin = false;

           
            int asr_begin_len = snprintf(asr->buffer, asr->buffer_size, BAIDU_ASR_BEGIN, asr->dev_pid, asr->record_sample_rates ) ;
            if (asr->on_begin) 
            {
                asr->on_begin(asr);
            }
            ESP_LOGI(TAG, "%s", asr->buffer );
            return _http_write_chunk(http, asr->buffer, asr_begin_len);
        }

        if (msg->buffer_len > asr->buffer_size * 3 / 2) 
        {
            ESP_LOGE(TAG, "Please use SR Buffer size greeter than %d", msg->buffer_len * 3 / 2);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "token %d", msg->buffer_len );

        /* Write b64 audio data */
        memcpy(asr->buffer + asr->remain_len, msg->buffer, msg->buffer_len);
        asr->remain_len += msg->buffer_len;
        asr->sr_total_raw += msg->buffer_len;
        int keep_next_time = asr->remain_len % 3;

        asr->remain_len -= keep_next_time;
        if (mbedtls_base64_encode((unsigned char *)asr->b64_buffer, asr->buffer_size,  &need_write, (unsigned char *)asr->buffer, asr->remain_len) != 0) 
        {
            ESP_LOGE(TAG, "Error encode b64");
            return ESP_FAIL;
        }
        if (keep_next_time > 0) 
        {
            memcpy(asr->buffer, asr->buffer + asr->remain_len, keep_next_time);
        }
        asr->remain_len = keep_next_time;
        ESP_LOGI(TAG, "\033[A\33[2K\rTotal bytes written: %d", asr->sr_total_write);
        // ESP_LOGI(TAG, "%s", asr->b64_buffer );
        write_len = _http_write_chunk(http, (const char *)asr->b64_buffer, need_write);
        if (write_len <= 0) 
        {
            return write_len;
        }
        asr->sr_total_write += write_len;
        return write_len;
    }

    /* Write End chunk */
    if (msg->event_id == HTTP_STREAM_POST_REQUEST) 
    {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
        need_write = 0;
        if (asr->remain_len) 
        {
            if (mbedtls_base64_encode((unsigned char *)asr->b64_buffer, asr->buffer_size,  &need_write, (unsigned char *)asr->buffer, asr->remain_len) != 0) 
            {
                ESP_LOGE(TAG, "Error encode b64");
                return ESP_FAIL;
            }
            //ESP_LOGI(TAG, "%s", asr->b64_buffer );
            write_len = _http_write_chunk(http, (const char *)asr->b64_buffer, need_write);
            if (write_len <= 0) 
            {
                return write_len;
            }
        }
 //int asr_begin_len = snprintf(asr->buffer, asr->buffer_size, BAIDU_ASR_BEGIN, asr->format, asr->record_sample_rates, asr->channel, asr->cuid, asr->token);
        write_len = snprintf(asr->buffer, asr->buffer_size, BAIDU_ASR_END, asr->sr_total_raw , asr->format, asr->cuid, asr->token, asr->channel ) ;
        ESP_LOGI(TAG, "%s", asr->buffer );
        write_len = _http_write_chunk(http, asr->buffer, write_len);
        if (write_len <= 0) {
            return ESP_FAIL;
        }
        /* Finish chunked */
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0) 
        {
            return ESP_FAIL;
        }
        return write_len;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) 
    {

        int read_len = esp_http_client_read(http, (char *)asr->buffer, asr->buffer_size);
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST, read_len=%d", read_len);
        if (read_len <= 0) 
        {
            return ESP_FAIL;
        }
        if (read_len > asr->buffer_size - 1) 
        {
            read_len = asr->buffer_size - 1;
        }
        asr->buffer[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char *)asr->buffer);
        if (asr->response_text) 
        {
            free(asr->response_text);
        }
        asr->response_text = json_get_token_value(asr->buffer, "result");
        return ESP_OK;
    }
    return ESP_OK;
}

baidu_asr_handle_t baidu_asr_init(baidu_asr_config_t *config)
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    baidu_asr_t *asr = calloc(1, sizeof(baidu_asr_t));
    AUDIO_MEM_CHECK(TAG, asr, return NULL);
    asr->pipeline = audio_pipeline_init(&pipeline_cfg);

    asr->buffer_size = config->buffer_size;
    if (asr->buffer_size <= 0) {
        asr->buffer_size = DEFAULT_SR_BUFFER_SIZE;
    }
    asr->record_sample_rates = config->record_sample_rates;
    if (asr->record_sample_rates <= 0) {
        asr->record_sample_rates = 16000;
    }
    asr->channel = config->channel;
    if (asr->channel <= 0) {
        asr->channel = 1;
    }
    asr->dev_pid = config->dev_pid;

    asr->buffer = malloc(asr->buffer_size);
    AUDIO_MEM_CHECK(TAG, asr->buffer, goto exit_asr_init);
    asr->b64_buffer = malloc(asr->buffer_size);
    AUDIO_MEM_CHECK(TAG, asr->b64_buffer, goto exit_asr_init);
    asr->access_key = strdup(config->access_key);
    AUDIO_MEM_CHECK(TAG, asr->access_key, goto exit_asr_init);
    asr->secret_key = strdup(config->secret_key);
    AUDIO_MEM_CHECK(TAG, asr->secret_key, goto exit_asr_init);
    asr->format = strdup(config->format);
    AUDIO_MEM_CHECK(TAG, asr->format, goto exit_asr_init);
    asr->cuid = strdup(config->cuid);
    AUDIO_MEM_CHECK(TAG, asr->cuid, goto exit_asr_init);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    asr->i2s_reader = i2s_stream_init(&i2s_cfg);

    http_stream_cfg_t http_cfg = {
        .type = AUDIO_STREAM_WRITER,
        .event_handle = _http_stream_writer_event_handle,
        .user_data = asr,
        .task_stack = BAIDU_ASR_TASK_STACK,
    };
    asr->http_stream_writer = http_stream_init(&http_cfg);
    asr->on_begin = config->on_begin;

    audio_pipeline_register(asr->pipeline, asr->http_stream_writer, "asr_http");
    audio_pipeline_register(asr->pipeline, asr->i2s_reader,         "asr_i2s");
    audio_pipeline_link(asr->pipeline, (const char *[]) {"asr_i2s", "asr_http"}, 2);
    i2s_stream_set_clk(asr->i2s_reader, config->record_sample_rates, 16, 1);

    return asr;
exit_asr_init:
    baidu_asr_destroy(asr);
    return NULL;
}

esp_err_t baidu_asr_destroy(baidu_asr_handle_t asr)
{
    if (asr == NULL) {
        return ESP_FAIL;
    }
    audio_pipeline_terminate(asr->pipeline);
    audio_pipeline_remove_listener(asr->pipeline);
    audio_pipeline_deinit(asr->pipeline);
    audio_element_deinit(asr->i2s_reader);
    audio_element_deinit(asr->http_stream_writer);
    free(asr->buffer);
    free(asr->b64_buffer);
    free(asr->access_key);
    free(asr->secret_key);
    free(asr->format);
    free(asr->cuid);
    free(asr->token);
    free(asr->lan);
    free(asr->speech);
    free(asr->response_text);
    free(asr);
    return ESP_OK;
}

esp_err_t baidu_asr_set_listener(baidu_asr_handle_t asr, audio_event_iface_handle_t listener)
{
    if (listener) {
        audio_pipeline_set_listener(asr->pipeline, listener);
    }
    return ESP_OK;
}

esp_err_t baidu_asr_start(baidu_asr_handle_t asr)
{
    snprintf(asr->buffer, asr->buffer_size, BAIDU_ASR_ENDPOINT);
    audio_element_set_uri(asr->http_stream_writer, asr->buffer);
    audio_pipeline_reset_items_state(asr->pipeline);
    audio_pipeline_reset_ringbuffer(asr->pipeline);
    audio_pipeline_run(asr->pipeline);
    return ESP_OK;
}

char *baidu_asr_stop(baidu_asr_handle_t asr)
{
    audio_pipeline_stop(asr->pipeline);
    audio_pipeline_wait_for_stop(asr->pipeline);
    return asr->response_text;
}
