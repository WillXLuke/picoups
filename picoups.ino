/*
 * RP2040 HID UPS Simulator — reports varying battery state with AC plug/unplug cycle
 *
 * Board: any RP2040 (Pi Pico, etc.)
 * Core:  Earle Philhower's arduino-pico  (USB Stack = "Adafruit TinyUSB")
 * Libs:  Adafruit TinyUSB Library  (Tools -> Manage Libraries)
 *
 * Tools menu settings:
 *   USB Stack: Adafruit TinyUSB
 *   CPU Speed: 133 MHz (default)
 */

#include <Adafruit_TinyUSB.h>

// ---- HID Report IDs ----
#define HID_RID_PRESENT_STATUS         1
#define HID_RID_REMAINING_CAP          2
#define HID_RID_RUNTIME_TO_EMPTY       3
#define HID_RID_BATTERY_STATUS         4
#define HID_RID_DESIGN_CAP             5
#define HID_RID_FULL_CHARGE_CAP        6
#define HID_RID_IDEVICE_CHEM           7
#define HID_RID_IPRODUCT               8
#define HID_RID_ISERIAL                9
#define HID_RID_IMANUFACTURER         10
#define HID_RID_RECHARGEABLE          11
#define HID_RID_CAPACITY_MODE         12
#define HID_RID_WARNING_CAP_LIMIT     13
#define HID_RID_REMAINING_CAP_LIMIT   14
#define HID_RID_DESIGN_VOLTAGE        15
#define HID_RID_CONFIG_VOLTAGE        16

// ---- Battery simulation tuning ----
static const uint8_t  kDesignCapacity       = 100;   // %
static const uint8_t  kFullChargeCapacity   = 100;   // %
static const uint8_t  kWarningLimit         = 20;    // %
static const uint8_t  kRemainingLimit       = 5;     // %
static const uint16_t kDesignVoltage_mV     = 12000; // 12.0 V
static const uint16_t kConfigVoltage_mV     = 12000; // 12.0 V

