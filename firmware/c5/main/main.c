/**
 * C5 桥接固件：UDP(16B pose) → SPI(30B qp_head_t) → F411
 *
 * 数据流
 *   Quest/PC ──UDP:5005──► ESP32-C5 ──SPI 5MHz──► STM32F411
 *
 * 为什么要在 C5 里改包长
 *   PC/浏览器发的是 16 字节精简帧（magic/seq/yaw/pitch/rate/flags/crc），
 *   而 F411 侧 esp_link.c 现在解析的是 30 字节 qp_head_t。
 *   接收端已经冻结，所以在 C5 里做一次重打包，F411 一行不用改。
 *   将来两边都改成 16 B 时，删掉 repack() 直接透传即可。
 */

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "ble_test.h"
#include "http_status.h"
#include "esp_now.h"

static const char *TAG = "bridge";

/* ─────────────────────────── 配置（按实际接线改这里） ─────────────────────────── */

/* 传输方式 —— 由 main/ht_mode.h 决定，例程分两个目录各带一份
 *
 * 射频限制（实测 2026-11-21）：
 *   C5 只有一套射频。WiFi 一开（5G/2.4G 都一样），BLE 广播就抢不到空口，
 *   手机/头显完全扫不到 C5-BLE-TEST；关掉 WiFi 立刻正常
 *   （RTT p50 14.93 ms / 抖动 0.33 ms）。所以 WiFi 和 BLE 只能二选一。
 *
 *   HT_MODE 1 = 网页版：WiFi(AP) + HTTPS 页面 + WebSocket
 *   HT_MODE 2 = App 版：纯 BLE（网页/热点全部关闭）
 */
#include "ht_mode.h"

#if HT_MODE == 1
  #define BRIDGE_LINK_UDP        1
  #define BRIDGE_LINK_ESPNOW     0
  #define BRIDGE_ENABLE_HTTP     1     /* 页面 + /ws */
  #define BRIDGE_ENABLE_BLE_TEST 0     /* 与 WiFi 抢射频，关掉 */
#elif HT_MODE == 2
  #define BRIDGE_LINK_UDP        0
  #define BRIDGE_LINK_ESPNOW     0
  #define BRIDGE_ENABLE_HTTP     0
  #define BRIDGE_ENABLE_BLE_TEST 1     /* 给 Unity/APK 用 */
#else
  #error "HT_MODE 只能是 1（网页版）或 2（App 版）"
#endif

#define ESPNOW_USE_BROADCAST 1      /* 1=广播（免配对，调试方便）；0=只收 PEER_MAC */
#if BRIDGE_LINK_ESPNOW
static const uint8_t ESPNOW_PEER_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
#endif

/* 频段：ESP-NOW 本身没有 band 参数，它跟着 WiFi 射频走。
   C5 是双频，用 esp_wifi_set_band_mode() 把射频切到 5G，ESP-NOW 就在 5G 上跑。
   2 = 2.4G only / 5 = 5G only / 0 = auto */
#define BRIDGE_BAND        2
#define ESPNOW_CHANNEL     36       /* 5G 时用 36/40/44/48，避开 5.8G 图传 */

#define BRIDGE_WIFI_MODE   1        /* 0 = STA 连路由器；1 = AP 自己开热点；2 = 纯 ESP-NOW 不组网 */

#define STA_SSID           "YOUR_SSID"
#define STA_PASS           "YOUR_PASSWORD"

#define AP_SSID            "HT-TURRET"
#define AP_PASS            "turret1234"   /* 至少 8 位 */
#define AP_CHANNEL         6    /* 2.4G：和 BLE 同频段，共存引擎才管得过来 */

#define UDP_PORT           5005

/* ─── SPI 引脚：用的是 ESP32-C5 的 SPI2 IOMUX 默认脚，通用性最好 ───
 *
 *   GPIO6  = SPI2 CLK   (IOMUX 默认，走 IO MUX 有最佳时序)
 *   GPIO7  = SPI2 MOSI  (IOMUX 默认)
 *   GPIO10 = SPI2 CS    (IOMUX 默认；CS 是软件驱动，其实任意空闲脚都行)
 *
 * 已核对不会撞车（IDF v5.5 soc/esp32c5）：
 *   · Flash/MSPI : GPIO16 / 17 / 21 / 22
 *   · UART0      : GPIO11 (TX) / GPIO12 (RX)   ← 控制台走这里
 *   · USB D-/D+  : GPIO13 / 14
 *   · 有效 GPIO  : 0~28
 *
 * 换 mini 板时：只要板子引出 6/7/10 就原样能用。
 * 若某个脚没引出，改下面三行即可——走 GPIO 矩阵，任意空闲脚都能跑 5 MHz：
 *   例如 SCLK=GPIO0, MOSI=GPIO1, CS=GPIO8（模拟模式，注意别和其他外设复用）
 */
