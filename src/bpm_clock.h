#pragma once
/**
 * bpm_clock.h -- global BPM clock, three input sources
 *
 * Priority: DMX > Tap > Manual. DMX only counts as active while a signal is
 * actually present (dmx_in::isReceiving()); Tap only counts while a tempo
 * has been established (>=2 taps) and the last tap was recent (<=3s ago).
 * Falls back to Manual otherwise.
 *
 * Lock-free by design (same tradeoff as galvo_out.cpp's gain/thresh
 * snapshot): all shared fields are single aligned scalars, written from
 * Core 0 (WebUI/DMX) and read from Core 1 (galvo frame loop). A torn read
 * costs at most one stale frame, never wrong geometry.
 */

#include <stdint.h>

namespace bpm_clock {

enum Source : uint8_t { SRC_MANUAL = 0, SRC_TAP = 1, SRC_DMX = 2 };

struct BpmClock {
    float    bpm      = 120.0f;   // effective BPM, resolved by tickMs()
    Source   source   = SRC_MANUAL;
    uint32_t phase_ms = 0;        // cached beat phase [0..999], see tickMs()
};
extern BpmClock gBpm;

// Loads manual BPM + DMX channel from NVS ("bpm" namespace).
void init();

// Manual source (Source 1). Clamped 20.0-300.0, persisted to NVS.
void  setManualBpm(float bpm);
float manualBpm();

// Tap tempo (Source 2). Registers one tap; BPM = average of the last up to
// 3 intervals between the last up to 4 taps. Resets if the gap since the
// previous tap exceeds 3000ms.
void tap();

// DMX channel used for Source 3 (Source 3). Absolute 1-based DMX address
// (1-512), independent of gConfig.dmx_address. Clamped, persisted to NVS.
void     setDmxChannel(uint16_t channel);
uint16_t dmxChannel();

// Per-frame hook -- call once per frame (galvo_out.cpp::updateSnapshot()).
// Resolves the active source (DMX > Tap > Manual), updates gBpm.bpm/source,
// and returns the beat phase in permille [0..999] for this frame.
uint32_t tickMs();

// Cheap accessor: beat phase as [0.0..1.0), from the last tickMs() call.
// Does not itself re-resolve the source or re-sample time.
float phaseNormalized();

}  // namespace bpm_clock
