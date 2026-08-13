#include "recorder/upload_service.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <algorithm>
#include <esp_wifi.h>
#include <time.h>

namespace cardputer_recorder {
namespace {

constexpr const char* kConfigPath = "/AGENT.CFG";
constexpr const char* kLegacyConfigPath = "/VOICEAGENT.CFG";
constexpr const char* kManualWifiConfigPath = "/AGENT_WIFI.CFG";
constexpr const char* kManualWifiConfigTempPath = "/AGENT_WIFI.CFG.TMP";
constexpr const char* kCaPath = "/AGENT_CA.PEM";
constexpr const char* kLegacyCaPath = "/VOICE_CA.PEM";
constexpr const char* kSentLedgerPath = "/VOICEAGENT.SENT";
constexpr const char* kMetadataSuffix = ".AGENT.JSON";
constexpr std::size_t kMaxWifiNetworks = 5;
constexpr std::size_t kMaxTotalWifiNetworks = 10;
constexpr std::size_t kMaxVoiceProfiles = 6;
constexpr unsigned long kConnectTimeoutMs = 15000;
constexpr unsigned long kOfflineRetryMs = 30000;
constexpr unsigned long kUploadPollMs = 15000;
constexpr unsigned long kJobPollMs = 3000;
constexpr unsigned long kErrorRetryMs = 60000;
constexpr unsigned long kTlsClockTimeoutMs = 10000;
constexpr time_t kMinimumTlsTime = 1704067200;  // 2024-01-01 UTC

bool isSuccessStatus(int status)
{
    return (status >= 200 && status < 300) || status == 409;
}

std::uint16_t dnsU16(const std::uint8_t* data)
{
    return (static_cast<std::uint16_t>(data[0]) << 8U) | data[1];
}

bool skipDnsName(const std::uint8_t* data, std::size_t length,
                 std::size_t& offset)
{
    std::size_t steps = 0;
    while (offset < length && steps++ < 128) {
        const std::uint8_t label = data[offset++];
        if (label == 0) {
            return true;
        }
        if ((label & 0xC0U) == 0xC0U) {
            if (offset >= length) {
                return false;
            }
            ++offset;
            return true;
        }
        if (label > 63 || offset + label > length) {
            return false;
        }
        offset += label;
    }
    return false;
}

String readDnsName(const std::uint8_t* data, std::size_t length,
                   std::size_t offset, std::size_t depth = 0)
{
    String result;
    if (depth > 8) {
        return result;
    }
    while (offset < length) {
        const std::uint8_t label = data[offset++];
        if (label == 0) {
            break;
        }
        if ((label & 0xC0U) == 0xC0U) {
            if (offset >= length) {
                return String();
            }
            const std::size_t pointer =
                ((label & 0x3FU) << 8U) | data[offset];
            const String suffix = readDnsName(data, length, pointer, depth + 1);
            if (suffix.length() > 0) {
                if (result.length() > 0) {
                    result += '.';
                }
                result += suffix;
            }
            break;
        }
        if (label > 63 || offset + label > length) {
            return String();
        }
        if (result.length() > 0) {
            result += '.';
        }
        for (std::uint8_t index = 0; index < label; ++index) {
            result += static_cast<char>(data[offset++]);
        }
    }
    return result;
}

std::size_t appendDnsLabel(std::uint8_t* packet, std::size_t offset,
                           const char* label)
{
    const std::size_t length = strlen(label);
    packet[offset++] = static_cast<std::uint8_t>(length);
    memcpy(packet + offset, label, length);
    return offset + length;
}

}  // namespace

void UploadService::begin(StorageService& storage)
{
    storage_ = &storage;
    activeNetworkIndex_ = 0;
    WiFi.onEvent(
        [this](WiFiEvent_t, WiFiEventInfo_t info) {
            const std::uint16_t reason =
                info.wifi_sta_disconnected.reason;
            if (reason != WIFI_REASON_ASSOC_LEAVE) {
                lastDisconnectReason_.store(reason,
                                            std::memory_order_relaxed);
                Serial.printf("[UPLOAD] Wi-Fi disconnected: %u %s\n",
                              reason,
                              WiFi.disconnectReasonName(
                                  static_cast<wifi_err_reason_t>(reason)));
            }
        },
        ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent(
        [this](WiFiEvent_t, WiFiEventInfo_t) {
            lastDisconnectReason_.store(0, std::memory_order_relaxed);
            Serial.printf("[UPLOAD] Wi-Fi connected: %s, IP %s\n",
                          WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str());
        },
        ARDUINO_EVENT_WIFI_STA_GOT_IP);
    restartWifiStation();
    status_ = loadConfig() ? Status::kOffline : Status::kDisabled;
    nextActionMs_ = millis() + 1000;
}

void UploadService::update(bool ioAllowed)
{
    if (backgroundUploadActive_.load(std::memory_order_acquire)) {
        return;
    }
    if (!ioAllowed || storage_ == nullptr ||
        !storage_->isMounted() || millis() < nextActionMs_) {
        return;
    }

    if (!config_.valid()) {
        status_ = Status::kDisabled;
        nextActionMs_ = millis() + kErrorRetryMs;
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (status_ != Status::kConnecting) {
            connectWifi();
            return;
        }
        if (millis() - connectStartedMs_ < kConnectTimeoutMs) {
            nextActionMs_ = millis() + 250;
            return;
        }
        WiFi.disconnect(false, false);
        activeNetworkIndex_ =
            (activeNetworkIndex_ + 1) % config_.wifiNetworks.size();
        const bool completedCycle = activeNetworkIndex_ == 0;
        status_ = Status::kOffline;
        nextActionMs_ = millis() +
                        (completedCycle ? kOfflineRetryMs : 1000);
        return;
    }

    status_ = Status::kReady;
    if (config_.gatewayBaseUrl == "auto" && !discoverGateway()) {
        status_ = Status::kError;
        nextActionMs_ = millis() + kOfflineRetryMs;
        return;
    }
    if (pendingManualSsid_.length() > 0 &&
        WiFi.SSID() == pendingManualSsid_) {
        if (persistManualNetworks()) {
            pendingManualSsid_ = "";
        }
    }
    String name;
    std::uint32_t size = 0;
    RecordingMetadata awaiting;
    if (findAwaitingJob(name, size, awaiting)) {
        status_ = Status::kUploading;
        activeUploadName_ = name;
        const bool polled = pollVoiceJob(name, awaiting.voiceJobId, size);
        activeUploadName_ = "";
        if (!polled) {
            lastFailedUploadName_ = name;
            status_ = Status::kError;
            nextActionMs_ = millis() + kErrorRetryMs;
        } else {
            status_ = Status::kReady;
            if (awaiting.status == "failed") {
                nextFailedPollMs_ = millis() + 60000;
            }
            nextActionMs_ = millis() + kJobPollMs;
        }
        return;
    }

    String path;
    if (!findPending(path, name, size)) {
        nextActionMs_ = millis() + kUploadPollMs;
        return;
    }

    if (lastFailedUploadName_ == name) {
        lastFailedUploadName_ = "";
    }
    if (!startBackgroundUpload(path, name, size)) {
        lastFailedUploadName_ = name;
        status_ = Status::kError;
        nextActionMs_ = millis() + kErrorRetryMs;
    }
}

bool UploadService::startBackgroundUpload(const String& path,
                                          const String& name,
                                          std::uint32_t size)
{
    bool expected = false;
    if (!backgroundUploadActive_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    backgroundUploadPath_ = path;
    backgroundUploadName_ = name;
    backgroundUploadSize_ = size;
    activeUploadName_ = name;
    status_ = Status::kUploading;
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(uploadTaskEntry, "voice_upload", 8192,
                                this, 1, &task, 0) != pdPASS) {
        activeUploadName_ = "";
        backgroundUploadActive_.store(false, std::memory_order_release);
        lastGatewayDiagnostic_ = "UPLOAD TASK FAILED";
        return false;
    }
    return true;
}

void UploadService::uploadTaskEntry(void* context)
{
    static_cast<UploadService*>(context)->backgroundUpload();
}

void UploadService::backgroundUpload()
{
    const String path = backgroundUploadPath_;
    const String name = backgroundUploadName_;
    const std::uint32_t size = backgroundUploadSize_;
    const bool delivered = upload(path, name, size);
    RecordingMetadata submitted;
    const bool completed = delivered && recordingMetadata(name, submitted) &&
                           submitted.status == "completed";
    const bool recorded = completed && markSent(name, size);
    activeUploadName_ = "";
    if (completed && !recorded) {
        lastGatewayDiagnostic_ = "UPLOAD OK; SD LEDGER ERROR";
    }
    if (recorded) {
        recordingChanged_ = true;
        lastFailedUploadName_ = "";
        status_ = Status::kReady;
        nextActionMs_ = millis() + 1000;
    } else if (delivered) {
        recordingChanged_ = true;
        lastFailedUploadName_ = "";
        status_ = Status::kReady;
        nextActionMs_ = millis() + kJobPollMs;
    } else {
        lastFailedUploadName_ = name;
        status_ = Status::kError;
        nextActionMs_ = millis() + kErrorRetryMs;
    }
    backgroundUploadPath_ = "";
    backgroundUploadName_ = "";
    backgroundUploadSize_ = 0;
    backgroundUploadActive_.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void UploadService::requestSoon()
{
    if (!wifiConnected() && config_.valid()) {
        WiFi.disconnect(false, false);
        activeNetworkIndex_ = 0;
        status_ = Status::kOffline;
    }
    nextActionMs_ = millis() + 1000;
}

String UploadService::shortStatus() const
{
    if (transferActive()) {
        return "UPLOADING";
    }
    switch (status_) {
        case Status::kOffline:
            return "NET OFF";
        case Status::kConnecting:
            return "NET ...";
        case Status::kReady:
            return "NET OK";
        case Status::kUploading:
            return "UPLOADING";
        case Status::kError:
            return lastHttpStatus_ > 0
                       ? "NET E" + String(lastHttpStatus_)
                       : "NET ERR";
        default:
            return "UPLOAD OFF";
    }
}

bool UploadService::configured() const
{
    return config_.valid();
}

bool UploadService::wifiConnected() const
{
    return WiFi.status() == WL_CONNECTED;
}

bool UploadService::transferActive() const
{
    return backgroundUploadActive_.load(std::memory_order_acquire);
}

String UploadService::wifiSsid() const
{
    if (wifiConnected()) {
        return WiFi.SSID();
    }
    if (config_.wifiNetworks.empty()) {
        return "--";
    }
    return config_.wifiNetworks[activeNetworkIndex_].ssid;
}

String UploadService::wifiProfileText() const
{
    if (config_.wifiNetworks.empty()) {
        return "0/0";
    }
    return String(activeNetworkIndex_ + 1) + "/" +
           String(config_.wifiNetworks.size());
}

String UploadService::wifiDisconnectReason() const
{
    const std::uint16_t reason =
        lastDisconnectReason_.load(std::memory_order_relaxed);
    if (reason == 0) {
        return "NONE";
    }
    const char* name = WiFi.disconnectReasonName(
        static_cast<wifi_err_reason_t>(reason));
    return String(name != nullptr && name[0] != '\0' ? name : "REASON") +
           " (" + String(reason) + ")";
}

const char* UploadService::authModeText(wifi_auth_mode_t authMode)
{
    switch (authMode) {
        case WIFI_AUTH_OPEN:
            return "OPEN";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";
        default:
            return "SECURE";
    }
}

bool UploadService::scanWifiNetworks(
    std::vector<WifiScanResult>& networks)
{
    networks.clear();
    lastScanResults_.clear();

    // ESP-IDF refuses to start a scan while the station is still associating.
    // The uploader normally spends 15 seconds in exactly that state, so pause
    // an unfinished attempt before entering the user-requested scan.
    if (WiFi.status() == WL_NO_SHIELD ||
        !(WiFi.getMode() & WIFI_MODE_STA)) {
        restartWifiStation();
    }
    const bool wasConnected = WiFi.status() == WL_CONNECTED;
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        esp_wifi_scan_stop();
        for (int retry = 0;
             retry < 20 &&
             WiFi.scanComplete() == WIFI_SCAN_RUNNING;
             ++retry) {
            delay(25);
        }
    }
    WiFi.scanDelete();
    if (!wasConnected) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        delay(150);
    }
    bool modeReady = WiFi.mode(WIFI_STA);
    delay(100);
    int count = WiFi.scanNetworks(false, true, false, 200);

    // A stale driver state can survive the first cancellation. Reset it once
    // and retry, while preserving an existing working connection whenever the
    // normal connected scan succeeds.
    if (count < 0) {
        modeReady = restartWifiStation();
        count = WiFi.scanNetworks(false, true, false, 200);
    }
    WiFi.setAutoReconnect(true);
    if (count < 0) {
        lastWifiScanDiagnostic_ =
            String(count == WIFI_SCAN_RUNNING ? "BUSY" : "DRIVER") +
            " (" + String(count) + ") WL=" +
            String(static_cast<int>(WiFi.status())) + " M=" +
            String(static_cast<int>(WiFi.getMode())) + " S=" +
            String(modeReady ? 1 : 0);
        status_ = wifiConnected() ? Status::kReady : Status::kOffline;
        if (!wifiConnected()) {
            nextActionMs_ = millis() + 250;
        }
        Serial.println("[UPLOAD] Wi-Fi scan: " +
                       lastWifiScanDiagnostic_);
        return false;
    }
    lastWifiScanDiagnostic_ = "OK " + String(count);
    for (int index = 0; index < count; ++index) {
        const String ssid = WiFi.SSID(index);
        if (ssid.length() == 0) {
            continue;
        }
        WifiScanResult candidate;
        candidate.ssid = ssid;
        candidate.rssi = WiFi.RSSI(index);
        candidate.channel = static_cast<std::uint8_t>(WiFi.channel(index));
        const wifi_auth_mode_t auth = WiFi.encryptionType(index);
        candidate.auth = authModeText(auth);
        candidate.open = auth == WIFI_AUTH_OPEN;

        auto existing = std::find_if(
            networks.begin(), networks.end(),
            [&ssid](const WifiScanResult& item) {
                return item.ssid == ssid;
            });
        if (existing == networks.end()) {
            networks.push_back(candidate);
        } else if (candidate.rssi > existing->rssi) {
            *existing = candidate;
        }
    }
    WiFi.scanDelete();
    std::sort(networks.begin(), networks.end(),
              [](const WifiScanResult& left,
                 const WifiScanResult& right) {
                  return left.rssi > right.rssi;
              });
    lastScanResults_ = networks;
    status_ = wifiConnected() ? Status::kReady : Status::kOffline;
    if (!wifiConnected()) {
        nextActionMs_ = millis() + 250;
    }
    Serial.println("[UPLOAD] Wi-Fi scan: " +
                   lastWifiScanDiagnostic_);
    return true;
}

bool UploadService::restartWifiStation()
{
    WiFi.setAutoReconnect(false);
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        esp_wifi_scan_stop();
        for (int retry = 0;
             retry < 20 &&
             WiFi.scanComplete() == WIFI_SCAN_RUNNING;
             ++retry) {
            delay(25);
        }
    }
    WiFi.scanDelete();
    if (WiFi.getMode() & WIFI_MODE_STA) {
        WiFi.disconnect(false, false);
    }
    const bool stopped = WiFi.mode(WIFI_OFF);
    delay(150);
    const bool started = WiFi.mode(WIFI_STA);
    for (int retry = 0;
         retry < 60 && WiFi.status() == WL_NO_SHIELD;
         ++retry) {
        delay(25);
    }
    const int mode = static_cast<int>(WiFi.getMode());
    const int status = static_cast<int>(WiFi.status());
    const bool ready = started && (mode & WIFI_MODE_STA);
    lastWifiScanDiagnostic_ =
        String(ready ? "READY" : "INIT FAIL") + " O=" +
        String(stopped ? 1 : 0) + " S=" + String(started ? 1 : 0) +
        " M=" + String(mode) + " WL=" + String(status);
    Serial.println("[UPLOAD] Wi-Fi init: " +
                   lastWifiScanDiagnostic_);
    return ready;
}

