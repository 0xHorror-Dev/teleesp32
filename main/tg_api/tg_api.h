#ifndef _ESP_32_TH_API_H_
#define _ESP_32_TH_API_H_

#include <stdint.h>

uint8_t tg_api_send_message(const char* message, const char* chat_id);

typedef struct 
{
    uint64_t update;
    uint64_t user_id;
    char chat_id[256];
    uint64_t message_id;
    char message[256];
    char username[256];
} tg_api_update;

uint8_t tg_api_get_update(const uint64_t update_id, tg_api_update* res);

#endif
