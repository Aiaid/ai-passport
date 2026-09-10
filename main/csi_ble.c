// main/csi_ble.c —— 见 csi_ble.h。
#include "csi_ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "csi_ble";

#define CSI_BLE_NAME          "AIPassport-CSI"
#define CSI_NOTIFY_PERIOD_US  (300 * 1000)  // ~3.3Hz
#define CSI_JSON_CAP          192

// 128-bit UUID 的字节序为小端(显示串的逆序)。三个 UUID 仅第 13 字节不同:
//   e2e9000X-8f2a-4c7b-9f3d-1a2b3c4d5e6f,X = 1/2/3。
#define CSI_UUID128(last) BLE_UUID128_INIT( \
    0x6f, 0x5e, 0x4d, 0x3c, 0x2b, 0x1a, 0x3d, 0x9f, \
    0x7b, 0x4c, 0x2a, 0x8f, (last), 0x00, 0xe9, 0xe2)

static const ble_uuid128_t s_svc_uuid     = CSI_UUID128(0x01);
static const ble_uuid128_t s_status_uuid  = CSI_UUID128(0x02);
static const ble_uuid128_t s_control_uuid = CSI_UUID128(0x03);

static csi_ble_status_fn s_status_fn;
static csi_ble_cmd_fn    s_cmd_fn;

static bool              s_inited;
static volatile bool     s_synced;
static volatile bool     s_active;     // 本模式是否在用(gate 广播重启与通知)
static uint8_t           s_own_addr_type;
static uint16_t          s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t          s_status_val_handle;
static volatile bool     s_status_subscribed;
static esp_timer_handle_t s_notify_timer;

static void start_advertising(void);

void csi_ble_set_status_provider(csi_ble_status_fn fn) { s_status_fn = fn; }
void csi_ble_set_cmd_handler(csi_ble_cmd_fn fn)        { s_cmd_fn = fn; }

// GATT 读写回调。STATUS 读 → 写入当前状态 JSON;CONTROL 写 → 解析并转交命令。
static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR &&
        attr_handle == s_status_val_handle) {
        char js[CSI_JSON_CAP];
        csi_status_t s = { 0 };
        if (s_status_fn) s_status_fn(&s);
        int n = csi_proto_build_status(js, sizeof(js), s.st, s.ssid, s.rssi,
                                       s.mot, s.rate, s.ftm, s.fv);
        if (n < 0) return BLE_ATT_ERR_UNLIKELY;
        int rc = os_mbuf_append(ctxt->om, js, (uint16_t)n);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[128];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        uint16_t outlen = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &outlen) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        buf[outlen] = '\0';
        csi_cmd_t cmd;
        if (csi_proto_parse_control(buf, &cmd) && s_cmd_fn) {
            s_cmd_fn(&cmd);  // 实现里只投递到队列,不阻塞 host task
        }
        return 0;  // 未知命令忽略,不报错
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_val_handle,
            },
            {
                .uuid = &s_control_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
        } else if (s_active) {
            start_advertising();  // 连接失败,继续广播
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_status_subscribed = false;
        if (s_active) start_advertising();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_active) start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_val_handle) {
            s_status_subscribed = event->subscribe.cur_notify;
        }
        return 0;
    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)CSI_BLE_NAME;
    fields.name_len = strlen(CSI_BLE_NAME);
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(TAG, "设置广播字段失败");
        return;
    }

    // 128-bit service UUID 放扫描响应(广播包放不下 name + uuid128)。
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "开始广播失败 rc=%d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "推断地址类型失败");
        return;
    }
    s_synced = true;
    if (s_active) start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host 复位,reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();               // 阻塞至 nimble_port_stop()
    nimble_port_freertos_deinit();
}

static void notify_cb(void *arg)
{
    (void)arg;
    if (!s_active || s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        !s_status_subscribed) {
        return;
    }
    char js[CSI_JSON_CAP];
    csi_status_t s = { 0 };
    if (s_status_fn) s_status_fn(&s);
    int n = csi_proto_build_status(js, sizeof(js), s.st, s.ssid, s.rssi,
                                   s.mot, s.rate, s.ftm, s.fv);
    if (n < 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(js, (uint16_t)n);
    if (!om) return;
    ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
}

esp_err_t csi_ble_start(void)
{
    s_active = true;

    if (!s_inited) {
        esp_err_t err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
            s_active = false;
            return err;
        }
        ble_hs_cfg.reset_cb = on_reset;
        ble_hs_cfg.sync_cb = on_sync;

        ble_svc_gap_init();
        ble_svc_gatt_init();
        int rc = ble_gatts_count_cfg(s_svcs);
        if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
        if (rc != 0) {
            ESP_LOGE(TAG, "注册 GATT 失败 rc=%d", rc);
            s_active = false;
            return ESP_FAIL;
        }
        ble_svc_gap_device_name_set(CSI_BLE_NAME);

        s_inited = true;
        nimble_port_freertos_init(host_task);  // 广播在 on_sync 里开始
    } else if (s_synced) {
        start_advertising();
    }

    if (!s_notify_timer) {
        const esp_timer_create_args_t args = {
            .callback = notify_cb,
            .name = "csi_notify",
        };
        esp_timer_create(&args, &s_notify_timer);
    }
    if (s_notify_timer) {
        esp_timer_stop(s_notify_timer);  // 幂等:先停再起
        esp_timer_start_periodic(s_notify_timer, CSI_NOTIFY_PERIOD_US);
    }
    return ESP_OK;
}

void csi_ble_stop(void)
{
    s_active = false;
    if (s_notify_timer) {
        esp_timer_stop(s_notify_timer);  // 保留定时器对象,供下次复用
    }
    ble_gap_adv_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_status_subscribed = false;
}
