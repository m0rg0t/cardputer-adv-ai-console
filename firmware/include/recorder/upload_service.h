#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <vector>

#include "recorder/hardware/storage_service.h"

namespace cardputer_recorder {

// Background, at-least-once delivery for completed WAV recordings. The
// gateway is responsible for content-hash deduplication, so a reset between a
// successful POST and ledger update cannot create duplicate Obsidian notes.
class UploadService {
public:
    enum class Destination : std::uint8_t {
        kNote,
        kCodex,
        kBoth,
    };

    struct CodexChat {
        String id;
        String name;
        String preview;
        String cwd;
    };

    struct CodexJobStatus {
        String status;
        String text;
        String error;
        String approvalId;
        String approvalKind;
        String approvalReason;
        String approvalCommand;
    };

    struct RecordingMetadata {
        String status;
        String stage;
        String destination;
        String transcript;
        String formatted;
        String notePath;
        String sha256;
        String voiceJobId;
        String codexJobId;
        String error;
        String profile = "default";
        std::uint8_t progress = 0;
        std::uint8_t attempts = 0;
        bool transcriptComplete = true;
    };

    struct WifiScanResult {
        String ssid;
        std::int32_t rssi = 0;
        std::uint8_t channel = 0;
        String auth;
        bool open = false;
    };

    struct GatewayServices {
        String overall = "NOT CHECKED";
        String gateway = "NOT CHECKED";
        String whisper = "NOT CHECKED";
        String codex = "NOT CHECKED";
        String formatter = "NOT CHECKED";
    };

    void begin(StorageService& storage);
    void update(bool ioAllowed);
    void requestSoon();
    String shortStatus() const;
    bool configured() const;
    bool wifiConnected() const;
    bool transferActive() const;
    std::uint8_t transferProgressPercent() const;
    std::uint32_t transferBytesSent() const;
    std::uint32_t transferBytesTotal() const;
    String wifiSsid() const;
    String wifiProfileText() const;
    String wifiDisconnectReason() const;
    String wifiObservation() const;
    String wifiScanDiagnostic() const;
    bool scanWifiNetworks(std::vector<WifiScanResult>& networks);
    bool connectScannedNetwork(const String& ssid,
                               const String& password);
    String gatewayBaseUrl() const;
    String gatewayDiagnostic() const;
    String currentVoiceProfile() const;
    String cycleVoiceProfile(int offset);
    bool refreshGatewayStatus();
    const GatewayServices& gatewayServices() const;
    std::size_t pendingRecordingCount();
    String localIp() const;
    std::uint16_t lastHttpStatus() const;
    bool recordingMetadata(const String& filename,
                           RecordingMetadata& metadata);
    String recordingStatus(const String& filename, std::uint32_t size);
    String recordingDeliveryDetail(const String& filename,
                                   std::uint32_t size);
    bool takeRecordingChanged();
    String takeLastJobId();
    String takeLastJobThreadId();
    bool ensureOnline(unsigned long timeoutMs = 15000);
    bool setRoute(const String& wavPath, Destination destination,
                  const String& threadId);
    bool refreshChats(std::vector<CodexChat>& chats);
    bool createCodexChat(CodexChat& chat);
    bool fetchConversation(const String& threadId, String& text);
    bool sendCodexMessage(const String& threadId, const String& text,
                          String& jobId);
    bool pollCodexJob(const String& jobId, CodexJobStatus& job);
    bool answerCodexApproval(const String& jobId,
                             const String& approvalId,
                             const String& decision);
    bool cancelCodexJob(const String& jobId);
    bool downloadCodexSpeech(const String& threadId, bool conversation,
                             const String& targetPath);
    bool retryVoiceJob(const String& filename);
    bool retryFailedVoiceJobs(std::size_t& retriedCount);
    bool cancelVoiceJob(const String& filename);
    bool reprocessVoiceJob(const String& filename, const String& profile);

private:
    struct WifiNetwork {
        String ssid;
        String password;
        bool manual = false;

        WifiNetwork() = default;
        WifiNetwork(const String& networkSsid,
                    const String& networkPassword, bool isManual)
            : ssid(networkSsid),
              password(networkPassword),
              manual(isManual)
        {
        }
    };

