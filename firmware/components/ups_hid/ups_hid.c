/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ups_hid.h"
#include "hid_parser.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include <math.h>
#include <string.h>

static const char *TAG = "ups_hid";

#define POLL_INTERVAL_MS      2000
#define FAILURES_BEFORE_RESET 5     /* ~10s of silence before a VBUS cycle */

/* Recovery backoff. A UPS that is simply unplugged must not have its VBUS
 * cycled every ten seconds forever, but one that may yet come back must not be
 * abandoned either. Start at 10s and double to a 5 minute ceiling, reset on the
 * first good poll. */
#define RECOVERY_BACKOFF_MIN_US (10LL * 1000 * 1000)
#define RECOVERY_BACKOFF_MAX_US (300LL * 1000 * 1000)

static ups_data_t       s_data;
static hid_report_map_t s_map;
static SemaphoreHandle_t s_lock;

/* Kept outside ups_data_t because ups_hid_recover() resets that wholesale. */
static int64_t s_next_recovery_us;
static int64_t s_recovery_backoff_us = RECOVERY_BACKOFF_MIN_US;

static void ups_data_reset(ups_data_t *d)
{
    memset(d, 0, sizeof(*d));
    d->battery_charge    = UPS_VALUE_ABSENT;
    d->battery_runtime   = UPS_VALUE_ABSENT;
    d->battery_voltage   = UPS_VALUE_ABSENT;
    d->input_voltage     = UPS_VALUE_ABSENT;
    d->input_frequency   = UPS_VALUE_ABSENT;
    d->output_voltage    = UPS_VALUE_ABSENT;
    d->ups_load          = UPS_VALUE_ABSENT;
    d->ups_realpower     = UPS_VALUE_ABSENT;
    d->ups_realpower_nom = UPS_VALUE_ABSENT;
    d->ups_apparentpower = UPS_VALUE_ABSENT;
    d->status            = UPS_STATUS_UNKNOWN;
    d->battery_mfr_date[0] = '\0';
}

/* Values are read with GET_REPORT(Feature) on each poll. Feature, not Input:
 * a UPS keeps the authoritative reading there, and an Input report of the same
 * usage only arrives when the device chooses to send one.
 *           esp_hid's host component is input-report oriented and will not do
 *           this for us -- issue the control transfers directly (CLAUDE.md). */

/* ---- USB host plumbing ------------------------------------------------ */

#define USB_CLIENT_EVENT_TIMEOUT_MS 100
#define CTRL_TIMEOUT_MS             1000
#define REPORT_DESC_MAX             2048

#define USB_CLASS_HID               0x03
#define HID_DESC_TYPE               0x21
#define HID_REPORT_DESC_TYPE        0x22
#define REQ_GET_DESCRIPTOR          0x06
#define REQ_GET_REPORT              0x01
#define REQ_SET_REPORT              0x09
#define REPORT_TYPE_FEATURE         0x03

static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;
static uint8_t                  s_dev_addr;
static bool                     s_dev_pending;
static uint8_t                  s_hid_iface = 0xFF;
static uint16_t                 s_report_desc_len;
static SemaphoreHandle_t        s_ctrl_done;
static uint8_t                  s_report_desc[REPORT_DESC_MAX];

/* Worked out at enumeration; defined with the M1 polling code below. */
static void build_poll_plan(void);

/* The control table lives with the write path further down, but enumeration
 * and the poll plan both come first and need to see it. Kept in step with the
 * table by the static assert beside it. */
#define CTRL_COUNT 4
static const hid_field_t *s_ctrl_field[CTRL_COUNT];
static uint32_t discover_caps(void);

/* ManufacturerDate, Battery System 0x85. Neither a reading nor a control, so
 * it gets its own slot: read once per poll like everything else, but decoded
 * into text rather than a float. */
static const hid_field_t *s_date_field;

/* A command from another task, executed by the polling task. Declared here
 * because the poll loop and ups_hid_start are both above the definitions. */
typedef struct {
    ups_cmd_t cmd;
    int32_t   arg;
    /* Not a UPS command: a board-level VBUS power cycle, queued here so it
     * executes in ups_task like everything else that touches the report map. */
    bool      recover;
} cmd_req_t;
static QueueHandle_t s_cmd_q;
static QueueHandle_t s_cmd_done;
static esp_err_t execute_cmd(const cmd_req_t *r);
static esp_err_t do_recover(void);

static void ctrl_cb(usb_transfer_t *xfer)
{
    xSemaphoreGive((SemaphoreHandle_t)xfer->context);
}

/* Blocking control OUT transfer: same shape as ctrl_in, with the payload
 * copied in after the setup packet instead of read out of it. Returns true if
 * the device ACKed. */
static bool ctrl_out(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                     uint16_t wIndex, const uint8_t *data, uint16_t len)
{
    usb_transfer_t *xfer = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + len, 0, &xfer) != ESP_OK) {
        return false;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest      = bRequest;
    setup->wValue        = wValue;
    setup->wIndex        = wIndex;
    setup->wLength       = len;
    if (len) memcpy(xfer->data_buffer + sizeof(usb_setup_packet_t), data, len);

    xfer->device_handle    = s_dev;
    xfer->bEndpointAddress = 0;
    xfer->num_bytes        = sizeof(usb_setup_packet_t) + len;
    xfer->callback         = ctrl_cb;
    xfer->context          = s_ctrl_done;

    bool done = false;
    if (usb_host_transfer_submit_control(s_client, xfer) == ESP_OK) {
        /* Same reason as ctrl_in: this task owns the event pump, so it has to
         * keep pumping or the completion callback can never run. */
        for (int waited = 0; waited < CTRL_TIMEOUT_MS; waited += 10) {
            usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
            if (xSemaphoreTake(s_ctrl_done, 0) == pdTRUE) { done = true; break; }
        }
    }
    bool ok = done && xfer->status == USB_TRANSFER_STATUS_COMPLETED;
    usb_host_transfer_free(xfer);
    return ok;
}

