#include <Adafruit_TinyUSB.h>

// ---- HID Report IDs ----
#define HID_RID_PRESENT_STATUS   1
#define HID_RID_REMAINING_CAP    2
#define HID_RID_RUNTIME_TO_EMPTY 3
#define HID_RID_BATTERY_STATUS   4
#define HID_RID_DESIGN_CAP       5
#define HID_RID_FULL_CHARGE_CAP  6
#define HID_RID_IDEVICE_CHEM     7
#define HID_RID_IPRODUCT         8
#define HID_RID_ISERIAL          9
#define HID_RID_IMANUFACTURER   10

// HID Power Device descriptor (Usage Page 0x84 = Power, 0x85 = Battery)
uint8_t const desc_hid_report[] = {
  0x05, 0x84,        // USAGE_PAGE (Power Device)
  0x09, 0x04,        // USAGE (UPS)
  0xA1, 0x01,        // COLLECTION (Application)

  0x09, 0x24,        //   USAGE (PowerSummary)
  0xA1, 0x02,        //   COLLECTION (Logical)

    // ---- String indices ----
    0x85, HID_RID_IMANUFACTURER,
    0x09, 0xFD, 0x79, 0x01, 0x75, 0x08, 0x95, 0x01,
    0x15, 0x00, 0x25, 0xFF, 0xB1, 0x23,

    0x85, HID_RID_IPRODUCT,
    0x09, 0xFE, 0x79, 0x02, 0xB1, 0x23,

    0x85, HID_RID_ISERIAL,
    0x09, 0xFF, 0x79, 0x03, 0xB1, 0x23,

    0x85, HID_RID_IDEVICE_CHEM,
    0x05, 0x85, 0x09, 0x89, 0x79, 0x04, 0xB1, 0x23,

    // ---- Capacities (mWh-ish, scaled) ----
    0x85, HID_RID_DESIGN_CAP,
    0x09, 0x83, 0x25, 0x64, 0xB1, 0x83,   // DesignCapacity = 100

    0x85, HID_RID_FULL_CHARGE_CAP,
    0x09, 0x67, 0xB1, 0x83,               // FullChargeCapacity = 100

    0x85, HID_RID_REMAINING_CAP,
    0x09, 0x66, 0x81, 0xA3,               // RemainingCapacity (Input)

    0x85, HID_RID_RUNTIME_TO_EMPTY,
    0x09, 0x68, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x81, 0xA3,               // RuntimeToEmpty (Input, 16-bit)

    // ---- PresentStatus bit field ----
    0x85, HID_RID_PRESENT_STATUS,
    0x05, 0x85,                           // USAGE_PAGE (Battery System)
    0x09, 0x44,                           // Charging
    0x09, 0x45,                           // Discharging
    0x09, 0x42,                           // ACPresent
    0x09, 0xD0,                           // BatteryPresent
    0x09, 0x47,                           // FullyCharged
    0x09, 0x46,                           // NeedReplacement
    0x09, 0xDB,                           // ShutdownRequested
    0x09, 0x68,                           // ShutdownImminent
    0x75, 0x01, 0x95, 0x08,
    0x15, 0x00, 0x25, 0x01, 0x81, 0xA3,   // 8 status bits (Input)

    // ---- BatteryStatus enum ----
    0x85, HID_RID_BATTERY_STATUS,
    0x09, 0x2C,                           // BatteryStatus usage
    0x75, 0x08, 0x95, 0x01,
    0x15, 0x00, 0x25, 0xFF, 0x81, 0xA3,

  0xC0,              //   END_COLLECTION (Logical)
  0xC0               // END_COLLECTION (Application)
};

Adafruit_USBD_HID usb_hid;

// String table the host pulls via Get_Report on indices above
const char* kManufacturer = "AnthropicLabs";
const char* kProduct      = "RP2040 Virtual UPS";
const char* kSerial       = "UPS-0001";
const char* kChemistry    = "LiON";

// Respond to GET_REPORT (FEATURE & INPUT) from the host
uint16_t get_report_cb(uint8_t report_id,
                       hid_report_type_t report_type,
                       uint8_t* buffer, uint16_t reqlen) {
  (void)report_type;

  auto putStr = [&](const char* s) -> uint16_t {
    uint16_t n = strlen(s);
    if (n > reqlen) n = reqlen;
    memcpy(buffer, s, n);
    return n;
  };

  switch (report_id) {
    case HID_RID_IMANUFACTURER: return putStr(kManufacturer);
    case HID_RID_IPRODUCT:      return putStr(kProduct);
    case HID_RID_ISERIAL:       return putStr(kSerial);
    case HID_RID_IDEVICE_CHEM:  return putStr(kChemistry);

    case HID_RID_DESIGN_CAP:
    case HID_RID_FULL_CHARGE_CAP:
    case HID_RID_REMAINING_CAP:
      buffer[0] = 100;                 // 100 %
      return 1;

    case HID_RID_RUNTIME_TO_EMPTY:
      buffer[0] = 0xFF; buffer[1] = 0xFF;   // "infinite"
      return 2;

    case HID_RID_PRESENT_STATUS: {
      // bit0 Charging, bit1 Discharging, bit2 ACPresent,
      // bit3 BatteryPresent, bit4 FullyCharged, bit5 NeedReplacement,
      // bit6 ShutdownRequested, bit7 ShutdownImminent
      buffer[0] = (1 << 2) | (1 << 3) | (1 << 4);   // AC + Battery + Full
      return 1;
    }
    case HID_RID_BATTERY_STATUS:
      buffer[0] = 2;                   // 2 = Fully Charged (HID PDC spec)
      return 1;
  }
  return 0;
}

// Host writes (SET_REPORT) — UPS has no writable features, ignore
void set_report_cb(uint8_t, hid_report_type_t,
                   uint8_t const*, uint16_t) {}

void setup() {
  // Identify as a UPS at the USB descriptor level too
  TinyUSBDevice.setManufacturerDescriptor(kManufacturer);
  TinyUSBDevice.setProductDescriptor(kProduct);
  TinyUSBDevice.setSerialDescriptor(kSerial);
  TinyUSBDevice.setID(0x239A, 0x80FE);   // Adafruit debug VID + sketch PID

  usb_hid.setPollInterval(10);
  usb_hid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
  usb_hid.setReportCallback(get_report_cb, set_report_cb);
  usb_hid.begin();

  while (!TinyUSBDevice.mounted()) delay(10);
}

void loop() {
  // Push an unsolicited PresentStatus every 2 s so the OS UI updates promptly
  static uint32_t last = 0;
  if (millis() - last > 2000 && usb_hid.ready()) {
    uint8_t status = (1 << 2) | (1 << 3) | (1 << 4);  // AC+Batt+Full
    usb_hid.sendReport(HID_RID_PRESENT_STATUS, &status, 1);

    uint8_t cap = 100;
    usb_hid.sendReport(HID_RID_REMAINING_CAP, &cap, 1);
    last = millis();
  }
}
