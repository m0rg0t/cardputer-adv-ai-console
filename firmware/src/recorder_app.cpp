#include "recorder/recorder_app.h"

#include "recorder/app/app_shared.h"

namespace cardputer_recorder {

void RecorderApp::begin()
{
    Serial.begin(115200);
    if (!board_.begin()) {
        setError("Cardputer ADV required.");
        return;
    }

    audio_.begin();
    power_.begin();
    const bool storageReady = storage_.begin();
    loadSettings();
    updateBattery(true);
    auto& display = M5Cardputer.Display;
    display.setRotation(1);
    applyBrightness();
    display.fillScreen(TFT_BLACK);
    display.setTextWrap(false);
    recorderCanvas.setColorDepth(8);
    recorderCanvas.createSprite(display.width(), display.height());
    recorderCanvas.setTextWrap(false);
    resetScreenSaverTimer();

    if (!storageReady) {
        setError("Insert a writable microSD card.");
    } else {
        uploader_.begin(storage_);
        scanFiles();
        message_ = "Hold G0 for settings.";
    }
    draw();
}

void RecorderApp::update()
{
    board_.update();
    storage_.update();
    audio_.update();
    power_.update();
    updateBattery();

    // Audio capture/playback and SD uploads never contend with each other.
    // The queue is serviced only from the idle library screen.
    const bool networkUiAllowed =
        state_ == State::kBrowsing || state_ == State::kOutbox ||
        state_ == State::kCodexChats ||
        state_ == State::kCodexConversation ||
        (state_ == State::kSettings &&
         (settingsPage_ == SettingsPage::kNetwork ||
          settingsPage_ == SettingsPage::kServices));
    uploader_.update(
        networkUiAllowed &&
        codexSpeechState_.load(std::memory_order_acquire) == 0);
    if (uploader_.takeRecordingChanged()) {
        scanFiles();
        if (state_ == State::kOutbox) {
            refreshOutbox();
        }
        if (settingsPage_ == SettingsPage::kServices) {
            servicePendingCount_ = uploader_.pendingRecordingCount();
        }
        forceRedraw_ = true;
    }
    const String submittedJobId = uploader_.takeLastJobId();
    if (submittedJobId.length() > 0) {
        const String submittedThreadId = uploader_.takeLastJobThreadId();
        if (submittedThreadId.length() > 0) {
            selectedCodexThreadId_ = submittedThreadId;
            for (const auto& chat : codexChats_) {
                if (chat.id == submittedThreadId) {
                    selectedCodexThreadName_ = chat.name;
                    break;
                }
            }
        }
        codexJobId_ = submittedJobId;
        codexJob_ = UploadService::CodexJobStatus{};
        codexJob_.status = "in_progress";
        codexJob_.text = "Voice message sent. Waiting for Codex...";
        lastCodexPollMs_ = 0;
        if (selectedCodexThreadId_.length() > 0) {
            state_ = State::kCodexConversation;
        }
        forceRedraw_ = true;
    }

    const InputEvent event = input_.poll();
    handleInput(event);
    serviceScreenSaver();

    if (state_ == State::kRecording ||
        (state_ == State::kHelp && helpReturnState_ == State::kRecording)) {
        serviceRecording();
    } else if (state_ == State::kSaving) {
        serviceSaving();
    } else if (state_ == State::kPlaying ||
               (state_ == State::kHelp &&
                helpReturnState_ == State::kPlaying)) {
        servicePlayback();
    }
    serviceCodexJob();
    serviceCodexSpeech();

    draw();
    delay(1);
}

void RecorderApp::handleInput(const InputEvent& event)
{
    const bool hasInput = anyInput(event);
    if (hasInput) {
        if (screenSaverState_ != ScreenSaverState::kAwake) {
            handleScreenSaverWake(event);
            resetScreenSaverTimer();
            return;
        }
        resetScreenSaverTimer();
    }

    if (state_ == State::kRecording) {
        if (event.help) {
            openHelp();
            return;
        }
        if (event.g0) {
            enterScreenSaver(true);
            return;
        }
        if (event.primaryKey == 'p' || event.primaryKey == 'P' ||
            event.primaryKey == ' ') {
            toggleRecordingPause();
            return;
        }
        if (millis() - operationStartedMs_ >=
                kRecordingStopGuardMs &&
            (event.confirm || event.back || event.record)) {
            stopRecording(true);
        }
        return;
    }
    if (state_ == State::kSaving) {
        return;
    }
    if (state_ == State::kSettings) {
        handleSettingsInput(event);
        return;
    }
    if (state_ == State::kHelp) {
        handleHelpInput(event);
        return;
    }
    if (state_ == State::kRename) {
        handleRenameInput(event);
        return;
    }
    if (state_ == State::kRecordingDetail) {
        handleRecordingDetailInput(event);
        return;
    }
    if (state_ == State::kOutbox) {
        handleOutboxInput(event);
        return;
    }
    if (state_ == State::kCodexChats) {
        handleCodexChatsInput(event);
        return;
    }
    if (state_ == State::kCodexConversation) {
        handleCodexConversationInput(event);
        return;
    }
    if (state_ == State::kCodexCompose) {
        handleCodexComposeInput(event);
        return;
    }
    if (state_ == State::kPlaying) {
        if (event.help) {
            openHelp();
            return;
        }
        if (event.g0) {
            enterScreenSaver(true);
            return;
        }
        if (event.up) {
            adjustVolume(16);
        } else if (event.down) {
            adjustVolume(-16);
        }
        if (event.left) {
            seekPlayback(-settings_.seekStepSeconds);
        } else if (event.right) {
            seekPlayback(settings_.seekStepSeconds);
        }
        if (event.speedDown) {
            changePlaybackSpeed(-1);
        } else if (event.speedUp) {
            changePlaybackSpeed(1);
        }
        if (event.confirm) {
            togglePlaybackPause();
        } else if (event.back) {
            stopPlayback();
        }
        return;
    }
    if (state_ == State::kError) {
        if (event.confirm) {
            if (storage_.remount()) {
                state_ = State::kBrowsing;
                scanFiles();
                message_ = "Storage ready.";
                forceRedraw_ = true;
            }
        }
        return;
    }

    if (event.g0) {
        enterScreenSaver(true);
        return;
    }
    if (event.settings) {
        openSettings();
        return;
    }
    if (event.help) {
        openHelp();
        return;
    }
    if (event.primaryKey == 'c' || event.primaryKey == 'C') {
        openCodexChats();
        return;
    }
    if (event.primaryKey == 'o' || event.primaryKey == 'O') {
        openOutbox();
        return;
    }
    if (event.primaryKey == 's' || event.primaryKey == 'S') {
        cycleLibrarySort(1);
        return;
    }
    if (event.primaryKey == 'p' || event.primaryKey == 'P') {
        message_ = "Profile: " + uploader_.cycleVoiceProfile(1);
        forceRedraw_ = true;
        return;
    }
    if ((event.primaryKey == 'i' || event.primaryKey == 'I') &&
        !files_.empty()) {
        openRecordingDetail();
        return;
    }
    if (deleteConfirm_ && (event.back || event.up || event.down ||
                           event.left || event.right || event.record ||
                           event.g0 || event.settings)) {
        deleteConfirm_ = false;
        deleteConfirmName_ = "";
        message_ = "Delete canceled.";
        forceRedraw_ = true;
    }
    if (deleteConfirm_ && event.confirm) {
        deleteSelected();
    } else if (event.up && !files_.empty()) {
        selected_ =
            (selected_ + static_cast<int>(files_.size()) - 1) %
            static_cast<int>(files_.size());
        deleteConfirm_ = false;
        forceRedraw_ = true;
    } else if (event.down && !files_.empty()) {
        selected_ = (selected_ + 1) % static_cast<int>(files_.size());
        deleteConfirm_ = false;
        forceRedraw_ = true;
    } else if (event.left && !files_.empty()) {
        toggleLockSelected();
    } else if (event.right && !files_.empty()) {
        beginRenameSelected();
    } else if (event.record) {
        startRecording();
    } else if (event.confirm && !files_.empty()) {
        startPlayback();
    } else if (event.deletePressed && !files_.empty()) {
        deleteSelected();
    }
}

void RecorderApp::openHelp()
{
    helpReturnState_ = state_;
    switch (state_) {
        case State::kOutbox:
            helpPage_ = 4;
            break;
        case State::kRecording:
            helpPage_ = 2;
            break;
        case State::kPlaying:
            helpPage_ = 3;
            break;
        case State::kCodexChats:
            helpPage_ = 5;
            break;
        case State::kCodexConversation:
            helpPage_ = 6;
            break;
        case State::kRecordingDetail:
            helpPage_ = 7;
            break;
        case State::kSettings:
            helpPage_ = 8;
            break;
        default:
            helpPage_ = 0;
            break;
    }
    deleteConfirm_ = false;
    deleteConfirmName_ = "";
    state_ = State::kHelp;
    message_ = "Help";
    forceRedraw_ = true;
}

void RecorderApp::handleHelpInput(const InputEvent& event)
{
    constexpr std::uint8_t kHelpPageCount = 10;
    if (event.back || event.confirm || event.help) {
        state_ = helpReturnState_;
        message_ = "Help closed.";
        forceRedraw_ = true;
        return;
    }
    if (event.left || event.up) {
        helpPage_ = static_cast<std::uint8_t>(
            (helpPage_ + kHelpPageCount - 1) % kHelpPageCount);
        forceRedraw_ = true;
    } else if (event.right || event.down) {
        helpPage_ =
            static_cast<std::uint8_t>((helpPage_ + 1) % kHelpPageCount);
        forceRedraw_ = true;
    }
}

void RecorderApp::updateBattery(bool force)
{
    const unsigned long now = millis();
    if (!force && now - lastBatteryReadMs_ < 5000) {
        return;
    }
    lastBatteryReadMs_ = now;
    battery_ = power_.readBattery();
    forceRedraw_ = true;
}

bool RecorderApp::shouldAutoSaveForLowBattery() const
{
    return settings_.lowBatterySavePercent > 0 && battery_.valid &&
           battery_.levelPercent <= settings_.lowBatterySavePercent;
}

void RecorderApp::setError(const String& message)
{
    if (state_ == State::kRecording) {
        stopRecording(false);
    } else if (state_ == State::kPlaying) {
        stopPlayback();
    }
    state_ = State::kError;
    message_ = message;
    forceRedraw_ = true;
    Serial.println("[RECORDER] " + message);
}

}  // namespace cardputer_recorder
