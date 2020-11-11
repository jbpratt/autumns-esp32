#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#define CHIP_NAME "ESP32"
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group;
static const char *TAG = "APP";
static const char *WIFI_TAG = "WIFI";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_err_t err =
        tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, CONFIG_ESP_HOSTNAME);
    if (err != ESP_OK) {
      ESP_LOGE(WIFI_TAG, "failed to set hostname: %d", err);
    }
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void init_wifi(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, NULL));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = CONFIG_ESP_WIFI_SSID,
              .password = CONFIG_ESP_WIFI_PASSWORD,
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
          },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(WIFI_TAG, "Connection established\n");
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(WIFI_TAG, "Failed to establish connection\n");
  } else {
    ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
  }

  ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler));
  ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler));
  vEventGroupDelete(s_wifi_event_group);
}

void app_main(void) {
  ESP_LOGI(TAG, "Startup..");
  ESP_LOGI(TAG, "Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_LOGI(TAG, "Configured WiFi SSID is %s\n", CONFIG_ESP_WIFI_SSID);
  init_wifi();
}