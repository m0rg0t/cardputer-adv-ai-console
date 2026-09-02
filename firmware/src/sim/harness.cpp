// Desktop preview of the Cardputer UI.
//
// Compiles the real drawing code (recorder_ui.cpp, screen_saver.cpp) against
// the M5GFX SDL backend and scripts RecorderApp's internal state for each
// screen. Hardware-facing members are satisfied by the stubs below, so this
// unit is the only place that needs to know the preview is not a device.
//
//   pio run -e native-sim -t exec                  interactive window
//   .pio/build/native-sim/program --shots out/     write every screen as PPM

#include "recorder/recorder_app.h"

#include <M5GFX.h>
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "recorder/app/app_shared.h"

// ---------------------------------------------------------------------------
// Globals the stubs and compiled UI units expect.

M5CardputerStub M5Cardputer;
SerialStub Serial;

namespace cardputer_sim {

unsigned long frozenMs = 0;
bool frozen = false;

unsigned long simMillis()
{
    return frozen ? frozenMs : static_cast<unsigned long>(SDL_GetTicks());
}

void simDelay(unsigned long ms)
{
    if (!frozen) {
        SDL_Delay(static_cast<Uint32>(ms));
    }
}

// Fake device/network state read by the UploadService and StorageService
// stubs. Scenarios adjust these alongside RecorderApp's own fields.
struct FakeDevice {
    bool wifiConnected = true;
    bool configured = true;
    std::uint16_t httpStatus = 200;
    bool transferActive = false;
    std::uint8_t transferPercent = 0;
    std::string shortStatus = "READY";
    std::string ssid = "HomeNet";
    std::string ip = "192.168.1.42";
    std::string profile = "1/2";
    std::string disconnectReason = "NONE";
    std::string observation = "RSSI -58 dBm CH6";
    std::vector<std::pair<std::string, std::string>> statuses;
    std::vector<std::pair<std::string, std::uint32_t>> sizes;
    cardputer_recorder::UploadService::GatewayServices services;
};
FakeDevice device;

std::string lookup(const std::vector<std::pair<std::string, std::string>>& table,
                   const std::string& key, const char* fallback)
{
    for (const auto& entry : table) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    return fallback;
}

}  // namespace cardputer_sim

// ---------------------------------------------------------------------------
// Stubs for hardware-backed members referenced by the compiled UI units.

