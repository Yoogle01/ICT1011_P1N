# Pi-Side — Jetson Nano face-presence + laptop-lock daemon

(The folder name is historical, from an earlier Raspberry Pi design —
this runs on a **Jetson Nano P3450**.)

The daemon reads an external USB webcam, checks the largest face against
a single trained owner reference photo, and sends `PC:FACE=0/1` to the
TinyScreen+ over USB serial (a plain USB-A host port, separate from the
micro-USB OTG port used for the laptop link below).

It also watches that serial link for the kit's `MCU:` lines and reacts by
injecting USB HID keystrokes into the laptop through a keyboard gadget on
the Jetson's micro-USB OTG port — see the main `../README.md` for the
full protocol and the blind-keystroke trade-offs. See `../Jetson-Setup/`
(in the `Team Project` folder) for the one-time Linux gadget setup and
the full step-by-step guide (`Jetson_Nano_Setup_Guide.docx`).

The laptop itself runs **zero project code** — see "Laptop setup" below.

## How it works

Three threads cooperate:

1. **Camera capture** — the only code that reads the webcam (a
   `cv2.VideoCapture` is not safe to read from multiple threads), at
   `SMARTLOCK_CAMERA_CAPTURE_FPS`, into a shared latest-frame buffer.
2. **Recognition** — runs face detection + LBPH recognition on every new
   frame, continuously.
3. **Main loop** — turns recognition results into one owner/not-owner
   decision every `SMARTLOCK_OWNER_DECISION_INTERVAL_SEC`, sends
   `PC:FACE=` every second, and handles the kit's commands.

The owner decision is debounced so one bad frame (a blink, motion blur)
can't trigger privacy mode: "absent" needs at most 10% of the last
`SMARTLOCK_FACE_MISS_CONFIRM_SEC` (default 5s) of checks to have matched
(a rolling window, so a stranger's occasional lucky frame can't restart
the timer); "present" takes effect as soon as half of the last second
matched, which also closes the privacy image right away.

A **startup grace period** (`SMARTLOCK_STARTUP_GRACE_SEC`) ignores
privacy/lock requests right after boot — a Jetson reboot usually also
power-cycles the kit, and both can momentarily report "not present"
while settling. Ignored locks aren't lost: the kit repeats them until
acknowledged.

## Files

```
Pi-Side/
├── README.md                              (this file)
├── face_presence_daemon.py                (main script - run this)
├── requirements.txt
├── smartlock-face.service                 (systemd unit, see below)
├── haarcascade_frontalface_default.xml    (OpenCV face detector, bundled)
├── privacy_wallpaper_black.jpg            (see "Laptop setup" below)
└── reference_photos/
    └── owner_photo/front.jpeg          (the one trained reference photo)
```

## Setup on the Jetson

1. Flash Ubuntu/L4T, set up the J48 power jumper, and set up the USB HID
   keyboard gadget first - see `../Jetson-Setup/Jetson_Nano_Setup_Guide.docx`
   for all of that (this README only covers the Python daemon itself).
2. Copy this whole `Pi-Side/` folder onto the Jetson, e.g.:
   ```bash
   scp -r Pi-Side <your-username>@<jetson-ip>:/home/<your-username>/Pi-Side
   ```
3. Confirm the single owner reference photo is present at
   `reference_photos/owner_photo/front.jpeg` (already included).
4. Plug the TinyScreen+ into a plain USB-A port on the Jetson (**not**
   the micro-USB OTG port used by the HID gadget), and the external USB
   webcam into another USB-A port.
5. On the Jetson:
   ```bash
   cd ~/Pi-Side
   python3 -m venv venv
   source venv/bin/activate
   pip install -r requirements.txt
   python3 face_presence_daemon.py
   ```
   `opencv-contrib-python` can take a long time to install on a Jetson if
   no prebuilt wheel is available for your exact Python/Ubuntu combo (may
   compile from source). If `pip install` is too slow/fails, try
   `sudo apt install python3-opencv` instead and skip that one line from
   `requirements.txt`.
6. Confirm it's working: it blocks at startup printing
   `[daemon] waiting for HID gadget at /dev/hidg0...` and/or
   `[daemon] could not open /dev/ttyACM0...` until both the HID gadget and
   the TinyScreen+'s serial link are ready, then prints
   `[daemon] TinyScreen+ serial link ready on ...`, `[daemon] HID gadget
   ready at ...`, and starts polling - plus a log line whenever
   owner-recognized status flips.

## Laptop setup (one-time, no code)

The laptop runs **no script, no file, nothing** - its only involvement is
receiving keystrokes from the Jetson's HID gadget. One manual, one-time
step is required on it before privacy mode will work correctly:

1. Copy `privacy_wallpaper_black.jpg` (already included in this folder)
   onto the laptop - e.g. via a USB drive.
2. Place it at the **exact path** `SMARTLOCK_PRIVACY_IMAGE_PATH` points
   to (default: `C:\Privacy\black.jpg`) - the filename/location you
   actually use doesn't matter as long as it matches that setting exactly.

From then on, when privacy mode triggers, the daemon blindly types that
path into the laptop's Run dialog (`Win+R`) and presses Enter, which
launches whatever app Windows has set as the default for that file type,
then sends `F11` to fullscreen it. If the path doesn't match a real
file, Windows shows a "file not found" error window instead — so getting
this exact path right matters.

`Win+L` (sent on an actual lock) is unaffected by any of this - it's
always just Windows' own built-in lock-screen shortcut.

## Running on boot (systemd)

Once the venv works manually (step 5 above), install the bundled unit so
it starts automatically on every boot, no monitor/keyboard needed:

