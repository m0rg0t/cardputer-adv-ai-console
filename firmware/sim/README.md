# Desktop UI preview

`native-sim` renders the real device screens on a Mac or Linux desktop. It
compiles `recorder_ui.cpp`, `screen_saver.cpp`, and `app_shared.cpp` against
the M5GFX SDL backend and scripts `RecorderApp`'s internal state for each
screen, so what you see is the same drawing code that runs on the Cardputer.
Hardware-facing members (storage, upload service, Wi-Fi) are replaced by the
small stubs in `sim/stubs/` and `src/sim/harness.cpp`.

## Requirements

- PlatformIO
- SDL2 (`brew install sdl2` on macOS)
- Python 3 with Pillow for the documentation export

## Interactive window

```sh
cd firmware
pio run -e native-sim
.pio/build/native-sim/program
```

LEFT/RIGHT switch scenarios, hold A to auto-cycle, ESC quits. The window title
names the current scenario. `--list` prints scenario names.

## Export screens for docs and the site

```sh
cd firmware
python3 sim/render_docs.py                       # docs/images/screens/*.png at 3x
python3 sim/render_docs.py --out ../site/public/images/screens
python3 sim/render_docs.py --sheet /tmp/sheet.png --only library codex
```

Frames are nearest-neighbour scaled so the 240 × 135 pixels stay crisp.

## Adding a scenario

Scenarios live in `SimAccess::scenarios()` in `src/sim/harness.cpp`. Each one
resets the fake device, sets the fields the screen reads, and is rendered with
`app.draw()`. Members that live in hardware-heavy units (sort label, settings
values, elapsed times) are mirrored in the harness; keep them in step with the
device implementation when those change.

## Limits

- Keyboard input, audio, storage, and networking are not simulated.
- The M5GFX SDL backend has no Cardputer bezel image, so frames are bare.
- The preview does not exercise `handleInput`; use it for layout and copy.