namespace cardputer_recorder {

using cardputer_sim::device;

bool UploadService::transferActive() const { return device.transferActive; }
std::uint8_t UploadService::transferProgressPercent() const
{
    return device.transferPercent;
}
bool UploadService::wifiConnected() const { return device.wifiConnected; }
bool UploadService::configured() const { return device.configured; }
std::uint16_t UploadService::lastHttpStatus() const { return device.httpStatus; }
String UploadService::shortStatus() const { return device.shortStatus.c_str(); }
String UploadService::wifiSsid() const { return device.ssid.c_str(); }
String UploadService::localIp() const { return device.ip.c_str(); }
String UploadService::wifiProfileText() const { return device.profile.c_str(); }
String UploadService::wifiDisconnectReason() const
{
    return device.disconnectReason.c_str();
}
String UploadService::wifiObservation() const
{
    return device.observation.c_str();
}
const UploadService::GatewayServices& UploadService::gatewayServices() const
{
    return device.services;
}
String UploadService::recordingStatus(const String& filename, std::uint32_t)
{
    return cardputer_sim::lookup(device.statuses, filename.c_str(), "QUEUE")
        .c_str();
}

std::uint32_t StorageService::fileSize(const char* path)
{
    std::string name = path;
    if (!name.empty() && name[0] == '/') {
        name.erase(0, 1);
    }
    for (const auto& entry : device.sizes) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return 0;
}

// RecorderApp members whose real definitions live in hardware-heavy units.
// They mirror the device logic so the preview stays faithful.

String RecorderApp::librarySortText() const
{
    switch (settings_.librarySortMode) {
        case LibrarySortMode::kOldest:
            return "OLD";
        case LibrarySortMode::kStatus:
            return "STATUS";
        case LibrarySortMode::kName:
            return "A-Z";
        default:
            return "NEW";
    }
}

bool RecorderApp::isLocked(const String& filename) const
{
    for (const auto& locked : lockedFiles_) {
        if (locked == filename) {
            return true;
        }
    }
    return false;
}

unsigned long RecorderApp::recordingElapsedMs() const
{
    unsigned long paused = recordingPausedTotalMs_;
    if (recordingPaused_.load() && recordingPausedAtMs_ > 0) {
        paused += millis() - recordingPausedAtMs_;
    }
    const unsigned long total = millis() - operationStartedMs_;
    return total > paused ? total - paused : 0;
}

unsigned long RecorderApp::playbackElapsedMs() const
{
    return playbackBaseElapsedMs_ +
           (playbackPaused_ ? 0 : millis() - operationStartedMs_);
}

void RecorderApp::applyBrightness()
{
    M5Cardputer.Display.setBrightness(static_cast<std::uint8_t>(
        max(1, static_cast<int>(settings_.brightnessPercent) * 255 / 100)));
}

String RecorderApp::selectedRecordingDetail()
{
    if (files_.empty()) {
        return "";
    }
    const String name = files_[selected_];
    const std::uint32_t size = storage_.fileSize(("/" + name).c_str());
    const unsigned long durationMs =
        static_cast<unsigned long>(static_cast<std::uint64_t>(size) * 1000 /
                                   32000);
    return formatByteCount(size) + "  " + formatTime(durationMs) + "  " +
           uploader_.recordingStatus(name, size) + "  " +
           uploader_.shortStatus();
}

String RecorderApp::settingValueText(std::uint8_t index) const
{
    if (settingsPage_ == SettingsPage::kScreenSaver) {
        switch (index) {
            case 0:
                return screenModeText(settings_.idleScreenMode);
            case 1:
                return screenModeText(settings_.recordingScreenMode);
            case 2:
                return screenModeText(settings_.playbackScreenMode);
            case 3:
                return settings_.triplePressWake ? "ON" : "OFF";
            case 4:
                return settings_.screenSaverStyle == 0 ? "CYBER GRID"
                                                       : "DATA RAIN";
            default:
                return "";
        }
    }
    if (settingsPage_ == SettingsPage::kReading) {
        if (index == 2) {
            return settings_.codexChatNamesMultiline ? "FULL" : "1 LINE";
        }
        const std::uint8_t scale = index == 0 ? settings_.codexTextScale
                                              : settings_.transcriptTextScale;
        return String(scale) + "x";
    }
    switch (index) {
        case 0:
            return String(settings_.brightnessPercent) + "%";
        case 5:
            return librarySortText();
        case 6:
            return settings_.compactAudio ? "8 kHz" : "16 kHz";
        case 7:
            return lowBatterySaveText(settings_.lowBatterySavePercent);
        case 8:
            return seekStepText(settings_.seekStepSeconds);
        case 10:
            return resetSettingsConfirm_ ? "CONFIRM?" : "";
        case 11:
            return kAppVersion;
        case 1:
        case 2:
        case 3:
        case 4:
        case 9:
            return ">";
        default:
            return "";
    }
}

}  // namespace cardputer_recorder

// ---------------------------------------------------------------------------
// Scenarios. SimAccess is a friend of RecorderApp, so it can script every
// private field the screens read.

namespace cardputer_recorder {

struct SimAccess {
    using App = RecorderApp;
    using State = App::State;
    using Page = App::SettingsPage;

    struct Scenario {
        const char* name;
        void (*apply)(App&);
    };