/* Blocking control IN transfer. Returns bytes of payload, or -1. */
static int ctrl_in(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                   uint16_t wIndex, uint16_t wLength, uint8_t *out)
{
    usb_transfer_t *xfer = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + wLength, 0, &xfer) != ESP_OK) {
        return -1;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest      = bRequest;
    setup->wValue        = wValue;
    setup->wIndex        = wIndex;
    setup->wLength       = wLength;

    xfer->device_handle    = s_dev;
    xfer->bEndpointAddress = 0;
    xfer->num_bytes        = sizeof(usb_setup_packet_t) + wLength;
    xfer->callback         = ctrl_cb;
    xfer->context          = s_ctrl_done;

    int got = -1;
    bool done = false;
    if (usb_host_transfer_submit_control(s_client, xfer) == ESP_OK) {
        /* Transfer callbacks are delivered from usb_host_client_handle_events,
         * which this same task owns. Blocking on the semaphore without pumping
         * events deadlocks: the callback that would give it can never run. So
         * pump while waiting. */
        for (int waited = 0; waited < CTRL_TIMEOUT_MS; waited += 10) {
            usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
            if (xSemaphoreTake(s_ctrl_done, 0) == pdTRUE) { done = true; break; }
        }
    }
    if (done && xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        got = xfer->actual_num_bytes - sizeof(usb_setup_packet_t);
        if (got < 0) got = 0;
        if (got > wLength) got = wLength;
        if (out && got > 0) memcpy(out, xfer->data_buffer + sizeof(usb_setup_packet_t), got);
    }
    usb_host_transfer_free(xfer);
    return got;
}

/* Set by client_event_cb, acted on by ups_task at a point where no control
 * transfer is in flight. See the DEV_GONE case for why the teardown cannot
 * happen in the callback. */
static volatile bool s_dev_gone;

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_dev_addr = msg->new_dev.address;
        s_dev_pending = true;
        ESP_LOGI(TAG, "USB device attached at address %u", s_dev_addr);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        /* Heap is logged on every attach and detach so a leak across
         * plug cycles is VISIBLE. Repeated enumeration is where USB stacks
         * leak handles, and it will not show up as a failure for hundreds of
         * cycles -- by which time it is a field fault, not a bench one.
         * CLAUDE.md requires this board to survive replugging indefinitely. */
        ESP_LOGW(TAG, "USB device disconnected (heap %u free, %u low-water)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
        /* FLAG ONLY. Do not release the interface or close the device here.
         *
         * This callback is dispatched by usb_host_client_handle_events(), and
         * ctrl_in()/ctrl_out() have to CALL that while waiting for a control
         * transfer to complete -- this task owns the event pump, so blocking
         * without pumping would deadlock. The consequence is that a device
         * disconnecting mid-poll runs this callback from INSIDE the transfer
         * wait, and the old code then released the interface and closed the
         * device handle while that transfer was still submitted against it,
         * after which ctrl_in() freed the transfer the stack still owned.
         *
         * Unplugging a UPS during a poll is not an exotic case; it is the
         * documented requirement in CLAUDE.md that this board survive being
         * unplugged mid-session. The attach path already had this right --
         * USB_HOST_CLIENT_EVENT_NEW_DEV only sets s_dev_pending and lets
         * ups_task open the device at a safe point. Detach now matches. */
        s_dev_gone = true;
        break;
    default:
        break;
    }
}

/* A UPS that has stopped answering must stop reporting numbers.
 *
 * An unplug clears everything through ups_data_reset(), but a UPS that is
 * still enumerated and has simply gone deaf -- the exact failure the VBUS
 * recovery exists for -- took a different path: `attached` went false and the
 * last good readings stayed behind it. The web UI then showed "UPS not
 * detected" next to a 100% battery and 120 V input, and NUT would have served
 * the same numbers to a client deciding whether to shut a NAS down.
 *
 * Identity is deliberately kept. Knowing WHICH UPS stopped answering is useful
 * and cannot be stale in any way that matters; a voltage can. */
static void invalidate_readings(ups_data_t *d)
{
    d->battery_charge    = UPS_VALUE_ABSENT;
    d->battery_runtime   = UPS_VALUE_ABSENT;
    d->battery_voltage   = UPS_VALUE_ABSENT;
    d->input_voltage     = UPS_VALUE_ABSENT;
    d->input_frequency   = UPS_VALUE_ABSENT;
    d->output_voltage    = UPS_VALUE_ABSENT;
    d->ups_load          = UPS_VALUE_ABSENT;
    d->ups_realpower     = UPS_VALUE_ABSENT;
    d->ups_realpower_nom = UPS_VALUE_ABSENT;
    d->ups_apparentpower = UPS_VALUE_ABSENT;
    d->status            = UPS_STATUS_UNKNOWN;
}

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* Walk the configuration descriptor for a HID interface and its report
 * descriptor length. Generic: matches on class, never on VID/PID. */
static bool find_hid_interface(const usb_config_desc_t *cfg)
{
    int offset = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    bool in_hid_iface = false;

    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            in_hid_iface = (i->bInterfaceClass == USB_CLASS_HID);
            if (in_hid_iface) s_hid_iface = i->bInterfaceNumber;
        } else if (in_hid_iface && d->bDescriptorType == HID_DESC_TYPE) {
            /* HID descriptor: wDescriptorLength is at offset 7. */
            const uint8_t *raw = (const uint8_t *)d;
            if (d->bLength >= 9) {
                s_report_desc_len = (uint16_t)(raw[7] | (raw[8] << 8));
                return true;
            }
        }
    }
    return false;
}

