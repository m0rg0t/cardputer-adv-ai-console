#include <esp_err.h>
#include <esp_wifi.h>

// M5Apps labels its shared NVS partition "apps_nvs" instead of the default
// "nvs" expected by Arduino-ESP32's WIFI_INIT_CONFIG_DEFAULT(). The firmware
// keeps every credential on microSD, so Wi-Fi can safely run with persistence
// disabled. Linker wrapping lets the standard Arduino WiFi API keep managing
// netifs, events, scanning, and sockets without modifying the global framework.
extern "C" esp_err_t __real_esp_wifi_init(
    const wifi_init_config_t* config);

extern "C" esp_err_t __wrap_esp_wifi_init(
    const wifi_init_config_t* config)
{
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_init_config_t compatible = *config;
    compatible.nvs_enable = 0;
    return __real_esp_wifi_init(&compatible);
}