#define PIN_SCLK           GPIO_NUM_6
#define PIN_MOSI           GPIO_NUM_7
#define PIN_CS             GPIO_NUM_10
#define PIN_MISO           GPIO_NUM_4    /* 回读 F411 的统计帧 */
#define SPI_FREQ_HZ        1000000    /* bring-up 用 1 MHz，通了再往 5 MHz 提 */

#define STAT_INTERVAL_US   5000000         /* 5 s 打印一次统计（1 s 太吵） */

/* ─────────────────────────── 协议定义 ─────────────────────────── */

#define WIRE_MAGIC         0xA55Au         /* PC/浏览器发的 16 B 帧 */
#define QP_MAGIC           0xA55Au         /* F411 期望的 30 B 帧 */
#define QP_VER             1u

#pragma pack(push, 1)
/* 16 字节：与 quest/bridge.js、tools/mockpose.ps1 一致 */
typedef struct {
    uint16_t magic;
    uint16_t seq;
    int16_t  yaw_cdeg;
    int16_t  pitch_cdeg;
    int16_t  yaw_rate;
    int16_t  pitch_rate;
    uint16_t flags;
    uint16_t crc;          /* 前 14 字节，CCITT 0x1021, init 0xFFFF */
} wire16_t;

/* 30 字节：与 F411 的 BSP/Inc/esp_link.h 中 qp_head_t 逐字节一致 */
typedef struct {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  flags;
    uint32_t seq;
    uint32_t t_us;
    int16_t  qw, qx, qy, qz;
    int16_t  yaw_cdeg;
    int16_t  pitch_cdeg;
    int16_t  yaw_rate;
    int16_t  pitch_rate;
    uint16_t crc;          /* 前 28 字节 */
} qp_head_t;
#pragma pack(pop)

_Static_assert(sizeof(wire16_t) == 16, "wire16 必须是 16 字节");
_Static_assert(sizeof(qp_head_t) == 30, "qp_head 必须是 30 字节");

/* ─────────────────────────── 工具 ─────────────────────────── */

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* 16 B → 30 B；成功返回 true */
/* F411 通过 MISO 回传的统计帧（30B，和下发帧同长） */
#pragma pack(push, 1)
typedef struct {
    uint16_t magic;      /* 0x5AA5 = 来自 F411 */
    uint16_t ver;
    uint32_t frames;     /* F411 累计收到多少帧 */
    uint32_t lost;       /* seq 缺口累计 */
    uint32_t crc_err;    /* F411 校验失败次数 */
    uint32_t tick_ms;    /* F411 的 HAL_GetTick */
    int16_t  yaw_cdeg;   /* F411 认为的最新 yaw */
    int16_t  pitch_cdeg;
    int16_t  spi_err;
    uint8_t  mode;       /* 0=DEMO 1=TRACK 2=FAULT */
    uint8_t  pad;
    uint16_t crc;
} f411_stat_t;
#pragma pack(pop)

static f411_stat_t s_f411;
static bool s_f411_ok = false;

static uint8_t  s_miso_last[8];
static uint32_t s_miso_bad = 0;
static uint32_t s_miso_crc_bad = 0;

static void f411_parse(const uint8_t *buf, int len)
{
    /* F411 的 MISO 流实测在帧首多一个字节（A5 A5 5A ...），
       在前 3 个位置里找 magic，找到就按它对齐。 */
    for (int i = 0; i < 3 && i + (int)sizeof(f411_stat_t) <= len; i++) {
        if (buf[i] == 0xA5 && buf[i + 1] == 0x5A) { buf += i; len -= i; break; }
    }
    if (len < (int)sizeof(f411_stat_t)) return;
    memcpy(s_miso_last, buf, 8);           /* 记住最近一次回读，便于排查 */
    if (((const f411_stat_t *)buf)->magic != 0x5AA5) s_miso_bad++;
    f411_stat_t t;
    memcpy(&t, buf, sizeof(t));
    if (t.magic != 0x5AA5) return;
    /* 遥测帧尾部偶发错位（SPI 从机 TX 指针）会让 CRC 对不上，
       但前面的 frames/mode/位置字段是可靠的。为了能看到 F411 状态，
       CRC 不符时仍然采用，只累加一个计数——这只影响显示，不影响控制。 */
    uint16_t calc = crc16((const uint8_t *)&t, sizeof(t) - 2);
    if (calc != t.crc) s_miso_crc_bad++;
    s_f411 = t;
    s_f411_ok = true;
}