static void read_string(uint8_t index, char *out, size_t cap)
{
    out[0] = '\0';
    if (!index) return;
    uint8_t buf[128];
    /* 0x0409 = US English. */
    int n = ctrl_in(0x80, REQ_GET_DESCRIPTOR, (uint16_t)(0x0300 | index), 0x0409,
                    sizeof(buf), buf);
    if (n < 2) return;

    /* bLength is authoritative, NOT the transfer length. We asked for 128
     * bytes; what comes back past the descriptor's own length is padding or
     * whatever was in the buffer, and decoding it produced "EC850LCD???" from
     * a CyberPower EC850LCD -- three characters of junk that then travelled
     * out through NUT's ups.model to every client. */
    size_t len = buf[0];
    if (len > (size_t)n) len = (size_t)n;

    size_t o = 0;
    for (size_t i = 2; i + 1 < len && o + 1 < cap; i += 2) {
        uint16_t ch = buf[i] | (buf[i + 1] << 8);
        /* Anything outside printable ASCII is DROPPED rather than replaced
         * with '?'. These strings end up in ups.model and on the status page,
         * where a client shows them verbatim, so a shorter honest string beats
         * a padded one. Non-Latin names are the cost, and no UPS ships one. */
        if (ch >= 0x20 && ch < 0x7F) out[o++] = (char)ch;
    }
    out[o] = '\0';
    while (o && out[o - 1] == ' ') out[--o] = '\0';   /* APC pads with spaces */
}

/* Open the attached device, claim its HID interface, fetch and parse the
 * report descriptor. Returns true when the device is ready to poll. */
static bool enumerate(void)
{
    if (usb_host_device_open(s_client, s_dev_addr, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "device_open failed");
        return false;
    }

    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(s_dev, &dd) != ESP_OK) {
        ESP_LOGE(TAG, "get_device_desc failed");
        return false;
    }

    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "get_config_desc failed");
        return false;
    }

    if (!find_hid_interface(cfg)) {
        ESP_LOGE(TAG, "no HID interface on %04x:%04x -- not a HID Power Device?",
                 dd->idVendor, dd->idProduct);
        return false;
    }

    if (usb_host_interface_claim(s_client, s_dev, s_hid_iface, 0) != ESP_OK) {
        ESP_LOGE(TAG, "interface_claim(%u) failed", s_hid_iface);
        return false;
    }

    ups_data_t local;
    ups_data_reset(&local);
    local.vid = dd->idVendor;
    local.pid = dd->idProduct;
    read_string(dd->iManufacturer, local.mfr, sizeof(local.mfr));
    read_string(dd->iProduct, local.model, sizeof(local.model));
    read_string(dd->iSerialNumber, local.serial, sizeof(local.serial));

    ESP_LOGI(TAG, "%04x:%04x  %s %s  serial %s",
             local.vid, local.pid, local.mfr, local.model, local.serial);
    ESP_LOGI(TAG, "heap %u free, %u low-water",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "HID interface %u, report descriptor %u bytes",
             s_hid_iface, s_report_desc_len);

    uint16_t want = s_report_desc_len;
    if (want > REPORT_DESC_MAX) {
        ESP_LOGE(TAG, "report descriptor %u bytes exceeds buffer %d",
                 want, REPORT_DESC_MAX);
        return false;
    }

    /* GET_DESCRIPTOR(Report) on the interface. */
    int n = ctrl_in(0x81, REQ_GET_DESCRIPTOR,
                    (uint16_t)(HID_REPORT_DESC_TYPE << 8), s_hid_iface,
                    want, s_report_desc);
    if (n != (int)want) {
        ESP_LOGE(TAG, "report descriptor read returned %d, wanted %u", n, want);
        return false;
    }

    /* Dump it so it can be diffed byte-for-byte against a host capture. */
    ESP_LOGI(TAG, "---- BEGIN REPORT DESCRIPTOR (%d bytes) ----", n);
    for (int i = 0; i < n; i += 16) {
        char line[64];
        int o = 0;
        for (int j = i; j < i + 16 && j < n; j++) {
            o += snprintf(line + o, sizeof(line) - o, "%02x", s_report_desc[j]);
        }
        ESP_LOGI(TAG, "%s", line);
    }
    ESP_LOGI(TAG, "---- END REPORT DESCRIPTOR ----");

    hid_parse_report_descriptor(s_report_desc, n, &s_map);
    hid_apply_quirks(&s_map, local.vid, local.pid);
    /* Caps first: build_poll_plan() adds the control reports to the polling
     * set, and it can only do that once discover_caps() has resolved them.
     * The other way round the controls are found but never read back, so the
     * page shows a beeper state of zero for a UPS that answers perfectly. */
    local.caps = discover_caps();
    build_poll_plan();

    local.attached = true;
    local.last_update_us = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = local;
    xSemaphoreGive(s_lock);
    return true;
}

/* ---- M1: reading actual values ---------------------------------------- */

/* Which parsed usage feeds which member of ups_data_t.
 *
 * Collection-qualified entries must come first: Voltage means input, output or
 * battery voltage depending on the collection it sits in, and a real APC
 * reports it in all three. The lookup below takes the first entry that matches,
 * so order is behaviour, not taste. */
#define PWR HID_PAGE_POWER_DEVICE
#define BAT HID_PAGE_BATTERY_SYSTEM

typedef struct {
    uint16_t page, usage, collection;
    size_t   offset;            /* offsetof() into ups_data_t */
} value_bind_t;

static const value_bind_t s_values[] = {
    { PWR, 0x30, HID_COLL_INPUT,   offsetof(ups_data_t, input_voltage)      },
    { PWR, 0x30, HID_COLL_OUTPUT,  offsetof(ups_data_t, output_voltage)     },
    { PWR, 0x30, HID_COLL_BATTERY, offsetof(ups_data_t, battery_voltage)    },
    { PWR, 0x32, HID_COLL_INPUT,   offsetof(ups_data_t, input_frequency)    },
    { BAT, 0x66, HID_COLL_NONE,    offsetof(ups_data_t, battery_charge)     },
    { BAT, 0x68, HID_COLL_NONE,    offsetof(ups_data_t, battery_runtime)    },
    { PWR, 0x35, HID_COLL_NONE,    offsetof(ups_data_t, ups_load)           },
    { PWR, 0x34, HID_COLL_NONE,    offsetof(ups_data_t, ups_realpower)      },
    { PWR, 0x44, HID_COLL_NONE,    offsetof(ups_data_t, ups_realpower_nom)  },
    { PWR, 0x33, HID_COLL_NONE,    offsetof(ups_data_t, ups_apparentpower)  },
};
#define VALUE_COUNT (sizeof(s_values) / sizeof(s_values[0]))

