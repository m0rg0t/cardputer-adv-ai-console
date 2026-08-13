#include "recorder/recorder_app.h"

#include <algorithm>
#include <ctime>

#include "recorder/app/app_shared.h"
#include "recorder/media/file_naming.h"

namespace cardputer_recorder {
namespace {

constexpr const char* kLockIndexPath = "/RECORDER.LCK";
constexpr std::size_t kMaxRenameBaseLength = 32;

struct RecordingSortInfo {
    String name;
    std::time_t modified = 0;
    std::uint8_t statusRank = 0;
};

std::uint8_t recordingStatusRank(const String& status)
{
    if (status == "ERROR") {
        return 0;
    }
    if (status == "WORK") {
        return 1;
    }
    if (status == "QUEUE") {
        return 2;
    }
    if (status == "SENT") {
        return 3;
    }
    if (status == "TEXT") {
        return 4;
    }
    return 5;
}

bool isSafeRenameCharacter(char character)
{
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') ||
           character == '-' || character == '_' || character == ' ';
}

String basenameWithoutWav(const String& filename)
{
    String base = filename;
    if (base.endsWith(".WAV") || base.endsWith(".wav")) {
        base.remove(base.length() - 4);
    }
    return base;
}

String routeSidecar(const String& filename)
{
    String path = "/" + basenameWithoutWav(filename);
    path += ".ROUTE";
    return path;
}

String agentMetadataSidecar(const String& filename)
{
    String path = "/" + basenameWithoutWav(filename);
    path += ".AGENT.JSON";
    return path;
}

}  // namespace

void RecorderApp::deleteSelected()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return;
    }
    const String filename = files_[selected_];
    if (isLocked(filename)) {
        message_ = "Locked";
        deleteConfirm_ = false;
        forceRedraw_ = true;
        return;
    }
    if (!deleteConfirm_ || deleteConfirmName_ != filename) {
        deleteConfirm_ = true;
        deleteConfirmName_ = filename;
        message_ = "ENTER confirms delete.";
        forceRedraw_ = true;
        return;
    }

    const String path = "/" + filename;
    if (!storage_.remove(path.c_str())) {
        setError("Could not delete recording.");
        return;
    }
    const String sidecar = routeSidecar(filename);
    if (storage_.exists(sidecar.c_str())) {
        storage_.remove(sidecar.c_str());
    }
    const String metadata = agentMetadataSidecar(filename);
    if (storage_.exists(metadata.c_str())) {
        storage_.remove(metadata.c_str());
    }
    message_ = "Deleted " + filename;
    deleteConfirm_ = false;
    deleteConfirmName_ = "";
    scanFiles();
    forceRedraw_ = true;
}
void RecorderApp::toggleLockSelected()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return;
    }

    const String filename = files_[selected_];
    auto found = std::find(lockedFiles_.begin(), lockedFiles_.end(),
                           filename);
    if (found == lockedFiles_.end()) {
        lockedFiles_.push_back(filename);
        message_ = "Locked " + filename;
    } else {
        lockedFiles_.erase(found);
        message_ = "Unlocked " + filename;
    }
    deleteConfirm_ = false;
    if (!saveLocks()) {
        setError("Could not save locks.");
        return;
    }
    forceRedraw_ = true;
}
bool RecorderApp::isLocked(const String& filename) const
{
    return std::find(lockedFiles_.begin(), lockedFiles_.end(), filename) !=
           lockedFiles_.end();
}
void RecorderApp::loadLocks()
{
    lockedFiles_.clear();
    File file = storage_.open(kLockIndexPath, FILE_READ);
    if (!file) {
        return;
    }
    while (file.available()) {
        String name = file.readStringUntil('\n');
        name.trim();
        String extension = name;
        extension.toLowerCase();
        if (extension.endsWith(".wav") &&
            std::find(lockedFiles_.begin(), lockedFiles_.end(), name) ==
                lockedFiles_.end()) {
            lockedFiles_.push_back(name);
        }
    }
    file.close();
}
bool RecorderApp::saveLocks()
{
    if (storage_.exists(kLockIndexPath)) {
        storage_.remove(kLockIndexPath);
    }
    if (lockedFiles_.empty()) {
        return true;
    }

    File file = storage_.open(kLockIndexPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    for (const String& name : lockedFiles_) {
        file.println(name);
    }
    file.close();
    return true;
}
void RecorderApp::scanFiles()
{
    const String previousSelection =
        !files_.empty() && selected_ >= 0 &&
                selected_ < static_cast<int>(files_.size())
            ? files_[selected_]
            : String();
    files_.clear();
    File directory = storage_.open("/", FILE_READ);
    if (!directory || !directory.isDirectory()) {
        setError("Could not read SD directory.");
        return;
    }
    File entry = directory.openNextFile();
    while (entry) {
        String name = entry.name();
        String normalizedName = name;
        normalizedName.toLowerCase();
        if (!entry.isDirectory() &&
            normalizedName.endsWith(".wav") &&
            entry.size() >= kPcmWavHeaderSize) {
            if (name.startsWith("/")) {
                name.remove(0, 1);
            }
            files_.push_back(name);
        }
        entry.close();
        entry = directory.openNextFile();
    }
    directory.close();
    loadLocks();
    bool prunedLocks = false;
    for (auto item = lockedFiles_.begin(); item != lockedFiles_.end();) {
        if (std::find(files_.begin(), files_.end(), *item) ==
            files_.end()) {
            item = lockedFiles_.erase(item);
            prunedLocks = true;
        } else {
            ++item;
        }
    }
    if (prunedLocks) {
        saveLocks();
    }
    sortFiles();
    if (files_.empty()) {
        selected_ = 0;
    } else {
        const auto previous =
            std::find(files_.begin(), files_.end(), previousSelection);
        if (previous != files_.end()) {
            selected_ = static_cast<int>(previous - files_.begin());
        } else if (selected_ >= static_cast<int>(files_.size())) {
            selected_ = files_.size() - 1;
        }
    }
    deleteConfirm_ = false;
    deleteConfirmName_ = "";
    servicePendingCount_ = uploader_.pendingRecordingCount();
}

void RecorderApp::sortFiles()
{
    if (files_.size() < 2) {
        return;
    }

    const String selectedName =
        selected_ >= 0 && selected_ < static_cast<int>(files_.size())
            ? files_[selected_]
            : String();
    std::vector<RecordingSortInfo> recordings;
    recordings.reserve(files_.size());
    for (const String& name : files_) {
        RecordingSortInfo info;
        info.name = name;
        const String path = "/" + name;
        File file = storage_.open(path.c_str(), FILE_READ);
        std::uint32_t size = 0;
        if (file) {
            size = static_cast<std::uint32_t>(file.size());
            info.modified = file.getLastWrite();
            file.close();
        }
        info.statusRank = recordingStatusRank(
            uploader_.recordingStatus(name, size));
        recordings.push_back(info);
    }

    const LibrarySortMode mode = settings_.librarySortMode;
    const auto comesBefore =
        [mode](const RecordingSortInfo& left,
               const RecordingSortInfo& right) {
            switch (mode) {
                case LibrarySortMode::kOldest:
                    if (left.modified != right.modified &&
                        left.modified > 0 && right.modified > 0) {
                        return left.modified < right.modified;
                    }
                    return left.name < right.name;
                case LibrarySortMode::kStatus:
                    if (left.statusRank != right.statusRank) {
                        return left.statusRank < right.statusRank;
                    }
                    return left.name > right.name;
                case LibrarySortMode::kName:
                    return left.name < right.name;
                case LibrarySortMode::kNewest:
                default:
                    if (left.modified != right.modified &&
                        left.modified > 0 && right.modified > 0) {
                        return left.modified > right.modified;
                    }
                    return left.name > right.name;
            }
        };
    for (std::size_t gap = recordings.size() / 2; gap > 0; gap /= 2) {
        for (std::size_t index = gap; index < recordings.size(); ++index) {
            RecordingSortInfo candidate = recordings[index];
            std::size_t position = index;
            while (position >= gap &&
                   comesBefore(candidate, recordings[position - gap])) {
                recordings[position] = recordings[position - gap];
                position -= gap;
            }
            recordings[position] = candidate;
        }
    }

    files_.clear();
    files_.reserve(recordings.size());
    for (const auto& recording : recordings) {
        files_.push_back(recording.name);
    }
    const auto selected =
        std::find(files_.begin(), files_.end(), selectedName);
    selected_ = selected == files_.end()
                    ? 0
                    : static_cast<int>(selected - files_.begin());
}

void RecorderApp::cycleLibrarySort(int offset)
{
    const int count = static_cast<int>(LibrarySortMode::kCount);
    const int current = static_cast<int>(settings_.librarySortMode);
    settings_.librarySortMode = static_cast<LibrarySortMode>(
        (current + count + offset) % count);
    sortFiles();
    saveSettings();
    message_ = "Sort: " + librarySortText();
    deleteConfirm_ = false;
    forceRedraw_ = true;
}

String RecorderApp::librarySortText() const
{
    switch (settings_.librarySortMode) {
        case LibrarySortMode::kOldest:
            return "OLD";
        case LibrarySortMode::kStatus:
            return "STATUS";
        case LibrarySortMode::kName:
            return "A-Z";
        case LibrarySortMode::kNewest:
        default:
            return "NEW";
    }
}
bool RecorderApp::chooseRecordingPath(char* path, std::size_t capacity)
{
    for (std::uint32_t index = 1; index <= 9999; ++index) {
        if (!makeRecordingPath(index, path, capacity)) {
            return false;
        }
        if (!storage_.exists(path)) {
            return true;
        }
    }
    return false;
}
String RecorderApp::storageUsageText() const
{
    if (!storage_.isMounted()) {
        return "SD unavailable";
    }
    const std::uint64_t capacity = storage_.capacityBytes();
    const std::uint64_t used = storage_.usedBytes();
    if (capacity == 0 || used > capacity) {
        return "SD size unknown";
    }
    return "FREE " + formatByteCount(capacity - used) + " / " +
           formatByteCount(capacity);
}
String RecorderApp::selectedRecordingDetail()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return storageUsageText();
    }

    const String path = "/" + files_[selected_];
    const std::uint32_t size = storage_.fileSize(path.c_str());
    String detail = formatByteCount(size);

    File file = storage_.open(path.c_str(), FILE_READ);
    WavReader detailReader;
    if (detailReader.begin(file)) {
        const WavInfo& wav = detailReader.info();
        const std::uint32_t bytesPerSecond =
            wav.spec.sampleRate * wav.spec.channels *
            (wav.spec.bitsPerSample / 8);
        if (bytesPerSecond > 0) {
            const unsigned long durationMs =
                static_cast<unsigned long>(
                    static_cast<std::uint64_t>(wav.dataSize) * 1000ULL /
                    bytesPerSecond);
            detail += "  " + formatTime(durationMs);
        }
        detailReader.end();
    } else if (file) {
        file.close();
    }
    return detail + "  " + uploader_.recordingStatus(files_[selected_], size) +
           "  " + uploader_.shortStatus();
}
void RecorderApp::openRecordingDetail()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return;
    }
    if (state_ != State::kOutbox) {
        recordingDetailReturnState_ = State::kBrowsing;
    }
    const String filename = files_[selected_];
    const String path = "/" + filename;
    const std::uint32_t size = storage_.fileSize(path.c_str());
    const String status = uploader_.recordingStatus(filename, size);
    const bool hasMetadata =
        uploader_.recordingMetadata(filename, recordingMetadata_);

    recordingDetailText_ = filename + "\n" +
                           uploader_.recordingDeliveryDetail(filename, size);
    if (hasMetadata) {
        recordingDetailText_ += "\nDestination: " + recordingMetadata_.destination;
        recordingDetailText_ += "\n---\n";
        if (recordingMetadata_.transcript.length() > 0) {
            recordingDetailText_ += recordingMetadata_.transcript;
            if (!recordingMetadata_.transcriptComplete) {
                recordingDetailText_ +=
                    "\n\n[Transcript preview truncated on SD; full text is in Obsidian.]";
            }
        } else {
            recordingDetailText_ += "Transcript is not available on this SD card.";
        }
    } else if (status == "SENT") {
        recordingDetailText_ +=
            "\n\nUploaded by an earlier firmware; transcript was not cached on SD.";
    } else if (status == "QUEUE" || status == "ERROR") {
        recordingDetailText_ +=
            "\n\nThe firmware will retry automatically.";
    }
    recordingDetailScroll_ = 0;
    state_ = State::kRecordingDetail;
    message_ = "Recording details";
    forceRedraw_ = true;
}
void RecorderApp::handleRecordingDetailInput(const InputEvent& event)
{
    if (event.help) {
        openHelp();
        return;
    }
    if ((event.primaryKey == 'p' || event.primaryKey == 'P')) {
        message_ = "Profile: " + uploader_.cycleVoiceProfile(1);
        recordingDetailText_ += "\nSelected profile: " +
                                uploader_.currentVoiceProfile();
        forceRedraw_ = true;
    } else if ((event.primaryKey == 'e' || event.primaryKey == 'E') &&
               !files_.empty()) {
        const String filename = files_[selected_];
        if (uploader_.reprocessVoiceJob(
                filename, uploader_.currentVoiceProfile())) {
            message_ = "Reprocessing with " +
                       uploader_.currentVoiceProfile();
            const std::uint32_t size =
                storage_.fileSize(("/" + filename).c_str());
            uploader_.recordingMetadata(filename, recordingMetadata_);
            recordingDetailText_ = filename + "\n" +
                uploader_.recordingDeliveryDetail(filename, size);
        } else {
            message_ = "Reprocess failed: " + uploader_.gatewayDiagnostic();
        }
        forceRedraw_ = true;
    } else if ((event.primaryKey == 'c' || event.primaryKey == 'C') &&
               recordingMetadata_.transcript.length() > 0) {
        pendingCodexTranscript_ = recordingMetadata_.transcript;
        openCodexChats();
        message_ = "Choose a chat for cached transcript.";
        forceRedraw_ = true;
    } else if ((event.primaryKey == 'r' || event.primaryKey == 'R') &&
               !files_.empty()) {
        if (uploader_.retryVoiceJob(files_[selected_])) {
            message_ = "Retry queued.";
        } else {
            uploader_.requestSoon();
            message_ = "Upload requested.";
        }
        forceRedraw_ = true;
    } else if (event.back || event.confirm || event.primaryKey == 'i' ||
        event.primaryKey == 'I') {
        state_ = recordingDetailReturnState_;
        if (state_ == State::kOutbox) {
            refreshOutbox();
        } else {
            message_ = "Recorder";
        }
        forceRedraw_ = true;
    } else if (event.up) {
        recordingDetailScroll_ = max(0, recordingDetailScroll_ - 3);
        forceRedraw_ = true;
    } else if (event.down) {
        recordingDetailScroll_ += 3;
        forceRedraw_ = true;
    }
}
void RecorderApp::beginRenameSelected()
{
    if (files_.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(files_.size())) {
        return;
    }
    deleteConfirm_ = false;
    renameOriginalName_ = files_[selected_];
    renameText_ = basenameWithoutWav(renameOriginalName_);
    state_ = State::kRename;
    message_ = "Rename";
    forceRedraw_ = true;
}
void RecorderApp::handleRenameInput(const InputEvent& event)
{
    if (event.back) {
        state_ = State::kBrowsing;
        renameOriginalName_ = "";
        renameText_ = "";
        message_ = "Rename canceled.";
        forceRedraw_ = true;
        return;
    }
    if (event.deletePressed && renameText_.length() > 0) {
        renameText_.remove(renameText_.length() - 1);
        forceRedraw_ = true;
    }
    for (std::size_t index = 0; index < event.text.length(); ++index) {
        char character = event.text[index];
        if (!isSafeRenameCharacter(character) ||
            renameText_.length() >= kMaxRenameBaseLength) {
            continue;
        }
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
        renameText_ += character;
        forceRedraw_ = true;
    }
    if (event.confirm) {
        commitRename();
    }
}
void RecorderApp::commitRename()
{
    renameText_.trim();
    if (renameText_.length() == 0) {
        message_ = "Name required.";
        forceRedraw_ = true;
        return;
    }

    String targetName = renameText_;
    targetName.toUpperCase();
    targetName += ".WAV";
    if (targetName == renameOriginalName_) {
        state_ = State::kBrowsing;
        renameOriginalName_ = "";
        renameText_ = "";
        message_ = "Rename unchanged.";
        forceRedraw_ = true;
        return;
    }

    const String fromPath = "/" + renameOriginalName_;
    const String toPath = "/" + targetName;
    if (storage_.exists(toPath.c_str())) {
        message_ = "Name exists.";
        forceRedraw_ = true;
        return;
    }
    const bool wasLocked = isLocked(renameOriginalName_);
    if (!storage_.rename(fromPath.c_str(), toPath.c_str())) {
        setError("Could not rename recording.");
        return;
    }
    const String fromRoute = routeSidecar(renameOriginalName_);
    const String toRoute = routeSidecar(targetName);
    if (storage_.exists(fromRoute.c_str()) &&
        !storage_.rename(fromRoute.c_str(), toRoute.c_str())) {
        Serial.println("[RECORDER] Could not rename route sidecar.");
    }
    const String fromMetadata = agentMetadataSidecar(renameOriginalName_);
    const String toMetadata = agentMetadataSidecar(targetName);
    if (storage_.exists(fromMetadata.c_str()) &&
        !storage_.rename(fromMetadata.c_str(), toMetadata.c_str())) {
        Serial.println("[RECORDER] Could not rename metadata sidecar.");
    }
    if (wasLocked) {
        auto found = std::find(lockedFiles_.begin(), lockedFiles_.end(),
                               renameOriginalName_);
        if (found != lockedFiles_.end()) {
            *found = targetName;
            if (!saveLocks()) {
                setError("Could not save locks.");
                return;
            }
        }
    }
    state_ = State::kBrowsing;
    message_ = "Renamed " + targetName;
    renameOriginalName_ = "";
    renameText_ = "";
    scanFiles();
    for (std::size_t index = 0; index < files_.size(); ++index) {
        if (files_[index] == targetName) {
            selected_ = static_cast<int>(index);
            break;
        }
    }
    forceRedraw_ = true;
}

}  // namespace cardputer_recorder
