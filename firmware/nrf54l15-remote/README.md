# nRF54L15 voice remote firmware (experimental)

Firmware for a DIY voice remote that `mi-remote-bridge` can use instead of the
Xiaomi Bluetooth Voice Remote 2 Pro. It exposes:

- a **BLE HID keyboard** whose voice button sends **F5** (HID usage `0x3E`,
  macOS key code `0x60`, the key the bridge intercepts), and
- the **Android TV Voice over BLE (ATVV) v1.0** service, streaming 16 kHz
  IMA ADPCM from a PDM microphone (or a 1 kHz test tone).

> **Status:** compiles for all targets below with nRF Connect SDK v3.4.1, and
> the portable logic passes host unit tests against C ports of the bridge's
> Swift decoder and parser. The ATVV behaviour follows Google's *Voice over
> BLE* spec v1.0. It has **not yet been run on hardware or tested against a
> Mac**, and no power measurements have been made. Expect to debug the first
> bring-up.

## Hardware

| Board | Voice button | Microphone | Default audio |
|---|---|---|---|
| Seeed **XIAO nRF54L15 Sense** | USR button | on-board PDM mic (pdm20, P1.12/P1.13) | mic |
| Seeed XIAO nRF54L15 (no Sense) | USR button | none | build with `tone.conf` |
| Nordic **nRF54L15 DK** | **BUTTON 1** (P1.09) | external PDM mic: CLK→P1.12, DATA→P1.13 | test tone; add `dmic.conf` for the mic |

On the DK, BUTTON 0 is not used because P1.13 is the microphone data pin.
Change the pins in `boards/nrf54l15dk_nrf54l15_cpuapp.overlay` if you wire
the mic differently. The DK's LED0 (the XIAO's user LED) lights while audio
is streaming.

## Build and flash

Requires [nRF Connect SDK](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html)
v3.4.x (tested: v3.4.1).

```bash
# XIAO nRF54L15 Sense (PDM mic)
west build -b xiao_nrf54l15/nrf54l15/cpuapp firmware/nrf54l15-remote

# XIAO nRF54L15 without mic: 1 kHz test tone
west build -b xiao_nrf54l15/nrf54l15/cpuapp firmware/nrf54l15-remote -- -DEXTRA_CONF_FILE=tone.conf

# nRF54L15 DK: test tone (no mic needed)
west build -b nrf54l15dk/nrf54l15/cpuapp firmware/nrf54l15-remote

# nRF54L15 DK with an external PDM mic
west build -b nrf54l15dk/nrf54l15/cpuapp firmware/nrf54l15-remote -- -DEXTRA_CONF_FILE=dmic.conf

# Low-power build without logging/UART (for battery measurements and use)
west build -b xiao_nrf54l15/nrf54l15/cpuapp firmware/nrf54l15-remote -- -DEXTRA_CONF_FILE=release.conf

# Production: also enforces your own USB VID/PID (edit production.conf first)
west build -b xiao_nrf54l15/nrf54l15/cpuapp firmware/nrf54l15-remote -- \
  -DEXTRA_CONF_FILE="release.conf;production.conf"

west flash
```

Every build includes MCUboot, so `west flash` writes the bootloader and the
application together. Before shipping, generate your own signing key (see
`sysbuild.conf`); the default is MCUboot's public development key.

