#!/bin/bash
# Smart Lock-Screen - USB HID keyboard gadget setup (Jetson Nano P3450)
#
# Creates a configfs-based USB HID gadget on the Jetson's micro-USB OTG
# port, making the Jetson appear to the laptop as a plain USB keyboard
# (no drivers needed on the laptop). ../face_presence_daemon.py writes
# raw HID reports to the resulting /dev/hidg0.
#
# See ../README.md's "HID gadget setup" for the one-time install steps
# (including disabling NVIDIA's conflicting default gadget) and
# smartlock-hid-gadget.service for running this on every boot.
set -e

modprobe libcomposite

CONFIGFS_HOME=/sys/kernel/config
GADGET_NAME="smartlock"
VID="0x1d6b"   # Linux Foundation vendor ID (generic, standard for gadget devices)
PID="0x0104"   # generic HID PID
SERIAL="smartlock-kbd-01"
MANUF="SmartLockScreen"
PRODUCT="SmartLock Privacy Keyboard"

mkdir -p "$CONFIGFS_HOME/usb_gadget/$GADGET_NAME"
cd "$CONFIGFS_HOME/usb_gadget/$GADGET_NAME"

echo "$VID" > idVendor
echo "$PID" > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "$SERIAL"  > strings/0x409/serialnumber
echo "$MANUF"   > strings/0x409/manufacturer
echo "$PRODUCT" > strings/0x409/product

mkdir -p configs/c.1/strings/0x409
echo "Keyboard Config" > configs/c.1/strings/0x409/configuration
echo 120 > configs/c.1/MaxPower

# --- HID keyboard function ---
mkdir -p functions/hid.usb0
echo 1 > functions/hid.usb0/protocol       # 1 = keyboard (boot protocol)
echo 1 > functions/hid.usb0/subclass       # 1 = boot interface subclass
echo 8 > functions/hid.usb0/report_length  # standard 8-byte keyboard report

# Standard USB HID boot-keyboard report descriptor (modifier byte + 6
# simultaneous keys).
echo -ne \
'\x05\x01\x09\x06\xa1\x01\x05\x07\x19\xe0\x29\xe7\x15\x00\x25\x01'\
'\x75\x01\x95\x08\x81\x02\x95\x01\x75\x08\x81\x03\x95\x05\x75\x01'\
'\x05\x08\x19\x01\x29\x05\x91\x02\x95\x01\x75\x03\x91\x03\x95\x06'\
'\x75\x08\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00\xc0' \
  > functions/hid.usb0/report_desc

ln -sf functions/hid.usb0 configs/c.1/

# Bind to whichever UDC this board exposes - the name differs by L4T version.
UDC_DEV=$(ls /sys/class/udc | head -n1)
echo "$UDC_DEV" > UDC

echo "HID keyboard gadget bound to $UDC_DEV -> /dev/hidg0 ready"