String UploadService::wifiScanDiagnostic() const
{
    return lastWifiScanDiagnostic_;
}

String UploadService::wifiObservation() const
{
    if (wifiConnected()) {
        return "LIVE " + String(WiFi.RSSI()) + "dBm CH" +
               String(WiFi.channel());
    }
    if (lastScanResults_.empty()) {
        return "NOT SCANNED";
    }
    const String target = wifiSsid();
    for (const auto& network : lastScanResults_) {
        if (network.ssid == target) {
            return "SEEN " + String(network.rssi) + "dBm CH" +
                   String(network.channel) + " " + network.auth;
        }
    }
    return "NOT SEEN IN LAST SCAN";
}

bool UploadService::connectScannedNetwork(const String& ssid,
                                          const String& password)
{
    if (ssid.length() == 0) {
        return false;
    }
    std::size_t selected = config_.wifiNetworks.size();
    std::size_t manualCount = 0;
    for (std::size_t index = 0; index < config_.wifiNetworks.size(); ++index) {
        if (config_.wifiNetworks[index].manual) {
            ++manualCount;
        }
        if (config_.wifiNetworks[index].ssid == ssid) {
            selected = index;
            if (!config_.wifiNetworks[index].manual &&
                manualCount >= kMaxWifiNetworks) {
                return false;
            }
            config_.wifiNetworks[index].password = password;
            config_.wifiNetworks[index].manual = true;
            break;
        }
    }
    if (selected == config_.wifiNetworks.size()) {
        if (config_.wifiNetworks.size() >= kMaxTotalWifiNetworks ||
            manualCount >= kMaxWifiNetworks) {
            return false;
        }
        config_.wifiNetworks.push_back(WifiNetwork{ssid, password, true});
        selected = config_.wifiNetworks.size() - 1;
    }
    pendingManualSsid_ = ssid;
    activeNetworkIndex_ = selected;
    lastDisconnectReason_.store(0, std::memory_order_relaxed);
    WiFi.disconnect(false, false);
    status_ = Status::kOffline;
    nextActionMs_ = millis() + 250;
    return true;
}

