// picoups.c — kernel module that exposes a fake battery and AC power_supply,
// fed from the RP2040 Virtual UPS over USB HID GET_REPORT.
//
// Strategy: register as a USB driver that matches the RP2040's CDC-ACM
// interface (interface 0). We don't actually use that interface for I/O —
// we just need to be bound to the device so we can issue a USB control
// transfer to endpoint 0 with the HID class request GET_REPORT. The
// endpoint 0 control endpoint is shared across all interfaces, so this
// works even though we don't own the HID interface.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/power_supply.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/usb/hcd.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("picoups");
MODULE_DESCRIPTION("RP2040 Virtual UPS battery/AC bridge (HID PDC -> power_supply)");

#define PICO_VID 0x239A
#define PICO_PID 0x80FE

/* ---- shared state ---- */
static struct power_supply *psy_battery;
static struct power_supply *psy_ac;

static int present_status;        /* byte from RID 1 */
static int remaining_capacity;    /* percent from RID 2 */
static int runtime_to_empty;      /* seconds from RID 3 (0xFFFF -> 0) */
static int battery_status;        /* HID PDC enum from RID 4 */
static bool ac_online;
static bool bat_present;
static bool bat_charging;
static bool bat_full;
static struct mutex state_lock;

static struct usb_device *pico_udev;     /* bound device, or NULL */
static struct task_struct *poll_thread;
static int poll_thread_should_stop;

/* ---- USB driver binding ---- */
/* Match our specific VID/PID. The probe is called for each interface, but
 * we only claim interface 2 (the HID one) by hand. CDC-ACM (interfaces 0,1)
 * continues to be owned by cdc_acm. */
static int picoups_probe(struct usb_interface *intf,
                         const struct usb_device_id *id);
static void picoups_disconnect(struct usb_interface *intf);

static const struct usb_device_id picoups_id_table[] = {
    { USB_DEVICE(PICO_VID, PICO_PID) },
    { },
};
MODULE_DEVICE_TABLE(usb, picoups_id_table);

static struct usb_driver picoups_driver = {
    .name       = "picoups",
    .id_table   = picoups_id_table,
    .probe      = picoups_probe,
    .disconnect = picoups_disconnect,
    .supports_autosuspend = 0,
};

/* ---- HID GET_REPORT over USB control endpoint ---- */
/* Returns the number of bytes read (including the Report ID echo at byte 0)
 * or a negative error. */
static int picoups_hid_get_report(struct usb_device *udev,
                                  unsigned int ifnum,  /* 2 = HID */
                                  u8 report_id,
                                  void *buf, u8 buflen)
{
    int ret;
    u8 *kbuf = kmalloc(buflen, GFP_KERNEL);
    if (!kbuf) return -ENOMEM;

    /* Standard USB control transfer to endpoint 0:
     *   requestType = 0xA1 (device-to-host, class, interface recipient)
     *   request     = 0x01 (GET_REPORT)
     *   value       = (report_type << 8) | report_id
     *                 report_type: 1 = Input, 2 = Output, 3 = Feature
     *   index       = interface number
     *   data        = buffer
     */
    ret = usb_control_msg(udev,
                          usb_rcvctrlpipe(udev, 0),
                          0x01,                /* GET_REPORT */
                          0xA1,                /* device-to-host | class | interface */
                          (3u << 8) | report_id,  /* report_type=Feature */
                          ifnum,
                          kbuf, buflen,
                          2000);               /* timeout ms */
    if (ret > 0)
        memcpy(buf, kbuf, ret);
    kfree(kbuf);
    return ret;
}

