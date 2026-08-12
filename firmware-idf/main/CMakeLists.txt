idf_component_register(
    SRCS
        "main.cpp"
        "device_id.cpp"
        "node_role.cpp"
        "bno08x.cpp"
        "espnow_link.cpp"
        "espnow_probe.cpp"
        "imu_frame.cpp"
        "leaf.cpp"
        "primary.cpp"
        "gateway.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_app_format
        esp_driver_gpio
        esp_driver_spi
        esp_event
        esp_netif
        esp_wifi
        esp_hw_support
        esp_timer
        nvs_flash
        sh2
)

# ALL THREE role sources are compiled into every image, even though only the
# selected role's entry point is called (see main.cpp). The role is a Kconfig
# choice, so conditional registration would mean two of the three roles are
# never compiled in any given build -- and a break in one of them would only
# surface when someone happened to select that role. Compiling all three keeps
# `idf.py build` a check on the whole tree; the unselected ones cost a few
# hundred bytes of stub code, and once the roles carry real drivers the linker
# drops what is unreachable from app_main.
