#include "recorder/recorder_app.h"

namespace cardputer_recorder {

void RecorderApp::openCodexChats()
{
    state_ = State::kCodexChats;
    selectedCodexChat_ = 0;
    message_ = "Loading Codex chats...";
    forceRedraw_ = true;
    draw();
    refreshCodexChats();
}

void RecorderApp::refreshCodexChats()
{
    if (uploader_.refreshChats(codexChats_)) {
        if (codexChats_.empty()) {
            selectedCodexChat_ = 0;
            message_ = "No Codex chats.";
        } else {
            selectedCodexChat_ = min(
                selectedCodexChat_, static_cast<int>(codexChats_.size()) - 1);
            message_ = String(codexChats_.size()) + " chats";
        }
    } else {
        codexChats_.clear();
        selectedCodexChat_ = 0;
        message_ = uploader_.gatewayDiagnostic();
        if (message_.length() > 52) {
            message_ = message_.substring(0, 52);
        }
    }
    forceRedraw_ = true;
}

void RecorderApp::handleCodexChatsInput(const InputEvent& event)
{
    if (event.help) {
        openHelp();
        return;
    }
    if (event.back || event.primaryKey == 'l' ||
        event.primaryKey == 'L') {
        pendingCodexTranscript_ = "";
        state_ = State::kBrowsing;
        message_ = "Recorder";
        forceRedraw_ = true;
        return;
    }
    if (event.primaryKey == 'n' || event.primaryKey == 'N') {
        UploadService::CodexChat chat;
        message_ = "Creating Codex task...";
        forceRedraw_ = true;
        draw();
        if (uploader_.createCodexChat(chat)) {
            codexChats_.insert(codexChats_.begin(), chat);
            selectedCodexChat_ = 0;
            openCodexConversation();
            openCodexCompose();
        } else {
            message_ = "Could not create task.";
            forceRedraw_ = true;
        }
        return;
    }
    if ((event.primaryKey == 'c' || event.primaryKey == 'C') &&
        codexChats_.empty()) {
        refreshCodexChats();
        return;
    }
    if (event.up && !codexChats_.empty()) {
        selectedCodexChat_ =
            (selectedCodexChat_ + static_cast<int>(codexChats_.size()) - 1) %
            static_cast<int>(codexChats_.size());
        forceRedraw_ = true;
    } else if (event.down && !codexChats_.empty()) {
        selectedCodexChat_ =
            (selectedCodexChat_ + 1) % static_cast<int>(codexChats_.size());
        forceRedraw_ = true;
    } else if (event.confirm && !codexChats_.empty()) {
        if (pendingCodexTranscript_.length() > 0) {
            selectedCodexThreadId_ = codexChats_[selectedCodexChat_].id;
            selectedCodexThreadName_ = codexChats_[selectedCodexChat_].name;
            const String transcript = pendingCodexTranscript_;
            pendingCodexTranscript_ = "";
            codexConversation_ = "YOU (cached voice): " + transcript + "\n\n";
            if (uploader_.sendCodexMessage(
                    selectedCodexThreadId_, transcript, codexJobId_)) {
                codexJob_ = UploadService::CodexJobStatus{};
                codexJob_.status = "in_progress";
                codexJob_.text = "Waiting for Codex...";
                state_ = State::kCodexConversation;
                message_ = "Cached transcript sent.";
                lastCodexPollMs_ = 0;
            } else {
                message_ = "Could not send transcript.";
            }
            forceRedraw_ = true;
        } else {
            openCodexConversation();
        }
    } else if ((event.primaryKey == 'r' || event.primaryKey == 'R') &&
               !codexChats_.empty()) {
        selectedCodexThreadId_ = codexChats_[selectedCodexChat_].id;
        selectedCodexThreadName_ = codexChats_[selectedCodexChat_].name;
        startRecording(UploadService::Destination::kCodex);
    } else if ((event.primaryKey == 'b' || event.primaryKey == 'B') &&
               !codexChats_.empty()) {
        selectedCodexThreadId_ = codexChats_[selectedCodexChat_].id;
        selectedCodexThreadName_ = codexChats_[selectedCodexChat_].name;
        startRecording(UploadService::Destination::kBoth);
    } else if ((event.primaryKey == 't' || event.primaryKey == 'T') &&
               !codexChats_.empty()) {
        selectedCodexThreadId_ = codexChats_[selectedCodexChat_].id;
        selectedCodexThreadName_ = codexChats_[selectedCodexChat_].name;
        openCodexCompose();
    } else if (event.primaryKey == 'c' || event.primaryKey == 'C') {
        refreshCodexChats();
    }
}

void RecorderApp::openCodexConversation()
{
    if (codexChats_.empty()) {
        return;
    }
    selectedCodexThreadId_ = codexChats_[selectedCodexChat_].id;
    selectedCodexThreadName_ = codexChats_[selectedCodexChat_].name;
    state_ = State::kCodexConversation;
    codexScroll_ = 0;
    message_ = "Loading conversation...";
    forceRedraw_ = true;
    draw();
    if (!uploader_.fetchConversation(selectedCodexThreadId_,
                                     codexConversation_)) {
        codexConversation_ = "Could not load this conversation.";
        message_ = "Gateway error";
    } else {
        message_ = selectedCodexThreadName_;
    }
    forceRedraw_ = true;
}

void RecorderApp::handleCodexConversationInput(const InputEvent& event)
{
    if (event.help) {
        openHelp();
        return;
    }
    if (event.primaryKey == 'l' || event.primaryKey == 'L') {
        state_ = State::kBrowsing;
        message_ = "Recorder";
        forceRedraw_ = true;
        return;
    }
    if (codexJob_.approvalId.length() > 0) {
        String decision;
        if (event.confirm) {
            decision = "accept";
        } else if (event.primaryKey == 's' || event.primaryKey == 'S') {
            decision = "acceptForSession";
        } else if (event.primaryKey == 'd' || event.primaryKey == 'D') {
            decision = "decline";
        } else if (event.primaryKey == 'x' || event.primaryKey == 'X') {
            decision = "cancel";
        }
        if (decision.length() > 0) {
            if (uploader_.answerCodexApproval(
                    codexJobId_, codexJob_.approvalId, decision)) {
                codexJob_.approvalId = "";
                message_ = "Approval sent.";
                lastCodexPollMs_ = 0;
            } else {
                message_ = "Approval failed.";
            }
            forceRedraw_ = true;
            return;
        }
    }

    if (codexSpeechState_.load(std::memory_order_acquire) == 1) {
        if (event.back) {
            message_ = "Speech is still being prepared.";
            forceRedraw_ = true;
        } else if (event.up) {
            codexScroll_ = max(0, codexScroll_ - 3);
            forceRedraw_ = true;
        } else if (event.down) {
            codexScroll_ += 3;
            forceRedraw_ = true;
        }
        return;
    }

    if ((event.primaryKey == 'x' || event.primaryKey == 'X') &&
        codexJobId_.length() > 0) {
        if (uploader_.cancelCodexJob(codexJobId_)) {
            codexJob_.status = "interrupted";
            codexJobId_ = "";
            message_ = "Codex interrupted.";
        } else {
            message_ = "Could not interrupt Codex.";
        }
        forceRedraw_ = true;
        return;
    }

    if (event.back) {
        state_ = State::kCodexChats;
        message_ = "Codex chats";
        forceRedraw_ = true;
    } else if (event.up) {
        codexScroll_ = max(0, codexScroll_ - 3);
        forceRedraw_ = true;
    } else if (event.down) {
        codexScroll_ += 3;
        forceRedraw_ = true;
    } else if (event.primaryKey == 'e' || event.primaryKey == 'E') {
        codexScroll_ = 32767;
        message_ = "End of conversation.";
        forceRedraw_ = true;
    } else if (event.primaryKey == 'v' || event.primaryKey == 'V') {
        startCodexSpeech(false);
    } else if (event.primaryKey == 'a' || event.primaryKey == 'A') {
        startCodexSpeech(true);
    } else if (event.primaryKey == 'r' || event.primaryKey == 'R') {
        startRecording(UploadService::Destination::kCodex);
    } else if (event.primaryKey == 'b' || event.primaryKey == 'B') {
        startRecording(UploadService::Destination::kBoth);
    } else if (event.primaryKey == 't' || event.primaryKey == 'T') {
        openCodexCompose();
    } else if (event.confirm && codexJobId_.length() == 0) {
        openCodexConversation();
    } else if (event.confirm) {
        lastCodexPollMs_ = 0;
        serviceCodexJob();
    }
}

void RecorderApp::startCodexSpeech(bool conversation)
{
    if (selectedCodexThreadId_.length() == 0 || uploader_.transferActive()) {
        message_ = "Network audio is busy.";
        forceRedraw_ = true;
        return;
    }
    std::uint8_t expected = 0;
    if (!codexSpeechState_.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel)) {
        return;
    }
    codexSpeechConversation_ = conversation;
    message_ = conversation ? "Preparing conversation audio..."
                            : "Preparing latest reply audio...";
    forceRedraw_ = true;
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(codexSpeechTaskEntry, "codex_tts", 8192,
                                this, 1, &task, 0) != pdPASS) {
        codexSpeechState_.store(0, std::memory_order_release);
        message_ = "Could not start speech task.";
    }
}