/* Status flags. `invert` covers ACPresent, which is TRUE when the UPS is on
 * line power -- the opposite sense to every other flag here. Getting that
 * backwards reports OB to Home Assistant while the mains is perfectly fine,
 * which is worse than reporting nothing. */
typedef struct {
    uint16_t page, usage;
    uint32_t bit;
    bool     invert;
} status_bind_t;

static const status_bind_t s_flags[] = {
    { BAT, 0xD0, UPS_STATUS_ONLINE,       false },  /* ACPresent            */
    { BAT, 0xD0, UPS_STATUS_ONBATT,       true  },  /* ...and its negation  */
    { BAT, 0x44, UPS_STATUS_CHARGING,     false },
    { BAT, 0x45, UPS_STATUS_DISCHARGE,    false },
    { BAT, 0x42, UPS_STATUS_LOWBATT,      false },  /* BelowRemainingCapLimit */
    { BAT, 0x4B, UPS_STATUS_REPLACEBATT,  false },
    { PWR, 0x65, UPS_STATUS_OVERLOAD,     false },
};
#define FLAG_COUNT (sizeof(s_flags) / sizeof(s_flags[0]))

/* The set of Feature reports worth fetching, worked out once at enumeration so
 * the poll loop does no searching. A UPS puts its values across many reports --
 * the captured APC spreads them over 44 IDs -- and fetching all of them every
 * two seconds would be pointless traffic. */
/* Nine reports carry the readings on the bench APC, and the four controls
 * add up to four more. */
#define MAX_POLL_REPORTS 16
static uint8_t s_poll_rid[MAX_POLL_REPORTS];
static uint8_t s_poll_bytes[MAX_POLL_REPORTS];
static size_t  s_poll_count;

static void note_report(const hid_field_t *f)
{
    if (!f || f->item_type != HID_ITEM_FEATURE) return;
    uint16_t need = (uint16_t)((f->bit_offset + f->bit_size + 7) / 8);
    for (size_t i = 0; i < s_poll_count; i++) {
        if (s_poll_rid[i] == f->report_id) {
            if (need > s_poll_bytes[i]) s_poll_bytes[i] = (uint8_t)need;
            return;
        }
    }
    if (s_poll_count >= MAX_POLL_REPORTS) return;
    s_poll_rid[s_poll_count]   = f->report_id;
    s_poll_bytes[s_poll_count] = (uint8_t)need;
    s_poll_count++;
}

/* Prefer a Feature report: that is where a UPS keeps the authoritative value.
 * An Input report of the same usage is the same datum but only arrives when the
 * device feels like sending it. */
static const hid_field_t *pick(uint16_t page, uint16_t usage, uint16_t coll)
{
    const hid_field_t *f = (coll == HID_COLL_NONE)
                         ? hid_find_typed(&s_map, page, usage, HID_ITEM_FEATURE)
                         : hid_find_in(&s_map, page, usage, coll);
    if (f && f->item_type != HID_ITEM_FEATURE) return NULL;
    return f;
}

static void build_poll_plan(void)
{
    s_poll_count = 0;
    for (size_t i = 0; i < VALUE_COUNT; i++) {
        note_report(pick(s_values[i].page, s_values[i].usage,
                         s_values[i].collection));
    }
    for (size_t i = 0; i < FLAG_COUNT; i++) {
        note_report(pick(s_flags[i].page, s_flags[i].usage, HID_COLL_NONE));
    }
    /* Controls are polled too, so the page shows what the UPS believes rather
     * than what we last asked it for. A beeper muted from the front panel
     * should show as muted here. */
    for (size_t i = 0; i < CTRL_COUNT; i++) {
        note_report(s_ctrl_field[i]);
    }
    note_report(s_date_field);

    char list[96];
    int o = 0;
    for (size_t i = 0; i < s_poll_count && o < (int)sizeof(list) - 8; i++) {
        o += snprintf(list + o, sizeof(list) - o, "%s%u",
                      i ? "," : "", s_poll_rid[i]);
    }
    ESP_LOGI(TAG, "polling %u feature report(s): %s",
             (unsigned)s_poll_count, s_poll_count ? list : "(none)");
}

/* GET_REPORT(Feature, id). Returns payload bytes with the report ID stripped,
 * which is the form hid_extract expects. */
static int get_feature(uint8_t report_id, uint8_t *out, uint8_t want)
{
    uint8_t buf[64];
    uint16_t len = (uint16_t)want + 1;          /* +1 for the leading report ID */
    if (len > sizeof(buf)) len = sizeof(buf);

    int n = ctrl_in(0xA1, REQ_GET_REPORT,
                    (uint16_t)((REPORT_TYPE_FEATURE << 8) | report_id),
                    s_hid_iface, len, buf);
    if (n <= 1) return -1;

    /* The device echoes the report ID as byte 0 when report IDs are in use.
     * Everything the descriptor said about bit offsets is relative to what
     * follows it. */
    if (buf[0] != report_id) return -1;
    int payload = n - 1;
    if (payload > want) payload = want;
    memcpy(out, buf + 1, payload);
    return payload;
}

/* The writable side of the descriptor.
 *
 * Everything here is discovered, never assumed. A UPS with no Test usage gets
 * no test button, and the web UI is driven from these bits rather than from a
 * model name -- the rule the whole HID path follows (CLAUDE.md: the parsing
 * must stay generic rather than hardcoding this model).
 */
typedef struct {
    uint16_t page, usage;
    uint32_t cap;
    const char *what;
} ctrl_bind_t;

static const ctrl_bind_t s_ctrls[] = {
    { HID_PAGE_POWER_DEVICE,   0x58, UPS_CAP_TEST,     "self-test" },
    { HID_PAGE_POWER_DEVICE,   0x5A, UPS_CAP_BEEPER,   "beeper" },
    { HID_PAGE_BATTERY_SYSTEM, 0x29, UPS_CAP_LOWBATT,  "low-battery limit" },
    { HID_PAGE_POWER_DEVICE,   0x57, UPS_CAP_SHUTDOWN, "delayed load shutdown" },
};
_Static_assert(sizeof(s_ctrls) / sizeof(s_ctrls[0]) == CTRL_COUNT,
               "CTRL_COUNT is declared at the top of this file and must match "
               "the table here");