    static void reset(App& app)
    {
        cardputer_sim::device = cardputer_sim::FakeDevice();
        app.state_ = State::kBrowsing;
        app.settings_ = App::Settings();
        app.battery_.valid = true;
        app.battery_.levelPercent = 61;
        app.battery_.voltageMv = 3900;
        app.message_ = "";
        app.toastMessage_ = "";
        app.toastShownMs_ = 0;
        app.deleteConfirm_ = false;
        app.servicePendingCount_ = 0;
        app.screenSaverState_ = App::ScreenSaverState::kAwake;
        app.screenSaverManual_ = false;
        app.wakeConfirmCount_ = 0;
        app.settingsPage_ = Page::kMain;
        app.selectedSetting_ = 0;
        app.helpPage_ = 0;
        app.selected_ = 0;
        app.codexScroll_ = 0;
        app.codexJobId_ = "";
        app.codexJob_ = UploadService::CodexJobStatus();
        app.codexSpeechState_.store(0);
        app.recordingDetailScroll_ = 0;
        app.files_ = {"2026-08-30 21-14 WEEKLY PLANNING NOTES.WAV",
                      "REC0004.WAV", "REC0003.WAV", "REC0002.WAV",
                      "REC0001.WAV"};
        app.lockedFiles_ = {"REC0002.WAV"};
        cardputer_sim::device.statuses = {
            {"2026-08-30 21-14 WEEKLY PLANNING NOTES.WAV", "TEXT"},
            {"REC0004.WAV", "45%"},
            {"REC0003.WAV", "ERROR"},
            {"REC0002.WAV", "SENT"},
            {"REC0001.WAV", "QUEUE"}};
        cardputer_sim::device.sizes = {
            {"2026-08-30 21-14 WEEKLY PLANNING NOTES.WAV", 6087680},
            {"REC0004.WAV", 1630208},
            {"REC0003.WAV", 512000},
            {"REC0002.WAV", 2880000},
            {"REC0001.WAV", 96000}};
        app.forceRedraw_ = true;
    }

