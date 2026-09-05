# esps_core: node identity, health/heartbeat, log hook, time sync. Requires
# esps_proto for frame/struct packing (never re-implements it), nvs_flash for
# identity persistence, esp_timer for the monotonic clock, esp_system for
# reset-reason/heap introspection, and esp_hw_support for the factory MAC.
idf_component_register(
    SRCS "src/esps_node_id.c" "src/esps_frame.c" "src/esps_health.c"
         "src/esps_log_hook.c" "src/esps_time.c"
    INCLUDE_DIRS "include"
    REQUIRES esps_proto nvs_flash esp_timer esp_system esp_hw_support log
)
