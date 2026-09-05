idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esps_proto esps_core esps_link nvs_flash driver esp_timer esp_system
              esp_hw_support json log freertos
)
