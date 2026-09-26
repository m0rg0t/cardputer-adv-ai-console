# Audio and storage

## Audio lifecycle

Cardputer ADV uses an ES8311 codec. Microphone and speaker operation are
mutually exclusive and are serialized by `AudioService`.

During capture, the firmware keeps M5Unified's two microphone slots occupied
and transfers completed blocks to a deeper application queue. A dedicated
low-priority task applies capture gain, updates the level snapshot, and writes
blocks to microSD.

Playback queues fixed-size mono 16-bit PCM blocks. A queued buffer must remain
unchanged until the speaker has consumed it.

## WAV finalization

Recording starts with a streaming WAV header. On normal stop, the writer seeks
back to repair the RIFF and data sizes, syncs the descriptor, and closes it.
An incomplete recording is discarded on capture or write failure.

The reader handles unknown RIFF chunks and does not require a fixed data
offset. Playback is limited to mono 16-bit PCM.

## Recording dates

`/RECORDER.IDX` on the microSD card keeps one immutable creation timestamp per
WAV. Newest/oldest sorting on both the Cardputer and its web panel uses this
index instead of the FAT last-write timestamp. Renames update the filename in
the index, deletes prune it, and metadata or WAV updates do not change the
stored date.

On the first start after upgrading, existing WAV files are indexed from their
current FAT timestamps. Updates are written through `/RECORDER.IDX.TMP`; an
interrupted update is recovered on the next start. Files without a valid clock
value continue to fall back to filename ordering.

## microSD

Cardputer ADV microSD uses SCK 40, MISO 39, MOSI 14, and CS 12. The firmware
uses a conservative 10 MHz clock for reliable sustained writes. These values
belong in `StorageService` and should not be duplicated elsewhere.
