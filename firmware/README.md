# Cardputer firmware

This directory is the complete PlatformIO project for Cardputer ADV Agent
Console. It builds an **app-only** M5Apps image; it is not a merged flash image.

```sh
platformio run -d firmware -e cardputer-adv-recorder
platformio test -d firmware -e native-tests
```

The build output is
`firmware/.pio/build/cardputer-adv-recorder/firmware.bin`. Install it from
M5Apps, or download the ready-to-use image from `release/firmware` or GitHub
Releases. Never flash this image at address `0x0000`.

Copy and edit the examples in `sdcard/` to configure Wi-Fi and the gateway.
Real credentials and device-generated files are intentionally ignored by Git.
