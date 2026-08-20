#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <functional>

#include "recorder/hardware/storage_service.h"
#include "recorder/upload_service.h"

namespace cardputer_recorder {

// Small same-origin administration UI for the local network. File access is
// deliberately gated by RecorderApp so the HTTP client never contends with
// recording or playback for the microSD bus.
class WebService {
public:
    using JsonWriter = std::function<void(JsonObject)>;
    using SettingsApplier =
        std::function<bool(JsonObjectConst, String& error)>;

    void begin(StorageService& storage, UploadService& uploader,
               JsonWriter writeStatus, JsonWriter writeSettings,
               SettingsApplier applySettings);
    void configure(bool enabled, const String& hostname);
    void update(bool fileIoAllowed);

    bool running() const;
    String address() const;

private:
    void startIfReady();
    void serviceMdns();
    void stop();
    void handleClient(WiFiClient& client);
    void sendResponse(WiFiClient& client, int status,
                      const char* contentType, const String& body,
                      const String& extraHeaders = "");
    void sendJson(WiFiClient& client, JsonDocument& document,
                  int status = 200);
    void sendError(WiFiClient& client, int status, const String& message);
    void handleStatus(WiFiClient& client);
    void handleSettingsGet(WiFiClient& client);
    void handleSettingsPost(WiFiClient& client, const String& body);
    void handleRecordings(WiFiClient& client);
    void handleRecordingDownload(WiFiClient& client, const String& name);
    bool safeRecordingName(const String& name) const;

    StorageService* storage_ = nullptr;
    UploadService* uploader_ = nullptr;
    JsonWriter writeStatus_;
    JsonWriter writeSettings_;
    SettingsApplier applySettings_;
    WiFiServer server_{80};
    WiFiUDP mdnsUdp_;
    String hostname_ = "recorder";
    bool enabled_ = true;
    bool serverStarted_ = false;
    bool mdnsStarted_ = false;
    bool fileIoAllowed_ = false;
};

}  // namespace cardputer_recorder