// HID Power Device descriptor (Usage Page 0x84 = Power, 0x85 = Battery System)
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

    // ---- Rechargeable (true) ----
    0x85, HID_RID_RECHARGEABLE,
    0x09, 0x8B, 0x75, 0x01, 0x95, 0x01,
    0x15, 0x00, 0x25, 0x01, 0xB1, 0x22,

    // ---- CapacityMode = 2 (percentage) ----
    0x85, HID_RID_CAPACITY_MODE,
    0x09, 0x2C, 0x75, 0x08, 0x95, 0x01,
    0x15, 0x00, 0x25, 0xFF, 0xB1, 0x23,

    // ---- WarningCapacityLimit (20%) ----
    0x85, HID_RID_WARNING_CAP_LIMIT,
    0x09, 0x8C, 0xB1, 0xA2,

    // ---- RemainingCapacityLimit (5%) ----
    0x85, HID_RID_REMAINING_CAP_LIMIT,
    0x09, 0x29, 0xB1, 0xA2,

    // ---- Capacities (percent) ----
    0x85, HID_RID_DESIGN_CAP,
    0x09, 0x83, 0x25, 0x64, 0xB1, 0x83,   // DesignCapacity = 100

    0x85, HID_RID_FULL_CHARGE_CAP,
    0x09, 0x67, 0xB1, 0x83,               // FullChargeCapacity = 100

    // RemainingCapacity: INPUT (live updates) + FEATURE (required by Windows)
    0x85, HID_RID_REMAINING_CAP,
    0x09, 0x66, 0x81, 0xA3,
    0x09, 0x66, 0xB1, 0xA3,

    // RuntimeToEmpty: INPUT + FEATURE (16-bit, seconds; 0xFFFF = unknown)
    0x85, HID_RID_RUNTIME_TO_EMPTY,
    0x09, 0x68, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x81, 0xA3,
    0x09, 0x68, 0xB1, 0xA3,

    // ---- Voltage (Volts x 10, i.e. 12.00 V = 1200 in centivolts-ish scale) ----
    // Use Volts * 10 as a unitless 16-bit value (matches many reference implementations).
    0x85, HID_RID_DESIGN_VOLTAGE,
    0x09, 0x57, 0x15, 0x00, 0x26, 0xFF, 0x7F,
    0x75, 0x10, 0x67, 0x21, 0xD1, 0xF0, 0x00, 0x55, 0x05,
    0xB1, 0x23,   // Feature, 16-bit, with UNIT (Centivolts) + UNIT_EXPONENT (5)

    0x85, HID_RID_CONFIG_VOLTAGE,
    0x09, 0x40, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x67, 0x21, 0xD1, 0xF0, 0x00, 0x55, 0x05,
    0xB1, 0x23,

    // ---- PresentStatus: 16-bit INPUT + FEATURE ----
    0x05, 0x85,                          // USAGE_PAGE (Battery System)
    0x85, HID_RID_PRESENT_STATUS,
    0x09, 0x44,                          // Charging
    0x09, 0x45,                          // Discharging
    0x09, 0xD0,                          // ACPresent
    0x09, 0xD1,                          // BatteryPresent
    0x09, 0x42,                          // BelowRemainingCapacityLimit
    0x09, 0x43,                          // RemainingTimeLimitExpired
    0x09, 0x4B,                          // NeedReplacement
    0x09, 0xDB,                          // VoltageNotRegulated
    0x09, 0x46,                          // FullyCharged
    0x09, 0x47,                          // FullyDischarged
    0x05, 0x84,
    0x09, 0x68,                          // ShutdownRequested
    0x09, 0x69,                          // ShutdownImminent
    0x09, 0x73,                          // CommunicationLost
    0x09, 0x65,                          // Overload
    0x75, 0x01, 0x95, 0x10,
    0x15, 0x00, 0x25, 0x01,
    0x81, 0xA3,                          // 14-bit Input (Volatile)
    0x95, 0x02, 0x81, 0x01,              // 2 padding bits (Input, Constant)
    0xB1, 0xA3,                          // Feature mirror

    // ---- BatteryStatus enum ----
    0x05, 0x85,
    0x85, HID_RID_BATTERY_STATUS,
    0x09, 0x2C, 0x75, 0x08, 0x95, 0x01,
    0x15, 0x00, 0x25, 0xFF, 0x81, 0xA3,
    0x09, 0x2C, 0xB1, 0xA3,

  0xC0,              //   END_COLLECTION (Logical)
  0xC0               // END_COLLECTION (Application)
};

Adafruit_USBD_HID usb_hid;

// String table the host pulls via Get_Report on indices above
const char* kManufacturer = "Pico USB";
const char* kProduct      = "RP2040 Virtual UPS";
const char* kSerial       = "UPS-0001";
const char* kChemistry    = "LiON";

// ---- Live battery simulation state ----
// HID Power Device BatteryStatus enum (0..6, see USB PDC spec).
enum : uint8_t {
  PDC_BS_DISCHARGING    = 0,
  PDC_BS_CHARGING       = 1,
  PDC_BS_FULLY_CHARGED  = 2,
};

struct BatteryState {
  uint8_t  capacity;        // 0..100 (percent)
  bool     ac_present;
  bool     battery_present;
  bool     charging;
  bool     fully_charged;
  bool     fully_discharged;
  bool     below_limit;
  uint8_t  status;          // PDC BatteryStatus enum
};

static BatteryState g_bat = {
  .capacity        = 100,
  .ac_present      = true,
  .battery_present = true,
  .charging        = false,
  .fully_charged   = true,
  .fully_discharged = false,
  .below_limit     = false,
  .status          = PDC_BS_FULLY_CHARGED,
};

static uint32_t g_state_change_ms = 0;   // millis() at last state change
static bool     g_unplugged       = false; // toggled by simulation

// How long to sit in each phase (ms). Tuned so a full demo loop is quick.
static const uint32_t kHoldFullMs       = 5000;   // hold at 100% then "unplug"
static const uint32_t kDrainStepMs      = 1000;   // tick every 1s while discharging
static const uint8_t  kDrainPercentStep = 2;      // 2% per tick (~50s for 100->0)
static const uint32_t kChargeStepMs     = 1000;
static const uint8_t  kChargePercentStep = 2;
static const uint32_t kHoldEmptyMs      = 5000;

