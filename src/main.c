/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_wifi.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#if !CONFIG_IDF_TARGET_LINUX
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#endif

#include "esp_http_client.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define CONFIG_BACKUP_WIFI_RETRY_COUNT 10

static const char *TAG = "thing";

static EventGroupHandle_t s_wifi_event_group;
static char s_http_client_buffer[2048];
static int s_retry_num = 0;

static esp_err_t _http_event_handler_blank(esp_http_client_event_t* evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
                     evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}


static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_BACKUP_WIFI_RETRY_COUNT) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry %d to connect to the AP", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}


esp_err_t wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));


    wifi_config_t wifi_config = {
        .sta = {
          .ssid = "B",
          .password = "temppassword"
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect");
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "UNEXPECTED EVENT");

    return ESP_FAIL;
}
static esp_err_t _http_event_handler(esp_http_client_event_t* evt) {
    static char* output_buffer;  // Buffer to store response of http request
                                 // from event handler
    static int output_len;       // Stores number of bytes read
    ESP_LOGI(TAG, "HTTP event: %d", evt->event_id);
    switch (evt->event_id) {
        default:
            ESP_LOGI(TAG, "Unhandled event");
            break;
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            s_http_client_buffer[0] = '\0';

            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
                     evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the
                // buffer
                if (evt->user_data) {
                    memcpy(evt->user_data + output_len, evt->data,
                           evt->data_len);
                } else {
                    if (output_buffer == NULL) {
                        output_buffer = (char*)malloc(
                            esp_http_client_get_content_length(evt->client));
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(
                                TAG,
                                "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    memcpy(output_buffer + output_len, evt->data,
                           evt->data_len);
                }
                output_len += evt->data_len;
            } else {
                int chunk_len = 0;
                esp_http_client_get_chunk_length(evt->client, &chunk_len);
                ESP_LOGI(TAG, "Received chunk length: %d", chunk_len);

                if (output_buffer == NULL) {
                    // if there's no buffer yet, alloc one
                    output_buffer = (char*)malloc(chunk_len);
                    output_len = 0;
                    ESP_LOGI(TAG, "Allocated memory for output buffer");
                } else {
                    // else, just expand the existing buffer
                    output_buffer =
                        (char*)realloc(output_buffer, output_len + chunk_len);
                    ESP_LOGI(TAG, "Reallocated memory for output buffer");
                }

                // handle out of memory error
                if (output_buffer == NULL) {
                    ESP_LOGE(TAG,
                             "Failed to allocate memory for output buffer");
                    return ESP_FAIL;
                }

                // read the chunk into the buffer
                ESP_LOGI(TAG,
                         "Copying memory from evt->data to output_buffer[%d]",
                         output_len);

                memcpy(&output_buffer[output_len], evt->data, chunk_len);
                output_len += chunk_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below
                // line to print the accumulated response
                ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);

                memcpy(s_http_client_buffer, output_buffer, output_len);
                s_http_client_buffer[output_len] = 0;

                // xEventGroupSetBits(s_http_client_event_group,
                //                    HTTP_CLIENT_HAS_DATA_BIT);

                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error(
                (esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (output_buffer != NULL) {
                // ESP_LOGI(TAG, "Received %d bytes of response: '%s'",
                // output_len, output_buffer);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED: finished handling cleanup");
            break;
    }
    return ESP_OK;
}

char long_header[2048] = {0};

void do_http_request() {
    memset(long_header, 'A', sizeof(long_header) - 1);

    esp_http_client_config_t config = {
        .url = "https://webhook.site/e9e65eee-fc54-46e7-b455-81a6781b5295",
        .event_handler = _http_event_handler_blank,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        // .client_cert_pem = settings->target_endpoint_client_cert,
        // .client_cert_len = strlen(settings->target_endpoint_client_cert) + 1,
        // .client_key_pem = settings->target_endpoint_client_private_key,
        // .client_key_len = strlen(settings->target_endpoint_client_private_key) + 1,
        .method = HTTP_METHOD_GET,
        .user_data = NULL,
        .user_agent = "bug demo lol",
        .timeout_ms = 120000,
        // .is_async = true,
        .buffer_size = 4096,
        .buffer_size_tx = 4096
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    ESP_ERROR_CHECK(esp_http_client_set_header(client, "X-Long-Header", long_header));
    
    ESP_ERROR_CHECK(esp_http_client_perform(client));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(wifi_init());
    ESP_LOGI(TAG, "Connected to AP, begin http example");
    
    do_http_request();

    ESP_LOGI(TAG, "Done with the http request");

    while(1) {
      vTaskDelay(100/portTICK_PERIOD_MS);
    }

}