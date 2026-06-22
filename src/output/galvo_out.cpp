#include "galvo_out.h"
#include "mutex.h"
#include "safety/safety.h"
#include <Arduino.h>
#include <SPI.h>
#include <esp_log.h>
#include "../util/log_buffer.h"
#include <esp_timer.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>

namespace galvo {

static const char* TAG = "galvo";

/* ============================================================
 * Ring buffer in PSRAM
 * ============================================================ */
static constexpr size_t RING_FRAMES = 8;
static LaserPoint*      s_ring[RING_FRAMES];
static volatile size_t  s_ring_sizes[RING_FRAMES];
static volatile size_t  s_ring_head = 0;
static volatile size_t  s_ring_tail = 0;
static volatile size_t  s_point_idx = 0;

// Laser-off latency compensation: on the first blank tick after a lit tick,
// the DAC is held at the last lit position for LASER_OFF_HOLD_TICKS output
// periods before moving. LEDC at 50kHz has a turn-off latency of up to 2x
// the PWM period (40us); at 30kpps (33us/tick) that spans just over 1 tick,
// so 2 hold ticks provide safe margin. Without this, rgbOff() and the first
// DAC move happen in the same tick, causing a short lit hook at every corner.
static constexpr uint8_t LASER_OFF_HOLD_TICKS = 2;
static uint8_t          s_laser_off_hold = 0;
static uint16_t         s_last_dac_x     = 0x8000;
static uint16_t         s_last_dac_y     = 0x8000;

static spi_device_handle_t s_galvo_spi   = nullptr;
static volatile bool       s_running     = false;
static TaskHandle_t        s_task_handle = nullptr;
static bool                s_dac_ok      = false;   // set true after successful init+selftest

// DAC debug-log snapshot (written from IRAM galvoTask, read/logged from a
// normal-priority task at ~2Hz — see logDacDebugIfPending())
static volatile uint16_t   s_dac_dbg_x       = 0x8000;
static volatile uint16_t   s_dac_dbg_y       = 0x8000;
static volatile bool       s_dac_dbg_pending = false;

static volatile uint32_t s_points_total = 0;
static volatile uint32_t s_points_last  = 0;
static volatile uint32_t s_last_pps_t   = 0;
static volatile uint32_t s_pps_cached   = 0;

/* ============================================================
 * DAC8562 Write (polled SPI, 24-Bit Frame)
 *
 * DAC8562 24-Bit Format (MSB first):
 *   Byte 0 (DB23..DB16) = [X][X][C2][C1][C0][A2][A1][A0]
 *     CMD (C2,C1,C0) = 011 = "Write input register n and update DAC-n"
 *     ADDR (A2,A1,A0) = 000 (DAC-A) or 001 (DAC-B)
 *     => byte = 00 011 0(addr) = 0x18 (DAC-A) / 0x19 (DAC-B)
 *   Bytes 1-2 = data (16-bit, full resolution, MSB first)
 *
 * Synchronous mode: LDAC tied permanently to GND/AGND, so the update
 * happens immediately on the 24th SCLK falling edge -- no extra LDAC
 * pulse needed.
 * SPI mode 1 (CPOL=0, CPHA=1): data stable on falling edge,
 * latched on rising edge.
 * ============================================================ */
static inline void IRAM_ATTR writeDAC8562(uint8_t channel, uint16_t value) {
    uint8_t tx[3];
    // Byte 0 (DB23..DB16): [X][X][C2][C1][C0][A2][A1][A0]
    //   CMD = 011 (C2,C1,C0) -> "Write to input register n and update DAC
    //         register n" (TI DAC8562 datasheet, Table 12/Table 17)
    //   ADDR = 000 for DAC-A, 001 for DAC-B (Table 13)
    // => byte = 00 011 0(channel) = 0x18 for DAC-A, 0x19 for DAC-B
    //
    // PREVIOUS BUGS:
    //   0x06 = 00 000 110 -> CMD=000 ("write input register only, no
    //          update"), ADDR=110 (reserved). Output never updated.
    //   0x0C = 00 001 100 -> CMD=001 ("software LDAC, update DAC register
    //          n" -- but does NOT load new data). This momentarily
    //          triggers an update but with stale/zero data, which is why
    //          VOUTA/VOUTB dropped to 0V (DAC8562 zero-scale) and stayed
    //          there on every subsequent "write".
    tx[0] = 0x18 | (channel & 0x01);
    tx[1] = (value >> 8) & 0xFF;
    tx[2] =  value       & 0xFF;
    spi_transaction_t t = {};
    t.length    = 24;
    t.tx_buffer = tx;
    if(s_galvo_spi) spi_device_polling_transmit(s_galvo_spi, &t);

    // Cheap snapshot for optional debug logging (no logging here — this is
    // IRAM_ATTR and runs at 30kpps; ESP_LOGI/LOG_I are not safe to call when
    // flash cache may be disabled, e.g. during OTA/NVS writes).
    // A separate task polls s_dac_dbg_* and logs at ~2Hz when enabled.
    if (gConfig.dac_debug_log) {
        if (channel == 0) s_dac_dbg_x = value; else s_dac_dbg_y = value;
        s_dac_dbg_pending = true;
    }
}

/* ============================================================
 * DAC8562 Initialization
 * Must be called once after power-on.
 * ============================================================ */
static void dac8562Init() {
    // FIX v1.4: release /CLR (GPIO13 HIGH) — DAC exits reset
    // GPIO13 was set LOW in galvo::init() (safe boot).
    // Release only now, after SPI init and before the first transfer.
    gpio_set_direction((gpio_num_t)13, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)13, 1);   // /CLR = HIGH → DAC active
    vTaskDelay(pdMS_TO_TICKS(1));

