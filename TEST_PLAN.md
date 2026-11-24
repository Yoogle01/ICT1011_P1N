# Smart Lock-Screen - Full Test Plan

Covers everything built so far: the TinyScreen+ lock logic
(`Kit-Side/SmartLockScreen/SmartLockScreen.ino`), the Jetson Nano daemon
(`Pi-Side/face_presence_daemon.py`), the HID link to the laptop, owner
re-registration buttons, the TinyScreen+ live preview, the web preview and
fail-secure behaviour. **Part 1** shows how to test when not all the
hardware is together. **Part 2** is the full test list. **Part 3** is the
timing reference (settings, expected stopwatch timelines, a sheet for measured times).

> **Lock flow (current).** The owner's face is the only thing that starts
> anything. Owner seen: nothing runs, whatever the heat or phone say. Owner
> not seen and **phone gone**: no privacy screen, lock 5s after the kit sees
> the owner absent. Owner not seen and **phone connected**: privacy screen
> first, then the lock 5s after the image is up (heat present) or 10s after
> (no heat). The startup grace is 10s now (production 15s, see Phase 13).

> **Ranges are starting points.** Thermal threshold is `28.0 C` with at least
> `3` warm pixels, and the BLE check currently means "phone connected" (the
> RSSI distance check is bypassed because `hci_read_rssi()` hangs the board).
> Tune against what you actually see.

---

# Part 1 - Setup when everything is not together

The system has 5 parts: **Kit** (TinyScreen+ with thermal, TOF and BLE),
**Phone** (BLE), **Jetson** (webcam, daemon), **Laptop** (receives keystrokes),
and the cables between them. Test them in stages so a fault is always in the
newest thing you added.

| Stage | Parts present | What it proves | Tests you can run (see "Stage" column in Part 2) |
|---|---|---|---|
| **A** | Kit + your dev PC (Arduino IDE) | Sensors, BLE, OLED, buttons, lock timing | A |
| **B** | Jetson + webcam only | Camera, recognition, web preview, daemon logic | B |
| **C** | Jetson + laptop (no kit) | HID gadget, Win+L, privacy-image keystrokes | C |
| **D** | Kit + Jetson (no laptop) | Serial protocol, face signal, ACKs, OLED preview | D |
| **E** | Everything | Full end-to-end | E (and everything above) |

Rule of thumb: do A, B and C in any order, then D, then E. If a stage fails,
fix it before moving on.

Connections for the full build (Stage E):

| Cable | From | To | Shows up as |
|---|---|---|---|
| USB | TinyScreen+ | Jetson USB-A port | `/dev/ttyACM0` |
| USB | Webcam | Jetson USB-A port | `/dev/video0` or `/dev/video1` |
| Micro-USB (OTG, **data** cable) | Jetson | Laptop | a USB keyboard on the laptop, `/dev/hidg0` on the Jetson |
| Wireling port 1 / port 0 | Thermal / TOF | TinyScreen+ adapter | - |
| Bluetooth | Phone | Kit (advertises as `LockGuard`) | - |

A charge-only micro-USB cable will not work for the HID link.

## Stage A - Kit only

Needs: kit, USB cable to your dev PC, Arduino IDE, a phone with a BLE scanner
app (nRF Connect). Libraries: Wireling, TinyScreen, Adafruit_AMG88xx, VL53L0X,
STBLE (zip in `ZIP/`). Board: TinyScreen+, Serial Monitor at **115200**.

1. If anything is missing or flaky, bring the kit up in order with
   `StepByStepTesting/Kit-Side-Steps/`: Step1 OLED, Step2 Thermal, Step3 TOF,
   Step4b BLE, Step5b combined. Do not skip ahead until each one works.
2. Flash `SmartLockScreen.ino` (use **Verify** first). The full sketch **halts**
   with `THERMAL ERR` or `TOF ERR` if either sensor is not attached, so use the
   step sketches when parts are missing.
3. Open the Serial Monitor. You should see `MCU:STATUS=HEAT=..,PHONE=..,BLE=..,
   FACE=..,LOCKED=..,ACTIVE=..,PRIVACY=..,REMAIN_MS=..` once a second, plus
   `DBG:` lines.
4. **Pretend to be the Jetson** by typing lines into the Serial Monitor (line
   ending set to *Newline*, then Send):
   - `PC:FACE=1` - owner recognized. The kit treats it as stale after 5s, so
     resend at least every 5 seconds to keep it alive.
   - `PC:FACE=0` - not recognized.
   - `PC:ACK=LOCK_SENT` - stops the kit repeating `MCU:STATE=LOCKED`.
   - `PC:ACK=PHOTO_DELETED`, `CAPTURE_OK`, `CAPTURE_NO_FACE`, `TRAIN_OK`,
     `TRAIN_FAIL` - show the matching OLED message.