String UploadService::gatewayBaseUrl() const
{
    return config_.gatewayBaseUrl;
}

String UploadService::gatewayDiagnostic() const
{
    return lastGatewayDiagnostic_;
}

String UploadService::currentVoiceProfile() const
{
    if (config_.voiceProfiles.empty()) {
        return "default";
    }
    return config_.voiceProfiles[
        min(activeVoiceProfileIndex_, config_.voiceProfiles.size() - 1)];
}

String UploadService::cycleVoiceProfile(int offset)
{
    if (config_.voiceProfiles.empty()) {
        return "default";
    }
    const int count = static_cast<int>(config_.voiceProfiles.size());
    activeVoiceProfileIndex_ = static_cast<std::size_t>(
        (static_cast<int>(activeVoiceProfileIndex_) + count + offset) % count);
    return currentVoiceProfile();
}

const UploadService::GatewayServices& UploadService::gatewayServices() const
{
    return gatewayServices_;
}

std::size_t UploadService::pendingRecordingCount()
{
    if (storage_ == nullptr || !storage_->isMounted()) {
        return 0;
    }
    std::size_t count = 0;
    File directory = storage_->open("/", FILE_READ);
    if (!directory || !directory.isDirectory()) {
        return 0;
    }
    File entry = directory.openNextFile();
    while (entry) {
        String name = entry.name();
        if (name.startsWith("/")) {
            name.remove(0, 1);
        }
        String lower = name;
        lower.toLowerCase();
        const std::uint32_t size = static_cast<std::uint32_t>(entry.size());
        const bool pending = !entry.isDirectory() && lower.endsWith(".wav") &&
                             size > 44 && !wasSent(name, size);
        entry.close();
        if (pending) {
            ++count;
        }
        entry = directory.openNextFile();
    }
    directory.close();
    return count;
}

String UploadService::localIp() const
{
    return wifiConnected() ? WiFi.localIP().toString() : "--";
}

std::uint16_t UploadService::lastHttpStatus() const
{
    return lastHttpStatus_;
}

String UploadService::recordingMetadataPath(const String& filename) const
{
    String base = filename;
    if (base.startsWith("/")) {
        base.remove(0, 1);
    }
    const int extension = base.lastIndexOf('.');
    if (extension >= 0) {
        base.remove(extension);
    }
    return "/" + base + kMetadataSuffix;
}

bool UploadService::recordingMetadata(const String& filename,
                                      RecordingMetadata& metadata)
{
    metadata = RecordingMetadata{};
    if (storage_ == nullptr || !storage_->isMounted()) {
        return false;
    }
    File file = storage_->open(recordingMetadataPath(filename).c_str(),
                               FILE_READ);
    if (!file) {
        return false;
    }
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, file);
    file.close();
    if (error) {
        return false;
    }
    metadata.status = document["status"] | "delivered";
    metadata.stage = document["stage"] | "";
    metadata.destination = document["destination"] | "note";
    metadata.transcript = document["transcript"] | "";
    metadata.formatted = document["formatted"] | "";
    metadata.notePath = document["note_path"] | "";
    metadata.sha256 = document["sha256"] | "";
    metadata.voiceJobId = document["voice_job_id"] | "";
    metadata.codexJobId = document["codex_job_id"] | "";
    // Compatibility with metadata produced by firmware 2.5 and older.
    if (metadata.codexJobId.length() == 0) {
        metadata.codexJobId = document["job_id"] | "";
    }
    metadata.error = document["error"] | "";
    metadata.profile = document["profile"] | "default";
    metadata.progress = document["progress"] | 0;
    metadata.attempts = document["attempts"] | 0;
    metadata.transcriptComplete = document["transcript_complete"] | true;
    return true;
}

String UploadService::recordingStatus(const String& filename,
                                      std::uint32_t size)
{
    RecordingMetadata metadata;
    if (recordingMetadata(filename, metadata)) {
        if (metadata.status == "failed") {
            return "ERROR";
        }
        if (metadata.status == "canceled") {
            return "STOP";
        }
        if (metadata.status == "completed") {
            return metadata.transcript.length() > 0 ? "TEXT" : "SENT";
        }
        if (metadata.voiceJobId.length() > 0) {
            return String(metadata.progress) + "%";
        }
    }
    if (activeUploadName_ == filename) {
        return "WORK";
    }
    if (lastFailedUploadName_ == filename) {
        return "ERROR";
    }
    return wasSent(filename, size) ? "SENT" : "QUEUE";
}

