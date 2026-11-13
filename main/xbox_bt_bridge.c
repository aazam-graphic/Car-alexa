/*
 * xbox_bt_bridge.c - publishes the USB Xbox pad as a BLE HID gamepad.
 *
 * Why: the Redgear/Xbox dongle enumerates as a USB device, but phones have no
 * USB host port for it. This module advertises the ESP32-S3 as a standard BLE
 * HID gamepad, so any Android/iOS/PC game that supports a Bluetooth controller
 * works with the same physical pad.
 *
 * HID report (report id 1, 8 bytes):
 *   [0..1] 16 buttons (bit0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB, 6=BACK, 7=START,
 *          8=LS, 9=RS, 10=DUP, 11=DDOWN, 12=DLEFT, 13=DRIGHT, 14=GUIDE)
 *   [2] LX  [3] LY  [4] RX  [5] RY   (int8, -127..127)
 *   [6] LT  [7] RT                   (uint8, 0..255)
 */
#include "sdkconfig.h"

#include "xbox_bt_bridge.h"
#include "xbox360.h"
#include "car_global.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "xbox_bt";

#define DEVICE_NAME "Azam Xbox Pad"

static bool s_enabled;
static bool s_inited;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;
static bool s_subscribed;

/* ---- HID report descriptor: 16 buttons + 4 axes ------------------------- */
static const uint8_t s_report_map[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x05,        /* Usage (Game Pad) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x01,        /*   Report ID (1) */
    0x05, 0x09,        /*   Usage Page (Button) */
    0x19, 0x01,        /*   Usage Minimum (1) */
    0x29, 0x10,        /*   Usage Maximum (16) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x10,        /*   Report Count (16) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x30,        /*   Usage (X) - left stick X  */
    0x09, 0x31,        /*   Usage (Y) - left stick Y  */
    0x09, 0x32,        /*   Usage (Z) - right stick X */
    0x09, 0x35,        /*   Usage (Rz)- right stick Y */
    0x15, 0x81,        /*   Logical Minimum (-127) */
    0x25, 0x7F,        /*   Logical Maximum (127) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x04,        /*   Report Count (4) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x09, 0x33,        /*   Usage (Rx) - left trigger  */
    0x09, 0x34,        /*   Usage (Ry) - right trigger */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,  /*   Logical Maximum (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x02,        /*   Report Count (2) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0xC0               /* End Collection */
};

static const uint8_t s_hid_info[4] = { 0x11, 0x01, 0x00, 0x02 }; /* bcdHID 1.11, no country, normally connectable */

/* ---- GATT callbacks ----------------------------------------------------- */
static int hid_info_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, s_hid_info, sizeof(s_hid_info)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_map_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, s_report_map, sizeof(s_report_map)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_ctrl_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    /* HID Control Point: 0=suspend, 1=exit suspend. Nothing to do. */
    return 0;
}

static int hid_report_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    /* Reads return the current report. */
    const xbox360_pad_t *p = xbox360_pad(0);
    uint8_t rep[8] = {0};
    if (p && p->present) {
        uint16_t d = (uint16_t)(p->buttons >> 16);
        uint16_t hid = 0;
        if (d & B_A)      hid |= 0x0001;
        if (d & B_B)      hid |= 0x0002;
        if (d & B_X)      hid |= 0x0004;
        if (d & B_Y)      hid |= 0x0008;
        if (d & B_LB)     hid |= 0x0010;
        if (d & B_RB)     hid |= 0x0020;
        if (d & B_BACK)   hid |= 0x0040;
        if (d & B_START)  hid |= 0x0080;
        if (d & B_LS)     hid |= 0x0100;
        if (d & B_RS)     hid |= 0x0200;
        if (d & B_DUP)    hid |= 0x0400;
        if (d & B_DDOWN)  hid |= 0x0800;
        if (d & B_DLEFT)  hid |= 0x1000;
        if (d & B_DRIGHT) hid |= 0x2000;
        if (d & B_GUIDE)  hid |= 0x4000;
        rep[0] = (uint8_t)(hid & 0xFF);
        rep[1] = (uint8_t)(hid >> 8);
        rep[2] = (uint8_t)(p->lx >> 8);
        rep[3] = (uint8_t)(p->ly >> 8);
        rep[4] = (uint8_t)(p->rx >> 8);
        rep[5] = (uint8_t)(p->ry >> 8);
        rep[6] = p->lt;
        rep[7] = p->rt;
    }
    return os_mbuf_append(ctxt->om, rep, sizeof(rep)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_proto_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return 0;
}

/* Report Reference descriptor (0x2908): host needs this to map the Report
   characteristic to "report ID 1, input" - Android/Windows HID-over-GATT
   will not route notifications without it. */
static const uint8_t s_rep_ref_in[2] = { 0x01, 0x01 };

static int hid_rep_ref_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, s_rep_ref_in, sizeof(s_rep_ref_in)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),          /* HID service */
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_info_cb,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_map_cb,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_ctrl_cb,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_report_cb,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .att_flags = BLE_ATT_F_READ,
                    .access_cb = hid_rep_ref_cb },
                  { 0 }
              } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_proto_cb,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { 0 }
        }
    },
    { 0 }
};

