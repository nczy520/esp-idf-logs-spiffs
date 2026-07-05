/**
 * @file web_ui.c
 * @brief Web UI 处理器（HTML 页面）
 *
 * @author 路灯
 * @version 2.0.1
 * @date 2026-07-06
 * @contact support@mdeve.com
 */
#include "logs_storage_internal.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "web_ui_lang.h"
#include <string.h>

static const char *TAG = "LOG_WEB_UI";

/* 前向声明 */
static esp_err_t index_handler(httpd_req_t *req);

/* 通用 Captive Portal 处理 - 返回 302 重定向到主页 */
static esp_err_t captive_portal_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/index.html");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<html><body>Redirecting...</body></html>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Apple iOS 专用处理 - 返回 Success 页面 */
static esp_err_t apple_captive_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 重定向到主页 */
static esp_err_t redirect_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/index.html");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "Redirecting...", strlen("Redirecting..."));
    return ESP_OK;
}

/* 主页 HTML */
static esp_err_t index_handler(httpd_req_t *req) {
    /* 获取 AP IP 地址 */
    char ip_str[32] = "192.168.4.1";
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                 (unsigned)((ip_info.ip.addr >> 0) & 0xFF),
                 (unsigned)((ip_info.ip.addr >> 8) & 0xFF),
                 (unsigned)((ip_info.ip.addr >> 16) & 0xFF),
                 (unsigned)((ip_info.ip.addr >> 24) & 0xFF));
    }

    /* 使用 httpd_resp_sendstr_chunk 避免 snprintf 格式问题 */
    httpd_resp_set_type(req, "text/html");
    
    /* 发送 HTML 头部 */
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html><head>");
    httpd_resp_sendstr_chunk(req, "<meta charset='utf-8'>");
    httpd_resp_sendstr_chunk(req, "<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'>");
    httpd_resp_sendstr_chunk(req, "<title>ESP Logs</title>");
    httpd_resp_sendstr_chunk(req, "<style>");
    httpd_resp_sendstr_chunk(req, "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}");
    httpd_resp_sendstr_chunk(req, "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:8px;background:#f0f2f5;color:#333;font-size:14px;line-height:1.5}");
    httpd_resp_sendstr_chunk(req, ".container{max-width:100%;margin:0 auto}");
    httpd_resp_sendstr_chunk(req, ".header{background:#2196F3;color:#fff;padding:12px 16px;border-radius:12px;margin-bottom:12px;text-align:center}");
    httpd_resp_sendstr_chunk(req, ".header h1{font-size:18px;margin:0 0 4px 0}");
    httpd_resp_sendstr_chunk(req, ".header .ip{font-size:13px;opacity:.9}");
    httpd_resp_sendstr_chunk(req, ".card{background:#fff;padding:12px;margin:8px 0;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.08)}");
    httpd_resp_sendstr_chunk(req, ".card-title{font-size:16px;font-weight:600;margin:0 0 12px 0;color:#1a1a1a}");
    httpd_resp_sendstr_chunk(req, ".stats{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}");
    httpd_resp_sendstr_chunk(req, ".stat{padding:10px;background:#f8f9fa;border-radius:8px;text-align:center}");
    httpd_resp_sendstr_chunk(req, ".stat b{display:block;color:#666;font-size:11px;margin-bottom:4px}");
    httpd_resp_sendstr_chunk(req, ".stat span{font-size:15px;font-weight:600;color:#1a1a1a}");
    httpd_resp_sendstr_chunk(req, ".stat.primary{background:#e3f2fd}");
    httpd_resp_sendstr_chunk(req, ".stat.success{background:#e8f5e9}");
    httpd_resp_sendstr_chunk(req, ".stat.warning{background:#fff3e0}");
    httpd_resp_sendstr_chunk(req, ".stat.danger{background:#fce4ec}");
    httpd_resp_sendstr_chunk(req, "table{width:100%;border-collapse:collapse;font-size:13px;margin-top:8px}");
    httpd_resp_sendstr_chunk(req, "th,td{padding:10px 8px;text-align:left;border-bottom:1px solid #eee}");
    httpd_resp_sendstr_chunk(req, "th{font-weight:600;color:#666;font-size:12px;background:#f8f9fa}");
    httpd_resp_sendstr_chunk(req, "td{word-break:break-all}");
    httpd_resp_sendstr_chunk(req, ".btn{padding:8px 16px;margin:2px;border:none;border-radius:6px;cursor:pointer;color:#fff;font-size:13px;font-weight:500;transition:opacity .2s}");
    httpd_resp_sendstr_chunk(req, ".btn:active{opacity:.8}");
    httpd_resp_sendstr_chunk(req, ".btn-dl{background:#2196F3}");
    httpd_resp_sendstr_chunk(req, ".btn-clear{background:#FF9800}");
    httpd_resp_sendstr_chunk(req, ".btn-restart{background:#F44336}");
    httpd_resp_sendstr_chunk(req, ".actions{display:flex;gap:8px;flex-wrap:wrap}");
    httpd_resp_sendstr_chunk(req, ".empty-state{text-align:center;padding:24px;color:#999}");
    httpd_resp_sendstr_chunk(req, ".file-size{color:#666;font-size:12px}");
    httpd_resp_sendstr_chunk(req, ".file-name{font-weight:500;color:#1a1a1a}");
    httpd_resp_sendstr_chunk(req, "@media(min-width:768px){.stats{grid-template-columns:repeat(3,1fr)}.container{max-width:900px;margin:0 auto}}");
    httpd_resp_sendstr_chunk(req, "</style></head><body>");
    
    /* 发送页面内容 */
    httpd_resp_sendstr_chunk(req, "<div class='container'>");
    
    /* 发送IP地址 */
    char header[256];
    snprintf(header, sizeof(header), "<div class='header'><h1>%s</h1><div class='ip'>%s: http://%s:8080</div></div>", 
             TXT_TITLE, TXT_ACCESS_ADDR, ip_str);
    httpd_resp_sendstr_chunk(req, header);
    
    httpd_resp_sendstr_chunk(req, "<div class='card'>");
    char stats_title[128];
    snprintf(stats_title, sizeof(stats_title), "<h2 class='card-title'>%s</h2>", TXT_STORAGE_STATS);
    httpd_resp_sendstr_chunk(req, stats_title);
    httpd_resp_sendstr_chunk(req, "<div class='stats' id='stats'>");
    httpd_resp_sendstr_chunk(req, "<div class='stat primary'><b>总容量</b><span>加载中...</span></div>");
    httpd_resp_sendstr_chunk(req, "<div class='stat success'><b>已使用</b><span>--</span></div>");
    httpd_resp_sendstr_chunk(req, "<div class='stat warning'><b>剩余空间</b><span>--</span></div>");
    httpd_resp_sendstr_chunk(req, "<div class='stat danger'><b>文件数量</b><span id='file-count'>--</span></div>");
    httpd_resp_sendstr_chunk(req, "</div></div>");
    
    httpd_resp_sendstr_chunk(req, "<div class='card'>");
    char list_title[128];
    snprintf(list_title, sizeof(list_title), "<h2 class='card-title'>%s</h2>", TXT_FILE_LIST);
    httpd_resp_sendstr_chunk(req, list_title);
    httpd_resp_sendstr_chunk(req, "<table id='filelist'>");
    httpd_resp_sendstr_chunk(req, "<tr><th>文件名</th><th>大小</th><th>操作</th></tr>");
    httpd_resp_sendstr_chunk(req, "<tr><td colspan='3' class='empty-state'>加载中...</td></tr>");
    httpd_resp_sendstr_chunk(req, "</table></div>");
    
    httpd_resp_sendstr_chunk(req, "<div class='card actions' style='justify-content:space-between;align-items:center'>");
    httpd_resp_sendstr_chunk(req, "<div style='display:flex;gap:8px;flex-wrap:wrap'>");
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-clear' onclick='clearLogs()'>清除所有日志</button>");
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-restart' onclick='restart()'>重启设备</button></div>");
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-dl' onclick='refreshList()' style='margin-left:auto'>刷新列表</button></div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    
    /* 发送 JavaScript - 使用语言对象实现动态切换 */
    httpd_resp_sendstr_chunk(req, "<script>");
    httpd_resp_sendstr_chunk(req, "const lang={");
    httpd_resp_sendstr_chunk(req, "total:'总容量',used:'已使用',free:'剩余空间',fileCount:'文件数量',");
    httpd_resp_sendstr_chunk(req, "fileName:'文件名',size:'大小',operation:'操作',download:'下载',");
    httpd_resp_sendstr_chunk(req, "clearLogs:'清除所有日志',restart:'重启设备',refresh:'刷新列表',");
    httpd_resp_sendstr_chunk(req, "loading:'加载中...',noFiles:'暂无日志文件',");
    httpd_resp_sendstr_chunk(req, "confirmClear:'确定要清除所有日志并重启设备吗？',cleared:'日志已清除，设备正在重启...',");
    httpd_resp_sendstr_chunk(req, "confirmRestart:'确定要重启设备吗？'");
    httpd_resp_sendstr_chunk(req, "};");
    httpd_resp_sendstr_chunk(req, "function fmt(n){if(n<1024)return n+' B';if(n<1048576)return(n/1024).toFixed(1)+' KB';return(n/1048576).toFixed(2)+' MB'}");
    httpd_resp_sendstr_chunk(req, "var fileCount=0;");
    httpd_resp_sendstr_chunk(req, "function loadStats(){fetch('/api/stats').then(r=>r.json()).then(d=>{");
    httpd_resp_sendstr_chunk(req, "document.getElementById('stats').innerHTML=");
    httpd_resp_sendstr_chunk(req, "'<div class=\"stat primary\"><b>'+lang.total+'</b><span>'+fmt(d.total_bytes)+'</span></div>'");
    httpd_resp_sendstr_chunk(req, "+'<div class=\"stat success\"><b>'+lang.used+'</b><span>'+fmt(d.used_bytes)+'</span></div>'");
    httpd_resp_sendstr_chunk(req, "+'<div class=\"stat warning\"><b>'+lang.free+'</b><span>'+fmt(d.free_bytes)+'</span></div>'");
    httpd_resp_sendstr_chunk(req, "+'<div class=\"stat danger\"><b>'+lang.fileCount+'</b><span>'+(fileCount||d.file_count||0)+'</span></div>';})}");
    httpd_resp_sendstr_chunk(req, "function loadList(){fetch('/api/list').then(r=>r.json()).then(d=>{");
    httpd_resp_sendstr_chunk(req, "if(d.length===0){document.getElementById('filelist').innerHTML='<tr><th>'+lang.fileName+'</th><th>'+lang.size+'</th><th>'+lang.operation+'</th></tr><tr><td colspan=3 class=empty-state>'+lang.noFiles+'</td></tr>';return}");
    httpd_resp_sendstr_chunk(req, "fileCount=d.length;");
    httpd_resp_sendstr_chunk(req, "let html='<tr><th>'+lang.fileName+'</th><th>'+lang.size+'</th><th>'+lang.operation+'</th></tr>';");
    httpd_resp_sendstr_chunk(req, "d.forEach(f=>{html+='<tr><td class=\"file-name\">'+f.name+'</td><td class=\"file-size\">'+fmt(f.size)+'</td>'");
    httpd_resp_sendstr_chunk(req, "+'<td><a href=\"/api/download?file='+encodeURIComponent(f.name)+'\" style=display:inline-block><button class=\"btn btn-dl\">'+lang.download+'</button></a></td></tr>'});");
    httpd_resp_sendstr_chunk(req, "document.getElementById('filelist').innerHTML=html;loadStats();})}");
    httpd_resp_sendstr_chunk(req, "function clearLogs(){if(confirm(lang.confirmClear)){fetch('/api/clear',{method:'POST'}).then(()=>{alert(lang.cleared);})}}");
    httpd_resp_sendstr_chunk(req, "function restart(){if(confirm(lang.confirmRestart)){fetch('/api/restart',{method:'POST'})}}");
    httpd_resp_sendstr_chunk(req, "function refreshList(){loadList();}");
    httpd_resp_sendstr_chunk(req, "loadStats();loadList();setInterval(loadStats,5000);");
    httpd_resp_sendstr_chunk(req, "</script></body></html>");
    
    /* 结束响应 */
    httpd_resp_sendstr_chunk(req, NULL);
    
    return ESP_OK;
}

