"""
Smart Lock-Screen - Jetson Nano face-presence + laptop-lock daemon.

Runs headless on a Jetson Nano P3450. Three threads cooperate:
  1. A camera-capture thread - the only code that reads the webcam -
     fills a shared latest-frame buffer at CAMERA_CAPTURE_FPS.
  2. A recognition thread checks the largest face in every new frame
     against the single trained owner photo (REFERENCE_PHOTO).
  3. The main loop debounces those results into one owner/not-owner
     decision, sends "PC:FACE=0/1" to the TinyScreen+ over USB serial,
     and handles the kit's MCU: lines.

Kit lines that trigger a USB HID action on the laptop (via this Jetson's
keyboard gadget on its micro-USB OTG port):
    "MCU:PRIVACY_MODE"   -> open PRIVACY_IMAGE_PATH (Win+R -> type path
                            -> Enter -> F11), ack "PC:ACK=PRIVACY_OPEN"
    "MCU:STATE=LOCKED"   -> Win+L, ack "PC:ACK=LOCK_SENT"
    "MCU:STATE=UNLOCKED" -> Alt+F4 if the privacy image is still open

Owner photo commands (MCU:CMD=DELETE/CAPTURE/TRAIN_REF_PHOTO) and the
OLED live preview (MCU:CMD=PREVIEW/PREVIEW_OFF) are handled here too.
See ../README.md for the full protocol and the blind-keystroke
trade-offs, and README.md in this folder for setup, configuration
(all SMARTLOCK_* environment variables) and tuning.

Fail-secure: if this script stops, the serial link drops, the reference
photo is missing, or the camera cannot be opened, the kit stops getting
fresh PC:FACE= updates and treats the owner as absent, never as present.
"""

import collections
import os
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import cv2
import numpy as np
import serial
from serial import SerialException

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

SERIAL_PORT = os.environ.get("SMARTLOCK_SERIAL_PORT", "/dev/ttyACM0")
BAUD_RATE = int(os.environ.get("SMARTLOCK_BAUD_RATE", "115200"))
# PC:FACE= heartbeat interval - the kit treats the value as stale after 5s.
POLL_INTERVAL_SEC = 1.0
CAMERA_INDEXES = [
    int(i) for i in os.environ.get("SMARTLOCK_CAMERA_INDEX", "0,1").split(",")
    if i.strip()
]
# How often the owner/not-owner decision is made from the recognition
# passes collected since the previous decision (majority must match).
OWNER_DECISION_INTERVAL_SEC = float(
    os.environ.get("SMARTLOCK_OWNER_DECISION_INTERVAL_SEC", "0.1")
)
# Port for the web-based live preview (see README.md).
WEB_PREVIEW_PORT = int(os.environ.get("SMARTLOCK_WEB_PREVIEW_PORT", "8080"))
# Camera-capture thread frame rate. A software pacing choice, not a
# hardware ceiling - raise for smoother preview at more CPU/USB cost.
CAMERA_CAPTURE_FPS = float(os.environ.get("SMARTLOCK_CAMERA_CAPTURE_FPS", "10.0"))
# Main loop tick; also the OLED preview frame rate (~10fps).
LOOP_TICK_SEC = 0.1
# Frames keep streaming this long after the kit's last MCU:CMD=PREVIEW,
# so streaming stops by itself if the kit resets or the cable is pulled.
PREVIEW_LEASE_SEC = 3.0
PREVIEW_W, PREVIEW_H = 96, 64   # the TinyScreen+ OLED's resolution
# "none", "h" (mirror), "v" (upside down) or "hv" (rotate 180) - applied
# to the picture sent to the OLED preview.
PREVIEW_FLIP = os.environ.get("SMARTLOCK_PREVIEW_FLIP", "none").lower()
# Owner-absence debounce: absent when at most FACE_ABSENT_MAX_RATIO of
# the last FACE_MISS_CONFIRM_SEC of checks matched (rolling window, so a
# stranger's occasional lucky frame can't restart the timer); present
# again as soon as half of the last FACE_RECOVER_WINDOW_SEC matched.
FACE_MISS_CONFIRM_SEC = float(
    os.environ.get("SMARTLOCK_FACE_MISS_CONFIRM_SEC", "5.0")
)
FACE_ABSENT_MAX_RATIO = 0.1
FACE_RECOVER_WINDOW_SEC = 1.0
RETRY_DELAY_SEC = float(os.environ.get("SMARTLOCK_RETRY_DELAY_SEC", "2.0"))
# Startup grace: ignore privacy/lock requests this long after boot while
# the kit and camera settle (production: 15.0 - see ../TEST_PLAN.md).
STARTUP_GRACE_SEC = float(os.environ.get("SMARTLOCK_STARTUP_GRACE_SEC", "10.0"))