/* s_ctrl_field is resolved once per attach, so a command never re-walks the
 * descriptor. */
/* The HID packed date: bits 0-4 day, 5-8 month, 9-15 year since 1980. A zero
 * field means the UPS has the usage but no value in it, which is common. */
static void decode_mfr_date(uint32_t raw, char *out, size_t cap)
{
    raw &= 0xFFFF;                       /* the field is 16 bits; bound it so
                                          * the year below cannot run away */
    unsigned day = raw & 0x1F, month = (raw >> 5) & 0x0F, year = 1980 + (raw >> 9);
    if (!raw || !day || !month || month > 12 || day > 31 || year > 2107) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%04u-%02u-%02u", year, month, day);
}

static uint32_t discover_caps(void)
{
    uint32_t caps = 0;
    s_date_field = pick(HID_PAGE_BATTERY_SYSTEM, 0x85, HID_COLL_NONE);
    if (s_date_field) caps |= UPS_CAP_BATTDATE;
    char list[128];
    int o = 0;
    for (size_t i = 0; i < CTRL_COUNT; i++) {
        s_ctrl_field[i] = pick(s_ctrls[i].page, s_ctrls[i].usage, HID_COLL_NONE);
        if (!s_ctrl_field[i]) continue;
        caps |= s_ctrls[i].cap;
        o += snprintf(list + o, sizeof(list) - o, "%s%s",
                      o ? ", " : "", s_ctrls[i].what);
    }
    ESP_LOGI(TAG, "controls this UPS supports: %s", o ? list : "(none)");
    return caps;
}

static const hid_field_t *ctrl_for(uint32_t cap)
{
    for (size_t i = 0; i < CTRL_COUNT; i++) {
        if (s_ctrls[i].cap == cap) return s_ctrl_field[i];
    }
    return NULL;
}

/* SET_REPORT(Feature, id), read-modify-write.
 *
 * A report can carry several fields -- on this APC report 21 holds
 * DelayBeforeShutdown alongside its neighbours -- so writing a freshly zeroed
 * buffer would clobber them. Reading first costs one extra control transfer
 * and means a beeper command cannot set a shutdown timer by accident. */
static bool set_field(const hid_field_t *f, uint32_t value)
{
    if (!f || f->item_type != HID_ITEM_FEATURE) return false;

    size_t bytes = (size_t)((f->bit_offset + f->bit_size + 7) / 8);
    if (bytes == 0 || bytes > 32) return false;

    uint8_t buf[33] = {0};
    int got = get_feature(f->report_id, buf + 1, (uint8_t)bytes);
    if (got < (int)bytes) {
        ESP_LOGW(TAG, "report %u did not read back, refusing to write it",
                 f->report_id);
        return false;
    }

    for (uint16_t b = 0; b < f->bit_size; b++) {
        uint16_t bit = f->bit_offset + b;
        uint8_t  mask = (uint8_t)(1u << (bit % 8));
        if (value & (1u << b)) buf[1 + bit / 8] |= mask;
        else                   buf[1 + bit / 8] &= (uint8_t)~mask;
    }

    buf[0] = f->report_id;          /* echoed back, as GET_REPORT returns it */
    return ctrl_out(0x21, REQ_SET_REPORT,
                    (uint16_t)((REPORT_TYPE_FEATURE << 8) | f->report_id),
                    s_hid_iface, buf, (uint16_t)(bytes + 1));
}

/* Fetch every planned report and fill `d`. Returns true if at least one report
 * came back -- a UPS that answers nothing at all is the wedged case the VBUS
 * recovery exists for. */
static bool poll_values(ups_data_t *d)
{
    uint8_t payload[MAX_POLL_REPORTS][32];
    int     got[MAX_POLL_REPORTS];
    bool    any = false;

    for (size_t i = 0; i < s_poll_count; i++) {
        uint8_t want = s_poll_bytes[i];
        if (want > sizeof(payload[0])) want = sizeof(payload[0]);
        got[i] = get_feature(s_poll_rid[i], payload[i], want);
        if (got[i] > 0) any = true;
    }
    if (!any) return false;

    for (size_t v = 0; v < VALUE_COUNT; v++) {
        const hid_field_t *f = pick(s_values[v].page, s_values[v].usage,
                                    s_values[v].collection);
        if (!f) continue;
        for (size_t i = 0; i < s_poll_count; i++) {
            if (s_poll_rid[i] != f->report_id || got[i] <= 0) continue;
            *(float *)((char *)d + s_values[v].offset) =
                hid_extract(f, payload[i], (size_t)got[i]);
            break;
        }
    }

    uint32_t status = 0;
    bool saw_flag = false;
    for (size_t k = 0; k < FLAG_COUNT; k++) {
        const hid_field_t *f = pick(s_flags[k].page, s_flags[k].usage,
                                    HID_COLL_NONE);
        if (!f) continue;
        for (size_t i = 0; i < s_poll_count; i++) {
            if (s_poll_rid[i] != f->report_id || got[i] <= 0) continue;
            bool on = hid_extract(f, payload[i], (size_t)got[i]) != 0.0f;
            if (s_flags[k].invert) on = !on;
            if (on) status |= s_flags[k].bit;
            saw_flag = true;
            break;
        }
    }
    if (saw_flag) d->status = status;

    if (s_date_field) {
        for (size_t i = 0; i < s_poll_count; i++) {
            if (s_poll_rid[i] != s_date_field->report_id || got[i] <= 0) continue;
            decode_mfr_date((uint32_t)hid_extract(s_date_field, payload[i], (size_t)got[i]),
                            d->battery_mfr_date, sizeof(d->battery_mfr_date));
            break;
        }
    }

    for (size_t c = 0; c < CTRL_COUNT; c++) {
        const hid_field_t *f = s_ctrl_field[c];
        if (!f) continue;
        for (size_t i = 0; i < s_poll_count; i++) {
            if (s_poll_rid[i] != f->report_id || got[i] <= 0) continue;
            float v = hid_extract(f, payload[i], (size_t)got[i]);
            switch (s_ctrls[c].cap) {
            case UPS_CAP_TEST:    d->test_result   = (uint8_t)v; break;
            case UPS_CAP_BEEPER:  d->beeper        = (uint8_t)v; break;
            case UPS_CAP_LOWBATT: d->lowbatt_limit = v;          break;
            default: break;   /* the shutdown timer is not worth showing */
            }
            break;
        }
    }

    return true;
}