    // DAC8562 unipolar 0..5V, internal ref active (VREFOUT pin10 = 2.5V):
    // 1. CMD=111: internal reference ON -> VREFOUT pin10 outputs 2.5V
    // 2. CMD=110: Gain×2 → VOUT = Code/65536 × 5.0V (unipolar)
    //    Code=0x0000 → 0V | Code=0x8000 → 2.5V | Code=0xFFFF → ~5V
    // OPA4134 difference amplifier (Hardware): VOUT = 2×(2.5V − VDAC)
    //    → Code=0x8000 → VDAC=2.5V → Galvo=0V (center) ✓
    //    → Code=0x0000 → VDAC=0V   → Galvo=+5V
    //    → Code=0xFFFF → VDAC≈5V   → Galvo=-5V
    //    Polarity correctable via gConfig.invert_x / invert_y

    spi_transaction_t t = {};
    t.length = 24;

    // Step 1: internal reference ON
    uint8_t ref_on[3] = { 0x38, 0x00, 0x01 };  // CMD=111 = 0x38>>1, both channels
    t.tx_buffer = ref_on;
    if(s_galvo_spi) spi_device_polling_transmit(s_galvo_spi, &t);
    vTaskDelay(pdMS_TO_TICKS(2));

    // Step 2 (removed): a separate "gain x2" write was here, but per the
    // TI datasheet (Table 9), enabling the internal reference in Step 1
    // (CMD=111, DB0=1) already resets the gain register to x2 for BOTH
    // channels automatically. The removed command used byte 0x0C, which
    // decodes as CMD=001 ("software LDAC update, no new data") -- this
    // triggered a spurious update with stale/zero data immediately after
    // init, collapsing VOUTA/VOUTB to zero-scale (0V) where they stayed
    // on every subsequent write (same root cause as the writeDAC8562 fix
    // below: CMD=001 vs CMD=011).

    // Step 3: both channels to center = 0V (code 0x8000)
    writeDAC8562(0, 0x8000);
    writeDAC8562(1, 0x8000);
    ESP_LOGI(TAG, "DAC8562 init: 0..5V, VREFOUT=2.5V, Gain×2, X=Y=center(2.5V)");
    ESP_LOGI(TAG, "  OPA4134 Diff-Amp: galvo center=0V @ Code=0x8000");
}

/* ============================================================
 * TTL-RGB-Laser-Steuerung via LEDC-PWM
 * RGB laser TTL-modulated via 6N137 optocouplers; OPA4134 quad op-amp
 * buffers VREF and drives the galvo difference amplifiers (X/Y channels)
 *
 * LEDC channel 2/3/4 @ 50 kHz, 8-Bit:
 *   duty=0   -> LOW  -> laser OFF
 *   duty=255 -> HIGH -> laser fully ON
 *   duty=128 → 50% duty cycle → ~50% brightness (via PWM persistence)
 *
 * Hardware: GPIO → 100Ω → laser TTL input
 * ============================================================ */
static volatile uint8_t s_rgb_r = 0;
static volatile uint8_t s_rgb_g = 0;
static volatile uint8_t s_rgb_b = 0;
// Removed for debugging static volatile bool    s_ledc_active = false;

// Hardware debug: direct output bypassing pattern engine
static volatile bool    s_hw_debug_active = false;
static volatile int16_t s_dbg_x = 0;      // DAC-value: -32767..+32767
static volatile int16_t s_dbg_y = 0;
static volatile uint8_t s_dbg_r = 0;      // PWM 0..255
static volatile uint8_t s_dbg_g = 0;
static volatile uint8_t s_dbg_b = 0;

// Raw DAC command request/result, executed by galvoTask (Core 1) to avoid
// concurrent SPI2 access from the HTTP handler task (Core 0).
static volatile bool    s_raw_cmd_pending = false;
static volatile uint8_t s_raw_cmd_cmd3    = 0;
static volatile uint8_t s_raw_cmd_addr3   = 0;
static volatile uint16_t s_raw_cmd_data   = 0;
static volatile int      s_raw_cmd_result = -1;  // -1=pending, 0=fail, 1=ok

