# claudi_net — Wi-Fi + mDNS + HTTP ingest.
idf_component_register(
    SRCS "claudi_net.c"
    INCLUDE_DIRS "include"
    REQUIRES claudi_core esp_wifi esp_netif esp_event esp_http_server nvs_flash json esp_timer
)
