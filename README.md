# ICT1011_P1N

# Smart Lock-Screen

A TinyScreen+ kit, a Jetson Nano, and a USB HID link that lock a laptop
when its owner walks away. The TinyScreen+ handles sensors and lock
timing; the Jetson handles face recognition and relays lock/privacy
actions to the laptop, which runs no project software at all.

## Hardware (all confirmed on hand)

**Base kit:**
- TinyScreen+ (Processor + OLED) — ASM2022
- Accelerometer TinyShield — ASD2511-R-A
- TinyShield ST BLE — ASD2116-R
- 150mAh Li-Poly Battery — ASR0003

**Loaned (Tiny Circuit Accessory Loan Form, team P1N):**
- IR Array Thermal Camera Wireling — AST1045
- Time-of-Flight (TOF) Distance Sensor Wireling — AST1024
- MicroSD TinyShield — ASD2201-R
- Wireling Adapter TinyShield — ASD2022

`Kit-Side/SmartLockScreen/SmartLockScreen.ino` implements the lock logic
on the kit. The Jetson sends the owner's face-recognition result; when
the owner is absent, the phone connection and thermal sensor decide the
privacy and lock countdowns. The TinyScreen+ has no direct USB link to
the laptop — the Jetson relays all actions.

## Architecture

```
TinyScreen+  --USB (SerialUSB)-->  Jetson Nano  --USB HID gadget-->  Laptop
(thermal+TOF+BLE)                  (webcam face                     (zero running
                                     recognition +                   code - just a
                                     HID relay)                      pre-staged image
                                                                      file copied on
                                                                      once by hand)
```

The TinyScreen+ only *announces* state changes over SerialUSB; the
Jetson daemon (`Pi-Side/face_presence_daemon.py`) reacts to them by
injecting USB HID keystrokes into the laptop through a keyboard gadget
on its micro-USB OTG port:

- `MCU:PRIVACY_MODE` → blindly open a pre-staged image on the laptop
  (`Win+R` → type path → `Enter` → `F11` to fullscreen)
