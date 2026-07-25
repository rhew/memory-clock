#include "provisioning.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "wifi_env.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RECONNECT_BIT BIT1

enum {
    WIFI_RECONNECT_INITIAL_MS = 1000,
    WIFI_RECONNECT_MAX_MS = 30000,
    WIFI_RECONNECT_TASK_STACK_BYTES = 3072,
    WIFI_RECONNECT_TASK_PRIORITY = 1,
};

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

static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    while(true) {
        xEventGroupWaitBits(wifi_events, WIFI_RECONNECT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

        uint32_t delay_ms = WIFI_RECONNECT_INITIAL_MS;
        unsigned attempt = 0;
        while(!wifi_connected) {
            xEventGroupClearBits(wifi_events, WIFI_RECONNECT_BIT);
            ++attempt;
            esp_err_t err = esp_wifi_connect();
            if(err == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi reconnect attempt %u started", attempt);
                EventBits_t bits = xEventGroupWaitBits(
                    wifi_events, WIFI_CONNECTED_BIT | WIFI_RECONNECT_BIT,
                    pdFALSE, pdFALSE,
                    pdMS_TO_TICKS(MEMORY_CLOCK_WIFI_CONNECT_TIMEOUT_MS));
                if((bits & WIFI_CONNECTED_BIT) != 0) break;
                if((bits & WIFI_RECONNECT_BIT) == 0) {
                    ESP_LOGW(TAG, "Wi-Fi reconnect attempt %u timed out", attempt);
                    esp_wifi_disconnect();
                }
            } else {
                ESP_LOGW(TAG, "Wi-Fi reconnect attempt %u failed to start: %s",
                         attempt, esp_err_to_name(err));
            }

            xEventGroupClearBits(wifi_events, WIFI_RECONNECT_BIT);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            if(delay_ms < WIFI_RECONNECT_MAX_MS) {
                delay_ms *= 2;
                if(delay_ms > WIFI_RECONNECT_MAX_MS) delay_ms = WIFI_RECONNECT_MAX_MS;
            }
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "station started");
        xEventGroupSetBits(wifi_events, WIFI_RECONNECT_BIT);
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
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(wifi_events, WIFI_RECONNECT_BIT);
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

    BaseType_t task_created = xTaskCreate(wifi_reconnect_task, "wifi_reconnect",
                                          WIFI_RECONNECT_TASK_STACK_BYTES, NULL,
                                          WIFI_RECONNECT_TASK_PRIORITY, NULL);
    if(task_created != pdPASS) return ESP_ERR_NO_MEM;

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

bool provisioning_get_rssi(int8_t *rssi_out)
{
    if(rssi_out == NULL || !wifi_connected) return false;

    wifi_ap_record_t ap_info;
    if(esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return false;
    *rssi_out = ap_info.rssi;
    return true;
}

esp_err_t provisioning_wait_for_connection(char *ip_out, size_t ip_out_size)
{
    if(!provisioning_initialized || wifi_events == NULL) return ESP_ERR_INVALID_STATE;

    if(!wifi_connected) {
        EventBits_t bits = xEventGroupWaitBits(
            wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(MEMORY_CLOCK_WIFI_CONNECT_TIMEOUT_MS));
        if((bits & WIFI_CONNECTED_BIT) == 0) return ESP_ERR_TIMEOUT;
    }

    if(ip_out != NULL && ip_out_size > 0) {
        strlcpy(ip_out, connected_ip, ip_out_size);
    }
    return ESP_OK;
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
    sta_config.sta.failure_retry_cnt = 3;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "station config");
    ESP_LOGI(TAG, "starting station for SSID %s", MEMORY_CLOCK_WIFI_SSID);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "station start");

    esp_err_t connect_err = provisioning_wait_for_connection(ip_out, ip_out_size);
    if(connect_err == ESP_OK) return ESP_OK;

    if(sta_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if(esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "timeout IP state: " IPSTR, IP2STR(&ip_info.ip));
        }
    }
    ESP_LOGE(TAG, "connection timed out for SSID %s; last disconnect: %s (%d)",
             MEMORY_CLOCK_WIFI_SSID, wifi_reason_name(last_disconnect_reason),
             (int)last_disconnect_reason);
    return connect_err;
}