String UploadService::recordingDeliveryDetail(const String& filename,
                                              std::uint32_t size)
{
    RecordingMetadata metadata;
    if (recordingMetadata(filename, metadata)) {
        String detail = "Submission: " + metadata.status;
        if (metadata.stage.length() > 0) {
            detail += "\nStage: " + metadata.stage + " (" +
                      String(metadata.progress) + "%)";
        }
        detail += "\nAttempts: " + String(metadata.attempts);
        if (metadata.error.length() > 0) {
            detail += "\nError: " + metadata.error;
        }
        detail += metadata.transcript.length() > 0
                      ? "\nTranscription: completed"
                      : "\nTranscription: waiting";
        if (metadata.notePath.length() > 0) {
            detail += "\nObsidian: saved";
        }
        if (metadata.codexJobId.length() > 0) {
            detail += "\nCodex: submitted";
        }
        return detail;
    }
    if (wasSent(filename, size)) {
        return "Submission: accepted by gateway"
               "\nTranscription: result not cached by old firmware";
    }
    if (activeUploadName_ == filename) {
        return "Submission: in progress"
               "\nTranscription: gateway is processing audio";
    }
    String detail = "Submission: not sent (queued on SD)";
    if (!wifiConnected()) {
        detail += "\nNetwork: offline";
    } else if (lastFailedUploadName_ == filename) {
        detail += "\nLast attempt: failed";
        detail += "\nGateway: " + lastGatewayDiagnostic_;
    } else {
        detail += "\nNetwork: connected to " + wifiSsid();
        detail += "\nGateway: " + lastGatewayDiagnostic_;
    }
    return detail;
}

bool UploadService::takeRecordingChanged()
{
    return recordingChanged_.exchange(false, std::memory_order_acq_rel);
}

String UploadService::takeLastJobId()
{
    const String result = lastSubmittedJobId_;
    lastSubmittedJobId_ = "";
    return result;
}

String UploadService::takeLastJobThreadId()
{
    const String result = lastSubmittedThreadId_;
    lastSubmittedThreadId_ = "";
    return result;
}

String UploadService::trimValue(String value)
{
    value.trim();
    if (value.length() >= 2 && value[0] == '"' &&
        value[value.length() - 1] == '"') {
        value = value.substring(1, value.length() - 1);
    }
    return value;
}

bool UploadService::loadConfig()
{
    File file = storage_->open(kConfigPath, FILE_READ);
    if (!file) {
        file = storage_->open(kLegacyConfigPath, FILE_READ);
        if (!file) {
            Serial.println("[UPLOAD] AGENT.CFG not found; uploads off.");
            return false;
        }
    }

    Config parsed;
    String wifiSsids[kMaxWifiNetworks];
    String wifiPasswords[kMaxWifiNetworks];
    String voiceProfiles[kMaxVoiceProfiles];
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) {
            continue;
        }
        const int separator = line.indexOf('=');
        if (separator <= 0) {
            continue;
        }
        String key = line.substring(0, separator);
        key.trim();
        key.toLowerCase();
        const String value = trimValue(line.substring(separator + 1));
        bool wifiKey = false;
        for (std::size_t index = 0; index < kMaxWifiNetworks; ++index) {
            const String suffix = index == 0 ? "" : "_" + String(index + 1);
            if (key == "wifi_ssid" + suffix) {
                wifiSsids[index] = value;
                wifiKey = true;
                break;
            }
            if (key == "wifi_password" + suffix) {
                wifiPasswords[index] = value;
                wifiKey = true;
                break;
            }
        }
        if (wifiKey) {
            continue;
        }
        bool profileKey = false;
        for (std::size_t index = 0; index < kMaxVoiceProfiles; ++index) {
            const String suffix = index == 0 ? "" : "_" + String(index + 1);
            if (key == "voice_profile" + suffix) {
                voiceProfiles[index] = value;
                voiceProfiles[index].toLowerCase();
                profileKey = true;
                break;
            }
        }
        if (profileKey) {
            continue;
        }
        if (key == "gateway_base_url" || key == "gateway_url") {
            parsed.gatewayBaseUrl = value;
        } else if (key == "gateway_fallback_url") {
            parsed.gatewayFallbackUrl = value;
        } else if (key == "gateway_discovery_scheme") {
            parsed.gatewayDiscoveryScheme = value;
            parsed.gatewayDiscoveryScheme.toLowerCase();
        } else if (key == "device_token") {
            parsed.deviceToken = value;
        } else if (key == "device_name") {
            parsed.deviceName = value;
        }
    }
    file.close();

    for (const String& profile : voiceProfiles) {
        if (profile.length() > 0 &&
            std::find(parsed.voiceProfiles.begin(), parsed.voiceProfiles.end(),
                      profile) == parsed.voiceProfiles.end()) {
            parsed.voiceProfiles.push_back(profile);
        }
    }
    if (parsed.voiceProfiles.empty()) {
        parsed.voiceProfiles.push_back("default");
        parsed.voiceProfiles.push_back("meeting");
        parsed.voiceProfiles.push_back("idea");
        parsed.voiceProfiles.push_back("task");
    }

    for (std::size_t index = 0; index < kMaxWifiNetworks; ++index) {
        if (wifiSsids[index].length() == 0) {
            continue;
        }
        bool duplicate = false;
        for (const auto& network : parsed.wifiNetworks) {
            if (network.ssid == wifiSsids[index]) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            parsed.wifiNetworks.push_back(
                WifiNetwork{wifiSsids[index], wifiPasswords[index], false});
        }
    }

    File manualFile = storage_->open(kManualWifiConfigPath, FILE_READ);
    if (manualFile) {
        String manualSsids[kMaxWifiNetworks];
        String manualPasswords[kMaxWifiNetworks];
        while (manualFile.available()) {
            String line = manualFile.readStringUntil('\n');
            line.trim();
            if (line.length() == 0 || line.startsWith("#")) {
                continue;
            }
            const int separator = line.indexOf('=');
            if (separator <= 0) {
                continue;
            }
            String key = line.substring(0, separator);
            key.trim();
            key.toLowerCase();
            const String value = trimValue(line.substring(separator + 1));
            for (std::size_t index = 0; index < kMaxWifiNetworks; ++index) {
                const String suffix =
                    index == 0 ? "" : "_" + String(index + 1);
                if (key == "wifi_ssid" + suffix) {
                    manualSsids[index] = value;
                    break;
                }
                if (key == "wifi_password" + suffix) {
                    manualPasswords[index] = value;
                    break;
                }
            }
        }
        manualFile.close();
        for (std::size_t index = 0; index < kMaxWifiNetworks; ++index) {
            if (manualSsids[index].length() == 0) {
                continue;
            }
            auto existing = std::find_if(
                parsed.wifiNetworks.begin(), parsed.wifiNetworks.end(),
                [&manualSsids, index](const WifiNetwork& network) {
                    return network.ssid == manualSsids[index];
                });
            if (existing != parsed.wifiNetworks.end()) {
                existing->password = manualPasswords[index];
                existing->manual = true;
            } else if (parsed.wifiNetworks.size() <
                       kMaxTotalWifiNetworks) {
                parsed.wifiNetworks.push_back(
                    WifiNetwork{manualSsids[index],
                                manualPasswords[index], true});
            }
        }
    }

    while (parsed.gatewayBaseUrl.endsWith("/")) {
        parsed.gatewayBaseUrl.remove(parsed.gatewayBaseUrl.length() - 1);
    }
    constexpr const char* legacyPaths[] = {"/v1/voice-notes", "/v1/voice"};
    for (const char* suffix : legacyPaths) {
        if (parsed.gatewayBaseUrl.endsWith(suffix)) {
            parsed.gatewayBaseUrl.remove(
                parsed.gatewayBaseUrl.length() - strlen(suffix));
            break;
        }
    }

    if (!parsed.valid()) {
        Serial.println("[UPLOAD] VOICEAGENT.CFG is incomplete.");
        return false;
    }
    config_ = parsed;
    Serial.printf("[UPLOAD] Configuration loaded with %u Wi-Fi profile(s).\n",
                  static_cast<unsigned int>(config_.wifiNetworks.size()));
    return true;
}