    static void library(App& app)
    {
        reset(app);
        app.servicePendingCount_ = 2;
    }
    static void libraryToast(App& app)
    {
        reset(app);
        app.selected_ = 1;
        app.message_ = "Saved REC0004.WAV";
    }
    static void libraryDelete(App& app)
    {
        reset(app);
        app.selected_ = 2;
        app.deleteConfirm_ = true;
    }
    static void libraryEmpty(App& app)
    {
        reset(app);
        app.files_.clear();
        cardputer_sim::device.wifiConnected = false;
        cardputer_sim::device.httpStatus = 0;
    }
    static void libraryUploading(App& app)
    {
        reset(app);
        cardputer_sim::device.transferActive = true;
        cardputer_sim::device.transferPercent = 63;
        app.servicePendingCount_ = 1;
    }
    static void recording(App& app)
    {
        reset(app);
        app.state_ = State::kRecording;
        app.currentPath_ = "/REC0005.WAV";
        app.operationStartedMs_ = millis() - 51000;
        app.recordingPausedTotalMs_ = 0;
        app.recordingPaused_.store(false);
        app.recordingLevel_.store(58);
        app.recordingBytes_.store(1630208);
    }
    static void recordingPaused(App& app)
    {
        recording(app);
        app.recordingPaused_.store(true);
        app.recordingPausedAtMs_ = millis() - 2000;
        app.recordingLevel_.store(0);
    }
    static void saving(App& app)
    {
        reset(app);
        app.state_ = State::kSaving;
        app.saveTotalBytes_ = 1630208;
        app.saveCopiedBytes_ = 1100000;
    }
    static void playing(App& app)
    {
        reset(app);
        app.state_ = State::kPlaying;
        app.currentPath_ = "/REC0004.WAV";
        app.playbackDurationMs_ = 190000;
        app.playbackBaseElapsedMs_ = 0;
        app.operationStartedMs_ = millis() - 72000;
        app.playbackPaused_ = false;
        app.playbackVolume_ = 200;
        app.playbackSpeedIndex_ = 1;
    }
    static void settingsMain(App& app)
    {
        reset(app);
        app.state_ = State::kSettings;
        app.selectedSetting_ = 5;
    }
    static void settingsScreen(App& app)
    {
        reset(app);
        app.state_ = State::kSettings;
        app.settingsPage_ = Page::kScreenSaver;
        app.selectedSetting_ = 4;
    }
    static void settingsReading(App& app)
    {
        reset(app);
        app.state_ = State::kSettings;
        app.settingsPage_ = Page::kReading;
        app.selectedSetting_ = 0;
    }
    static void network(App& app)
    {
        reset(app);
        app.state_ = State::kSettings;
        app.settingsPage_ = Page::kNetwork;
    }
    static void services(App& app)
    {
        reset(app);
        app.state_ = State::kSettings;
        app.settingsPage_ = Page::kServices;
        auto& services = cardputer_sim::device.services;
        services.overall = "OK";
        services.gateway = "OK 2.0";
        services.whisper = "OK whisper-server";
        services.codex = "OK app-server";
        services.formatter = "OK gpt-5";
        app.servicePendingCount_ = 2;
    }
    static void wifiScan(App& app)
    {
        reset(app);
        app.state_ = State::kSettings;
        app.settingsPage_ = Page::kWifiScan;
        app.wifiScanResults_.clear();
        const char* names[] = {"HomeNet", "Cafe Guest", "Office-5G",
                               "Neighbour", "Printer"};
        const int rssi[] = {-52, -67, -71, -80, -88};
        for (int index = 0; index < 5; ++index) {
            UploadService::WifiScanResult result;
            result.ssid = names[index];
            result.rssi = rssi[index];
            result.channel = static_cast<std::uint8_t>(1 + index * 3);
            result.auth = index == 1 ? "OPEN" : "WPA2";
            result.open = index == 1;
            app.wifiScanResults_.push_back(result);
        }
        app.selectedWifiNetwork_ = 1;
    }
    static void wifiPassword(App& app)
    {
        reset(app);
        app.state_ = State::kSettings;
        app.settingsPage_ = Page::kWifiPassword;
        app.wifiTargetSsid_ = "Office-5G";
        app.wifiPasswordText_ = "hunter2hunter";
        app.message_ = "Type the password, ENTER to connect";
    }
    static void rename(App& app)
    {
        reset(app);
        app.state_ = State::kRename;
        app.renameOriginalName_ = "REC0004.WAV";
        app.renameText_ = "STANDUP 30 AUG";
        app.message_ = "A-Z 0-9 space - _";
    }
    static void help(App& app)
    {
        reset(app);
        app.state_ = State::kHelp;
        app.helpPage_ = 0;
    }
    static void helpSystem(App& app)
    {
        help(app);
        app.helpPage_ = 9;
    }
    static void outbox(App& app)
    {
        reset(app);
        app.state_ = State::kOutbox;
        app.outboxFiles_ = {"REC0004.WAV", "REC0003.WAV", "REC0001.WAV"};
        app.selectedOutbox_ = 1;
        app.servicePendingCount_ = 3;
    }
    static void outboxEmpty(App& app)
    {
        reset(app);
        app.state_ = State::kOutbox;
        app.outboxFiles_.clear();
    }
    static void chats(App& app, bool multiline)
    {
        reset(app);
        app.state_ = State::kCodexChats;
        app.settings_.codexChatNamesMultiline = multiline;
        app.codexChats_.clear();
        const char* names[] = {
            "Fix web panel settings and add HTTP range playback",
            "Voice-first Codex workflows",
            "Recorder: rename and delete controls",
            "ElevenLabs TTS caching",
            "Long-audio delivery hardening"};
        for (int index = 0; index < 5; ++index) {
            UploadService::CodexChat chat;
            chat.id = "thread_" + String(index);
            chat.name = names[index];
            app.codexChats_.push_back(chat);
        }
        app.selectedCodexChat_ = 0;
    }
    static void chatsCompact(App& app) { chats(app, false); }
    static void chatsFull(App& app) { chats(app, true); }
    static void conversation(App& app)
    {
        reset(app);
        app.state_ = State::kCodexConversation;
        app.codexConversation_ =
            "YOU: Review the recorder web panel and fix what you find.\n"
            "CODEX: The settings form posted numbers as strings and the "
            "firmware ignored them. I made the parser accept both, added "
            "byte-range support so Safari can play recordings, and refreshed "
            "the panel layout.\n"
            "YOU: Also check the on-device UI.\n";
        app.codexJobId_ = "job_1";
        app.codexJob_.status = "in_progress";
        app.codexJob_.text =
            "Long titles overflowed the library rows. Clipping them to the "
            "pixel width now.";
        app.codexScroll_ = 2;
    }
    static void conversationLarge(App& app)
    {
        conversation(app);
        app.codexJobId_ = "";
        app.settings_.codexTextScale = 2;
        app.codexScroll_ = 0;
    }
    static void approval(App& app)
    {
        conversation(app);
        app.codexJob_.approvalId = "approval_1";
        app.codexJob_.approvalKind = "command";
        app.codexJob_.approvalCommand =
            "pio run -e cardputer-adv-recorder && git commit -am \"Polish "
            "library rows\"";
    }
    static void compose(App& app)
    {
        reset(app);
        app.state_ = State::kCodexCompose;
        app.codexComposeText_ =
            "Add a scroll indicator to the settings list and keep the "
            "footer hints consistent";
    }
    static void detail(App& app)
    {
        reset(app);
        app.state_ = State::kRecordingDetail;
        app.recordingDetailText_ =
            "2026-08-30 21-14 WEEKLY PLANNING NOTES.WAV\n"
            "Submission: completed\nStage: exported (100%)\n"
            "Destination: both\n---\n"
            "Weekly planning. We agreed to ship the recorder panel fixes "
            "first, then the SDL preview harness, and to keep the firmware "
            "under the M5Apps slot limit. Anton will flash the build and "
            "check the library with a renamed file.";
    }
    static void error(App& app)
    {
        reset(app);
        app.state_ = State::kError;
        app.message_ =
            "microSD card not detected. Insert a FAT32 card and press ENTER "
            "to retry.";
    }
    static void saverGrid(App& app)
    {
        reset(app);
        app.screenSaverState_ = App::ScreenSaverState::kDim;
        app.settings_.screenSaverStyle = 0;
    }
    static void saverRain(App& app)
    {
        reset(app);
        app.screenSaverState_ = App::ScreenSaverState::kDim;
        app.settings_.screenSaverStyle = 1;
        app.screenSaverManual_ = true;
    }
    static void saverRecording(App& app)
    {
        recording(app);
        app.screenSaverState_ = App::ScreenSaverState::kDim;
    }
    static void saverWake(App& app)
    {
        saverGrid(app);
        app.settings_.triplePressWake = true;
        app.wakeConfirmCount_ = 2;
    }

