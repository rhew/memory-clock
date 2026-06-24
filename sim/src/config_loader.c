#include "config_loader.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool copy_json_string(const cJSON *root, const char *key, char *destination,
                             size_t destination_size, char *error, size_t error_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if(!cJSON_IsString(item) || item->valuestring[0] == '\0') {
        snprintf(error, error_size, "'%s' must be a non-empty string", key);
        return false;
    }

    size_t length = strlen(item->valuestring);
    if(length >= destination_size) {
        snprintf(error, error_size, "'%s' is too long", key);
        return false;
    }

    memcpy(destination, item->valuestring, length + 1);
    return true;
}

static char *read_file(const char *path, char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    if(file == NULL) {
        snprintf(error, error_size, "cannot open %s", path);
        return NULL;
    }

    if(fseek(file, 0, SEEK_END) != 0) {
        snprintf(error, error_size, "cannot read %s", path);
        fclose(file);
        return NULL;
    }

    long length = ftell(file);
    if(length < 0 || length > 64 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        snprintf(error, error_size, "invalid config size for %s", path);
        fclose(file);
        return NULL;
    }

    char *contents = malloc((size_t)length + 1);
    if(contents == NULL) {
        snprintf(error, error_size, "out of memory reading %s", path);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(contents, 1, (size_t)length, file);
    fclose(file);
    if(bytes_read != (size_t)length) {
        snprintf(error, error_size, "cannot read %s", path);
        free(contents);
        return NULL;
    }

    contents[length] = '\0';
    return contents;
}

bool sim_config_load(const char *path, clock_config_t *config, char *error, size_t error_size)
{
    memset(config, 0, sizeof(*config));

    char *contents = read_file(path, error, error_size);
    if(contents == NULL) return false;

    cJSON *root = cJSON_Parse(contents);
    free(contents);
    if(root == NULL || !cJSON_IsObject(root)) {
        snprintf(error, error_size, "invalid JSON in %s", path);
        cJSON_Delete(root);
        return false;
    }

    bool valid = copy_json_string(root, "timezone", config->timezone, sizeof(config->timezone), error, error_size)
                 && copy_json_string(root, "server_url", config->server_url, sizeof(config->server_url), error,
                                     error_size)
                 && copy_json_string(root, "bearer_token", config->bearer_token, sizeof(config->bearer_token), error,
                                     error_size);

    const cJSON *ntp = cJSON_GetObjectItemCaseSensitive(root, "ntp");
    if(valid && (!cJSON_IsArray(ntp) || cJSON_GetArraySize(ntp) == 0)) {
        snprintf(error, error_size, "'ntp' must be a non-empty array");
        valid = false;
    }

    int ntp_count = valid ? cJSON_GetArraySize(ntp) : 0;
    if(valid && ntp_count > CLOCK_CONFIG_MAX_NTP_SERVERS) {
        snprintf(error, error_size, "'ntp' supports at most %d servers", CLOCK_CONFIG_MAX_NTP_SERVERS);
        valid = false;
    }

    for(int index = 0; valid && index < ntp_count; ++index) {
        const cJSON *server = cJSON_GetArrayItem(ntp, index);
        if(!cJSON_IsString(server) || server->valuestring[0] == '\0') {
            snprintf(error, error_size, "'ntp[%d]' must be a non-empty string", index);
            valid = false;
            break;
        }

        size_t length = strlen(server->valuestring);
        if(length >= sizeof(config->ntp_servers[index])) {
            snprintf(error, error_size, "'ntp[%d]' is too long", index);
            valid = false;
            break;
        }

        memcpy(config->ntp_servers[index], server->valuestring, length + 1);
        config->ntp_count++;
    }

    cJSON_Delete(root);
    return valid;
}