bool UploadService::persistManualNetworks()
{
    if (storage_ == nullptr || !storage_->isMounted()) {
        return false;
    }
    if (storage_->exists(kManualWifiConfigTempPath)) {
        storage_->remove(kManualWifiConfigTempPath);
    }
    File file = storage_->open(kManualWifiConfigTempPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    file.println("# Wi-Fi networks saved from Agent Console");
    std::size_t saved = 0;
    for (const auto& network : config_.wifiNetworks) {
        if (!network.manual || saved >= kMaxWifiNetworks) {
            continue;
        }
        const String suffix = saved == 0 ? "" : "_" + String(saved + 1);
        file.printf("wifi_ssid%s=\"%s\"\n", suffix.c_str(),
                    network.ssid.c_str());
        file.printf("wifi_password%s=\"%s\"\n", suffix.c_str(),
                    network.password.c_str());
        ++saved;
    }
    file.flush();
    file.close();
    if (storage_->exists(kManualWifiConfigPath) &&
        !storage_->remove(kManualWifiConfigPath)) {
        storage_->remove(kManualWifiConfigTempPath);
        return false;
    }
    return storage_->rename(kManualWifiConfigTempPath,
                            kManualWifiConfigPath);
}

bool UploadService::connectWifi()
{
    if (config_.wifiNetworks.empty()) {
        return false;
    }
    if (activeNetworkIndex_ >= config_.wifiNetworks.size()) {
        activeNetworkIndex_ = 0;
    }
    const WifiNetwork& network = config_.wifiNetworks[activeNetworkIndex_];
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    lastDisconnectReason_.store(0, std::memory_order_relaxed);
    WiFi.begin(network.ssid.c_str(), network.password.c_str());
    connectStartedMs_ = millis();
    status_ = Status::kConnecting;
    nextActionMs_ = millis() + 250;
    Serial.printf("[UPLOAD] Connecting to Wi-Fi profile %u/%u: %s\n",
                  static_cast<unsigned int>(activeNetworkIndex_ + 1),
                  static_cast<unsigned int>(config_.wifiNetworks.size()),
                  network.ssid.c_str());
    return true;
}

bool UploadService::discoverGateway()
{
    if (config_.gatewayBaseUrl != "auto") {
        return true;
    }
    constexpr std::size_t kPacketCapacity = 1024;
    std::uint8_t packet[kPacketCapacity] = {};
    packet[5] = 1;  // QDCOUNT
    std::size_t queryLength = 12;
    queryLength = appendDnsLabel(packet, queryLength, "_cardputer-agent");
    queryLength = appendDnsLabel(packet, queryLength, "_tcp");
    queryLength = appendDnsLabel(packet, queryLength, "local");
    packet[queryLength++] = 0;
    packet[queryLength++] = 0;
    packet[queryLength++] = 12;  // PTR
    packet[queryLength++] = 0x80;  // Ask for a unicast response.
    packet[queryLength++] = 1;

    WiFiUDP udp;
    if (!udp.begin(0)) {
        lastGatewayDiagnostic_ = "DISCOVERY UDP FAILED";
        return false;
    }
    const IPAddress multicast(224, 0, 0, 251);
    udp.beginPacket(multicast, 5353);
    udp.write(packet, queryLength);
    udp.endPacket();

    String host;
    String scheme = config_.gatewayDiscoveryScheme;
    std::uint16_t port = 0;
    const unsigned long started = millis();
    while (millis() - started < 2500 && (host.length() == 0 || port == 0)) {
        const int available = udp.parsePacket();
        if (available <= 0) {
            delay(20);
            continue;
        }
        const std::size_t received = static_cast<std::size_t>(
            udp.read(packet, min(available, static_cast<int>(kPacketCapacity))));
        if (received < 12) {
            continue;
        }
        const std::uint16_t questions = dnsU16(packet + 4);
        const std::uint16_t records = dnsU16(packet + 6) +
                                      dnsU16(packet + 8) +
                                      dnsU16(packet + 10);
        std::size_t offset = 12;
        bool valid = true;
        for (std::uint16_t index = 0; index < questions; ++index) {
            if (!skipDnsName(packet, received, offset) || offset + 4 > received) {
                valid = false;
                break;
            }
            offset += 4;
        }
        for (std::uint16_t index = 0; valid && index < records; ++index) {
            if (!skipDnsName(packet, received, offset) || offset + 10 > received) {
                break;
            }
            const std::uint16_t type = dnsU16(packet + offset);
            const std::uint16_t dataLength = dnsU16(packet + offset + 8);
            offset += 10;
            if (offset + dataLength > received) {
                break;
            }
            if (type == 33 && dataLength >= 7) {  // SRV
                port = dnsU16(packet + offset + 4);
                host = readDnsName(packet, received, offset + 6);
            } else if (type == 16 && dataLength > 1) {  // TXT
                std::size_t textOffset = offset;
                const std::size_t end = offset + dataLength;
                while (textOffset < end) {
                    const std::uint8_t textLength = packet[textOffset++];
                    if (textOffset + textLength > end) {
                        break;
                    }
                    String item;
                    for (std::uint8_t textIndex = 0;
                         textIndex < textLength; ++textIndex) {
                        item += static_cast<char>(packet[textOffset++]);
                    }
                    if (item.startsWith("scheme=")) {
                        scheme = item.substring(7);
                    }
                }
            }
            offset += dataLength;
        }
    }
    udp.stop();
    if (host.length() > 0 && port > 0) {
        scheme.toLowerCase();
        if (scheme != "http" && scheme != "https") {
            scheme = "https";
        }
        config_.gatewayBaseUrl = scheme + "://" + host + ":" + String(port);
        lastGatewayDiagnostic_ = "DISCOVERED " + config_.gatewayBaseUrl;
        Serial.println("[AGENT] " + lastGatewayDiagnostic_);
        return true;
    }
    if (config_.gatewayFallbackUrl.length() > 0) {
        config_.gatewayBaseUrl = config_.gatewayFallbackUrl;
        lastGatewayDiagnostic_ = "DISCOVERY FALLBACK " + config_.gatewayBaseUrl;
        return true;
    }
    lastGatewayDiagnostic_ = "GATEWAY DISCOVERY FAILED";
    return false;
}

bool UploadService::findPending(String& path, String& name,
                                std::uint32_t& size)
{
    File directory = storage_->open("/", FILE_READ);
    if (!directory || !directory.isDirectory()) {
        return false;
    }
    File entry = directory.openNextFile();
    while (entry) {
        String candidate = entry.name();
        if (candidate.startsWith("/")) {
            candidate.remove(0, 1);
        }
        String lower = candidate;
        lower.toLowerCase();
        const std::uint32_t candidateSize =
            static_cast<std::uint32_t>(entry.size());
        const bool wav = !entry.isDirectory() && lower.endsWith(".wav") &&
                         candidateSize > 44;
        entry.close();
        RecordingMetadata metadata;
        const bool hasServerJob =
            wav && recordingMetadata(candidate, metadata) &&
            metadata.voiceJobId.length() > 0;
        if (wav && !wasSent(candidate, candidateSize) && !hasServerJob) {
            directory.close();
            name = candidate;
            path = "/" + candidate;
            size = candidateSize;
            return true;
        }
        entry = directory.openNextFile();
    }
    directory.close();
    return false;
}

bool UploadService::findAwaitingJob(String& name, std::uint32_t& size,
                                    RecordingMetadata& metadata)
{
    File directory = storage_->open("/", FILE_READ);
    if (!directory || !directory.isDirectory()) {
        return false;
    }
    File entry = directory.openNextFile();
    while (entry) {
        String candidate = entry.name();
        if (candidate.startsWith("/")) {
            candidate.remove(0, 1);
        }
        String lower = candidate;
        lower.toLowerCase();
        const std::uint32_t candidateSize =
            static_cast<std::uint32_t>(entry.size());
        const bool wav = !entry.isDirectory() && lower.endsWith(".wav") &&
                         candidateSize > 44;
        entry.close();
        RecordingMetadata candidateMetadata;
        if (wav && !wasSent(candidate, candidateSize) &&
            recordingMetadata(candidate, candidateMetadata) &&
            candidateMetadata.voiceJobId.length() > 0 &&
            candidateMetadata.status != "completed" &&
            candidateMetadata.status != "canceled" &&
            (candidateMetadata.status != "failed" ||
             millis() >= nextFailedPollMs_)) {
            directory.close();
            name = candidate;
            size = candidateSize;
            metadata = candidateMetadata;
            return true;
        }
        entry = directory.openNextFile();
    }
    directory.close();
    return false;
}

bool UploadService::wasSent(const String& name, std::uint32_t size)
{
    File ledger = storage_->open(kSentLedgerPath, FILE_READ);
    if (!ledger) {
        return false;
    }
    const String expected = name + "|" + String(size);
    while (ledger.available()) {
        String line = ledger.readStringUntil('\n');
        line.trim();
        if (line == expected) {
            ledger.close();
            return true;
        }
    }
    ledger.close();
    return false;
}

bool UploadService::markSent(const String& name, std::uint32_t size)
{
    File ledger = storage_->open(kSentLedgerPath, FILE_APPEND);
    if (!ledger) {
        return false;
    }
    ledger.println(name + "|" + String(size));
    ledger.flush();
    ledger.close();
    return true;
}

bool UploadService::writeRecordingMetadata(
    const String& filename, const JsonDocument& response)
{
    if (storage_ == nullptr || !storage_->isMounted()) {
        return false;
    }
    const String target = recordingMetadataPath(filename);
    const String temporary = target + ".TMP";
    if (storage_->exists(temporary.c_str())) {
        storage_->remove(temporary.c_str());
    }
    File file = storage_->open(temporary.c_str(), FILE_WRITE);
    if (!file) {
        return false;
    }
    JsonDocument metadata;
    metadata["status"] = response["status"] | "delivered";
    metadata["stage"] = response["stage"] | "";
    metadata["destination"] = response["destination"] | "note";
    metadata["transcript"] = response["transcript"] | "";
    metadata["formatted"] = response["formatted"] | "";
    metadata["transcript_complete"] =
        response["transcript_complete"] | true;
    metadata["note_path"] = response["note_path"] | "";
    metadata["sha256"] = response["sha256"] | "";
    String voiceJobId = response["id"] | "";
    if (voiceJobId.length() == 0) {
        voiceJobId = response["voice_job_id"] | "";
    }
    metadata["voice_job_id"] = voiceJobId;
    metadata["codex_job_id"] = response["codex_job_id"] | "";
    metadata["error"] = response["error"] | "";
    metadata["profile"] = response["profile"] | "default";
    metadata["progress"] = response["progress"] | 0;
    metadata["attempts"] = response["attempts"] | 0;
    const std::size_t written = serializeJson(metadata, file);
    file.flush();
    file.close();
    if (written == 0) {
        storage_->remove(temporary.c_str());
        return false;
    }
    if (storage_->exists(target.c_str()) &&
        !storage_->remove(target.c_str())) {
        storage_->remove(temporary.c_str());
        return false;
    }
    return storage_->rename(temporary.c_str(), target.c_str());
}

bool UploadService::loadCaCertificate(String& pem)
{
    const char* paths[] = {kCaPath, kLegacyCaPath};
    for (const char* path : paths) {
        File file = storage_->open(path, FILE_READ);
        if (!file) {
            continue;
        }
        if (file.size() > 8192) {
            file.close();
            continue;
        }
        pem = file.readString();
        file.close();
        if (pem.indexOf("BEGIN CERTIFICATE") >= 0) {
            Serial.printf("[AGENT] Using gateway CA from %s\n", path);
            return true;
        }
    }
    pem = "";
    return false;
}

bool UploadService::ensureTlsClock()
{
    time_t now = 0;
    time(&now);
    if (now >= kMinimumTlsTime) {
        return true;
    }

    Serial.println("[AGENT] Synchronizing clock for TLS...");
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    const unsigned long started = millis();
    while (millis() - started < kTlsClockTimeoutMs) {
        delay(100);
        time(&now);
        if (now >= kMinimumTlsTime) {
            Serial.printf("[AGENT] TLS clock synchronized: %lld\n",
                          static_cast<long long>(now));
            return true;
        }
    }
    lastGatewayDiagnostic_ = "TLS CLOCK NOT SET";
    Serial.println("[AGENT] TLS clock synchronization failed.");
    return false;
}

bool UploadService::upload(const String& path, const String& name,
                           std::uint32_t size)
{
    File wav = storage_->open(path.c_str(), FILE_READ);
    if (!wav) {
        return false;
    }

    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    String caPem;
    const String url = apiUrl("/v1/voice/jobs");
    const bool began = beginHttp(
        http, plainClient, secureClient, url, caPem);
    if (!began) {
        wav.close();
        return false;
    }

    http.setConnectTimeout(10000);
    http.setTimeout(45000);
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("X-Voice-Filename", name);
    addAuthHeaders(http);
    Destination destination = Destination::kNote;
    String threadId;
    String profile;
    readRoute(path, destination, threadId, profile);
    http.addHeader("X-Voice-Destination", destinationText(destination));
    http.addHeader("X-Voice-Profile", profile);
    if (threadId.length() > 0) {
        http.addHeader("X-Codex-Thread-ID", threadId);
    }
    const int status = http.sendRequest("POST", &wav, size);
    const String responseBody = http.getString();
    rememberHttpResult("VOICE", status, responseBody);
    wav.close();
    http.end();
    bool metadataSaved = false;
    if (isSuccessStatus(status) && responseBody.length() > 0) {
        JsonDocument response;
        if (!deserializeJson(response, responseBody)) {
            metadataSaved = writeRecordingMetadata(name, response);
            lastSubmittedJobId_ = response["codex_job_id"] | "";
            if (lastSubmittedJobId_.length() > 0) {
                lastSubmittedThreadId_ = threadId;
            }
        }
    }
    Serial.printf("[UPLOAD] %s -> HTTP %d\n", name.c_str(), status);
    return isSuccessStatus(status) && metadataSaved;
}

bool UploadService::pollVoiceJob(const String& filename,
                                 const String& voiceJobId,
                                 std::uint32_t size)
{
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure,
                   apiUrl("/v1/voice/jobs/" + voiceJobId), caPem)) {
        return false;
    }
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    addAuthHeaders(http);
    const int code = http.GET();
    const String body = http.getString();
    rememberHttpResult("VOICE JOB", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument response;
    if (deserializeJson(response, body) ||
        !writeRecordingMetadata(filename, response)) {
        lastGatewayDiagnostic_ = "VOICE JOB INVALID RESPONSE";
        return false;
    }
    const String state = response["status"] | "";
    if (state == "completed") {
        if (!wasSent(filename, size) && !markSent(filename, size)) {
            lastGatewayDiagnostic_ = "JOB OK; SD LEDGER ERROR";
            return false;
        }
        lastSubmittedJobId_ = response["codex_job_id"] | "";
        recordingChanged_ = true;
    } else if (state == "failed" || state == "canceled") {
        lastFailedUploadName_ = filename;
        recordingChanged_ = true;
    }
    return true;
}

