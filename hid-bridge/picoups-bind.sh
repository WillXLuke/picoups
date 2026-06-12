#!/bin/bash
# Called by udev when the RP2040 Virtual UPS's HID interface appears.
# Unbind it from whichever driver currently owns it, then bind to picoups.
INTF="$1"
[ -n "$INTF" ] || exit 0
INTF_PATH="/sys/bus/usb/devices/$INTF"
[ -e "$INTF_PATH" ] || exit 0

# If already bound to picoups, nothing to do.
DRIVER=$(basename "$(readlink "$INTF_PATH/driver" 2>/dev/null)" 2>/dev/null)
[ "$DRIVER" = "picoups" ] && exit 0

# Unbind from current driver (usbhid/hid-generic) if any.
if [ -n "$DRIVER" ]; then
    echo "$INTF" > "/sys/bus/usb/drivers/$DRIVER/unbind" 2>/dev/null || true
fi

# Bind to picoups.
echo "$INTF" > /sys/bus/usb/drivers/picoups/bind 2>/dev/null || true