static int hid_poll_thread(void *data)
{
    int consecutive_failures = 0;
    bool last_present = false;

    while (!kthread_should_stop() && !poll_thread_should_stop) {
        bool got_data = false;

        if (pico_udev) {
            u8 buf[8];
            int n;

            mutex_lock(&state_lock);

            /* RID 1 PresentStatus */
            n = picoups_hid_get_report(pico_udev, 2, 1, buf, sizeof(buf));
            if (n >= 2) {
                present_status = buf[1];
                ac_online     = !!(buf[1] & (1 << 2));
                bat_present   = !!(buf[1] & (1 << 3));
                bat_full      = !!(buf[1] & (1 << 4));
                bat_charging  = !!(buf[1] & (1 << 0));
                consecutive_failures = 0;
                got_data = true;
            } else {
                consecutive_failures++;
            }

            if (got_data) {
                n = picoups_hid_get_report(pico_udev, 2, 2, buf, sizeof(buf));
                if (n >= 2) remaining_capacity = buf[1];
                n = picoups_hid_get_report(pico_udev, 2, 3, buf, sizeof(buf));
                if (n >= 3) {
                    u16 r = buf[1] | (buf[2] << 8);
                    runtime_to_empty = (r == 0xFFFF) ? 0 : r;
                }
                n = picoups_hid_get_report(pico_udev, 2, 4, buf, sizeof(buf));
                if (n >= 2) battery_status = buf[1];
            }

            if (consecutive_failures >= 3) {
                bat_present = false;
                ac_online = false;
            }
            mutex_unlock(&state_lock);
        } else {
            mutex_lock(&state_lock);
            bat_present = false;
            ac_online = false;
            mutex_unlock(&state_lock);
        }

        if (got_data && !last_present)
            pr_info("picoups: device online, battery present\n");
        if (!got_data && last_present)
            pr_info("picoups: device offline, battery -> not present\n");
        last_present = got_data;

        if (psy_battery) power_supply_changed(psy_battery);
        if (psy_ac) power_supply_changed(psy_ac);

        ssleep(2);
    }
    return 0;
}

static int picoups_probe(struct usb_interface *intf,
                         const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    unsigned int ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

    /* We only want the HID interface (2). CDC-ACM (0,1) stays with cdc_acm. */
    if (ifnum != 2) {
        pr_debug("picoups: ignoring interface %u (not HID)\n", ifnum);
        return -ENODEV;
    }

    if (pico_udev) {
        pr_warn("picoups: second UPS ignored\n");
        return -EBUSY;
    }

    pico_udev = usb_get_dev(udev);
    pr_info("picoups: bound to %04x:%04x HID if%u (bus %d dev %d)\n",
            le16_to_cpu(udev->descriptor.idVendor),
            le16_to_cpu(udev->descriptor.idProduct), ifnum,
            udev->bus->busnum, udev->devnum);
    return 0;   /* claim the HID interface */
}

static void picoups_disconnect(struct usb_interface *intf)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    if (pico_udev == udev) {
        usb_put_dev(pico_udev);
        pico_udev = NULL;
        pr_info("picoups: device gone\n");
    }
}

/* ---- power_supply callbacks ---- */
static int battery_get_property(struct power_supply *psy,
                                enum power_supply_property psp,
                                union power_supply_propval *val)
{
    mutex_lock(&state_lock);
    switch (psp) {
    case POWER_SUPPLY_PROP_STATUS:
        if (!bat_present)
            val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
        else if (battery_status == 2)
            val->intval = POWER_SUPPLY_STATUS_FULL;
        else if (bat_charging)
            val->intval = POWER_SUPPLY_STATUS_CHARGING;
        else if (!ac_online)
            val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
        else
            val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
        break;
    case POWER_SUPPLY_PROP_CAPACITY:
        val->intval = remaining_capacity;
        break;
    case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
        val->intval = runtime_to_empty;
        break;
    case POWER_SUPPLY_PROP_PRESENT:
        val->intval = bat_present ? 1 : 0;
        break;
    case POWER_SUPPLY_PROP_TECHNOLOGY:
        val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
        break;
    case POWER_SUPPLY_PROP_HEALTH:
        val->intval = POWER_SUPPLY_HEALTH_GOOD;
        break;
    case POWER_SUPPLY_PROP_MODEL_NAME:
        val->strval = "RP2040 Virtual UPS";
        break;
    case POWER_SUPPLY_PROP_MANUFACTURER:
        val->strval = "AnthropicLabs";
        break;
    case POWER_SUPPLY_PROP_SERIAL_NUMBER:
        val->strval = "UPS-0001";
        break;
    case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
        if (remaining_capacity >= 95) val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
        else if (remaining_capacity >= 70) val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
        else if (remaining_capacity >= 40) val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
        else if (remaining_capacity >= 15) val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
        else val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
        break;
    default:
        mutex_unlock(&state_lock);
        return -EINVAL;
    }
    mutex_unlock(&state_lock);
    return 0;
}