/* ============================================================
 * Gamma-Lookup-Tabelle γ=2.2 (sRGB-default)
 * Corrects the linear PWM output to a perceptually linear
 * brightness: a color value of 128 feels visually like
 * ~50% brightness (not the physical 22%).
 *
 * white balance calculation (laser specification):
 *   R: 1000mW × sens(638nm,0.265) = 265 mW_vis → gain=115
 *   G: 1000mW × sens(520nm,0.710) = 710 mW_vis → gain=43
 *   B: 3000mW × sens(445nm,0.040) = 120 mW_vis → gain=255
 * All three channels deliver ~120 mW_vis at full drive.
 * ============================================================ */
static const uint8_t GAMMA_LUT[256] PROGMEM = {
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,
      1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
      3,  3,  3,  3,  3,  4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,
      6,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10, 10, 11, 11, 11, 12,
     12, 13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19,
     20, 20, 21, 22, 22, 23, 23, 24, 25, 25, 26, 26, 27, 28, 28, 29,
     30, 30, 31, 32, 33, 33, 34, 35, 35, 36, 37, 38, 39, 39, 40, 41,
     42, 43, 43, 44, 45, 46, 47, 48, 49, 49, 50, 51, 52, 53, 54, 55,
     56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
     73, 74, 75, 76, 77, 78, 79, 81, 82, 83, 84, 85, 87, 88, 89, 90,
     91, 93, 94, 95, 97, 98, 99,100,102,103,105,106,107,109,110,111,
    113,114,116,117,119,120,121,123,124,126,127,129,130,132,133,135,
    137,138,140,141,143,145,146,148,149,151,153,154,156,158,159,161,
    163,165,166,168,170,172,173,175,177,179,181,182,184,186,188,190,
    192,194,196,197,199,201,203,205,207,209,211,213,215,217,219,221,
    223,225,227,229,231,234,236,238,240,242,244,246,248,251,253,255
};

static inline uint8_t applyGamma(uint8_t v) {
    return gConfig.gamma_enable ? GAMMA_LUT[v] : v;  // applyGamma: gConfig OK (only init-Zeit)
}

static inline void rgbOff() {
    // Pure register write — LEDC stays permanently attached after setup().
    // Duty 255 = pin HIGH = 6N137 conducts = laser OFF (inverted logic).
    // No attach/detach here: at 15-30kpps that caused visible flicker
    // on patterns with frequent blank/unblank transitions (wireframes,
    // multi-segment presets).
    ledcWrite(LEDC_CH_R, 255);
    ledcWrite(LEDC_CH_G, 255);
    ledcWrite(LEDC_CH_B, 255);
}

static inline void rgbWrite(uint8_t r, uint8_t g, uint8_t b) {
    // Inverted logic: HIGH = laser OFF, LOW = laser ON
    // Driver MN-1W5AT is active-HIGH: TTL HIGH (1.65V) enables laser
    // 6N137 inverts: GPIO HIGH -> conducts -> Pin6 LOW -> laser OFF
    //                GPIO LOW  -> off       -> Pin6 HIGH (1.65V) -> laser ON
    ledcWrite(LEDC_CH_R, 255 - applyGamma(r));
    ledcWrite(LEDC_CH_G, 255 - applyGamma(g));
    ledcWrite(LEDC_CH_B, 255 - applyGamma(b));
}

/* ============================================================
 * Galvo streaming task — Core 1, max. priority
 * Busy-wait with esp_timer_get_time() for precise 20µs timing.
 * ============================================================ */
/* ============================================================
 * GalvoSnapshot — thread-safe config snapshot for Core 1
 *
 * FIX: race condition on gConfig (core 0 writes / core 1 reads)
 *
 * Strategy: snapshot pattern
 *   - Once per frame, gConfig is briefly (< 1µs) read under mutex
 *     and written into a local, core-1-exclusive copy.
 *   - galvoTask accesses only s_snap -- no direct
 *     gConfig access in the 50-kHz path.
 *   - Writes from core 0 (Web UI, DMX, Art-Net) happen safely:
 *     galvoTask sees the new value only at the next snapshot.
 * ============================================================ */
struct GalvoSnapshot {
    uint8_t  gain_r      = 115;
    uint8_t  gain_g      =  43;
    uint8_t  gain_b      = 255;
    bool     gamma_en    = true;
    uint16_t dac_limit_min = 0x0666;
    uint16_t dac_limit_max = 0xF999;
    // Galvo geometry (from applyCalibration -- already applied before frame)
};
static GalvoSnapshot s_snap;

// Called by galvoTask once per frame (not per point!)
// Under mutex: < 200 ns, well within the 20µs frame budget
static inline void updateSnapshot() {
    if (xSemaphoreTake(mtx::config, 0) == pdTRUE) {
        s_snap.gain_r   = gConfig.gain_r;
        s_snap.gain_g   = gConfig.gain_g;
        s_snap.gain_b   = gConfig.gain_b;
        s_snap.gamma_en = gConfig.gamma_enable;
        s_snap.dac_limit_min = gConfig.dac_limit_min;
        s_snap.dac_limit_max = gConfig.dac_limit_max;
        xSemaphoreGive(mtx::config);
    }
    // If mutex not immediately available: keep old snapshot (safe)
}

