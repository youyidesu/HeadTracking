/**
 * ble_test.c —— BLE 连接能力 + 延迟测试（只做这两件事，不参与控制）
 *
 * 服务：FFF0
 *   FFF1  write / write-no-rsp   客户端写入任意字节（建议 16 B，带时间戳）
 *   FFF2  notify                 收到后**原样回显**，客户端算 RTT
 *
 * 用法：烧进去 → 手机 nRF Connect 或 tools/ble_latency.py 连 "C5-BLE-TEST"
 *      写 16 字节 → 立刻收到相同的 16 字节，RTT/2 ≈ 单程延迟
 */
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_test.h"
#include "bridge.h"

static const char *TAG = "ble_test";

#define DEVICE_NAME      "C5-BLE-TEST"
#define UUID_SVC         0xFFF0
#define UUID_CH_RX       0xFFF1
#define UUID_CH_TX       0xFFF2

static uint8_t  s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint32_t s_rx_cnt, s_tx_cnt, s_fail;
static volatile bool s_need_adv = false;
static int64_t  s_disc_us = 0;
static uint16_t s_itvl = 0;      /* 实际连接间隔，单位 1.25 ms */

/* ---------------- GATT 回调 ---------------- */
/* arg: 1 = RX 写入口，2 = TX 通知口（用 arg 区分，不能用 attr —— 那是句柄不是 UUID） */
static int gatt_access(uint16_t ch, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)ch; (void)attr;
    const int which = (int)(intptr_t)arg;

    if (which == 1) {
        s_rx_cnt++;

        /* 原样回显（16 B 一帧，不解析内容——延迟测试只关心往返） */
        uint8_t buf[64];
        uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (n > sizeof(buf)) n = sizeof(buf);
        if (os_mbuf_copydata(ctxt->om, 0, n, buf) != 0) return BLE_ATT_ERR_UNLIKELY;

        /* 顺带喂给桥接层（校验→重打包→SPI 给 F411），这样 BLE 也能驱动云台 */
        if (n >= 16) bridge_feed(buf, n);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
        if (om && s_conn != BLE_HS_CONN_HANDLE_NONE) {
            if (ble_gatts_notify_custom(s_conn, s_tx_handle, om) == 0) s_tx_cnt++;
            else s_fail++;
        } else {
            s_fail++;
        }
        return 0;
    }

    if (which == 2) {
        /* TX 只用于 notify，不响应读 */
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID_CH_RX),
                .access_cb = gatt_access,
                .arg = (void *)1,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_CH_TX),
                .access_cb = gatt_access,
                .arg = (void *)2,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

static int gap_event(struct ble_gap_event *ev, void *arg);

/* ---------------- 广播 ---------------- */
static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start rc=%d", rc);
    else ESP_LOGI(TAG, "广播中：%s", DEVICE_NAME);
}

/* ---------------- GAP 事件 ---------------- */
static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            struct ble_gap_conn_desc d0;
            if (ble_gap_conn_find(s_conn, &d0) == 0) s_itvl = d0.conn_itvl;
            ESP_LOGI(TAG, "已连接 (handle=%d)，初始间隔 %.2f ms", s_conn, s_itvl * 1.25);
            /* 请求更快的连接间隔（iOS/Android 可能拒绝，看对端） */
            struct ble_gap_upd_params up = {
                .itvl_min = 6,        /* 7.5 ms = 6 × 1.25 ms */
                .itvl_max = 6,        /* 强制 7.5 ms，不留给对端挑 15 ms 的余地 */
                .latency = 0,
                .supervision_timeout = 400,   /* 4 s */
            };
            ble_gap_update_params(s_conn, &up);
        } else {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "断开 (reason=0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        /* 不能在回调里立刻重开广播：NimBLE 的断开流程还没收尾，
           立刻 advertise() 会让对端连上就断（S3 上实测过这个坑）。 */
        s_need_adv = true;
        s_disc_us  = esp_timer_get_time();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &desc) == 0) {
            s_itvl = desc.conn_itvl;
            ESP_LOGI(TAG, "连接间隔已协商: %.2f ms（上限 %.0f Hz）",
                     s_itvl * 1.25, s_itvl ? 1000.0 / (s_itvl * 1.25) : 0.0);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    default:
        return 0;
    }
}

/* ---------------- 协议栈 ---------------- */
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc) { ESP_LOGE(TAG, "infer_addr rc=%d", rc); return; }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

/* 断连后延迟 300 ms 重新广播，由独立任务负责 */
static void adv_task(void *param)
{
    (void)param;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_need_adv && (esp_timer_get_time() - s_disc_us) > 300000) {
            s_need_adv = false;
            advertise();
        }
    }
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();              /* 永不返回 */
    nimble_port_freertos_deinit();
}

void ble_test_start(void)
{
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* 配对策略：Just Works、不绑定。
       不加这段，主机会主动发起配对然后报「PIN 或密钥不正确」。 */
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding        = 0;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_sc             = 0;
    ble_hs_cfg.sm_our_key_dist   = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return; }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc) { ESP_LOGE(TAG, "add_svcs rc=%d", rc); return; }

    /* 发射功率拉到最大：默认只有 +3 dBm，天线不良时差 17 dB */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,    ESP_PWR_LVL_P20);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN,   ESP_PWR_LVL_P20);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P20);
    ESP_LOGI(TAG, "BLE 发射功率已设为 +20 dBm");

    ble_svc_gap_device_name_set(DEVICE_NAME);
    nimble_port_freertos_init(host_task);
    xTaskCreate(adv_task, "adv", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "BLE 测试服务已启动：%s / 服务 0x%04X", DEVICE_NAME, UUID_SVC);
}

void ble_test_stats(void)
{
    ESP_LOGI(TAG, "BLE 收 %u 回显 %u 失败 %u | 连接%s | 间隔 %.1f ms",
             (unsigned)s_rx_cnt, (unsigned)s_tx_cnt, (unsigned)s_fail,
             s_conn == BLE_HS_CONN_HANDLE_NONE ? "未连" : "已连",
             s_itvl * 1.25);
}