bool UploadService::controlVoiceJob(const String& filename,
                                    const char* action)
{
    RecordingMetadata metadata;
    if (!recordingMetadata(filename, metadata) ||
        metadata.voiceJobId.length() == 0 || !ensureOnline()) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    const String path = "/v1/voice/jobs/" + metadata.voiceJobId + "/" + action;
    if (!beginHttp(http, plain, secure, apiUrl(path), caPem)) {
        return false;
    }
    http.setTimeout(30000);
    addAuthHeaders(http);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST("{}");
    const String body = http.getString();
    rememberHttpResult(action, code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument response;
    if (deserializeJson(response, body) ||
        !writeRecordingMetadata(filename, response)) {
        return false;
    }
    recordingChanged_ = true;
    nextFailedPollMs_ = millis() + 250;
    nextActionMs_ = millis() + 250;
    return true;
}

bool UploadService::retryVoiceJob(const String& filename)
{
    lastFailedUploadName_ = "";
    return controlVoiceJob(filename, "retry");
}

bool UploadService::retryFailedVoiceJobs(std::size_t& retriedCount)
{
    retriedCount = 0;
    if (!ensureOnline()) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure,
                   apiUrl("/v1/voice/jobs/retry-failed"), caPem)) {
        return false;
    }
    http.setTimeout(30000);
    addAuthHeaders(http);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST("{}");
    const String body = http.getString();
    rememberHttpResult("RETRY ALL", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument response;
    if (deserializeJson(response, body)) {
        return false;
    }
    retriedCount = response["count"] | 0;
    lastFailedUploadName_ = "";
    recordingChanged_ = true;
    nextFailedPollMs_ = millis() + 250;
    nextActionMs_ = millis() + 250;
    return true;
}

