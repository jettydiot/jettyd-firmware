idf_component_register(
    SRCS "hcsr04.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer jettyd freertos
)
