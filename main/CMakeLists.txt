idf_component_register(SRCS "main.c" "wifi.c" "bme680_sensor.c"
                    INCLUDE_DIRS ""
                    EMBED_TXTFILES ${project_dir}/certs/ca_cert.pem)