static int ac_get_property(struct power_supply *psy,
                            enum power_supply_property psp,
                            union power_supply_propval *val)
{
    mutex_lock(&state_lock);
    switch (psp) {
    case POWER_SUPPLY_PROP_ONLINE:
        val->intval = ac_online ? 1 : 0;
        break;
    case POWER_SUPPLY_PROP_MODEL_NAME:
        val->strval = "RP2040 Virtual UPS";
        break;
    case POWER_SUPPLY_PROP_MANUFACTURER:
        val->strval = "AnthropicLabs";
        break;
    default:
        mutex_unlock(&state_lock);
        return -EINVAL;
    }
    mutex_unlock(&state_lock);
    return 0;
}

static enum power_supply_property battery_props[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_CAPACITY,
    POWER_SUPPLY_PROP_CAPACITY_LEVEL,
    POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
    POWER_SUPPLY_PROP_PRESENT,
    POWER_SUPPLY_PROP_TECHNOLOGY,
    POWER_SUPPLY_PROP_HEALTH,
    POWER_SUPPLY_PROP_MODEL_NAME,
    POWER_SUPPLY_PROP_MANUFACTURER,
    POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static enum power_supply_property ac_props[] = {
    POWER_SUPPLY_PROP_ONLINE,
    POWER_SUPPLY_PROP_MODEL_NAME,
    POWER_SUPPLY_PROP_MANUFACTURER,
};

static const struct power_supply_desc battery_desc = {
    .name = "picoups-battery",
    .type = POWER_SUPPLY_TYPE_BATTERY,
    .properties = battery_props,
    .num_properties = ARRAY_SIZE(battery_props),
    .get_property = battery_get_property,
};

static const struct power_supply_desc ac_desc = {
    .name = "picoups-ac",
    .type = POWER_SUPPLY_TYPE_MAINS,
    .properties = ac_props,
    .num_properties = ARRAY_SIZE(ac_props),
    .get_property = ac_get_property,
};

static int __init picoups_init(void)
{
    int ret;

    mutex_init(&state_lock);
    present_status = 0;
    ac_online = false;
    bat_present = false;
    bat_charging = false;
    bat_full = false;
    remaining_capacity = 0;
    runtime_to_empty = 0;
    battery_status = 0;

    psy_battery = power_supply_register(NULL, &battery_desc, NULL);
    if (IS_ERR(psy_battery)) {
        ret = PTR_ERR(psy_battery);
        pr_err("picoups: failed to register battery: %d\n", ret);
        return ret;
    }

    psy_ac = power_supply_register(NULL, &ac_desc, NULL);
    if (IS_ERR(psy_ac)) {
        ret = PTR_ERR(psy_ac);
        pr_err("picoups: failed to register ac: %d\n", ret);
        power_supply_unregister(psy_battery);
        return ret;
    }

    poll_thread = kthread_run(hid_poll_thread, NULL, "picoups_poll");
    if (IS_ERR(poll_thread)) {
        ret = PTR_ERR(poll_thread);
        pr_warn("picoups: poll thread failed to start\n");
        poll_thread = NULL;
    }

    ret = usb_register(&picoups_driver);
    if (ret) {
        pr_err("picoups: usb_register failed: %d\n", ret);
    }

    pr_info("picoups: loaded\n");
    return 0;
}

static void __exit picoups_exit(void)
{
    poll_thread_should_stop = 1;
    if (poll_thread) kthread_stop(poll_thread);
    usb_deregister(&picoups_driver);
    if (psy_ac) power_supply_unregister(psy_ac);
    if (psy_battery) power_supply_unregister(psy_battery);
    pr_info("picoups: unloaded\n");
}

module_init(picoups_init);
module_exit(picoups_exit);
