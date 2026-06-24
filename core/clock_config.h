#ifndef MOM_CLOCK_CONFIG_H
#define MOM_CLOCK_CONFIG_H

#include <stddef.h>

enum {
    CLOCK_CONFIG_MAX_NTP_SERVERS = 4,
    CLOCK_CONFIG_TIMEZONE_SIZE = 64,
    CLOCK_CONFIG_NTP_SERVER_SIZE = 256,
    CLOCK_CONFIG_SERVER_URL_SIZE = 512,
    CLOCK_CONFIG_BEARER_TOKEN_SIZE = 256,
};

typedef struct {
    char timezone[CLOCK_CONFIG_TIMEZONE_SIZE];
    char ntp_servers[CLOCK_CONFIG_MAX_NTP_SERVERS][CLOCK_CONFIG_NTP_SERVER_SIZE];
    size_t ntp_count;
    char server_url[CLOCK_CONFIG_SERVER_URL_SIZE];
    char bearer_token[CLOCK_CONFIG_BEARER_TOKEN_SIZE];
} clock_config_t;

#endif