REFERENCE_PHOTO = os.environ.get(
    "SMARTLOCK_REF_PHOTO",
    os.path.join(_SCRIPT_DIR, "reference_photos", "owner_photo", "front.jpeg"),
)
# Where a freshly captured owner-candidate photo lands before training
# either promotes it to REFERENCE_PHOTO or discards it.
CANDIDATE_PHOTO_PATH = os.path.join(
    _SCRIPT_DIR, "reference_photos", "_pending_owner_capture.jpg"
)
FACE_SIZE = (200, 200)              # LBPH needs a consistent crop size
# LBPH "confidence" is a distance - lower = more similar. Measured here:
# owner ~55-65, stranger ~70-78, so the cutoff sits between the clusters.
RECOGNITION_CONFIDENCE_MAX = float(os.environ.get("SMARTLOCK_CONFIDENCE_MAX", "68.0"))
OWNER_LABEL = 0                 # only one identity is ever trained (the owner)

# ---------------------------------------------------------------------------
# USB HID gadget (the Jetson's device-mode link to the laptop, over its
# micro-USB OTG port - see README.md for the one-time setup). Raw 8-byte
# boot-keyboard reports: [modifier, 0, keycode, 0, 0, 0, 0, 0].
# ---------------------------------------------------------------------------
HIDG_PATH = os.environ.get("SMARTLOCK_HIDG_PATH", "/dev/hidg0")

# The exact Windows path typed into the Run dialog to open the privacy
# image - must match wherever the image was copied on the laptop.
PRIVACY_IMAGE_PATH = os.environ.get("SMARTLOCK_PRIVACY_IMAGE_PATH", r"C:\Privacy\black.jpg")

_MOD_LEFT_SHIFT = 0x02
_MOD_LEFT_ALT = 0x04
_MOD_LEFT_GUI = 0x08
_KEY_R = 0x15        # 'r' - for Win+R (open Run dialog)
_KEY_D = 0x07        # 'd' - unused, kept as a reference/fallback
_KEY_L = 0x0F        # 'l' - for Win+L (the real OS lock)
_KEY_ENTER = 0x28
_KEY_UP = 0x52       # Up arrow - unused, kept as a reference/fallback
_KEY_F11 = 0x44      # F11 - fullscreen in most image viewers/browsers
_KEY_F4 = 0x3D
_KEY_RELEASE_DELAY_SEC = 0.05
# Delay between typed characters of the privacy image path.
TYPE_CHAR_DELAY_SEC = float(os.environ.get("SMARTLOCK_TYPE_CHAR_DELAY_SEC", "0.02"))
# Wait after Win+R for the Run dialog to gain focus before typing. 0.5s
# was too tight on a real laptop; raise toward 1.2 if typing stops landing.
RUN_DIALOG_OPEN_WAIT_SEC = float(os.environ.get("SMARTLOCK_RUN_DIALOG_OPEN_WAIT_SEC", "0.7"))
# Wait after Enter before sending F11 so the viewer has time to open.
APP_LAUNCH_WAIT_SEC = float(os.environ.get("SMARTLOCK_APP_LAUNCH_WAIT_SEC", "1.0"))

_CASCADE_PATH = os.path.join(_SCRIPT_DIR, "haarcascade_frontalface_default.xml")
_FACE_CASCADE = cv2.CascadeClassifier(_CASCADE_PATH)
if _FACE_CASCADE.empty():
    sys.exit(
        f"[daemon] could not load face cascade from {_CASCADE_PATH} - "
        "re-download it from "
        "https://raw.githubusercontent.com/opencv/opencv/master/data/"
        "haarcascades/haarcascade_frontalface_default.xml"
    )


def _detect_largest_face(gray_frame):
    """Return the largest face box (x, y, w, h), or None. Assumes the
    biggest face in frame is whoever's actually at the laptop.
    """
    faces = _FACE_CASCADE.detectMultiScale(
        gray_frame, scaleFactor=1.2, minNeighbors=5, minSize=(60, 60)
    )
    if len(faces) == 0:
        return None
    return max(faces, key=lambda f: f[2] * f[3])


def load_owner_recognizer(photo_path=None):
    """Train an LBPH face recognizer from the single photo at photo_path
    (defaults to REFERENCE_PHOTO). Returns None if the photo is missing
    or unusable - fail-secure, never "owner always present".
    """
    if photo_path is None:
        photo_path = REFERENCE_PHOTO

    if not os.path.isfile(photo_path):
        print(f"[daemon] no reference photo at {photo_path}")
        return None

    img = cv2.imread(photo_path)
    if img is None:
        print(f"[daemon] could not read reference photo {photo_path}")
        return None

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    face = _detect_largest_face(gray)
    if face is None:
        print(f"[daemon] no face found in reference photo {photo_path}")
        return None

    x, y, w, h = face
    face_crop = cv2.resize(gray[y:y + h, x:x + w], FACE_SIZE)

    recognizer = cv2.face.LBPHFaceRecognizer_create()
    recognizer.train([face_crop], np.array([OWNER_LABEL]))
    print(f"[daemon] trained owner face recognizer on {photo_path}")
    return recognizer


