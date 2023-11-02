#include "tg_api.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <esp_log.h>
#include "cJSON.h"

#include "esp_http_client.h"

#include "esp_tls.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif


#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

extern const char api_telegram_pem_start[] asm("_binary_api_telegram_org_pem_start");
extern const char api_telegram_pem_end[]   asm("_binary_api_telegram_org_pem_end"); 



//https://api.telegram.org/bot/sendMessage
#define BOT_TOKEN "6764252341:AAFo29i-0XVCMcnpfIK5f1v4ZM0xB9m2nAM"

static const char* const send_url = "https://api.telegram.org/bot" BOT_TOKEN "/sendMessage";

static const char* TAG = "TG_API";

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


uint8_t tg_api_send_message(const char* message, const char* chat_id)
{
    ESP_LOGI(TAG, "url %s", send_url);

    esp_http_client_config_t config = {
        .url = send_url,
        .event_handler = _http_event_handler,
        .cert_pem = api_telegram_pem_start,
        .is_async = true,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;
    char *post_data[1024];
    bzero(post_data, 0);
    sprintf(post_data,"{\"chat_id\":%s,\"text\":\"%s\"}",chat_id, message);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    while (1) 
    {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) 
        {
            break;
        }
    }

    uint8_t status = 0;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %"PRId64,
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
        status = 1;
    } else {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);

    return status;
}


static const char* const update_url ="https://api.telegram.org/bot" BOT_TOKEN "/getUpdates?offset="; 
static const char* const update_url_end ="&limit=1"; 
#define NUMBER_STR_LEN 255

uint8_t tg_api_get_update(const uint64_t update_id, tg_api_update* res)
{
    uint32_t update_url_len = strlen(update_url);
    uint32_t update_url_end_len = strlen(update_url_end);

    uint64_t number = update_id;

    char* number_str[NUMBER_STR_LEN];

    int number_len = sprintf(number_str,"%llu", number);
    if(number_len < 0)
    {
        ESP_LOGE(TAG, "int number_len = vsnprintf(NULL, 0, \"fmt\", number); -> number_len < 0 ");
        return 0;
    }

    uint32_t len = update_url_len + update_url_end_len + number_len;

    char* request_string = malloc(len + 1);
    memset(request_string, 0, len);
    bzero (request_string, len);

    strcat(request_string, update_url);
    strcat(request_string, number_str);
    strcat(request_string, update_url_end);

    request_string[len] = '\0';

    ESP_LOGI(TAG, "url = %s", request_string);

    esp_http_client_config_t config =
    { 
        .url = "https://api.telegram.org", 
        .transport_type = HTTP_TRANSPORT_OVER_SSL, 
        .event_handler = _http_event_handler,
	    .cert_pem = api_telegram_pem_start, 
    };
    
    // esp_http_client_config_t config = {
    //     .url = "https://api.telegram.org", 
    //     .event_handler = _http_event_handler,
    //     .cert_pem = api_telegram_pem_start,
    //     .is_async = true,
    //     .timeout_ms = 5000,
    // };
    
    esp_http_client_handle_t client = esp_http_client_init (&config);
    esp_http_client_set_url (client, request_string);

    free(request_string);

    esp_err_t err;
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s",
        esp_err_to_name (err));
        return 0;
    }
    vTaskDelay(400 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "esp_http_client_open - OK");
    
    int content_length = esp_http_client_fetch_headers (client);
    int total_read_len = 0, read_len;
    char *buffer = malloc (content_length + 1);
    bzero (buffer, content_length);
    ESP_LOGI(TAG, "esp_http_client_fetch_headers - OK");
    
    if (total_read_len < content_length)
    {
        read_len = esp_http_client_read (client, buffer, content_length);
        if (read_len <= 0)
        {
            ESP_LOGE(TAG, "Error read data");
            return 0;
        }
        
        buffer[read_len] = 0;
        ESP_LOGD(TAG, "read_len = %d", read_len);
    }

    ESP_LOGI(TAG, "HTTP Stream reader Status = %d, content_length = %lld",
	   esp_http_client_get_status_code (client),
	   esp_http_client_get_content_length (client));

    cJSON *parser = cJSON_Parse (buffer);
    free (buffer);

    cJSON *result = cJSON_GetObjectItem (parser, "result");
    cJSON *resultArray = cJSON_GetArrayItem (result, 0);
    if (resultArray == NULL)
    {
        cJSON_Delete (parser);
        esp_http_client_cleanup (client);
        return 0;
    }

    cJSON *updateID = cJSON_GetObjectItem (resultArray, "update_id");
    res->update = updateID->valueint;

    cJSON *message = cJSON_GetObjectItem (resultArray, "message");
    cJSON *text = cJSON_GetObjectItem (message, "text");
    cJSON *message_id = cJSON_GetObjectItem (message, "message_id");
    res->message_id = message_id->valueint;
    cJSON *chat = cJSON_GetObjectItem (message, "chat");
    cJSON *chat_id = cJSON_GetObjectItem (chat, "id");
    //res->chat_id = chat_id->valueint;
    char* schat_id = cJSON_Print(chat_id);

    ESP_LOGI(TAG, "chat id: %d", chat_id->valueint);
    ESP_LOGI(TAG, "chat id: %s",schat_id);
    strcpy(res->chat_id, schat_id);
    cJSON *username = cJSON_GetObjectItem (chat, "username");
    
    free(schat_id);

    strcpy(res->message, text->valuestring);
    strcpy(res->username, username->valuestring);

    cJSON_Delete (parser);
    esp_http_client_cleanup (client);
    ESP_LOGI(TAG, "esp_get_free_heap_size: %u", (unsigned int)esp_get_free_heap_size ());
    return 1;
}