// Advance the simulated battery one tick.
static void tick_battery(uint32_t now) {
  // Discharging: AC unplugged, battery draining.
  if (!g_bat.ac_present && g_bat.battery_present) {
    if (now - g_state_change_ms < kDrainStepMs) return;
    if (g_bat.capacity > 0) {
      g_bat.capacity = (g_bat.capacity > kDrainPercentStep)
                         ? (g_bat.capacity - kDrainPercentStep)
                         : 0;
    }
    g_bat.charging        = false;
    g_bat.status          = PDC_BS_DISCHARGING;
    g_bat.fully_charged   = false;
    g_bat.fully_discharged = (g_bat.capacity == 0);
    g_bat.below_limit     = (g_bat.capacity <= kRemainingLimit);

    if (g_bat.capacity == 0) {
      // Spent. Hold at empty briefly, then "plug in" to start charging.
      if (now - g_state_change_ms > kHoldEmptyMs + 30000) {
        // After ~30s at empty, plug AC back in.
        g_bat.ac_present   = true;
        g_bat.charging     = true;
        g_bat.status       = PDC_BS_CHARGING;
        g_state_change_ms  = now;
      }
      return;
    }
    g_state_change_ms = now;
    return;
  }

  // Charging: AC present, battery charging.
  if (g_bat.ac_present && g_bat.battery_present && !g_bat.fully_charged) {
    if (now - g_state_change_ms < kChargeStepMs) return;
    if (g_bat.capacity < 100) {
      g_bat.capacity = (g_bat.capacity + kChargePercentStep > 100)
                         ? 100
                         : g_bat.capacity + kChargePercentStep;
    }
    g_bat.charging        = true;
    g_bat.status          = PDC_BS_CHARGING;
    g_bat.fully_discharged = false;
    g_bat.below_limit     = (g_bat.capacity <= kRemainingLimit);

    if (g_bat.capacity >= 100) {
      g_bat.fully_charged = true;
      g_bat.charging      = false;
      g_bat.status        = PDC_BS_FULLY_CHARGED;
      g_state_change_ms   = now;
      return;
    }
    g_state_change_ms = now;
    return;
  }

  // Fully charged, on AC. After a hold period, simulate an AC outage.
  if (g_bat.fully_charged && g_bat.ac_present) {
    if (now - g_state_change_ms > kHoldFullMs) {
      g_bat.ac_present     = false;
      g_bat.charging       = false;
      g_bat.fully_charged  = false;
      g_bat.status         = PDC_BS_DISCHARGING;
      g_state_change_ms    = now;
    }
    return;
  }
}

