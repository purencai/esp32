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

#ifndef _BAIDU_SR_H_
#define _BAIDU_SR_H_

#include "esp_err.h"
#include "audio_event_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_SR_BUFFER_SIZE (2048)

/**
 * baidu Cloud Speech-to-Text audio encoding
 */
typedef enum {
    ENCODING_LINEAR16 = 0,  /*!< baidu Cloud Speech-to-Text audio encoding PCM 16-bit mono */
} baidu_asr_encoding_t;

typedef struct baidu_asr* baidu_asr_handle_t;
typedef void (*baidu_asr_event_handle_t)(baidu_asr_handle_t sr);

/**
 * baidu Cloud Speech-to-Text configurations
 */
typedef struct {
    char *access_key;
    char *secret_key;
    char *format;                       /*!< Speech-to-Text language code */
    int record_sample_rates;            /*!< Audio recording sample rate */
    int channel;                        /*!< Audio encoding */
    char *cuid;                           /*!< Processing buffer size */
    char *token;
    int dev_pid;
    char *lan;
    char *speech;
    int len;
    int buffer_size;
    baidu_asr_event_handle_t on_begin;  /*!< Begin send audio data to server */
} baidu_asr_config_t;


/**
 * @brief      initialize baidu Cloud Speech-to-Text, this function will return a Speech-to-Text context
 *
 * @param      config  The baidu Cloud Speech-to-Text configuration
 *
 * @return     The Speech-to-Text context
 */
baidu_asr_handle_t baidu_asr_init(baidu_asr_config_t *config);

/**
 * @brief      Start recording and sending audio to baidu Cloud Speech-to-Text
 *
 * @param[in]  sr   The Speech-to-Text context
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t baidu_asr_start(baidu_asr_handle_t sr);

/**
 * @brief      Stop sending audio to baidu Cloud Speech-to-Text and get the result text
 *
 * @param[in]  sr   The Speech-to-Text context
 *
 * @return     baidu Cloud Speech-to-Text server response
 */
char *baidu_asr_stop(baidu_asr_handle_t sr);

/**
 * @brief      Cleanup the Speech-to-Text object
 *
 * @param[in]  sr   The Speech-to-Text context
 *
 * @return
 *  - ESP_OK
 *  - ESP_FAIL
 */
esp_err_t baidu_asr_destroy(baidu_asr_handle_t sr);

/**
 * @brief      Register listener for the Speech-to-Text context
 *
 * @param[in]   sr   The Speech-to-Text context
 * @param[in]  listener  The listener
 *
 * @return
 *  - ESP_OK
 *  - ESP_FAIL
 */
esp_err_t baidu_asr_set_listener(baidu_asr_handle_t sr, audio_event_iface_handle_t listener);

#ifdef __cplusplus
}
#endif

#endif