bool UploadService::cancelVoiceJob(const String& filename)
{
    return controlVoiceJob(filename, "cancel");
}

bool UploadService::reprocessVoiceJob(const String& filename,
                                      const String& profile)
{
    RecordingMetadata metadata;
    if (!recordingMetadata(filename, metadata) ||
        metadata.voiceJobId.length() == 0 || !ensureOnline()) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    const String path =
        "/v1/voice/jobs/" + metadata.voiceJobId + "/reprocess";
    if (!beginHttp(http, plain, secure, apiUrl(path), caPem)) {
        return false;
    }
    http.setTimeout(30000);
    addAuthHeaders(http);
    http.addHeader("Content-Type", "application/json");
    JsonDocument request;
    request["profile"] = profile;
    String payload;
    serializeJson(request, payload);
    const int code = http.POST(payload);
    const String body = http.getString();
    rememberHttpResult("REPROCESS", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument response;
    if (deserializeJson(response, body) ||
        !writeRecordingMetadata(filename, response)) {
        return false;
    }
    recordingChanged_ = true;
    nextActionMs_ = millis() + 250;
    return true;
}

bool UploadService::ensureOnline(unsigned long timeoutMs)
{
    if (transferActive()) {
        lastGatewayDiagnostic_ = "AUDIO UPLOAD IN PROGRESS";
        return false;
    }
    if (!config_.valid()) {
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    connectWifi();
    const unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
        delay(50);
    }
    if (WiFi.status() == WL_CONNECTED) {
        status_ = Status::kReady;
        return true;
    }
    status_ = Status::kOffline;
    return false;
}

String UploadService::routePath(const String& wavPath) const
{
    String result = wavPath;
    const int extension = result.lastIndexOf('.');
    if (extension >= 0) {
        result.remove(extension);
    }
    result += ".ROUTE";
    return result;
}

const char* UploadService::destinationText(Destination destination)
{
    switch (destination) {
        case Destination::kCodex:
            return "codex";
        case Destination::kBoth:
            return "both";
        default:
            return "note";
    }
}

bool UploadService::setRoute(const String& wavPath,
                             Destination destination,
                             const String& threadId)
{
    if (storage_ == nullptr || !storage_->isMounted()) {
        return false;
    }
    const String sidecar = routePath(wavPath);
    if (storage_->exists(sidecar.c_str())) {
        storage_->remove(sidecar.c_str());
    }
    File file = storage_->open(sidecar.c_str(), FILE_WRITE);
    if (!file) {
        return false;
    }
    file.printf("destination=%s\n", destinationText(destination));
    file.printf("profile=%s\n", currentVoiceProfile().c_str());
    if (threadId.length() > 0) {
        file.printf("thread_id=%s\n", threadId.c_str());
    }
    file.flush();
    file.close();
    return true;
}

bool UploadService::readRoute(const String& wavPath,
                              Destination& destination,
                              String& threadId, String& profile)
{
    destination = Destination::kNote;
    threadId = "";
    profile = "default";
    if (storage_ == nullptr) {
        return false;
    }
    File file = storage_->open(routePath(wavPath).c_str(), FILE_READ);
    if (!file) {
        return false;
    }
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        const int separator = line.indexOf('=');
        if (separator <= 0) {
            continue;
        }
        String key = line.substring(0, separator);
        String value = trimValue(line.substring(separator + 1));
        key.toLowerCase();
        value.toLowerCase();
        if (key == "destination") {
            if (value == "codex") {
                destination = Destination::kCodex;
            } else if (value == "both") {
                destination = Destination::kBoth;
            }
        } else if (key == "thread_id") {
            threadId = trimValue(line.substring(separator + 1));
        } else if (key == "profile") {
            profile = trimValue(line.substring(separator + 1));
            profile.toLowerCase();
        }
    }
    file.close();
    return true;
}

String UploadService::apiUrl(const String& path) const
{
    return config_.gatewayBaseUrl + path;
}

bool UploadService::beginHttp(HTTPClient& http, WiFiClient& plain,
                              WiFiClientSecure& secure,
                              const String& url, String& caPem)
{
    if (url.startsWith("https://")) {
        if (!ensureTlsClock()) {
            return false;
        }
        time_t now = 0;
        time(&now);
        Serial.printf("[AGENT] HTTPS %s (epoch %lld)\n", url.c_str(),
                      static_cast<long long>(now));
        if (!loadCaCertificate(caPem)) {
            lastGatewayDiagnostic_ = "GATEWAY CA MISSING";
            Serial.println("[AGENT] Gateway CA is unavailable.");
            return false;
        }
        secure.setCACert(caPem.c_str());
        const bool began = http.begin(secure, url);
        if (!began) {
            lastGatewayDiagnostic_ = "HTTPS CLIENT INIT FAILED";
        }
        return began;
    }
    const bool began = url.startsWith("http://") && http.begin(plain, url);
    if (!began) {
        lastGatewayDiagnostic_ = "INVALID GATEWAY URL";
    }
    return began;
}

void UploadService::rememberHttpResult(const char* operation, int code,
                                       const String& responseBody)
{
    lastHttpCode_ = code;
    lastHttpStatus_ = code > 0 ? static_cast<std::uint16_t>(code) : 0;
    if (isSuccessStatus(code)) {
        lastGatewayDiagnostic_ = String(operation) + " OK HTTP " + String(code);
    } else if (code < 0) {
        lastGatewayDiagnostic_ = String(operation) + " " +
                                 HTTPClient::errorToString(code) + " (" +
                                 String(code) + ")";
    } else {
        String detail;
        if (responseBody.length() > 0) {
            JsonDocument response;
            if (!deserializeJson(response, responseBody)) {
                detail = response["detail"] | "";
            }
        }
        if (detail.length() > 70) {
            detail = detail.substring(0, 70);
        }
        lastGatewayDiagnostic_ = String(operation) + " HTTP " + String(code);
        if (detail.length() > 0) {
            lastGatewayDiagnostic_ += ": " + detail;
        }
    }
    Serial.println("[AGENT] " + lastGatewayDiagnostic_);
}

void UploadService::addAuthHeaders(HTTPClient& http)
{
    http.addHeader("X-Device-Name", config_.deviceName);
    http.addHeader("X-Device-Token", config_.deviceToken);
}

bool UploadService::refreshGatewayStatus()
{
    gatewayServices_ = GatewayServices{};
    if (!ensureOnline()) {
        gatewayServices_.overall = "OFFLINE";
        gatewayServices_.gateway = "NO WI-FI";
        lastGatewayDiagnostic_ = "STATUS NO WI-FI";
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure, apiUrl("/v1/status"), caPem)) {
        gatewayServices_.overall = "UNREACHABLE";
        gatewayServices_.gateway = lastGatewayDiagnostic_;
        return false;
    }
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    addAuthHeaders(http);
    const int code = http.GET();
    const String body = http.getString();
    rememberHttpResult("STATUS", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        gatewayServices_.overall = "UNREACHABLE";
        gatewayServices_.gateway = lastGatewayDiagnostic_;
        return false;
    }
    JsonDocument document;
    if (deserializeJson(document, body)) {
        gatewayServices_.overall = "INVALID RESPONSE";
        gatewayServices_.gateway = "INVALID JSON";
        lastGatewayDiagnostic_ = "STATUS INVALID JSON";
        return false;
    }
    auto component = [&document](const char* name) {
        const String state = document[name]["status"] | "unknown";
        const String detail = document[name]["detail"] | "";
        String result = state;
        result.toUpperCase();
        if (detail.length() > 0) {
            result += " " + detail;
        }
        return result;
    };
    gatewayServices_.overall = document["status"] | "unknown";
    gatewayServices_.overall.toUpperCase();
    gatewayServices_.gateway = component("gateway");
    gatewayServices_.whisper = component("whisper");
    gatewayServices_.codex = component("codex");
    gatewayServices_.formatter = component("formatter");
    return true;
}

