#pragma once
/**
 * galvo_out.h -- Galvo-XY and RGB-Output
 *
 *   DAC8562 -- 16-bit dual-DAC, SPI up to 50 MHz, shared LDAC for synchronous latching
 *   LEDC-PWM -- TTL RGB-Laser GPIO35(R)/36(G)/37(B), 50kHz 8-Bit
 *   OPA4134 -- quad op-amp: ch1/ch2 = galvo diff-amp (X/Y), ch3 = VREF buffer
 *
 * Streaming-Architektur:
 *   - ring buffer with LaserPoint frames in PSRAM
 *   - hardware timer on core 1 triggers SPI transfer per sample
 *   - DMA-capable SPI with FIFO pre-filling
 *   - RGB updated in parallel via I2C-DMA (slightly slower, acceptable)
 *
 * Timing-Ziel: 50 kSamples/s = 20 µs per Punkt
 *   - SPI at 20 MHz: 32 bit = 1.6 µs -> comfortable
 *   - I2C at 400 kHz: 4 bytes = ~100 µs -> TOO SLOW for 50 kHz!
 *     Solution: I2C update at 5 kHz, RGB interpolated locally between updates.
 *             Sufficient for visually clean color fading, geometry remains scharf.
 */

#include "config.h"
#include <driver/spi_master.h>

namespace galvo {

void init();
void start();
void stop();

// Feed pattern: copies points into the next free ring buffer slot
// Non-blocking, returns false if buffer full.
bool pushFrame(const LaserPoint* points, size_t count);
void applyCalibration(LaserPoint* pts, size_t n);

// Telemetrie
uint32_t pointsPerSec();
uint32_t bufferFillLevel();   // 0..100 %

// ── Hardware debug: direkte Ausgabe ohne Pattern-Engine ──────────────────
// Only active if s_hw_debug_active=true (set via /api/debug/hw).
// galvoTask reads s_dbg_x/y/r/g/b and outputs them directly while active.
// safety: automatisch deenabled if laser_armed=false.
void setDebugOutput(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b);
void clearDebugOutput();       // to center + laser OFF
bool isDebugOutputActive();

// ── DAC / HW status ──────────────────────────────────────────────────────
// dacOk()    : true if DAC8562 SPI init succeeded and self-test passed.
//              Always false when noHwMode() is true.
// noHwMode() : true when gDebugNoHW flag is set (SPI skipped at boot).
bool dacOk();
bool noHwMode();

// ── Shared SPI2 bus access ───────────────────────────────────────────────
// Returns the SPI2_HOST handle after galvo::init() has run.
// sd_card::init() uses this to add the SD card as a second SPI2 device
// (CS=GPIO9) without reinitializing the bus.
// Must NOT be called before galvo::init().
spi_host_device_t getSpi2Host();

// Logs the most recent DAC X/Y codes (~2Hz, called from a non-IRAM task)
// when gConfig.dac_debug_log is enabled. Safe to call every ~500ms.
// ── DAC low-level diagnostics ─────────────────────────────────────────────
// Sends a raw 24-bit DAC8562 command directly (bypasses normal galvo output).
// cmd/addr per TI datasheet Table 12/13 (3 bits each), data = 16-bit payload.
// Only safe to call while s_hw_debug_active is true (galvoTask paused).
// Returns false if SPI transmit fails.
bool sendRawCommand(uint8_t cmd3, uint8_t addr3, uint16_t data);

// Holds a fixed code on one channel for durationMs (1-60000), then returns
// to center (0x8000). Blocks the calling task for durationMs.
// Requires laser_armed -- intended for hardware debugging only.
void holdChannelValue(uint8_t channel, uint16_t code, uint32_t durationMs);

void logDacDebugIfPending();

}  // namespace galvo
