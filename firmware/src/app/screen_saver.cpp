#include "recorder/recorder_app.h"

#include "recorder/app/app_shared.h"

namespace cardputer_recorder {
namespace {

constexpr unsigned long kTriplePressWakeTimeoutMs = 3000;

}  // namespace

bool RecorderApp::anyInput(const InputEvent& event) const
{
    return event.g0 || event.left || event.right || event.up ||
           event.down || event.speedDown || event.speedUp ||
           event.confirm || event.back || event.fail || event.record ||
           event.deletePressed || event.settings || event.help;
}
bool RecorderApp::screenSaverAllowed() const
{
    return state_ == State::kBrowsing || state_ == State::kRecording ||
           state_ == State::kPlaying || state_ == State::kError;
}
std::uint8_t RecorderApp::screenSaverModeForState() const
{
    if (state_ == State::kRecording) {
        return settings_.recordingScreenMode;
    }
    if (state_ == State::kPlaying) {
        return settings_.playbackScreenMode;
    }
    return settings_.idleScreenMode;
}
void RecorderApp::serviceScreenSaver()
{
    if (screenSaverState_ != ScreenSaverState::kAwake &&
        wakeConfirmCount_ > 0 &&
        millis() - wakeConfirmLastMs_ > kTriplePressWakeTimeoutMs) {
        resetWakeConfirmation();
        if (screenSaverModeForState() == 2) {
            screenSaverState_ = ScreenSaverState::kOff;
            M5Cardputer.Display.setBrightness(0);
            M5Cardputer.Display.sleep();
            forceRedraw_ = false;
        } else {
            forceRedraw_ = true;
        }
    }

    if (!screenSaverAllowed()) {
        if (screenSaverState_ != ScreenSaverState::kAwake) {
            wakeScreen();
        }
        resetScreenSaverTimer();
        return;
    }

    if (screenSaverState_ != ScreenSaverState::kAwake ||
        screenSaverModeForState() == 0) {
        return;
    }

    const unsigned long delayMs =
        (state_ == State::kRecording || state_ == State::kPlaying)
            ? kActiveScreenSaverDelayMs
            : kIdleScreenSaverDelayMs;
    if (millis() - lastUserActivityMs_ >= delayMs) {
        enterScreenSaver(false);
    }
}
void RecorderApp::resetScreenSaverTimer()
{
    lastUserActivityMs_ = millis();
    screenSaverManual_ = false;
}
void RecorderApp::enterScreenSaver(bool manual)
{
    if (!screenSaverAllowed()) {
        return;
    }
    const std::uint8_t mode = screenSaverModeForState();
    if (mode == 0) {
        return;
    }

    screenSaverManual_ = manual;
    resetWakeConfirmation();
    if (mode == 2) {
        screenSaverState_ = ScreenSaverState::kOff;
        M5Cardputer.Display.setBrightness(0);
        M5Cardputer.Display.sleep();
        forceRedraw_ = false;
    } else {
        screenSaverState_ = ScreenSaverState::kDim;
        M5Cardputer.Display.wakeup();
        M5Cardputer.Display.setBrightness(kDimBrightness);
        forceRedraw_ = true;
    }
}
bool RecorderApp::handleScreenSaverWake(const InputEvent& event)
{
    if (!settings_.triplePressWake) {
        wakeScreen();
        return true;
    }

    const char key = event.primaryKey != '\0' ? event.primaryKey : '?';
    const unsigned long now = millis();
    if (wakeConfirmCount_ == 0 ||
        now - wakeConfirmLastMs_ > kTriplePressWakeTimeoutMs ||
        wakeConfirmKey_ != key) {
        wakeConfirmKey_ = key;
        wakeConfirmCount_ = 1;
    } else if (wakeConfirmCount_ < 3) {
        ++wakeConfirmCount_;
    }
    wakeConfirmLastMs_ = now;

    if (screenSaverState_ == ScreenSaverState::kOff) {
        screenSaverState_ = ScreenSaverState::kDim;
        M5Cardputer.Display.wakeup();
        M5Cardputer.Display.setBrightness(kDimBrightness);
    }

    if (wakeConfirmCount_ >= 3) {
        wakeScreen();
        return true;
    }

    forceRedraw_ = true;
    return true;
}
void RecorderApp::resetWakeConfirmation()
{
    wakeConfirmKey_ = '\0';
    wakeConfirmCount_ = 0;
    wakeConfirmLastMs_ = 0;
}
void RecorderApp::wakeScreen()
{
    M5Cardputer.Display.wakeup();
    applyBrightness();
    screenSaverState_ = ScreenSaverState::kAwake;
    screenSaverManual_ = false;
    resetWakeConfirmation();
    forceRedraw_ = true;
}
void RecorderApp::drawScreenSaver(unsigned long now)
{
    auto& display = recorderCanvas;
    constexpr std::uint16_t background = TFT_BLACK;
    constexpr std::uint16_t muted = 0x7BEF;
    constexpr std::uint16_t accent = 0x05FF;
    constexpr std::uint16_t neonPink = 0xF81F;
    constexpr std::uint16_t deepBlue = 0x0014;

    display.fillSprite(background);
    const int horizon = 78;
    const bool dataRain = settings_.screenSaverStyle == 1;
    if (dataRain) {
        // Procedural data trails keep the animation tiny and avoid bitmap RAM.
        for (int column = 0; column < 15; ++column) {
            const int x = 4 + column * 16;
            const int head = static_cast<int>(
                (now / 38 + column * 23) % display.height());
            for (int trail = 0; trail < 5; ++trail) {
                const int y =
                    (head + display.height() - trail * 9) % display.height();
                const std::uint16_t color =
                    trail == 0 ? TFT_WHITE
                               : trail == 1 ? accent : 0x03E0;
                display.fillRect(x, y, 2, 6, color);
                if (((column + trail) & 1) != 0) {
                    display.drawPixel(x + 4, y + 2, color);
                }
            }
        }
    } else {
        // Cyberpunk horizon drifts slowly to avoid a static idle screen.
        for (int y = horizon; y < display.height(); y += 10) {
            display.drawFastHLine(0, y, display.width(), deepBlue);
        }
        const int drift = static_cast<int>((now / 90) % 24);
        for (int x = -display.width(); x < display.width() * 2; x += 24) {
            display.drawLine(display.width() / 2, horizon,
                             x + drift, display.height() - 1, deepBlue);
        }
    }
    display.setTextDatum(top_left);
    display.setTextFont(1);
    display.setTextColor(muted, background);
    display.setCursor(8, 8);
    display.print("RECORDER");
    display.setTextDatum(top_right);
    if (battery_.valid) {
        display.drawString(String(battery_.levelPercent) + "%",
                           display.width() - 8, 8);
    } else {
        display.drawString("--%", display.width() - 8, 8);
    }
    display.setTextDatum(top_left);

    if (wakeConfirmCount_ > 0) {
        display.setTextFont(2);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, background);
        display.drawString("Triple-Press Wake",
                           display.width() / 2, 44);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.drawString("Press same key 3 times",
                           display.width() / 2, 68);
        display.setTextFont(2);
        display.setTextColor(accent, background);
        display.drawString(
            "Wake " + String(wakeConfirmCount_) + "/3",
            display.width() / 2, 91);
        display.setTextDatum(top_left);
    } else if (state_ == State::kRecording) {
        const std::uint32_t recordingBytes =
            recordingBytes_.load(std::memory_order_relaxed);
        display.fillCircle(20, 45, 7, TFT_RED);
        display.setTextFont(4);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, background);
        display.drawString(formatTime(now - operationStartedMs_),
                           display.width() / 2, 58);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.drawString(
            String(static_cast<unsigned long>(recordingBytes / 1024)) +
                " KB",
            display.width() / 2, 88);
        display.setTextDatum(top_left);
    } else if (state_ == State::kPlaying) {
        const unsigned long elapsed = playbackElapsedMs();
        const std::uint32_t percent =
            playbackDurationMs_ == 0
                ? 0
                : elapsed * 100 / playbackDurationMs_;
        display.setTextFont(4);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, background);
        display.drawString(playbackPaused_ ? "PAUSED"
                                           : formatTime(elapsed),
                           display.width() / 2, 54);
        display.drawRoundRect(20, 80, display.width() - 40, 8, 3,
                              muted);
        display.fillRoundRect(
            22, 82, (display.width() - 44) * percent / 100, 4, 2,
            TFT_GREEN);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.drawString(
            "VOL " +
                String(static_cast<unsigned int>(
                    playbackVolume_ * 100U / 255U)) +
                "% " + playbackSpeedText(playbackSpeedIndex_),
            display.width() / 2, 99);
        display.setTextDatum(top_left);
    } else {
        if (dataRain) {
            display.fillRect(27, 27, display.width() - 54, 78, background);
            display.drawRoundRect(27, 27, display.width() - 54, 78, 5,
                                  0x03E0);
            const int pulse = static_cast<int>((now / 35) % 150);
            display.drawFastHLine(43, 84, 150, 0x03E0);
            display.fillRect(43 + pulse, 82, 4, 5, accent);
        } else {
            const int scanner = 22 + static_cast<int>((now / 45) % 50);
            display.drawFastHLine(12, scanner, display.width() - 24,
                                 neonPink);
            for (int x = 3; x < display.width(); x += 17) {
                const int height = 8 + ((x * 13) % 31);
                display.fillRect(x, horizon - height, 12, height,
                                 0x0841);
                display.drawFastVLine(x + 3, horizon - height + 4,
                                      max(1, height - 7), accent);
            }
        }
        display.setTextFont(2);
        display.setTextDatum(middle_center);
        display.setTextColor(dataRain ? TFT_GREEN : neonPink, background);
        display.drawString(dataRain ? "DATA RAIN // SYNC"
                                    : "AGENT // ONLINE",
                           display.width() / 2, 35);
        display.setTextFont(1);
        display.setTextColor(accent, background);
        display.drawString(dataRain ? "PACKETS STANDBY"
                                    : "NEURAL LINK STANDBY",
                           display.width() / 2, 58);
        display.setTextColor(TFT_WHITE, background);
        display.drawString("PRESS ANY KEY", display.width() / 2, 98);
        display.setTextDatum(top_left);
    }

    if (screenSaverManual_) {
        display.setTextDatum(bottom_right);
        display.setTextFont(1);
        display.setTextColor(muted, background);
        display.drawString("MANUAL", display.width() - 8,
                           display.height() - 6);
        display.setTextDatum(top_left);
    }
    recorderCanvas.pushSprite(0, 0);
}

}  // namespace cardputer_recorder
