#include <stdio.h>

#include "cJSON.h"
#include "driver/i2c.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iot_bme280.h"
#include "iot_i2c_bus.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#define CHIP_NAME "ESP32"
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Websocket timeout if no data is received
#define NO_DATA_TIMEOUT_SEC 60

// HTTP settings
#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

static const char *TAG = "APP";
static const char *WIFI_TAG = "WIFI";
static const char *WS_TAG = "WEBSOCKET";
static const char *BME280_TAG = "BME280";
// static const char *HTTP_TAG = "HTTP";

static EventGroupHandle_t s_wifi_event_group;

#if CONFIG_ENABLE_BME280_SENSOR

static i2c_bus_handle_t i2c_bus = NULL;
static bme280_handle_t dev = NULL;

#define I2C_MASTER_SCL_IO 21        /*!< gpio number for I2C master clock IO21*/
#define I2C_MASTER_SDA_IO 15        /*!< gpio number for I2C master data  IO15*/
#define I2C_MASTER_NUM I2C_NUM_1    /*!< I2C port number for master dev */
#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master do not need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master do not need buffer */
#define I2C_MASTER_FREQ_HZ 100000   /*!< I2C master clock frequency */

#endif

/*
echo "" | openssl s_client -showcerts -connect strims.gg:443 | sed -n \
 "1,/Root/d; /BEGIN/,/END/p" | openssl x509 -outform PEM >certs/ca_cert.pem
*/
// strims ssl cert
extern const uint8_t ws_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ws_cert_pem_end[] asm("_binary_ca_cert_pem_end");

// Websocket signaling
static TimerHandle_t shutdown_signal_timer;
static SemaphoreHandle_t shutdown_sema;

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

static void ws_shutdown_signaler(TimerHandle_t xTimer) {
  ESP_LOGI(WS_TAG, "No data received for %d seconds, signaling shutdown",
           NO_DATA_TIMEOUT_SEC);
  xSemaphoreGive(shutdown_sema);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_CONNECTED");
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_DISCONNECTED");
      break;
    case WEBSOCKET_EVENT_DATA:
      ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_DATA: Received opcode=%d",
               data->op_code);
      ESP_LOGW(
          WS_TAG,
          "Total payload length=%d, data_len=%d, current payload offset=%d\r\n",
          data->payload_len, data->data_len, data->payload_offset);

      char type[12];
      char *rendered;
      cJSON *root;

      sscanf((char *)data->data_ptr, "%11s", type);
      if (strcmp(type, "JOIN") == 0) {
        root = cJSON_Parse((char *)data->data_ptr + 5);
        rendered = cJSON_Print(root);
        ESP_LOGI(WS_TAG, "Join: %s\n", rendered);
        cJSON_Delete(root);
      } else if (strcmp(type, "QUIT") == 0) {
        root = cJSON_Parse((char *)data->data_ptr + 5);
        rendered = cJSON_Print(root);
        ESP_LOGI(WS_TAG, "Quit: %s\n", rendered);
        cJSON_Delete(root);
      } else if (strcmp(type, "VIEWERSTATE") == 0) {
        root = cJSON_Parse((char *)data->data_ptr + 11);
        rendered = cJSON_Print(root);
        ESP_LOGI(WS_TAG, "Viewerstate: %s\n", rendered);
        cJSON_Delete(root);
      } else if (strcmp(type, "MSG") == 0) {
        root = cJSON_Parse((char *)data->data_ptr + 4);
        rendered = cJSON_Print(root);
        ESP_LOGI(WS_TAG, "Msg: %s\n", rendered);
        cJSON_Delete(root);
      }

      xTimerReset(shutdown_signal_timer, portMAX_DELAY);
      break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_ERROR");
      break;
  }
}

void init_ws(void) {
  const esp_websocket_client_config_t websocket_cfg = {
      .uri = CONFIG_WEBSOCKET_URI,
      .cert_pem = (const char *)ws_cert_pem_start,
  };

  shutdown_signal_timer =
      xTimerCreate("Websocket shutdown timer",
                   NO_DATA_TIMEOUT_SEC * 1000 / portTICK_PERIOD_MS, pdFALSE,
                   NULL, ws_shutdown_signaler);
  shutdown_sema = xSemaphoreCreateBinary();

  esp_websocket_client_handle_t client =
      esp_websocket_client_init(&websocket_cfg);
  esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                websocket_event_handler, (void *)client);

  esp_websocket_client_start(client);

  while (esp_websocket_client_is_connected(client)) {
  }

  xSemaphoreTake(shutdown_sema, portMAX_DELAY);
  esp_websocket_client_stop(client);
  ESP_LOGI(WS_TAG, "Websocket Stopped");
  esp_websocket_client_destroy(client);
}

static void i2c_bus_init(void) {
  i2c_config_t conf;
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = I2C_MASTER_SDA_IO;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = I2C_MASTER_SCL_IO;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
  i2c_bus = iot_i2c_bus_create(I2C_MASTER_NUM, &conf);
}

void bme280_run(void *pvParameters) {
  ESP_LOGI("BME280:", "temperature:%f\n", iot_bme280_read_temperature(dev));
  vTaskDelay(300 / portTICK_RATE_MS);
  ESP_LOGI("BME280:", "humidity:%f\n", iot_bme280_read_humidity(dev));
  vTaskDelay(300 / portTICK_RATE_MS);
  ESP_LOGI("BME280:", "pressure:%f\n", iot_bme280_read_pressure(dev));
  vTaskDelay(300 / portTICK_RATE_MS);
}

void bme280_init(void) {
  i2c_bus_init();
  dev = iot_bme280_create(i2c_bus, BME280_I2C_ADDRESS_DEFAULT);
  ESP_LOGI(BME280_TAG, "iot_bme280_init:%d", iot_bme280_init(dev));
  xTaskCreate(bme280_run, "bme280_run", 1024 * 2, NULL, 10, NULL);
}

void app_main(void) {
  ESP_LOGI(TAG, "Startup..");
  ESP_LOGI(TAG, "Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_LOGI(TAG, "Configured WiFi SSID is %s\n", CONFIG_ESP_WIFI_SSID);
  init_wifi();

  init_ws();
#if CONFIG_ENABLE_BME280_SENSOR
  bme280_init();
#endif
}