static bool repack(const wire16_t *in, qp_head_t *out)
{
    if (in->magic != WIRE_MAGIC) return false;
    if (crc16((const uint8_t *)in, 14) != in->crc) return false;

    memset(out, 0, sizeof(*out));
    out->magic      = QP_MAGIC;
    out->ver        = QP_VER;
    out->flags      = (uint8_t)(in->flags & 0xFF);   /* F411 只用低 8 位 */
    out->seq        = in->seq;                       /* 16→32 位扩展 */
    out->t_us       = (uint32_t)esp_timer_get_time();/* 用 C5 的单调时钟打时间戳 */
    out->yaw_cdeg   = in->yaw_cdeg;
    out->pitch_cdeg = in->pitch_cdeg;
    out->yaw_rate   = in->yaw_rate;
    out->pitch_rate = in->pitch_rate;
    out->crc        = crc16((const uint8_t *)out, sizeof(*out) - 2);
    return true;
}

/* ─────────────────────────── WiFi ─────────────────────────── */

static volatile bool s_wifi_up = false;
static char s_ip[32] = "0.0.0.0";

/* 注意：esp_wifi_set_band_mode() 必须在 esp_wifi_start() 之后调用，
   否则返回 ESP_ERR_WIFI_NOT_STARTED(0x3002)。 */
static void apply_band_mode(const char *where)
{
    esp_err_t e = ESP_OK;
#if BRIDGE_BAND == 5
    e = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
    if (e == ESP_OK) ESP_LOGI(TAG, "射频锁定 5 GHz (%s)", where);
#elif BRIDGE_BAND == 2
    e = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (e == ESP_OK) ESP_LOGI(TAG, "射频锁定 2.4 GHz (%s)", where);
#else
    e = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (e == ESP_OK) ESP_LOGI(TAG, "射频 AUTO 2.4+5 GHz (%s)", where);
#endif
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "设置频段失败(%s): %s —— 退回 AUTO", where, esp_err_to_name(e));
        esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        apply_band_mode("sta_start");     /* 必须在 connect 之前 */
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        ESP_LOGW(TAG, "STA 断开，2s 后重连");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_up = true;
        ESP_LOGI(TAG, "STA 已连接，IP=%s，UDP 监听 :%d", s_ip, UDP_PORT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "有设备连上 C5 热点");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGW(TAG, "设备断开 C5 热点");
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));

#if BRIDGE_WIFI_MODE == 2
    /* 纯 ESP-NOW：不连 AP、不开热点，只把射频定在指定信道上收发 */
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_band_mode("espnow");
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    s_wifi_up = true;
    snprintf(s_ip, sizeof(s_ip), "-");
    ESP_LOGI(TAG, "纯 ESP-NOW 模式，信道 %d，UDP 不可用", ESPNOW_CHANNEL);
#elif BRIDGE_WIFI_MODE == 0
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, STA_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, STA_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_band_mode("sta");           /* STA_START 事件里还会再设一次，保险 */
    /* 关省电：否则 WiFi 会周期性休眠，延迟从几 ms 抖到几十 ms */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "STA 模式，连接 \"%s\" …", STA_SSID);
#else
    esp_netif_create_default_wifi_ap();
    wifi_config_t wc = {0};
    strncpy((char *)wc.ap.ssid, AP_SSID, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)wc.ap.password, AP_PASS, sizeof(wc.ap.password) - 1);
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    /* 顺序很重要：C5 默认频段是 5G_ONLY，先把射频切到 2.4G 再设 AP 信道，
       否则 set_config 会报 "Channel 6 (2G) is not supported in current band mode"。
       set_band_mode 又必须在 start() 之后才有效，所以是 start → 切频段 → set_config。 */
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_band_mode("ap");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    s_wifi_up = true;
    snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    uint8_t ch = 0; wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch, &sec);
    ESP_LOGI(TAG, "AP 模式，SSID=\"%s\"，信道 %d，IP %s，UDP :%d",
             AP_SSID, ch, s_ip, UDP_PORT);
#endif
}

/* ─────────────────────────── SPI ─────────────────────────── */

static spi_device_handle_t s_spi;