def detect_owner_presence(recognizer, frame):
    """Returns (any_face_detected, owner_recognized, confidence, face_box).

    confidence is None if there is no face or no recognizer. face_box is
    only used by the live preview overlay.
    """
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    face = _detect_largest_face(gray)
    if face is None:
        return False, False, None, None
    if recognizer is None:
        return True, False, None, face

    x, y, w, h = face
    face_crop = cv2.resize(gray[y:y + h, x:x + w], FACE_SIZE)
    label, confidence = recognizer.predict(face_crop)
    owner_recognized = (label == OWNER_LABEL) and (confidence <= RECOGNITION_CONFIDENCE_MAX)
    return True, owner_recognized, confidence, face


def _annotate_frame(frame, any_face, owner_recognized, confidence, face_box):
    """Draw the face box (green=recognized, red=not) and a status line
    onto a copy of frame, for the web preview stream.
    """
    annotated = frame.copy()
    if face_box is not None:
        x, y, w, h = face_box
        box_color = (0, 255, 0) if owner_recognized else (0, 0, 255)  # BGR
        cv2.rectangle(annotated, (x, y), (x + w, y + h), box_color, 2)

    if not any_face:
        status_text = "no face detected"
        text_color = (0, 0, 255)
    elif owner_recognized:
        status_text = f"OWNER (confidence={confidence:.1f})"
        text_color = (0, 255, 0)
    else:
        conf_str = f"{confidence:.1f}" if confidence is not None else "--"
        status_text = f"NOT OWNER (confidence={conf_str})"
        text_color = (0, 0, 255)

    cv2.putText(
        annotated, status_text, (10, 25),
        cv2.FONT_HERSHEY_SIMPLEX, 0.7, text_color, 2,
    )
    return annotated


def build_preview_packet(frame, owner_recognized):
    """Turn a webcam frame into one OLED preview packet: 2 magic bytes
    (0xA5 0x5A) + 96*64 pixel bytes. The frame is centre-cropped to 3:2,
    shrunk to 96x64, given a green/red border, and packed into the OLED's
    native 8-bit BBBGGGRR colour format.
    """
    h, w = frame.shape[:2]
    crop_h = int(w * PREVIEW_H / PREVIEW_W)
    if crop_h <= h:
        y0 = (h - crop_h) // 2
        crop = frame[y0:y0 + crop_h, :]
    else:
        crop_w = int(h * PREVIEW_W / PREVIEW_H)
        x0 = (w - crop_w) // 2
        crop = frame[:, x0:x0 + crop_w]
    small = cv2.resize(crop, (PREVIEW_W, PREVIEW_H), interpolation=cv2.INTER_AREA)
    if "h" in PREVIEW_FLIP:
        small = small[:, ::-1]
    if "v" in PREVIEW_FLIP:
        small = small[::-1, :]
    small = np.ascontiguousarray(small)

    border = (0, 255, 0) if owner_recognized else (0, 0, 255)  # BGR
    cv2.rectangle(small, (0, 0), (PREVIEW_W - 1, PREVIEW_H - 1), border, 2)

    b, g, r = small[:, :, 0], small[:, :, 1], small[:, :, 2]
    packed = (b & 0xE0) | ((g >> 5) << 2) | (r >> 6)
    return b"\xa5\x5a" + packed.astype(np.uint8).tobytes()


# ---------------------------------------------------------------------------
# Shared camera-capture state. A cv2.VideoCapture is not safe to read()
# from multiple threads, so _camera_capture_loop() is the only code that
# reads the camera; everything else reads the shared buffer below.
# ---------------------------------------------------------------------------
_latest_frame = None
_latest_frame_id = 0  # increments per new frame so consumers can skip repeats
_latest_frame_time = 0.0  # time.monotonic() of the last successful read
_latest_frame_lock = threading.Lock()
FRAME_MAX_AGE_SEC = 2.0  # older than this = camera stalled/unplugged, don't trust it

_last_detection_lock = threading.Lock()
_last_detection = {
    "any_face": False, "raw_recognized": False,
    "confidence": None, "face_box": None,
}

# raw_recognized result of every recognition pass since main() last took
# the window for an owner decision - see _recognition_loop()/main().
_window_samples = []
_window_samples_lock = threading.Lock()


