#pragma once
#include <cstdint>
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 } wifi_auth_mode_t;
class IPAddress {};
class WiFiClient {};
class WiFiServer {
public:
    explicit WiFiServer(std::uint16_t) {}
};