5. The kit prints `MCU:...` lines (state, warning, commands) instead of anything
   being done. That is the expected result at this stage.

Cannot test at A: anything that needs the real camera, keystrokes or video.

## Stage B - Jetson only (camera + recognition + web preview)

Needs: Jetson, webcam, network to your laptop/phone for the web page. No kit,
no laptop link needed. The daemon normally waits for the serial port and the HID
device before starting, so give it stand-ins:

```
sudo systemctl stop smartlock-face.service        # only one process can use the camera and port 8080
sudo apt install socat                             # one-time, needs internet
touch /tmp/fake_hidg                               # fake HID device (writes land in this file)
socat -d -d pty,raw,echo=0,link=/tmp/ttyKIT pty,raw,echo=0,link=/tmp/ttyHOST &
cat /tmp/ttyHOST                                   # leave running in terminal 2: shows what the Jetson sends to the "kit"

cd ~/Pi-Side && source venv/bin/activate          # terminal 1
export OPENBLAS_CORETYPE=ARMV8 PYTHONUNBUFFERED=1
SMARTLOCK_HIDG_PATH=/tmp/fake_hidg SMARTLOCK_SERIAL_PORT=/tmp/ttyKIT python3 face_presence_daemon.py
```

Keep `cat /tmp/ttyHOST` running. If nothing reads it, the Jetson's serial writes
time out.

You should see: `camera opened on index N`, `trained owner face recognizer`,
`camera-capture thread running`, `continuous recognition thread running`, and
the browser at `http://<jetson-ip>:8080/` shows the camera with a green or red
box. In terminal 2 you see `PC:FACE=0` or `PC:FACE=1` once a second.

Fake the kit: `echo "MCU:PRIVACY_MODE" > /tmp/ttyHOST` makes the daemon behave as
if the kit asked for the privacy screen (it writes to the fake HID file and
logs it). Likewise `MCU:STATE=LOCKED` and `MCU:STATE=UNLOCKED`. Stop with Ctrl+C,
then `sudo systemctl start smartlock-face.service` when done.

Cannot test at B: real keystrokes, the kit, OLED preview.

## Stage C - Jetson + laptop (no kit)

Needs: Jetson, laptop, data micro-USB cable from the Jetson's OTG port.

Laptop one-time prep: copy `Pi-Side/privacy_wallpaper_black.jpg` to
`C:\Privacy\black.jpg` (or set `SMARTLOCK_PRIVACY_IMAGE_PATH`). Make sure a
double-click on a `.jpg` opens a viewer where `F11` goes fullscreen.