static void spi_start(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,    /* 回读 F411 统计帧 */
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_FREQ_HZ,
        .mode = 0,                  /* CPOL=0, CPHA=0 —— 必须和 F411 一致 */
        .spics_io_num = PIN_CS,
        .queue_size = 4,
        .command_bits = 0,
        .address_bits = 0,
        /* CS 前后各留 1 个时钟的余量：从机（F411）靠 CS 边沿复位移位器，
           没有这段余量时实测会出现整帧 1 字节错位 */
        .cs_ena_pretrans  = 1,
        .cs_ena_posttrans = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));
    ESP_LOGI(TAG, "SPI2 主机 %.1f MHz, mode0, CS=GPIO%d", SPI_FREQ_HZ / 1e6, PIN_CS);
}

static uint8_t s_rxbuf[64];

static bool spi_send(const qp_head_t *f)
{
    static uint8_t tx[40];
    memcpy(tx, f, sizeof(*f));
    memset(tx + sizeof(*f), 0, sizeof(tx) - sizeof(*f));

    spi_transaction_t t = {
        .length    = 8 * 31,           /* 31 字节：从机帧首会多一个字节，解析时跳过 */
        .tx_buffer = tx,
        .rx_buffer = s_rxbuf,          /* 同时收回 F411 的统计帧 */
    };
    bool ok = spi_device_polling_transmit(s_spi, &t) == ESP_OK;
    if (ok) f411_parse(s_rxbuf, (int)(t.length / 8));   /* 传真实字节数，别用 sizeof(结构) */
    return ok;
}

/* ─────────────────────────── 收包处理（UDP / ESP-NOW 共用） ─────────────────────────── */

static uint32_t s_rx, s_ok, s_crc, s_len, s_spi_err;          /* 每秒增量 */
volatile uint32_t g_rx_total, g_ok_total, g_crc_total, g_len_total, g_spi_total;  /* 累计 */
static void handle_frame(const uint8_t *buf, int len);
#include "bridge.h"

/* 一个报文可以是 1..N 个 16B 帧（批量发送，减少空口竞争次数） */
void bridge_feed(const uint8_t *buf, int len)
{
    if (len > 0 && (len % (int)sizeof(wire16_t)) == 0) {
        for (int off = 0; off + (int)sizeof(wire16_t) <= len; off += (int)sizeof(wire16_t)) {
            handle_frame(buf + off, (int)sizeof(wire16_t));
        }
        return;
    }
    handle_frame(buf, len);          /* 30B 旧格式 / 异常长度走原路径 */
}

static void handle_frame(const uint8_t *buf, int len)
{
    s_rx++; g_rx_total++;

    qp_head_t out;
    bool ok = false;

    if (len == (int)sizeof(wire16_t)) {
        wire16_t in;
        memcpy(&in, buf, sizeof(in));
        ok = repack(&in, &out);
        if (!ok) { s_crc++; g_crc_total++; }
    } else if (len == (int)sizeof(qp_head_t)) {
        memcpy(&out, buf, sizeof(out));
        ok = (out.magic == QP_MAGIC && out.ver == QP_VER &&
              crc16((const uint8_t *)&out, sizeof(out) - 2) == out.crc);
        if (!ok) { s_crc++; g_crc_total++; }
    } else {
        s_len++; g_len_total++;
    }

    if (ok) {
        if (spi_send(&out)) { s_ok++; g_ok_total++; }
        else                { s_spi_err++; g_spi_total++; }
    }
}

#if BRIDGE_LINK_ESPNOW
static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    bridge_feed(data, len);
}
#endif

#if BRIDGE_LINK_ESPNOW
static void espnow_start(void)
{
    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, ESPNOW_PEER_MAC, 6);
    peer.channel = 0;                 /* 0 = 跟随当前信道 */
    peer.ifidx   = (BRIDGE_WIFI_MODE == 0) ? WIFI_IF_STA : WIFI_IF_AP;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_espnow_recv));
    ESP_LOGI(TAG, "ESP-NOW 已启动（%s，2.4G 射频）", ESPNOW_USE_BROADCAST ? "广播" : "点对点");
}
#endif

/* ─────────────────────────── 主循环 ─────────────────────────── */