    static const std::vector<Scenario>& scenarios()
    {
        static const std::vector<Scenario> list = {
            {"library", library},
            {"library-toast", libraryToast},
            {"library-delete", libraryDelete},
            {"library-empty", libraryEmpty},
            {"library-uploading", libraryUploading},
            {"recording", recording},
            {"recording-paused", recordingPaused},
            {"saving", saving},
            {"playing", playing},
            {"settings", settingsMain},
            {"settings-screen", settingsScreen},
            {"settings-reading", settingsReading},
            {"network", network},
            {"services", services},
            {"wifi-scan", wifiScan},
            {"wifi-password", wifiPassword},
            {"rename", rename},
            {"help", help},
            {"help-system", helpSystem},
            {"outbox", outbox},
            {"outbox-empty", outboxEmpty},
            {"chats", chatsCompact},
            {"chats-full", chatsFull},
            {"codex", conversation},
            {"codex-2x", conversationLarge},
            {"codex-approval", approval},
            {"compose", compose},
            {"detail", detail},
            {"error", error},
            {"saver-grid", saverGrid},
            {"saver-rain", saverRain},
            {"saver-recording", saverRecording},
            {"saver-wake", saverWake},
        };
        return list;
    }

    static void render(App& app)
    {
        app.forceRedraw_ = true;
        app.lastDrawMs_ = 0;
        app.draw();
    }
};

}  // namespace cardputer_recorder

