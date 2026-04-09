#ifndef _BLUFI_INIT_H_
#define _BLUFI_INIT_H_

#include <stdio.h>
#include "esp_log.h"
#include "sdkconfig.h"

#ifndef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL ESP_LOG_INFO
#endif

#define BLUFI_EXAMPLE_TAG "BLUFI_EXAMPLE"
#define BLUFI_INFO(fmt, ...)   ESP_LOGI(BLUFI_EXAMPLE_TAG, fmt, ##__VA_ARGS__)
#define BLUFI_ERROR(fmt, ...)  ESP_LOGE(BLUFI_EXAMPLE_TAG, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

extern esp_err_t blufi_security_init(void);
extern void blufi_security_deinit(void);
extern esp_err_t esp_blufi_controller_init();
extern esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *example_callbacks);
extern void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free);
extern int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
extern int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
extern uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);


#ifdef __cplusplus
}
#endif

#endif