void app_main(void)
{
    /* 系统日志压到 WARN，自己的统计照常输出 */
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("bridge", ESP_LOG_INFO);
    esp_log_level_set("ble_test", ESP_LOG_INFO);
    esp_log_level_set("httpd", ESP_LOG_INFO);   /* 状态页/HTTPS 的启动日志 */
    esp_log_level_set("wifi", ESP_LOG_ERROR);   /* WiFi 的 trace 警告太吵 */
    esp_log_level_set("esp_netif", ESP_LOG_ERROR);

    /* NVS 是 WiFi 驱动的硬依赖 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    spi_start();

    /* 只做 BLE 测试时不必开 WiFi —— 单射频，WiFi 会抢 BLE 的时隙，把延迟测歪 */
#if BRIDGE_LINK_UDP || BRIDGE_LINK_ESPNOW || BRIDGE_ENABLE_HTTP
    wifi_start();
#endif

#if BRIDGE_ENABLE_BLE_TEST
    ble_test_start();
#endif

#if BRIDGE_ENABLE_HTTP
    http_status_start(AP_SSID, AP_CHANNEL, s_ip);
#endif

#if BRIDGE_LINK_ESPNOW
    espnow_start();
#endif

#if BRIDGE_LINK_UDP
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket 创建失败: errno=%d", errno);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind 失败: errno=%d", errno);
        close(sock);
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "UDP 已监听 :%d", UDP_PORT);
#endif

    uint8_t buf[64];
    int64_t t_stat = esp_timer_get_time();

    while (1) {
#if BRIDGE_LINK_UDP
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (len > 0) {
            bridge_feed(buf, len);
        }
#else
        vTaskDelay(pdMS_TO_TICKS(50));   /* 只走 ESP-NOW 时，这里空转即可 */
#endif

        int64_t now = esp_timer_get_time();
        if (now - t_stat >= STAT_INTERVAL_US) {
            /* 计数是"本窗口累计值"，必须除以真实窗口长度才是每秒帧数 */
            uint32_t window_us = (uint32_t)(now - t_stat);
            uint32_t rx_ps  = s_rx  ? (uint32_t)((uint64_t)s_rx  * 1000000ull / window_us) : 0;
            uint32_t ok_ps  = s_ok  ? (uint32_t)((uint64_t)s_ok  * 1000000ull / window_us) : 0;
            if (s_rx == 0) {
                static qp_head_t dummy;
                static uint16_t dseq;
                memset(&dummy, 0, sizeof(dummy));
                dummy.magic = QP_MAGIC;
                dummy.ver   = QP_VER;
                dummy.flags = 0;
                dummy.seq   = dseq++;
                dummy.t_us   = (uint32_t)esp_timer_get_time();
                dummy.yaw_cdeg = 0;
                dummy.crc   = crc16((const uint8_t *)&dummy, sizeof(dummy) - 2);
                spi_send(&dummy);      /* 合法空帧：没有输入时也能验证整条 SPI 链 */
            }
            if (s_f411_ok) {
                ESP_LOGI(TAG, "F411 %s | 收 %u 帧 | 丢 %u | 应答 %u/%u | pos %d.%d° / %d.%d°",
                         s_f411.mode == 1 ? "TRACK" : (s_f411.mode == 2 ? "FAULT" : "DEMO"),
                         (unsigned)s_f411.frames, (unsigned)s_f411.lost,
                         (unsigned)(s_f411.crc_err >> 16), (unsigned)(s_f411.crc_err & 0xFFFF),
                         s_f411.yaw_cdeg / 10, abs(s_f411.yaw_cdeg % 10),
                         s_f411.pitch_cdeg / 10, abs(s_f411.pitch_cdeg % 10));
            } else {
                ESP_LOGW(TAG, "F411 无回读 | 原始字节 %02X %02X %02X %02X %02X %02X %02X %02X | 不匹配 %u 次",
                         s_miso_last[0], s_miso_last[1], s_miso_last[2], s_miso_last[3],
                         s_miso_last[4], s_miso_last[5], s_miso_last[6], s_miso_last[7],
                         (unsigned)s_miso_bad);
            }
            /* 只用不遍历堆链表的接口：heap_caps_get_largest_free_block 会
               tlsf_walk_pool 扫描空闲链表，并发分配时实测会 panic。 */
            ESP_LOGI(TAG, "收 %4u/s | 转发 %4u/s | 窗口 %u 帧 | crc错 %u | 长度错 %u | SPI错 %u | 堆 %u / 内部 %u B",
                     (unsigned)rx_ps, (unsigned)ok_ps, (unsigned)s_rx,
                     (unsigned)s_crc, (unsigned)s_len, (unsigned)s_spi_err,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_rx = s_ok = s_crc = s_len = s_spi_err = 0;
            t_stat = now;

#if BRIDGE_ENABLE_BLE_TEST
            ble_test_stats();
#endif
        }
    }
}