    struct Config {
        std::vector<WifiNetwork> wifiNetworks;
        String gatewayBaseUrl;
        String gatewayFallbackUrl;
        String gatewayDiscoveryScheme = "https";
        String deviceToken;
        String deviceName = "cardputer-adv";
        std::vector<String> voiceProfiles;

        bool valid() const
        {
            return !wifiNetworks.empty() && gatewayBaseUrl.length() > 0 &&
                   deviceToken.length() >= 16;
        }
    };

    enum class Status : std::uint8_t {
        kDisabled,
        kOffline,
        kConnecting,
        kReady,
        kUploading,
        kError,
    };

    bool loadConfig();
    bool persistManualNetworks();
    bool restartWifiStation();
    bool connectWifi();
    bool discoverGateway();
    bool findPending(String& path, String& name, std::uint32_t& size);
    bool findAwaitingJob(String& name, std::uint32_t& size,
                         RecordingMetadata& metadata);
    bool pollVoiceJob(const String& filename,
                      const String& voiceJobId,
                      std::uint32_t size);
    bool controlVoiceJob(const String& filename, const char* action);
    bool wasSent(const String& name, std::uint32_t size);
    bool markSent(const String& name, std::uint32_t size);
    bool renameCompletedRecording(const String& filename,
                                  const String& suggestedFilename,
                                  String& finalFilename);
    bool writeRecordingMetadata(const String& filename,
                                const JsonDocument& response);
    String recordingMetadataPath(const String& filename) const;
    bool upload(const String& path, const String& name,
                std::uint32_t size);
    bool startBackgroundUpload(const String& path, const String& name,
                               std::uint32_t size);
    static void uploadTaskEntry(void* context);
    void backgroundUpload();
    bool readRoute(const String& wavPath, Destination& destination,
                   String& threadId, String& profile);
    String routePath(const String& wavPath) const;
    String apiUrl(const String& path) const;
    bool loadCaCertificate(String& pem);
    bool ensureTlsClock();
    bool beginHttp(HTTPClient& http, WiFiClient& plain,
                   WiFiClientSecure& secure, const String& url,
                   String& caPem);
    void rememberHttpResult(const char* operation, int code,
                            const String& responseBody = "");
    void setGatewayDiagnostic(const String& diagnostic);
    void lockState() const;
    void unlockState() const;
    void addAuthHeaders(HTTPClient& http);
    static const char* destinationText(Destination destination);
    static const char* authModeText(wifi_auth_mode_t authMode);
    static String trimValue(String value);

    StorageService* storage_ = nullptr;
    Config config_;
    std::atomic<Status> status_{Status::kDisabled};
    std::atomic<unsigned long> nextActionMs_{0};
    std::atomic<unsigned long> nextFailedPollMs_{0};
    std::atomic<unsigned long> connectStartedMs_{0};
    std::size_t activeNetworkIndex_ = 0;
    std::size_t activeVoiceProfileIndex_ = 0;
    std::atomic<std::uint16_t> lastDisconnectReason_{0};
    String lastWifiScanDiagnostic_ = "NOT RUN";
    std::vector<WifiScanResult> lastScanResults_;
    String pendingManualSsid_;
    std::atomic<std::uint16_t> lastHttpStatus_{0};
    std::atomic<int> lastHttpCode_{0};
    String lastGatewayDiagnostic_ = "NOT CHECKED";
    GatewayServices gatewayServices_;
    String activeUploadName_;
    String backgroundUploadPath_;
    String backgroundUploadName_;
    std::atomic<std::uint32_t> backgroundUploadSize_{0};
    std::atomic<bool> backgroundUploadActive_{false};
    std::atomic<std::uint32_t> backgroundUploadBytesSent_{0};
    std::atomic<std::uint32_t> backgroundUploadBytesTotal_{0};
    String lastFailedUploadName_;
    String lastSubmittedJobId_;
    String lastSubmittedThreadId_;
    std::atomic<bool> recordingChanged_{false};
    // HTTP uploads run on a FreeRTOS task while the UI reads status and
    // diagnostics from the main loop.  Arduino String is not thread-safe, so
    // protect the shared text fields (scalar progress fields remain atomic).
    mutable SemaphoreHandle_t stateMutex_ = nullptr;
};

}  // namespace cardputer_recorder