def _get_latest_frame():
    """Returns (True, frame) if a frame was read within FRAME_MAX_AGE_SEC,
    else (False, None) - a stale frame is never mistaken for a current one.
    """
    with _latest_frame_lock:
        if _latest_frame is None:
            return False, None
        if time.monotonic() - _latest_frame_time > FRAME_MAX_AGE_SEC:
            return False, None
        return True, _latest_frame


def _get_latest_frame_with_id():
    with _latest_frame_lock:
        return _latest_frame, _latest_frame_id


def _set_last_detection(any_face, raw_recognized, confidence, face_box):
    """Store the latest recognition result for the preview overlays."""
    with _last_detection_lock:
        _last_detection["any_face"] = any_face
        _last_detection["raw_recognized"] = raw_recognized
        _last_detection["confidence"] = confidence
        _last_detection["face_box"] = face_box


def _get_last_detection():
    with _last_detection_lock:
        return dict(_last_detection)


def _take_window_samples():
    """Return and clear the recognition results collected since the last
    call - used once per owner decision.
    """
    global _window_samples
    with _window_samples_lock:
        samples = _window_samples
        _window_samples = []
    return samples


def _recognition_loop():
    """Background thread: face detection + recognition on every new
    camera frame, as fast as frames arrive. Each result updates the
    preview overlay and the owner-decision window. Exceptions are caught
    per pass so a bad frame can never kill the thread.
    """
    last_id = -1
    while True:
        frame, frame_id = _get_latest_frame_with_id()
        if frame is None or frame_id == last_id:
            time.sleep(0.01)  # no new frame yet - don't spin the CPU
            continue
        last_id = frame_id
        try:
            any_face, raw_recognized, confidence, face_box = detect_owner_presence(
                recognizer, frame
            )
        except Exception as exc:
            print(f"[daemon] recognition pass failed ({exc}) - skipping this frame")
            continue
        _set_last_detection(any_face, raw_recognized, confidence, face_box)
        with _window_samples_lock:
            _window_samples.append(raw_recognized)


def _camera_capture_loop(cap, web_server_holder):
    """Background thread: reads webcam frames at CAMERA_CAPTURE_FPS into
    the shared latest-frame buffer and updates the web preview stream.

    web_server_holder is a 1-element list wrapping main()'s web_server
    variable, so this thread sees it set to None if the server fails.
    """
    global _latest_frame, _latest_frame_id, _latest_frame_time
    target_interval = 1.0 / CAMERA_CAPTURE_FPS
    while True:
        loop_start = time.monotonic()

        ok, frame = cap.read()
        if ok:
            with _latest_frame_lock:
                _latest_frame = frame
                _latest_frame_id += 1
                _latest_frame_time = time.monotonic()

            if web_server_holder[0] is not None:
                det = _get_last_detection()
                try:
                    _update_web_stream_frame(
                        frame, det["any_face"], det["raw_recognized"],
                        det["confidence"], det["face_box"]
                    )
                except Exception as exc:
                    print(f"[daemon] web preview frame update failed "
                          f"({exc}) - disabling it for the rest of this "
                          "run (lock/unlock logic is unaffected)")
                    web_server_holder[0] = None

        elapsed = time.monotonic() - loop_start
        time.sleep(max(0.0, target_interval - elapsed))


# ---------------------------------------------------------------------------
# Web-based live preview (best-effort, see README.md). A tiny built-in
# HTTP server on its own background thread serves an MJPEG stream.
# ---------------------------------------------------------------------------
_web_frame_jpeg = None
_web_frame_lock = threading.Lock()


def _update_web_stream_frame(frame, any_face, owner_recognized, confidence, face_box):
    """Annotate the frame and store it as JPEG bytes for the stream."""
    global _web_frame_jpeg
    annotated = _annotate_frame(frame, any_face, owner_recognized, confidence, face_box)
    ok, buf = cv2.imencode(".jpg", annotated)
    if ok:
        with _web_frame_lock:
            _web_frame_jpeg = buf.tobytes()


