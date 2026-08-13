#include "recorder/recorder_app.h"

#include "recorder/app/app_shared.h"

namespace cardputer_recorder {
namespace {

constexpr const char* kRecorderConfigPath = "/RECORDER.CFG";
constexpr const char* kRecorderConfigTempPath = "/RECORDER.CFG.TMP";

bool parseBool(const String& value, bool fallback)
{
    String normalized = value;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized == "true" || normalized == "on" || normalized == "1") {
        return true;
    }
    if (normalized == "false" || normalized == "off" || normalized == "0") {
        return false;
    }
    return fallback;
}

}  // namespace

void RecorderApp::openSettings()
{
    selectedSetting_ = 0;
    settingsPage_ = SettingsPage::kMain;
    resetSettingsConfirm_ = false;
    state_ = State::kSettings;
    message_ = "Settings";
    forceRedraw_ = true;
}
void RecorderApp::closeSettings()
{
    saveSettings();
    state_ = State::kBrowsing;
    message_ = "Settings saved. Hold G0 to reopen.";
    forceRedraw_ = true;
}
void RecorderApp::handleSettingsInput(const InputEvent& event)
{
    if (event.help && settingsPage_ != SettingsPage::kWifiPassword) {
        openHelp();
        return;
    }
    if (event.settings || event.back) {
        if (resetSettingsConfirm_) {
            resetSettingsConfirm_ = false;
            message_ = "Reset canceled.";
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kScreenSaver) {
            settingsPage_ = SettingsPage::kMain;
            selectedSetting_ = 1;
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kNetwork) {
            settingsPage_ = SettingsPage::kMain;
            selectedSetting_ = 2;
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kServices) {
            settingsPage_ = SettingsPage::kMain;
            selectedSetting_ = 3;
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kReading) {
            settingsPage_ = SettingsPage::kMain;
            selectedSetting_ = 4;
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kWifiScan) {
            settingsPage_ = SettingsPage::kNetwork;
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kWifiPassword) {
            settingsPage_ = SettingsPage::kWifiScan;
            wifiPasswordText_ = "";
            forceRedraw_ = true;
            return;
        }
        closeSettings();
        return;
    }
    if (settingsPage_ == SettingsPage::kWifiScan) {
        handleWifiScanInput(event);
        return;
    }
    if (settingsPage_ == SettingsPage::kWifiPassword) {
        handleWifiPasswordInput(event);
        return;
    }
    if (settingsPage_ == SettingsPage::kNetwork) {
        if (event.primaryKey == 's' || event.primaryKey == 'S') {
            openWifiScan();
        } else if (event.confirm || event.record) {
            uploader_.requestSoon();
            message_ = "Connection retry requested.";
            forceRedraw_ = true;
        }
        return;
    }
    if (settingsPage_ == SettingsPage::kServices) {
        if (event.confirm || event.record || event.primaryKey == 'r' ||
            event.primaryKey == 'R') {
            openServiceStatus();
        }
        return;
    }
    const std::uint8_t settingCount =
        settingsPage_ == SettingsPage::kScreenSaver
            ? kScreenSaverSettingsCount
            : settingsPage_ == SettingsPage::kReading
                  ? kReadingSettingsCount
                  : kSettingsCount;
    if (event.up) {
        resetSettingsConfirm_ = false;
        selectedSetting_ =
            (selectedSetting_ + settingCount - 1) % settingCount;
        forceRedraw_ = true;
    } else if (event.down) {
        resetSettingsConfirm_ = false;
        selectedSetting_ = (selectedSetting_ + 1) % settingCount;
        forceRedraw_ = true;
    } else if (event.right || event.confirm) {
        if (settingsPage_ == SettingsPage::kMain &&
            selectedSetting_ == 1) {
            settingsPage_ = SettingsPage::kScreenSaver;
            selectedSetting_ = 0;
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kMain &&
            selectedSetting_ == 2) {
            settingsPage_ = SettingsPage::kNetwork;
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kMain &&
            selectedSetting_ == 3) {
            openServiceStatus();
            return;
        }
        if (settingsPage_ == SettingsPage::kMain &&
            selectedSetting_ == 4) {
            settingsPage_ = SettingsPage::kReading;
            selectedSetting_ = 0;
            forceRedraw_ = true;
            return;
        }
        if (settingsPage_ == SettingsPage::kMain &&
            selectedSetting_ == 9) {
            openHelp();
            return;
        }
        cycleSelectedSetting(1);
    } else if (event.left) {
        if (resetSettingsConfirm_) {
            resetSettingsConfirm_ = false;
            message_ = "Reset canceled.";
            forceRedraw_ = true;
            return;
        }
        cycleSelectedSetting(-1);
    }
}

