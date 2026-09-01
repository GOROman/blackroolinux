idf_component_register(
    SRCS "main.c" "esp_hid_gap.c"
    INCLUDE_DIRS "."
    REQUIRES bt driver esp_hid
)
