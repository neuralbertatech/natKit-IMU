idf_component_register(
    SRCS
        "main.cpp"
        "device_id.cpp"
        "status_led.cpp"
        "node_role.cpp"
        "bno08x.cpp"
        "espnow_link.cpp"
        "espnow_probe.cpp"
        "imu_frame.cpp"
        "uplink.cpp"
        "registry.cpp"
        "uplink_reader.cpp"
        "gateway_net.cpp"
        "ethernet_net.cpp"
        "channel_survey.cpp"
        "time_sync.cpp"
        "leaf.cpp"
        "command_relay.cpp"
        "commands.cpp"
        "primary.cpp"
        "gateway.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_app_format
        esp_driver_gpio
        esp_driver_spi
        # The die-temperature sensor, for the primary's transmit-path investigation
        # (TEC-NATKIT-50). Guarded by SOC_TEMP_SENSOR_SUPPORTED in primary.cpp, but
        # the REQUIRES has to be unconditional: the component list is evaluated
        # before any target-specific header is visible.
        esp_driver_tsens
        esp_driver_uart
        esp_event
        esp_netif
        esp_eth
        esp_wifi
        esp_hw_support
        esp_timer
        nvs_flash
        mqtt
        json
        lwip
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
