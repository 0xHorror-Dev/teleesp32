
set(requires "")
idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "linux")
    list(APPEND requires esp_stubs esp_event esp-tls esp_http_client protocol_examples_common nvs_flash)
endif()

idf_component_register(SRCS "main.c" "tg_api/tg_api.c"
                    INCLUDE_DIRS "."
                    INCLUDE_DIRS "."
                    REQUIRES ${requires}
                    EMBED_TXTFILES
                    "${CMAKE_CURRENT_SOURCE_DIR}/api_telegram_org.pem"
                    "${CMAKE_CURRENT_SOURCE_DIR}/ifconfig_me_chain.pem"
                    )