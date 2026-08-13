#include "recorder/recorder_app.h"

#include <algorithm>

namespace cardputer_recorder {

void RecorderApp::refreshOutbox()
{
    outboxFiles_.clear();
    for (const String& filename : files_) {
        const std::uint32_t size =
            storage_.fileSize(("/" + filename).c_str());
        const String status = uploader_.recordingStatus(filename, size);
        if (status != "TEXT" && status != "SENT") {
            outboxFiles_.push_back(filename);
        }
    }
    if (outboxFiles_.empty()) {
        selectedOutbox_ = 0;
        message_ = "Outbox is empty.";
    } else {
        selectedOutbox_ = min(
            selectedOutbox_, static_cast<int>(outboxFiles_.size()) - 1);
        message_ = String(outboxFiles_.size()) + " pending recordings";
    }
    forceRedraw_ = true;
}

void RecorderApp::openOutbox()
{
    selectedOutbox_ = 0;
    state_ = State::kOutbox;
    refreshOutbox();
}

void RecorderApp::handleOutboxInput(const InputEvent& event)
{
    if (event.help) {
        openHelp();
        return;
    }
    if (event.back || event.primaryKey == 'o' || event.primaryKey == 'O' ||
        event.primaryKey == 'l' || event.primaryKey == 'L') {
        state_ = State::kBrowsing;
        message_ = "Recorder";
        forceRedraw_ = true;
        return;
    }
    if (event.up && !outboxFiles_.empty()) {
        selectedOutbox_ =
            (selectedOutbox_ + static_cast<int>(outboxFiles_.size()) - 1) %
            static_cast<int>(outboxFiles_.size());
        forceRedraw_ = true;
    } else if (event.down && !outboxFiles_.empty()) {
        selectedOutbox_ =
            (selectedOutbox_ + 1) % static_cast<int>(outboxFiles_.size());
        forceRedraw_ = true;
    } else if (event.confirm && !outboxFiles_.empty()) {
        const auto found = std::find(
            files_.begin(), files_.end(), outboxFiles_[selectedOutbox_]);
        if (found != files_.end()) {
            selected_ = static_cast<int>(found - files_.begin());
            recordingDetailReturnState_ = State::kOutbox;
            openRecordingDetail();
        }
    } else if ((event.primaryKey == 'r' || event.primaryKey == 'R') &&
               !outboxFiles_.empty()) {
        const String filename = outboxFiles_[selectedOutbox_];
        if (uploader_.retryVoiceJob(filename)) {
            message_ = "Retry queued.";
            uploader_.requestSoon();
        } else {
            // A recording without a server job is already queued locally.
            uploader_.requestSoon();
            message_ = "Local upload requested.";
        }
        refreshOutbox();
    } else if (event.primaryKey == 'a' || event.primaryKey == 'A') {
        std::size_t count = 0;
        if (uploader_.retryFailedVoiceJobs(count)) {
            message_ = count == 0 ? "No failed jobs."
                                  : "Retrying " + String(count) + " jobs.";
            uploader_.requestSoon();
        } else {
            message_ = "Retry all failed.";
        }
        forceRedraw_ = true;
    } else if ((event.primaryKey == 'x' || event.primaryKey == 'X') &&
               !outboxFiles_.empty()) {
        if (uploader_.cancelVoiceJob(outboxFiles_[selectedOutbox_])) {
            message_ = "Processing canceled.";
            refreshOutbox();
        } else {
            message_ = "Nothing active to cancel.";
            forceRedraw_ = true;
        }
    }
}

}  // namespace cardputer_recorder