// Forward declaration: defined after galvoTask, called from within it.
static bool sendRawCommandImpl(uint8_t cmd3, uint8_t addr3, uint16_t data);

static void IRAM_ATTR galvoTask(void*) {
    // Dynamic rate from ProjectionConfig (12..60 kpps), default 30 kpps.
    // Reread every frame — the task loop is fast enough that one extra
    // read per 20-83µs is harmless.
    auto getPeriodUs = []() -> uint32_t {
        uint16_t kpps = gProjection.galvo_kpps;
        if (kpps < 12) kpps = 12;
        if (kpps > 60) kpps = 60;
        return 1000000UL / ((uint32_t)kpps * 1000UL);
    };
    uint32_t period_us = getPeriodUs();
    uint64_t next_tick = esp_timer_get_time();

    uint32_t hb_count = 0;
    uint32_t sub_hb_count = 0;
    while (s_running) {
        // Notify safety subsystem that galvoTask is alive (SYS_GALVO = 1).
        // Rate-limited to once per 100 iterations (~3ms @ 30kpps) to avoid
        // overhead in the tight 20µs timing loop.
        if (++sub_hb_count >= 100) { sub_hb_count = 0; safety::subsystemHeartbeat(1); }

        // Hardware heartbeat: toggle every 100ms (retriggerable monoflop possible)
        // Refresh period every frame (cheap, handles runtime changes)
        period_us = getPeriodUs();
        if (++hb_count >= (1000000UL / (period_us * 10UL))) {
            hb_count = 0;
            #ifdef PIN_HEARTBEAT
            gpio_set_level((gpio_num_t)PIN_HEARTBEAT,
                           !gpio_get_level((gpio_num_t)PIN_HEARTBEAT));
            #endif
        }
        // debug mode: no SPI, Ring-Buffer leeren so that Pattern-Task not blockiert
        if (gDebugNoHW) {
            size_t tail = s_ring_tail;
            if (s_point_idx >= s_ring_sizes[tail]) {
                size_t nx = (tail+1) % RING_FRAMES;
                if (nx != s_ring_head) { s_ring_tail=nx; s_point_idx=0; }
            } else { s_point_idx++; }
            s_points_total++;
            next_tick += period_us;
            while(esp_timer_get_time()<(int64_t)next_tick){}
            continue;
        }
        s_points_total++;

        updateSnapshot();  // FIX: gConfig snapshot once per iteration

        // Execute any pending raw DAC debug command (Config tab -> DAC
        // Low-Level Commands). Runs here so only galvoTask touches SPI2.
        if (s_raw_cmd_pending) {
            bool ok = sendRawCommandImpl(s_raw_cmd_cmd3, s_raw_cmd_addr3, s_raw_cmd_data);
            s_raw_cmd_result  = ok ? 1 : 0;
            s_raw_cmd_pending = false;
        }

        if (!gState.laser_armed) {
            // Output is forced to center/off for safety, but the ring
            // buffer must still be drained -- otherwise it fills up while
            // disarmed (pattern_engine keeps calling pushFrame()), and
            // pattern_engine's pushFrame() retry-loop (up to 500ms of
            // 2ms-spaced retries per cycle) burns Core 0 CPU continuously,
            // which is the main cause of the high idle cpu0 load and the
            // resulting WebUI/WiFi latency.
            writeDAC8562(0, 0x8000);
            writeDAC8562(1, 0x8000);
            rgbOff(); 
            size_t tail = s_ring_tail;
            if (s_point_idx >= s_ring_sizes[tail]) {
                size_t next_tail = (tail + 1) % RING_FRAMES;
                if (next_tail != s_ring_head) {
                    s_ring_tail = next_tail;
                    s_point_idx = 0;
                }
                // else: ring full but nothing to do -- pattern_engine will
                // see pushFrame() fail until we advance again next tick.
            } else {
                s_point_idx = s_ring_sizes[tail];  // skip rest of this frame
            }
        } else if (s_hw_debug_active) {
            // Hardware debug: fixed position/color, takes absolute priority
            // over ring-buffer consumption. Must be checked BEFORE the
            // underrun logic below -- otherwise an empty ring (the normal
            // case during pure HW-debug testing) causes underrun=true on
            // every iteration, which writes 0x8000 (2.5V) at 30kHz and
            // permanently overwrites the debug X/Y values before they can
            // settle on the DAC output.
            if (!gState.laser_armed.load()) {
                s_hw_debug_active = false;  // Auto-Deactiveierung if disarmed
            } else {
                writeDAC8562(0, (uint16_t)(s_dbg_x + 32768));
                writeDAC8562(1, (uint16_t)(s_dbg_y + 32768));
                rgbWrite(s_dbg_r, s_dbg_g, s_dbg_b);
            }
            next_tick += period_us;
            while (esp_timer_get_time() < (int64_t)next_tick) {}
            continue;
        } else {
            size_t tail = s_ring_tail;
            bool underrun = false;
            // Debug: count underruns and log ring state every 5s
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (s_point_idx >= s_ring_sizes[tail]) {
                size_t next_tail = (tail + 1) % RING_FRAMES;
                if (next_tail != s_ring_head) {
                    s_ring_tail = next_tail;
                    tail        = next_tail;
                    s_point_idx = 0;
                } else {
                     // Underrun: no new frame ready. Replay current frame from
                    // beginning instead of blanking — eliminates flicker caused
                    // by the 37ms gap between pattern_engine pushes at 25fps.
                    s_point_idx = 0;
                }
            }
            {
                if (!s_ring[tail]) { s_point_idx=0; continue; }  // Null-Guard

                const LaserPoint& p = s_ring[tail][s_point_idx++];

                // int16_t [-32767..32767] → uint16_t [0..65535] for DAC8562
                // DAC8562 with internal VREF: 0x0000=0V, 0x8000=VRef/2, 0xFFFF=VRef
                // Galvo typically expects ±5V around GND → 0x8000 = midpoint = 0V
                int32_t x = constrain((int32_t)p.x + 0x8000, 0, 0xFFFF);
                int32_t y = constrain((int32_t)p.y + 0x8000, 0, 0xFFFF);
                // DAC output limiting (Config -> Output Limiting): clamp to
                // configured min/max codes to avoid clipping the galvo input
                // (hardware gain is 2.2x, full DAC range would exceed +/-5V).
                x = constrain(x, (int32_t)s_snap.dac_limit_min, (int32_t)s_snap.dac_limit_max);
                y = constrain(y, (int32_t)s_snap.dac_limit_min, (int32_t)s_snap.dac_limit_max);
            if (p.blank) {
                    rgbOff();
                    if (s_laser_off_hold > 0) {
                        // Still within hold window: keep DAC parked at the
                        // last lit position while LEDC/6N137 finish turning off.
                        writeDAC8562(0, s_last_dac_x);
                        writeDAC8562(1, s_last_dac_y);
                        s_laser_off_hold--;
                    } else {
                        writeDAC8562(0, (uint16_t)x);
                        writeDAC8562(1, (uint16_t)y);
                        s_last_dac_x = (uint16_t)x;
                        s_last_dac_y = (uint16_t)y;
                    }
                } else {
                    writeDAC8562(0, (uint16_t)x);
                    writeDAC8562(1, (uint16_t)y);
                    // PWM duty: 8-bit. Apply dimmer + gain.
                    uint8_t dim = gState.master_dimmer.load();  // atomic load
                    // FIX: s_snap instead of gConfig — Race-Condition-frei
                    uint8_t r = (uint8_t)(((uint32_t)p.r * dim * s_snap.gain_r) / (255UL * 255));
                    uint8_t g = (uint8_t)(((uint32_t)p.g * dim * s_snap.gain_g) / (255UL * 255));
                    uint8_t b = (uint8_t)(((uint32_t)p.b * dim * s_snap.gain_b) / (255UL * 255));
                    // DEBUG: force red-only output to eliminate color mixing
                    // and isolate blanking/geometry issues. Remove when done.
                    if (r == 0 && (g > 0 || b > 0)) r = (g > b) ? g : b;
                    g = 0; b = 0;
                    rgbWrite(r, g, b);
                    s_last_dac_x = (uint16_t)x;
                    s_last_dac_y = (uint16_t)y;
                    s_laser_off_hold = LASER_OFF_HOLD_TICKS;
                }
            }
        }

        next_tick += period_us;
        while (esp_timer_get_time() < (int64_t)next_tick) {
            // Busy-wait core 1 -- IDLE1 removed from WDT
        }
    }
    writeDAC8562(0, 0x8000);
    writeDAC8562(1, 0x8000);
    rgbOff();
    vTaskDelete(nullptr);
}

