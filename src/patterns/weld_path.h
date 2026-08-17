#pragma once
/**
 * weld_path.h -- arc-length path construction shared by every Welding-effect
 * source (see weld_patterns.cpp): Paint canvas strokes and Text Mode glyph
 * outlines both reduce to the same generic list of open/closed pen strokes,
 * so the torch/afterglow/spark renderer walks one flattened, arc-length-
 * parameterized path regardless of which mode produced it.
 *
 * Deliberately dependency-free (no Arduino.h/PSRAM/millis) so it is host-
 * testable -- see test/test_optimizer/test_weld_path.cpp.
 */
#include <stdint.h>
#include <stddef.h>

namespace weld {

// One sub-path (pen stroke) to stitch into the flattened path. A view into
// caller-owned storage -- no copy. `closed` repeats vertex 0 as a trailing
// node so the arc length wraps back to the start.
struct SourceStroke {
    const float* x;
    const float* y;
    uint16_t     count;
    bool         closed;
};

// One flattened, arc-length-parameterized path node. `lift` marks a stroke's
// first node -- the gap between strokes costs zero arc length, but a sample
// window spanning a lift boundary must not interpolate across it (that would
// draw a straight line between two logically disconnected strokes, e.g. two
// separate letters).
struct PathNode { float x, y, s; bool lift; };

// Build the flattened path + per-stroke lift-boundary table from `strokes`.
// Strokes with count < 2 are skipped. Returns the node count written to
// `nodesOut` (capacity `maxNodes`), or 0 if fewer than 2 usable nodes
// resulted (nothing to draw) -- `pathLen` is only meaningful when the return
// value is nonzero. `liftSOut`/`liftCount` collect each stroke's start
// arc-length (capacity `maxLifts`; extra lifts beyond capacity are silently
// dropped -- the corresponding boundary just stops being detected).
size_t buildArcLengthPath(const SourceStroke* strokes, size_t strokeCount,
                           PathNode* nodesOut, size_t maxNodes,
                           float* liftSOut, size_t maxLifts, size_t& liftCount,
                           float& pathLen);

// Sample the path at arc-length `s`. Invalid (valid=false) outside
// [0, pathLen] -- callers use this to fade a trail at open ends instead of
// wrapping across a jump.
struct Sample { bool valid; float x, y; };
Sample sampleAt(const PathNode* nodes, size_t count, float pathLen, float s);

// True if any lift boundary sits strictly between sa and sb (i.e. the window
// crosses a stroke boundary / blank jump). Order-independent in sa/sb.
bool crossesLift(const float* liftS, size_t liftCount, float sa, float sb);

} // namespace weld
