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