class _StreamRequestHandler(BaseHTTPRequestHandler):
    """Serves the live preview over HTTP: "/" is a tiny HTML wrapper,
    "/stream" is the MJPEG multipart feed the <img> tag loads.
    """

    def log_message(self, format, *args):
        pass  # Suppress per-request access logging.

    def do_GET(self):
        if self.path == "/stream":
            self._serve_stream()
        else:
            self._serve_index()

    def _serve_index(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(
            b"<html><body style='margin:0;background:#000'>"
            b"<img src='/stream' style='width:100%;height:auto'>"
            b"</body></html>"
        )

    def _serve_stream(self):
        self.send_response(200)
        self.send_header(
            "Content-Type", "multipart/x-mixed-replace; boundary=frame"
        )
        self.end_headers()
        try:
            while True:
                with _web_frame_lock:
                    jpeg_bytes = _web_frame_jpeg
                if jpeg_bytes is not None:
                    self.wfile.write(b"--frame\r\n")
                    self.wfile.write(b"Content-Type: image/jpeg\r\n")
                    self.wfile.write(
                        f"Content-Length: {len(jpeg_bytes)}\r\n\r\n".encode()
                    )
                    self.wfile.write(jpeg_bytes)
                    self.wfile.write(b"\r\n")
                time.sleep(0.1)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass  # Browser tab/connection closed - not a real error.


class _ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    # ThreadingMixIn instead of http.server.ThreadingHTTPServer (3.7+) -
    # this daemon must also run on the Jetson's stock Python 3.6.
    daemon_threads = True


def start_web_stream_server():
    """Start the web preview server in a background thread. Returns the
    server, or None if it could not start - never raises, since this is
    best-effort and must not break the lock logic.
    """
    try:
        server = _ThreadingHTTPServer(("0.0.0.0", WEB_PREVIEW_PORT), _StreamRequestHandler)
    except OSError as exc:
        print(f"[daemon] could not start web preview server on port "
              f"{WEB_PREVIEW_PORT} ({exc}) - continuing without it")
        return None

    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    print(f"[daemon] web preview server listening on port {WEB_PREVIEW_PORT} - "
          f"visit http://<jetson-ip>:{WEB_PREVIEW_PORT}/ from any device's "
          "browser (including the laptop's) to view the live feed")
    return server


def open_serial():
    """Block until the TinyScreen+'s serial link is available, retrying
    every RETRY_DELAY_SEC.
    """
    while True:
        try:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1, write_timeout=2)
            print(f"[daemon] TinyScreen+ serial link ready on {SERIAL_PORT}")
            return ser
        except SerialException as exc:
            print(f"[daemon] could not open {SERIAL_PORT}: {exc} - retrying in "
                  f"{RETRY_DELAY_SEC}s")
            time.sleep(RETRY_DELAY_SEC)


def wait_for_hid_gadget():
    """Block until the HID gadget device node exists and is writable,
    retrying every RETRY_DELAY_SEC. (Whether the laptop has enumerated it
    cannot be checked from here.)
    """
    while True:
        if os.path.exists(HIDG_PATH) and os.access(HIDG_PATH, os.W_OK):
            print(f"[daemon] HID gadget ready at {HIDG_PATH}")
            return
        print(f"[daemon] waiting for HID gadget at {HIDG_PATH} (see README.md's "
              f"gadget setup) - retrying in {RETRY_DELAY_SEC}s")
        time.sleep(RETRY_DELAY_SEC)


def open_camera():
    """Try each index in CAMERA_INDEXES and return the first webcam that
    opens (the /dev/video number can move after a replug or reboot).
    """
    for index in CAMERA_INDEXES:
        cap = cv2.VideoCapture(index)
        if cap.isOpened():
            print(f"[daemon] camera opened on index {index}")
            return cap
        cap.release()
    sys.exit(
        f"[daemon] could not open any camera index in {CAMERA_INDEXES} - "
        "check the external webcam is plugged in (ls /dev/video*)"
    )


def _send_hid_report(modifier, keycode):
    report = bytes([modifier, 0, keycode, 0, 0, 0, 0, 0])
    with open(HIDG_PATH, "wb") as f:
        f.write(report)


def _press_and_release(modifier, keycode):
    try:
        _send_hid_report(modifier, keycode)
        time.sleep(_KEY_RELEASE_DELAY_SEC)
        _send_hid_report(0, 0)  # release all keys
        return True
    except OSError as exc:
        print(f"[daemon] HID gadget write failed (ignored): {exc}")
        return False


def send_win_l():
    """Send Win+L. Returns True if the HID write succeeded - main() only
    acks the lock when it did, so a failed attempt gets retried.
    """
    print("[daemon] sending Win+L to laptop")
    ok = _press_and_release(_MOD_LEFT_GUI, _KEY_L)
    print(f"[daemon] Win+L HID write -> {'ok' if ok else 'FAILED'}")
    return ok


def _char_to_hid(c):
    """Map a character to (modifier, keycode) for HID typing. Supports
    what a Windows file path needs; returns None for anything else.
    """
    # ord(c) < 128 instead of str.isascii() (3.7+) - this daemon must
    # also run on the Jetson's stock Python 3.6.
    if c.isalpha() and ord(c) < 128:
        keycode = 0x04 + (ord(c.lower()) - ord('a'))
        modifier = _MOD_LEFT_SHIFT if c.isupper() else 0
        return modifier, keycode
    if c.isdigit():
        keycode = 0x27 if c == '0' else 0x1E + (int(c) - 1)
        return 0, keycode
    special = {
        '\\': (0, 0x31),
        ':': (_MOD_LEFT_SHIFT, 0x33),
        '.': (0, 0x37),
        ' ': (0, 0x2C),
        '-': (0, 0x2D),
        '_': (_MOD_LEFT_SHIFT, 0x2D),
    }
    return special.get(c)


