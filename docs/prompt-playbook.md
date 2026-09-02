# Review-to-screenshots prompt playbook

The prompts that took this project from "review the codebase" to fixed bugs, a
polished device UI, an SDL preview harness, and rendered screenshots in the
README and site. Swap the bracketed parts and run the same sequence on another
project.

## 1. Review and fix

```text
Review our [project name] codebase, try to improve and fix the issues you
find. You can also polish the UI if needed.

Start by running the existing tests and a build to get a baseline. Fan out
parallel reviews for [backend], [firmware / core], and [web UI]. Verify every
finding against the code and the platform docs before fixing it, and tell me
which findings you rejected and why. Add regression tests for the bugs you fix.
```

What it produced here: a silent settings bug (browser forms post numbers as
strings, ArduinoJson's default operator ignores them), missing HTTP range
support for Safari audio, a 30 s UI stall on the main loop, four gateway fixes,
and two rejected "high severity" findings already handled by the platform.

The second paragraph is the part worth keeping. Without "verify before fixing"
you get a reviewer's guesses applied as patches.

## 2. Commit discipline

```text
After each set of fixes, commit and push. One commit per phase with a message
that lists what changed and why. Never add [private dirs: recovery/, backups/,
.env]; if something untracked looks private, leave it out and tell me.
```

## 3. Assess the real UI, not the web one

```text
And what about the UI and UX of the app on the [device / screen, e.g. ESP32
Cardputer 240×135 display]? Read the drawing code and the existing
screenshots, list concrete problems with the screen and line they come from,
then fix them. Watch for text that is clipped by character count instead of
pixel width, controls that exist but are never hinted, and indicators that are
never explained.
```

What it produced here: long titles overflowing the row counter, an Outbox
nobody could discover, and header indicators with no legend.

## 4. Find or build a way to see it

```text
Are there any [device] simulators or emulators we can use to see the UI
without flashing? Compare the realistic options and recommend one.
```

```text
Let's improve the UI findings, and build the [SDL / desktop] preview harness
as the follow-up. Compile the real drawing code against [graphics library's
desktop backend] with stub headers for the hardware, script every screen with
realistic data (long titles, 2× text, error states, empty states), and give it
a headless mode that dumps every screen to an image. Keep the harness out of
the device build and out of CI.
```

What it produced here: `pio run -e native-sim`, 33 scripted screens, and
within minutes three defects the photos never showed: words split mid-line at
2× scale, a chat card overflowing its panel, and status toasts that were never
drawn on the main screen.

"With realistic data" is what makes the harness find bugs instead of just
looking pretty.

## 5. Real screenshots into docs and site

```text
Use real screenshots from the emulated screen in the docs and the site. Add a
script that rebuilds the preview, renders every screen, and exports crisp
nearest-neighbour PNGs at 3× into [docs/images/screens] and
[site/public/images/screens]. Update the README and the site's screenshot
section to use them and say how to regenerate.
```

## 6. Show me

```text
Show me the screenshots too. Send a labelled contact sheet of every screen
plus the key screens at full size.
```

## 7. One-shot version

```text
Review the [project] codebase and fix what you find, with tests and a baseline
build first. Then assess the UI/UX on [the real screen] from the drawing code.
Check whether a simulator exists; if not, build a desktop preview that
compiles the real drawing code against [graphics backend] with stubbed
hardware and scripted realistic screens, including a headless image-dump
mode. Use the renders to find and fix layout defects, export them as crisp
PNGs into the docs and site, and update both. Commit and push after each
phase; never commit [private paths]. Finish by sending me a contact sheet of
every screen.
```

## Placeholders

| Placeholder | This project | Examples elsewhere |
| --- | --- | --- |
| `[device / screen]` | ESP32-S3 Cardputer ADV, 240×135 ST7789 | Flipper Zero 128×64, Watchy e-paper, a CLI TUI |
| `[graphics backend]` | M5GFX SDL backend | LovyanGFX SDL, LVGL simulator, u8g2 SDL, ncurses in a pty |
| `[build tool]` | PlatformIO `native-sim` env | CMake target, cargo feature, make target |
| `[private paths]` | `recovery/`, `backups/`, `gateway/.env` | anything with tokens, MACs, device dumps |
| `[screenshot dirs]` | `docs/images/screens`, `site/public/images/screens` | README assets, storefront images, wiki |

## Things worth saying explicitly

- **Baseline first.** "Run the tests and a build before changing anything"
  turns every later claim into a diff against a known state.
- **Verify findings.** Two of the reviewer's highest-severity items were
  already handled by the platform. Asking for rejected findings keeps them from
  becoming patches.
- **Size and CI limits.** Name the constraint: "keep the image under the
  `0x170000` slot" or "CI only builds these envs" saves a round trip.
- **Realistic scenario data.** Screens with short placeholder names look fine.
  Long titles, 2× scale, empty and error states are where defects live.
- **Private files.** Say which directories are off limits, or a reviewer will
  stage everything untracked.
