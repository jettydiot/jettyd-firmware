idf_component_register(
    SRCS "camera.c"
    INCLUDE_DIRS "include"
    REQUIRES driver jettyd freertos esp_http_client mbedtls
)
