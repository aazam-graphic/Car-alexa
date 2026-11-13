/*
 * xbox_bt_bridge.c - publishes the USB Xbox pad as a BLE HID gamepad.
 *
 * Why: the Redgear/Xbox dongle enumerates as a USB device, but phones have no
 * USB host port for it. This module advertises the ESP32-S3 as a Bluetooth LE
 * HID gamepad so a mobile game (Call of Duty Mobile) sees a controller.
 *
 * Two identities (runtime switchable, persisted in NVS):
 *
 *   XBOX    - impersonates a real Xbox Series X|S wireless controller
 *             (Microsoft 045E:0B13). Uses the 283-byte descriptor dumped from
 *             a real unit (see xbox_bt_descriptor.h) + the PnP ID that carries
 *             VID/PID. Report ID 1, 17-byte notification:
 *               [0]     report ID (1)
 *               [1..4]  X, Y   left stick   16-bit LE, 0..65534, center 32768
 *               [5..8]  Z, Rz  right stick  same
 *               [9..12] Brake (LT) + Accelerator (RT), 10-bit each + 6 pad
 *               [13]    hat switch 4-bit (1..8, 0 = null)
 *               [14..15] 15 buttons
 *               [16]    consumer Record (Share) bit
 *
 *   GENERIC - the plain 8-byte gamepad the bridge shipped with:
 *               [0..1] 16 buttons, [2..5] sticks int8, [6] LT [7] RT
 *
 * Button bits in the XBOX profile (verified against xpadneo's btmon dump and
 * Android's Generic.kl / Vendor_045e_Product_0b12.kl - all three agree):
 *   A=1  B=2  X=4  Y=5  LB=7  RB=8  BACK=11  START=12  GUIDE=13
 *   LS=14  RS=15   (3, 6, 9, 10 unused, exactly like a real pad)
 */
#include "sdkconfig.h"

#include "xbox_bt_bridge.h"
#include "xbox360.h"
#include "car_global.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "xbox_bt_descriptor.h"   /* 283-byte Xbox Series X|S report map */

static const char *TAG = "xbox_bt";

#define DEVICE_NAME_XBOX    "Xbox Wireless Controller"
#define DEVICE_NAME_GENERIC "Azam Xbox Pad"

#define NVS_NS_PROFILE "xboxbt"
#define NVS_KEY_PROFILE "prof"

static bool s_enabled;
static bool s_inited;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;
static uint16_t s_out_handle;
static bool s_subscribed;
static xbox_bt_profile_t s_profile = XBOX_BT_PROFILE_XBOX;

/* ---- PnP ID (0x2A50): this is where VID/PID actually live --------------- */
/* [source][vid lo][vid hi][pid lo][pid hi][ver lo][ver hi] */
static const uint8_t s_pnp_xbox[7] = {
    0x02,              /* Vendor ID Source = USB-IF */
    0x5E, 0x04,        /* Vendor ID 0x045E (Microsoft) */
    0x13, 0x0B,        /* Product ID 0x0B13 (Xbox Series X|S) */
    0x13, 0x05         /* Product Version 0x0513 */
};
static const uint8_t s_pnp_generic[7] = {
    0x01,              /* Vendor ID Source = Bluetooth SIG */
    0xE5, 0x02,        /* Company ID 0x02E5 (Espressif) */
    0x01, 0x00,        /* Product ID 0x0001 */
    0x00, 0x01         /* Product Version 0x0100 */
};

/* ---- GENERIC report descriptor: 16 buttons + 4 axes --------------------- */
static const uint8_t s_report_map_generic[] = {
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

static const char *profile_name(void)
{
    return s_profile == XBOX_BT_PROFILE_XBOX ? DEVICE_NAME_XBOX : DEVICE_NAME_GENERIC;
}

/* ---- NVS persistence ---------------------------------------------------- */
static void profile_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PROFILE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY_PROFILE, &v) == ESP_OK && v <= XBOX_BT_PROFILE_XBOX)
        s_profile = (xbox_bt_profile_t)v;
    nvs_close(h);
}

static void profile_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PROFILE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_PROFILE, (uint8_t)s_profile);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- GATT callbacks ----------------------------------------------------- */
static int hid_info_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, s_hid_info, sizeof(s_hid_info)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_map_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    const uint8_t *map = s_report_map_generic;
    uint16_t len = (uint16_t)sizeof(s_report_map_generic);
    if (s_profile == XBOX_BT_PROFILE_XBOX) {
        map = s_xbox_rdesc;
        len = (uint16_t)sizeof(s_xbox_rdesc);
    }
    return os_mbuf_append(ctxt->om, map, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_pnp_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    const uint8_t *id = (s_profile == XBOX_BT_PROFILE_XBOX) ? s_pnp_xbox : s_pnp_generic;
    return os_mbuf_append(ctxt->om, id, 7) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_ctrl_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    /* HID Control Point: 0=suspend, 1=exit suspend. Nothing to do. */
    return 0;
}

