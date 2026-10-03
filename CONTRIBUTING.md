# Contributing

Open an issue before making a large behavioral or hardware change. Pull
requests should stay focused and must not assume compatibility with the
original Cardputer.

Before submitting:

```sh
platformio run -d firmware -e cardputer-adv-recorder --target clean
platformio run -d firmware -e cardputer-adv-recorder
platformio test -d firmware -e native-tests

cd gateway
uv run --extra dev pytest
```

Audio or storage changes also require the relevant checks in
`docs/hardware-test-checklist.md`.


Maintenance verification (2026-10-03): firmware pins now use espressif32 7.1.3,
M5Unified 0.2.25, M5GFX 0.2.32 and ArduinoJson 7.4.3; CI uses PlatformIO 6.2.0.
Native environments pin platform 1.2.1. M5Cardputer uses its official 1.2.0 Git
tag because that release is not in the PlatformIO registry; upstream still
labels the tag's package metadata as 1.1.1. IRremote remains at 4.7.1.
Host builds/tests do not establish actual M5Apps partition compatibility or
physical microphone, playback, SD, upload or live gateway behavior. Existing
prebuilt release/site firmware is unchanged by dependency maintenance.