void RecorderApp::openServiceStatus()
{
    settingsPage_ = SettingsPage::kServices;
    message_ = "Checking Gateway services...";
    forceRedraw_ = true;
    draw();
    uploader_.refreshGatewayStatus();
    servicePendingCount_ = uploader_.pendingRecordingCount();
    message_ = uploader_.gatewayDiagnostic();
    forceRedraw_ = true;
}

void RecorderApp::openWifiScan()
{
    settingsPage_ = SettingsPage::kWifiScan;
    message_ = "Scanning Wi-Fi...";
    wifiScanResults_.clear();
    selectedWifiNetwork_ = 0;
    forceRedraw_ = true;
    draw();
    if (uploader_.scanWifiNetworks(wifiScanResults_)) {
        message_ = wifiScanResults_.empty()
                       ? "No visible networks."
                       : String(wifiScanResults_.size()) + " networks found";
    } else {
        message_ = "Scan failed: " + uploader_.wifiScanDiagnostic();
    }
    forceRedraw_ = true;
}

void RecorderApp::handleWifiScanInput(const InputEvent& event)
{
    if ((event.primaryKey == 's' || event.primaryKey == 'S') &&
        !event.confirm) {
        openWifiScan();
        return;
    }
    if (event.up && !wifiScanResults_.empty()) {
        selectedWifiNetwork_ =
            (selectedWifiNetwork_ +
             static_cast<int>(wifiScanResults_.size()) - 1) %
            static_cast<int>(wifiScanResults_.size());
        forceRedraw_ = true;
    } else if (event.down && !wifiScanResults_.empty()) {
        selectedWifiNetwork_ =
            (selectedWifiNetwork_ + 1) %
            static_cast<int>(wifiScanResults_.size());
        forceRedraw_ = true;
    } else if (event.confirm && !wifiScanResults_.empty()) {
        openWifiPassword();
    }
}

void RecorderApp::openWifiPassword()
{
    if (wifiScanResults_.empty() || selectedWifiNetwork_ < 0 ||
        selectedWifiNetwork_ >=
            static_cast<int>(wifiScanResults_.size())) {
        return;
    }
    wifiTargetSsid_ = wifiScanResults_[selectedWifiNetwork_].ssid;
    wifiPasswordText_ = "";
    if (wifiScanResults_[selectedWifiNetwork_].open) {
        if (uploader_.connectScannedNetwork(wifiTargetSsid_, "")) {
            settingsPage_ = SettingsPage::kNetwork;
            message_ = "Connecting to open network...";
        } else {
            message_ = "Could not add network.";
        }
        forceRedraw_ = true;
        return;
    }
    settingsPage_ = SettingsPage::kWifiPassword;
    message_ = "Enter Wi-Fi password";
    forceRedraw_ = true;
}