// ---------------------------------------------------------------------------
// Entry point.

namespace {

using cardputer_recorder::recorderCanvas;
using cardputer_recorder::RecorderApp;
using cardputer_recorder::SimAccess;

std::string shotsDirectory;
bool listOnly = false;

bool writePpm(const std::string& path)
{
    const int width = recorderCanvas.width();
    const int height = recorderCanvas.height();
    std::vector<lgfx::rgb888_t> pixels(static_cast<std::size_t>(width) *
                                       height);
    recorderCanvas.readRect(0, 0, width, height, pixels.data());
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (const auto& pixel : pixels) {
        const unsigned char rgb[3] = {pixel.r, pixel.g, pixel.b};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    return true;
}

int userMain(bool* running)
{
    auto& display = M5Cardputer.Display;
    display.init();
    display.setRotation(1);
    recorderCanvas.setColorDepth(8);
    recorderCanvas.createSprite(display.width(), display.height());

    static RecorderApp app;
    const auto& scenarios = SimAccess::scenarios();

    if (!shotsDirectory.empty()) {
        cardputer_sim::frozen = true;
        cardputer_sim::frozenMs = 100000;
        for (const auto& scenario : scenarios) {
            scenario.apply(app);
            SimAccess::render(app);
            const std::string path =
                shotsDirectory + "/" + scenario.name + ".ppm";
            if (!writePpm(path)) {
                fprintf(stderr, "could not write %s\n", path.c_str());
                return 1;
            }
            printf("%s\n", path.c_str());
        }
        // Panel_sdl::main keeps running until the window closes; a headless
        // render has nothing more to show.
        fflush(stdout);
        std::exit(0);
    }

    std::size_t current = 0;
    scenarios[current].apply(app);
    bool leftHeld = false;
    bool rightHeld = false;
    unsigned long lastAutoMs = SDL_GetTicks();
    while (*running) {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        const bool left = keys[SDL_SCANCODE_LEFT] != 0;
        const bool right = keys[SDL_SCANCODE_RIGHT] != 0 ||
                           keys[SDL_SCANCODE_SPACE] != 0;
        if (keys[SDL_SCANCODE_ESCAPE] || keys[SDL_SCANCODE_Q]) {
            break;
        }
        bool changed = false;
        if (right && !rightHeld) {
            current = (current + 1) % scenarios.size();
            changed = true;
        } else if (left && !leftHeld) {
            current = (current + scenarios.size() - 1) % scenarios.size();
            changed = true;
        }
        rightHeld = right;
        leftHeld = left;
        if (keys[SDL_SCANCODE_A] && SDL_GetTicks() - lastAutoMs > 2500) {
            current = (current + 1) % scenarios.size();
            changed = true;
        }
        if (changed) {
            lastAutoMs = SDL_GetTicks();
            scenarios[current].apply(app);
            std::string title = "Cardputer preview: ";
            title += scenarios[current].name;
            title += "   (LEFT/RIGHT switch, hold A to cycle, ESC quit)";
            static_cast<lgfx::Panel_sdl*>(display.panel())
                ->setWindowTitle(title.c_str());
        }
        SimAccess::render(app);
        SDL_Delay(33);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--shots") == 0 && index + 1 < argc) {
            shotsDirectory = argv[++index];
        } else if (strcmp(argv[index], "--list") == 0) {
            listOnly = true;
        }
    }
    if (listOnly) {
        for (const auto& scenario : SimAccess::scenarios()) {
            printf("%s\n", scenario.name);
        }
        return 0;
    }
    return lgfx::Panel_sdl::main(userMain, 128);
}
