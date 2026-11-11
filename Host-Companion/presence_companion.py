"""
Smart Lock-Screen - host companion (webcam face recognition)

PC-side helper for ../Kit-Side/SmartLockScreen/SmartLockScreen.ino. Every
POLL_INTERVAL_SEC it grabs a webcam frame, checks if the largest face
matches the owner (trained on photos in REFERENCE_PHOTOS_DIR), and sends
"PC:FACE=1" or "PC:FACE=0" to the TinyScreen+ over serial. Also shows a
preview window with a box + label so you can see what it's picking up -
press 'q' to quit.

If this script stops, crashes, or the serial link drops, the MCU just
stops getting PC:FACE= updates and treats it as absent after
PC_SIGNAL_TIMEOUT_MS (fail-secure, never "always present"). Same idea in
here - no photos, no face, or a bad match all count as "not the owner".

Setup:
    1. Put a few clear owner photos (jpg/png) in REFERENCE_PHOTOS_DIR
    2. pip install -r requirements.txt (needs opencv-contrib-python, not
       plain opencv-python - uninstall the plain one first if you have it)
    3. Set SERIAL_PORT to the TinyScreen+'s COM port
    4. python presence_companion.py
"""

import os
import sys
import time

import cv2
import numpy as np
import serial
from serial import SerialException

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SERIAL_PORT = "COM10"          # TODO: set to the TinyScreen+'s actual port
BAUD_RATE = 115200
POLL_INTERVAL_SEC = 1.0        # how often to sample the webcam
CAMERA_INDEX = 0               # 0 = default/built-in webcam
RECONNECT_DELAY_SEC = 2.0      # retry delay if the serial port disappears
SHOW_PREVIEW_WINDOW = True     # live webcam window with face box + label

REFERENCE_PHOTOS_DIR = (
    r"C:\Users\ohbei\Desktop\SIT Modules\SIT_ICT1011 - comp org & arch"
    r"\Team Project\TinyCircuit_pictures\ohbeirui_photo"
)
FACE_SIZE = (200, 200)              # LBPH needs a consistent crop size
RECOGNITION_CONFIDENCE_MAX = 100.0  # LBPH "confidence" is actually a
                                     # distance - lower = more similar,
                                     # tune against real test values
OWNER_LABEL = 0                 # only one identity is ever trained (the owner)