On the Jetson:
```
sudo systemctl status smartlock-hid-gadget.service   # should be active
ls /dev/hidg0                                         # should exist
```
On the laptop, Device Manager should list a new keyboard ("SmartLock Privacy
Keyboard").

Send a lock directly (this **will lock the laptop**):
```
sudo bash -c "printf '\x08\x00\x0f\x00\x00\x00\x00\x00' > /dev/hidg0; printf '\x00\x00\x00\x00\x00\x00\x00\x00' > /dev/hidg0"
```
To test the privacy keystrokes with no kit, run the daemon as in Stage B but with
the real device: `SMARTLOCK_SERIAL_PORT=/tmp/ttyKIT python3 face_presence_daemon.py`
(without the `SMARTLOCK_HIDG_PATH` override), then `echo "MCU:PRIVACY_MODE" >
/tmp/ttyHOST` (after the 10s startup grace).

Cannot test at C: the kit, sensors.

## Stage D - Kit + Jetson (no laptop)

Needs: kit flashed with the current `SmartLockScreen.ino`, Jetson, webcam, USB
cable from the kit to the Jetson. Keep the HID stand-in so nothing reaches a
laptop:

```
touch /tmp/fake_hidg
SMARTLOCK_HIDG_PATH=/tmp/fake_hidg python3 face_presence_daemon.py     # with the service stopped
```
(`/dev/ttyACM0` is the default serial port. Check `ls /dev/ttyACM*`.) Or leave the
service running and simply leave the Jetson's micro-USB cable unplugged: the HID
write fails, is logged, and the kit keeps repeating the lock (see T56).

What you check here is the log: `sending Win+L to laptop` followed by
`Win+L HID write -> ok`, and the kit stops repeating `MCU:STATE=LOCKED`.

Cannot test at D: whether the laptop really locks, the privacy image appearing.

## Stage E - Full system

Start the services (they auto-start on boot), plug everything in, connect the
phone to `LockGuard`, and open `journalctl -u smartlock-face.service -f`.

### Pre-flight checklist (Stage E)

| # | Check | OK |
|---|---|---|
| 1 | `sudo systemctl status smartlock-hid-gadget.service smartlock-face.service` both active | ☐ |
| 2 | Laptop shows the Jetson as a keyboard | ☐ |
| 3 | `C:\Privacy\black.jpg` exists and opens in a viewer where `F11` goes fullscreen | ☐ |
| 4 | Laptop is unlocked and a harmless window (e.g. Notepad) is in front | ☐ |
| 5 | Phone connected to `LockGuard` | ☐ |
| 6 | Log shows camera opened, serial ready, HID ready, grace period finished (10s) | ☐ |
| 7 | Web preview reachable at `http://<jetson-ip>:8080/` | ☐ |
| 8 | You are recognized at 0.5-1.5m (green box) before starting the lock tests | ☐ |

---

# Part 2 - Test list

Columns: **Stage** is the lowest stage where the test can run (A, B, C, D, E).
Tick the last column as you go. Keep `journalctl -u smartlock-face.service -f`
open and watch the `[MCU] MCU:STATUS=...` lines, which show the kit's exact state.

## Phase 1 - Boot and auto-start

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T01 | A | Power the kit | OLED shows `UNLOCKED`; no `THERMAL ERR` / `TOF ERR` / `CFG ERR` | ☐ |
| T02 | D | Cold reboot the Jetson | Both services start with no commands | ☐ |
| T03 | D | Read the log after boot | `startup grace period: 10.0s` appears, then camera, serial and HID ready lines | ☐ |
| T04 | D | Watch the first ~10s after boot | No privacy image / `Win+L` is acted on even if sensors look absent | ☐ |
| T05 | B | Check the log | `camera-capture thread running at 10.0 fps` and `continuous recognition thread running` | ☐ |
| T06 | E | Reboot the Jetson, then wait | Daemon retries until the kit and HID are ready, then starts by itself | ☐ |

## Phase 2 - Thermal sensor (AMG8833)

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T07 | A | In range: 0.3-2m, facing the sensor | `HEAT=1` | ☐ |
| T08 | A | Out of range: more than 3m, or step to the side | `HEAT=0` in the log. Heat alone starts nothing; with the phone connected and the owner not seen it makes the countdown 10s instead of 5s (see T28a) | ☐ |
| T09 | A | Edge case: 2-3m, partly in view | May flicker `HEAT=1/0` in the log; heat is read once when the privacy countdown starts, so a flicker mid-countdown changes nothing | ☐ |

## Phase 3 - TOF sensor

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T10 | A | Boot the kit with the TOF wireling attached | No `TOF ERR`. The distance is read but **not used** by the lock logic | ☐ |

## Phase 4 - BLE phone proximity

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T11 | A | Connect a phone to `LockGuard` with nRF Connect | `PHONE=1,BLE=1` | ☐ |
| T12 | A | Disconnect, or walk away until the link actually drops | `PHONE=0` in the log (not just "far away", it must really disconnect). With your face seen nothing happens; with it not seen, the lock is silent (no privacy screen) 5s after the kit sees you gone | ☐ |
| T13 | A | Connect a different phone | Any BLE connection counts as "phone nearby" (known limit, phones cannot be told apart) | ☐ |

## Phase 5 - Face recognition (Jetson)

Cutoff `SMARTLOCK_CONFIDENCE_MAX` = **68**. Measured here: owner ~55-65,
stranger ~70-78. The Haar cascade needs the face at least 60x60 px.

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T14 | B | You at 0.5-1.5m, centred, good light | Green box, confidence ~55-65 | ☐ |
| T15 | B | Less than 0.3m | Face cropped or blurred, "no face" | ☐ |
| T16 | B | More than 2.5m | Face too small, not detected | ☐ |
| T17 | B | Stranger at 0.5-1.5m | Red box, confidence ~70-78, "NOT OWNER" | ☐ |
| T18 | B | A photo of the owner on a phone | Likely recognized (known limit of single-photo LBPH, not a security feature) | ☐ |
| T19 | B | Delete or rename the reference photo, restart | Warning in the log, everyone treated as "not owner" (fail-secure) | ☐ |
| T20 | D | Owner reported present | Log shows `decision: ... reported=True` immediately when you are recognized | ☐ |
| T21 | D | Owner leaves the camera, or a stranger sits there | `reported=False` after about **5s** (at most 10% of the last 5s of checks matched the owner; `SMARTLOCK_FACE_MISS_CONFIRM_SEC`); a blink or a stray lucky frame does nothing | ☐ |

## Phase 6 - Serial protocol (kit <-> Jetson)

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T22 | D | Look at the kit's `MCU:STATUS` | `FACE=1` while you are recognized, within ~1s of the log's `reported=True` | ☐ |
| T23 | D | Stop the daemon and watch the kit | After 5s with no `PC:FACE=` the kit treats the face as absent (`FACE=0`) | ☐ |
| T24 | D | Unplug and replug the kit's USB | The daemon logs `serial link dropped, reconnecting`, then recovers | ☐ |
| T25 | D | Replug and check the device name | It is still `/dev/ttyACM0`. If it became `ttyACM1`, set `SMARTLOCK_SERIAL_PORT` or fix the udev rule | ☐ |

## Phase 7 - Lock trigger paths (timed)

Setup for all rows: kit unlocked, you at 0.5-1.5m recognized (green), phone
connected unless the row says otherwise, grace period over. Use a stopwatch.
Only the owner's face starts anything. When it is not seen:
- **phone gone**: no privacy screen, lock 5s after the kit sees you gone
  (5-6.5s Jetson decision + up to 1s kit, then the 5s countdown);
- **phone connected**: privacy image first, then the lock 5s (heat present) or
  10s (no heat) after the image is up. "Image up" is ~9-10.5s after you leave
  the camera (5-6.5s Jetson decision + up to 1s kit + ~3s typing the path).

| # | Stage | Heat / Phone / Face | Do this | Start the stopwatch when | Expected timeline | Pass |
|---|---|---|---|---|---|---|
| T26 | E | any / any / IN | Sit still 10s | Immediately | No privacy, no countdown, stays `UNLOCKED`, `FACE=1` | ☐ |
| T26a | E | any / OUT / IN (phone away, owner in front) | Disconnect the phone, keep sitting in view of the camera | `PHONE=0` | **Nothing happens** for at least 30s: no privacy image, no lock | ☐ |
| T27 | E | OUT / OUT / OUT (walk away) | Leave the thermal view and the camera with the phone disconnected | Your face stops being recognized | **No privacy image.** ~6-7.5s the kit sees you gone, `MCU:WARN` 3s later, `MCU:STATE=LOCKED` 5s after the kit sees you gone. Total about **11-12.5s**, then the laptop gets `Win+L` | ☐ |
| T28 | E | IN / OUT / OUT (heat present, no phone) | Stay in the thermal view but turn away or cover your face, phone disconnected | Your face stops being recognized | Same as T27: no privacy image, lock about **11-12.5s** | ☐ |
| T28a | E | OUT / IN / OUT (no heat, phone connected) | Leave the thermal view and the camera, phone stays connected | Your face stops being recognized | Image up at ~9-10.5s, `MCU:WARN` 8s after that, lock 10s after the image is up. Total about **19-20.5s** | ☐ |
| T29 | E | IN / IN / OUT (stranger) | Have a stranger face the camera, or cover/turn away; phone stays | Your face stops being recognized | Image up ~9-10.5s, `MCU:WARN` 3s after that, lock 5s after the image is up. Total about **14-15.5s** | ☐ |
| T29a | E | Phone drops mid-countdown | Start as T28a, then disconnect the phone while the countdown runs | `PHONE=0` | The countdown shortens to 5s measured from when the image came up (locks right away if 5s have already passed) | ☐ |
| T29b | E | Phone reconnects during the silent 5s | Start as T27, reconnect the phone within the 5s | Phone reconnects | The countdown keeps going and the lock still happens (only the owner's face cancels it) | ☐ |
| T30 | E | Owner returns during the countdown | Show your face to the camera before the lock | Face recognized again | Within ~1-2s the countdown is cancelled and **no lock** happens. On the privacy path `MCU:STATE=UNLOCKED` also closes the image (`Alt+F4`) | ☐ |
| T30a | E | Re-arm after a lock | Let the kit lock (T27 or T29), return facing the camera with the phone connected, then unlock the laptop | Your face is recognized | Kit shows `UNLOCKED` after ~2-3s of recognition (log: `MCU:STATE=UNLOCKED`). Then walk away: the privacy and lock flow runs again | ☐ |
| T30b | E | A stranger never re-arms | Lock the kit, have a stranger stand in front of it | - | Kit stays `LOCKED` (only a recognized owner face re-arms) | ☐ |
| T30c | E | Thermal sensor misses you (`HEAT=0`) while you sit, face recognized | Sit in front of the camera | - | Nothing happens; `MCU:STATUS` just shows `HEAT=0` | ☐ |
| T31 | D | Any of the above without a laptop | Same actions | - | Log shows `sending Win+L to laptop` then `Win+L HID write -> ok`; the kit stops repeating the lock line | ☐ |

## Phase 8 - Privacy screen behaviour

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T32 | C/E | Trigger privacy (T29) and watch the laptop | Run dialog opens, the path is typed, the image opens and goes fullscreen with `F11` (about 3s, see Part 3) | ☐ |
| T33 | E | Be recognized again **before** the lock (during the countdown) | `MCU:STATE=UNLOCKED`, the Jetson sends `Alt+F4`, the image closes, the countdown is cancelled, no lock | ☐ |
| T34 | E | Let the lock happen, then return and unlock | Kit says `UNLOCKED` ~2-3s after your face is recognized, laptop needs your password, and the privacy image may **still be open** after you log in (the daemon does not close it before locking). Close it with `Esc` or `Alt+F4` | ☐ |
| T35 | E | Privacy while the kit is already `LOCKED` | Not triggered. `MCU:PRIVACY_MODE` only prints while unlocked (known design limit) | ☐ |
| T35a | D | Check the countdown starts on the confirmation | Trigger privacy and watch the log | `MCU:PRIVACY_MODE`, then the Jetson logs the image opening, then the kit's `MCU:STATUS` shows `ACTIVE=1` only after `PC:ACK=PRIVACY_OPEN` | ☐ |
| T36 | E | Something steals focus on the laptop during the privacy keystrokes | The blind keystrokes can land in the wrong place (known fragile by design, note what happens) | ☐ |

## Phase 9 - Owner re-registration buttons

Warning: after deleting the photo nobody is recognized, so on a live kit the
privacy flow will start. Do the whole delete -> capture -> train sequence
promptly.

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T37 | D | Press **upper-left** | OLED `DELETING`, then `DELETED`; you become unrecognized (red box) | ☐ |
| T38 | D | Press **upper-right**, face at 0.5-1.5m | `LOOK AT CAMERA` 3, 2, 1, `CAPTURING`, then `CAPTURE OK` | ☐ |
| T39 | D | Press **upper-right** with no face in range (more than 2.5m or covered) | `NO FACE` | ☐ |
| T40 | D | Press **lower-right** after a good capture | `TRAINING`, then `TRAINED OK`; recognized again within a second or two, no restart needed | ☐ |
| T41 | D | Press **lower-right** with no candidate photo | `TRAIN FAIL` | ☐ |
| T42 | D | Mash the buttons or press several at once | 300ms debounce and the capture-countdown lockout prevent double triggers | ☐ |
| T43 | D | Restart the daemon after T40 | The new photo is trained at startup from `reference_photos/owner_photo/front.jpeg` | ☐ |

## Phase 10 - TinyScreen+ live preview

Hold the **lower-left** button. Do not hold it while powering on (that enters
the bootloader).

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T44 | D | Press and **hold** lower-left | OLED shows the webcam picture (96x64) at about 10 fps; log says `TinyScreen+ live preview started` | ☐ |
| T45 | D | Check the border while previewing | Green when you are recognized, red when not | ☐ |
| T46 | D | **Release** lower-left | Back to `LOCKED`/`UNLOCKED` right away; log says `live preview stopped`; no stray frames afterwards | ☐ |
| T47 | D | Check the picture orientation and colours | Upright, not mirrored, normal colours. If not, set `SMARTLOCK_PREVIEW_FLIP` to `h`, `v` or `hv` | ☐ |
| T48 | A | Hold lower-left with no Jetson attached | `MCU:CMD=PREVIEW` printed once a second, OLED shows `NO VIDEO` after ~2s, `MCU:CMD=PREVIEW_OFF` on release | ☐ |
| T49 | D | Hold lower-left, then stop the daemon | `NO VIDEO` after ~2s; lock logic keeps running | ☐ |
| T50 | E | Hold lower-left while a countdown or warning fires | Warning/lock still happens and still shows | ☐ |
| T51 | D | Hold during the upper-right capture countdown | Preview does not start during the countdown | ☐ |

## Phase 11 - Web preview

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T52 | B | Open `http://<jetson-ip>:8080/` on the laptop and a phone at once | Both work (threaded server); face box and confidence shown | ☐ |
| T53 | B | Restart `smartlock-face.service` with a tab open | Tab breaks briefly, daemon does not crash, reload works | ☐ |
| T54 | B | Use it without internet | Works on any shared LAN (direct Ethernet or hotspot). The OLED preview needs no network at all | ☐ |

## Phase 12 - Fail-secure and robustness

| # | Stage | Test | Expected | Pass |
|---|---|---|---|---|
| T55 | E | `sudo systemctl stop smartlock-face.service` while you sit unlocked | After 5s with no `PC:FACE=` the kit treats the face as absent. Phone gone: silent lock 5s later (about 10-12s after the stop). Phone connected: privacy mode with no image, the countdown starts 5s later, then 5s (heat) or 10s (no heat), so the lock comes about 15-17s or 20-22s after the stop. The lock is delivered when the daemon is back | ☐ |
| T56 | E | Unplug the Jetson's HID micro-USB, then trigger a lock | `HID gadget write failed (ignored)` logged, no crash. Kit repeats `MCU:STATE=LOCKED` every 5s. Plug back in: the laptop locks, repeating stops | ☐ |
| T57 | E | Stop the daemon, let the kit lock, then `sudo systemctl start smartlock-face.service` | After the 10s grace, the laptop locks by itself (the kit kept repeating) | ☐ |
| T58 | B | Unplug the webcam mid-run | Error logged, no crash; after ~2s frames count as stale, not owner, fail-secure | ☐ |
| T59 | E | Power off cleanly: `sudo shutdown -h now`, then unplug after the LED goes off | Kit fails toward locked after 5s; both services auto-start on next boot | ☐ |
| T60 | E | Cut Jetson power without shutting down (only if you accept the SD-card risk) | Same as T59 on next boot | ☐ |
| T61 | A | Run the kit with no Jetson, phone present, heat present | After 5s of no `PC:FACE=` it counts as face absent: privacy mode, a 5s fallback, then the 5s countdown (about 15-17s total) | ☐ |

## Phase 13 - Before a real demo

| # | Stage | Item | Change | Pass |
|---|---|---|---|---|
| T62 | E | Kit lock delays | Nothing to restore: `SILENT_LOCK_MS` (5000), `PRIVACY_LOCK_HEAT_MS` (5000) and `PRIVACY_LOCK_NO_HEAT_MS` (10000) are the real values | ☐ |
| T63 | E | Startup grace | `SMARTLOCK_STARTUP_GRACE_SEC=15` in the service file, or edit the default (10s now) | ☐ |
| T64 | E | Re-run T26-T30 | Same timings as listed in Phase 7 (see Part 3) | ☐ |
| T65 | E | Re-check the confidence cutoff in your demo room | Owner stays green for 2 minutes of normal sitting; a stranger stays red. Adjust `SMARTLOCK_CONFIDENCE_MAX` (68 now) only after reading real values from the log | ☐ |

## Phase 14 - Final demo run-through

Do this once everything above passes. Time it.

1. Boot everything cold. Within ~30s: OLED `UNLOCKED`, green box on the web preview.
2. Sit and work for 1 minute. Expected: no lock, no privacy screen.
3. Let a stranger step in front of the camera: privacy image, then the lock.
4. Walk out of range without the phone: no privacy image, the lock about 11-12s later. Repeat with the phone: privacy image, then the lock.
5. Return: kit `UNLOCKED`. Log in on the laptop.
6. Hold lower-left to show the live camera on the OLED, release to leave it.
7. Re-register the owner with the three buttons, then confirm you are recognized.

---

# Part 3 - Timing reference

All values are the **current** settings. Only the daemon startup grace differs
from production (10s now, 15s). Allow about **±1s** on any stopwatch reading:
the kit checks its sensors once a second, so a reaction can land up to 1s late.

## 3.1 Timing settings

| Item | Where | Now | Production |
|---|---|---|---|
| Owner not seen, **phone gone**: silent lock (no privacy screen) this long after the kit sees the owner absent | Kit `SILENT_LOCK_MS` | 5s | 5s |
| Owner not seen, phone connected, **heat present**: lock this long after the privacy image is up | Kit `PRIVACY_LOCK_HEAT_MS` | 5s | 5s |
| Owner not seen, phone connected, **no heat**: lock this long after the privacy image is up | Kit `PRIVACY_LOCK_NO_HEAT_MS` | 10s | 10s |
| Countdown starts anyway if the image is never confirmed | Kit `PRIVACY_OPEN_FALLBACK_MS` | 5s after `MCU:PRIVACY_MODE` | 5s |
| Kit repeats `MCU:PRIVACY_MODE` until `PC:ACK=PRIVACY_OPEN` | Kit `PRIVACY_RESEND_INTERVAL_MS` | every 2s | every 2s |
| `MCU:WARN` before the lock | Kit `LOCK_WARNING_LEAD_MS` | 2s (3s into a 5s countdown, 8s into the 10s one) | 2s |
| Daemon startup grace | Daemon `SMARTLOCK_STARTUP_GRACE_SEC` | 10s | 15s |
| Kit sensor check | Kit `SENSOR_POLL_INTERVAL_MS` | 1s | 1s |
| `PC:FACE=` goes stale (face counts as absent) | Kit `PC_SIGNAL_TIMEOUT_MS` | 5s | 5s |
| `PC:FACE=` heartbeat | Daemon `POLL_INTERVAL_SEC` | 1s (sent at once when the value changes) | 1s |
| Lock line repeated until `PC:ACK=LOCK_SENT` | Kit `LOCK_RESEND_INTERVAL_MS` | every 5s | every 5s |
| Re-arm: owner's face seen continuously while `LOCKED` | Kit `REARM_FACE_CONFIRM_MS` | 2s | 2s |
| Owner decision check | Daemon `OWNER_DECISION_INTERVAL_SEC` | every 0.1s | every 0.1s |
| Owner reported **present** | Daemon | immediately | immediately |
| Owner reported **absent** | Daemon `SMARTLOCK_FACE_MISS_CONFIRM_SEC` | about 5s (at most 10% of the last 5s of checks matched) | 5s |
| Camera frame rate | Daemon `SMARTLOCK_CAMERA_CAPTURE_FPS` | 10 fps | 10 fps |
| Camera frame counts as stale | Daemon `FRAME_MAX_AGE_SEC` | 2s | 2s |
| Serial / HID retry | Daemon `SMARTLOCK_RETRY_DELAY_SEC` | 2s | 2s |
| Button debounce (same button) | Kit `BUTTON_DEBOUNCE_MS` | 300ms | 300ms |
| "LOOK AT CAMERA" countdown | Kit `CAPTURE_COUNTDOWN_SEC` | 3s | 3s |
| Result message on the OLED | Kit `STATUS_OVERRIDE_DURATION_MS` | 2s | 2s |
| OLED preview: release debounce | Kit `PREVIEW_RELEASE_DEBOUNCE_MS` | 100ms | 100ms |
| OLED preview: "keep streaming" request | Kit `PREVIEW_PING_INTERVAL_MS` | every 1s | every 1s |
| OLED preview: Jetson stops on its own after | Daemon `PREVIEW_LEASE_SEC` | 3s without a request | 3s |
| OLED preview: `NO VIDEO` shown after | Kit `NO_VIDEO_AFTER_MS` | 2s without a frame | 2s |
| OLED preview frame rate | Daemon `LOOP_TICK_SEC` | ~10 fps | ~10 fps |
| Privacy image opens (Win+R, type path, Enter, F11) | Daemon typing delays | about 3s (0.7s Run dialog + ~1.3s typing 19 characters + 1.0s app launch) | about 3s |

## 3.2 Expected stopwatch timelines

"t=0" is the moment you start the stopwatch. "Image up" is about 9-10.5s after
the owner's face leaves (5-6.5s Jetson decision + up to 1s kit + ~3s typing).

| Scenario | t=0 | Milestones | Production |
|---|---|---|---|
| Walk away (no heat, no phone, owner not seen) | Your face stops being recognized | **no privacy screen**; kit sees you gone ~6-7.5s, `MCU:WARN` ~9-10.5s, `MCU:STATE=LOCKED` and `Win+L` ~11-12.5s | same |
| Heat present, no phone, owner not seen | Your face stops being recognized | same as walk away: no privacy screen, lock ~11-12.5s | same |
| Stranger or turned away (heat and phone present) | Your face stops being recognized | image up ~9-10.5s, `MCU:WARN` ~12-13.5s, lock ~14-15.5s | same |
| No heat, phone connected, owner not seen | Your face stops being recognized | image up ~9-10.5s, `MCU:WARN` ~17-18.5s, lock ~19-20.5s | same |
| Phone away, owner in front | Phone disconnects | nothing happens | same |
| Owner comes back before the lock | You are recognized again | `reported=True` at once, kit `UNLOCKED` within ~1-2s, privacy image closed with `Alt+F4`, countdown cancelled | same |
| Re-arm after a lock | Owner's face recognized again | kit `UNLOCKED` ~2-3s after recognition | same |
| Daemon stopped (`systemctl stop`) | Command entered | face stale after 5s. Phone gone: silent lock ~10-12s. Phone connected: privacy mode (no image), countdown starts 5s later, lock ~15-17s (heat) or ~20-22s (no heat) | same |
| Kit locked, daemon restarted | `systemctl start` entered | grace ends at 10s, lock delivered at the next repeat (between 10s and 15s) | grace 15s, lock between 15s and 20s |
| HID cable plugged back in after a lock | Cable connected | lock delivered at the next repeat (within 5s), then repeating stops | same |
| Re-register: capture | Upper-right pressed | countdown 3, 2, 1, then `CAPTURE OK` or `NO FACE` shown for 2s | same |
| OLED preview | Lower-left held | first frame in under 1s, about 10 fps; releasing returns to the status screen at once | same |

## 3.3 Measured times (fill in while testing)

| Scenario | Expected | Measured | OK |
|---|---|---|---|
| Walk away (no phone): `MCU:WARN` | ~9-10.5s | | ☐ |
| Walk away (no phone): lock, no privacy image | ~11-12.5s | | ☐ |
| Stranger (heat and phone): privacy image visible | ~9-10.5s | | ☐ |
| Stranger (heat and phone): lock | ~14-15.5s | | ☐ |
| No heat, phone connected: lock | ~19-20.5s | | ☐ |
| Phone away, owner in front: nothing happens | 30s with no action | | ☐ |
| Owner returns: kit `UNLOCKED` | under ~2s | | ☐ |
| Daemon stopped: lock | ~10-12s (phone gone) / ~15-22s (phone connected) | | ☐ |
| Lock repeat after daemon restart | 10-15s | | ☐ |
| OLED preview: first frame | under 1s | | ☐ |

## 3.4 Rough time to run the plan (planning estimate, not measured)

| Part | Estimate |
|---|---|
| Stages A-D setup | 30-60 min (depends on how much needs wiring and flashing) |
| Phases 1-6 (boot, sensors, face, serial) | 25-35 min |
| Phase 7 (timed lock paths) | about 10 min (each run is under a minute, plus resetting between runs) |
| Phases 8-11 (privacy, buttons, OLED and web preview) | 20-25 min |
| Phase 12 (fail-secure, includes a reboot) | about 15 min |
| Phase 13 (restore production timings, re-flash, re-test) | about 15 min |
| Phase 14 (final demo run-through) | about 10 min |

Full run with everything connected: roughly 1.5-2 hours.

---

## Known limits (not bugs)

- The kit's `LOCKED`/`UNLOCKED` label is the kit's decision, not the laptop's real
  state. There is no code on the laptop, so nothing can report it back. After a
  lock the kit re-arms to `UNLOCKED` when the owner's face is recognized for ~2s,
  whether or not the laptop was actually unlocked.
- The kit never types a password, so it cannot unlock the laptop.
- Privacy mode only starts while the kit is unlocked.
- Only the owner's face starts a lock. With the phone gone the lock is silent (no
  privacy screen). With the phone connected, heat only chooses 5s vs 10s. A photo of
  the owner shown to the camera would stop a lock.
- BLE counts as "phone nearby" for any connected phone (no RSSI, no per-phone identity).
- Recognition is classical LBPH from one photo, so it is not secure against a
  photo of the owner.
- The privacy image is opened with blind keystrokes and may stay open after a
  lock.
- The TOF sensor is initialised but not used by the lock logic.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| OLED shows `THERMAL ERR` or `TOF ERR` | Wireling cable or port. Use the Step sketches to test one sensor at a time |
| `CFG ERR` | `LOCK_WARNING_LEAD_MS` is not smaller than `SILENT_LOCK_MS` or `PRIVACY_LOCK_HEAT_MS` |
| Daemon loops on `waiting for HID gadget` | `smartlock-hid-gadget.service` not running or `/dev/hidg0` missing (Stage C setup) |
| Daemon loops on `could not open /dev/ttyACM0` | Kit not plugged in, in the bootloader, or a different `ttyACM` number |
| `could not open any camera index` | Check `ls /dev/video*`; the webcam number moves (default tries `0,1`) |
| Always red box, privacy keeps firing | Confidence cutoff too tight or a bad reference photo; read the confidence in the log or on the web preview |
| Privacy never fires | Kit already `LOCKED`, or the stranger is still scoring under 68 |
| Laptop does not react to `Win+L` | Charge-only cable, or the Jetson is not listed as a keyboard on the laptop |
| OLED preview mirrored or upside down | Set `SMARTLOCK_PREVIEW_FLIP` to `h`, `v` or `hv` |
| OLED shows `NO VIDEO` | Daemon not running, or no camera frame in the last 2s |
| Log output missing under systemd | `Environment=PYTHONUNBUFFERED=1` missing from the service file |
| `Illegal instruction` on import | `Environment=OPENBLAS_CORETYPE=ARMV8` missing from the service file |

## Useful commands (Jetson)

```
journalctl -u smartlock-face.service -f              # live log
sudo systemctl restart smartlock-face.service        # after copying a new daemon
sudo systemctl status smartlock-hid-gadget.service smartlock-face.service
ls /dev/ttyACM* /dev/video* /dev/hidg0
```
