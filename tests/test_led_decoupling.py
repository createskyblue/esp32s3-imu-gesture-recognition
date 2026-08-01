import json
import re
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
LED_COMPONENT = PROJECT_ROOT / "components" / "led_task"

LED_REFERENCE = re.compile(
    r"\bled_task(?:_init)?\b|"
    r"\bled_(?:send_cmd|fatal_error|cmd_t)\b|"
    r"\bLED_(?:RED|YELLOW|GREEN|BLUE|COUNT|CMD_[A-Z_]+)\b"
)
BOARD_LED_GPIO = re.compile(r"\bGPIO_NUM_(?:5|6|7|15)\b")
WIFI_PLATFORM_COUPLING = re.compile(
    r"sd_logger|cJSON|wifi_config\.h|WIFI_CONFIG_PATH|/littlefs|"
    r"\bfopen\s*\(|\bfputs\s*\("
)


def production_sources(root: Path):
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix in {".c", ".h"} or path.name == "CMakeLists.txt":
            yield path


def matches_in(paths, pattern):
    matches = []
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for line_number, line in enumerate(text.splitlines(), start=1):
            if pattern.search(line):
                matches.append(f"{path.relative_to(PROJECT_ROOT)}:{line_number}: {line.strip()}")
    return matches


class LedDecouplingTests(unittest.TestCase):
    def test_default_config_does_not_require_application_idle_hook(self):
        defaults = (PROJECT_ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")

        self.assertNotRegex(defaults, r"(?m)^CONFIG_FREERTOS_USE_IDLE_HOOK=y$")

    def test_platform_does_not_reference_or_claim_leds(self):
        platform_sources = [
            path
            for path in production_sources(PROJECT_ROOT / "components")
            if LED_COMPONENT not in path.parents
        ]
        platform_sources.extend(
            [
                PROJECT_ROOT / "main" / "web_platform.c",
                PROJECT_ROOT / "main" / "web_platform.h",
            ]
        )
        violations = matches_in(platform_sources, LED_REFERENCE)
        violations.extend(matches_in(platform_sources, BOARD_LED_GPIO))

        self.assertEqual([], violations)

    def test_optional_led_component_does_not_reserve_green_led(self):
        source = (LED_COMPONENT / "led_task.c").read_text(encoding="utf-8")

        self.assertNotRegex(source, r"(?:cmd->led|i)\s*==\s*LED_GREEN")

    def test_main_starts_green_led_heartbeat_at_two_hz(self):
        source = (PROJECT_ROOT / "main" / "main.c").read_text(encoding="utf-8")
        cmake = (PROJECT_ROOT / "main" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertRegex(source, r'(?m)^#include\s+"led_task\.h"$')
        self.assertRegex(cmake, r"\bled_task\b")
        self.assertIn("ESP_ERROR_CHECK(led_task_init())", source)
        self.assertIn(".led = LED_GREEN", source)
        self.assertIn(".type = LED_CMD_BLINK", source)
        self.assertIn(".period_ms = 500u", source)
        self.assertIn(".on_ms = 250u", source)
        self.assertIn("led_send_cmd(&heartbeat)", source)
        self.assertLess(
            source.index("app_storage_init()"), source.index("led_task_init()")
        )


class ComponentBoundaryTests(unittest.TestCase):
    def test_wifi_manager_has_no_storage_or_sd_logger_dependency(self):
        wifi_sources = list(
            production_sources(PROJECT_ROOT / "components" / "wifi_manager")
        )

        self.assertEqual([], matches_in(wifi_sources, WIFI_PLATFORM_COUPLING))

    def test_wifi_manager_accepts_caller_provided_credentials(self):
        header = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.h"
        ).read_text(encoding="utf-8")

        self.assertIn("wifi_manager_credentials_t", header)
        self.assertIn("wifi_manager_config_t", header)
        self.assertRegex(
            header,
            r"wifi_manager_init\s*\(\s*const wifi_manager_config_t\s*\*",
        )
        self.assertRegex(
            header,
            r"wifi_manager_set_credentials\s*\(\s*const wifi_manager_credentials_t\s*\*",
        )

    def test_main_mounts_storage_before_loading_and_starting_wifi(self):
        source = (PROJECT_ROOT / "main" / "main.c").read_text(encoding="utf-8")
        required = [
            "app_storage_init()",
            "wifi_config_store_load(&wifi_config.sta)",
            "wifi_manager_init(&wifi_config)",
        ]
        for token in required:
            self.assertIn(token, source)

        self.assertLess(source.index(required[0]), source.index("setenv("))
        self.assertLess(source.index(required[0]), source.index(required[1]))
        self.assertLess(source.index(required[1]), source.index(required[2]))

    def test_application_storage_is_the_littlefs_mount_owner(self):
        storage_source = PROJECT_ROOT / "main" / "app_storage.c"
        self.assertTrue(storage_source.exists())
        source = storage_source.read_text(encoding="utf-8")
        self.assertIn("esp_vfs_littlefs_register", source)

        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("esp_vfs_littlefs_register", web_source)

    def test_wifi_config_transaction_lives_in_store(self):
        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )
        store_source = (PROJECT_ROOT / "main" / "wifi_config_store.c").read_text(
            encoding="utf-8"
        )

        # web_platform delegates the whole transaction to one composite API.
        self.assertIn("wifi_config_store_apply_credentials(&wifi_credentials)", web_source)
        self.assertNotIn("wifi_config_store_stage", web_source)
        self.assertNotIn("wifi_config_store_commit", web_source)
        self.assertNotIn("wifi_config_store_discard", web_source)

        # The transaction (stage -> snapshot -> apply -> commit, with rollback)
        # lives in the store.
        apply_fn = store_source[
            store_source.index("esp_err_t wifi_config_store_apply_credentials") :
        ]
        for token in [
            "stage_unlocked(credentials)",
            "wifi_manager_get_credentials(&previous)",
            "wifi_manager_set_credentials(credentials)",
            "commit_unlocked()",
            "wifi_manager_set_credentials(&previous)",
            "wifi_manager_enter_provisioning_mode()",
        ]:
            self.assertIn(token, apply_fn)
        self.assertLess(apply_fn.index("stage_unlocked(credentials)"),
                        apply_fn.index("wifi_manager_get_credentials(&previous)"))
        self.assertLess(apply_fn.index("wifi_manager_get_credentials(&previous)"),
                        apply_fn.index("wifi_manager_set_credentials(credentials)"))
        self.assertLess(apply_fn.index("wifi_manager_set_credentials(credentials)"),
                        apply_fn.index("commit_unlocked()"))

    def test_wifi_startup_policy_is_application_configured(self):
        header = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.h"
        ).read_text(encoding="utf-8")
        source = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.c"
        ).read_text(encoding="utf-8")
        main_source = (PROJECT_ROOT / "main" / "main.c").read_text(
            encoding="utf-8"
        )
        store_header = (PROJECT_ROOT / "main" / "wifi_config_store.h").read_text(
            encoding="utf-8"
        )

        for field in [
            "ap_ssid",
            "ap_password",
            "ap_channel",
            "ap_max_connections",
            "captive_portal_dns_enabled",
            "sntp_server",
        ]:
            self.assertIn(field, header)
        for hard_coded_policy in [
            "ESP32S3-Template",
            "template1234",
            "ntp.aliyun.com",
            "#define WIFI_AP_SSID",
            "#define WIFI_AP_PASS",
        ]:
            self.assertNotIn(hard_coded_policy, source)
        self.assertIn("wifi_manager_config_t wifi_config = {", main_source)
        self.assertIn('.ap_ssid = "ESP32S3-Template"', main_source)
        self.assertIn('.sntp_server = "ntp.aliyun.com"', main_source)
        self.assertIn("wifi_manager_credentials_t", store_header)

    def test_dns_response_buffer_includes_appended_answer(self):
        source = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.c"
        ).read_text(encoding="utf-8")

        self.assertIn("DNS_A_ANSWER_TEMPLATE", source)
        self.assertRegex(
            source,
            r"uint8_t\s+response\s*\[\s*DNS_MAX_QUERY_LEN\s*\+\s*"
            r"sizeof\(DNS_A_ANSWER_TEMPLATE\)\s*\]",
        )

    def test_softap_dhcp_dns_uses_offer_flag_and_dns_address(self):
        source = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.c"
        ).read_text(encoding="utf-8")

        self.assertIn("uint8_t dns_offer_enabled = 1u", source)
        self.assertRegex(
            source,
            r"esp_netif_dhcps_option\([^;]+ESP_NETIF_DOMAIN_NAME_SERVER,"
            r"\s*&dns_offer_enabled,\s*sizeof\(dns_offer_enabled\)\)",
        )
        self.assertRegex(
            source,
            r"esp_netif_set_dns_info\(s_ap_netif,\s*ESP_NETIF_DNS_MAIN,"
            r"\s*&dns_info\)",
        )

    def test_debug_endpoint_exposes_heap_capacity_and_usage(self):
        source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )

        for field in [
            '"internal_total_heap"',
            '"internal_used_heap"',
            '"psram_total_heap"',
            '"psram_used_heap"',
        ]:
            self.assertIn(field, source)
        self.assertIn("heap_caps_get_total_size", source)

    def test_system_info_formats_used_total_remaining_and_percentage(self):
        source = (PROJECT_ROOT / "data" / "index.html").read_text(
            encoding="utf-8"
        )

        for element_id in [
            "internal-heap-summary",
            "internal-heap-bar",
            "internal-heap-pct",
            "internal-min-heap",
            "internal-largest-block",
            "psram-summary",
            "psram-pct",
        ]:
            self.assertRegex(source, rf'id="{element_id}"')
        self.assertIn("已用", source)
        self.assertIn("剩余", source)
        self.assertIn("renderUsage", source)
        self.assertIn("formatPercent", source)
        self.assertIn('class="task-label"', source)
        self.assertIn('<pre id="task-list">', source)
        self.assertNotIn('class="task-details"', source)
        self.assertIn("任务列表（栈剩余）", source)

    def test_default_file_manager_does_not_advertise_unconfigured_sd(self):
        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )
        files_source = (PROJECT_ROOT / "data" / "files.html").read_text(
            encoding="utf-8"
        )

        self.assertIn(".sd_mount_point = NULL", web_source)
        self.assertNotIn("OPTIONAL_SD_MOUNT_POINT", web_source)
        self.assertIn("updateOptionalStorageTabs", files_source)
        self.assertIn("tab.hidden=data.mounted!==true", files_source)

    def test_frontend_pages_share_common_css(self):
        common = (PROJECT_ROOT / "data" / "common.css").read_text(
            encoding="utf-8"
        )

        self.assertIn(":root{", common)
        self.assertIn(".panel{", common)
        for page in ["index.html", "files.html"]:
            html = (PROJECT_ROOT / "data" / page).read_text(encoding="utf-8")
            self.assertIn('href="/common.css"', html)
            # Shared design tokens must not be re-declared inline.
            self.assertNotIn("--shadow-hover:0 18px", html)

    def test_default_build_does_not_register_hello_example(self):
        main_source = (PROJECT_ROOT / "main" / "main.c").read_text(
            encoding="utf-8"
        )
        cmake_source = (PROJECT_ROOT / "main" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertNotIn('#include "hello_web.h"', main_source)
        self.assertNotIn("hello_web_register", main_source)
        self.assertNotIn('"hello_web.c"', cmake_source)

    def test_defaults_do_not_enable_unused_runtime_features(self):
        defaults = (PROJECT_ROOT / "sdkconfig.defaults").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y", defaults)
        self.assertNotIn("CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y", defaults)

    def test_wifi_manager_preserves_valid_boundary_lengths(self):
        source = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.c"
        ).read_text(encoding="utf-8")

        self.assertIn("memcpy(cfg.sta.ssid", source)
        self.assertIn("memcpy(cfg.sta.password", source)
        self.assertNotIn("copy_str((char *)cfg.sta", source)

    def test_wifi_manager_exposes_time_synced_callback_hook(self):
        header = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.h"
        ).read_text(encoding="utf-8")
        source = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.c"
        ).read_text(encoding="utf-8")

        self.assertIn("wifi_manager_time_synced_cb_t", header)
        self.assertIn("wifi_manager_set_time_synced_callback", header)
        self.assertIn("wifi_manager_set_time_synced_callback", source)
        # The SNTP handler invokes the registered callback.
        self.assertIn("s_time_synced_cb(", source)

    def test_wifi_manager_defers_sta_apply_for_response(self):
        source = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.c"
        ).read_text(encoding="utf-8")

        # Newly applied STA credentials are reconnected via a short one-shot
        # timer so a caller (e.g. the HTTP handler) can flush its response
        # before the STA drops.
        self.assertIn("WIFI_STA_APPLY_DELAY_MS", source)
        self.assertIn("esp_timer_start_once(s_apply_timer", source)
        self.assertIn("apply_timer_cb", source)
        self.assertIn("esp_timer_create(&apply_timer_args, &s_apply_timer)", source)

    def test_wifi_store_replaces_config_atomically(self):
        source = (PROJECT_ROOT / "main" / "wifi_config_store.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("WIFI_CONFIG_TEMP_PATH", source)
        self.assertIn("fsync(", source)
        self.assertIn("rename(", source)

    def test_wifi_store_rejects_incomplete_json_allocation(self):
        source = (PROJECT_ROOT / "main" / "wifi_config_store.c").read_text(
            encoding="utf-8"
        )

        self.assertRegex(
            source,
            r'cJSON_AddStringToObject\(root,\s*"ssid"[^;]+!=\s*NULL',
        )
        self.assertRegex(
            source,
            r'cJSON_AddStringToObject\(root,\s*"password"[^;]+!=\s*NULL',
        )

    def test_wifi_store_can_reload_maximally_escaped_valid_credentials(self):
        source = (PROJECT_ROOT / "main" / "wifi_config_store.c").read_text(
            encoding="utf-8"
        )
        match = re.search(
            r"#define\s+WIFI_CONFIG_JSON_BUFFER_BYTES\s+(\d+)u", source
        )
        self.assertIsNotNone(match)

        encoded = json.dumps(
            {
                "ssid": "\x01" * 32,
                "password": "\x01" * 64,
            },
            separators=(",", ":"),
        )
        self.assertGreaterEqual(int(match.group(1)), len(encoded.encode()) + 1)

    def test_wifi_manager_serializes_credentials_and_propagates_init_errors(self):
        source = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.c"
        ).read_text(encoding="utf-8")
        header = (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.h"
        ).read_text(encoding="utf-8")
        main_source = (PROJECT_ROOT / "main" / "main.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("s_credentials_mutex", source)
        self.assertIn("xSemaphoreCreateMutex", source)
        self.assertNotIn("ESP_ERROR_CHECK", source)
        self.assertIn("wifi_manager_is_started", header)
        self.assertIn("wifi_manager_is_started()", main_source)

    def test_application_storage_coordinates_filesystem_ota_and_http_access(self):
        storage_header = (PROJECT_ROOT / "main" / "app_storage.h").read_text(
            encoding="utf-8"
        )
        store_source = (PROJECT_ROOT / "main" / "wifi_config_store.c").read_text(
            encoding="utf-8"
        )
        ota_header = (
            PROJECT_ROOT / "components" / "ota_manager" / "ota_manager.h"
        ).read_text(encoding="utf-8")
        ota_source = (
            PROJECT_ROOT / "components" / "ota_manager" / "ota_manager.c"
        ).read_text(encoding="utf-8")
        ota_cmake = (
            PROJECT_ROOT / "components" / "ota_manager" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        file_header = (
            PROJECT_ROOT / "components" / "file_manager" / "file_manager.h"
        ).read_text(encoding="utf-8")
        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )

        for token in [
            "app_storage_acquire",
            "app_storage_try_acquire",
            "app_storage_release",
            "app_storage_begin_update",
            "app_storage_end_update",
        ]:
            self.assertIn(token, storage_header)
        self.assertIn("app_storage_acquire()", store_source)
        self.assertIn("app_storage_release()", store_source)

        self.assertIn("ota_manager_config_t", ota_header)
        self.assertIn("filesystem_update_begin", ota_header)
        self.assertNotIn("esp_littlefs", ota_source)
        self.assertNotRegex(ota_source, r'partition_label\s*=\s*"storage"')
        self.assertNotRegex(ota_source, r'esp_partition_find_first\([^;]+"storage"')
        self.assertNotIn("littlefs", ota_cmake.lower())

        self.assertIn("file_manager_set_access_callbacks", file_header)
        self.assertIn("file_manager_set_access_callbacks(", web_source)
        self.assertIn("app_storage_try_acquire()", web_source)

    def test_ota_keeps_simple_firmware_only_init_api(self):
        ota_header = (
            PROJECT_ROOT / "components" / "ota_manager" / "ota_manager.h"
        ).read_text(encoding="utf-8")
        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )

        self.assertRegex(ota_header, r"ota_manager_init\s*\(\s*void\s*\)")
        self.assertRegex(
            ota_header,
            r"ota_manager_init_with_config\s*\(\s*const ota_manager_config_t\s*\*",
        )
        self.assertIn("ota_manager_init_with_config(&ota_config)", web_source)

    def test_file_manager_storage_locations_are_application_configured(self):
        file_header = (
            PROJECT_ROOT / "components" / "file_manager" / "file_manager.h"
        ).read_text(encoding="utf-8")
        file_source = (
            PROJECT_ROOT / "components" / "file_manager" / "file_manager.c"
        ).read_text(encoding="utf-8")
        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("file_manager_storage_config_t", file_header)
        self.assertIn("file_manager_set_storage_config", file_header)
        for concrete_value in ['"/littlefs"', '"/sdcard"', '"storage"']:
            self.assertNotIn(concrete_value, file_source)
        self.assertIn("file_manager_set_storage_config(&file_manager_config)", web_source)
        self.assertLess(
            web_source.index("file_manager_set_storage_config(&file_manager_config)"),
            web_source.index("start_webserver()"),
        )

    def test_corrupt_filesystem_still_allows_ota_recovery(self):
        storage_source = (PROJECT_ROOT / "main" / "app_storage.c").read_text(
            encoding="utf-8"
        )
        main_source = (PROJECT_ROOT / "main" / "main.c").read_text(
            encoding="utf-8"
        )
        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )

        begin_update = storage_source[
            storage_source.index("esp_err_t app_storage_begin_update") :
            storage_source.index("esp_err_t app_storage_end_update")
        ]
        self.assertNotIn("!s_mounted || s_update_active", begin_update)
        self.assertIn("if (s_mounted)", begin_update)
        self.assertNotIn("ESP_ERROR_CHECK(app_storage_init())", main_source)
        self.assertIn("FILESYSTEM_RECOVERY_HTML", web_source)
        self.assertIn("/ota/upload/filesystem", web_source)

    def test_web_reports_file_existence_instead_of_false_loaded_state(self):
        source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )

        self.assertNotIn('"config_loaded"', source)
        self.assertNotIn('"loaded_from_file"', source)
        self.assertIn('"config_exists"', source)

    def test_wifi_config_is_blocked_from_public_file_access(self):
        store_header = (
            PROJECT_ROOT / "main" / "wifi_config_store.h"
        ).read_text(encoding="utf-8")
        file_header = (
            PROJECT_ROOT / "components" / "file_manager" / "file_manager.h"
        ).read_text(encoding="utf-8")
        file_source = (
            PROJECT_ROOT / "components" / "file_manager" / "file_manager.c"
        ).read_text(encoding="utf-8")
        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("wifi_config_store_is_path", store_header)
        self.assertIn("file_manager_set_read_guard", file_header)
        self.assertIn("file_manager_set_read_guard(protect_wifi_config)", web_source)
        self.assertIn(
            "file_manager_set_mutation_guard(protect_wifi_config)", web_source
        )
        self.assertIn("wifi_config_store_is_path(path)", web_source)

        mkdir_action = file_source[
            file_source.index("static esp_err_t file_manager_mkdir_action") :
            file_source.index("static cJSON *receive_upload_metadata")
        ]
        self.assertIn("mutation_denied(fs_type, resolved)", mkdir_action)

    def test_default_platform_does_not_start_sd_logger(self):
        platform_entrypoints = [
            PROJECT_ROOT / "main" / "main.c",
            PROJECT_ROOT / "main" / "web_platform.c",
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.c",
        ]

        self.assertEqual([], matches_in(platform_entrypoints, re.compile(r"sd_logger")))

    def test_default_platform_does_not_start_sd_card(self):
        main_source = (PROJECT_ROOT / "main" / "main.c").read_text(
            encoding="utf-8"
        )
        main_cmake = (PROJECT_ROOT / "main" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertNotRegex(main_source, r'(?m)^#include\s+"sd_card\.h"$')
        self.assertNotIn("sd_card_init()", main_source)
        self.assertNotRegex(main_cmake, r"\bsd_card\b")
        self.assertTrue((PROJECT_ROOT / "components" / "sd_card" / "sd_card.c").exists())

    def test_wifi_provisioning_example_is_json_not_compile_time_macros(self):
        header_example = PROJECT_ROOT / "main" / "wifi_config.example.h"
        json_example = PROJECT_ROOT / "main" / "wifi_config.example.json"

        self.assertFalse(header_example.exists())
        self.assertTrue(json_example.exists())
        example = json.loads(json_example.read_text(encoding="utf-8"))
        self.assertEqual({"ssid", "password"}, set(example))

        docs = (PROJECT_ROOT / "README.md").read_text(encoding="utf-8")
        ignore = (PROJECT_ROOT / ".gitignore").read_text(encoding="utf-8")
        self.assertNotIn("wifi_config.example.h", docs)
        self.assertNotIn("main/wifi_config.h", ignore)

    def test_template_identity_has_single_source(self):
        index = (PROJECT_ROOT / "data" / "index.html").read_text(
            encoding="utf-8"
        )
        web_source = (PROJECT_ROOT / "main" / "web_platform.c").read_text(
            encoding="utf-8"
        )
        app_config = (PROJECT_ROOT / "main" / "app_config.h").read_text(
            encoding="utf-8"
        )

        # Frontend reads the AP password from /network.json instead of hardcoding it.
        self.assertNotIn("const AP_PASS", index)
        self.assertIn("d.ap_password", index)
        # Platform exposes the AP password through the wifi_manager getter.
        self.assertIn("wifi_manager_get_ap_password()", web_source)
        self.assertIn("wifi_manager_get_ap_password", (
            PROJECT_ROOT / "components" / "wifi_manager" / "wifi_manager.h"
        ).read_text(encoding="utf-8"))
        # Build id comes from the single app_config source.
        self.assertIn("#define APP_BUILD_ID", app_config)
        self.assertNotIn('"esp32s3-template-v1"', web_source)


if __name__ == "__main__":
    unittest.main()