- `MCU:STATE=LOCKED` → send `Win+L` (Windows' normal lock screen)
- `MCU:STATE=UNLOCKED` → close the privacy image if one is open (`Alt+F4`)

**Why blind keystrokes?** Zero running code on the laptop was a hard
requirement, and nothing else can open or display a file on a host with
nothing running on it. The accepted trade-offs: if anything steals
keyboard focus mid-sequence the typed path lands in the wrong place; the
opened window's look depends on Windows' default image viewer (and
whether it supports `F11`); and `Alt+F4` closes whatever window is
focused, which may not be the image.

## Serial protocol (TinyScreen+ ↔ Jetson)

Kit → Jetson:

| Line | Meaning |
|---|---|
| `MCU:PRIVACY_MODE` | Open the privacy image (repeated every 2s until acked) |
| `MCU:STATE=LOCKED` | Send Win+L (repeated every 5s until acked, so a restarting daemon can't miss a lock) |
| `MCU:STATE=UNLOCKED` | Close the privacy image if open |
| `MCU:WARN` | Informational warning, 2s before a lock |
| `MCU:STATUS=HEAT=..,PHONE=..,BLE=..,FACE=..,LOCKED=..,ACTIVE=..,PRIVACY=..,REMAIN_MS=..` | Once-per-second diagnostics (logging only) |
| `MCU:CMD=DELETE_REF_PHOTO` / `CAPTURE_REF_PHOTO` / `TRAIN_REF_PHOTO` | Owner photo buttons |
| `MCU:CMD=PREVIEW` / `PREVIEW_OFF` | OLED live preview on/off |

Jetson → kit:

| Line | Meaning |
|---|---|
| `PC:FACE=0/1` | Owner face status, re-sent every second (the kit treats it as stale after 5s — fail-secure) |
| `PC:ACK=PRIVACY_OPEN` | Privacy image is open — this starts the kit's lock countdown |
| `PC:ACK=LOCK_SENT` | Win+L was actually written — kit stops repeating the lock line |
| `PC:ACK=PHOTO_DELETED` / `CAPTURE_OK` / `CAPTURE_NO_FACE` / `TRAIN_OK` / `TRAIN_FAIL` | Owner photo results, shown on the OLED |
| Binary `0xA5 0x5A` + 6144 bytes | One 96x64 preview frame (8-bit BBBGGGRR) |

## Project files

```
.
├── README.md
├── TEST_PLAN.md                                    (full test plan + timing reference)
├── Kit-Side/
│   └── SmartLockScreen/
│       └── SmartLockScreen.ino                     (TinyCircuit implementation)
├── Pi-Side/                                         (Jetson daemon + setup, see its README)
└── StepByStepTesting/                              (staged bring-up/test sketches)
```

---

## `SmartLockScreen` — presence-aware auto-lock

### Physical assembly

```
TinyScreen+  (base - processor + OLED screen already built in)
    │  (32-pin TinyShield connector)
    ├── BLE TinyShield
    │  (32-pin TinyShield connector)
    └── Wireling Adapter TinyShield
            │  (5-pin Wireling cable, Port 1)
            └── Thermal Camera Wireling
            │  (5-pin Wireling cable, Port 0)
            └── TOF Distance Sensor Wireling
```

The Accelerometer TinyShield and MicroSD TinyShield are on hand but not
currently stacked/used by the sketch (see "What's left to do" below).

### Core logic

> **Timings.** See `TEST_PLAN.md` for the full timing reference and test
> checklist. The Jetson's startup grace is 10s (`SMARTLOCK_STARTUP_GRACE_SEC`,
> normally 15s).

The owner's face is the only thing that starts anything, re-checked once
per second:

1. **Owner's face recognized** → nothing runs, whatever the heat or phone
   say (a countdown already running is cancelled and the privacy image
   closed).
2. **Owner's face not recognized/missing** → what happens depends on the
   phone at that moment:
   - **Phone gone** → **no privacy screen**; full lock (`Win+L`) **5s**
     after the kit sees the owner absent
   - **Phone connected** → "privacy mode": the Jetson opens a pre-staged
     image file on the laptop (blind `Win+R` → type path → `Enter` → `F11`
     keystroke sequence) and confirms it (`PC:ACK=PRIVACY_OPEN`). Only then
     does the lock countdown start: **5s** after the image is up if heat is
     present (stranger / turned away), **10s** if there is no heat

   If the Jetson never confirms (daemon down), the privacy countdown starts
   anyway 5s after the request. Heat is read once, when the privacy
   countdown starts. A phone that drops mid-countdown shortens it to 5s.

This maps onto everyday scenarios like:

| Scenario | Heat | Phone | Owner face | Result |
|---|---|---|---|---|
| Sitting at the laptop, face recognized | any | any | ✅ | **UNLOCKED**, nothing runs |
| Walked away, phone left behind or gone | ❌ | ❌ | ❌ | No privacy screen → **LOCKED** 5s after the kit sees you gone |
| Someone in view, phone gone | ✅ | ❌ | ❌ | No privacy screen → **LOCKED** 5s after the kit sees you gone |
| Stranger in front of the camera, or you turned away | ✅ | ✅ | ❌ | Privacy screen → **LOCKED** 5s after it appears |
| Walked away, phone still connected nearby | ❌ | ✅ | ❌ | Privacy screen → **LOCKED** 10s after it appears |
| Owner returns during the privacy screen / countdown | any | any | ✅ again | Image closes (if open), countdown cancelled |

The kit prints `MCU:WARN` 2s before the lock. After locking, the kit
can't see whether the laptop was really unlocked (nothing runs there), so
it rearms itself: once the owner's face is recognized continuously for
2s, it returns to UNLOCKED and the lock logic runs again.

### Owner re-registration (3 onboard buttons)

The TinyScreen+ has 4 physical buttons; the 4th (lower-left) shows the
live camera preview (next section). The other 3 let someone become the
new recognized owner without touching a laptop — each press just tells
the Jetson what to do, since it owns the webcam, photo file and training:

| Button | Sends | Jetson does |
|---|---|---|
| Upper-left | `MCU:CMD=DELETE_REF_PHOTO` | Deletes the current reference photo and clears the live recognizer - fail-secure, no one is recognized as owner again until a new photo is **both** captured and trained |
| Upper-right | (3s "LOOK AT CAMERA" countdown on the OLED, then) `MCU:CMD=CAPTURE_REF_PHOTO` | Grabs one webcam frame as the new candidate photo, but only if a face is actually found in it |
| Lower-right | `MCU:CMD=TRAIN_REF_PHOTO` | Trains on that candidate photo and, only on success, promotes it to the real reference photo |

The Jetson replies with `PC:ACK=<result>`, shown on the OLED for a couple
of seconds. **Deliberately simple for a shared/demo kit**: there is no
presence gate or hold-to-confirm step — anyone with physical access to
the buttons can trigger any step at any time.

### Live camera preview on the TinyScreen+ (lower-left button)

Hold lower-left to show the Jetson's webcam on the OLED (96x64; green
border = owner recognized, red = not); let go to return to
LOCKED/UNLOCKED. The Jetson streams frames at ~10fps while the kit keeps
requesting them, and stops by itself ~3s after the last request. Lock
logic keeps running underneath. Holding this button while powering on
still enters the bootloader. If the picture is mirrored or upside-down,
set `SMARTLOCK_PREVIEW_FLIP` (`h`, `v` or `hv`) in the service file.