/* ---- Advertising / connection ------------------------------------------ */
static void start_adv(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected, conn=%u", (unsigned)s_conn);
        } else {
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            start_adv();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        if (s_enabled) start_adv();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        /* The input report is our only notify characteristic, so accept every
           subscription instead of matching an exact handle (val_handle+1 is the
           CCCD handle on some hosts). */
        if (s_report_handle == 0 ||
            event->subscribe.attr_handle == s_report_handle ||
            event->subscribe.attr_handle == (uint16_t)(s_report_handle + 1)) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "input report %s (handle %u)",
                     s_subscribed ? "SUBSCRIBED" : "unsubscribed",
                     (unsigned)event->subscribe.attr_handle);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_enabled) start_adv();
        return 0;
    }
    return 0;
}

static void start_adv(void)
{
    if (!s_enabled || s_conn != BLE_HS_CONN_HANDLE_NONE) return;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = 0x03C8;           /* HID Game Pad */
    fields.appearance_is_present = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    /* HID service UUID in the advertising packet: phones/PCs can tell this is
       a gamepad before connecting (appearance alone is not always enough). */
    static const ble_uuid16_t hid_svc = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = (const ble_uuid16_t *)&hid_svc;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc == 0) ESP_LOGI(TAG, "advertising as \"%s\"", name);
    else         ESP_LOGE(TAG, "adv start rc=%d", rc);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return; }

    /* Attribute handles only exist after the host has started, so resolve the
       input-report value handle here - doing it in xbox_bt_init() returns
       BLE_HS_ENOENT and leaves s_report_handle at 0 (notify would never fire,
       which is exactly why the phone got no input). */
    if (!s_report_handle) {
        rc = ble_gatts_find_chr(BLE_UUID16_DECLARE(0x1812),
                                BLE_UUID16_DECLARE(0x2A4D), NULL, &s_report_handle);
        if (rc == 0) ESP_LOGI(TAG, "report value handle=%u", (unsigned)s_report_handle);
        else         ESP_LOGE(TAG, "report handle lookup rc=%d", rc);
    }

    start_adv();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- Public API --------------------------------------------------------- */
void xbox_bt_init(void)
{
    if (s_inited) return;
    s_inited = true;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt register rc=%d", rc);
        return;
    }

    /* NOTE: the input-report value handle is resolved in on_sync() - GATT
       handles do not exist until the host has started. */

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE HID gamepad ready");
}

void xbox_bt_set_enabled(bool on)
{
    if (on == s_enabled) return;
    s_enabled = on;
    if (!s_inited) {
        if (on) xbox_bt_init();
        return;
    }
    if (on) {
        start_adv();
    } else {
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        ble_gap_adv_stop();
    }
}

bool xbox_bt_is_enabled(void) { return s_enabled; }
bool xbox_bt_connected(void)  { return s_subscribed && s_conn != BLE_HS_CONN_HANDLE_NONE; }

void xbox_bt_poll(void)
{
    if (!s_enabled || !s_subscribed || !s_report_handle ||
        s_conn == BLE_HS_CONN_HANDLE_NONE) return;

    /* Cap at 100 Hz: the car loop can run faster than the radio should send. */
    static int64_t s_last_us;
    int64_t now_us = esp_timer_get_time();
    if (s_last_us && now_us - s_last_us < 10000) return;
    s_last_us = now_us;

    const xbox360_pad_t *p = xbox360_pad(0);
    uint8_t rep[8] = {0};
    if (p && p->present) {
        uint16_t d = (uint16_t)(p->buttons >> 16);
        uint16_t hid = 0;
        if (d & B_A)      hid |= 0x0001;
        if (d & B_B)      hid |= 0x0002;
        if (d & B_X)      hid |= 0x0004;
        if (d & B_Y)      hid |= 0x0008;
        if (d & B_LB)     hid |= 0x0010;
        if (d & B_RB)     hid |= 0x0020;
        if (d & B_BACK)   hid |= 0x0040;
        if (d & B_START)  hid |= 0x0080;
        if (d & B_LS)     hid |= 0x0100;
        if (d & B_RS)     hid |= 0x0200;
        if (d & B_DUP)    hid |= 0x0400;
        if (d & B_DDOWN)  hid |= 0x0800;
        if (d & B_DLEFT)  hid |= 0x1000;
        if (d & B_DRIGHT) hid |= 0x2000;
        if (d & B_GUIDE)  hid |= 0x4000;
        rep[0] = (uint8_t)(hid & 0xFF);
        rep[1] = (uint8_t)(hid >> 8);
        rep[2] = (uint8_t)(p->lx >> 8);
        rep[3] = (uint8_t)(p->ly >> 8);
        rep[4] = (uint8_t)(p->rx >> 8);
        rep[5] = (uint8_t)(p->ry >> 8);
        rep[6] = p->lt;
        rep[7] = p->rt;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(rep, sizeof(rep));
    if (!om) return;
    ble_gatts_notify_custom(s_conn, s_report_handle, om);
}

#else  /* Bluetooth not enabled in sdkconfig: compile to safe no-ops */

void xbox_bt_init(void) {}
void xbox_bt_set_enabled(bool on) { (void)on; }
bool xbox_bt_is_enabled(void) { return false; }
bool xbox_bt_connected(void) { return false; }
void xbox_bt_poll(void) {}

#endif