def type_text(text):
    """Blindly type text character-by-character via the HID gadget.
    Deliberately logs every character so "it's not typing" can be
    diagnosed from the journal.
    """
    print(f"[daemon] typing {len(text)} characters: {text!r}")
    for c in text:
        mapped = _char_to_hid(c)
        if mapped is None:
            print(f"[daemon] WARNING: cannot type character {c!r} (unsupported "
                  "by _char_to_hid), skipping - check PRIVACY_IMAGE_PATH")
            continue
        modifier, keycode = mapped
        ok = _press_and_release(modifier, keycode)
        print(f"[daemon]   sent {c!r} (modifier=0x{modifier:02x} "
              f"keycode=0x{keycode:02x}) -> {'ok' if ok else 'FAILED'}")
        time.sleep(TYPE_CHAR_DELAY_SEC)


def open_privacy_image():
    """Blindly open PRIVACY_IMAGE_PATH on the laptop: Win+R -> type path
    -> Enter -> F11. See ../README.md for the fragility trade-offs.
    """
    print(f"[daemon] opening privacy image on laptop: {PRIVACY_IMAGE_PATH}")
    _press_and_release(_MOD_LEFT_GUI, _KEY_R)   # Win+R - open Run dialog
    time.sleep(RUN_DIALOG_OPEN_WAIT_SEC)
    type_text(PRIVACY_IMAGE_PATH)
    _press_and_release(0, _KEY_ENTER)           # Launch the default app.
    time.sleep(APP_LAUNCH_WAIT_SEC)
    # F11 fullscreens the Windows Photos app and most other viewers. It is
    # app-level, not an OS shortcut - a default viewer that ignores it
    # just leaves the image in a normal window.
    _press_and_release(0, _KEY_F11)


def close_privacy_image():
    """Alt+F4 the privacy image window (only when returning to
    OWNER_PRESENT without a full lock). Assumes it is still the focused
    window - see ../README.md for this trade-off.
    """
    print("[daemon] closing privacy image window on laptop")
    _press_and_release(_MOD_LEFT_ALT, _KEY_F4)


# ---------------------------------------------------------------------------
# Owner re-registration - handlers for the kit's MCU:CMD= lines.
# `recognizer` is module-level so these handlers can swap it out live.
# ---------------------------------------------------------------------------
recognizer = None


def delete_reference_photo():
    """Delete REFERENCE_PHOTO and stop recognizing anyone (fail-secure)
    until a new photo is captured AND trained. Returns the ack value.
    """
    global recognizer
    recognizer = None
    if os.path.isfile(REFERENCE_PHOTO):
        os.remove(REFERENCE_PHOTO)
        print(f"[daemon] deleted reference photo {REFERENCE_PHOTO}")
    else:
        print(f"[daemon] no reference photo to delete at {REFERENCE_PHOTO}")
    return "PHOTO_DELETED"


def capture_candidate_photo():
    """Save the latest camera frame to CANDIDATE_PHOTO_PATH, but only if
    a face is found in it. Returns the ack value.
    """
    ok, frame = _get_latest_frame()
    if not ok:
        print("[daemon] capture failed: no camera frame available yet")
        return "CAPTURE_NO_FACE"

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    if _detect_largest_face(gray) is None:
        print("[daemon] capture failed: no face found in frame")
        return "CAPTURE_NO_FACE"

    os.makedirs(os.path.dirname(CANDIDATE_PHOTO_PATH), exist_ok=True)
    if not cv2.imwrite(CANDIDATE_PHOTO_PATH, frame):
        # imwrite only reports a failed save through its return value.
        print(f"[daemon] capture failed: could not write {CANDIDATE_PHOTO_PATH} "
              "(check the folder's permissions: ls -ld on its parent)")
        return "CAPTURE_NO_FACE"
    print(f"[daemon] captured candidate owner photo -> {CANDIDATE_PHOTO_PATH}")
    return "CAPTURE_OK"


def train_candidate_photo():
    """Train on CANDIDATE_PHOTO_PATH and, only on success, promote it to
    REFERENCE_PHOTO and swap in the new recognizer. Returns the ack value.
    """
    global recognizer
    if not os.path.isfile(CANDIDATE_PHOTO_PATH):
        print("[daemon] train failed: no candidate photo captured yet")
        return "TRAIN_FAIL"

    new_recognizer = load_owner_recognizer(CANDIDATE_PHOTO_PATH)
    if new_recognizer is None:
        print("[daemon] train failed: could not train on candidate photo")
        return "TRAIN_FAIL"

    try:
        os.makedirs(os.path.dirname(REFERENCE_PHOTO), exist_ok=True)
        os.replace(CANDIDATE_PHOTO_PATH, REFERENCE_PHOTO)
    except OSError as exc:
        print(f"[daemon] train failed: could not save the new owner photo to "
              f"{REFERENCE_PHOTO} ({exc}) - check that folder's permissions")
        return "TRAIN_FAIL"
    recognizer = new_recognizer
    print(f"[daemon] trained + promoted new owner photo -> {REFERENCE_PHOTO}")
    return "TRAIN_OK"


