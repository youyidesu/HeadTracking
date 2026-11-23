/**
 * http_status.c —— 板子状态页（192.168.4.1）
 *
 *   GET /      每秒自动刷新（前端 fetch /json，不整页重载）
 *   GET /json  纯 JSON，给脚本/curl 用
 *
 * 之前那版是纯静态页，所以运行时长必须手动按刷新才变——这里改成 1 Hz 局部刷新。
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "web_page.h"
#include "certs/cert_pem.h"

#include "http_status.h"
#include "bridge.h"

static const char *TAG = "httpd";

static char s_ssid[33] = "-";
static int  s_channel = 0;
static char s_ip[32] = "-";

static esp_err_t json_get(httpd_req_t *req)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"ssid\":\"%s\",\"ch\":%d,\"ip\":\"%s\",\"up\":%lld,"
        "\"rx\":%lu,\"ok\":%lu,\"crc\":%lu,\"len\":%lu,\"spi\":%lu,"
        "\"heap\":%lu,\"largest\":%lu}",
        s_ssid, s_channel, s_ip,
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned long)g_rx_total, (unsigned long)g_ok_total,
        (unsigned long)g_crc_total, (unsigned long)g_len_total, (unsigned long)g_spi_total,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t root_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET / （状态页被请求）");
    static const char PAGE[] =
        "<!doctype html><meta charset=utf-8><title>HT-TURRET</title>"
        "<body style='font:14px/1.9 monospace;background:#111;color:#0f0;padding:20px'>"
        "<h2 style='margin:0 0 14px'>ESP32-C5 头追桥接节点</h2>"
        "<div>SSID&nbsp;&nbsp;: <span id=ssid>-</span></div>"
        "<div>信道&nbsp;&nbsp;: <span id=ch>-</span></div>"
        "<div>IP&nbsp;&nbsp;&nbsp;&nbsp;: <span id=ip>-</span></div>"
        "<div>运行&nbsp;&nbsp;: <span id=up>-</span> s</div><br>"
        "<div>累计收 : <span id=rx>0</span> 帧</div>"
        "<div>已转发 : <span id=ok>0</span></div>"
        "<div>CRC错&nbsp;: <span id=crc>0</span></div>"
        "<div>长度错 : <span id=len>0</span></div>"
        "<div>SPI错&nbsp;: <span id=spi>0</span></div><br>"
        "<div>堆&nbsp;&nbsp;&nbsp;&nbsp;: <span id=heap>-</span> B（最大块 <span id=largest>-</span>）</div>"
        "<div id=st style='color:#888;margin-top:10px'>每秒自动刷新</div>"
        "<script>"
        "const $=i=>document.getElementById(i);"
        "async function tick(){try{"
        "const r=await fetch('/json',{cache:'no-store'});const j=await r.json();"
        "$('ssid').textContent=j.ssid;$('ch').textContent=j.ch;$('ip').textContent=j.ip;"
        "$('up').textContent=j.up;$('rx').textContent=j.rx;$('ok').textContent=j.ok;"
        "$('crc').textContent=j.crc;$('len').textContent=j.len;$('spi').textContent=j.spi;"
        "$('heap').textContent=j.heap;$('largest').textContent=j.largest;"
        "$('st').textContent='更新于 '+new Date().toLocaleTimeString();"
        "}catch(e){$('st').textContent='连接断开';}}"
        "tick();setInterval(tick,1000);"
        "</script>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t xr_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /xr （页面被请求）");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)XR_PAGE, XR_PAGE_LEN);
}

/* ─────────── WebSocket：浏览器把 16 B 帧从这里灌进来 ─────────── */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS 握手完成（升级成功）");
        return ESP_OK;
    }

    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_BINARY };
    esp_err_t ret = httpd_ws_recv_frame(req, &f, 0);   /* 先问长度 */
    if (ret != ESP_OK || f.len == 0) return ret;
    if (f.len > 256) return ESP_ERR_INVALID_SIZE;

    uint8_t buf[256];
    f.payload = buf;
    ret = httpd_ws_recv_frame(req, &f, f.len);
    if (ret == ESP_OK) {
        static uint32_t n = 0;
        if (n < 3) { ESP_LOGI(TAG, "WS 收到 %d 字节（第 %u 帧）", (int)f.len, (unsigned)(n + 1)); }
        n++;
        bridge_feed(buf, (int)f.len);                  /* 校验 → 重打包 → SPI */
    }
    return ret;
}

/* 明文口访问 /xr 时直接跳到 https，省得用户手打协议名 */
static esp_err_t xr_redirect(httpd_req_t *req)
{
    char url[64];
    snprintf(url, sizeof(url), "https://%s/xr", s_ip);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void http_status_start(const char *ssid, int channel, const char *ip)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid ? ssid : "-");
    snprintf(s_ip, sizeof(s_ip), "%s", ip ? ip : "-");
    s_channel = channel;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "HTTP 启动失败（可能端口被占）");
        return;
    }

    httpd_uri_t u1 = { .uri = "/",     .method = HTTP_GET, .handler = root_get };
    httpd_uri_t u2 = { .uri = "/json", .method = HTTP_GET, .handler = json_get };
    httpd_uri_t u3 = { .uri = "/xr",   .method = HTTP_GET, .handler = xr_get };
    httpd_uri_t u3r = { .uri = "/xr",  .method = HTTP_GET, .handler = xr_redirect };
    httpd_uri_t u4  = { .uri = "/ws",  .method = HTTP_GET, .handler = ws_handler,
                        .is_websocket = true };
    httpd_register_uri_handler(srv, &u1);
    httpd_register_uri_handler(srv, &u2);
    httpd_register_uri_handler(srv, &u3r);
    httpd_register_uri_handler(srv, &u4);

    ESP_LOGI(TAG, "状态页 http://%s/  ·  XR 测试页 http://%s/xr", s_ip, s_ip);
    ESP_LOGI(TAG, "WebSocket 端点 ws://%s/ws（wss://%s/ws 走 TLS）", s_ip, s_ip);

    if (HT_CERT_LEN > 16) {   /* 运行时判断：sizeof 不能用在 #if 里 */
    /* 有证书就再开一个 TLS 端口：WebXR / Web Bluetooth 需要安全上下文 */
    httpd_ssl_config_t scfg = HTTPD_SSL_CONFIG_DEFAULT();
    scfg.servercert = HT_CERT_PEM;
    scfg.servercert_len = HT_CERT_LEN;
    scfg.prvtkey_pem = HT_KEY_PEM;
    scfg.prvtkey_len = HT_KEY_LEN;
    scfg.port_secure = 443;

    httpd_handle_t srv_s = NULL;
    esp_err_t herr = httpd_ssl_start(&srv_s, &scfg);
    if (herr == ESP_OK) {
        httpd_register_uri_handler(srv_s, &u1);
        httpd_register_uri_handler(srv_s, &u2);
        httpd_register_uri_handler(srv_s, &u3);
        httpd_register_uri_handler(srv_s, &u4);   /* wss://192.168.4.1/ws */
        ESP_LOGI(TAG, "HTTPS 已开：https://%s/xr  （头显要用这个）", s_ip);
    } else {
        ESP_LOGW(TAG, "HTTPS 启动失败: %s (0x%x) —— 证书格式或内存问题，只跑 HTTP",
                 esp_err_to_name(herr), herr);
    }
    } else {
        ESP_LOGW(TAG, "没有证书 → 只有 HTTP，WebXR/WebBluetooth 在 http 下不可用");
    }
}
