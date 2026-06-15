#pragma once

/* ═══════════════════════════════════════════════════════════════════
 * ESP32-S3-DevKitC-1 J2 PINOUT (right side, 22 pins, verified from photo)
 * ───────────────────────────────────────────────────────────────────
 *  Pin  1: GND         Pin  2: GPIO43(TX)  Pin  3: GPIO44(RX)
 *  Pin  4: GPIO1(RTC)  Pin  5: GPIO2(RTC)
 *  Pin  6: GPIO42      Pin  7: GPIO41      Pin  8: GPIO40      Pin  9: GPIO39
 *  Pin 10: GPIO38      Pin 11: GPIO37      Pin 12: GPIO36      Pin 13: GPIO35
 *  Pin 14: GPIO0(BOOT) Pin 15: GPIO45      Pin 16: GPIO48      Pin 17: GPIO47
 *  Pin 18: GPIO21(RTC) Pin 19: GPIO20(USB_D+) Pin 20: GPIO19(USB_D-)
 *  Pin 21: GND         Pin 22: GND
 *  [NOTE: Pin1=GND, Pin19/20=USB reserved, Pin13=PSRAM internal]
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * ESP32-S3-DevKitC-1 J1 PINOUT (left side, 22 pins, verified from photo)
 * ───────────────────────────────────────────────────────────────────
 *  Pin  1: 3V3        Pin  2: 3V3       Pin  3: RST
 *  Pin  4: GPIO4      Pin  5: GPIO5     Pin  6: GPIO6     Pin  7: GPIO7
 *  Pin  8: GPIO15     Pin  9: GPIO16    Pin 10: GPIO17    Pin 11: GPIO18
 *  Pin 12: GPIO8      Pin 13: GPIO3     Pin 14: GPIO46
 *  Pin 15: GPIO9      Pin 16: GPIO10    Pin 17: GPIO11    Pin 18: GPIO12
 *  Pin 19: GPIO13     Pin 20: GPIO14
 *  Pin 21: 5V0 (VIN)  Pin 22: GND
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * pinmap.h  --  Hardware Pin Assignments
 * ESP32-S3-DevKitC-1 (N16R8) Replacement Controller
 *
 * GPIO selection follows these rules:
 *   - GPIO 35..37 are NOT available on the N16R8 (Octal PSRAM)
 *   - GPIO 0, 3, 45, 46 = strapping pins, critical during boot
 *   - GPIO 19, 20 = USB D-/D+ (reserved for native USB CDC + Ether Dream/Helios)
 *   - SPI galvo DAC is on SPI2 (DMA-capable, FSPI)
 *   - I2C bus shared between RGB-DAC, OLED, external expansion
 */

/* ============================================================
 * SPI 2 (FSPI) -- DAC8562 galvo DAC, DMA streaming
 * ============================================================ */
#define PIN_GALVO_SCK    12
#define PIN_GALVO_MOSI   11
#define PIN_GALVO_CS     10
// PIN_GALVO_LDAC entfaellt: DAC8562 CMD=011 Write+Update without LDAC pulse
// galvo signal path: DAC8562 bipolar +-2.5V -> OPA4134UA channel 1/2 (gain x2) -> +-5V -> XIN/YIN
// OPA4134UA: 14-SOIC, V+=Pin11(+15V), V-=Pin4(-15V), channel3+4=NC (Unity-Buffer)

/* ============================================================
 * RGB TTL-Laser moduleation via LEDC-PWM
 * RGB: GPIO TTL via 6N137 optocouplers (GPIO7=R, GPIO8=G, GPIO21=B)
 * OPA4134 quad op-amp: galvo diff-amp X/Y + VREF unity buffer
 * PWM 50 kHz, 8-bit -> brightness via duty cycle (0=off, 255=full on)
 * 100Ω series resistor on each laser TTL input
 * ============================================================ */
#define PIN_LASER_R       7   /* Red   638 nm — GPIO 7 (was I2C_SCL, now free) */
#define PIN_LASER_G       8   /* Green 520 nm — GPIO 8 (was I2C_SDA, now free) */
#define PIN_LASER_B      21   /* J2-Pin18 = GPIO21 */   /* Blue  445 nm — GPIO 21 (free) */

#define LEDC_CH_R         2   /* LEDC-channels 0+1 reserviert for Fan */
#define LEDC_CH_G         3
#define LEDC_CH_B         4
#define LEDC_FREQ_RGB 50000   /* 50 kHz — invisible flicker, TTL-compatible */
#define LEDC_RES_RGB      8   /* 8-Bit = 256 brightness steps */

/* GPIO 35/36/37 BLOCKED on N16R8 -- are Octal-PSRAM pins!
 * Laser TTL moved to GPIO 7/8/21.
 * HARDWARE-MANDATORY: 10kΩ pull-down on each laser TTL pin!
 * Prevents laser-ON if GPIO floats (boot/crash). */

/* ILDA RGB->TTL comparator (LM393, 2x DIP-8):
 * ILDA channel 0-5V analog -> LM393 threshold ~1.0V -> TTL HIGH/LOW -> laser
 * reference voltage: 10kOhm voltage divider 3.3V->1.0V (2x10kOhm + 1x5.1kOhm)   */