def _send_ack(ser, ack):
    """Send a PC:ACK=<ack> line so the kit can show the result."""
    try:
        ser.write(f"PC:ACK={ack}\n".encode())
    except SerialException:
        print(f"[daemon] could not send PC:ACK={ack} - serial link dropped")


def handle_mcu_line(line, privacy_image_open, in_grace_period):
    """React to the kit lines that need a HID action on the laptop.
    Returns (privacy_image_open, lock_delivered).

    privacy_image_open is the only tracking of whether the privacy image
    is still open (nothing on the laptop can tell us). lock_delivered is
    True only when Win+L was actually written, which main() answers with
    PC:ACK=LOCK_SENT. During the startup grace period, privacy/lock
    requests are logged but ignored - unacked, so the kit repeats them.
    """
    lock_delivered = False
    if line == "MCU:PRIVACY_MODE":
        if in_grace_period:
            print("[daemon] MCU:PRIVACY_MODE received during startup grace "
                  "period - ignoring (not opening privacy image yet)")
        elif not privacy_image_open:
            open_privacy_image()
            privacy_image_open = True
    elif line == "MCU:STATE=LOCKED":
        if in_grace_period:
            print("[daemon] MCU:STATE=LOCKED received during startup grace "
                  "period - ignoring (not sending Win+L yet; the "
                  "TinyScreen+ will repeat it)")
        else:
            lock_delivered = send_win_l()
            # No need to close the image viewer first - the OS lock
            # screen covers it either way.
            privacy_image_open = False
    elif line == "MCU:STATE=UNLOCKED":
        if privacy_image_open:
            close_privacy_image()
            privacy_image_open = False
    return privacy_image_open, lock_delivered