// Respond to GET_REPORT (FEATURE & INPUT) from the host.
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
    case HID_RID_IMANUFACTURER:       return putStr(kManufacturer);
    case HID_RID_IPRODUCT:            return putStr(kProduct);
    case HID_RID_ISERIAL:             return putStr(kSerial);
    case HID_RID_IDEVICE_CHEM:        return putStr(kChemistry);

    case HID_RID_RECHARGEABLE:        buffer[0] = 1; return 1;
    case HID_RID_CAPACITY_MODE:       buffer[0] = 2; return 1; // 2 = percentage
    case HID_RID_WARNING_CAP_LIMIT:   buffer[0] = kWarningLimit; return 1;
    case HID_RID_REMAINING_CAP_LIMIT: buffer[0] = kRemainingLimit; return 1;

    case HID_RID_DESIGN_VOLTAGE:
      buffer[0] = kDesignVoltage_mV & 0xFF;
      buffer[1] = (kDesignVoltage_mV >> 8) & 0xFF;
      return 2;

    case HID_RID_CONFIG_VOLTAGE:
      buffer[0] = kConfigVoltage_mV & 0xFF;
      buffer[1] = (kConfigVoltage_mV >> 8) & 0xFF;
      return 2;

    case HID_RID_DESIGN_CAP:
    case HID_RID_FULL_CHARGE_CAP:
      buffer[0] = kDesignCapacity;
      return 1;

    case HID_RID_REMAINING_CAP:
      buffer[0] = g_bat.capacity;
      return 1;

    case HID_RID_RUNTIME_TO_EMPTY: {
      // While discharging, report seconds remaining as cap% * 60 (1% = 1 minute).
      // While on AC, report unknown (0xFFFF).
      if (!g_bat.ac_present && g_bat.capacity > 0 && !g_bat.fully_charged) {
        uint16_t secs = (uint16_t)g_bat.capacity * 60;
        buffer[0] = secs & 0xFF;
        buffer[1] = (secs >> 8) & 0xFF;
      } else {
        buffer[0] = 0xFF;
        buffer[1] = 0xFF;
      }
      return 2;
    }

    case HID_RID_PRESENT_STATUS: {
      // 16-bit field, little-endian. Bit assignments per the descriptor.
      uint16_t s = 0;
      if (g_bat.charging)         s |= (1u << 0);
      if (!g_bat.ac_present)      s |= (1u << 1); // Discharging
      if (g_bat.ac_present)       s |= (1u << 2); // ACPresent
      if (g_bat.battery_present)  s |= (1u << 3); // BatteryPresent
      if (g_bat.below_limit)      s |= (1u << 4);
      // bit 5 RemainingTimeLimitExpired: leave 0
      // bit 6 NeedReplacement: leave 0
      // bit 7 VoltageNotRegulated: leave 0
      if (g_bat.fully_charged)    s |= (1u << 8);
      if (g_bat.fully_discharged) s |= (1u << 9);
      // bits 10..13: ShutdownRequested, ShutdownImminent, CommunicationLost, Overload -> 0
      buffer[0] = s & 0xFF;
      buffer[1] = (s >> 8) & 0xFF;
      return 2;
    }

    case HID_RID_BATTERY_STATUS:
      buffer[0] = g_bat.status;
      return 1;
  }
  return 0;
}

// Host writes (SET_REPORT) — UPS has no writable features, ignore.
void set_report_cb(uint8_t, hid_report_type_t,
                   uint8_t const*, uint16_t) {}

void setup() {
  // Identify as a UPS at the USB descriptor level too.
  TinyUSBDevice.setManufacturerDescriptor(kManufacturer);
  TinyUSBDevice.setProductDescriptor(kProduct);
  TinyUSBDevice.setSerialDescriptor(kSerial);
  TinyUSBDevice.setID(0x239A, 0x80FE);   // Adafruit debug VID + sketch PID

  usb_hid.setPollInterval(10);
  usb_hid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
  usb_hid.setReportCallback(get_report_cb, set_report_cb);
  usb_hid.begin();

  while (!TinyUSBDevice.mounted()) delay(10);

  g_state_change_ms = millis();
}

void loop() {
  uint32_t now = millis();

  // Advance the simulated battery.
  tick_battery(now);

  // Push unsolicited PresentStatus + RemainingCapacity + BatteryStatus every 2s
  // so the OS UI updates promptly.
  static uint32_t last = 0;
  if (now - last > 2000 && usb_hid.ready()) {
    uint16_t status = 0;
    if (g_bat.charging)         status |= (1u << 0);
    if (!g_bat.ac_present)      status |= (1u << 1);
    if (g_bat.ac_present)       status |= (1u << 2);
    if (g_bat.battery_present)  status |= (1u << 3);
    if (g_bat.below_limit)      status |= (1u << 4);
    if (g_bat.fully_charged)    status |= (1u << 8);
    if (g_bat.fully_discharged) status |= (1u << 9);

    uint8_t ps[2] = { (uint8_t)(status & 0xFF), (uint8_t)(status >> 8) };
    usb_hid.sendReport(HID_RID_PRESENT_STATUS, ps, sizeof(ps));

    uint8_t cap = g_bat.capacity;
    usb_hid.sendReport(HID_RID_REMAINING_CAP, &cap, 1);

    uint8_t bs = g_bat.status;
    usb_hid.sendReport(HID_RID_BATTERY_STATUS, &bs, 1);

    last = now;
  }
}