/* ============================================================
 * UART 1 -- DMX receive (via MAX485 with 6N137 isolation)
 * ============================================================ */
#define PIN_DMX_RX        4   /* MAX485 module RO → ESP32, UART1 RX */
// PIN_DMX_TX    removed — DMX receive only, no transmit
// PIN_DMX_DE_RE removed -- DE+RE of the module fixed to GND
// GPIO5, GPIO6 now free

/* ============================================================
 * Mode-Switch & Status
 * ============================================================ */
// PIN_MODE_SWITCH (GPIO1) removed -- physical ILDA interface/LF092 fallback droppedt (v3)
                              /* HIGH = ESP32 active, LOW = LF-092 active */
#define PIN_STATUS_LED   48   /* J2-Pin16 = GPIO48 */   /* Onboard-RGB-LED */

/* ============================================================
 * Safety Hardware
 * ============================================================ */
#define PIN_WATCHDOG_OUT 14   /* J1-Pin20 = GPIO14 */
#define PIN_ESTOP        47   /* J2-Pin17 = GPIO47 */   /* emergency stop status (active = not pressed) */
#define PIN_LASER_ENABLE 38   /* J2-Pin10 = GPIO38 */   /* central enable — HIGH only when all safety checks pass */
                              /* Goes to SSR/relay that releases the laser power rail */
#define PIN_SCAN_FAIL_IN 39   /* J2-Pin9  = GPIO39 */   /* NE555 Scan-Fail output (not yet populated) */

/* ============================================================
 * UI -- rotary encoder with button (no OLED display for now;
 * to be replaced by 7-segment display from original housing)
 * ============================================================ */
#define PIN_ENC_A        15
#define PIN_ENC_B        40   /* J2-Pin8  = GPIO40 */
#define PIN_ENC_BTN      41   /* J2-Pin7  = GPIO41 */   /* Encoder button (not yet populated) */

/* ============================================================
 * Temperature (DS18B20 1-Wire)
 * All 5 probes on one bus. 4.7 kOhm pullup to 3.3 V required.
 * ============================================================ */
#define PIN_ONEWIRE      18

/* ============================================================
 * Fan control (HW-517 MOSFET module -- already fitted)
 * J1-Header: PWM → Q1 (Fan 1), PWM → Q2 (Fan 2)
 * 25 kHz PWM, 8-bit. Minimum ~15% duty for reliable startup.
 * ============================================================ */
#define PIN_FAN1_PWM     16
#define PIN_FAN2_PWM     17

/* ============================================================
 * Diagnostics / expansion
 * ============================================================ */
#define PIN_DEBUG_TX     43   /* J2-Pin2  = GPIO43 */   /* USB-CDC primary, hardware-UART0 for emergency */
#define PIN_DEBUG_RX     44   /* J2-Pin3  = GPIO44 */

/* ============================================================
 * Reserved pins (do NOT use)
 * ============================================================ */
/*  19, 20 -- USB D-/D+
 *  35, 36, 37 -- PSRAM (Octal)
 *   0, 3, 45, 46 -- Strapping-Pins
 */

/* ============================================================
 * SPI 2 (FSPI) -- SD card (shares bus with DAC8562)
 * SCK + MOSI shared with DAC8562.
 * MISO added (DAC8562 has no MISO → no conflict).
 * ============================================================ */
#define PIN_SD_MISO       2   /* J2-Pin5  = GPIO2  */   /* SD card MISO (strapping pin, pull-up required) */
#define PIN_SD_CS         9   /* J1-Pin15 = GPIO9  */   /* SD card CS — independent from DAC CS=GPIO10   */
// SD card shares SPI2 bus with DAC8562:
//   SCK  = PIN_GALVO_SCK  (GPIO12) — shared
//   MOSI = PIN_GALVO_MOSI (GPIO11) — shared
//   MISO = PIN_SD_MISO    (GPIO2)  — SD only (DAC has no MISO)
//   CS   = PIN_SD_CS      (GPIO9)  — SD only
// GPIO39 and GPIO41 are now free (unconnected, not used by SD anymore).

/* ILDA player config */
#define ILDA_MAX_FILES   40   /* Maximum count of .ild files on SD */
#define ILDA_MAX_PATH   128   /* maximum path length */

/* ── alias definitions for legacy code references ──────────────────── */
#define PIN_LASER_EN     PIN_LASER_ENABLE   /* emergencyStop alias */
// PIN_DAC_CLR_N (GPIO13, J1-Pin19): /CLR of DAC8562 (Pin5) — hardware pull-up 10kΩ
// In firmware GPIO13 is briefly pulsed LOW at init (CLR active) then HIGH (DAC active)
#define PIN_DAC_CLR_N    13   /* DAC8562 /CLR — J1-Pin19 = GPIO13 */

// Optional: hardware heartbeat for retriggerable blank failsafe
// #define PIN_HEARTBEAT  45  // uncomment if hardware is fitted