/* 注册所有 UI 路由 */
bool web_ui_register_routes(httpd_handle_t server) {
    httpd_uri_t uris[] = {
        { .uri = "/",               .method = HTTP_GET,  .handler = redirect_handler,     .user_ctx = NULL },
        { .uri = "/index.html",    .method = HTTP_GET,  .handler = index_handler,       .user_ctx = NULL },
        /* Captive Portal 检测 URL */
        { .uri = "/generate_204",  .method = HTTP_GET,  .handler = captive_portal_handler, .user_ctx = NULL },
        { .uri = "/gen_204",       .method = HTTP_GET,  .handler = captive_portal_handler, .user_ctx = NULL },
        /* Apple iOS 检测 */
        { .uri = "/hotspot-detect.html", .method = HTTP_GET,  .handler = apple_captive_handler, .user_ctx = NULL },
        { .uri = "/library/test/success.html", .method = HTTP_GET,  .handler = apple_captive_handler, .user_ctx = NULL },
        /* Android 检测 */
        { .uri = "/connectivitycheck.html", .method = HTTP_GET,  .handler = captive_portal_handler, .user_ctx = NULL },
        { .uri = "/success.txt",   .method = HTTP_GET,  .handler = captive_portal_handler, .user_ctx = NULL },
        /* Windows 检测 */
        { .uri = "/ncsi.txt",      .method = HTTP_GET,  .handler = captive_portal_handler, .user_ctx = NULL },
        { .uri = "/connecttest.txt", .method = HTTP_GET,  .handler = captive_portal_handler, .user_ctx = NULL },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register UI route: %s", uris[i].uri);
            return false;
        }
    }
    return true;
}