bool UploadService::refreshChats(std::vector<CodexChat>& chats)
{
    chats.clear();
    if (!ensureOnline()) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure,
                   apiUrl("/v1/codex/chats?limit=10"), caPem)) {
        return false;
    }
    http.setTimeout(30000);
    addAuthHeaders(http);
    const int code = http.GET();
    const String body = http.getString();
    rememberHttpResult("CHATS", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument document;
    if (deserializeJson(document, body)) {
        lastGatewayDiagnostic_ = "CHATS INVALID JSON";
        Serial.println("[AGENT] " + lastGatewayDiagnostic_);
        return false;
    }
    for (JsonObject item : document["data"].as<JsonArray>()) {
        CodexChat chat;
        chat.id = item["id"] | "";
        chat.name = item["name"] | "Untitled";
        chat.preview = item["preview"] | "";
        chat.cwd = item["cwd"] | "";
        if (chat.id.length() > 0) {
            chats.push_back(chat);
        }
    }
    return true;
}

bool UploadService::createCodexChat(CodexChat& chat)
{
    chat = CodexChat{};
    if (!ensureOnline()) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure, apiUrl("/v1/codex/chats"), caPem)) {
        return false;
    }
    http.setTimeout(30000);
    addAuthHeaders(http);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST("{\"cwd\":\"\"}");
    const String body = http.getString();
    rememberHttpResult("NEW CHAT", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument response;
    if (deserializeJson(response, body)) {
        return false;
    }
    chat.id = response["id"] | "";
    chat.name = response["name"] | "Untitled";
    chat.preview = response["preview"] | "";
    chat.cwd = response["cwd"] | "";
    return chat.id.length() > 0;
}

bool UploadService::fetchConversation(const String& threadId, String& text)
{
    text = "";
    if (!ensureOnline()) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure,
                   apiUrl("/v1/codex/chats/" + threadId), caPem)) {
        return false;
    }
    http.setTimeout(30000);
    addAuthHeaders(http);
    const int code = http.GET();
    const String body = http.getString();
    rememberHttpResult("CHAT", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument document;
    if (deserializeJson(document, body)) {
        lastGatewayDiagnostic_ = "CHAT INVALID JSON";
        return false;
    }
    for (JsonObject item : document["messages"].as<JsonArray>()) {
        const String role = item["role"] | "";
        const String message = item["text"] | "";
        text += role == "user" ? "YOU: " : "CODEX: ";
        text += message;
        text += "\n\n";
    }
    if (text.length() > 6000) {
        text = text.substring(text.length() - 6000);
    }
    return true;
}

bool UploadService::sendCodexMessage(const String& threadId,
                                     const String& text, String& jobId)
{
    jobId = "";
    if (!ensureOnline()) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure,
                   apiUrl("/v1/codex/chats/" + threadId + "/messages"),
                   caPem)) {
        return false;
    }
    http.setTimeout(45000);
    addAuthHeaders(http);
    http.addHeader("Content-Type", "application/json");
    JsonDocument request;
    request["text"] = text;
    String payload;
    serializeJson(request, payload);
    const int code = http.POST(payload);
    const String body = http.getString();
    rememberHttpResult("MESSAGE", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument response;
    if (deserializeJson(response, body)) {
        lastGatewayDiagnostic_ = "MESSAGE INVALID JSON";
        return false;
    }
    jobId = response["id"] | "";
    return jobId.length() > 0;
}

bool UploadService::pollCodexJob(const String& jobId,
                                 CodexJobStatus& job)
{
    if (!ensureOnline(3000)) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure,
                   apiUrl("/v1/codex/jobs/" + jobId), caPem)) {
        return false;
    }
    http.setTimeout(15000);
    addAuthHeaders(http);
    const int code = http.GET();
    const String body = http.getString();
    rememberHttpResult("JOB", code, body);
    http.end();
    if (!isSuccessStatus(code)) {
        return false;
    }
    JsonDocument response;
    if (deserializeJson(response, body)) {
        lastGatewayDiagnostic_ = "JOB INVALID JSON";
        return false;
    }
    job.status = response["status"] | "";
    job.text = response["text"] | "";
    job.error = response["error"] | "";
    JsonArray approvals = response["approvals"].as<JsonArray>();
    if (!approvals.isNull() && approvals.size() > 0) {
        JsonObject approval = approvals[0];
        job.approvalId = approval["id"] | "";
        job.approvalKind = approval["kind"] | "";
        job.approvalReason = approval["reason"] | "";
        job.approvalCommand = approval["command"] | "";
    } else {
        job.approvalId = "";
        job.approvalKind = "";
        job.approvalReason = "";
        job.approvalCommand = "";
    }
    return true;
}

bool UploadService::answerCodexApproval(const String& jobId,
                                        const String& approvalId,
                                        const String& decision)
{
    if (!ensureOnline(3000)) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    const String path = "/v1/codex/jobs/" + jobId + "/approvals/" + approvalId;
    if (!beginHttp(http, plain, secure, apiUrl(path), caPem)) {
        return false;
    }
    http.setTimeout(15000);
    addAuthHeaders(http);
    http.addHeader("Content-Type", "application/json");
    JsonDocument request;
    request["decision"] = decision;
    String payload;
    serializeJson(request, payload);
    const int code = http.POST(payload);
    rememberHttpResult("APPROVAL", code);
    http.end();
    return isSuccessStatus(code);
}

bool UploadService::cancelCodexJob(const String& jobId)
{
    if (jobId.length() == 0 || !ensureOnline(3000)) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    if (!beginHttp(http, plain, secure,
                   apiUrl("/v1/codex/jobs/" + jobId + "/cancel"), caPem)) {
        return false;
    }
    http.setTimeout(30000);
    addAuthHeaders(http);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST("{}");
    const String body = http.getString();
    rememberHttpResult("CANCEL", code, body);
    http.end();
    return isSuccessStatus(code);
}

bool UploadService::downloadCodexSpeech(const String& threadId,
                                        bool conversation,
                                        const String& targetPath)
{
    if (threadId.length() == 0 || !ensureOnline()) {
        return false;
    }
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    String caPem;
    const String scope = conversation ? "conversation" : "last";
    const String path = "/v1/codex/chats/" + threadId +
                        "/speech?scope=" + scope;
    if (!beginHttp(http, plain, secure, apiUrl(path), caPem)) {
        return false;
    }
    http.setConnectTimeout(10000);
    http.setTimeout(60000);
    addAuthHeaders(http);
    const int code = http.GET();
    if (!isSuccessStatus(code)) {
        const String body = http.getString();
        rememberHttpResult("SPEECH", code, body);
        http.end();
        return false;
    }
    if (storage_->exists(targetPath.c_str())) {
        storage_->remove(targetPath.c_str());
    }
    File target = storage_->open(targetPath.c_str(), FILE_WRITE);
    const int written = target ? http.writeToStream(&target) : -1;
    if (target) {
        target.flush();
        target.close();
    }
    rememberHttpResult("SPEECH", code);
    http.end();
    if (written <= 44) {
        storage_->remove(targetPath.c_str());
        return false;
    }
    return true;
}

}  // namespace cardputer_recorder
