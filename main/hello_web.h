#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * 注册自定义 HTTP 端点（示例模板，展示如何在 web_platform 基础上添加业务）。
 *
 * 在 web_platform_init() 之后、web_platform_register_static_fallback() 之前调用。
 *
 * 用法：
 *   web_platform_init();
 *   hello_web_register(web_platform_get_server());          // ← 你的业务
 *   web_platform_register_static_fallback();                // 必须最后
 */
esp_err_t hello_web_register(httpd_handle_t server);
