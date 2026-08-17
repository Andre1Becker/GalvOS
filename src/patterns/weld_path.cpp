#include "weld_path.h"
#include <math.h>

namespace weld {

size_t buildArcLengthPath(const SourceStroke* strokes, size_t strokeCount,
                           PathNode* nodesOut, size_t maxNodes,
                           float* liftSOut, size_t maxLifts, size_t& liftCount,
                           float& pathLen) {
    size_t nc = 0;
    float  s  = 0.f;
    liftCount = 0;
    for (size_t st = 0; st < strokeCount; st++) {
        const SourceStroke& stroke = strokes[st];
        if (stroke.count < 2) continue;
        for (uint16_t i = 0; i < stroke.count && nc < maxNodes; i++) {
            if (i > 0) {
                float dx = stroke.x[i] - stroke.x[i - 1];
                float dy = stroke.y[i] - stroke.y[i - 1];
                s += sqrtf(dx * dx + dy * dy);
            }
            nodesOut[nc].x = stroke.x[i];
            nodesOut[nc].y = stroke.y[i];
            nodesOut[nc].s = s;
            nodesOut[nc].lift = (i == 0);
            if (i == 0 && liftCount < maxLifts) liftSOut[liftCount++] = s;
            nc++;
        }
        if (stroke.closed && stroke.count >= 2 && nc < maxNodes) {
            // Repeat vertex 0 as the closing node -- not a new lift, the
            // stroke just wraps back to where it started.
            float dx = stroke.x[0] - stroke.x[stroke.count - 1];
            float dy = stroke.y[0] - stroke.y[stroke.count - 1];
            s += sqrtf(dx * dx + dy * dy);
            nodesOut[nc].x = stroke.x[0];
            nodesOut[nc].y = stroke.y[0];
            nodesOut[nc].s = s;
            nodesOut[nc].lift = false;
            nc++;
        }
    }
    if (nc < 2) { pathLen = 0.f; return 0; }
    pathLen = nodesOut[nc - 1].s;
    return nc;
}

Sample sampleAt(const PathNode* nodes, size_t count, float pathLen, float s) {
    Sample r = {false, 0.f, 0.f};
    if (count < 2 || s < 0.f || s > pathLen) return r;
    int lo = 0, hi = (int)count - 1, idx = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (nodes[mid].s <= s) { idx = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if ((size_t)idx >= count - 1) { r.valid = true; r.x = nodes[count - 1].x; r.y = nodes[count - 1].y; return r; }
    const PathNode& a = nodes[idx];
    const PathNode& b = nodes[idx + 1];
    float ds = b.s - a.s;
    float t = (ds > 1e-6f) ? (s - a.s) / ds : 0.f;
    r.valid = true;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    return r;
}

bool crossesLift(const float* liftS, size_t liftCount, float sa, float sb) {
    float lo = sa < sb ? sa : sb, hi = sa < sb ? sb : sa;
    for (size_t i = 0; i < liftCount; i++) {
        float ls = liftS[i];
        if (ls > lo && ls < hi) return true;
    }
    return false;
}

} // namespace weld
