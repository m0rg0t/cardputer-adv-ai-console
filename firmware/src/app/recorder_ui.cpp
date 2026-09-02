#include "recorder/recorder_app.h"

#include "recorder/app/app_shared.h"

namespace cardputer_recorder {
namespace {

std::size_t utf8LengthAt(const String& value, std::size_t index)
{
    const std::uint8_t first = static_cast<std::uint8_t>(value[index]);
    if ((first & 0x80U) == 0) {
        return 1;
    }
    if ((first & 0xE0U) == 0xC0U) {
        const std::size_t remaining = value.length() - index;
        return remaining < 2 ? remaining : 2;
    }
    if ((first & 0xF0U) == 0xE0U) {
        const std::size_t remaining = value.length() - index;
        return remaining < 3 ? remaining : 3;
    }
    if ((first & 0xF8U) == 0xF0U) {
        const std::size_t remaining = value.length() - index;
        return remaining < 4 ? remaining : 4;
    }
    return 1;
}

String utf8Prefix(const String& value, std::size_t maximumCharacters)
{
    String result;
    std::size_t characters = 0;
    for (std::size_t index = 0;
         index < value.length() && characters < maximumCharacters;
         ++characters) {
        const std::size_t length = utf8LengthAt(value, index);
        result += value.substring(index, index + length);
        index += length;
    }
    return result;
}

// Thin position indicator in the 5 px right margin shared by every list.
void drawScrollbar(M5Canvas& display, int top, int height, int total,
                   int first, int visible, std::uint16_t color)
{
    if (total <= visible || height <= 0) {
        return;
    }
    const int x = display.width() - 3;
    display.drawFastVLine(x, top, height, 0x2124);
    const int thumb = max(6, height * visible / total);
    const int travel = height - thumb;
    const int maxFirst = max(1, total - visible);
    const int y = top + travel * min(first, maxFirst) / maxFirst;
    display.fillRect(x - 1, y, 3, thumb, color);
}

// Wrap UTF-8 text into lines of at most columnsPerLine characters, breaking
// at spaces where possible.
std::vector<String> wrapLines(const String& value, int columnsPerLine)
{
    std::vector<String> lines;
    String line;
    int columns = 0;
    int lastSpaceByte = -1;   // byte offset of the last space in `line`
    int lastSpaceColumn = 0;  // column count up to (excluding) that space
    for (std::size_t index = 0; index < value.length();) {
        const char character = value[index];
        if (character == '\r') {
            ++index;
            continue;
        }
        if (character == '\n') {
            lines.push_back(line);
            line = "";
            columns = 0;
            lastSpaceByte = -1;
            ++index;
            continue;
        }
        if (columns >= columnsPerLine) {
            // Break at the last space when the line holds more than one
            // word, so words are not split mid-way at either text scale.
            if (character != ' ' && lastSpaceByte > 0) {
                lines.push_back(line.substring(0, lastSpaceByte));
                line = line.substring(lastSpaceByte + 1);
                columns -= lastSpaceColumn + 1;
            } else {
                lines.push_back(line);
                line = "";
                columns = 0;
            }
            lastSpaceByte = -1;
            if (character == ' ') {
                ++index;
                continue;
            }
        }
        if (character == ' ') {
            if (columns == 0) {
                ++index;
                continue;
            }
            lastSpaceByte = static_cast<int>(line.length());
            lastSpaceColumn = columns;
        }
        const std::size_t length = utf8LengthAt(value, index);
        line += value.substring(index, index + length);
        index += length;
        ++columns;
    }
    if (line.length() > 0 || lines.empty()) {
        lines.push_back(line);
    }
    return lines;
}

void drawTextViewport(M5Canvas& display, const String& value, int scroll,
                      std::uint16_t foreground,
                      std::uint16_t background, std::uint8_t scale = 1)
{
    scale = constrain(scale, 1, 2);
    const int columnsPerLine = 37 / scale;
    const int visibleRows = 8 / scale;
    const std::vector<String> lines = wrapLines(value, columnsPerLine);
    const int maximum = max(0, static_cast<int>(lines.size()) - visibleRows);
    const int first = min(max(0, scroll), maximum);
    display.setFont(&fonts::efontCN_10);
    display.setTextSize(scale);
    display.setTextColor(foreground, background);
    for (int row = 0; row < visibleRows &&
                      first + row < static_cast<int>(lines.size()); ++row) {
        display.setCursor(7, 28 + row * 10 * scale);
        display.print(lines[first + row]);
    }
    display.setTextSize(1);
    drawScrollbar(display, 27, 82, static_cast<int>(lines.size()), first,
                  visibleRows, 0x05FF);
}

// Clip text to a pixel budget using the current font, appending ".." so a
// long human-readable title never runs into the counter or off screen.
String fitText(M5Canvas& display, const String& value, int maxWidth)
{
    if (maxWidth <= 0 || display.textWidth(value) <= maxWidth) {
        return value;
    }
    std::size_t characters = 0;
    for (std::size_t index = 0; index < value.length();
         index += utf8LengthAt(value, index)) {
        ++characters;
    }
    while (characters > 1) {
        --characters;
        const String clipped = utf8Prefix(value, characters) + "..";
        if (display.textWidth(clipped) <= maxWidth) {
            return clipped;
        }
    }
    return "..";
}

}  // namespace

void RecorderApp::draw()
{
    const unsigned long now = millis();
    // Library toast: surface message_ changes that other screens would
    // otherwise swallow, and redraw once more when the toast expires.
    if (message_ != toastMessage_) {
        toastMessage_ = message_;
        toastShownMs_ = message_.length() > 0 ? max(1UL, now) : 0;
        forceRedraw_ = true;
    } else if (toastShownMs_ != 0 && now - toastShownMs_ >= kToastMs) {
        toastShownMs_ = 0;
        forceRedraw_ = true;
    }
    const bool active =
        state_ == State::kRecording || state_ == State::kSaving ||
        state_ == State::kPlaying ||
        uploader_.transferActive() ||
        (state_ == State::kSettings &&
         (settingsPage_ == SettingsPage::kNetwork ||
          settingsPage_ == SettingsPage::kServices));
    if (!forceRedraw_ &&
        (!active || now - lastDrawMs_ < kActiveDrawIntervalMs)) {
        return;
    }
    forceRedraw_ = false;
    lastDrawMs_ = now;

    if (screenSaverState_ == ScreenSaverState::kOff) {
        return;
    }
    if (screenSaverState_ == ScreenSaverState::kDim) {
        drawScreenSaver(now);
        return;
    }

    auto& display = recorderCanvas;
    constexpr std::uint16_t background = 0x0841;
    constexpr std::uint16_t panel = 0x10C3;
    constexpr std::uint16_t selectedPanel = 0x1948;
    constexpr std::uint16_t muted = 0x8410;
    constexpr std::uint16_t accent = 0x05FF;

    display.fillSprite(background);
    display.fillRect(0, 0, display.width(), 24, panel);
    display.setTextFont(2);
    display.setTextColor(TFT_WHITE, panel);
    display.setCursor(8, 5);
    display.print("AGENT");
    display.setTextFont(1);
    display.setTextColor(
        battery_.valid && battery_.levelPercent <= 15
            ? TFT_RED
            : TFT_LIGHTGREY,
        panel);
    display.setCursor(54, 8);
    if (battery_.valid) {
        display.printf("%d%%", battery_.levelPercent);
    } else {
        display.print("--%");
    }
    display.setCursor(86, 8);
    display.setTextColor(uploader_.wifiConnected() ? TFT_GREEN : TFT_RED,
                         panel);
    display.print("W");
    const std::uint16_t httpStatus = uploader_.lastHttpStatus();
    display.setCursor(98, 8);
    display.setTextColor(httpStatus >= 200 && httpStatus < 300
                             ? TFT_GREEN
                             : TFT_ORANGE,
                         panel);
    display.print("G");
    display.setCursor(110, 8);
    display.setTextColor(servicePendingCount_ > 0 ? TFT_YELLOW : muted,
                         panel);
    display.print("Q" + String(servicePendingCount_));

    display.setTextDatum(top_right);
    display.setTextFont(1);
    display.setTextColor(accent, panel);
    String pageLabel = "LIB " + librarySortText();
    switch (state_) {
        case State::kRecording:
            pageLabel = "RECORDING";
            break;
        case State::kSaving:
            pageLabel = "SAVING";
            break;
        case State::kPlaying:
            pageLabel = "PLAYING";
            break;
        case State::kSettings:
            if (settingsPage_ == SettingsPage::kScreenSaver) {
                pageLabel = "SCREEN";
            } else if (settingsPage_ == SettingsPage::kNetwork) {
                pageLabel = "NETWORK";
            } else if (settingsPage_ == SettingsPage::kServices) {
                pageLabel = "SERVICES";
            } else if (settingsPage_ == SettingsPage::kWifiScan) {
                pageLabel = "WI-FI SCAN";
            } else if (settingsPage_ == SettingsPage::kWifiPassword) {
                pageLabel = "WI-FI PASS";
            } else {
                pageLabel = "SETTINGS";
            }
            break;
        case State::kRename:
            pageLabel = "RENAME";
            break;
        case State::kHelp:
            pageLabel = "HELP";
            break;
        case State::kRecordingDetail:
            pageLabel = "DETAILS";
            break;
        case State::kOutbox:
            pageLabel = "OUTBOX";
            break;
        case State::kCodexChats:
            pageLabel = "CHATS";
            break;
        case State::kCodexConversation:
            pageLabel = "CODEX";
            break;
        case State::kCodexCompose:
            pageLabel = "COMPOSE";
            break;
        case State::kError:
            pageLabel = "ERROR";
            break;
        default:
            break;
    }
    const bool uploadActive = uploader_.transferActive();
    const std::uint8_t uploadPercent =
        uploadActive ? uploader_.transferProgressPercent() : 0;
    if (uploadActive) {
        pageLabel = "UPLOAD " + String(uploadPercent) + "%";
    }
    display.drawString(pageLabel, display.width() - 8, 8);
    display.setTextDatum(top_left);
    if (uploadActive) {
        display.fillRect(0, 21, display.width(), 3, selectedPanel);
        display.fillRect(0, 21,
                         display.width() * uploadPercent / 100, 3,
                         accent);
    }

    if (state_ == State::kRecording) {
        const std::uint8_t recordingLevel =
            recordingLevel_.load(std::memory_order_relaxed);
        const std::uint32_t recordingBytes =
            recordingBytes_.load(std::memory_order_relaxed);
        display.fillCircle(16, 39, 5, TFT_RED);
        display.setTextFont(4);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, background);
        display.drawString(
            recordingPaused_.load(std::memory_order_relaxed)
                ? "PAUSED"
                : formatTime(recordingElapsedMs()),
            display.width() / 2, 52);
        display.setTextDatum(top_left);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.setCursor(8, 72);
        display.print(currentPath_);
        display.setTextDatum(top_right);
        display.drawString(
            String(static_cast<unsigned long>(
                       recordingBytes / 1024)) +
                " KB",
            display.width() - 8, 72);
        display.setTextDatum(top_left);
        display.drawRoundRect(8, 89, display.width() - 16, 12, 4,
                              muted);
        const int levelWidth =
            (display.width() - 20) * recordingLevel / 100;
        display.fillRoundRect(10, 91, levelWidth, 8, 3,
                              recordingLevel > 80 ? TFT_RED : accent);
        display.setTextColor(TFT_WHITE, background);
        display.setCursor(8, 112);
        display.print("P PAUSE  ENTER");
        display.setTextDatum(top_right);
        display.setTextColor(muted, background);
        display.drawString("STOP & SAVE", display.width() - 8, 112);
        display.setTextDatum(top_left);
    } else if (state_ == State::kSaving) {
        const std::uint32_t percent =
            saveTotalBytes_ == 0
                ? 0
                : saveCopiedBytes_ * 100 / saveTotalBytes_;
        display.setTextFont(4);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, background);
        display.drawString(String(percent) + "%",
                           display.width() / 2, 53);
        display.setTextDatum(top_left);
        display.drawRoundRect(12, 78, display.width() - 24, 14, 5,
                              muted);
        const int progress =
            (display.width() - 28) * percent / 100;
        display.fillRoundRect(14, 80, progress, 10, 4, accent);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.setCursor(12, 101);
        display.printf("%lu / %lu KB",
                       static_cast<unsigned long>(
                           saveCopiedBytes_ / 1024),
                       static_cast<unsigned long>(
                           saveTotalBytes_ / 1024));
        display.setTextDatum(top_right);
        display.drawString("KEEP DEVICE ON",
                           display.width() - 12, 101);
        display.setTextDatum(top_left);
    } else if (state_ == State::kPlaying) {
        const unsigned long elapsed = playbackElapsedMs();
        const std::uint32_t percent =
            playbackDurationMs_ == 0
                ? 0
                : elapsed * 100 / playbackDurationMs_;
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.setCursor(8, 31);
        display.print(currentPath_);
        display.setTextFont(4);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, background);
        display.drawString(playbackPaused_ ? "PAUSED"
                                           : formatTime(elapsed),
                           display.width() / 2, 59);
        display.setTextDatum(top_left);
        display.drawRoundRect(8, 81, display.width() - 16, 10, 4,
                              muted);
        display.fillRoundRect(
            10, 83, (display.width() - 20) * percent / 100, 6, 3,
            TFT_GREEN);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.setCursor(8, 98);
        display.printf("%s left",
                       formatTime(playbackDurationMs_ - elapsed).c_str());
        display.setTextDatum(top_right);
        display.drawString(
            "VOL " +
                String(static_cast<unsigned int>(
                    playbackVolume_ * 100U / 255U)) +
                "% " + playbackSpeedText(playbackSpeedIndex_),
            display.width() - 8, 98);
        display.setTextDatum(top_left);
        display.setTextColor(TFT_WHITE, background);
        display.setCursor(8, 116);
        display.print("LEFT/RIGHT seek");
        display.setTextDatum(top_right);
        display.drawString("ENTER pause", display.width() - 8, 116);
        display.setTextDatum(top_left);
    } else if (state_ == State::kSettings) {
        const bool screenSaverPage =
            settingsPage_ == SettingsPage::kScreenSaver;
        const bool networkPage = settingsPage_ == SettingsPage::kNetwork;
        const bool servicesPage = settingsPage_ == SettingsPage::kServices;
        const bool readingPage = settingsPage_ == SettingsPage::kReading;
        const bool wifiScanPage =
            settingsPage_ == SettingsPage::kWifiScan;
        const bool wifiPasswordPage =
            settingsPage_ == SettingsPage::kWifiPassword;
        if (networkPage) {
            const String netStatus = !uploader_.configured()
                                         ? "NOT CONFIGURED"
                                         : uploader_.wifiConnected()
                                               ? "CONNECTED"
                                               : uploader_.shortStatus();
            display.setTextFont(2);
            display.setTextColor(uploader_.wifiConnected() ? TFT_GREEN
                                                            : TFT_ORANGE,
                                 background);
            display.setCursor(8, 31);
            display.print(netStatus);
            display.setTextDatum(top_right);
            display.drawString("PROFILE " + uploader_.wifiProfileText(),
                               display.width() - 8, 34);
            display.setTextDatum(top_left);
            display.setTextFont(1);
            display.setTextColor(muted, background);
            display.setCursor(8, 51);
            display.print("SSID: " + utf8Prefix(uploader_.wifiSsid(), 29));
            display.setCursor(8, 65);
            display.print("IP: " + uploader_.localIp());
            display.setCursor(8, 79);
            display.print("LAST: " +
                          utf8Prefix(uploader_.wifiDisconnectReason(), 30));
            display.setCursor(8, 93);
            display.print("RADIO: " +
                          utf8Prefix(uploader_.wifiObservation(), 29));
        } else if (servicesPage) {
            const auto& services = uploader_.gatewayServices();
            const bool ready = services.overall == "OK";
            display.setTextFont(2);
            display.setTextColor(ready ? TFT_GREEN : TFT_ORANGE,
                                 background);
            display.setCursor(8, 30);
            display.print(services.overall);
            display.setTextDatum(top_right);
            display.setTextFont(1);
            display.drawString("QUEUE " + String(servicePendingCount_),
                               display.width() - 8, 35);
            display.setTextDatum(top_left);
            display.setTextColor(muted, background);
            display.setCursor(8, 51);
            display.print("GATEWAY: " + utf8Prefix(services.gateway, 27));
            display.setCursor(8, 65);
            display.print("WHISPER: " + utf8Prefix(services.whisper, 27));
            display.setCursor(8, 79);
            display.print("CODEX: " + utf8Prefix(services.codex, 29));
            display.setCursor(8, 93);
            display.print("EDITOR: " + utf8Prefix(services.formatter, 28));
        } else if (wifiScanPage) {
            if (wifiScanResults_.empty()) {
                display.setTextFont(2);
                display.setTextColor(TFT_WHITE, background);
                display.setCursor(10, 40);
                display.print("No visible networks");
                display.setTextFont(1);
                display.setTextColor(muted, background);
                display.setCursor(10, 65);
                display.print(message_);
            } else {
                int first = max(0, selectedWifiNetwork_ - 1);
                if (first + 4 > static_cast<int>(wifiScanResults_.size())) {
                    first = max(
                        0, static_cast<int>(wifiScanResults_.size()) - 4);
                }
                for (int row = 0; row < 4 &&
                                      first + row <
                                          static_cast<int>(
                                              wifiScanResults_.size());
                     ++row) {
                    const int index = first + row;
                    const int y = 29 + row * 20;
                    const bool selected = index == selectedWifiNetwork_;
                    if (selected) {
                        display.fillRoundRect(5, y - 2,
                                              display.width() - 10, 19, 4,
                                              selectedPanel);
                    }
                    display.setFont(&fonts::efontCN_10);
                    display.setTextColor(selected ? TFT_WHITE : muted,
                                         selected ? selectedPanel
                                                  : background);
                    display.setCursor(9, y + 2);
                    display.print(utf8Prefix(wifiScanResults_[index].ssid,
                                             21));
                    display.setTextDatum(middle_right);
                    display.setTextFont(1);
                    display.drawString(
                        String(wifiScanResults_[index].rssi) + " C" +
                            String(wifiScanResults_[index].channel) + " " +
                            wifiScanResults_[index].auth,
                        display.width() - 7, y + 7);
                    display.setTextDatum(top_left);
                }
                drawScrollbar(display, 27, 80,
                              static_cast<int>(wifiScanResults_.size()),
                              first, 4, accent);
            }
        } else if (wifiPasswordPage) {
            display.setTextFont(1);
            display.setTextColor(muted, background);
            display.setCursor(8, 34);
            display.print("SSID: " + utf8Prefix(wifiTargetSsid_, 29));
            display.fillRoundRect(8, 52, display.width() - 16, 26, 4,
                                  panel);
            String masked;
            const std::size_t visible = wifiPasswordText_.length() > 32
                                            ? 32
                                            : wifiPasswordText_.length();
            for (std::size_t index = 0; index < visible; ++index) {
                masked += '*';
            }
            display.setTextFont(2);
            display.setTextColor(TFT_WHITE, panel);
            display.setCursor(13, 58);
            display.print(masked + "_");
            display.setTextFont(1);
            display.setTextColor(muted, background);
            display.setCursor(8, 86);
            display.print(utf8Prefix(message_, 36));
        } else {
            const char* mainLabels[kSettingsCount] = {
                "Brightness", "Screen Saver", "Network", "Services",
                "Reading", "Library Sort", "Compact Audio",
                "Low Battery Save", "Seek Step", "Help",
                "Reset to Default", "Version"};
            const char* screenSaverLabels[kScreenSaverSettingsCount] = {
                "When Home", "While Recording", "While Playing",
                "Triple-Press Wake", "Visual Style"};
            const char* readingLabels[kReadingSettingsCount] = {
                "Codex Chat", "Transcripts", "Chat Names"};
            const std::uint8_t settingCount =
                screenSaverPage ? kScreenSaverSettingsCount
                                : readingPage ? kReadingSettingsCount
                                              : kSettingsCount;
            int first = static_cast<int>(selectedSetting_) - 1;
            if (first < 0) {
                first = 0;
            }
            if (first + 4 > settingCount) {
                first = max(0, static_cast<int>(settingCount) - 4);
            }
            for (int row = 0; row < 4 && first + row < settingCount; ++row) {
                const int index = first + row;
                const int y = 31 + row * 19;
                const bool selected =
                    index == static_cast<int>(selectedSetting_);
                if (selected) {
                    display.fillRoundRect(5, y - 2, display.width() - 10,
                                          18, 4, selectedPanel);
                }
                display.setTextFont(1);
                display.setTextColor(selected ? TFT_WHITE : muted,
                                     selected ? selectedPanel : background);
                display.setCursor(10, y + 3);
                display.print(screenSaverPage
                                  ? screenSaverLabels[index]
                                  : readingPage ? readingLabels[index]
                                                : mainLabels[index]);
                display.setTextDatum(middle_right);
                display.drawString(settingValueText(index),
                                   display.width() - 10, y + 8);
                display.setTextDatum(top_left);
            }
            drawScrollbar(display, 29, 76, settingCount, first, 4, accent);
        }
        display.fillRect(0, 111, display.width(), 24, panel);
        display.setTextFont(1);
        display.setTextColor(accent, panel);
        display.setCursor(8, 120);
        if (networkPage) {
            display.print("S SCAN");
        } else if (wifiScanPage) {
            display.print("S RESCAN");
        } else if (wifiPasswordPage) {
            display.print(String(wifiPasswordText_.length()) + "/63");
        } else if (servicesPage) {
            display.print("END-TO-END CHECK");
        } else if (!screenSaverPage && !readingPage && selectedSetting_ == 10) {
            display.print(resetSettingsConfirm_ ? "ENTER reset"
                                                : "ENTER confirm");
        } else if (!screenSaverPage && !readingPage && selectedSetting_ == 11) {
            display.print("VERSION");
        } else if (!screenSaverPage && !readingPage && selectedSetting_ == 9) {
            display.print("ENTER open help");
        } else {
            display.print("LEFT/RIGHT value");
        }
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        const char* settingsFooter = "ESC SAVE";
        if (networkPage) {
            settingsFooter = "ENTER RETRY";
        } else if (servicesPage) {
            settingsFooter = "ENTER REFRESH";
        } else if (wifiScanPage) {
            settingsFooter = "ENTER SELECT";
        } else if (wifiPasswordPage) {
            settingsFooter = "ENTER CONNECT";
        } else if (screenSaverPage || readingPage) {
            settingsFooter = "ESC BACK";
        }
        display.drawString(settingsFooter,
                           display.width() - 8, 120);
        display.setTextDatum(top_left);
    } else if (state_ == State::kRename) {
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.setCursor(8, 34);
        display.print(renameOriginalName_);
        display.fillRoundRect(8, 54, display.width() - 16, 24, 4,
                              panel);
        display.setTextFont(2);
        display.setTextColor(TFT_WHITE, panel);
        display.setCursor(14, 59);
        display.print(renameText_);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.setCursor(14, 84);
        display.print(".WAV");
        display.setCursor(8, 98);
        display.print(message_);
        display.fillRect(0, 111, display.width(), 24, panel);
        display.setTextColor(accent, panel);
        display.setCursor(8, 120);
        display.print("TYPE NAME");
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        display.drawString("ENTER SAVE", display.width() - 8, 120);
        display.setTextDatum(top_left);
    } else if (state_ == State::kHelp) {
        const char* titles[] = {
            "Library", "Files", "Recording", "Playback", "Outbox",
            "Codex Tasks", "Codex Chat", "Recording Info", "Settings",
            "System"};
        const char* rows[][4] = {
            {"R  record note", "S  cycle sorting", "C Codex / O Outbox",
             "I  details"},
            {"ENTER  play", "LEFT  lock", "RIGHT  rename", "DEL  delete"},
            {"ENTER/R/ESC  save", "P/SPACE  pause", "G0  screen saver",
             "uploads after save"},
            {"ENTER  pause", "ESC  stop", "LEFT/RIGHT seek",
             "UP/DOWN vol  [ ] speed"},
            {"ENTER  details", "R  retry one", "A  retry all", "X  cancel"},
            {"ENTER  open", "N type V voice", "R voice  T type",
             "B both  L library"},
            {"UP/DOWN  scroll", "E  jump to end", "V  speak reply",
             "A  speak chat"},
            {"UP/DOWN  scroll", "P  profile", "E  reprocess",
             "R retry  C Codex"},
            {"ARROWS  select/edit", "S  Wi-Fi scan", "Reading  text size",
             "Screen  visual style"},
            {"Hold G0  settings", "H  context help",
             "W wifi  G gateway  Q queue", "ESC  back"},
        };

        display.setTextFont(2);
        display.setTextColor(TFT_WHITE, background);
        display.setCursor(8, 32);
        display.print(titles[helpPage_]);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.setTextDatum(top_right);
        display.drawString(
            String(helpPage_ + 1) + "/10", display.width() - 8, 35);
        display.setTextDatum(top_left);
        display.setTextFont(2);
        display.setTextColor(TFT_WHITE, background);
        for (std::uint8_t row = 0; row < 4; ++row) {
            if (rows[helpPage_][row][0] == '\0') {
                continue;
            }
            display.setCursor(12, 50 + row * 16);
            display.print(rows[helpPage_][row]);
        }
        display.fillRect(0, 117, display.width(), 18, panel);
        display.setTextFont(1);
        display.setTextColor(accent, panel);
        display.setCursor(8, 124);
        display.print("LEFT/RIGHT page");
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        display.drawString("ENTER/ESC BACK", display.width() - 8, 124);
        display.setTextDatum(top_left);
    } else if (state_ == State::kOutbox) {
        if (outboxFiles_.empty()) {
            display.setTextFont(2);
            display.setTextColor(TFT_GREEN, background);
            display.setCursor(12, 45);
            display.print("Outbox is empty");
            display.setTextFont(1);
            display.setTextColor(muted, background);
            display.setCursor(12, 68);
            display.print("All recordings delivered");
        } else {
            int first = max(0, selectedOutbox_ - 1);
            if (first + 4 > static_cast<int>(outboxFiles_.size())) {
                first = max(0, static_cast<int>(outboxFiles_.size()) - 4);
            }
            for (int row = 0; row < 4 &&
                              first + row < static_cast<int>(outboxFiles_.size());
                 ++row) {
                const int index = first + row;
                const int y = 29 + row * 20;
                const bool selected = index == selectedOutbox_;
                if (selected) {
                    display.fillRoundRect(5, y - 2, display.width() - 10,
                                          19, 4, selectedPanel);
                }
                const String& filename = outboxFiles_[index];
                const String itemStatus = uploader_.recordingStatus(
                    filename, storage_.fileSize(("/" + filename).c_str()));
                display.setTextFont(2);
                display.setTextColor(itemStatus == "ERROR" ? TFT_RED : accent,
                                     selected ? selectedPanel : background);
                display.setCursor(9, y + 1);
                display.print(utf8Prefix(itemStatus, 6));
                display.setTextColor(selected ? TFT_WHITE : muted,
                                     selected ? selectedPanel : background);
                display.setCursor(52, y + 1);
                display.print(fitText(display, filename,
                                      display.width() - 52 - 7));
            }
            drawScrollbar(display, 27, 80,
                          static_cast<int>(outboxFiles_.size()), first, 4,
                          accent);
        }
        display.fillRect(0, 111, display.width(), 24, panel);
        display.setTextFont(1);
        display.setTextColor(accent, panel);
        display.setCursor(7, 120);
        display.print("ENTER INFO  R ONE");
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        display.drawString("A ALL  X CANCEL", display.width() - 7, 120);
        display.setTextDatum(top_left);
    } else if (state_ == State::kCodexChats) {
        if (codexChats_.empty()) {
            display.setTextFont(2);
            display.setTextColor(TFT_WHITE, background);
            display.setCursor(12, 42);
            display.print("No chats available");
            display.setTextFont(1);
            display.setTextColor(muted, background);
            display.setCursor(12, 64);
            display.print(message_);
        } else if (settings_.codexChatNamesMultiline) {
            display.fillRoundRect(5, 27, display.width() - 10, 79, 4,
                                  selectedPanel);
            display.setFont(&fonts::efontCN_10);
            display.setTextColor(TFT_WHITE, selectedPanel);
            const std::vector<String> lines =
                wrapLines(codexChats_[selectedCodexChat_].name, 36);
            for (std::size_t row = 0; row < lines.size() && row < 6; ++row) {
                display.setCursor(10, 31 + static_cast<int>(row) * 11);
                display.print(lines[row]);
            }
            display.setTextFont(1);
            display.setTextDatum(bottom_right);
            display.drawString(String(selectedCodexChat_ + 1) + "/" +
                                   String(codexChats_.size()),
                               display.width() - 9, 103);
            display.setTextDatum(top_left);
        } else {
            int first = max(0, selectedCodexChat_ - 1);
            if (first + 4 > static_cast<int>(codexChats_.size())) {
                first = max(0, static_cast<int>(codexChats_.size()) - 4);
            }
            for (int row = 0; row < 4 &&
                              first + row < static_cast<int>(codexChats_.size());
                 ++row) {
                const int index = first + row;
                const int y = 28 + row * 20;
                const bool selected = index == selectedCodexChat_;
                if (selected) {
                    display.fillRoundRect(5, y - 2, display.width() - 10,
                                          19, 4, selectedPanel);
                }
                const String counter =
                    String(index + 1) + "/" + String(codexChats_.size());
                display.setTextFont(1);
                const int counterWidth = display.textWidth(counter);
                display.setFont(&fonts::efontCN_10);
                display.setTextColor(selected ? TFT_WHITE : muted,
                                     selected ? selectedPanel : background);
                display.setCursor(10, y + 3);
                display.print(fitText(display, codexChats_[index].name,
                                      display.width() - 9 - counterWidth -
                                          6 - 10));
                display.setTextDatum(middle_right);
                display.setTextFont(1);
                display.drawString(counter, display.width() - 9, y + 7);
                display.setTextDatum(top_left);
            }
            drawScrollbar(display, 26, 80,
                          static_cast<int>(codexChats_.size()), first, 4,
                          accent);
        }
        display.fillRect(0, 111, display.width(), 24, panel);
        display.setTextFont(1);
        display.setTextColor(accent, panel);
        display.setCursor(7, 120);
        display.print("N TYPE V VOICE");
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        display.drawString("ENTER OPEN  ESC", display.width() - 7, 120);
        display.setTextDatum(top_left);
    } else if (state_ == State::kCodexConversation) {
        if (codexJob_.approvalId.length() > 0) {
            display.fillRoundRect(6, 28, display.width() - 12, 76, 5,
                                  panel);
            display.setTextFont(2);
            display.setTextColor(TFT_ORANGE, panel);
            display.setCursor(12, 34);
            display.print("APPROVAL: " + codexJob_.approvalKind);
            String approval = codexJob_.approvalCommand.length() > 0
                                  ? codexJob_.approvalCommand
                                  : codexJob_.approvalReason;
            if (approval.length() > 90) {
                approval = approval.substring(0, 90) + "...";
            }
            display.setTextFont(1);
            display.setTextColor(TFT_WHITE, panel);
            display.setCursor(12, 55);
            display.setClipRect(12, 55, display.width() - 24, 36);
            display.setTextWrap(true);
            display.print(approval);
            display.setTextWrap(false);
            display.clearClipRect();
            display.setCursor(12, 94);
            display.setTextColor(accent, panel);
            display.print("ENTER once  S session  D deny");
        } else {
            String view = codexConversation_;
            if (codexJobId_.length() > 0 && codexJob_.text.length() > 0) {
                view += "\nCODEX: " + codexJob_.text;
            }
            drawTextViewport(display, view, codexScroll_, TFT_WHITE,
                             background, settings_.codexTextScale);
        }
        display.fillRect(0, 111, display.width(), 24, panel);
        display.setTextFont(1);
        display.setTextColor(accent, panel);
        display.setCursor(7, 120);
        display.print(codexSpeechState_.load(std::memory_order_acquire) == 1
                          ? "TTS PREPARING..."
                          : codexJobId_.length() > 0 ? "X STOP  E END"
                                                     : "E END  V SPEAK");
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        display.drawString("A CHAT  T TYPE", display.width() - 7, 120);
        display.setTextDatum(top_left);
    } else if (state_ == State::kCodexCompose) {
        display.fillRoundRect(6, 29, display.width() - 12, 75, 5, panel);
        String visible = codexComposeText_;
        if (visible.length() > 280) {
            visible = visible.substring(visible.length() - 280);
        }
        drawTextViewport(display, visible + "_", 0, TFT_WHITE, panel);
        display.fillRect(0, 111, display.width(), 24, panel);
        display.setTextFont(1);
        display.setTextColor(accent, panel);
        display.setCursor(7, 120);
        display.print(String(codexComposeText_.length()) + "/1000");
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        display.drawString("ENTER SEND  ESC CANCEL", display.width() - 7, 120);
        display.setTextDatum(top_left);
    } else if (state_ == State::kRecordingDetail) {
        drawTextViewport(display, recordingDetailText_, recordingDetailScroll_,
                         TFT_WHITE, background,
                         settings_.transcriptTextScale);
        display.fillRect(0, 111, display.width(), 24, panel);
        display.setTextFont(1);
        display.setTextColor(accent, panel);
        display.setCursor(8, 120);
        display.print("P PROFILE  E REPROCESS");
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        display.drawString("R RETRY  ESC", display.width() - 8, 120);
        display.setTextDatum(top_left);
    } else if (state_ == State::kError) {
        display.fillRoundRect(8, 34, display.width() - 16, 70, 6,
                              panel);
        display.setTextFont(2);
        display.setTextColor(TFT_RED, panel);
        display.setCursor(18, 44);
        display.print("Something went wrong");
        display.setTextFont(1);
        display.setTextColor(TFT_WHITE, panel);
        display.setCursor(18, 66);
        display.setClipRect(18, 66, display.width() - 36, 34);
        display.setTextWrap(true);
        display.print(message_);
        display.setTextWrap(false);
        display.clearClipRect();
        display.setTextColor(accent, background);
        display.setCursor(8, 116);
        display.print("ENTER retry storage");
    } else {
        const String detail = selectedRecordingDetail();
        if (files_.empty()) {
            display.drawRoundRect(14, 33, display.width() - 28, 53, 8,
                                  muted);
            display.fillCircle(display.width() / 2, 51, 10, TFT_RED);
            display.setTextFont(2);
            display.setTextDatum(middle_center);
            display.setTextColor(TFT_WHITE, background);
            display.drawString("No recordings",
                               display.width() / 2, 75);
            display.setTextDatum(top_left);
        } else {
            int first = selected_ - 1;
            if (first < 0) {
                first = 0;
            }
            if (first + 3 > static_cast<int>(files_.size())) {
                first = max(0, static_cast<int>(files_.size()) - 3);
            }
            for (int row = 0; row < 3 &&
                              first + row <
                                  static_cast<int>(files_.size());
                 ++row) {
                const int index = first + row;
                const int y = 29 + row * 19;
                if (index == selected_) {
                    display.fillRoundRect(
                        5, y - 2, display.width() - 10, 18, 4,
                        selectedPanel);
                }
                display.setTextFont(2);
                display.setTextColor(
                    index == selected_ ? TFT_WHITE : muted,
                    index == selected_ ? selectedPanel : background);
                display.setCursor(10, y);
                const String recordingStatus = uploader_.recordingStatus(
                    files_[index],
                    storage_.fileSize(("/" + files_[index]).c_str()));
                display.setTextColor(
                    recordingStatus == "TEXT"
                        ? TFT_GREEN
                        : recordingStatus == "SENT"
                              ? TFT_YELLOW
                              : recordingStatus == "ERROR" ? TFT_RED : muted,
                    index == selected_ ? selectedPanel : background);
                display.print(recordingStatus == "TEXT"
                                  ? "T "
                                  : recordingStatus == "SENT"
                                        ? "S "
                                        : recordingStatus == "WORK"
                                              ? "> "
                                              : recordingStatus == "ERROR"
                                                    ? "! "
                                                    : "Q ");
                display.setTextColor(
                    index == selected_ ? TFT_WHITE : muted,
                    index == selected_ ? selectedPanel : background);
                if (isLocked(files_[index])) {
                    display.print("* ");
                }
                const int nameX = display.getCursorX();
                display.setTextDatum(middle_right);
                display.setTextFont(1);
                const String counter =
                    String(index + 1) + "/" + String(files_.size());
                display.drawString(counter, display.width() - 10, y + 7);
                const int counterWidth = display.textWidth(counter);
                display.setTextDatum(top_left);
                display.setTextFont(2);
                display.setCursor(nameX, y);
                display.print(fitText(
                    display, files_[index],
                    display.width() - 10 - counterWidth - 6 - nameX));
            }
        }
        drawScrollbar(display, 27, 60, static_cast<int>(files_.size()),
                      max(0, selected_ - 1), 3, accent);
        display.setTextFont(1);
        display.setCursor(8, 94);
        if (toastShownMs_ != 0 && state_ == State::kBrowsing) {
            display.setTextColor(accent, background);
            display.print(fitText(display, message_, display.width() - 16));
        } else {
            display.setTextColor(muted, background);
            display.print(fitText(display, detail, display.width() - 16));
        }
        display.fillRect(0, 111, display.width(), 24, panel);
        display.setTextFont(1);
        display.setTextColor(TFT_RED, panel);
        display.setCursor(8, 120);
        display.print(deleteConfirm_ ? "ENTER DELETE" : "R REC");
        display.setTextDatum(top_center);
        display.setTextColor(accent, panel);
        display.drawString(deleteConfirm_        ? "ESC CANCEL"
                           : servicePendingCount_ > 0 ? "I INFO  O OUTBOX"
                                                      : "I INFO  S SORT",
                           display.width() / 2, 120);
        display.setTextDatum(top_right);
        display.setTextColor(muted, panel);
        display.drawString("C CODEX", display.width() - 8, 120);
        display.setTextDatum(top_left);
    }
    recorderCanvas.pushSprite(0, 0);
}

}  // namespace cardputer_recorder