/* ============================================================
 * Oeffentliche API
 * ============================================================ */
void init() {
    // Ring-Buffer IMMER allozieren (also NoHW — Pattern-Task bralsot ihn)
    for (size_t i = 0; i < RING_FRAMES; i++) {
        if (!s_ring[i]) {
            const size_t slot_bytes = PATTERN_POINTS_MAX * sizeof(LaserPoint);
            // Try: ps_malloc (PSRAM) -> malloc (DRAM) as fallback
            s_ring[i] = (LaserPoint*)ps_malloc(slot_bytes);
            if (!s_ring[i]) {
                s_ring[i] = (LaserPoint*)malloc(slot_bytes);
                if (s_ring[i]) ESP_LOGW(TAG, "Ring slot %u: DRAM Fallback (no PSRAM)", i);
                else           ESP_LOGE(TAG, "Ring slot %u: ALLOC FAILED - no RAM", i);
            }
            s_ring_sizes[i] = 0;
        }
    }
    if (gDebugNoHW) {
        ESP_LOGW(TAG, "galvo::init() NoHW: ring buffer OK, SPI skipped");
        for(int p:{PIN_LASER_R,PIN_LASER_G,PIN_LASER_B}){
            gpio_set_direction((gpio_num_t)p,GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)p,0);
        }
        return;
    }
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num    = PIN_GALVO_MOSI;
    buscfg.miso_io_num    = PIN_SD_MISO;    // GPIO2: needed for SD card MISO
    buscfg.sclk_io_num    = PIN_GALVO_SCK;
    buscfg.quadwp_io_num  = -1;
    buscfg.quadhd_io_num  = -1;
    buscfg.max_transfer_sz = 4096;          // increased for SD block transfers
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO); // DMA for SD

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 20 * 1000 * 1000;
    devcfg.mode           = 1;         // SPI Mode 1: CPOL=0, CPHA=1
    devcfg.spics_io_num   = PIN_GALVO_CS;
    devcfg.queue_size     = 4;
    spi_bus_add_device(SPI2_HOST, &devcfg, &s_galvo_spi);

    // ── Safety: pull-downs on laser TTL pins (hardware interlock) ──
    // If the ESP32 crashes and GPIOs float -> laser stays OFF.
    // IN ADDITION to the 10kOhm pull-down resistors on the board!
    gpio_set_direction((gpio_num_t)PIN_LASER_R, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_LASER_G, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_LASER_B, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_LASER_R, 1);  // HIGH = laser OFF (inverted logic)
    gpio_set_level((gpio_num_t)PIN_LASER_G, 1);
    gpio_set_level((gpio_num_t)PIN_LASER_B, 1);

        // ── TTL-RGB-PWM via LEDC ──────────────────────────────────────
    // Attach once here, permanently. rgbOff()/rgbWrite() only ever
    // call ledcWrite() afterwards — no per-point attach/detach, which
    // at 15-30kpps caused visible dotted-line flicker on patterns with
    // frequent blank/unblank transitions (wireframes, multi-segment
    // presets). The GPIO HIGH set above covers the brief window until
    // attach; rgbOff() immediately drives duty=255 (=laser OFF) right
    // after attach, closing the laser-on-boot window.
    ledcSetup(LEDC_CH_R, LEDC_FREQ_RGB, LEDC_RES_RGB);
    ledcSetup(LEDC_CH_G, LEDC_FREQ_RGB, LEDC_RES_RGB);
    ledcSetup(LEDC_CH_B, LEDC_FREQ_RGB, LEDC_RES_RGB);
    ledcAttachPin(PIN_LASER_R, LEDC_CH_R);
    ledcAttachPin(PIN_LASER_G, LEDC_CH_G);
    ledcAttachPin(PIN_LASER_B, LEDC_CH_B);
    rgbOff();  // explicitly OFF -- no laser on boot
    ESP_LOGI(TAG, "TTL-RGB PWM: GPIO R=%d G=%d B=%d @ %dHz 8-Bit",
             PIN_LASER_R, PIN_LASER_G, PIN_LASER_B, LEDC_FREQ_RGB);

    gpio_set_direction((gpio_num_t)PIN_DAC_CLR_N, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_DAC_CLR_N, 0);  // CLR active
    vTaskDelay(pdMS_TO_TICKS(10));

    // Ring buffer already allocated in init() above — here only reset sizes
    for (size_t i = 0; i < RING_FRAMES; i++) s_ring_sizes[i] = 0;

    dac8562Init();
    ESP_LOGI(TAG, "Galvo init OK. DAC8562 16-Bit, SPI=20MHz, Rate=%d Hz", GALVO_RATE_HZ);

    // ── DAC self-test: write three codes (min / mid / max) ────────────────
    // DAC8562 is write-only — no readback possible.
    // We verify that spi_device_polling_transmit() returns ESP_OK for all
    // three transfers. If SPI bus or CS wiring is broken, it returns an error.
    {
        bool ok = true;
        const uint16_t codes[3] = { 0x0000, 0x8000, 0xFFFF };
        const char*    labels[3] = { "0x0000(min)", "0x8000(mid)", "0xFFFF(max)" };
        spi_transaction_t t = {};
        t.length = 24;
        uint8_t tx[3];
        for (int ci = 0; ci < 3; ci++) {
            tx[0] = 0x18;                        // CMD=011 (Write+Update), ADDR=0 (DAC-A)
            tx[1] = (codes[ci] >> 8) & 0xFF;
            tx[2] =  codes[ci]       & 0xFF;
            t.tx_buffer = tx;
            esp_err_t err = spi_device_polling_transmit(s_galvo_spi, &t);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "[DAC] Self-test FAIL @ code %s: %s", labels[ci], esp_err_to_name(err));
                LOG_E(logbuf::CAT_SYSTEM, "[DAC] Self-test FAIL @ %s: %s", labels[ci], esp_err_to_name(err));
                ok = false;
                break;
            }
        }
        // Return galvo to center after test
        writeDAC8562(0, 0x8000);
        writeDAC8562(1, 0x8000);

        s_dac_ok = ok;
        if (ok) {
            ESP_LOGI(TAG, "[DAC] DAC8562 self-test OK (SPI transfers verified, galvo at center)");
            LOG_I(logbuf::CAT_SYSTEM, "[DAC] DAC8562 OK — SPI verified, galvo centered");
        } else {
            ESP_LOGE(TAG, "[DAC] DAC8562 self-test FAILED — check SPI wiring GPIO10/11/12");
            LOG_E(logbuf::CAT_SYSTEM, "[DAC] DAC8562 FAIL — check SPI CS/MOSI/SCK wiring");
        }
    }
}

