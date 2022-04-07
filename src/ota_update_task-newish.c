/**
 * @brief ESP32_ota_update_task - Pull Over the Air (OTA) update from http server (bleeding edge)
 *
 * Inspired by <https://github.com/espressif/esp-idf/blob/master/examples/system/ota/native_ota_example/main/native_ota_example.c>
 * 
 * Written in 2022 by ESPRESSIF and Coert Vonk 
 * 
 * To the extent possible under law, the author(s) have dedicated all copyright and related and
 * neighboring rights to this software to the public domain worldwide. This software is
 * distributed without any warranty. You should have received a copy of the CC0 Public Domain
 * Dedication along with this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
 * 
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include "esp_https_ota.h"
#include <esp_flash_partitions.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_image_format.h>

#include "ota_update_task.h"

#define ARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))
#define ALIGN( type ) __attribute__((aligned( __alignof__( type ) )))
#define PACK( type )  __attribute__((aligned( __alignof__( type ) ), packed ))
#define PACK8  __attribute__((aligned( __alignof__( uint8_t ) ), packed ))

//#define BUFFSIZE 1024
//#define HASH_LEN 32 /* SHA-256 digest length */

static char const * const TAG = "ota_task";
//extern uint8_t const server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
//extern uint8_t const server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
//static char ota_write_data[BUFFSIZE + 1] = { 0 };  // OTA data buffer ready to write to flash

static void
_http_cleanup(esp_https_ota_handle_t https_ota_handle)
{
    esp_https_ota_abort(https_ota_handle);
    ESP_LOGE(TAG, "upgrade failed");
}

static void
__attribute__((noreturn)) _delete_task()
{
    ESP_LOGI(TAG, "Exiting task ..");
    (void)vTaskDelete(NULL);

    while (1) {  // FreeRTOS requires that tasks never return
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static bool
_versions_match(esp_app_desc_t const * const desc1, esp_app_desc_t const * const desc2)
{
    return
        strncmp(desc1->project_name, desc2->project_name, sizeof(desc1->project_name)) == 0 &&
        strncmp(desc1->version, desc2->version, sizeof(desc1->version)) == 0 &&
        strncmp(desc1->date, desc2->date, sizeof(desc1->date)) == 0 &&
        strncmp(desc1->time, desc2->time, sizeof(desc1->time)) == 0;
}

void
ota_update_task(void * pvParameter)
{
    ESP_LOGI(TAG, "Checking for OTA update (%s)", CONFIG_OTA_UPDATE_FIRMWARE_URL);

    esp_partition_t const * const configured_part = esp_ota_get_boot_partition();
    esp_partition_t const * const running_part = esp_ota_get_running_partition();
    esp_partition_t const * update_part = esp_ota_get_next_update_partition(NULL);

    if (configured_part != running_part) {
        // this can happen if either the OTA boot data or preferred boot image become corrupted
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured_part->address, running_part->address);
    }
    ESP_LOGI(TAG, "Running from part \"%s\" (0x%08x)", running_part->label, running_part->address);

    // connect to OTA server

    esp_http_client_config_t config = {
        .url = CONFIG_OTA_UPDATE_FIRMWARE_URL,
        //.cert_pem = (char *)server_cert_pem_start,
        .timeout_ms = CONFIG_OTA_UPDATE_RECV_TIMEOUT,
        .keep_alive_enable = true
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Begin failed");
        _delete_task();
    }
    if (https_ota_handle == NULL) {
        _http_cleanup(https_ota_handle);
        ESP_LOGE(TAG, "No update found");
        _delete_task();
    }

    // get versions

    esp_app_desc_t new_app_info;
    err = esp_https_ota_get_img_desc(https_ota_handle, &new_app_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Can't get server version (%02X)", err);
        ESP_ERROR_CHECK(err);
        _http_cleanup(https_ota_handle);
        _delete_task();
    }
    ESP_LOGI(TAG, "Firmware on server: %s.%s (%s %s)", new_app_info.project_name, new_app_info.version, new_app_info.date, new_app_info.time);

    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running_part, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Firmware running:   %s.%s (%s %s)", running_app_info.project_name, running_app_info.version, running_app_info.date, running_app_info.time);
    }

    esp_partition_t const * const last_invalid_app = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_app_info;
    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Firmware marked invalid: %s.%s (%s %s)", invalid_app_info.project_name, invalid_app_info.version, new_app_info.date, new_app_info.time);
    }

    // compare versions

    if (last_invalid_app != NULL) {
        if (_versions_match(&invalid_app_info, &new_app_info)) {
            ESP_LOGW(TAG, "Version on server is the same as invalid version (%s)", invalid_app_info.version);
            _http_cleanup(https_ota_handle);
            _delete_task();
        }
    }
    if (_versions_match(&new_app_info, &running_app_info)) {
        ESP_LOGI(TAG, "No update available");
        _http_cleanup(https_ota_handle);
        _delete_task();
    }

    // download update

    ESP_LOGW(TAG, "Downloading OTA update ..");
    assert(update_part != NULL);
    ESP_LOGI(TAG, "Writing to part \"%s\" at offset 0x%x", update_part->label, update_part->address);

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        ESP_LOGI(TAG, "Bytes read: %d", esp_https_ota_get_image_len_read(https_ota_handle));
    }

    // check

    if (err != ESP_OK || esp_https_ota_is_complete_data_received(https_ota_handle) != true) {
        ESP_LOGE(TAG, "Download error (%u)", err);
        _http_cleanup(https_ota_handle);
        _delete_task();
    }
    err = esp_https_ota_finish(https_ota_handle);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGE(TAG, "Downloaded image is corrupted");
    }

    // restart with new image

    ESP_LOGI(TAG, "Prepare to restart system!");
    esp_restart();

    while (1) {  // FreeRTOS requires that tasks never return
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