static int hid_proto_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return 0;
}

/* Report Reference descriptor (0x2908): host needs this to map the Report
   characteristic to "report ID N, input/output". Without it Android/Windows
   will not route notifications. */
static const uint8_t s_rep_ref_in[2]  = { 0x01, 0x01 };  /* ID 1, input  */
static const uint8_t s_rep_ref_out[2] = { 0x03, 0x02 };  /* ID 3, output */

static int hid_rep_ref_in_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, s_rep_ref_in, sizeof(s_rep_ref_in)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_rep_ref_out_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, s_rep_ref_out, sizeof(s_rep_ref_out)) == 0
           ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ---- report packing ----------------------------------------------------- */
/* Xbox sticks: 0..65534 with 32768 centre. Pad reports +Y = stick up, but the
   HID Y/Rz axis minimum is "up", so the two Y axes are inverted here. */
static uint16_t xbox_axis(int16_t v, bool invert)
{
    int32_t out = invert ? (32768 - (int32_t)v) : (32768 + (int32_t)v);
    if (out < 0) out = 0;
    if (out > 65534) out = 65534;
    return (uint16_t)out;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* Hat switch: 1=N 2=NE 3=E 4=SE 5=S 6=SW 7=W 8=NW, 0 = centred/null */
static uint8_t xbox_hat(uint16_t d)
{
    bool u  = (d & B_DUP)    != 0;
    bool dn = (d & B_DDOWN)  != 0;
    bool l  = (d & B_DLEFT)  != 0;
    bool r  = (d & B_DRIGHT) != 0;
    if (u && r)  return 2;
    if (r && dn) return 4;
    if (dn && l) return 6;
    if (l && u)  return 8;
    if (u)       return 1;
    if (r)       return 3;
    if (dn)      return 5;
    if (l)       return 7;
    return 0;
}

static uint16_t xbox_buttons(uint16_t d)
{
    uint16_t b = 0;
    if (d & B_A)     b |= (1u << 0);   /* button 1  */
    if (d & B_B)     b |= (1u << 1);   /* button 2  */
    if (d & B_X)     b |= (1u << 3);   /* button 4  */
    if (d & B_Y)     b |= (1u << 4);   /* button 5  */
    if (d & B_LB)    b |= (1u << 6);   /* button 7  */
    if (d & B_RB)    b |= (1u << 7);   /* button 8  */
    if (d & B_BACK)  b |= (1u << 10);  /* button 11 (View)  */
    if (d & B_START) b |= (1u << 11);  /* button 12 (Menu)  */
    if (d & B_GUIDE) b |= (1u << 12);  /* button 13 (Xbox)  */
    if (d & B_LS)    b |= (1u << 13);  /* button 14 */
    if (d & B_RS)    b |= (1u << 14);  /* button 15 */
    return b;
}

/* Build the current report. Returns the number of bytes written. */
static uint8_t build_report(uint8_t *rep)
{
    const xbox360_pad_t *p = xbox360_pad(0);
    bool present = p && p->present;
    uint16_t d = present ? (uint16_t)(p->buttons >> 16) : 0;

    if (s_profile != XBOX_BT_PROFILE_XBOX) {
        memset(rep, 0, 8);
        if (present) {
            uint8_t lo = 0, hi = 0;
            if (d & B_A)     lo |= 0x01;
            if (d & B_B)     lo |= 0x02;
            if (d & B_X)     lo |= 0x04;
            if (d & B_Y)     lo |= 0x08;
            if (d & B_LB)    lo |= 0x10;
            if (d & B_RB)    lo |= 0x20;
            if (d & B_BACK)  lo |= 0x40;
            if (d & B_START) lo |= 0x80;
            if (d & B_LS)    hi |= 0x01;
            if (d & B_RS)    hi |= 0x02;
            if (d & B_DUP)   hi |= 0x04;
            if (d & B_DDOWN) hi |= 0x08;
            if (d & B_DLEFT) hi |= 0x10;
            if (d & B_DRIGHT) hi |= 0x20;
            if (d & B_GUIDE) hi |= 0x40;
            rep[0] = lo;
            rep[1] = hi;
            rep[2] = (uint8_t)(p->lx >> 8);
            rep[3] = (uint8_t)(p->ly >> 8);
            rep[4] = (uint8_t)(p->rx >> 8);
            rep[5] = (uint8_t)(p->ry >> 8);
            rep[6] = p->lt;
            rep[7] = p->rt;
        }
        return 8;
    }

    memset(rep, 0, 17);
    rep[0] = 0x01;                       /* report ID */
    if (!present) return 17;

    put16(&rep[1],  xbox_axis(p->lx, false));   /* X  */
    put16(&rep[3],  xbox_axis(p->ly, true));    /* Y  */
    put16(&rep[5],  xbox_axis(p->rx, false));   /* Z  */
    put16(&rep[7],  xbox_axis(p->ry, true));    /* Rz */

    uint16_t brake = (uint16_t)((uint32_t)p->lt * 1023u / 255u);  /* LT */
    uint16_t accel = (uint16_t)((uint32_t)p->rt * 1023u / 255u);  /* RT */
    rep[9]  = (uint8_t)(brake & 0xFF);
    rep[10] = (uint8_t)((brake >> 8) & 0x03);
    rep[11] = (uint8_t)(accel & 0xFF);
    rep[12] = (uint8_t)((accel >> 8) & 0x03);

    rep[13] = xbox_hat(d);

    uint16_t b = xbox_buttons(d);
    rep[14] = (uint8_t)(b & 0xFF);
    rep[15] = (uint8_t)((b >> 8) & 0x7F);
    rep[16] = 0;                          /* Record/Share not used */
    return 17;
}

static int hid_report_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    uint8_t rep[17];
    uint8_t n = build_report(rep);
    return os_mbuf_append(ctxt->om, rep, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* Output report (rumble/LED). We accept and ignore it so hosts that send
   force-feedback writes do not see an error. */
static int hid_out_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0;
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
              .val_handle = &s_report_handle,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .att_flags = BLE_ATT_F_READ,
                    .access_cb = hid_rep_ref_in_cb },
                  { 0 }
              } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_out_cb,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
              .val_handle = &s_out_handle,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .att_flags = BLE_ATT_F_READ,
                    .access_cb = hid_rep_ref_out_cb },
                  { 0 }
              } },
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = hid_pnp_cb,
              .flags = BLE_GATT_CHR_F_READ },        /* PnP ID: VID/PID live here */
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

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    /* The name does not fit next to the 16-bit UUID + appearance in the 31-byte
       advertising packet ("Xbox Wireless Controller" alone is 26 bytes), so it
       goes in the scan response - which is what Bluetooth settings shows. */
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    const char *name = ble_svc_gap_device_name();
    rsp.name = (uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan rsp fields rc=%d", rc);
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

    /* Handles are filled in by ble_gatts_add_svcs() via .val_handle, so this
       is only a sanity log - never look them up here. */
    ESP_LOGI(TAG, "report value handle=%u, out=%u",
             (unsigned)s_report_handle, (unsigned)s_out_handle);
    if (!s_report_handle) ESP_LOGE(TAG, "input report handle missing - notify disabled");

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

    profile_load();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(profile_name());

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt register rc=%d", rc);
        return;
    }

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE HID gamepad ready (profile=%s)", xbox_bt_profile_name());
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