void start() {
    s_running = true;
    // rgbUpdateTask removed -- LEDC-PWM is ISR-safe, no task needed
    xTaskCreatePinnedToCore(galvoTask, "galvo", 4096, nullptr,
                            configMAX_PRIORITIES - 1, &s_task_handle, 1);
    ESP_LOGI(TAG, "Galvo streaming started @ %u kpps (period=%u µs)",
             (unsigned)gProjection.galvo_kpps,
             (unsigned)(1000000UL/((uint32_t)gProjection.galvo_kpps*1000UL)));
}

void stop() {
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(100));
}

bool pushFrame(const LaserPoint* pts, size_t count) {
    if (count == 0 || count > PATTERN_POINTS_MAX) return false;
    if (!s_ring[s_ring_head]) return false;   // Null-Guard: PSRAM-Alloc failed

    // While hardware debug mode is active (Galvo & Laser test tab),
    // galvoTask's consumer does not drain the ring buffer (it outputs the
    // fixed debug X/Y/RGB values instead). If we accepted frames here, the
    // ring would fill up and subsequent pushFrame() calls would fail for
    // ~500ms -> pattern_engine emergency stop. Pretend success instead;
    // the pattern data is simply discarded until the user exits debug mode
    // (Hardware Test tab -> "Exit Debug Mode").
    if (s_hw_debug_active) return true;

    size_t next_head = (s_ring_head + 1) % RING_FRAMES;
    if (next_head == s_ring_tail) {
        static uint32_t s_overflow_count = 0;
        s_overflow_count++;
        if (s_overflow_count % 100 == 1)  // not spammen
            ESP_LOGW("galvo", "Ring buffer overflow #%u", s_overflow_count);
        return false;
    }
    memcpy(s_ring[s_ring_head], pts, count * sizeof(LaserPoint));
    s_ring_sizes[s_ring_head] = count;
    // RELEASE fence: ensure memcpy + size write are visible on Core 1
    // before s_ring_head advances and galvoTask sees the new slot.
    __atomic_thread_fence(__ATOMIC_RELEASE);
     s_ring_head = next_head;
    s_ring_head = next_head;
    return true;
}

