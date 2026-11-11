# Over-the-air updates for the Waveshare 4C (ESP32-P4)

The 4C port lives in the fork **sccstest/FlightScnr**, and the 4C firmware pulls its
updates from that fork's GitHub releases (`FLIGHTSCNR_OTA_REPO` in
`include/hardware/boards/waveshare_p4_4c.h`). Upstream releases have no P4 image.

## How the device finds an update

- On every boot, and again after 24 hours of uptime, the device asks
  `https://api.github.com/repos/sccstest/FlightScnr/releases/latest` for the newest
  release. Drafts and pre-releases are ignored.
- It looks for the asset named after its own board:

  | Board (PlatformIO env) | ESP32-P4 silicon | OTA asset |
  | --- | --- | --- |
  | `waveshare-p4-4c` | rev 3.x (current boards) | `FlightScnr-waveshare-p4-4c-app.bin` |
  | `waveshare-p4-4c-v1` | pre-rev3 (v1.x) | `FlightScnr-waveshare-p4-4c-v1-app.bin` |

  The two images are not interchangeable, so each board only ever downloads its own.
- If the release tag is newer than the running version, the **About** screen shows
  "Update available". Versions compare as dotted numbers (`2026.12.5.1` >
  `2026.12.4.3`); a local build reports `dev`, which is older than any release.
- The image must fit the 12.5 MB app slot (`default_32MB.csv`). The device checks
  this before and after the download, and the release workflow refuses to publish
  a P4 image that is too big.

## Installing an update on the device

1. Open the device's web settings page (`http://flightscnr-xxxx.local/` or its IP).
2. **Firmware update** card → **Check GitHub**, then **Install latest**.
3. When it reports success, **press the reset button** (or unplug and replug).

Wi-Fi and settings are kept. If the download fails part-way, the running firmware
is untouched; try again.

## First install (USB, before the gift)

1. Find the chip revision: `esptool --chip esp32p4 chip-id` and look for
   "Chip is ESP32-P4 (revision v3.x)" (rev 3 → `waveshare-p4-4c`, older →
   `waveshare-p4-4c-v1`).
2. Flash the matching **merged** image from a release at 0x0:

   ```bash
   esptool --chip esp32p4 write_flash 0x0 FlightScnr-waveshare-p4-4c-merged.bin
   ```

   This erases Wi-Fi and settings. Flash a release image rather than a local build
   so the device starts on a real version number.

## Cutting a release on the fork

One-time setup on github.com/sccstest/FlightScnr:

- **Actions** tab: GitHub disables workflows on new forks. Enable them.
- **Settings → Actions → General → Workflow permissions**: the Release workflow
  needs to create releases (`contents: write`). If releases fail with a 403,
  choose "Read and write permissions".
- The WebFlasher (GitHub Pages) step only runs on upstream. To run it on the fork
  too, add the repository variable `DEPLOY_WEBFLASHER` = `true` (not needed for the 4C).

Then, with the fix committed on the branch that holds the 4C port:

```bash
git tag 2026.12.20.1
git push origin 2026.12.20.1
```

Tags must be four dotted numbers (`YYYY.M.D.N`) and newer than the version on the
device. The **Release** workflow builds the two S3 boards and both 4C variants, and
publishes one release with all app and merged images plus `SHA256SUMS.txt`.

You can also start it from **Actions → Release → Run workflow** and pick the branch;
it then makes a tag from today's date (`YYYY.M.D.N`). A push to `master`/`main` also
releases automatically unless the commit message contains `[skip release]`.

Within a day (or at the next power-on) the 4C shows "Update available"; install it
from the settings page as above.

## Testing before you publish

Every published release reaches the device within a day, so test first:

```powershell
$env:PLATFORMIO_CACHE_DIR='C:\pioc'
pio run -e waveshare-p4-4c
```

Then upload `.pio\build\waveshare-p4-4c\firmware.bin` from the settings page
(**Firmware update** → upload). Use the app image, never the merged one.

## Undoing a bad release

The device only installs a version **newer** than the one it runs, so it will not
go back to an older tag. Fix forward: commit the fix (or a revert) and release it
under a new, higher tag. Deleting the bad release only stops devices that have
not installed it yet.
