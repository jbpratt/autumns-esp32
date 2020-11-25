#ifdef CONFIG_ENABLE_BME680_SENSOR
#include "bme680_sensor.h"
static const char *BME680_TAG = "BME680";

// I2C interface defintions for ESP32
#define I2C_BUS 0
#define I2C_SCL_PIN 22
#define I2C_SDA_PIN 21
#define I2C_FREQ I2C_FREQ_100K

// user task stack depth for ESP32
#define TASK_STACK_DEPTH 2048

static bme680_sensor_t *sensor = 0;

void user_task(void *pvParameters) {
  bme680_values_float_t values;

  TickType_t last_wakeup = xTaskGetTickCount();

  // as long as sensor configuration isn't changed, duration is constant
  uint32_t duration = bme680_get_measurement_duration(sensor);

  while (1) {
    // trigger the sensor to start one TPHG measurement cycle
    if (bme680_force_measurement(sensor)) {
      // passive waiting until measurement results are available
      vTaskDelay(duration);

      // alternatively: busy waiting until measurement results are available
      // while (bme680_is_measuring (sensor)) ;

      // get the results and do something with them
      if (bme680_get_results_float(sensor, &values))
        ESP_LOGI(BME680_TAG,
                 "%.3f BME680 Sensor: %.2f °C, %.2f %%, %.2f hPa, %.2f Ohm\n",
                 (double)sdk_system_get_time() * 1e-3, values.temperature,
                 values.humidity, values.pressure, values.gas_resistance);
    }
    // passive waiting until 1 second is over
    vTaskDelayUntil(&last_wakeup, 1000 / portTICK_PERIOD_MS);
  }
}

void bme680_init(void) {
  // Set UART Parameter.
  uart_set_baud(0, 115200);
  // Give the UART some time to settle
  vTaskDelay(1);

  // Init all I2C bus interfaces at which BME680 sensors are connected
  i2c_init(I2C_BUS, I2C_SCL_PIN, I2C_SDA_PIN, I2C_FREQ);

  // init the sensor with slave address BME680_I2C_ADDRESS_2 connected to
  // I2C_BUS.
  sensor = bme680_init_sensor(I2C_BUS, BME680_I2C_ADDRESS_2, 0);

  if (sensor) {
    // Changes the oversampling rates to 4x oversampling for temperature
    // and 2x oversampling for humidity. Pressure measurement is skipped.
    bme680_set_oversampling_rates(sensor, osr_4x, osr_none, osr_2x);

    // Change the IIR filter size for temperature and pressure to 7.
    bme680_set_filter_size(sensor, iir_size_7);

    // Change the heater profile 0 to 200 degree Celcius for 100 ms.
    bme680_set_heater_profile(sensor, 0, 200, 100);
    bme680_use_heater_profile(sensor, 0);

    // Set ambient temperature to 10 degree Celsius
    bme680_set_ambient_temperature(sensor, 10);

    // must be done last to avoid concurrency situations with the sensor
    // configuration part

    // Create a task that uses the sensor
    xTaskCreate(user_task, "user_task", TASK_STACK_DEPTH, NULL, 2, NULL);
  } else
    ESP_LOGE(BME680_TAG, "Could not initialize BME680 sensor\n");
}
#endif