/* One line describing what the UPS is actually saying.
 *
 * Logged when a reading changes and as a heartbeat otherwise, rather than on
 * every poll: at a 2s interval a per-poll line is 43,000 lines a day of mostly
 * nothing, and the one line that mattered is the one nobody scrolled back far
 * enough to find. Absent values print as "-" -- a UPS not reporting a usage is
 * normal, and printing 0 for it would be a lie. */
static void log_readings(const ups_data_t *d)
{
    static ups_data_t prev;
    static int64_t    last_us;
    static bool       first = true;

    bool changed = first ||
        d->status != prev.status ||
        fabsf(d->battery_charge  - prev.battery_charge)  >= 1.0f ||
        fabsf(d->battery_runtime - prev.battery_runtime) >= 60.0f ||
        /* 3V: measured mains wanders 120<->121 all day and at 1V that logged
         * on nearly every poll. A transfer to battery shows up in the status
         * bits, which always log, so nothing important is lost by being deaf
         * to normal supply wander. */
        fabsf(d->input_voltage   - prev.input_voltage)   >= 3.0f ||
        /* 0.5V, not 0.1: a float charger ripples ~0.3V and at 0.1 this logged
         * on every single poll -- the exact spam this function exists to
         * avoid. A discharging battery moves far more than 0.5V. */
        fabsf(d->battery_voltage - prev.battery_voltage) >= 0.5f ||
        fabsf(d->ups_load        - prev.ups_load)        >= 1.0f;

    int64_t now = esp_timer_get_time();
    if (!changed && now - last_us < 30 * 1000000LL) return;
    first = false;
    prev = *d;
    last_us = now;

    char st[48];
    int o = 0;
    #define FLAG(bit, name) \
        if (d->status & (bit)) o += snprintf(st + o, sizeof(st) - o, \
                                             "%s" name, o ? "," : "")
    FLAG(UPS_STATUS_ONLINE,       "OL");
    FLAG(UPS_STATUS_ONBATT,       "OB");
    FLAG(UPS_STATUS_LOWBATT,      "LB");
    FLAG(UPS_STATUS_CHARGING,     "CHRG");
    FLAG(UPS_STATUS_DISCHARGE,    "DISCHRG");
    FLAG(UPS_STATUS_REPLACEBATT,  "RB");
    FLAG(UPS_STATUS_OVERLOAD,     "OVER");
    #undef FLAG
    if (!o) snprintf(st, sizeof(st), "(none)");

    bool est = false;
    float w = ups_power_watts(d, &est);

    char chg[12], rt[12], iv[12], bv[12], ld[12], pw[16];
    #define FMT(buf, val, spec) \
        do { if (ups_valid(val)) snprintf(buf, sizeof(buf), spec, (double)(val)); \
             else snprintf(buf, sizeof(buf), "-"); } while (0)
    FMT(chg, d->battery_charge,  "%.0f%%");
    FMT(rt,  d->battery_runtime, "%.0fs");
    FMT(iv,  d->input_voltage,   "%.1fV");
    FMT(bv,  d->battery_voltage, "%.2fV");
    FMT(ld,  d->ups_load,        "%.0f%%");
    #undef FMT
    if (ups_valid(w)) snprintf(pw, sizeof(pw), "%.0fW%s", (double)w, est ? "~" : "");
    else              snprintf(pw, sizeof(pw), "-");

    ESP_LOGI(TAG, "batt %s %s %s | in %s | load %s %s | %s",
             chg, rt, bv, iv, ld, pw, st);
}