### Software setup

1. Install the **Arduino IDE** and add TinyCircuits' board support
   (see https://learn.tinycircuits.com/Help/Upload_Arduino_Programs/)
2. Install the TinyCircuits **`TinyScreen`** library via the Library Manager
   (or download from https://github.com/TinyCircuits/TinyCircuits-TinyScreen_Lib)
   - the board support package does **not** include this; without it you'll
   get `fatal error: TinyScreen.h: No such file or directory`
3. Install `Adafruit_AMG88xx` via the Library Manager (thermal sensor)
4. Install the TinyCircuits `Wireling` library (or download from
   https://github.com/TinyCircuits/TinyCircuits-Wireling-Lib)
5. Install the **Pololu `VL53L0X`** library via the Library Manager (or
   download from https://github.com/pololu/vl53l0x-arduino) - this is the
   exact library TinyCircuits' own AST1024 Time-of-Flight Wireling
   tutorial/example sketch uses
6. Download the **STBLE** library + example from
   https://github.com/TinyCircuits/TinyCircuits-TinyShield-BLE-ASD2116
   (`examples/STBLE.zip`) - not in the Library Manager
7. Open `Kit-Side/SmartLockScreen/SmartLockScreen.ino`, select the
   TinyScreen+ board under **Tools → Board**, and upload

### Known limitations

- **Can't tell phones apart** — Android gives out a new random BLE
  address every connection, so ANY BLE connection to this device counts
  as "phone nearby", including a teammate's phone.
- **RSSI distance check is bypassed** — `hci_read_rssi()` hangs the board
  right after a BLE connection forms, so any active connection counts as
  "nearby" with no actual distance filtering.
- **The privacy image must be pre-staged** — copy it to the exact path
  `SMARTLOCK_PRIVACY_IMAGE_PATH` points to (default `C:\Privacy\black.jpg`)
  before relying on privacy mode. Nothing on the laptop can do this
  automatically; a wrong path just shows a "file not found" error.

---

## Pi-Side — Jetson Nano daemon

The folder name is historical (an earlier Raspberry Pi design); the
daemon runs on a **Jetson Nano P3450**. It recognizes the owner from a
USB webcam, sends face status to the kit, and relays the kit's
lock/privacy commands to the laptop over a USB HID gadget. It waits for
both the kit's serial link and the HID gadget before starting, so it can
run unattended on boot.

See `Pi-Side/README.md` for Jetson setup, laptop preparation,
configuration and systemd instructions.

### Why the MicroSD shield is unused

The loan justification proposed the **MicroSD TinyShield (ASD2201-R)**
for on-device logging. It shares SPI chip-select pin 10 with the **BLE
TinyShield (ASD2116-R)**; stacking both unmodified would contend on the
SPI bus and break BLE, which the sketch depends on. Event logging is
currently not implemented.

### What's left to do

- Decide whether the Accelerometer TinyShield is still part of the final
  design (it was only needed for an earlier tap-override fallback).
- Note in the report why the loaned MicroSD TinyShield is unused (see
  above), since it was named in the loan justification.
- The TOF sensor is still initialized in `SmartLockScreen.ino` but its
  reading is not used by any decision logic — give it a purpose or remove
  its init code.
- Decide whether event logging (presence readings + lock/unlock events)
  is still needed and, if so, pick a replacement approach.

## Tuning notes

- `BLE_RSSI_THRESHOLD_DBM` is unused while the RSSI check is bypassed. If
  re-enabled, treat it as an approximate zone, not a precise radius — BLE
  signal strength varies with walls, body blocking and interference.
- `PRESENCE_TEMP_THRESHOLD_C` and `PRESENCE_MIN_PIXELS` need adjusting to
  room temperature and sensor distance (the AMG8833 reliably detects a
  human up to ~7m away).
