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

// Direct SPI2 hardware register access for low-overhead DAC writes.
// Avoids including hal/spi_ll.h (conflicts with Arduino C++ operator overloads).
// ESP32-S3 TRM ch.26 register offsets (SPI2 base = 0x60024000).
#define GALVO_SPI2_BASE        0x60024000UL
#define GALVO_SPI2_CMD         (*(volatile uint32_t*)(GALVO_SPI2_BASE + 0x000))  // SPI_CMD_REG
#define GALVO_SPI2_USER        (*(volatile uint32_t*)(GALVO_SPI2_BASE + 0x010))  // SPI_USER_REG
#define GALVO_SPI2_USER1       (*(volatile uint32_t*)(GALVO_SPI2_BASE + 0x014))  // SPI_USER1_REG
#define GALVO_SPI2_USER2       (*(volatile uint32_t*)(GALVO_SPI2_BASE + 0x018))  // SPI_USER2_REG
#define GALVO_SPI2_MS_DLEN     (*(volatile uint32_t*)(GALVO_SPI2_BASE + 0x01C))  // SPI_MS_DLEN_REG
#define GALVO_SPI2_DMA_INT_RAW (*(volatile uint32_t*)(GALVO_SPI2_BASE + 0x03C))  // SPI_DMA_INT_RAW_REG
#define GALVO_SPI2_DMA_CONF    (*(volatile uint32_t*)(GALVO_SPI2_BASE + 0x030))  // SPI_DMA_CONF_REG
#define GALVO_SPI2_DMA_TX_ENA  (1u << 28)  // SPI_DMA_TX_ENA: must be 0 for W0 CPU-mode transfers
#define GALVO_SPI2_W0          (*(volatile uint32_t*)(GALVO_SPI2_BASE + 0x098))  // SPI_W0_REG

#define GALVO_SPI2_UPDATE     (1u << 23)  // SPI_CMD_REG:     SPI_UPDATE (latch config into core)
#define GALVO_SPI2_USR        (1u << 24)  // SPI_CMD_REG:     SPI_USR   (start transfer)
#define GALVO_SPI2_USR_MOSI   (1u << 27)  // SPI_USER_REG:    enable MOSI output phase
#define GALVO_SPI2_TRANS_DONE (1u << 12)  // SPI_DMA_INT_RAW: SPI_TRANS_DONE_INT_RAW

// GPIO single-cycle set/clear for manual CS (GPIO10 = PIN_GALVO_CS)
#define GALVO_GPIO_W1TS  (*(volatile uint32_t*)0x60004008UL)  // GPIO_OUT_W1TS_REG
#define GALVO_GPIO_W1TC  (*(volatile uint32_t*)0x6000400CUL)  // GPIO_OUT_W1TC_REG
#define GALVO_CS_MASK    (1u << PIN_GALVO_CS)                  // GPIO10 = bit 10

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
static volatile uint32_t s_overflow_count = 0;   // cumulative, see pushFrame() / overflowCount()

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

static volatile uint32_t s_frames_total = 0;
static volatile uint32_t s_frames_last  = 0;
static volatile uint32_t s_last_fps_t   = 0;
static volatile uint32_t s_fps_cached   = 0;

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
    // CS is manual (spics_io_num=-1): toggle GPIO10 around the transfer.
    if (s_galvo_spi) {
        GALVO_GPIO_W1TC = GALVO_CS_MASK;
        spi_device_polling_transmit(s_galvo_spi, &t);
        GALVO_GPIO_W1TS = GALVO_CS_MASK;
    }

    // Cheap snapshot for optional debug logging (no logging here — this is
    // IRAM_ATTR and runs at 30kpps; ESP_LOGI/LOG_I are not safe to call when
    // flash cache may be disabled, e.g. during OTA/NVS writes).
    // A separate task polls s_dac_dbg_* and logs at ~2Hz when enabled.
    if (gConfig.dac_debug_log) {
        if (channel == 0) s_dac_dbg_x = value; else s_dac_dbg_y = value;
        s_dac_dbg_pending = true;
    }
}

