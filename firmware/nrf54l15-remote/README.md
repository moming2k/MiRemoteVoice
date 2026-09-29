# nRF54L15 voice remote firmware (experimental)

Firmware for a DIY voice remote that `mi-remote-bridge` can use instead of the
Xiaomi Bluetooth Voice Remote 2 Pro. It exposes:

- a **BLE HID keyboard** whose voice button sends **F5** (HID usage `0x3E`,
  macOS key code `0x60`, the key the bridge intercepts), and
- the **Android TV Voice over BLE (ATVV) v1.0** service, streaming 16 kHz
  IMA ADPCM from a PDM microphone (or a 1 kHz test tone).

> **Status:** compiles for all targets below with nRF Connect SDK v3.4.1, and
> the portable logic passes host unit tests against C ports of the bridge's
> Swift decoder and parser. It has **not yet been run on hardware or tested
> against a Mac**. Expect to debug the first bring-up.

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

west flash
```

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

Only the bridge's connection-status indicator is tied to the Xiaomi remote
(`HIDWatcher` matches VID `0x2717` / PID `0x32B8`). Audio and the voice key
work without changes. To make the status indicator work, point `HIDWatcher`
at this firmware's IDs (`CONFIG_BT_DIS_PNP_VID/PID`).

## Protocol as implemented

| Direction | Bytes | Meaning |
|---|---|---|
| Mac → remote | `0A 01 00 00 03 03` | GET_CAPS |
| remote → Mac | `0B 01 00 02 03 00 78 00 00` | caps: v1.0, ADPCM 16 kHz, hold-to-talk, 120-byte frames |
| button down | HID F5 down, then `08` | START_SEARCH |
| Mac → remote | `0C 00` | MIC_OPEN |
| remote → Mac | `04 03 02 <id>` | AUDIO_START (id 1–127) |
| remote → Mac | `0A 02 <seq> <pred> <step>` | AUDIO_SYNC before the first frame, and again after any dropped frame |
| remote → Mac | raw ADPCM on `AB5E0003` | audio, high nibble first |
| Mac → remote | `0E <id>` every 4 s | MIC_EXTEND (keep-alive) |
| button up | `00 02`, then HID F5 up | AUDIO_STOP *before* key-up, as the bridge expects |

- A MIC_OPEN that arrives after the button has been released (short tap)
  gets `0C 0F 01` (MIC_OPEN_ERROR).
- A stream stops on its own after 12 s without MIC_EXTEND, or after 60 s in
  total.
- The interaction-model and reason codes are logged but not acted on by the
  bridge. Check them against Google's Voice over BLE spec before using this
  remote with a real Android TV.

## Configuration

| Option | Default | |
|---|---|---|
| `CONFIG_APP_AUDIO_SOURCE_DMIC` / `_TONE` | tone (DMIC on XIAO) | audio source |
| `CONFIG_APP_MIC_GAIN_SHIFT` | 0 | extra digital gain; the bridge already adds +20 dB |
| `CONFIG_APP_ATVV_FRAME_SIZE` | 120 | ADPCM bytes per notification (also capped by MTU) |
| `CONFIG_APP_KEEPALIVE_TIMEOUT_SECONDS` | 12 | stop the stream without MIC_EXTEND |
| `CONFIG_APP_STREAM_MAX_SECONDS` | 60 | hard cap per stream |
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
src/main.c         Bluetooth setup, button, events
tests/host/        host unit tests for the portable modules
```

`adpcm`, `atvv_proto` and `remote_sm` have no Zephyr dependencies.

## Tests

```bash
make -C firmware/nrf54l15-remote/tests/host
```

The tests compare the encoder sample-by-sample with a C port of the bridge's
Swift decoder, check message layouts against the Swift parser's expectations,
simulate a dropped notification to check resync, and walk the state machine
through hold, tap, close, timeout and disconnect.

## Not done yet

- **Power:** no sleep between presses, no wake-on-button deep sleep. Battery
  life will be poor until this is added.
- Battery level is reported as a fixed value (BAS is enabled but not fed from
  the ADC).
- No OTA firmware update (MCUboot/DFU) yet.
- No filter-accept list or directed advertising for the bonded Mac.
- Production USB VID/PID and Bluetooth SIG qualification (see the project
  discussion; the SDK's qualified design numbers can be referenced).