uint32_t pointsPerSec() {
    uint32_t now = millis();
    if (now - s_last_pps_t >= 1000) {
        s_pps_cached  = (s_points_total - s_points_last) * 1000 / (now - s_last_pps_t);
        s_points_last = s_points_total;
        s_last_pps_t  = now;
        gState.points_per_sec = s_pps_cached;
    }
    return s_pps_cached;
}

uint32_t bufferFillLevel() {
    int32_t fill = (int32_t)s_ring_head - (int32_t)s_ring_tail;
    if (fill < 0) fill += RING_FRAMES;
    return (uint32_t)(fill * 100 / RING_FRAMES);
}



// ── applyCalibration: Offset, Gain, Flip from gConfig ─────────────────
// Called by etherdream.cpp for externally received frames.
void applyCalibration(LaserPoint* pts, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int32_t x = pts[i].x, y = pts[i].y;
        // Gain (scaling, symmetry adjustment)
        x = (int32_t)x * gConfig.galvo_x_gain / 32767;
        y = (int32_t)y * gConfig.galvo_y_gain / 32767;
        // Offset
        x += gConfig.galvo_x_offset;
        y += gConfig.galvo_y_offset;
        // axis swap
        if (gConfig.swap_xy) { int32_t tmp = x; x = y; y = tmp; }
        // inversion
        if (gConfig.invert_x) x = -x;
        if (gConfig.invert_y) y = -y;
        pts[i].x = (int16_t)constrain(x, -32767, 32767);
        pts[i].y = (int16_t)constrain(y, -32767, 32767);
    }
}

// ── Hardware debug API ────────────────────────────────────────────────────
void setDebugOutput(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    s_dbg_x = x; s_dbg_y = y;
    s_dbg_r = r; s_dbg_g = g; s_dbg_b = b;
    s_hw_debug_active = true;
    ESP_LOGW(TAG, "HW-Debug: X=%d Y=%d R=%u G=%u B=%u", x, y, r, g, b);
    LOG_W(logbuf::CAT_GALVO, "HW-Debug: X=%d Y=%d R=%u G=%u B=%u", x, y, r, g, b);
}

