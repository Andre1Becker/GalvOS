/**
 * main.cpp -- galvOS - A Mikoy Laser Replacement Firmware (WebUI-Focus)
 *
 * Core 0: WiFi, WebUI, Art-Net, Ether Dream, Safety, DMX
 * Core 1: Galvo-Timer-ISR + Pattern-Engine
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_log.h>
#include <esp_task_wdt.h>   // WDT-control
#include "config.h"
#include "pinmap.h"
#include "safety/safety.h"
#include "output/galvo_out.h"
#include "control/dmx_in.h"
#include "patterns/pattern_engine.h"
#include "storage/sd_card.h"
#include "control/encoder.h"
#include "mutex.h"
#include <esp_system.h>
#include "net/ota_update.h"
#include "net/etherdream.h"
#include "storage/playlist.h"
#include "ilda/ilda_player.h"
#include "net/artnet_in.h"
#include "net/ntp_client.h"
#include "net/web_ui.h"
#include "net/etherdream.h"
#include "sensors/temp_monitor.h"
#include "util/log_buffer.h"
#include "util/cpu_monitor.h"

// Global data -- must be defined exactly once here
RuntimeConfig    gConfig;
RuntimeState     gState;
SafetyConfig     gSafety;
PlaylistConfig   gPlaylist;
WebOverride      gOverride;
LivePresetControls gLivePreset;
TextConfig       gTextConfig;
CurveConfig      gCurves;
ProjectionConfig gProjection;
OptimizerLiveConfig gOptimizerConfig;   // GalvOS v5 Point Optimizer (Pillar 1)
ZoneConfig       gZone;                 // touch-defined projection zone
volatile bool    gDebugNoHW = false;

static const char* TAG = "main";
static Preferences s_prefs;

static void loadConfig() {
    s_prefs.begin("laser", true);
    gConfig.dmx_address     = s_prefs.getUShort("dmx_addr",  DEFAULT_DMX_ADDRESS);
    gConfig.artnet_universe = s_prefs.getUShort("artnet_uni",DEFAULT_DMX_UNIVERSE);
    gConfig.galvo_x_offset  = s_prefs.getShort("xoff", 0);
    gConfig.galvo_y_offset  = s_prefs.getShort("yoff", 0);
    gConfig.galvo_x_gain    = s_prefs.getShort("xgain", 32767);
    gConfig.galvo_y_gain    = s_prefs.getShort("ygain", 32767);
    gConfig.dac_limit_min   = s_prefs.getUShort("dac_lim_lo", 0x0666);
    gConfig.dac_limit_max   = s_prefs.getUShort("dac_lim_hi", 0xF999);
    gConfig.swap_xy         = s_prefs.getBool("swap", false);
    gConfig.invert_x        = s_prefs.getBool("invx", false);
    gConfig.invert_y        = s_prefs.getBool("invy", false);
    gConfig.gain_r          = s_prefs.getUChar("gain_r", 115);  // FIX: R=1W
    gConfig.gain_g          = s_prefs.getUChar("gain_g",  43);  // FIX: G=1W
    gConfig.gain_b          = s_prefs.getUChar("gain_b", 255);  // FIX: B=3W
    gConfig.gamma_enable    = s_prefs.getBool ("gamma_en", true);
    gConfig.gamma_val       = s_prefs.getFloat("gamma_val", 2.2f);
    gOptimizerConfig.corner_angle_deg   = s_prefs.getFloat("opt_cad",   25.0f);
    gOptimizerConfig.min_corner_pts     = s_prefs.getUChar("opt_mincp", 1);
    gOptimizerConfig.max_corner_pts     = s_prefs.getUChar("opt_maxcp", 6);
    gOptimizerConfig.pts_per_1000_units = s_prefs.getFloat("opt_ppu",   4.0f);
    gOptimizerConfig.min_segment_pts    = s_prefs.getUChar("opt_minsp", 2);
    gOptimizerConfig.blank_samples      = s_prefs.getUChar("opt_blank", 40);
    gOptimizerConfig.max_pts_per_frame  = s_prefs.getUShort("opt_maxppf", 310);
    gOptimizerConfig.min_blank_samples  = s_prefs.getUChar("opt_minbl", 8);
    gOptimizerConfig.blank_pts_per_1000_units = s_prefs.getFloat("opt_blppu", 10.0f);
    gOptimizerConfig.min_interior_pts_per_segment = s_prefs.getUChar("opt_minip", 6);
    gOptimizerConfig.stage1_blank_target = s_prefs.getUChar("opt_s1tgt", 20);
    gSafety.temp_warn_c     = s_prefs.getUChar("t_warn",  45);
    gSafety.temp_reduce_c   = s_prefs.getUChar("t_red",   55);
    gSafety.temp_shutdown_c = s_prefs.getUChar("t_shut",  70);
    gSafety.fan_min_pct     = s_prefs.getUChar("fan_min", 15);
    gSafety.fan_auto        = s_prefs.getBool ("fan_auto",true);
    s_prefs.getString("ssid", gConfig.wifi_ssid, sizeof(gConfig.wifi_ssid));
    s_prefs.getString("pass", gConfig.wifi_pass, sizeof(gConfig.wifi_pass));
    s_prefs.getString("host", gConfig.hostname,  sizeof(gConfig.hostname));
    gConfig.wifi_static = s_prefs.getBool("static_ip", false);
    s_prefs.getString("ip",   gConfig.wifi_ip,   sizeof(gConfig.wifi_ip));
    s_prefs.getString("gw",   gConfig.wifi_gw,   sizeof(gConfig.wifi_gw));
    s_prefs.getString("mask", gConfig.wifi_mask,  sizeof(gConfig.wifi_mask));
    s_prefs.getString("dns",  gConfig.wifi_dns,   sizeof(gConfig.wifi_dns));
    s_prefs.getString("auth_hash", gConfig.auth_hash, sizeof(gConfig.auth_hash));
    
    // Generate unique hostname from last 3 MAC bytes if none stored in NVS
    if (strlen(gConfig.hostname) == 0) {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(gConfig.hostname, sizeof(gConfig.hostname),
                 "galvos-%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    s_prefs.end();
    ESP_LOGI(TAG, "Config loaded. DMX=%u Hostname=%s Auth=%s",
             gConfig.dmx_address, gConfig.hostname,
             strlen(gConfig.auth_hash) ? "custom" : "default ('laser')");
}

// ══════════════════════════════════════════════════════════════
// Hardware panic blanking -- laser off on ANY crash
//
// Called on: Guru Meditation, Double Exception,
// Stack Smash, Hardfault, Brownout, WDT
//
// IRAM_ATTR: runs from RAM -- works even if flash defective/busy
// No FreeRTOS context -- ONLY direct GPIO register access
// ══════════════════════════════════════════════════════════════
#include <esp_private/panic_internal.h>

static void IRAM_ATTR laser_panic_blanking(const char* reason) {
    // Direct GPIO-Register-Manipulation (no Arduino, no FreeRTOS)
    // Works even with a complete stack overflow
    // Inverted logic: HIGH = laser OFF (active-HIGH driver MN-1W5AT)
    gpio_set_level((gpio_num_t)PIN_LASER_R,  1);  // HIGH = laser OFF
    gpio_set_level((gpio_num_t)PIN_LASER_G,  1);
    gpio_set_level((gpio_num_t)PIN_LASER_B,  1);
    gpio_set_level((gpio_num_t)PIN_LASER_EN, 0);  // SSR off
    // Call ESP-IDF default panic handler (prints backtrace + reset)
    // esp_panic_handler(reason);  
    esp_restart();  // safety reboot
}

// Brownout-Callback (registriert in setup)
static void IRAM_ATTR brownout_cb() {
    gpio_set_level((gpio_num_t)PIN_LASER_R,  1);  // HIGH = laser OFF
    gpio_set_level((gpio_num_t)PIN_LASER_G,  1);
    gpio_set_level((gpio_num_t)PIN_LASER_B,  1);
    gpio_set_level((gpio_num_t)PIN_LASER_EN, 0);
}

// ── Task-start helper with error checking ─────────────
static bool startTask(TaskFunction_t fn, const char* name,
                      uint32_t stack, UBaseType_t prio, BaseType_t core = 0) {
    TaskHandle_t h = nullptr;
    BaseType_t r = xTaskCreatePinnedToCore(
        fn, name, stack, nullptr, prio, &h, core);
    if (r != pdPASS || !h) {
        ESP_LOGE("main", "TASK FAILED: %s (Heap=%u)", name, ESP.getFreeHeap());
        LOG_E(logbuf::CAT_SYSTEM, "Task failed: %s", name);
        return false;
    }
    ESP_LOGI("main", "Task OK: %-10s p=%u c=%d s=%u", name, prio, core, stack);
    return true;
}

// ── WiFi watchdog — reconnects and starts services after late connection ──────
static bool s_wifi_services_started = false;
static bool s_services_created = false;  // init()/tasks run once per boot only

static void wifiWatchdogTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));   // check every 15 s
        if (strlen(gConfig.wifi_ssid) == 0) continue;

        if (WiFi.status() == WL_CONNECTED) {
            if (!s_wifi_services_started) {
                // Connected (possibly via auto-reconnect after AP-mode boot)
                s_wifi_services_started = true;
                // init()/tasks must run only once per boot. A fast down/up
                // flap inside the 15s window clears s_wifi_services_started
                // (else branch below), so guarding on it alone re-ran init()
                // and spawned duplicate artnet/edream/ntp tasks. s_services_created
                // is never reset, so reconnects skip re-creation.
                if (!s_services_created) {
                    s_services_created = true;
                    artnet_in::init();
                    ntp_client::init();
                    etherdream::init();
                    xTaskCreatePinnedToCore(artnet_in::task,  "artnet", 4096, nullptr, 3, nullptr, 0);
                    xTaskCreatePinnedToCore(etherdream::task, "edream", 8192, nullptr, 3, nullptr, 0);
                    xTaskCreatePinnedToCore(ntp_client::task, "ntp",    4096, nullptr, 2, nullptr, 0);
                }
                // Switch off AP if STA is now up (clean up AP mode)
                if (WiFi.getMode() == WIFI_AP_STA) {
                    WiFi.softAPdisconnect(true);
                    WiFi.mode(WIFI_STA);
                    ESP_LOGI("wifi_wd", "AP disabled, STA connected: %s",
                             WiFi.localIP().toString().c_str());
                }
                LOG_I(logbuf::CAT_WIFI, "WiFi: %s RSSI %d dBm",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
            }
        } else {
            if (s_wifi_services_started) {
                // Connection dropped
                s_wifi_services_started = false;
                ESP_LOGW("wifi_wd", "WiFi dropped, will auto-reconnect");
            }
            // setAutoReconnect(true) handles the reconnection; no explicit reconnect() needed
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);

    // ── mutexes first above all else ─────────────────
    mtx::init();
    cpu_mon::init();
    esp_register_shutdown_handler([]() {
        gpio_set_level((gpio_num_t)PIN_LASER_R,  1);  // HIGH = laser OFF
        gpio_set_level((gpio_num_t)PIN_LASER_G,  1);
        gpio_set_level((gpio_num_t)PIN_LASER_B,  1);
        gpio_set_level((gpio_num_t)PIN_LASER_EN, 0);
    });

    ESP_LOGI(TAG, "=== Mikoy Laser FW %s ===", LASER_FW_VERSION);
    ESP_LOGI(TAG, "Chip: %s, Cores: %d, PSRAM: %u",
             ESP.getChipModel(), ESP.getChipCores(), ESP.getPsramSize());

    loadConfig();
    // debug mode from NVS load
    { Preferences p; p.begin("laser",true);
      gDebugNoHW = p.getBool("dbg_nohw", false); p.end();
      if(gDebugNoHW) ESP_LOGW("main","DEBUG NO-HW MODE active"); }
    logbuf::init();
    LOG_I(logbuf::CAT_SYSTEM, "FW %s | Chip %s | PSRAM %uKB | Heap %uKB",
          LASER_FW_VERSION, ESP.getChipModel(),
          ESP.getPsramSize()/1024, ESP.getFreeHeap()/1024);

    // Load projection config from NVS
    {
        Preferences prefs;
        prefs.begin("projection", true);
        if (prefs.isKey("kpps")) {
            gProjection.galvo_kpps          = prefs.getUShort("kpps", 30);
            gProjection.scan_angle_mech_deg = prefs.getFloat("scan_ang", 20.0f);
            gProjection.exit_angle_deg      = prefs.getFloat("exit_ang", 15.0f);
            gProjection.ilda_test_angle_deg = prefs.getFloat("ilda_ang", 8.0f);
            gProjection.power_r_mw          = prefs.getFloat("pwr_r",   1000.0f);
            gProjection.power_g_mw          = prefs.getFloat("pwr_g",   1000.0f);
            gProjection.power_b_mw          = prefs.getFloat("pwr_b",   3000.0f);
            gProjection.distance_m          = prefs.getFloat("dist_m",  3.0f);
        }
        prefs.end();
        ESP_LOGI("main", "Projection: %u kpps, exit=%.1f deg, dist=%.1f m",
                 (unsigned)gProjection.galvo_kpps,
                 gProjection.exit_angle_deg, gProjection.distance_m);
    }

    safety::init();
    galvo::init();
    ilda::init();
    // NOTE: sd_card::init() is called AFTER galvo::start() below.
    // SPIClass(FSPI).begin() must not run while SPI2 DAC self-test is active.
    dmx_in::init();
    patterns::init();
    temp::init();   // DS18B20 + Fan-PWM

    // network
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(gConfig.hostname);
    if (strlen(gConfig.wifi_ssid) > 0) {
        // configure Static IP if enabled
        if (gConfig.wifi_static && strlen(gConfig.wifi_ip) > 0) {
            IPAddress ip, gw, mask, dns;
            if (ip.fromString(gConfig.wifi_ip) &&
                gw.fromString(strlen(gConfig.wifi_gw) > 0 ? gConfig.wifi_gw : "0.0.0.0") &&
                mask.fromString(strlen(gConfig.wifi_mask) > 0 ? gConfig.wifi_mask : "255.255.255.0")) {
                dns.fromString(strlen(gConfig.wifi_dns) > 0 ? gConfig.wifi_dns : "8.8.8.8");
                WiFi.config(ip, gw, mask, dns);
                ESP_LOGI(TAG, "Static IP: %s", gConfig.wifi_ip);
            }
        }
            WiFi.setAutoReconnect(true);
        WiFi.persistent(true);
        WiFi.begin(gConfig.wifi_ssid, gConfig.wifi_pass);
        uint32_t t0 = millis();
        ESP_LOGI(TAG, "WiFi connecting to '%s' ...", gConfig.wifi_ssid);
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000)
            vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (WiFi.status() == WL_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected: %s (RSSI %d dBm)",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
        LOG_I(logbuf::CAT_WIFI, "WiFi: %s RSSI %d dBm",
              WiFi.localIP().toString().c_str(), WiFi.RSSI());
        artnet_in::init();
        ntp_client::init();
        etherdream::init();
        s_wifi_services_started = true;  // prevent watchdog from re-starting tasks
        s_services_created      = true;  // tasks spawned below in setup(); block watchdog re-create
    } else {
        // No WiFi within timeout — start AP for config access
        // Arduino's setAutoReconnect(true) will keep retrying in the background;
        // if the router comes up later, the ESP will connect automatically.
        char ap_ssid[20], ap_pwd[12];
        uint64_t mac = ESP.getEfuseMac();
        snprintf(ap_ssid, sizeof(ap_ssid), "Laser-%04X", (uint16_t)(mac & 0xFFFF));
        snprintf(ap_pwd,  sizeof(ap_pwd),  "%08X",  (uint32_t)(mac >> 16));
        ESP_LOGW(TAG, "WiFi timeout -- SoftAP '%s' PW: '%s'", ap_ssid, ap_pwd);
        LOG_W(logbuf::CAT_WIFI, "WiFi timeout -- SoftAP 192.168.4.1");
        // WIFI_AP_STA: AP for immediate config access + STA continues auto-reconnect
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(ap_ssid, ap_pwd);
        ESP_LOGI(TAG, "SoftAP IP: %s", WiFi.softAPIP().toString().c_str());
    }
    // WebUI last -- needs LittleFS and knows safety::requestArm
    web_ui::init();

    // ── Feature 1: Encor ────────────────────────────────────
    encoder::init();
    startTask(encoder::task, "encoder", 3072, 2, 0);

    // ── Feature 2: OTA ────────────────────────────────────────
    ota_update::init();
    ESP_LOGI("main", "OTA URL: http://%s.local/update", gConfig.hostname);

    // ── Feature 4: Playlist ──────────────────────────────────
    playlist::loadFromSD();
    startTask(playlist::task, "playlist", 4096, 2, 0);

    // Tasks
    startTask(safety::task,   "safety",  4096, 6, 0);
    startTask(dmx_in::task,   "dmx_rx",  4096, 5, 0);
    startTask(temp::task,     "temp",    3072, 1, 0);
    startTask(web_ui::task,   "webui",   10240, 3, 0); // p=3: below safety/dmx
    startTask(wifiWatchdogTask, "wifi_wd", 3072, 2, 0);
    if (WiFi.status() == WL_CONNECTED) {
        startTask(artnet_in::task,  "artnet", 4096, 3, 0);
        startTask(etherdream::task, "edream", 8192, 3, 0);
        startTask(ntp_client::task, "ntp",    4096, 2, 0);
    }

    // Galvo task on core 1 uses busy-wait (50 kHz, 20us loop).
    // IDLE1 (Core 1) gets almost no CPU time → WDT would fire.
    // Solution: remove IDLE1 from WDT monitoring.
    // IDLE0 (core 0) stays monitored -- safety net for all other tasks.
    {
        // xTaskGetIdleTaskHandleForCore (IDF5) or
        // xTaskGetIdleTaskHandleForCPU (IDF4/Arduino Core 2.x)
        #if defined(xTaskGetIdleTaskHandleForCore)
            TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
        #else
            TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCPU(1);
        #endif
        if (idle1) {
            esp_err_t e = esp_task_wdt_delete(idle1);
            ESP_LOGI(TAG, "IDLE1 WDT-Remove: %s", e == ESP_OK ? "OK" : "skipped");
        }
    }

    // ══════════════════════════════════════════════════════════
    // Core layout after FIX 2:
    //
    //  Core 0 (all network/IO tasks, FreeRTOS round-robin):
    //    safety(6) > dmx_rx(5) > webui(4) > artnet(4) > edream(4)
    //    > pattern(4) > encor(3) > playlist(3) > temp(3) > modesw(3)
    //
    //  Core 1 (EXCLUSIVE for real-time):
    //    galvoTask(configMAX_PRIORITIES-1) — Busy-Wait 50kHz
    //    -> no other task on core 1!
    //    → patterns::task liefert Frames via Ring-Buffer (thread-sicher)
    // ══════════════════════════════════════════════════════════
    // FIX 2: patterns::task on core 0 (not core 1!)
    // Core 1 = exclusive for galvoTask (busy-wait, highest priority)
    // -> patterns::task on core 1 would starve (task starvation)
    // → Core 0 shares CPU fairly (FreeRTOS round-robin at equal priority)
    startTask(patterns::task, "pattern", 8192,  2, 0); // p=2: pattern calculation
    galvo::start();   // starts galvoTask on core 1 at highest priority

    // SD card init runs in a separate one-shot task on Core 0.
    // This prevents SPIClass(FSPI).begin() from conflicting with the
    // galvoTask SPI2 transfers that start immediately after galvo::start().
    xTaskCreatePinnedToCore([](void*) {
        vTaskDelay(pdMS_TO_TICKS(500));  // wait for galvoTask steady-state
        if (sd_card::init()) {
            ESP_LOGI("main", "SD card: %u ILDA files", sd_card::fileCount());
        } else {
            ESP_LOGW("main", "SD card unavailable -- ILDA playback disabled");
        }
        vTaskDelete(nullptr);
    }, "sd_init", 4096, nullptr, 1, nullptr, 0);  // Core 0, low priority

    ESP_LOGI(TAG, "Ready. Heap=%u PSRAM=%u", ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop() {
    ota_update::handle();  // Feature 2: OTA handle in loop

    cpu_mon::update();     // update CPU load measurement (internal 500ms rate-limit)
    safety::heartbeat();

    static uint32_t last_print = 0;
    if (millis() - last_print > 10000) {
        // ── System status ──────────────────────────────────────────────
        ESP_LOGI(TAG, "Heap=%u PSRAM=%u pps=%lu src=%d arm=%d",
                 ESP.getFreeHeap(), ESP.getFreePsram(),
                 (unsigned long)gState.points_per_sec,
                 (int)gState.source, (int)gState.laser_armed);

        // ── CPU load ───────────────────────────────────────────────────
        ESP_LOGI(TAG, "CPU  Core0=%u%%  Core1=%u%%",
                 cpu_mon::load0(), cpu_mon::load1());

        // ── Fan + temperatures ─────────────────────────────────────────
        ESP_LOGI(TAG, "Fan  fan1=%u%%  fan2=%u%%",
                 (unsigned)(temp::gTempState.fan1_duty * 100 / 255),
                 (unsigned)(temp::gTempState.fan2_duty * 100 / 255));
        for (uint8_t i = 0; i < temp::NUM_SENSORS; i++) {
            if (temp::gTempState.sensor_ok[i]) {
                ESP_LOGI(TAG, "Temp [%u] %-20s %.1f C%s%s",
                         i, temp::sensor_names[i],
                         temp::gTempState.temp_c[i],
                         temp::gTempState.warn_active[i]  ? " WARN"  : "",
                         temp::gTempState.alert_active[i] ? " ALERT" : "");
            }
        }

        // ── Task stack high-water marks ────────────────────────────────
        // Shows minimum free stack ever seen per task (bytes).
        // If close to 0 -> stack overflow risk!
        static const char* const k_tasks[] = {
            "galvo_out", "pattern", "web_ui", "temp", "dmx_rx", nullptr
        };
        for (int i = 0; k_tasks[i]; i++) {
            TaskHandle_t h = xTaskGetHandle(k_tasks[i]);
            if (h) {
                ESP_LOGI(TAG, "Stack [%-10s] HWM=%u bytes free",
                         k_tasks[i],
                         (unsigned)(uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t)));
            }
        }

        last_print = millis();
    }
    delay(100);
}