xbox_bt_profile_t xbox_bt_profile(void) { return s_profile; }

const char *xbox_bt_profile_name(void)
{
    return s_profile == XBOX_BT_PROFILE_XBOX ? "XBOX 0B13" : "GENERIC";
}

void xbox_bt_set_profile(xbox_bt_profile_t p)
{
    if (p > XBOX_BT_PROFILE_XBOX) p = XBOX_BT_PROFILE_XBOX;
    if (p == s_profile) return;
    s_profile = p;
    profile_save();

    if (!s_inited) {
        ESP_LOGI(TAG, "profile -> %s (applies at init)", xbox_bt_profile_name());
        return;
    }

    /* The name and the report map are read by the host at discovery time, so
       drop the link and re-advertise with the new identity. The phone has to
       forget the old bond and pair again. */
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
    }
    ble_svc_gap_device_name_set(profile_name());
    if (s_enabled) {
        ble_gap_adv_stop();
        start_adv();
    }
    ESP_LOGI(TAG, "profile -> %s (re-pair the phone)", xbox_bt_profile_name());
}

void xbox_bt_poll(void)
{
    if (!s_enabled || !s_subscribed || !s_report_handle ||
        s_conn == BLE_HS_CONN_HANDLE_NONE) return;

    /* Cap at 100 Hz: the car loop can run faster than the radio should send. */
    static int64_t s_last_us;
    int64_t now_us = esp_timer_get_time();
    if (s_last_us && now_us - s_last_us < 10000) return;
    s_last_us = now_us;

    uint8_t rep[17];
    uint8_t n = build_report(rep);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(rep, n);
    if (!om) return;
    ble_gatts_notify_custom(s_conn, s_report_handle, om);
}

#else  /* Bluetooth not enabled in sdkconfig: compile to safe no-ops */

void xbox_bt_init(void) {}
void xbox_bt_set_enabled(bool on) { (void)on; }
bool xbox_bt_is_enabled(void) { return false; }
bool xbox_bt_connected(void) { return false; }
xbox_bt_profile_t xbox_bt_profile(void) { return XBOX_BT_PROFILE_GENERIC; }
const char *xbox_bt_profile_name(void) { return "OFF"; }
void xbox_bt_set_profile(xbox_bt_profile_t p) { (void)p; }
void xbox_bt_poll(void) {}

#endif
