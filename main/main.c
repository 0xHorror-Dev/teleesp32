#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_tls.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "esp_system.h"

#include "esp_http_client.h"

#include "tg_api/tg_api.h"


#define WIFI_SSID "WIFI_SSID"
#define WIFI_SSID_PASS "WIFI_PASS"
#define WIFI_AUTH_MODE WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define WIFI_H2E_ID ""

#define MAX_ATTEMPTS 3

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

// s
static const char* TAG = "tg_wifi";

static EventGroupHandle_t gWifiEventGroup;
static int gTryCounter = 0;

extern const char ifconfig_pem_start[] asm("_binary_ifconfig_me_chain_pem_start");
extern const char ifconfig_pem_end[]   asm("_binary_ifconfig_me_chain_pem_end");

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if(gTryCounter < MAX_ATTEMPTS)
        {
            esp_wifi_connect();
            gTryCounter += 1;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else 
        {
            xEventGroupSetBits(gWifiEventGroup, WIFI_FAIL_BIT);
        }
    }else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        gTryCounter = 0;
        xEventGroupSetBits(gWifiEventGroup, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
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
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            // Clean the buffer in case of a new request
            if (output_len == 0 && evt->user_data) {
                // we are just starting to copy the output data into the use
                memset(evt->user_data, 0, MAX_HTTP_OUTPUT_BUFFER);
            }
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                int copy_len = 0;
                if (evt->user_data) {
                    // The last byte in evt->user_data is kept for the NULL character in case of out-of-bound access.
                    copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
                    if (copy_len) {
                        memcpy(evt->user_data + output_len, evt->data, copy_len);
                    }
                } else {
                    int content_len = esp_http_client_get_content_length(evt->client);
                    if (output_buffer == NULL) {
                        // We initialize output_buffer with 0 because it is used by strlen() and similar functions therefore should be null terminated.
                        output_buffer = (char *) calloc(content_len + 1, sizeof(char));
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    copy_len = MIN(evt->data_len, (content_len - output_len));
                    if (copy_len) {
                        memcpy(output_buffer + output_len, evt->data, copy_len);
                    }
                }
                output_len += copy_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            esp_http_client_set_header(evt->client, "From", "user@example.com");
            esp_http_client_set_header(evt->client, "Accept", "text/html");
            esp_http_client_set_redirection(evt->client);
            break;
    }
    return ESP_OK;
}

uint8_t wifi_init()
{
    gWifiEventGroup = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip);
 
    wifi_config_t wifi_config = {
        .sta ={
            .ssid = WIFI_SSID,
            .password = WIFI_SSID_PASS,
            .threshold.authmode = WIFI_AUTH_MODE,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
//            .sae_h2e_identifier  = WIFI_H2E_ID
        }
    };
 
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    //Waiting until either the connection is established 

    EventBits_t bits = xEventGroupWaitBits(gWifiEventGroup, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

        /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_SSID_PASS);
        return 1;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_SSID_PASS);
        return 0;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return 0;
    }

}



// void http_client_test()
// {
//     esp_http_client_config_t config = {
//         .url = "https://postman-echo.com/post",
//         .event_handler = _http_event_handler,
//         .cert_pem = postman_root_cert_pem_start,
//         .is_async = true,
//         .timeout_ms = 5000,
//     };


//     esp_http_client_handle_t client = esp_http_client_init(&config);
//     esp_err_t err;
//     const char *post_data = "Using a Palantír requires a person with great strength of will and wisdom. The Palantíri were meant to "
//                             "be used by the Dúnedain to communicate throughout the Realms in Exile. During the War of the Ring, "
//                             "the Palantíri were used by many individuals. Sauron used the Ithil-stone to take advantage of the users "
//                             "of the other two stones, the Orthanc-stone and Anor-stone, but was also susceptible to deception himself.";
//     esp_http_client_set_method(client, HTTP_METHOD_POST);
//     esp_http_client_set_post_field(client, post_data, strlen(post_data));
//     while (1) {
//         err = esp_http_client_perform(client);
//         if (err != ESP_ERR_HTTP_EAGAIN) {
//             break;
//         }
//     }

//     if (err == ESP_OK) {
//         ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %"PRId64,
//                 esp_http_client_get_status_code(client),
//                 esp_http_client_get_content_length(client));
//     } else {
//         ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
//     }
//     esp_http_client_cleanup(client);
//     vTaskDelete(NULL);
// }

#define NUMBER_STR_LEN 256

static uint64_t update_counter = 0;
static uint8_t work = 1;


void tg_bot_proccess_loop()
{
    tg_api_update update;

    if(!tg_api_get_update(update_counter, &update))
    {
        return;
    }
    
    update_counter = update.update + 1;
    
    if(strcmp(update.message,"/st") == 0)
    {
        tg_api_send_message("hi 192.168.0.1", update.chat_id);

        return;
    }

    if(strcmp(update.message,"/get_ip") == 0) 
    {
        esp_http_client_config_t config = {
            .url = "https://myexternalip.com/raw",
            .transport_type = HTTP_TRANSPORT_OVER_SSL, 
            .event_handler = _http_event_handler,
            .cert_pem = ifconfig_pem_start,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_err_t err;
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        err = esp_http_client_open(client, 0);
        int content_length = esp_http_client_fetch_headers (client);
        char buffer[256];

        int read_len = esp_http_client_read (client, buffer, 256);
        ESP_LOGD(TAG, "read_len = %d", read_len);
        if (read_len <= 0)
        {
            ESP_LOGE(TAG, "Error read data");
            esp_http_client_cleanup(client);
            return;
        }
        
        buffer[read_len] = 0;

        ESP_LOGI(TAG, "HTTP Stream reader Status = %d, content_length = %lld",
	   esp_http_client_get_status_code (client),
	   esp_http_client_get_content_length (client));

        tg_api_send_message(buffer, update.chat_id);

        esp_http_client_cleanup(client);
    }


}

void tg_bot_proccess()
{
    while(work)
    {
        tg_bot_proccess_loop();
       vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    if(!wifi_init())
    {
        return;
    }

    ESP_LOGI(TAG, "wifi init end!");
    vTaskDelay(500 / portTICK_PERIOD_MS);
    xTaskCreate(tg_bot_proccess, "tg_bot_proccess", 8192, NULL, 1, NULL);

    //xTaskCreate(&http_client_test, "http_client_test", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "task created!");
}