static void ups_task(void *arg)
{
    /* Enumeration is deferred until here so the UPS gets a cold start with a
     * driver already listening. On boards without a load switch this is a
     * no-op and the UPS is powered however the board wires VBUS. */
    board_vbus_set(true);
    vTaskDelay(pdMS_TO_TICKS(500));   /* let VBUS settle before enumerating */

    int64_t next_poll_us = 0;

    for (;;) {
        /* Drives client_event_cb; must be called continuously. */
        usb_host_client_handle_events(s_client,
                                      pdMS_TO_TICKS(USB_CLIENT_EVENT_TIMEOUT_MS));

        cmd_req_t req;
        while (s_cmd_q && xQueueReceive(s_cmd_q, &req, 0) == pdTRUE) {
            esp_err_t res = req.recover ? do_recover()
                          : s_dev        ? execute_cmd(&req)
                                         : ESP_ERR_INVALID_STATE;
            xQueueSend(s_cmd_done, &res, 0);
        }

        /* Teardown for a device that vanished, done here rather than in the
         * event callback so nothing is freed underneath an in-flight transfer.
         * Ordered before the attach check so an unplug/replug faster than one
         * loop iteration still tears down before it builds up. */
        if (s_dev_gone) {
            s_dev_gone = false;
            if (s_dev) {
                usb_host_interface_release(s_client, s_dev, s_hid_iface);
                usb_host_device_close(s_client, s_dev);
                s_dev = NULL;
                s_hid_iface = 0xFF;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            ups_data_reset(&s_data);
            xSemaphoreGive(s_lock);
            /* The map and everything derived from it, for the same reason as
             * do_recover(): the next device may have a different descriptor. */
            memset(&s_map, 0, sizeof(s_map));
            s_poll_count = 0;
            s_date_field = NULL;
        }

        if (s_dev_pending && !s_dev) {
            s_dev_pending = false;
            if (!enumerate()) {
                if (s_dev) {
                    usb_host_device_close(s_client, s_dev);
                    s_dev = NULL;
                }
            }
        }

        int64_t now = esp_timer_get_time();
        if (now < next_poll_us) continue;
        next_poll_us = now + (int64_t)POLL_INTERVAL_MS * 1000;

        bool ok = false;
        /* An EMPTY PORT is not a failing UPS, and the difference decides
         * whether the recovery machinery below runs at all.
         *
         * Without this the counter climbed on every poll with nothing plugged
         * in, crossed FAILURES_BEFORE_RESET within seconds, and left the
         * device permanently "recovering": on the devkit an error logged
         * forever, and on Rev A a load switch cycling VBUS on an empty
         * connector every few minutes for as long as it is powered. A board
         * with no UPS attached should sit quietly and wait for one. */
        const bool have_dev = (s_dev != NULL);

        ups_data_t fresh;
        if (have_dev && s_poll_count) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            fresh = s_data;                 /* keep identity and link state */
            xSemaphoreGive(s_lock);
            ok = poll_values(&fresh);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!have_dev) {
            /* Nothing to talk to. Hold the counters at rest so an empty port
             * accumulates no fault and the recovery below stays idle; the
             * disconnect teardown has already invalidated the readings. */
            s_data.poll_failures = 0;
        } else if (ok) {
            /* Copy readings back; identity and counters stay authoritative
             * here, not in the snapshot the poll worked on. */
            fresh.attached       = true;
            fresh.poll_failures  = 0;
            fresh.last_update_us = esp_timer_get_time();
            s_data = fresh;
            log_readings(&fresh);
        } else {
            s_data.poll_failures++;
            if (s_data.poll_failures >= FAILURES_BEFORE_RESET) {
                if (s_data.attached) {
                    ESP_LOGW(TAG, "%u consecutive poll failures, dropping the "
                                  "readings rather than serving stale ones",
                             (unsigned)s_data.poll_failures);
                }
                s_data.attached = false;
                invalidate_readings(&s_data);
            }
        }
        uint32_t failures = s_data.poll_failures;
        xSemaphoreGive(s_lock);

        if (ok || !have_dev) {
            /* A good poll, or nothing plugged in: either way the next UPS to
             * misbehave starts from a fresh backoff rather than inheriting
             * one from a device that is no longer there. */
            s_recovery_backoff_us = RECOVERY_BACKOFF_MIN_US;
            s_next_recovery_us = 0;
        } else if (failures >= FAILURES_BEFORE_RESET) {
            int64_t t = esp_timer_get_time();
            if (t >= s_next_recovery_us) {
                ESP_LOGW(TAG, "UPS unresponsive, attempting recovery "
                              "(next attempt in %llds)",
                         (long long)(s_recovery_backoff_us / 1000000));
                /* Directly, not ups_hid_recover(): this IS ups_task, and the
                 * public entry point queues work for this task to do. Calling
                 * it here would block waiting for a queue only this task
                 * drains. */
                do_recover();
                s_next_recovery_us = t + s_recovery_backoff_us;
                s_recovery_backoff_us *= 2;
                if (s_recovery_backoff_us > RECOVERY_BACKOFF_MAX_US) {
                    s_recovery_backoff_us = RECOVERY_BACKOFF_MAX_US;
                }
            }
        }
    }
}

esp_err_t ups_hid_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    ups_data_reset(&s_data);
    memset(&s_map, 0, sizeof(s_map));

    s_ctrl_done = xSemaphoreCreateBinary();
    if (!s_ctrl_done) return ESP_ERR_NO_MEM;

    s_cmd_q    = xQueueCreate(4, sizeof(cmd_req_t));
    s_cmd_done = xQueueCreate(1, sizeof(esp_err_t));
    if (!s_cmd_q || !s_cmd_done) return ESP_ERR_NO_MEM;

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    err = usb_host_client_register(&client_cfg, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client_register failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(ups_task, "ups_hid", 5120, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB host started, waiting for a UPS");
    return ESP_OK;
}

void ups_hid_get(ups_data_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_data, sizeof(*out));
    xSemaphoreGive(s_lock);
}

/* Commands run on the polling task, never on the caller's.
 *
 * ctrl_in and ctrl_out both pump usb_host_client_handle_events, and that pump
 * belongs to one task. A command arriving from the web server's task would be
 * a second pumper on the same client -- so requests are queued, the poll loop
 * executes them between polls, and the caller waits for the result. */
static uint32_t cap_for_cmd(ups_cmd_t c)
{
    switch (c) {
    case UPS_CMD_TEST_QUICK:
    case UPS_CMD_TEST_DEEP:
    case UPS_CMD_TEST_ABORT:      return UPS_CAP_TEST;
    case UPS_CMD_BEEPER_DISABLE:
    case UPS_CMD_BEEPER_ENABLE:
    case UPS_CMD_BEEPER_MUTE:     return UPS_CAP_BEEPER;
    case UPS_CMD_LOWBATT_LIMIT:   return UPS_CAP_LOWBATT;
    case UPS_CMD_LOAD_OFF:        return UPS_CAP_SHUTDOWN;
    case UPS_CMD_BATTERY_DATE:    return UPS_CAP_BATTDATE;
    }
    return 0;
}

