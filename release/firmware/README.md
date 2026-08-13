# Ready-to-install firmware

`Cardputer-ADV-Agent-Console-2.8.1-M5Apps.bin` is an app-only image for the
M5Stack Cardputer ADV and M5Apps. Verify it with `SHA256SUMS.txt`, copy it to a
FAT32 microSD card, and install it through **M5Apps → Installer → SD**.

Do not write this image at flash address `0x0000`: doing so would overwrite the
M5Apps launcher. Tagged releases are also published through GitHub Releases.