def main():
    global recognizer
    recognizer = load_owner_recognizer()
    if recognizer is None:
        print("[daemon] WARNING: no trained recognizer - every frame will be "
              "reported as 'not the owner' (fail-secure)")

    cap = open_camera()

    # Block until both the HID gadget and the kit's serial link are ready.
    wait_for_hid_gadget()
    ser = open_serial()

    print(f"[daemon] sending PC:FACE= updates on {SERIAL_PORT} every "
          f"{POLL_INTERVAL_SEC}s, recognizing camera index(es) {CAMERA_INDEXES} "
          f"continuously, owner decision every "
          f"{OWNER_DECISION_INTERVAL_SEC}s "
          f"(owner reported absent when under {int(FACE_ABSENT_MAX_RATIO * 100)}% of the "
          f"last {FACE_MISS_CONFIRM_SEC}s of checks matched)")
    print(f"[daemon] startup grace period: {STARTUP_GRACE_SEC}s - "
          "MCU:PRIVACY_MODE/MCU:STATE=LOCKED will be logged but not acted "
          "on until this elapses")
    grace_period_ends_at = time.monotonic() + STARTUP_GRACE_SEC

    web_server = start_web_stream_server()  # Best-effort.
    web_server_holder = [web_server]
    capture_thread = threading.Thread(
        target=_camera_capture_loop, args=(cap, web_server_holder), daemon=True
    )
    capture_thread.start()
    print(f"[daemon] camera-capture thread running at {CAMERA_CAPTURE_FPS} fps")
    threading.Thread(target=_recognition_loop, daemon=True).start()
    print("[daemon] continuous recognition thread running")

    last_owner_recognized = None  # Force a log line on the first reading.
    privacy_image_open = False
    line_buf = ""

    # owner_recognized is the debounced value sent as PC:FACE=.
    owner_recognized = False
    recent_samples = collections.deque()   # (time, matched_owner) rolling window
    history_started_at = None              # When recent_samples first got data
    last_sample_at = time.monotonic()
    next_decision_at = time.monotonic() + OWNER_DECISION_INTERVAL_SEC
    next_face_send_at = 0.0   # Send the first PC:FACE= immediately.
    preview_until = 0.0       # Frames go to the OLED while now < this.

    try:
        while True:
            start = time.monotonic()

            if start >= next_decision_at:
                next_decision_at = start + OWNER_DECISION_INTERVAL_SEC

                # Strict majority of the passes since the last decision
                # must match; a 50/50 split or an empty window counts as
                # not the owner (fail-secure).
                window_samples = _take_window_samples()
                for sample in window_samples:
                    recent_samples.append((start, sample))
                if window_samples:
                    last_sample_at = start
                    if history_started_at is None:
                        history_started_at = start

                # Keep only the last FACE_MISS_CONFIRM_SEC of results.
                while recent_samples and start - recent_samples[0][0] > FACE_MISS_CONFIRM_SEC:
                    recent_samples.popleft()

                if start - last_sample_at > FACE_MISS_CONFIRM_SEC:
                    # Camera silent for the whole window: fail-secure.
                    recent_samples.clear()
                    history_started_at = None
                    owner_recognized = False
                else:
                    # PRESENT: at least half of the last
                    # FACE_RECOVER_WINDOW_SEC matched the owner.
                    last_second = [s for t, s in recent_samples
                                   if start - t <= FACE_RECOVER_WINDOW_SEC]
                    if len(last_second) >= 3 and sum(last_second) * 2 >= len(last_second):
                        owner_recognized = True
                    # ABSENT: a full window of history exists and at most
                    # FACE_ABSENT_MAX_RATIO of it matched the owner.
                    elif (owner_recognized and history_started_at is not None
                          and start - history_started_at >= FACE_MISS_CONFIRM_SEC):
                        total = len(recent_samples)
                        matched = sum(1 for _, s in recent_samples if s)
                        if total and matched <= total * FACE_ABSENT_MAX_RATIO:
                            owner_recognized = False

                if owner_recognized != last_owner_recognized:
                    total = len(recent_samples)
                    matched = sum(1 for _, s in recent_samples if s)
                    print(f"[daemon] decision: {matched}/{total} checks in the last "
                          f"{FACE_MISS_CONFIRM_SEC}s matched owner, "
                          f"reported={owner_recognized}")
                    last_owner_recognized = owner_recognized
                    next_face_send_at = 0.0  # Tell the kit right away.

            try:
                # PC:FACE= goes out every POLL_INTERVAL_SEC; while
                # previewing, a camera frame also goes out every tick.
                if start >= next_face_send_at:
                    next_face_send_at = start + POLL_INTERVAL_SEC
                    ser.write(f"PC:FACE={1 if owner_recognized else 0}\n".encode())
                if start < preview_until:
                    frame_ok, frame = _get_latest_frame()
                    if frame_ok:
                        try:
                            packet = build_preview_packet(
                                frame, _get_last_detection()["raw_recognized"]
                            )
                        except Exception as exc:
                            # A preview bug must never take down the lock logic.
                            print(f"[daemon] could not build OLED preview frame "
                                  f"({exc}) - stopping preview")
                            preview_until = 0.0
                            packet = None
                        if packet is not None:
                            ser.write(packet)
            except SerialException:
                print("[daemon] serial link dropped, reconnecting...")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = open_serial()
                line_buf = ""

            # Non-blocking read of whatever the kit already sent.
            try:
                pending = ser.read(ser.in_waiting).decode(errors="ignore")
            except SerialException:
                pending = ""
            line_buf += pending
            while "\n" in line_buf:
                line, line_buf = line_buf.split("\n", 1)
                line = line.strip()
                if not line:
                    continue
                if line in ("MCU:PRIVACY_MODE", "MCU:STATE=LOCKED", "MCU:STATE=UNLOCKED"):
                    in_grace_period = time.monotonic() < grace_period_ends_at
                    privacy_image_open, lock_delivered = handle_mcu_line(
                        line, privacy_image_open, in_grace_period
                    )
                    if lock_delivered:
                        _send_ack(ser, "LOCK_SENT")
                    if line == "MCU:PRIVACY_MODE" and privacy_image_open:
                        # The image is open - this starts the kit's lock
                        # countdown (also answers repeated requests).
                        _send_ack(ser, "PRIVACY_OPEN")
                elif line == "MCU:CMD=DELETE_REF_PHOTO":
                    _send_ack(ser, delete_reference_photo())
                elif line == "MCU:CMD=CAPTURE_REF_PHOTO":
                    _send_ack(ser, capture_candidate_photo())
                elif line == "MCU:CMD=TRAIN_REF_PHOTO":
                    _send_ack(ser, train_candidate_photo())
                elif line == "MCU:CMD=PREVIEW":
                    if time.monotonic() >= preview_until:
                        print("[daemon] TinyScreen+ live preview started")
                    preview_until = time.monotonic() + PREVIEW_LEASE_SEC
                elif line == "MCU:CMD=PREVIEW_OFF":
                    if preview_until > 0.0:
                        print("[daemon] TinyScreen+ live preview stopped")
                    preview_until = 0.0
                else:
                    print(f"[MCU] {line}")

            elapsed = time.monotonic() - start
            time.sleep(max(0.0, LOOP_TICK_SEC - elapsed))
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
        ser.close()


if __name__ == "__main__":
    main()