// Write both DAC channels (X=ch0, Y=ch1) directly via SPI2 hardware registers.
//
// spi_device_polling_transmit() carries ~5-8us of IDF overhead per call
// (CS hooks, FIFO setup, bus mutex). At 45kpps that exceeds the 22.2us
// per-point budget. This bypasses the IDF entirely:
//   - CS (GPIO10) toggled via GPIO W1TS/W1TC (single-cycle, no IDF)
//   - Data loaded directly into SPI2 W0 register
//   - Transfer triggered via SPI_UPDATE + SPI_USR sequence (ESP32-S3 TRM)
//   - Completion polled via DMA_INT_RAW.TRANS_DONE
//
// ESP32-S3 transfer sequence (mirrors IDF spi_ll_master_user_start):
//   1. Set USER reg: usr_mosi=1, preserve clock-phase bits (read-modify-write)
//   2. Write MS_DLEN (bitlen-1) and W0
//   3. cmd.UPDATE=1, poll until cleared   (latches config into SPI core)
//   4. CS LOW, cmd.USR=1                  (start transfer)
//   5. Poll DMA_INT_RAW.TRANS_DONE, CS HIGH
//
// DAC8562 word: [cmd(8)][data_hi(8)][data_lo(8)] MSB-first.
// SPI2 W0 sends bit23 first -> pack cmd in W0[23:16], no byte-swap needed.
//
static bool s_spi_user_configured = false;
static inline void IRAM_ATTR writeDAC8562XY(uint16_t x, uint16_t y) {
    // Configure USER register once: set usr_mosi, preserve IDF clock-phase bits.
    // Blind overwrite of USER would destroy CPOL/CPHA bits set by IDF init.
    if (!s_spi_user_configured) {
        uint32_t u = GALVO_SPI2_USER;
        u |=  GALVO_SPI2_USR_MOSI;  // enable MOSI phase
        u &= ~(1u << 28);           // usr_miso  = 0
        u &= ~(1u << 29);           // usr_dummy = 0
        u &= ~(1u << 30);           // usr_addr  = 0
        u &= ~(1u << 31);           // usr_command = 0
        GALVO_SPI2_USER  = u;
        GALVO_SPI2_USER1 = 0;
        GALVO_SPI2_USER2 = 0;
        // Disable DMA-TX: SPI bus initialized with SPI_DMA_CH_AUTO for SD card,
        // but W0 CPU-mode transfers require DMA_TX_ENA=0. SD access is blocked
        // while galvoTask runs, so safe to hold this cleared permanently.
        GALVO_SPI2_DMA_CONF &= ~GALVO_SPI2_DMA_TX_ENA;
        s_spi_user_configured = true;
    }

    // W0 is LSB-first at the hardware level: byte order must be inverted
    // relative to transmission order (confirmed via scope).
    uint32_t wordA = ((uint32_t)(x & 0xFF) << 16) | ((uint32_t)((x >> 8) & 0xFF) << 8) | 0x18;
    uint32_t wordB = ((uint32_t)(y & 0xFF) << 16) | ((uint32_t)((y >> 8) & 0xFF) << 8) | 0x19;


    GALVO_SPI2_MS_DLEN = 23;  // 24 bits - 1

    // --- DAC-A (X) ---
    GALVO_SPI2_W0  = wordA;
    GALVO_SPI2_CMD = GALVO_SPI2_UPDATE;               // latch W0+DLEN into core
    while (GALVO_SPI2_CMD & GALVO_SPI2_UPDATE) {}
    GALVO_GPIO_W1TC = GALVO_CS_MASK;                  // CS LOW
    GALVO_SPI2_CMD  = GALVO_SPI2_USR;                 // start transfer
    while (GALVO_SPI2_CMD & GALVO_SPI2_USR) {}        // poll USR bit — cleared when done
    GALVO_GPIO_W1TS = GALVO_CS_MASK;                  // CS HIGH — DAC-A latches

    // --- DAC-B (Y) ---
    GALVO_SPI2_W0  = wordB;
    GALVO_SPI2_CMD = GALVO_SPI2_UPDATE;
    while (GALVO_SPI2_CMD & GALVO_SPI2_UPDATE) {}
    GALVO_GPIO_W1TC = GALVO_CS_MASK;
    GALVO_SPI2_CMD  = GALVO_SPI2_USR;
    while (GALVO_SPI2_CMD & GALVO_SPI2_USR) {}
    GALVO_GPIO_W1TS = GALVO_CS_MASK;                  // CS HIGH — DAC-B latches

    if (gConfig.dac_debug_log) {
        s_dac_dbg_x = x; s_dac_dbg_y = y;
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
    if (s_galvo_spi) {
        GALVO_GPIO_W1TC = GALVO_CS_MASK;
        spi_device_polling_transmit(s_galvo_spi, &t);
        GALVO_GPIO_W1TS = GALVO_CS_MASK;
    }
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
// PROGMEM is a no-op on ESP32 (no AVR Harvard split) -- plain DRAM array.
static uint8_t GAMMA_LUT[256];

// Rebuilds GAMMA_LUT for the given gamma exponent (gConfig.gamma_val, 1.0-3.0).
// Call once at boot (galvo::init) and again whenever gamma_val changes via /api/config.
void rebuildGammaLut(float gamma) {
    for (int v = 0; v < 256; v++) {
        GAMMA_LUT[v] = (uint8_t)(255.0f * powf(v / 255.0f, gamma) + 0.5f);
    }
}

static inline uint8_t applyGamma(uint8_t v) {
    return gConfig.gamma_enable ? GAMMA_LUT[v] : v;
}

/* ============================================================
 * Visibility-threshold mapping ("Basiswert")
 * Below a channel-specific PWM duty the laser diode driver emits no
 * visible light at all (measured empirically, see thresh_r/g/b in
 * RuntimeConfig -- Calib tab). Remaps the logical 0-255 range onto the
 * physically visible [threshold..255] duty range, so 0-100% brightness
 * always spans exactly what is actually visible. logical=0 always stays
 * fully off (no minimum-on floor).
 * ============================================================ */
static inline uint8_t mapVisibleRange(uint8_t logical, uint8_t threshold) {
    if (logical == 0 || threshold >= 255) return logical;
    uint16_t span = 255 - threshold;
    return threshold + (uint8_t)((span * logical + 127) / 255);
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

static inline void rgbWrite(uint8_t r, uint8_t g, uint8_t b,
                             uint8_t thresh_r, uint8_t thresh_g, uint8_t thresh_b) {
    // Inverted logic: HIGH = laser OFF, LOW = laser ON
    // Driver MN-1W5AT is active-HIGH: TTL HIGH (1.65V) enables laser
    // 6N137 inverts: GPIO HIGH -> conducts -> Pin6 LOW -> laser OFF
    //                GPIO LOW  -> off       -> Pin6 HIGH (1.65V) -> laser ON
    ledcWrite(LEDC_CH_R, 255 - mapVisibleRange(applyGamma(r), thresh_r));
    ledcWrite(LEDC_CH_G, 255 - mapVisibleRange(applyGamma(g), thresh_g));
    ledcWrite(LEDC_CH_B, 255 - mapVisibleRange(applyGamma(b), thresh_b));
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
    uint8_t  thresh_r    = 143;
    uint8_t  thresh_g    = 144;
    uint8_t  thresh_b    = 169;
    uint16_t dac_limit_min = 0x0666;
    uint16_t dac_limit_max = 0xF999;
    uint32_t period_us     = 33;   // derived from gProjection.galvo_kpps (12..60)
    // Galvo geometry (from applyCalibration -- already applied before frame)
    ZoneConfig zone;        // projection-zone clip polygon, see updateSnapshot()
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
        s_snap.thresh_r = gConfig.thresh_r;
        s_snap.thresh_g = gConfig.thresh_g;
        s_snap.thresh_b = gConfig.thresh_b;
        s_snap.dac_limit_min = gConfig.dac_limit_min;
        s_snap.dac_limit_max = gConfig.dac_limit_max;
        xSemaphoreGive(mtx::config);
    }
    if (xSemaphoreTake(mtx::zone, 0) == pdTRUE) {
        s_snap.zone = gZone;   // polygon + count + enabled, copied atomically
        xSemaphoreGive(mtx::zone);
    }
    // kpps changes rarely; recompute period once per frame, not per tick.
    uint16_t kpps = gProjection.galvo_kpps;
    if (kpps < 12) kpps = 12;
    if (kpps > 60) kpps = 60;
    s_snap.period_us = 1000000UL / ((uint32_t)kpps * 1000UL);
    // If mutex not immediately available: keep old snapshot (safe)
}

// Forward declaration: defined after galvoTask, called from within it.
static bool sendRawCommandImpl(uint8_t cmd3, uint8_t addr3, uint16_t data);

static void IRAM_ATTR galvoTask(void*) {
    // Dynamic rate from ProjectionConfig (12..60 kpps). period_us is now
    // computed in updateSnapshot() once per frame (not per tick) to keep
    // the 50kHz loop free of a division + 2 branches on every point.
    uint32_t period_us = s_snap.period_us;
    uint64_t next_tick = esp_timer_get_time();

    uint32_t hb_count = 0;
    uint32_t sub_hb_count = 0;
    while (s_running) {
        // Notify safety subsystem that galvoTask is alive (SYS_GALVO = 1).
        // Rate-limited to once per 100 iterations (~3ms @ 30kpps) to avoid
        // overhead in the tight 20µs timing loop.
        if (++sub_hb_count >= 100) { sub_hb_count = 0; safety::subsystemHeartbeat(1); }

        // Hardware heartbeat: toggle every 100ms (retriggerable monoflop possible)
        // period_us refreshed from snapshot (updated once per frame below).
        if (++hb_count >= (1000000UL / (period_us * 10UL))) {
            hb_count = 0;
            #ifdef PIN_HEARTBEAT
            gpio_set_level((gpio_num_t)PIN_HEARTBEAT,
                           !gpio_get_level((gpio_num_t)PIN_HEARTBEAT));
            #endif
        }
        // debug mode: no SPI, Ring-Buffer leeren so that Pattern-Task not blockiert
        if (gDebugNoHW) {
            { uint16_t k = gProjection.galvo_kpps;
              if (k < 12) k = 12; if (k > 60) k = 60;
              period_us = 1000000UL / ((uint32_t)k * 1000UL); }
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

        // Snapshot once per FRAME, not per point. Calling updateSnapshot()
        // every tick costs an xSemaphoreTake/Give + a 64-bit division per
        // point; at 45kpps (22us budget) that overruns the per-point window,
        // so galvoTask cannot reach the target rate, the ring drains slower
        // than pattern_engine pushes, and the buffer overflows (= flicker).
        // s_point_idx==0 marks the start of a new frame.
        if (s_point_idx == 0) {
            updateSnapshot();
            period_us = s_snap.period_us;  // pick up runtime kpps changes
        }

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
            writeDAC8562XY(0x8000, 0x8000);
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
                // ACQUIRE: pair with RELEASE in setDebugOutput() so all five
                // debug fields are seen consistently (no torn read where
                // s_hw_debug_active is true but x/y/rgb are stale).
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                int16_t dx = s_dbg_x, dy = s_dbg_y;
                uint8_t dr = s_dbg_r, dg = s_dbg_g, db = s_dbg_b;
                writeDAC8562XY((uint16_t)(dx + 32768), (uint16_t)(dy + 32768));
                rgbWrite(dr, dg, db, s_snap.thresh_r, s_snap.thresh_g, s_snap.thresh_b);
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
                    s_frames_total++;
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
            // Projection zone clip: a lit point outside the user-defined
            // polygon is forced blank (laser OFF, mirror keeps moving). Test
            // uses pre-limit signed coords (p.x/p.y) = polygon coordinate
            // space. Already-blanked points are unaffected.
            bool zone_blank = p.blank ||
                (s_snap.zone.enabled && !s_snap.zone.contains(p.x, p.y));
            if (zone_blank) {
                    rgbOff();
                    if (s_laser_off_hold > 0) {
                        // Still within hold window: keep DAC parked at the
                        // last lit position while LEDC/6N137 finish turning off.
                        writeDAC8562XY(s_last_dac_x, s_last_dac_y);
                        s_laser_off_hold--;
                    } else {
                        writeDAC8562XY((uint16_t)x, (uint16_t)y);
                        s_last_dac_x = (uint16_t)x;
                        s_last_dac_y = (uint16_t)y;
                    }
                } else {
                    writeDAC8562XY((uint16_t)x, (uint16_t)y);
                    // PWM duty: 8-bit. Apply dimmer + thermal scale + gain.
                    uint8_t dim = gState.master_dimmer.load();  // atomic load
                    uint8_t thermalScale = gState.thermal_power_scale.load();  // atomic load
                    uint8_t dimEff = (uint8_t)(((uint16_t)dim * thermalScale) / 255);
                    // FIX: s_snap instead of gConfig — race-condition-free
                    uint8_t r = (uint8_t)(((uint32_t)p.r * dimEff * s_snap.gain_r) / (255UL * 255));
                    uint8_t g = (uint8_t)(((uint32_t)p.g * dimEff * s_snap.gain_g) / (255UL * 255));
                    uint8_t b = (uint8_t)(((uint32_t)p.b * dimEff * s_snap.gain_b) / (255UL * 255));
                    rgbWrite(r, g, b, s_snap.thresh_r, s_snap.thresh_g, s_snap.thresh_b);
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
    rebuildGammaLut(gConfig.gamma_val);
    // ALWAYS allocate Ring-Buffer
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
    devcfg.spics_io_num   = -1;        // CS managed manually (GPIO W1TS/W1TC)
    devcfg.queue_size     = 4;
    spi_bus_add_device(SPI2_HOST, &devcfg, &s_galvo_spi);

    // Manual CS: GPIO10 output, idle HIGH (DAC inactive).
    // Must be before dac8562Init() and self-test.
    gpio_set_direction((gpio_num_t)PIN_GALVO_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_GALVO_CS, 1);

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
            GALVO_GPIO_W1TC = GALVO_CS_MASK;
            esp_err_t err = spi_device_polling_transmit(s_galvo_spi, &t);
            GALVO_GPIO_W1TS = GALVO_CS_MASK;
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

// Real drawn-frame rate: counts only genuine ring-tail advances (see galvoTask),
// so underrun replays and disarmed/blanked output are correctly excluded.
uint32_t fps() {
    uint32_t now = millis();
    if (now - s_last_fps_t >= 1000) {
        s_fps_cached  = (s_frames_total - s_frames_last) * 1000 / (now - s_last_fps_t);
        s_frames_last = s_frames_total;
        s_last_fps_t  = now;
        gState.fps = s_fps_cached;
    }
    return s_fps_cached;
}

uint32_t bufferFillLevel() {
    int32_t fill = (int32_t)s_ring_head - (int32_t)s_ring_tail;
    if (fill < 0) fill += RING_FRAMES;
    return (uint32_t)(fill * 100 / RING_FRAMES);
}

uint32_t overflowCount() { return s_overflow_count; }

/* ============================================================
 * Sample-Rate Autotune
 *
 * Ring buffer overflow (pushFrame() above) happens when the configured
 * galvo_kpps exceeds what galvoTask can actually sustain: pattern_engine
 * paces its pushFrame() calls assuming the ring drains at exactly kpps
 * (see the "drain_ms = n / gProjection.galvo_kpps" pacing in
 * pattern_engine.cpp), but galvoTask's busy-wait loop only *targets* that
 * rate -- above a hardware/CPU-dependent ceiling its per-tick SPI + RGB +
 * safety work no longer fits in period_us, the real drain rate falls
 * behind the configured one, and the ring fills up.
 *
 * That ceiling depends on the live pattern (point count, corner density)
 * as well as the hardware, so it can't be computed analytically -- this
 * runs a binary search against whatever is currently playing, each trial
 * measuring real overflow events (not theoretical throughput) over a
 * settle+observe window.
 * ============================================================ */
static constexpr uint16_t AUTOTUNE_KPPS_MIN    = 12;
static constexpr uint16_t AUTOTUNE_KPPS_MAX    = 60;
static constexpr uint32_t AUTOTUNE_SETTLE_MS   = 300;   // let old ring frames drain at the new rate
static constexpr uint32_t AUTOTUNE_MEASURE_MS  = 1500;  // overflow observation window per trial
static constexpr uint16_t AUTOTUNE_MARGIN_KPPS = 2;     // safety margin subtracted from the found ceiling
static constexpr uint8_t  AUTOTUNE_MAX_STEPS   = 8;     // ceil(log2(60-12)) + headroom
static constexpr uint8_t  AUTOTUNE_FILL_LIMIT  = 85;    // % -- a climbing fill predicts an overflow just past the window

static AutotuneStatus  s_autotune;
static uint16_t        s_autotune_saved_kpps = 0;
static volatile bool   s_autotune_abort_req  = false;

// Applies a candidate kpps and reports whether it ran overflow-free for
// AUTOTUNE_MEASURE_MS. Bumps gPatternCacheGen like the normal /api/projection
// handler does, so cached presets recompute their PPS-scaled point density
// for the candidate rate instead of testing against stale geometry.
static bool autotuneTrial(uint16_t kpps) {
    gProjection.galvo_kpps = kpps;
    gPatternCacheGen++;
    vTaskDelay(pdMS_TO_TICKS(AUTOTUNE_SETTLE_MS));

    uint32_t before   = s_overflow_count;
    uint32_t deadline = millis() + AUTOTUNE_MEASURE_MS;
    uint32_t max_fill = 0;
    while (millis() < deadline) {
        if (s_autotune_abort_req) return false;
        uint32_t fill = bufferFillLevel();
        if (fill > max_fill) max_fill = fill;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return (s_overflow_count - before) == 0 && max_fill < AUTOTUNE_FILL_LIMIT;
}

static void autotuneTask(void*) {
    uint16_t hi = gProjection.galvo_rated_kpps;
    if (hi < AUTOTUNE_KPPS_MIN || hi > AUTOTUNE_KPPS_MAX) hi = AUTOTUNE_KPPS_MAX;
    uint16_t lo   = AUTOTUNE_KPPS_MIN;
    uint16_t best = lo;
    bool     floor_ok = false;

    s_autotune.step           = 1;
    s_autotune.candidate_kpps = lo;
    if (!s_autotune_abort_req) {
        floor_ok = autotuneTrial(lo);
        if (floor_ok) {
            best = lo;
            uint16_t l = lo, h = hi;
            while (l <= h && s_autotune.step < AUTOTUNE_MAX_STEPS) {
                if (s_autotune_abort_req) break;
                uint16_t mid = l + (h - l) / 2;
                s_autotune.candidate_kpps = mid;
                s_autotune.step++;
                if (autotuneTrial(mid)) { best = mid; l = mid + 1; }
                else if (mid == AUTOTUNE_KPPS_MIN) break;
                else h = mid - 1;
            }
        }
    }

    if (s_autotune_abort_req) {
        gProjection.galvo_kpps = s_autotune_saved_kpps;
        gPatternCacheGen++;
        s_autotune.done = false;
        ESP_LOGI(TAG, "Autotune aborted, restored %u kpps", s_autotune_saved_kpps);
    } else {
        // No margin to subtract if even the floor was unstable -- best IS
        // the floor already, there's nothing lower to fall back to.
        uint16_t result = (floor_ok && best > AUTOTUNE_KPPS_MIN + AUTOTUNE_MARGIN_KPPS)
                         ? best - AUTOTUNE_MARGIN_KPPS : best;
        gProjection.galvo_kpps    = result;
        gPatternCacheGen++;
        s_autotune.result_kpps    = result;
        s_autotune.floor_unstable = !floor_ok;
        s_autotune.done           = true;
        ESP_LOGI(TAG, "Autotune done: %u kpps (raw ceiling %u, floor_ok=%d)", result, best, (int)floor_ok);
        LOG_I(logbuf::CAT_GALVO, "Galvo autotune: %u kpps (raw ceiling %u, margin -%u)",
              result, best, AUTOTUNE_MARGIN_KPPS);
    }
    s_autotune.running = false;
    vTaskDelete(nullptr);
}

void autotuneStart() {
    if (s_autotune.running) return;   // already running
    s_autotune_saved_kpps = gProjection.galvo_kpps;
    s_autotune_abort_req  = false;
    s_autotune             = AutotuneStatus{};
    s_autotune.running     = true;
    s_autotune.step_total  = AUTOTUNE_MAX_STEPS;
    xTaskCreatePinnedToCore(autotuneTask, "autotune", 4096, nullptr, 2, nullptr, 0);
}

void autotuneAbort() {
    if (s_autotune.running) s_autotune_abort_req = true;
}

AutotuneStatus autotuneStatus() { return s_autotune; }

// ── applyCalibration: Offset, Gain, Flip from gConfig ─────────────────
// Called by etherdream.cpp for externally received frames.
void applyCalibration(LaserPoint* pts, size_t n) {
    // Snapshot calibration once under mutex (runs on Core 0 / etherdream);
    // gConfig is concurrently written by the WebUI handler task. Reading the
    // 7 fields directly in the loop could mix old/new values mid-frame.
    int16_t x_gain, y_gain, x_off, y_off;
    bool swap, inv_x, inv_y;
    uint16_t dacLimitMin, dacLimitMax;
    if (xSemaphoreTake(mtx::config, pdMS_TO_TICKS(2)) == pdTRUE) {
        x_gain = gConfig.galvo_x_gain; y_gain = gConfig.galvo_y_gain;
        x_off  = gConfig.galvo_x_offset; y_off = gConfig.galvo_y_offset;
        swap   = gConfig.swap_xy; inv_x = gConfig.invert_x; inv_y = gConfig.invert_y;
        dacLimitMin = gConfig.dac_limit_min; dacLimitMax = gConfig.dac_limit_max;
        xSemaphoreGive(mtx::config);
    } else {
        // Mutex busy: skip calibration this frame rather than risk a torn read.
        return;
    }
    int32_t limLo = (int32_t)dacLimitMin - 0x8000;
    int32_t limHi = (int32_t)dacLimitMax - 0x8000;
    for (size_t i = 0; i < n; i++) {
        int32_t x = pts[i].x, y = pts[i].y;
        if (swap) { int32_t tmp = x; x = y; y = tmp; }
        if (inv_x) x = -x;
        if (inv_y) y = -y;
        x = (int32_t)x * x_gain / 32767;
        y = (int32_t)y * y_gain / 32767;
        x += x_off;
        y += y_off;
        pts[i].x = (int16_t)constrain(x, limLo, limHi);
        pts[i].y = (int16_t)constrain(y, limLo, limHi);
    }
}

// ── Hardware debug API ────────────────────────────────────────────────────
void setDebugOutput(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (gConfig.invert_x) x = -x;
    if (gConfig.invert_y) y = -y;
    s_dbg_x = x; s_dbg_y = y;
    s_dbg_r = r; s_dbg_g = g; s_dbg_b = b;
    // RELEASE: ensure all five fields are committed before galvoTask (Core 1)
    // observes s_hw_debug_active=true via its ACQUIRE fence.
    __atomic_thread_fence(__ATOMIC_RELEASE);
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