void RecorderApp::codexSpeechTaskEntry(void* context)
{
    static_cast<RecorderApp*>(context)->codexSpeechTask();
}

void RecorderApp::codexSpeechTask()
{
    const bool ready = uploader_.downloadCodexSpeech(
        selectedCodexThreadId_, codexSpeechConversation_,
        "/CODEX_TTS.WAV");
    codexSpeechState_.store(ready ? 2 : 3, std::memory_order_release);
    vTaskDelete(nullptr);
}

void RecorderApp::serviceCodexSpeech()
{
    const std::uint8_t speechState =
        codexSpeechState_.load(std::memory_order_acquire);
    if (speechState == 1 || speechState == 0) {
        return;
    }
    codexSpeechState_.store(0, std::memory_order_release);
    if (speechState == 2 &&
        startPlaybackPath("/CODEX_TTS.WAV", State::kCodexConversation,
                          true)) {
        message_ = "Playing Codex response.";
    } else {
        storage_.remove("/CODEX_TTS.WAV");
        message_ = "Could not prepare speech: " +
                   uploader_.gatewayDiagnostic();
    }
    forceRedraw_ = true;
}

void RecorderApp::openCodexCompose()
{
    codexComposeText_ = "";
    state_ = State::kCodexCompose;
    message_ = "Type a Codex message";
    forceRedraw_ = true;
}

