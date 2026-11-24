#!/bin/bash
# Smart Lock-Screen - tears down the USB HID gadget created by start.sh.
# Safe to run even if the gadget isn't currently set up (each step is
# best-effort / ignores "already gone" errors).
CONFIGFS_HOME=/sys/kernel/config
GADGET_NAME="smartlock"
cd "$CONFIGFS_HOME/usb_gadget/$GADGET_NAME" 2>/dev/null || exit 0

echo "" > UDC 2>/dev/null || true
rm -f configs/c.1/hid.usb0
rmdir functions/hid.usb0 2>/dev/null
rmdir configs/c.1/strings/0x409 2>/dev/null
rmdir configs/c.1 2>/dev/null
rmdir strings/0x409 2>/dev/null
cd ..
rmdir "$GADGET_NAME" 2>/dev/null

exit 0
