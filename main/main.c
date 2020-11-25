#include <stdio.h>

#include "bme680_sensor.h"
#include "driver/i2c.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_bme280.h"
#include "iot_i2c_bus.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#define CHIP_NAME "ESP32"
#endif

// HTTP settings
#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

static const char *TAG = "APP";

/*
echo "" | openssl s_client -showcerts -connect strims.gg:443 | sed -n \
 "1,/Root/d; /BEGIN/,/END/p" | openssl x509 -outform PEM >certs/ca_cert.pem
*/
// strims ssl cert
// extern const uint8_t ws_cert_pem_start[] asm("_binary_ca_cert_pem_start");
// extern const uint8_t ws_cert_pem_end[] asm("_binary_ca_cert_pem_end");
static const char *HTTP_TAG = "HTTP";

#ifdef CONFIG_ENABLE_BME280_SENSOR
static const char *BME280_TAG = "BME280";

static i2c_bus_handle_t i2c_bus = NULL;
static bme280_handle_t dev = NULL;

#define I2C_MASTER_SCL_IO 21        /*!< gpio number for I2C master clock IO21*/
#define I2C_MASTER_SDA_IO 15        /*!< gpio number for I2C master data  IO15*/
#define I2C_MASTER_NUM I2C_NUM_1    /*!< I2C port number for master dev */
#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master do not need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master do not need buffer */
#define I2C_MASTER_FREQ_HZ 100000   /*!< I2C master clock frequency */

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
#endif

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  static char *output_buffer;
  static int output_len;

  switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
      ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ERROR");
      break;
    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(HTTP_TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
               evt->header_key, evt->header_value);
      break;
    case HTTP_EVENT_ON_DATA:
      ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_DATA, len%d", evt->data_len);
      // check for chunked encoding
      if (!esp_http_client_is_chunked_response(evt->client)) {
        // If user_data buffer is configured, copy the response into the buffer
        if (evt->user_data) {
          memcpy(evt->user_data + output_len, evt->data, evt->data_len);
        } else {
          // if output_buffer is NULL, allocate needed memory and copy the data
          if (!output_buffer) {
            output_buffer =
                (char *)malloc(esp_http_client_get_content_length(evt->client));
            output_len = 0;
            if (!output_buffer) {
              ESP_LOGE(HTTP_TAG, "Failed to allocate memory for output buffer");
              return ESP_FAIL;
            }
          }
          memcpy(output_buffer + output_len, evt->data, evt->data_len);
        }
        output_len += evt->data_len;
      }
      break;

    case HTTP_EVENT_ON_FINISH:
      ESP_LOGI(HTTP_TAG, "HTTP_EVENT_ON_FINISH");
      if (output_buffer) {
        free(output_buffer);
        output_buffer = NULL;
      }
      output_len = 0;
      break;

    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGI(HTTP_TAG, "HTTP_EVENT_DISCONNECTED");
      int mbedtls_err = 0;
      esp_err_t err =
          esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
      if (err != 0) {
        if (output_buffer) {
          free(output_buffer);
          output_buffer = NULL;
        }
        output_len = 0;
        ESP_LOGI(HTTP_TAG, "Last esp error code: 0x%x", err);
        ESP_LOGI(HTTP_TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
      }
      break;
  }

  return ESP_OK;
}

void influx_post_data(char *data) {
  char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

  esp_http_client_config_t conf = {.url = CONFIG_INFLUXDB_URI,
                                   .event_handler = http_event_handler,
                                   .user_data = local_response_buffer,
                                   .method = HTTP_METHOD_POST};

  esp_http_client_handle_t client = esp_http_client_init(&conf);
  esp_http_client_set_post_field(client, data, strlen(data));

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(HTTP_TAG, "HTTP POST Status = %d, content_length = %d",
             status_code, esp_http_client_get_content_length(client));
  } else {
    ESP_LOGE(HTTP_TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
  }

  err = esp_http_client_cleanup(client);
  if (err != ESP_OK) {
    ESP_LOGE(HTTP_TAG, "HTTP client cleanup failed: %s", esp_err_to_name(err));
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "Startup..");
  ESP_LOGI(TAG, "Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_LOGI(TAG, "Configured WiFi SSID is %s\n", CONFIG_ESP_WIFI_SSID);
  init_wifi();

#if CONFIG_ENABLE_BME280_SENSOR
  bme280_init();
#endif

#ifdef CONFIG_ENABLE_BME680_SENSOR
  bme680_init();
#endif
}