void RecorderApp::handleCodexComposeInput(const InputEvent& event)
{
    if (event.back) {
        state_ = State::kCodexConversation;
        message_ = "Message canceled.";
        forceRedraw_ = true;
        return;
    }
    if (event.deletePressed && codexComposeText_.length() > 0) {
        codexComposeText_.remove(codexComposeText_.length() - 1);
        forceRedraw_ = true;
    }
    for (std::size_t index = 0; index < event.text.length(); ++index) {
        const char character = event.text[index];
        if (character >= 32 && character <= 126 &&
            codexComposeText_.length() < 1000) {
            codexComposeText_ += character;
            forceRedraw_ = true;
        }
    }
    if (event.confirm) {
        sendCodexCompose();
    }
}

void RecorderApp::sendCodexCompose()
{
    codexComposeText_.trim();
    if (codexComposeText_.length() == 0) {
        message_ = "Message is empty.";
        forceRedraw_ = true;
        return;
    }
    message_ = "Sending to Codex...";
    forceRedraw_ = true;
    draw();
    if (!uploader_.sendCodexMessage(
            selectedCodexThreadId_, codexComposeText_, codexJobId_)) {
        message_ = "Codex send failed.";
        forceRedraw_ = true;
        return;
    }
    codexJob_ = UploadService::CodexJobStatus{};
    codexJob_.status = "in_progress";
    codexJob_.text = "Waiting for Codex...";
    codexComposeText_ = "";
    lastCodexPollMs_ = 0;
    state_ = State::kCodexConversation;
    message_ = "Codex working";
    forceRedraw_ = true;
}

void RecorderApp::serviceCodexJob()
{
    if (state_ != State::kCodexConversation || codexJobId_.length() == 0 ||
        millis() - lastCodexPollMs_ < 2500) {
        return;
    }
    lastCodexPollMs_ = millis();
    if (!uploader_.pollCodexJob(codexJobId_, codexJob_)) {
        message_ = "Codex status unavailable";
        forceRedraw_ = true;
        return;
    }
    message_ = codexJob_.approvalId.length() > 0
                   ? "Approval required"
                   : "Codex " + codexJob_.status;
    if (codexJob_.status == "completed" ||
        codexJob_.status == "failed" ||
        codexJob_.status == "interrupted") {
        String refreshed;
        if (uploader_.fetchConversation(selectedCodexThreadId_, refreshed)) {
            codexConversation_ = refreshed;
        } else if (codexJob_.text.length() > 0) {
            codexConversation_ += "\nCODEX: " + codexJob_.text;
        }
        codexJobId_ = "";
    }
    forceRedraw_ = true;
}

}  // namespace cardputer_recorder