The DK is flashed through its on-board J-Link. The XIAO has an on-board
CMSIS-DAP probe (SAMD11) and flashes with OpenOCD; see Seeed's
[XIAO nRF54L15 wiki](https://wiki.seeedstudio.com/xiao_nrf54l15_sense_getting_started)
for the OpenOCD build that supports the nRF54L. Logs appear on the board's
USB serial port.

## Using it with the Mac bridge

1. Flash the board. It advertises as **MiRemoteVoice**.
2. Pair it from **System Settings → Bluetooth** (it shows up as a keyboard).
3. If the bridge was used with a Xiaomi remote before, delete its saved
   device so it scans again:
   `rm ~/Library/Application\ Support/mi-remote-bridge/uuid.txt`
4. Start `MiRemoteBridge.app`. It finds the remote by the ATVV service UUID
   in the advertisement.
5. Short press = MacBook mic, hold ≥ 300 ms = this remote's mic. Start with a
   test-tone build and turn on **debug recording** in the bridge's menu. You
   should get a clean 1 kHz tone in
   `~/Library/Application Support/MiRemoteBridge/Recordings/`.

The bridge's status icon follows the HID connection of any remote listed in
`HIDWatcher.knownRemotes`, which includes this firmware's development IDs.

## Protocol as implemented

Checked against Google's *Voice over BLE* spec v1.0. The interaction model
is picked from the host's GET_CAPS: **Hold-to-Talk** when the host supports
it (mi-remote-bridge always does), otherwise **On-request**.

Hold-to-Talk (the Mac bridge):

| When | Remote → Mac | Notes |
|---|---|---|
| Mac sends GET_CAPS `0A 01 00 00 03 03` | `0B 01 00 02 03 <frame> 01 00` | v1.0, ADPCM 16 kHz, HTT, frame = min(120, MTU−3), DLE hint |
| button down | HID F5 down, `04 03 02 <id>` | AUDIO_START, reason HTT, stream id 0x01–0x80 |
| streaming | `0A 02 <frame no> <pred> <step>`, then ADPCM on `AB5E0003` | AUDIO_SYNC before the first frame and after any dropped frame; high nibble first |
| Mac sends `0E <id>` (every 4 s) | – | MIC_EXTEND restarts the Audio Transfer Timeout |
| button up | `00 02`, then HID F5 up | AUDIO_STOP before key-up, as the bridge expects |

On-request (default before GET_CAPS, and hosts without HTT):

| When | Remote → host |
|---|---|
| button down | `08` (START_SEARCH), then HID F5 down |
| host MIC_OPEN `0C 00` | `04 00 02 00`, audio until MIC_CLOSE or timeout |
| repeated MIC_OPEN | `00 04` then a new AUDIO_START |

Stops and errors:

| Situation | Message |
|---|---|
| MIC_CLOSE with the stream id or `FF` | AUDIO_STOP `00 00` (other ids are ignored) |
| no MIC_EXTEND for 20 s (Audio Transfer Timeout) | AUDIO_STOP `00 08` |
| stream longer than 120 s (stuck button) | AUDIO_STOP `00 80` |
| host disables audio notifications mid-stream | AUDIO_STOP `00 10` |
| MIC_OPEN during a Hold-to-Talk stream | MIC_OPEN_ERROR `0C 0F 80`, stream continues |
| MIC_OPEN with audio notifications off | `0C 0F 03` |
| MIC_OPEN more than 60 s after the last press (Active Remote Timeout) | `0C 0F 02` |

Not implemented: Press-to-Talk, 8 kHz fallback ("dynamic bandwidth
adjustment") and the v0.4e protocol that the spec suggests for "universal"
remotes. The HID event is F5 (what the bridge expects), not the Android
`KEYCODE_ASSIST` consumer key, so a stock Android TV will not treat the
button as its Assistant key.

## Power

- Between presses the SoC sleeps in System ON; the connection uses
  peripheral latency so idle intervals are skipped.
- The microphone (PDM clock) and audio thread only run while streaming.
- While disconnected: fast advertising for 30 s, then slow (~1 s) advertising,
  then **System OFF after 5 minutes**. Pressing the voice button wakes the
  remote; it resets and reconnects, so **the waking press itself is not
  delivered** — press again once it has reconnected.
- `release.conf` removes logging and the UART console, which otherwise keep
  the UART receiver running. Measure current with a release build.
- Not done: gating the XIAO Sense microphone/IMU supply (P0.01 is held on by
  the board devicetree) and trimming radio TX power.

## Battery level

The Battery Service reports a measured level every 10 minutes and on each
connection:

- **XIAO nRF54L15:** VBAT on AIN7 through the board's 2:1 divider, which is
  switched on (P1.15) only while measuring; LiPo discharge curve.
- **nRF54L15 DK:** the SoC supply voltage (as on a remote running directly
  from 2×AAA), mapped linearly 2.0–3.0 V. On the DK this is the regulated
  supply, so the value is not meaningful there.
- Other boards: describe the ADC channel in `/zephyr,user` (see
  `src/battery.c`); without it the level stays fixed.

## Firmware updates over Bluetooth

Builds produce `build/dfu_application.zip`. Update with Nordic's
**nRF Connect Device Manager** app (iOS/Android) or any MCUmgr/SMP client:

1. The remote accepts one connection at a time, so turn Bluetooth off on the
   Mac (or unpair it) during the update.
2. Pair the phone when asked: the SMP service requires an encrypted link.
3. Load `dfu_application.zip` and start the update; the remote reboots into
   the new image. MCUboot only boots images signed with your key.

## Production IDs

`prj.conf` uses Nordic's sample USB VID/PID (`0x1915/0xEEEF`). Put your own
in `production.conf`; a build with `production.conf` fails until you do.
Add the same IDs to `HIDWatcher.knownRemotes` in the bridge
(`mi-remote-bridge/Sources/MiRemoteBridge/main.swift`) so its status icon
recognises the remote.

## Configuration

| Option | Default | |
|---|---|---|
| `CONFIG_APP_AUDIO_SOURCE_DMIC` / `_TONE` | tone (DMIC on XIAO) | audio source |
| `CONFIG_APP_MIC_GAIN_SHIFT` | 0 | extra digital gain; the bridge already adds +20 dB |
| `CONFIG_APP_ATVV_FRAME_SIZE` | 120 | ADPCM bytes per notification (also capped by MTU) |
| `CONFIG_APP_AUDIO_TRANSFER_TIMEOUT_SECONDS` | 20 | spec Audio Transfer Timeout (15–60) |
| `CONFIG_APP_STREAM_MAX_SECONDS` | 120 | hard cap per stream |
| `CONFIG_APP_ACTIVE_REMOTE_TIMEOUT_SECONDS` | 60 | spec Active Remote Timeout, 0 = off |
| `CONFIG_APP_ADV_FAST_SECONDS` | 30 | fast advertising after disconnect/press |
| `CONFIG_APP_POWEROFF` / `_IDLE_SECONDS` | y / 300 | System OFF when disconnected |
| `CONFIG_APP_BATTERY_CURVE_LIPO` / `_LINEAR` | linear (LiPo on XIAO) | voltage → % mapping |
| `CONFIG_APP_BATTERY_EMPTY_MV` / `_FULL_MV` | 2000 / 3000 | linear mapping range |
| `CONFIG_APP_BATTERY_INTERVAL_SECONDS` | 600 | measurement interval |
| `CONFIG_BT_DEVICE_NAME` | `MiRemoteVoice` | advertised name |
| `CONFIG_BT_DIS_PNP_VID/PID` | `0x1915/0xEEEF` | Nordic's sample IDs: **replace before shipping** |

## Layout

```
src/adpcm.c        IMA ADPCM encoder (mirrors ADPCMDecoder.swift)
src/atvv_proto.c   ATVV message builders/parser + framer with resync
src/remote_sm.c    hold-to-talk state machine
src/atvv_service.c ATVV GATT service
src/hid.c          HID-over-GATT keyboard (NCS bt_hids)
src/audio.c        audio thread: PDM mic or test tone -> framer
src/battery.c      ADC battery measurement -> Battery Service
src/battery_level.c voltage -> percentage curves
src/main.c         Bluetooth setup, button, events
tests/host/        host unit tests for the portable modules
```

`adpcm`, `atvv_proto`, `remote_sm` and `battery_level` have no Zephyr
dependencies.

## Tests

```bash
make -C firmware/nrf54l15-remote/tests/host
```

The tests compare the encoder sample-by-sample with a C port of the bridge's
Swift decoder, check message layouts against the Swift parser's expectations,
simulate a dropped notification to check resync, walk the state machine
through both interaction models and every stop/error path above, and check
the battery curves.

## Not done yet

- Hardware bring-up and current measurements.
- Microphone/IMU supply gating on the XIAO Sense, TX power tuning.
- Delivering the press that wakes the remote from System OFF.
- Filter-accept list / directed advertising for the bonded Mac.
- Press-to-Talk, 8 kHz fallback, v0.4e compatibility (only matter for
  Android TV hosts).
- Bluetooth SIG qualification (the SDK's qualified design numbers can be
  referenced).
