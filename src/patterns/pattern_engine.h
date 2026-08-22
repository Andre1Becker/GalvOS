#pragma once
#include "config.h"
#include "preset_patterns.h"

namespace patterns {

void init();
void task(void*);
void setManualMode(bool enable, uint8_t pattern_id);
void triggerTestPattern(const char* name);
void stopTestPattern();           // cancel running hw test pattern immediately
void setPreset(presets::Preset idx);   // Preset::None = preset off
presets::Preset getPreset();
void   setCurve(int8_t idx);  // -1 = off, 0-8 = curve
int8_t getCurve();
void   setPaintActive(bool active);  // Paint-by-Finger mode on/off
bool   getPaintActive();

// ── Render-stage timing (diagnostic) ─────────────────────────────────────
// Per-frame cost of the Preset render path, split by stage, so "the pattern
// engine costs N% of Core 0" can be attributed to an actual stage instead of
// guessed at. Accumulated by task() and drained by the reader, which yields
// the mean over whatever interval passed since the previous read.
//
// Deliberately not mutex-protected: these are plain counters written once per
// frame by a single producer and read by an HTTP handler. A torn read costs
// one skewed diagnostic sample and nothing else, which is not worth taking a
// lock on the render path for.
struct RenderTiming {
    uint32_t frames;       // frames folded into this sample
    uint32_t cacheHits;    // of those, served from the whole-pipeline cache
    uint32_t generateUs;   // presets::generate() incl. optimizer::optimize()
    uint32_t pipelineUs;   // colour/duplicator/mirror/kaleido/transform/calib
    uint32_t pushUs;       // galvo::pushFrame() (memcpy into the PSRAM ring)
    uint32_t totalUs;      // whole frame, excluding paceProducer()'s delay
};
// Returns the accumulated totals and resets them. Averages = field / frames.
RenderTiming takeRenderTiming();

}  // namespace patterns