# Bundled locally since some opencv-contrib-python installs ship
# cv2.data.haarcascades without the actual xml files inside.
_CASCADE_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "haarcascade_frontalface_default.xml",
)
_FACE_CASCADE = cv2.CascadeClassifier(_CASCADE_PATH)
if _FACE_CASCADE.empty():
    sys.exit(
        f"[companion] could not load face cascade from {_CASCADE_PATH} - "
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


def load_owner_recognizer():
    """Train an LBPH face recognizer from photos under REFERENCE_PHOTOS_DIR.

    Returns None if no usable photo was found - treat that as fail-secure
    ("can't recognize the owner"), never as "owner always present".
    """
    if not os.path.isdir(REFERENCE_PHOTOS_DIR):
        print(f"[companion] no reference photos folder at {REFERENCE_PHOTOS_DIR}")
        return None

    samples = []
    for name in sorted(os.listdir(REFERENCE_PHOTOS_DIR)):
        if not name.lower().endswith((".jpg", ".jpeg", ".png")):
            continue
        path = os.path.join(REFERENCE_PHOTOS_DIR, name)
        img = cv2.imread(path)
        if img is None:
            print(f"[companion] could not read {name}, skipping")
            continue
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        face = _detect_largest_face(gray)
        if face is None:
            print(f"[companion] no face found in {name}, skipping")
            continue
        x, y, w, h = face
        face_crop = cv2.resize(gray[y:y + h, x:x + w], FACE_SIZE)
        samples.append(face_crop)

    if not samples:
        print(f"[companion] no usable reference photos in {REFERENCE_PHOTOS_DIR}")
        return None

    recognizer = cv2.face.LBPHFaceRecognizer_create()
    recognizer.train(samples, np.array([OWNER_LABEL] * len(samples)))
    print(f"[companion] trained owner face recognizer on {len(samples)} reference photo(s)")
    return recognizer


def detect_owner_presence(recognizer, frame):
    """Returns (any_face_detected, owner_recognized, confidence, face_box).

    confidence is None if there's no face or no recognizer. owner_recognized
    needs both a face AND a match within RECOGNITION_CONFIDENCE_MAX.
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


def show_preview(frame, face_box, any_face, owner_recognized, confidence):
    """Live preview window with a face box + label. Just a visual aid -
    doesn't affect the PC:FACE= signal sent to the MCU.
    """
    display_frame = frame.copy()
    if face_box is not None:
        x, y, w, h = face_box
        color = (0, 255, 0) if owner_recognized else (0, 0, 255)  # BGR
        cv2.rectangle(display_frame, (x, y), (x + w, y + h), color, 2)
        if owner_recognized:
            label = "OWNER"
        elif any_face:
            label = "NOT OWNER"
        else:
            label = "NO FACE"
        if confidence is not None:
            label += f" ({confidence:.0f})"
        cv2.putText(display_frame, label, (x, max(0, y - 10)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)
    else:
        cv2.putText(display_frame, "NO FACE", (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)
    cv2.putText(display_frame, "Press 'q' in this window to quit",
                (10, display_frame.shape[0] - 10),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
    cv2.imshow("Smart Lock-Screen - Webcam Preview", display_frame)


def open_serial():
    while True:
        try:
            # write_timeout matters too - without it ser.write() can block
            # forever if the port stalls, which freezes the preview window.
            return serial.Serial(
                SERIAL_PORT, BAUD_RATE, timeout=0.1, write_timeout=0.1
            )
        except SerialException as exc:
            # Windows only lets one program hold a serial port open at a
            # time - this is usually Arduino IDE's Serial Monitor (or
            # another copy of this script) already connected.
            print(f"[companion] could not open {SERIAL_PORT} (retrying in "
                  f"{RECONNECT_DELAY_SEC:.0f}s) - if this keeps happening, "
                  "check that Arduino IDE's Serial Monitor (or any other "
                  f"program) isn't already connected to {SERIAL_PORT}: {exc}")
            time.sleep(RECONNECT_DELAY_SEC)


def main():
    cam = cv2.VideoCapture(CAMERA_INDEX)
    if not cam.isOpened():
        sys.exit(f"[companion] could not open camera index {CAMERA_INDEX}")

    recognizer = load_owner_recognizer()
    if recognizer is None:
        print("[companion] WARNING: no owner recognizer trained - the face "
              "signal will always report 'not recognized' until reference "
              f"photos are added to {REFERENCE_PHOTOS_DIR}")

    ser = open_serial()
    print(f"[companion] connected to {SERIAL_PORT}, starting presence loop")

    # prints the MCU's own serial output (BLE debug, MCU:WARN, etc.) here so
    # you don't also need Serial Monitor open on the same port
    line_buf = ""

    try:
        while True:
            ok, frame = cam.read()
            if ok:
                any_face, owner_recognized, confidence, face_box = detect_owner_presence(recognizer, frame)
            else:
                any_face, owner_recognized, confidence, face_box = False, False, None, None

            if SHOW_PREVIEW_WINDOW and ok:
                show_preview(frame, face_box, any_face, owner_recognized, confidence)

            try:
                ser.write(f"PC:FACE={1 if owner_recognized else 0}\n".encode())
            except SerialException:
                print("[companion] serial link dropped, reconnecting...")
                ser.close()
                ser = open_serial()
                line_buf = ""
                continue

            # non-blocking read of whatever the MCU already sent
            try:
                pending = ser.read(ser.in_waiting or 1).decode(errors="ignore")
            except SerialException:
                pending = ""
            line_buf += pending
            while "\n" in line_buf:
                line, line_buf = line_buf.split("\n", 1)
                line = line.strip()
                if line:
                    print(f"[MCU] {line}")

            if SHOW_PREVIEW_WINDOW:
                # waitKey pumps the window's event loop and doubles as the
                # loop delay; 'q' with the window focused quits
                key = cv2.waitKey(max(1, int(POLL_INTERVAL_SEC * 1000))) & 0xFF
                if key == ord('q'):
                    print("[companion] 'q' pressed in preview window, stopping")
                    break
            else:
                time.sleep(POLL_INTERVAL_SEC)
    except KeyboardInterrupt:
        print("\n[companion] stopping")
    finally:
        cam.release()
        cv2.destroyAllWindows()
        ser.close()


if __name__ == "__main__":
    main()