void RecorderApp::handleWifiPasswordInput(const InputEvent& event)
{
    if (event.deletePressed && wifiPasswordText_.length() > 0) {
        wifiPasswordText_.remove(wifiPasswordText_.length() - 1);
        forceRedraw_ = true;
    }
    for (std::size_t index = 0; index < event.text.length(); ++index) {
        const char character = event.text[index];
        if (character >= 32 && character <= 126 &&
            wifiPasswordText_.length() < 63) {
            wifiPasswordText_ += character;
            forceRedraw_ = true;
        }
    }
    if (event.confirm) {
        if (wifiPasswordText_.length() < 8) {
            message_ = "Password must be at least 8 characters.";
            forceRedraw_ = true;
            return;
        }
        if (!uploader_.connectScannedNetwork(wifiTargetSsid_,
                                             wifiPasswordText_)) {
            message_ = "Could not add network.";
            forceRedraw_ = true;
            return;
        }
        wifiPasswordText_ = "";
        settingsPage_ = SettingsPage::kNetwork;
        message_ = "Connecting to selected network...";
        forceRedraw_ = true;
    }
}
void RecorderApp::cycleSelectedSetting(int offset)
{
    if (settingsPage_ == SettingsPage::kScreenSaver) {
        std::uint8_t* value = nullptr;
        if (selectedSetting_ == 0) {
            value = &settings_.idleScreenMode;
        } else if (selectedSetting_ == 1) {
            value = &settings_.recordingScreenMode;
        } else if (selectedSetting_ == 2) {
            value = &settings_.playbackScreenMode;
        }
        if (value != nullptr) {
            *value = static_cast<std::uint8_t>(
                (static_cast<int>(*value) + 3 + offset) % 3);
            saveSettings();
            forceRedraw_ = true;
        } else if (selectedSetting_ == 3) {
            settings_.triplePressWake = !settings_.triplePressWake;
            saveSettings();
            forceRedraw_ = true;
        } else if (selectedSetting_ == 4) {
            settings_.screenSaverStyle = static_cast<std::uint8_t>(
                (settings_.screenSaverStyle + 2 + offset) % 2);
            saveSettings();
            forceRedraw_ = true;
        }
        return;
    }
    if (settingsPage_ == SettingsPage::kReading) {
        std::uint8_t& scale = selectedSetting_ == 0
                                  ? settings_.codexTextScale
                                  : settings_.transcriptTextScale;
        scale = scale == 1 ? 2 : 1;
        saveSettings();
        forceRedraw_ = true;
        return;
    }

    switch (selectedSetting_) {
        case 0: {
            constexpr std::uint8_t values[] = {
                10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
            constexpr int count = sizeof(values) / sizeof(values[0]);
            int index = 0;
            for (int candidate = 0; candidate < count; ++candidate) {
                if (settings_.brightnessPercent == values[candidate]) {
                    index = candidate;
                    break;
                }
            }
            index = (index + count + offset) % count;
            settings_.brightnessPercent = values[index];
            applyBrightness();
            break;
        }
        case 5:
            cycleLibrarySort(offset);
            return;
        case 6:
            settings_.compactAudio = !settings_.compactAudio;
            break;
        case 7:
        {
            constexpr std::uint8_t values[] = {0, 1, 5, 10};
            constexpr int count = sizeof(values) / sizeof(values[0]);
            int index = 3;
            for (int candidate = 0; candidate < count; ++candidate) {
                if (settings_.lowBatterySavePercent ==
                    values[candidate]) {
                    index = candidate;
                    break;
                }
            }
            index = (index + count + offset) % count;
            settings_.lowBatterySavePercent = values[index];
            break;
        }
        case 8:
        {
            constexpr std::uint8_t values[] = {5, 10, 20, 60};
            constexpr int count = sizeof(values) / sizeof(values[0]);
            int index = 0;
            for (int candidate = 0; candidate < count; ++candidate) {
                if (settings_.seekStepSeconds == values[candidate]) {
                    index = candidate;
                    break;
                }
            }
            index = (index + count + offset) % count;
            settings_.seekStepSeconds = values[index];
            break;
        }
        case 10:
            if (resetSettingsConfirm_) {
                resetSettingsToDefault();
            } else if (offset > 0) {
                resetSettingsConfirm_ = true;
                message_ = "Press Enter again to reset.";
            }
            break;
        case 11:
            break;
        default:
            break;
    }
    saveSettings();
    forceRedraw_ = true;
}
void RecorderApp::resetSettingsToDefault()
{
    settings_ = Settings{};
    resetSettingsConfirm_ = false;
    saveSettings();
    applyBrightness();
    resetScreenSaverTimer();
    message_ = "Settings reset.";
}
void RecorderApp::loadSettings()
{
    if (storage_.isMounted()) {
        File file = storage_.open(kRecorderConfigPath, FILE_READ);
        while (file && file.available()) {
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
            String value = line.substring(separator + 1);
            key.trim();
            key.toLowerCase();
            value.trim();
            if (key == "brightness_percent") {
                settings_.brightnessPercent = value.toInt();
            } else if (key == "screen_home") {
                settings_.idleScreenMode = value.toInt();
            } else if (key == "screen_recording") {
                settings_.recordingScreenMode = value.toInt();
            } else if (key == "screen_playback") {
                settings_.playbackScreenMode = value.toInt();
            } else if (key == "screen_style") {
                settings_.screenSaverStyle = value.toInt();
            } else if (key == "low_battery_save_percent") {
                settings_.lowBatterySavePercent = value.toInt();
            } else if (key == "seek_step_seconds") {
                settings_.seekStepSeconds = value.toInt();
            } else if (key == "compact_audio") {
                settings_.compactAudio =
                    parseBool(value, settings_.compactAudio);
            } else if (key == "vad_enabled") {
                settings_.vadEnabled = parseBool(value, settings_.vadEnabled);
            } else if (key == "triple_press_wake") {
                settings_.triplePressWake =
                    parseBool(value, settings_.triplePressWake);
            } else if (key == "codex_text_scale") {
                settings_.codexTextScale = value.toInt();
            } else if (key == "transcript_text_scale") {
                settings_.transcriptTextScale = value.toInt();
            } else if (key == "library_sort") {
                settings_.librarySortMode =
                    static_cast<LibrarySortMode>(value.toInt());
            }
        }
        if (file) {
            file.close();
        }
    }

    if (settings_.brightnessPercent < 10 ||
        settings_.brightnessPercent > 100) {
        settings_.brightnessPercent = 70;
    }
    if (settings_.idleScreenMode > 2) {
        settings_.idleScreenMode = 1;
    }
    if (settings_.recordingScreenMode > 2) {
        settings_.recordingScreenMode = 1;
    }
    if (settings_.playbackScreenMode > 2) {
        settings_.playbackScreenMode = 1;
    }
    if (settings_.screenSaverStyle > 1) {
        settings_.screenSaverStyle = 0;
    }
    if (settings_.lowBatterySavePercent != 0 &&
        settings_.lowBatterySavePercent != 1 &&
        settings_.lowBatterySavePercent != 5 &&
        settings_.lowBatterySavePercent != 10) {
        settings_.lowBatterySavePercent = 10;
    }
    if (settings_.seekStepSeconds != 5 &&
        settings_.seekStepSeconds != 10 &&
        settings_.seekStepSeconds != 20 &&
        settings_.seekStepSeconds != 60) {
        settings_.seekStepSeconds = 5;
    }
    if (settings_.codexTextScale < 1 || settings_.codexTextScale > 2) {
        settings_.codexTextScale = 1;
    }
    if (settings_.transcriptTextScale < 1 ||
        settings_.transcriptTextScale > 2) {
        settings_.transcriptTextScale = 1;
    }
    if (settings_.librarySortMode >= LibrarySortMode::kCount) {
        settings_.librarySortMode = LibrarySortMode::kNewest;
    }

    // First boot creates a documented, portable configuration on the card.
    if (storage_.isMounted() && !storage_.exists(kRecorderConfigPath)) {
        saveSettings();
    }
}
void RecorderApp::saveSettings()
{
    if (!storage_.isMounted()) {
        return;
    }
    if (storage_.exists(kRecorderConfigTempPath)) {
        storage_.remove(kRecorderConfigTempPath);
    }
    File file = storage_.open(kRecorderConfigTempPath, FILE_WRITE);
    if (!file) {
        Serial.println("[RECORDER] Could not write RECORDER.CFG.TMP");
        return;
    }
    file.println("# Cardputer ADV Recorder settings (0=off, 1=dim, 2=black)");
    file.printf("brightness_percent=%u\n", settings_.brightnessPercent);
    file.printf("screen_home=%u\n", settings_.idleScreenMode);
    file.printf("screen_recording=%u\n", settings_.recordingScreenMode);
    file.printf("screen_playback=%u\n", settings_.playbackScreenMode);
    file.printf("screen_style=%u\n", settings_.screenSaverStyle);
    file.printf("low_battery_save_percent=%u\n",
                settings_.lowBatterySavePercent);
    file.printf("seek_step_seconds=%u\n", settings_.seekStepSeconds);
    file.printf("compact_audio=%s\n",
                settings_.compactAudio ? "true" : "false");
    file.printf("vad_enabled=%s\n", settings_.vadEnabled ? "true" : "false");
    file.printf("triple_press_wake=%s\n",
                settings_.triplePressWake ? "true" : "false");
    file.printf("codex_text_scale=%u\n", settings_.codexTextScale);
    file.printf("transcript_text_scale=%u\n",
                settings_.transcriptTextScale);
    file.printf("library_sort=%u\n",
                static_cast<std::uint8_t>(settings_.librarySortMode));
    file.flush();
    file.close();
    if (storage_.exists(kRecorderConfigPath) &&
        !storage_.remove(kRecorderConfigPath)) {
        Serial.println("[RECORDER] Could not replace RECORDER.CFG");
        return;
    }
    if (!storage_.rename(kRecorderConfigTempPath, kRecorderConfigPath)) {
        Serial.println("[RECORDER] Could not commit RECORDER.CFG");
    }
}
void RecorderApp::applyBrightness()
{
    const std::uint8_t brightness = static_cast<std::uint8_t>(
        max(1, static_cast<int>(settings_.brightnessPercent) * 255 / 100));
    M5Cardputer.Display.setBrightness(brightness);
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
        const std::uint8_t scale = index == 0 ? settings_.codexTextScale
                                              : settings_.transcriptTextScale;
        return String(scale) + "x";
    }

    switch (index) {
        case 0:
            return String(settings_.brightnessPercent) + "%";
        case 1:
            return ">";
        case 2:
            return ">";
        case 3:
            return ">";
        case 4:
            return ">";
        case 5:
            return librarySortText();
        case 6:
            return settings_.compactAudio ? "8 kHz" : "16 kHz";
        case 7:
            return lowBatterySaveText(settings_.lowBatterySavePercent);
        case 8:
            return seekStepText(settings_.seekStepSeconds);
        case 9:
            return ">";
        case 10:
            return resetSettingsConfirm_ ? "CONFIRM?" : "";
        case 11:
            return kAppVersion;
        default:
            return "";
    }
}

}  // namespace cardputer_recorder