```bash
sudo cp smartlock-face.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now smartlock-face.service
sudo systemctl status smartlock-face.service
journalctl -u smartlock-face.service -f   # live logs
```

(Edit the `User=`/paths inside `smartlock-face.service` first to match
your actual Linux username and folder location, if different from the
placeholders.) This service depends on `smartlock-hid-gadget.service`
(see `../Jetson-Setup/`) being active first - the daemon won't make
progress past its startup retry loop until that gadget exists.

## Configuration

All overridable via environment variables (e.g. in the systemd unit's
`[Service]` section, or `export` before running manually) - defaults
shown:

| Variable | Default | Notes |
|---|---|---|
| `SMARTLOCK_SERIAL_PORT` | `/dev/ttyACM0` | TinyScreen+'s USB CDC serial port - check `ls /dev/ttyACM*` if this doesn't match |
| `SMARTLOCK_BAUD_RATE` | `115200` | Cosmetic only - `SerialUSB` is native USB CDC, not a real UART |
| `SMARTLOCK_CAMERA_INDEX` | `0,1` | Comma-separated webcam indexes tried in order (`/dev/video0`, then `/dev/video1`) - the webcam's number can move after a replug or reboot |
| `SMARTLOCK_REF_PHOTO` | `./reference_photos/owner_photo/front.jpeg` | Single reference photo path, not a folder |
| `SMARTLOCK_HIDG_PATH` | `/dev/hidg0` | This Jetson's own HID gadget device node |
| `SMARTLOCK_RETRY_DELAY_SEC` | `2.0` | Retry delay for both the TinyScreen link and the HID gadget readiness check |
| `SMARTLOCK_PRIVACY_IMAGE_PATH` | `C:\Privacy\black.jpg` | Exact Windows path blindly typed into the laptop's Run dialog on privacy mode - must match a real file there (see "Laptop setup" above) |
| `SMARTLOCK_WEB_PREVIEW_PORT` | `8080` | Port for the built-in web-based live preview (see "Live debug preview" below) |
| `SMARTLOCK_CAMERA_CAPTURE_FPS` | `10.0` | How fast the dedicated camera-capture background thread reads frames (see "Live debug preview" below) |
| `SMARTLOCK_PREVIEW_FLIP` | `none` | Orientation of the picture sent to the TinyScreen+ OLED live preview: `h` (mirror), `v` (upside down) or `hv` (rotate 180) |
| `SMARTLOCK_CONFIDENCE_MAX` | `68.0` | Face-match cutoff (LBPH distance, lower = better match). Measured here: owner ~55-65, stranger ~70-78 |
| `SMARTLOCK_OWNER_DECISION_INTERVAL_SEC` | `0.1` | How often the owner/not-owner decision is made |
| `SMARTLOCK_FACE_MISS_CONFIRM_SEC` | `5.0` | Rolling window used to confirm the owner is absent (see "How it works") |
| `SMARTLOCK_STARTUP_GRACE_SEC` | `10.0` | Privacy/lock requests are logged but ignored for this long after startup (production: 15, see `../TEST_PLAN.md`) |
| `SMARTLOCK_RUN_DIALOG_OPEN_WAIT_SEC` | `0.7` | Wait after Win+R before typing the path. Raise toward 1.2 if typing silently stops landing (0.5 was too tight on a real laptop) |
| `SMARTLOCK_TYPE_CHAR_DELAY_SEC` | `0.02` | Delay between typed characters of the privacy image path |
| `SMARTLOCK_APP_LAUNCH_WAIT_SEC` | `1.0` | Wait after Enter before sending F11. Raise if F11 fires before the viewer opens |

## Tuning recognition precision

The LBPH face recognizer compares a live webcam frame against the
single trained `SMARTLOCK_REF_PHOTO` and returns a distance score
(lower = more similar) - `RECOGNITION_CONFIDENCE_MAX` (in
`face_presence_daemon.py`, env var `SMARTLOCK_CONFIDENCE_MAX`, default 68) is the cutoff
below which a match counts as "the owner." A few ways to improve
accuracy, roughly in order of effort:

1. **Reference photo quality matters most** - well-lit, front-facing,
   neutral expression, similar distance/angle to how you'll actually
   sit day-to-day. A reference photo taken in very different lighting
   than normal use is a common cause of inconsistent recognition.
2. **Tune `RECOGNITION_CONFIDENCE_MAX` against real numbers** - the
   daemon logs real confidence values every time its reported owner
   status changes (`[daemon] face=... confidence=... reported=...`).
   Watch several of these during normal use vs. a stranger's face, then
   set the threshold between those two observed clusters.

## Live debug preview

Always-on and best-effort — not required for lock/unlock operation, and
a failure (e.g. port already in use) is logged once and ignored. The
webcam feed, with the detected face box (green=recognized, red=not)
drawn on it, is served as a plain MJPEG stream over HTTP. Open
`http://<jetson-ip>:8080/` in a browser on any device on the same
network — **including the laptop**, since visiting a URL in an
already-installed browser isn't "running project code" there.

The webcam supports up to 30fps (checked via `v4l2-ctl
--list-formats-ext`), so `SMARTLOCK_CAMERA_CAPTURE_FPS` is a software
pacing choice — raise it for smoother video at the cost of Jetson
CPU/USB bandwidth shared with recognition, HID and BLE.

## Fail-secure behavior

If the daemon crashes, the serial link drops, the reference photo is
missing, or the camera can't be opened, the TinyScreen+ simply stops
receiving fresh `PC:FACE=` updates and - per its own
`PC_SIGNAL_TIMEOUT_MS` logic - treats the owner as **absent** once that
goes stale. It never defaults to "always present".
