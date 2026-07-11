#include "provisioning.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "wifi_env.h"

#define CONNECT_TIMEOUT_MS 30000
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "provisioning";
static EventGroupHandle_t wifi_events;
static esp_netif_t *sta_netif;
static char connected_ip[16];
static bool provisioning_initialized;
static volatile bool wifi_connected;
static wifi_err_reason_t last_disconnect_reason;

static const char *wifi_reason_name(wifi_err_reason_t reason)
{
    switch(reason) {
    case WIFI_REASON_UNSPECIFIED: return "WIFI_REASON_UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE: return "WIFI_REASON_AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE: return "WIFI_REASON_AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE: return "WIFI_REASON_ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY: return "WIFI_REASON_ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED: return "WIFI_REASON_NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED: return "WIFI_REASON_NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE: return "WIFI_REASON_ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "WIFI_REASON_ASSOC_NOT_AUTHED";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "WIFI_REASON_802_1X_AUTH_FAILED";
    case WIFI_REASON_BEACON_TIMEOUT: return "WIFI_REASON_BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "WIFI_REASON_NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "WIFI_REASON_AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "WIFI_REASON_ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "WIFI_REASON_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL: return "WIFI_REASON_CONNECTION_FAIL";
    default: return "WIFI_REASON_UNKNOWN";
    }
}

static esp_err_t wifi_safe_stop(void)
{
    esp_err_t err = esp_wifi_stop();
    if(err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) return ESP_OK;
    return err;
}

static bool lock_to_best_24g_bssid(const char *ssid, uint8_t out_bssid[6], uint8_t *out_channel)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)ssid,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    if(esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return false;
    }

    uint16_t ap_count = 0;
    if(esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        return false;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(*records));
    if(records == NULL) {
        return false;
    }

    bool found = false;
    int best_rssi = -127;
    if(esp_wifi_scan_get_ap_records(&ap_count, records) == ESP_OK) {
        for(uint16_t i = 0; i < ap_count; ++i) {
            if(strcmp((const char *)records[i].ssid, ssid) == 0 && records[i].primary <= 13 &&
               records[i].rssi > best_rssi) {
                best_rssi = records[i].rssi;
                memcpy(out_bssid, records[i].bssid, sizeof(records[i].bssid));
                *out_channel = records[i].primary;
                found = true;
            }
        }
    }

    free(records);
    return found;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "station started");
    } else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        esp_netif_dhcp_status_t status;
        esp_err_t err = esp_netif_dhcpc_get_status(sta_netif, &status);
        if(err == ESP_OK) {
            ESP_LOGI(TAG, "STA dhcpc status: %d", (int)status);
            if(status == ESP_NETIF_DHCP_INIT) {
                err = esp_netif_dhcpc_start(sta_netif);
                if(err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                    ESP_LOGI(TAG, "dhcpc started on STA netif");
                } else {
                    ESP_LOGW(TAG, "dhcpc start failed: %s", esp_err_to_name(err));
                }
            }
        } else {
            ESP_LOGW(TAG, "dhcpc status failed: %s", esp_err_to_name(err));
        }
    } else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        last_disconnect_reason = event != NULL ? event->reason : WIFI_REASON_UNSPECIFIED;
        ESP_LOGW(TAG, "station disconnected, reason=%d (%s)",
                 event != NULL ? event->reason : -1,
                 wifi_reason_name(last_disconnect_reason));
        esp_err_t reconnect_err = esp_wifi_connect();
        if(reconnect_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi reconnect failed to start: %s", esp_err_to_name(reconnect_err));
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, connected_ip, sizeof(connected_ip));
        ESP_LOGI(TAG, "connected with IP " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t provisioning_init(void)
{
    if(provisioning_initialized) return ESP_OK;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");

    esp_err_t loop_err = esp_event_loop_create_default();
    if(loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }

    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable wifi power save");

    wifi_events = xEventGroupCreate();
    if(wifi_events == NULL) return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL), TAG,
                        "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL), TAG,
                        "ip handler");

    provisioning_initialized = true;
    return ESP_OK;
}

const char *provisioning_ssid(void)
{
    return MEMORY_CLOCK_WIFI_SSID;
}

bool provisioning_is_connected(void)
{
    return wifi_connected;
}

esp_err_t provisioning_start(char *ip_out, size_t ip_out_size)
{
    ESP_RETURN_ON_ERROR(provisioning_init(), TAG, "init");

    connected_ip[0] = '\0';
    wifi_connected = false;
    last_disconnect_reason = 0;
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, MEMORY_CLOCK_WIFI_SSID, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, MEMORY_CLOCK_WIFI_PASSWORD,
            sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode =
        MEMORY_CLOCK_WIFI_PASSWORD[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_RETURN_ON_ERROR(wifi_safe_stop(), TAG, "wifi stop");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start for scan");

    uint8_t bssid[6];
    uint8_t channel = 0;
    if(lock_to_best_24g_bssid(MEMORY_CLOCK_WIFI_SSID, bssid, &channel)) {
        memcpy(sta_config.sta.bssid, bssid, sizeof(bssid));
        sta_config.sta.bssid_set = true;
        sta_config.sta.channel = channel;
        ESP_LOGI(TAG,
                 "locking to 2.4 GHz BSSID %02x:%02x:%02x:%02x:%02x:%02x on channel %u",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], channel);
    } else {
        ESP_LOGW(TAG, "no 2.4 GHz BSSID lock found for SSID %s; continuing without lock",
                 MEMORY_CLOCK_WIFI_SSID);
    }

    ESP_RETURN_ON_ERROR(wifi_safe_stop(), TAG, "wifi stop after scan");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "station config");
    ESP_LOGI(TAG, "starting station for SSID %s", MEMORY_CLOCK_WIFI_SSID);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "station start");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "station connect");

    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if((bits & WIFI_CONNECTED_BIT) != 0) {
        strlcpy(ip_out, connected_ip, ip_out_size);
        return ESP_OK;
    }

    if(sta_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if(esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "timeout IP state: " IPSTR, IP2STR(&ip_info.ip));
        }
    }
    ESP_LOGE(TAG, "connection timed out for SSID %s; last disconnect: %s (%d)",
             MEMORY_CLOCK_WIFI_SSID, wifi_reason_name(last_disconnect_reason),
             (int)last_disconnect_reason);
    return ESP_ERR_TIMEOUT;
}
