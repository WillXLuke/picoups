# RP2040 HID UPS Simulator

A small Arduino sketch that turns a Raspberry Pi RP2040 (tested on a VCC-GND YD RP2040 clone) into a USB HID-class **Uninterruptible Power Supply (UPS)**. The device enumerates with Usage Page `0x84` (Power Device), Usage `0x04` (UPS), and a `PowerSummary` logical collection. It answers GET_REPORT for all 10 fields (capacities, runtime, present status, battery status, manufacturer/product/serial/chemistry strings) and pushes unsolicited PresentStatus and RemainingCapacity reports every 2 s.

The host sees:

```
Bus 001 Device 071: ID 239a:80fe Adafruit RP2040 Virtual UPS
  iManufacturer "Pico USB"
  iProduct      "RP2040 Virtual UPS"
  iSerial       "UPS-0001"
```

Read by Windows' built-in HID Battery driver, NUT (`usbhid-ups`), and any other application that speaks the HID Power Device (PDC) spec.

## Hardware

- Any RP2040 board. Tested on **VCC-GND YD RP2040** (4 MB on-board flash, RP2040 chip, no WiFi).
- USB cable (data, not charge-only).
- No external components required.

## Software / toolchain

- Arduino CLI (`arduino-cli`)
- Board package: `rp2040:rp2040` (Earle Philhower's arduino-pico) v5.x
- Library: `Adafruit_TinyUSB_Library` v3.7.x

Install:

```sh
arduino-cli core update-index
arduino-cli core install rp2040:rp2040
arduino-cli lib install "Adafruit TinyUSB Library"
```

## Build and flash

The sketch lives at `picoups.ino` (the file was renamed from `main.ino` to match the directory — the Arduino CLI requires `<dirname>.ino`).

```sh
arduino-cli compile \
  --fqbn rp2040:rp2040:vccgnd_yd_rp2040 \
  --board-options "flash=4194304_0,usbstack=tinyusb" \
  --upload --port /dev/ttyACM0 .
```

Notes:

- The FQBN must be the YD-specific one (`vccgnd_yd_rp2040`), not `rpipicow` or `rpipico` — only the YD's menu exposes the 4 MB flash option.
- `flash=4194304_0` selects "4 MB, no FS". `usbstack=tinyusb` selects the Adafruit TinyUSB USB stack. Without `usbstack=tinyusb` the build fails with `TinyUSB is not selected, please select it in Tools->Menu->USB Stack`.
- If your board is on a different serial port, replace `/dev/ttyACM0`. If `arduino-cli board list` does not see it, add yourself to the `dialout` group or `chmod 666` the device node.
- For other RP2040 boards, substitute the matching FQBN from `arduino-cli board listall` (e.g. `rp2040:rp2040:rpipico` for the original Pi Pico, `rp2040:rp2040:rpipicow` for the Pico W). Use the closest 2 MB or 4 MB flash option that matches your board.

## What the device reports

The device exposes 10 HID reports. All 10 are GET_REPORT-able; the first 2 (PresentStatus, RemainingCapacity) are also pushed unsolicited every 2 s.

| Report ID | Name | Size | Advertised as | Value |
|---|---|---|---|---|
| 1 | PresentStatus | 1 byte | Input | `0x1C` = ACPresent + BatteryPresent + FullyCharged |
| 2 | RemainingCapacity | 1 byte | Input | `100` (0x64) |
| 3 | RuntimeToEmpty | 2 bytes | Input | `0xFFFF` (infinite / unknown) |
| 4 | BatteryStatus | 1 byte | Input | `2` (Fully Charged) |
| 5 | DesignCapacity | 1 byte | Feature | `100` |
| 6 | FullChargeCapacity | 1 byte | Feature | `100` |
| 7 | iDeviceChemistry | variable | Feature | `"LiON"` |
| 8 | iProduct | variable | Feature | `"RP2040 Virtual UPS"` |
| 9 | iSerial | variable | Feature | `"UPS-0001"` |
| 10 | iManufacturer | variable | Feature | `"Pico USB"` |

The USB device-level VID/PID is `0x239A:0x80FE` (Adafruit debug VID, sketch-specific PID).

## Verifying it works

### 1. Enumeration

```sh
lsusb -v -d 239a:80fe
```

You should see three interfaces (CDC-ACM for Serial, CDC-Data, and one HID with class code 3 and a 10 ms interrupt-IN endpoint). The HID report descriptor (131 bytes) must begin with `05 84 09 04 a1 01` (Usage Page = Power Device, Usage = UPS, Application collection).

### 2. Battery data on the wire (host-side probe)

The sketch defines Feature reports for capacities and string fields, so any process that opens the device's `hidraw` node and issues HID GET_REPORT will get the values back. A reference test in `tools/hidraw_get_report.c` (see the project wiki or compile from the snippet below) does exactly that:

```c
// Compile:  gcc -O2 -Wall tools/hidraw_get_report.c -o tools/hidraw_get_report
// Run:      sudo ./tools/hidraw_get_report /dev/hidraw6
//
// For each Report ID 1..10:
//   buf[0] = report_id;
//   ioctl(fd, HIDIOCGFEATURE(64), buf);
//   print ret bytes hex + ASCII
```

Expected output (observed on Linux 7.0.0-22-generic, 7.0.0-22 kernel, xhci driver, 2026-06):

```
Device: bus=3 vid=0x239a pid=0xffff80fe
Report descriptor size: 131 bytes
RID  1 PresentStatus       (1) :  2 bytes : 01 1c  | ..
RID  2 RemainingCapacity   (2) :  2 bytes : 02 64  | .d
RID  3 RuntimeToEmpty      (3) :  3 bytes : 03 ff ff  | ...
RID  4 BatteryStatus       (4) :  2 bytes : 04 02  | ..
RID  5 DesignCapacity      (5) :  2 bytes : 05 64  | .d
RID  6 FullChargeCapacity  (6) :  2 bytes : 06 64  | .d
RID  7 iDeviceChemistry    (7) :  5 bytes : 07 4c 69 4f 4e  | .LiON
RID  8 iProduct             (8) : 19 bytes : 08 52 50 32 30 34 30 20 56 69 72 74 75 61 6c 20 55 50 53  | .RP2040 Virtual UPS
RID  9 iSerial              (9) :  9 bytes : 09 55 50 53 2d 30 30 30 31  | .UPS-0001
RID 10 iManufacturer       (10): 14 bytes : 0a 41 6e 74 68 72 6f 70 69 63 4c 61 62 73  | .Pico USB
```

The first byte of every response is the Report ID echoed back; the second byte (and beyond) is the actual data. The reported `pid=0xffff80fe` is a kernel artifact (it ORs the bus number into the high bits) — the real device PID is `0x80FE`.

### 3. upower / NUT

On distros with the `hid-ups` (a.k.a. `usbhid-ups`) kernel module and `upower` or NUT installed, the device should appear as a UPS:

```sh
upower -d
# or
nut-scanner
# or
upsdrvctl start
```

On Ubuntu 25+ with the stock `linux-image-generic` kernel, `hid-ups` is not built as a module by default and is not in the apt repository, so the device enumerates as a generic HID (`hid-generic`) rather than as a UPS at the kernel-power-supply layer. This is a host-side limitation, not a device bug — the HID class is correct, and NUT's `usbhid-ups` driver (which speaks directly to `/dev/hidrawN` without needing `hid-ups` in the kernel) will work.

## Customising

- **Change the simulated state:** edit the `case` arms in `get_report_cb` and the `if` block in `loop()`.
- **Change the identity strings:** edit the four `const char*` declarations (`kManufacturer`, `kProduct`, `kSerial`, `kChemistry`) and the matching `setXDescriptor` calls in `setup()`. Keep them consistent with `desc_hid_report`'s `79 01..79 04` string-index assignments.
- **Change the VID/PID:** edit the `TinyUSBDevice.setID(0x239A, 0x80FE)` call. If you ship a real product, use a real vendor ID from usb.org.
- **Adjust the unsolicited report interval:** change the `2000` ms in `loop()`.
- **Add fields:** add a new Report ID via `#define`, append it to `desc_hid_report`, add a `case` to `get_report_cb`, and (optionally) add a `sendReport` call in `loop()`.

## HID descriptor cheat sheet

The descriptor at `desc_hid_report[]` is hand-written. Important bytes:

- `05 84` — USAGE_PAGE (Power Device)
- `09 04` — USAGE (UPS)
- `A1 01` — COLLECTION (Application)
- `09 24 A1 02` — USAGE (PowerSummary) / COLLECTION (Logical)
- `05 85` — USAGE_PAGE (Battery System) — switched to for the PresentStatus field
- `B1 23` — Feature (variable, const, absolute, no-wait) — used for strings and capacities
- `81 A3` — Input (variable, const, absolute, no-wait) — used for live-updated values
- `C0 C0` — END_COLLECTION (Logical) / END_COLLECTION (Application)

The 8 PresentStatus bits (Report ID 1) are documented in HID Usage Tables 0x85 / Battery System:

| Bit | Usage | Meaning |
|---|---|---|
| 0 | 0x44 | Charging |
| 1 | 0x45 | Discharging |
| 2 | 0x42 | ACPresent |
| 3 | 0xD0 | BatteryPresent |
| 4 | 0x47 | FullyCharged |
| 5 | 0x46 | NeedReplacement |
| 6 | 0xDB | ShutdownRequested |
| 7 | 0x68 | ShutdownImminent |

## Files

- `picoups.ino` — the only source file. Open in the Arduino IDE or compile with `arduino-cli`.
- `hid-bridge/` — out-of-tree Linux kernel module that reads the device over `/dev/hidrawN` and exposes `picoups-battery` + `picoups-ac` to the kernel `power_supply` class, so `upower` and the GNOME power panel see the UPS as a real battery.

## Linux battery / GNOME panel visibility

A `power_supply` device is the only way for the Linux power-management stack (`upower`, GNOME Settings, KDE PowerDevil) to see a battery. The shipping `linux-image-generic` kernel on Ubuntu 25+ does not include the `hid-ups` driver that translates HID Power Device reports into `power_supply` devices, so the device enumerates as a generic HID but no battery shows up.

This repo includes a small out-of-tree kernel module, `hid-bridge/picoups.ko`, that:

1. Registers as a USB driver matching `239A:80FE`.
2. Claims the device's HID interface (interface 2) so it can issue USB control transfers.
3. Registers a `picoups-battery` (type `BATTERY`) and `picoups-ac` (type `MAINS`) `power_supply` device.
4. Spawns a kernel thread that issues HID `GET_REPORT` control transfers (Report IDs 1–4, every 2 s) via `usb_control_msg` against endpoint 0.
5. Decodes the 8-bit PresentStatus byte and updates `power_supply` properties accordingly.
6. When the device is unplugged, the GET_REPORT control transfers fail. After 3 consecutive failures (~6 s), the module marks `bat_present = false` and `ac_online = false`, so the battery icon disappears from upower and GNOME. When the device comes back, the icon reappears.

> Note: because picoups owns the HID interface, `/dev/hidrawN` for this UPS will not be exposed while the module is loaded. The CDC-ACM Serial port (`/dev/ttyACM0`) is unaffected.

### Build

```sh
sudo apt install -y linux-headers-$(uname -r) build-essential
cd hid-bridge
make
sudo cp picoups.ko /lib/modules/$(uname -r)/extra/
sudo depmod
```

### Load now and on every boot

```sh
sudo modprobe picoups
echo picoups | sudo tee /etc/modules-load.d/picoups.conf
```

### Auto-bind the HID interface on (re)plug

The kernel probes USB drivers in registration order. `usbhid` is built into the kernel and claims the HID interface before `picoups` gets a chance. Install this udev rule so the HID interface is handed to `picoups` every time the UPS is plugged in:

```sh
sudo cp hid-bridge/picoups-bind.sh /lib/udev/picoups-bind.sh
sudo chmod +x /lib/udev/picoups-bind.sh
sudo cp hid-bridge/99-picoups-bind.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

If the UPS is already plugged in when you load the module, the driver will usually bind automatically on `modprobe picoups` (the kernel re-probes existing interfaces for newly registered drivers). If it doesn't, run once:

```sh
# Find the bus path of your UPS (e.g. "1-8")
lsusb -t | grep 239a
# Replace "1-8:1.2" with your actual bus path and HID interface number
sudo sh -c 'echo "1-8:1.2" > /sys/bus/usb/drivers/usbhid/unbind'
sudo sh -c 'echo "1-8:1.2" > /sys/bus/usb/drivers/picoups/bind'
```

### Verify

```sh
ls -l /sys/class/power_supply/picoups-battery /sys/class/power_supply/picoups-ac
cat /sys/class/power_supply/picoups-battery/present           # 1 (device present)
cat /sys/class/power_supply/picoups-battery/capacity          # 100
cat /sys/class/power_supply/picoups-battery/status            # Full
cat /sys/class/power_supply/picoups-ac/online                 # 1
upower -d
```

`upower -d` should show two extra devices:

```
Device: /org/freedesktop/UPower/devices/battery_picoups_battery
  vendor:               Pico USB
  model:                RP2040 Virtual UPS
  serial:               UPS-0001
  state:                fully-charged
  percentage:           100%
  technology:           lithium-ion
  icon-name:           'battery-full-charged-symbolic'

Device: /org/freedesktop/UPower/devices/line_power_picoups_ac
  online:               yes
  icon-name:           'ac-adapter-symbolic'
```

These are exactly the object paths and icon names that the GNOME top-bar battery indicator and the GNOME Settings → Power panel consume, so a fake battery shows up there.

### Unplug behaviour

When the pico is unplugged (or the USB device is deauthorised), the kernel logs `picoups: device offline, battery -> not present`. Within a few seconds:

- `/sys/class/power_supply/picoups-battery/present` becomes `0`
- `status` becomes `Unknown`
- `upower -d` shows `present: no`, `state: unknown`, icon `battery-missing-symbolic`
- The GNOME battery indicator removes the battery icon

When the pico is plugged back in, the icon reappears within ~2 s (one poll cycle).

### Re-build after a kernel upgrade

```sh
cd hid-bridge && make
sudo cp picoups.ko /lib/modules/$(uname -r)/extra/
sudo depmod
```