void clearDebugOutput() {
    s_hw_debug_active = false;
    s_dbg_x = 0; s_dbg_y = 0;
    s_dbg_r = 0; s_dbg_g = 0; s_dbg_b = 0;
    // NOTE: previously called writeDAC8562(0/1, 0x8000) directly here, but
    // this races with galvoTask's SPI2 transactions (Core 1) since this
    // function runs on the HTTP handler task (Core 0) -- causes
    // "Cannot send polling transaction while the previous polling
    // transaction is not terminated" and corrupted/interleaved log output.
    // Not needed: as soon as s_hw_debug_active=false, galvoTask's next
    // iteration (<33us later) writes center (if disarmed/ring-empty) or
    // resumes normal pattern output -- both via the single SPI2 owner.
    if (!gDebugNoHW) { rgbOff(); }
    ESP_LOGI(TAG, "HW debug: galvo=center, laser=OFF");
}

bool isDebugOutputActive() { return s_hw_debug_active; }

bool dacOk()    { return s_dac_ok; }
bool noHwMode() { return gDebugNoHW; }
spi_host_device_t getSpi2Host() { return SPI2_HOST; }

// Internal worker -- only called from galvoTask (Core 1), which owns SPI2.
static bool sendRawCommandImpl(uint8_t cmd3, uint8_t addr3, uint16_t data) {
    if (!s_galvo_spi) return false;
    uint8_t tx[3];
    // Byte0 = [X][X][C2 C1 C0][A2 A1 A0]
    tx[0] = ((cmd3 & 0x07) << 3) | (addr3 & 0x07);
    tx[1] = (data >> 8) & 0xFF;
    tx[2] =  data       & 0xFF;
    spi_transaction_t t = {};
    t.length    = 24;
    t.tx_buffer = tx;
    esp_err_t err = spi_device_polling_transmit(s_galvo_spi, &t);
    ESP_LOGI(TAG, "[DAC] raw cmd: bytes=%02X %02X %02X (cmd=%u addr=%u data=0x%04X) -> %s",
             tx[0], tx[1], tx[2], cmd3, addr3, data, esp_err_to_name(err));
    LOG_I(logbuf::CAT_GALVO, "[DAC] raw cmd: %02X %02X %02X -> %s",
          tx[0], tx[1], tx[2], esp_err_to_name(err));
    return err == ESP_OK;
}

// Public API -- queues the command for galvoTask and blocks (max ~200ms)
// until it has been executed. Safe to call from any task.
bool sendRawCommand(uint8_t cmd3, uint8_t addr3, uint16_t data) {
    s_raw_cmd_cmd3   = cmd3;
    s_raw_cmd_addr3  = addr3;
    s_raw_cmd_data   = data;
    s_raw_cmd_result = -1;
    s_raw_cmd_pending = true;
    for (int i = 0; i < 200; i++) {  // up to 200ms (galvoTask runs at 30kHz)
        if (s_raw_cmd_result != -1) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (s_raw_cmd_result == -1) {
        s_raw_cmd_pending = false;
        ESP_LOGW(TAG, "[DAC] raw cmd timed out (galvoTask not running?)");
        return false;
    }
    return s_raw_cmd_result == 1;
}

void holdChannelValue(uint8_t channel, uint16_t code, uint32_t durationMs) {
    if (durationMs < 1) durationMs = 1;
    if (durationMs > 60000) durationMs = 60000;
    ESP_LOGI(TAG, "[DAC] hold ch=%c code=0x%04X for %u ms",
             channel ? 'B' : 'A', code, durationMs);
    LOG_I(logbuf::CAT_GALVO, "[DAC] hold ch=%c code=0x%04X for %u ms",
          channel ? 'B' : 'A', code, durationMs);

    // Route through the existing hardware-debug mechanism so only
    // galvoTask (Core 1) ever touches the SPI2 bus -- avoids racing
    // with concurrent DAC writes from this HTTP handler task (Core 0).
    int16_t dx = (channel == 0) ? (int16_t)((int32_t)code - 32768) : 0;
    int16_t dy = (channel == 1) ? (int16_t)((int32_t)code - 32768) : 0;
    setDebugOutput(dx, dy, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(durationMs));
    clearDebugOutput();

    ESP_LOGI(TAG, "[DAC] hold done, returned to center");
    LOG_I(logbuf::CAT_GALVO, "[DAC] hold done, returned to center");
}

void logDacDebugIfPending() {
    if (!gConfig.dac_debug_log) { s_dac_dbg_pending = false; return; }
    if (!s_dac_dbg_pending) return;
    static uint32_t s_last_log_ms = 0;
    uint32_t now = millis();
    if (now - s_last_log_ms < 500) return;
    s_last_log_ms = now;
    s_dac_dbg_pending = false;
    uint16_t x = s_dac_dbg_x, y = s_dac_dbg_y;
    ESP_LOGI("galvo", "[DAC] X=0x%04X Y=0x%04X", x, y);
    LOG_I(logbuf::CAT_GALVO, "[DAC] X=0x%04X Y=0x%04X", x, y);
}

}  // namespace galvo