static esp_err_t execute_cmd(const cmd_req_t *r)
{
    uint32_t cap = cap_for_cmd(r->cmd);
    const hid_field_t *f = (cap == UPS_CAP_BATTDATE) ? s_date_field : ctrl_for(cap);
    if (!f) return ESP_ERR_NOT_SUPPORTED;

    uint32_t v;
    switch (r->cmd) {
    case UPS_CMD_TEST_QUICK:      v = UPS_TEST_CMD_QUICK;      break;
    case UPS_CMD_TEST_DEEP:       v = UPS_TEST_CMD_DEEP;       break;
    case UPS_CMD_TEST_ABORT:      v = UPS_TEST_CMD_ABORT;      break;
    case UPS_CMD_BEEPER_DISABLE:  v = UPS_BEEPER_DISABLED; break;
    case UPS_CMD_BEEPER_ENABLE:   v = UPS_BEEPER_ENABLED;  break;
    case UPS_CMD_BEEPER_MUTE:     v = UPS_BEEPER_MUTED;    break;
    case UPS_CMD_BATTERY_DATE:
    case UPS_CMD_LOWBATT_LIMIT:
    case UPS_CMD_LOAD_OFF: {
        /* Clamp to what the descriptor says the field can hold. A value wider
         * than the field would otherwise wrap and mean something else -- and
         * for the shutdown timer, something else is "sooner". */
        int32_t hi = (f->bit_size >= 32) ? INT32_MAX : (int32_t)((1u << f->bit_size) - 1);
        if (r->arg < 0)  return ESP_ERR_INVALID_ARG;
        v = (uint32_t)(r->arg > hi ? hi : r->arg);
        break;
    }
    default: return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "command %d -> report %u = %lu",
             (int)r->cmd, f->report_id, (unsigned long)v);
    return set_field(f, v) ? ESP_OK : ESP_FAIL;
}

/* Is this argument meaningful, as opposed to merely representable?
 *
 * execute_cmd() already clamps to the width of the descriptor's field, which
 * stops a value wrapping into a different meaning. That is not the same check:
 * a field being wide enough to hold 255 does not make 255 a battery
 * percentage. Both commands that take a human-meaningful argument are
 * reachable without authentication -- the NUT server accepts
 * SET VAR battery.charge.low from any client on 3493 -- and that variable is
 * what decides when the UPS tells the machines it protects to shut down.
 * Setting it to 0 means it may never warn them; clamping 9999 to 255 means it
 * warns immediately and forever.
 *
 * The date rules deliberately mirror decode_mfr_date(), so a value this
 * accepts is one the firmware can read back. */
static bool arg_is_sane(ups_cmd_t cmd, int32_t arg)
{
    switch (cmd) {
    case UPS_CMD_LOWBATT_LIMIT:
        return arg >= 0 && arg <= 100;              /* a percentage */
    case UPS_CMD_LOAD_OFF:
        return arg >= 0;                            /* seconds of delay */
    case UPS_CMD_BATTERY_DATE: {
        if (arg <= 0 || arg > 0xFFFF) return false; /* the field is 16 bits */
        unsigned day = (unsigned)arg & 0x1F;
        unsigned mon = ((unsigned)arg >> 5) & 0x0F;
        unsigned yr  = 1980 + ((unsigned)arg >> 9);
        return day >= 1 && day <= 31 && mon >= 1 && mon <= 12 && yr <= 2107;
    }
    default:
        return true;                                /* takes no argument */
    }
}

esp_err_t ups_hid_command(ups_cmd_t cmd, int32_t arg)
{
    if (!s_cmd_q) return ESP_ERR_INVALID_STATE;
    if (!arg_is_sane(cmd, arg)) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool attached = s_data.attached;
    uint32_t caps = s_data.caps;
    xSemaphoreGive(s_lock);

    if (!attached) return ESP_ERR_INVALID_STATE;
    if (!(caps & cap_for_cmd(cmd))) return ESP_ERR_NOT_SUPPORTED;

    cmd_req_t r = { .cmd = cmd, .arg = arg };
    xQueueReset(s_cmd_done);
    if (xQueueSend(s_cmd_q, &r, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t res = ESP_ERR_TIMEOUT;
    xQueueReceive(s_cmd_done, &res, pdMS_TO_TICKS(5000));
    return res;
}

size_t ups_hid_report_descriptor(const uint8_t **out)
{
    if (!s_dev || !s_report_desc_len) return 0;
    if (out) *out = s_report_desc;
    return s_report_desc_len;
}

/* The recovery itself. ONLY ever runs in ups_task, which is what makes the
 * unlocked writes below safe: s_map, the poll plan and s_date_field are owned
 * by that task and touched nowhere else.
 *
 * It did not used to be. ups_hid_recover() ran this in the CALLER's task --
 * the web server, for the "reset UPS link" button -- and on Rev A that is a
 * two-second blocking VBUS cycle (CONFIG_UPSA_VBUS_CYCLE_MS) during which
 * ups_task keeps running: the UPS disconnects, VBUS returns, and the device
 * can re-enumerate and repopulate s_map before the web server task reaches the
 * memset. Clearing it then wipes a map the poll plan is already pointing into,
 * and the UPS sits there attached and permanently unreadable until someone
 * unplugs it by hand -- a recovery that causes the fault it exists to clear.
 *
 * Invisible on the Freenove devkit, which has no load switch: board_vbus_cycle()
 * returns ESP_ERR_NOT_SUPPORTED immediately, so there is no window and no
 * re-enumeration. This is a Rev A-only defect found by reading, not running. */
static esp_err_t do_recover(void)
{
    esp_err_t err = board_vbus_cycle();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    ups_data_reset(&s_data);          /* descriptor may differ after re-enum */
    xSemaphoreGive(s_lock);

    /* The map and everything derived from it go together. s_poll_rid/_bytes
     * and s_date_field are indexes and pointers INTO s_map, so clearing the
     * map alone leaves the poll loop reading a zeroed descriptor through a
     * plan that still looks valid. */
    memset(&s_map, 0, sizeof(s_map));
    s_poll_count = 0;
    s_date_field = NULL;

    return err;
}

esp_err_t ups_hid_recover(void)
{
    if (!s_cmd_q) return ESP_ERR_INVALID_STATE;

    /* Hand it to ups_task and wait for the answer, so the map is only ever
     * written by its owner. The timeout allows for the VBUS off-time plus
     * whatever transfer the task has to finish before it picks this up. */
    cmd_req_t r = { .recover = true };
    xQueueReset(s_cmd_done);
    if (xQueueSend(s_cmd_q, &r, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t res = ESP_ERR_TIMEOUT;
    xQueueReceive(s_cmd_done, &res,
                  pdMS_TO_TICKS(CONFIG_UPSA_VBUS_CYCLE_MS + 8000));
    return res;
}
