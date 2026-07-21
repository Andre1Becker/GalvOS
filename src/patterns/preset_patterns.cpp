#include "preset_patterns.h"
#include "countdown_timer.h"
#include "point_optimizer.h"
#include "util/mem_registry.h"
#include <math.h>
#include <string.h>
#include <Arduino.h>

namespace presets {

static constexpr float PI2  = 2.0f * M_PI;
static constexpr float SC   = 18000.0f;

static inline float ssc(uint8_t s) { return 0.25f + (s / 255.0f) * 1.1765f; }
static inline float aang(uint32_t ph, uint8_t sp, float m=1.0f) {
    if (!sp) return 0.f;
    return fmodf(ph * (sp / 5000.0f) * m, PI2);
}
static inline void ap(LaserPoint* o, size_t& n, size_t mx,
                       float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t bl=0) {
    if (n>=mx) return;
    o[n]={int16_t(constrain(x,-32767.f,32767.f)),
          int16_t(constrain(y,-32767.f,32767.f)),r,g,b,bl};
    n++;
}
static inline float L(float a,float b,float t){return a+(b-a)*t;}
// contDelta()/csweep() -- continuous-sweep parameterisation (#2). Instead of
// drawing a fixed [0,2pi] loop plus a per-frame rotation offset (which makes
// frame K's end and frame K+1's start land at different angles -> a lit jump
// the galvo chases, the "Delle" seam), the sweep parameter advances
// continuously: span = 2pi + contDelta(sp); since phase (ph) is monotonic,
// frame K's last param == frame K+1's first param exactly. seam = 0,
// C1-continuous, zero state, zero budget. fmod in double preserves precision
// to ph=0xFFFFFF. Visual unchanged (closed figure rotating at contDelta/frame).
static inline float contDelta(uint8_t sp) { return (sp / 255.0f) * 0.05f; }
static inline float csweep(uint32_t ph, uint8_t sp, int i, int N) {
    const double span = (double)PI2 + contDelta(sp);
    double base = fmod((double)ph * span, (double)PI2);
    return (float)(base + span * (double)i / (double)N);
}
static inline bool isContinuous(uint8_t idx) {
    switch (idx) {
        case 0: case 22: case 23: case 24: case 26: case 27: case 100: case 76: return true;
        default: return false;
    }
}

static inline int adaptN(uint8_t sz, int base, int min_pts=8, int max_pts=512){
    float factor = 0.4f + (sz / 255.f) * 1.2f;
    int n = (int)(base * factor);
    if (n < min_pts) n = min_pts;
    if (n > max_pts) n = max_pts;
    return n;
}

// liveOptimizerConfig() -- converts the WebUI-tunable OptimizerLiveConfig
// into the optimizer module's own OptimizerConfig type.
static inline optimizer::OptimizerConfig liveOptimizerConfig() {
    optimizer::OptimizerConfig cfg;
    cfg.corner_angle_deg   = gOptimizerConfig.corner_angle_deg;
    cfg.min_corner_pts     = gOptimizerConfig.min_corner_pts;
    cfg.max_corner_pts     = gOptimizerConfig.max_corner_pts;
    cfg.pts_per_1000_units = gOptimizerConfig.pts_per_1000_units;
    cfg.min_segment_pts    = gOptimizerConfig.min_segment_pts;
    cfg.blank_samples      = gOptimizerConfig.blank_samples;
    cfg.max_pts_per_frame  = gOptimizerConfig.max_pts_per_frame;
    cfg.min_blank_samples  = gOptimizerConfig.min_blank_samples;
    cfg.blank_pts_per_1000_units = gOptimizerConfig.blank_pts_per_1000_units;
    cfg.min_interior_pts_per_segment = gOptimizerConfig.min_interior_pts_per_segment;
    cfg.stage1_blank_target = gOptimizerConfig.stage1_blank_target;
    cfg.resample_enabled       = gOptimizerConfig.resample_enabled;
    cfg.resample_spacing_units = gOptimizerConfig.resample_spacing_units;
    cfg.ringing_comp_enabled = gOptimizerConfig.ringing_comp_enabled;
    cfg.ring_freq_hz         = gOptimizerConfig.ring_freq_hz;
    cfg.ring_damping_ratio   = gOptimizerConfig.ring_damping_ratio;
    cfg.galvo_kpps           = gProjection.galvo_kpps;
    cfg.transform                    = optimizer::gLiveTransform;  // Phase 3: live Z-rot + move
    cfg.vel_clamp_enabled            = gOptimizerConfig.vel_clamp_enabled;
    cfg.max_step_units               = gOptimizerConfig.max_step_units;
    cfg.accel_clamp_enabled          = gOptimizerConfig.accel_clamp_enabled;
    cfg.max_accel_units              = gOptimizerConfig.max_accel_units;
    // PPS-derived scaling: density + both clamps from rated/output kpps.
    optimizer::applyPpsScaling(cfg, gProjection.galvo_rated_kpps, gProjection.galvo_kpps);
    return cfg;
}

// ngon() -- GalvOS v5: migrated to point_optimizer (Pillar 1, adaptive
// density). Describes the polygon as `sides` corner vertices and lets
// optimizer::optimize() decide point placement.
static size_t ngon(LaserPoint*o,size_t mx,int sides,float sc,float off,uint8_t r,uint8_t g,uint8_t b){
    if (sides < 3 || sides > 64) return 0;
    optimizer::PathVertex verts[64];
    for (int s = 0; s < sides; s++) {
        float a = PI2 * s / sides + off;
        verts[s].x = cosf(a) * sc;
        verts[s].y = sinf(a) * sc;
        verts[s].r = r; verts[s].g = g; verts[s].b = b;
        verts[s].lift = false;
    }
    optimizer::PathSegment seg(verts, (size_t)sides, /*closed=*/true);
    return optimizer::optimize(&seg, 1, o, mx, liveOptimizerConfig());
}

// star() -- GalvOS v5.3: migrated to point_optimizer (Pillar 1).
// Previously: fixed 16-step interpolation per edge, no corner awareness.
// Now: builds PathVertex array with alternating outer/inner radii and
// passes to optimize(closed=true). The optimizer's corner-angle-aware
// density is the main win: a 5-star has 144°/36° alternating exterior
// angles -- exactly what cornerPointCount() was designed for. The tip
// corners (144°) get max_corner_pts dwell; the inner valleys (36°,
// below corner_angle_deg threshold) get min_corner_pts. This removes
// the visual "soft tip / sharp valley" asymmetry of the fixed-step version.
static size_t star(LaserPoint*o,size_t mx,int pts,float outer,float inner,float off,uint8_t r,uint8_t g,uint8_t b){
    if (pts < 2 || pts > 32) return 0;
    const int nverts = pts * 2;
    optimizer::PathVertex verts[64]; // max 32 points * 2 = 64 vertices
    for (int s = 0; s < nverts; s++) {
        float a = PI2 * s / nverts + off - (float)(M_PI/2);
        float rad = (s % 2 == 0) ? outer : inner;
        verts[s].x = cosf(a) * rad;
        verts[s].y = sinf(a) * rad;
        verts[s].r = r; verts[s].g = g; verts[s].b = b;
        verts[s].lift = false;
    }
    optimizer::PathSegment seg(verts, (size_t)nverts, /*closed=*/true);
    return optimizer::optimize(&seg, 1, o, mx, liveOptimizerConfig());
}

struct P3D{float x,y,z;};
static void prj(P3D v,float ry,float rx,float sc,float&ox,float&oy){
    float rx2=v.x*cosf(ry)+v.z*sinf(ry),rz=-v.x*sinf(ry)+v.z*cosf(ry);
    ox=(rx2)*sc; oy=(v.y*cosf(rx)-rz*sinf(rx))*sc;
}
static const P3D CV[]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
static const int CE[][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};

// wf() -- GalvOS v5: migrated to point_optimizer (Pillar 1, adaptive
// density + budget-aware blanking). See wf() comments in original for
// full rationale on buildWfChains() and chain-based approach.
static const int WF_MAX_EDGES = 64;
static const int WF_MAX_VERTS = 32;

// Greedily walks unused edges into the longest possible continuous
// chain, closing it into a loop if the walk returns to its own start.
// A chain only ends where it truly must: a dead end (no unused edge left
// at `cur`) or a closed loop -- NOT merely because `cur` has more than
// two edges. The previous version broke the walk at any vertex with
// degree != 2, which for any proper polyhedron (min vertex degree 3,
// e.g. every cube/pyramid/octahedron/tetrahedron corner) fragmented
// every single edge into its own isolated 2-point segment before it
// could ever be tested against `start`. Each such fragment gets its own
// optimizer blank-jump (see optimize()'s per-segment emitBlankJump()),
// so the wireframe never actually closed anywhere -- corners looked
// "open" even where three+ edges met at one point. Removing the degree
// check lets the walk continue across those vertices too, so e.g. a
// cube now resolves into 2 closed square loops + 4 open struts instead
// of 12 disconnected edges, and a pyramid into 1 closed base loop + 2
// open apex chains instead of 8 disconnected edges -- far fewer blank
// jumps, and the corners that are topologically closed (loops) render
// as closed instead of flickering open.
static int buildWfChains(int nv, const int(*E)[2], int ne,
                          int out_chains[][WF_MAX_VERTS + 1],
                          int out_chain_len[], bool out_closed[],
                          int max_chains) {
    if (nv > WF_MAX_VERTS) return 0;
    static bool used[WF_MAX_EDGES];
    memset(used, 0, sizeof(used));
    static int adj_edge[WF_MAX_VERTS][WF_MAX_VERTS];
    static int adj_other[WF_MAX_VERTS][WF_MAX_VERTS];
    static int adj_count[WF_MAX_VERTS];
    memset(adj_count, 0, sizeof(adj_count));
    for (int e = 0; e < ne && e < WF_MAX_EDGES; e++) {
        int a = E[e][0], b = E[e][1];
        if (a < 0 || a >= nv || b < 0 || b >= nv) continue;
        if (adj_count[a] < WF_MAX_VERTS) { adj_edge[a][adj_count[a]] = e; adj_other[a][adj_count[a]] = b; adj_count[a]++; }
        if (adj_count[b] < WF_MAX_VERTS) { adj_edge[b][adj_count[b]] = e; adj_other[b][adj_count[b]] = a; adj_count[b]++; }
    }

    int nchains = 0;
    for (int e0 = 0; e0 < ne && e0 < WF_MAX_EDGES && nchains < max_chains; e0++) {
        if (used[e0]) continue;
        used[e0] = true;
        int start = E[e0][0], cur = E[e0][1];
        static int chain[WF_MAX_VERTS + 1];
        int len = 0;
        chain[len++] = start;
        chain[len++] = cur;
        while (len <= WF_MAX_VERTS) {
            int next_edge = -1, next_v = -1;
            for (int k = 0; k < adj_count[cur]; k++) {
                if (!used[adj_edge[cur][k]]) {
                    next_edge = adj_edge[cur][k];
                    next_v = adj_other[cur][k];
                    break;
                }
            }
            if (next_edge < 0) break;
            if (next_v == start) {
                used[next_edge] = true;
                out_closed[nchains] = true;
                goto chain_done;
            }
            used[next_edge] = true;
            chain[len++] = next_v;
            cur = next_v;
        }
        out_closed[nchains] = false;
        chain_done:
        for (int i = 0; i < len; i++) out_chains[nchains][i] = chain[i];
        out_chain_len[nchains] = len;
        nchains++;
    }
    return nchains;
}

static size_t wf(LaserPoint*o,size_t mx,const P3D*V,int nv,const int(*E)[2],int ne,float ry,float rx,float sc,uint8_t r,uint8_t g,uint8_t b){
    if (ne > WF_MAX_EDGES) ne = WF_MAX_EDGES;
    static int chains[WF_MAX_EDGES][WF_MAX_VERTS + 1];
    static int chain_len[WF_MAX_EDGES];
    static bool chain_closed[WF_MAX_EDGES];
    int nchains = buildWfChains(nv, E, ne, chains, chain_len, chain_closed, WF_MAX_EDGES);

    optimizer::PathSegment segs[WF_MAX_EDGES];
    static optimizer::PathVertex verts[WF_MAX_EDGES][WF_MAX_VERTS + 1];
    for (int c = 0; c < nchains; c++) {
        int len = chain_len[c];
        for (int i = 0; i < len; i++) {
            float ox, oy;
            prj(V[chains[c][i]], ry, rx, sc, ox, oy);
            verts[c][i].x = ox; verts[c][i].y = oy;
            verts[c][i].r = r;  verts[c][i].g = g;  verts[c][i].b = b;
            verts[c][i].lift = (i == 0);
        }
        segs[c] = optimizer::PathSegment(verts[c], (size_t)len, chain_closed[c]);
    }
    return optimizer::optimize(segs, nchains, o, mx, liveOptimizerConfig());
}

// line() -- GalvOS v5.3: migrated to point_optimizer.
// Previously: manual 40-blank settle + fixed S-step interpolation.
// Now: 2-vertex open PathSegment with lift=true on the start vertex.
// optimizer::emitBlankJump() handles the blank travel to start;
// emitSegment() handles interior density proportional to edge length.
// The S parameter is kept for API compatibility but ignored -- optimizer
// controls actual density via pts_per_1000_units.
static void line(LaserPoint*o,size_t&n,size_t mx,
                 float x0,float y0,float x1,float y1,
                 uint8_t r,uint8_t g,uint8_t b,
                 int /*S*/=20)
{
    optimizer::PathVertex verts[2];
    verts[0].x = x0; verts[0].y = y0;
    verts[0].r = r;  verts[0].g = g;  verts[0].b = b;
    verts[0].lift = true;   // blank-jump to start
    verts[1].x = x1; verts[1].y = y1;
    verts[1].r = r;  verts[1].g = g;  verts[1].b = b;
    verts[1].lift = false;
    optimizer::PathSegment seg(verts, 2, /*closed=*/false);
    size_t written = optimizer::optimize(&seg, 1, o + n, mx - n, liveOptimizerConfig());
    n += written;
}

// ─── PARAMETRIC CURVE -> OPTIMIZER BRIDGE ────────────────────
// Design doc Section 9.2 previously excluded parametric curves from the
// optimizer on the grounds that they have "no discrete corners". That is
// true of the MATH, but not of the emitted point stream: every ap() loop
// already discretises the curve into a fixed vertex list at a hardcoded N.
// Feeding that same vertex list to optimize() as a PathSegment costs nothing
// and buys the whole pipeline -- length-proportional resampling, the frame
// budget clamp (max_pts_per_frame), eased blank jumps, and the velocity /
// acceleration clamps. The generator's N becomes a SHAPE-FIDELITY hint
// (enough vertices to describe the curve) rather than a scanner-rate
// decision; the optimizer then re-spaces points to the galvo's real limits.
//
// Corner detection is harmless here: a smoothly sampled curve has near-zero
// exterior angle at every vertex, so cornerPointCount() returns
// min_corner_pts throughout. The Smooth profile sets max_corner_pts low so
// no budget is wasted looking for corners that do not exist.
//
// CURVE_MAX_PTS caps the stack vertex buffer. Generators sampling above this
// are clamped -- the optimizer resamples to the budget anyway, so a higher
// input count only costs stack and CPU without reaching the DAC.
static const int CURVE_MAX_PTS = 512;

// curveEmit() -- feed a caller-filled PathVertex list to the optimizer as one
// segment. `closed` mirrors PathSegment semantics (implicit final edge back
// to vertex 0). Returns points written into o[].
static size_t curveEmit(LaserPoint*o,size_t mx,const optimizer::PathVertex*v,int count,bool closed){
    if(count<2||!v) return 0;
    optimizer::PathSegment seg(v,(size_t)count,closed);
    return optimizer::optimize(&seg,1,o,mx,liveOptimizerConfig());
}

// vertexBudget() -- the largest input vertex count the optimizer can still
// bring under the frame budget for the ACTIVE profile.
//
// This exists because of a hard floor inside optimize(): Stage 2 scales only
// the interior (length-proportional) points down to fit the budget. Corner
// points are explicitly treated as FIXED overhead and are never scaled, and
// every vertex is charged corner points -- cornerPointCount() runs at each
// one and returns somewhere in [min_corner_pts, max_corner_pts] according to
// the exterior angle. So:
//
//     output_points >= corner_cost * N + blank_overhead
//
// no matter how far pts_per_1000_units is scaled down. Feeding N above that
// bound yields a frame the optimizer physically cannot fit; it silently
// overruns max_pts_per_frame.
//
// The trap is that corner_cost is NOT min_corner_pts for a sampled curve. On
// a circle sampled at N the exterior angle per vertex is 2*pi/N: large N is
// safe (angle -> 0 -> min_corner_pts), but MODERATE N scores above the
// threshold and charges up to max_corner_pts per vertex. Measured on a plain
// circle under the Smooth profile (cad=60, mincp=2, maxcp=3, cap=1300):
//
//     N=482 -> 1467 points = 3.04/vertex  (max_corner_pts, 13% over budget)
//     N=576 -> 1174 points = 2.04/vertex  (min_corner_pts, fits)
//
// Note this is non-monotonic in N -- which is exactly why it survived until a
// size-slider sweep caught it. Budget against the WORST case, max_corner_pts.
//
// seg_count is the number of PathSegments the caller will emit; the blank
// reserve is derived from the config the same way optimize() computes it
// (blank_samples + min_blank_samples per segment, plus one closing blank)
// rather than from a guessed percentage -- a fixed fudge factor was measured
// too small for the Smooth profile (reserved 195 pts, actual cost 227) and
// left ~2% overruns on Hypotrochoid/Epitrochoid.
//
// A margin is still applied on top because emitBlankJump() scales the
// movement run with jump distance and Stage 1 may shrink blank_samples, so
// the reserve is an upper bound rather than exact.
static int vertexBudget(size_t seg_count = 1) {
    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    uint8_t worst_corner_cost = cfg.max_corner_pts;
    if (worst_corner_cost < cfg.min_corner_pts) worst_corner_cost = cfg.min_corner_pts;
    if (worst_corner_cost < 1) worst_corner_cost = 1;
    // Mirror of optimize()'s blank_overhead: one jump per segment + closing.
    uint32_t blank_reserve =
        (uint32_t)(cfg.blank_samples + cfg.min_blank_samples) * (uint32_t)(seg_count + 1);
    // Leave room for the interior points too: a curve reduced to nothing but
    // corner dots is not a curve. Reserve a further 10% of the frame for them.
    uint32_t interior_reserve = (uint32_t)((float)cfg.max_pts_per_frame * 0.10f);
    int32_t avail = (int32_t)cfg.max_pts_per_frame
                  - (int32_t)blank_reserve - (int32_t)interior_reserve;
    if (avail < 8) avail = 8;
    int budget = avail / (int)worst_corner_cost;
    if (budget < 8) budget = 8;
    if (budget > CURVE_MAX_PTS) budget = CURVE_MAX_PTS;
    return budget;
}

// curve() -- samples a parametric function into vertices, then hands the list
// to the optimizer. fn(i, x, y, r, g, b) fills one sample. This is the single
// migration path for every former "for(i..N) ap(...)" curve generator.
//
// N is clamped to vertexBudget(): oversampling is not merely wasteful, it
// breaks the frame budget outright (see vertexBudget above). Clamping here
// rather than at each call site means every migrated generator inherits the
// guard, including ones using adaptN() whose N grows with the size slider.
template<typename F>
static size_t curve(LaserPoint*o,size_t mx,int N,bool closed,F fn){
    if(N<2) return 0;
    const int vb=vertexBudget();
    if(N>vb) N=vb;                 // clamp BEFORE sampling
    // fn receives (i, N, ...) so it maps its parameter (angle, t, x) across the
    // ACTUAL emitted count. Passing i/N with the pre-clamp N truncated the
    // shape: a circle requested at N=576 but emitted at N=375 covered only
    // 375/576 = 65% of the sweep -- the "only upper half visible" bug. Any
    // closed curve whose adaptN() exceeded vertexBudget() at large size was
    // affected, not just the circle.
    optimizer::PathVertex v[CURVE_MAX_PTS];
    for(int i=0;i<N;i++){
        float x=0,y=0; uint8_t r=255,g=255,b=255;
        fn(i,N,x,y,r,g,b);
        v[i]=optimizer::PathVertex(x,y,r,g,b,false);
    }
    return curveEmit(o,mx,v,N,closed);
}

// sinewave() -- migrated: samples the wave into a vertex list and lets the
// optimizer own point placement. The former 40-point fixed blank run at the
// start is gone: `lift=true` on vertex 0 makes emitBlankJump() produce a
// distance-proportional, smoothstep-eased jump instead (Pillar 2).
// budget_share: when >0, caps this call's slice of the frame budget. Callers
// that invoke sinewave() more than once per frame (p37 Multi Wave) MUST pass
// it: optimize() clamps to min(max_out, cfg.max_pts_per_frame), so shrinking
// only max_out leaves max_pts_per_frame at the full-frame value and each call
// independently plans a whole frame's worth of points -- 3 calls then emit ~3x
// the budget. Lowering max_pts_per_frame per call is what actually binds.
static size_t sinewave(LaserPoint*o,size_t mx,float A,float f,float ph_off,float sc,uint8_t r,uint8_t g,uint8_t b,int N=120,uint16_t budget_share=0){
    const float effA=A*gLivePreset.wave_amp, effF=f*gLivePreset.wave_freq;
    if(N<2) return 0;
    optimizer::OptimizerConfig cfg=liveOptimizerConfig();
    if(budget_share>0 && budget_share<cfg.max_pts_per_frame)
        cfg.max_pts_per_frame=budget_share;
    // Sine waves are smooth curves with no corners, so the corner-dwell-based
    // vertex budget formula (avail/max_corner_pts) would artificially cap N
    // far below what the optimizer can actually spend. Skip it: the optimizer
    // enforces cfg.max_pts_per_frame on output, which is already set to
    // budget_share above. Just guard against the array limit.
    if(N>CURVE_MAX_PTS-1) N=CURVE_MAX_PTS-1;
    optimizer::PathVertex v[CURVE_MAX_PTS];
    for(int i=0;i<=N;i++){
        float x=L(-1.f,1.f,i/float(N));
        v[i]=optimizer::PathVertex(x*sc,effA*sinf(effF*x*PI2+ph_off)*sc,r,g,b,i==0);
    }
    optimizer::PathSegment seg(v,(size_t)(N+1),false);
    return optimizer::optimize(&seg,1,o,mx,cfg);
}

// circ_draw() -- GalvOS v5.3: migrated to optimizer via ngon().
// Previously: fixed S-step loop with manual blank jump.
// Now: delegates to ngon() which uses PathSegment(closed=true).
// S parameter controls polygon sides (clamped to [16,64]).
// This automatically improves all callers: circ_draw() is used by
// Vehicles (p91-p98), party silhouettes, and combo presets.
static void circ_draw(LaserPoint*o,size_t&n,size_t m,float cx,float cy,float r2,uint8_t cr,uint8_t cg,uint8_t cb,int S=24){
    if (S < 16) S = 16;
    if (S > 64) S = 64;
    // ngon() generates centered at origin; we shift via per-vertex offset.
    // Build PathVertex directly so we can apply cx/cy offset.
    optimizer::PathVertex verts[64];
    for (int i = 0; i < S; i++) {
        float a = PI2 * i / S;
        verts[i].x = cx + cosf(a) * r2;
        verts[i].y = cy + sinf(a) * r2;
        verts[i].r = cr; verts[i].g = cg; verts[i].b = cb;
        verts[i].lift = false;
    }
    optimizer::PathSegment seg(verts, (size_t)S, /*closed=*/true);
    size_t written = optimizer::optimize(&seg, 1, o + n, m - n, liveOptimizerConfig());
    n += written;
}

// rect_outline() -- 4 line segments forming a rectangle.
// Migrated automatically via line() migration.
static void rect_outline(LaserPoint*o,size_t&n,size_t m,float x0,float y0,float x1,float y1,uint8_t r,uint8_t g,uint8_t b,int S=10){
    line(o,n,m,x0,y0,x1,y0,r,g,b,S); line(o,n,m,x1,y0,x1,y1,r,g,b,S);
    line(o,n,m,x1,y1,x0,y1,r,g,b,S); line(o,n,m,x0,y1,x0,y0,r,g,b,S);
}

// scrollX() -- vehicle horizontal scroll helper, unchanged.
static float scrollX(uint32_t ph,uint8_t sp){
    float spd=0.06f+(sp/255.f)*0.44f;
    float t=fmodf(ph*spd*0.00025f,1.f);
    return (t*2.f-1.f)*SC*1.25f;
}

typedef size_t(*PFn)(LaserPoint*,size_t,uint32_t,uint8_t,uint8_t);

// ─── GEOMETRY 0-9 ──────────────────────────────────────────
static size_t p00(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    // Circle: continuous-sweep (#2), seam=0/C1. No offset, no closing dup.
    // bl always 0: csweep already yields exact frame-to-frame position
    // continuity, so blanking i==0 was punching a real 1-point dark notch
    // into the ring every frame -- static (span==2pi exactly) it sits at a
    // fixed angle and is imperceptible; rotating (span!=2pi) it advances
    // frame to frame and is seen as a travelling "Delle" (dent).
    float sc=SC*ssc(sz)*.9f;int N=adaptN(sz,360,60,900);
    // Migrated to optimizer via curve(). closed=false preserves the #2
    // continuous-sweep contract: csweep() already spans exactly one loop plus
    // contDelta, so vertex[N-1] -> vertex[0] must NOT be re-drawn as a closing
    // edge. Sampling stays at adaptN() (shape fidelity); the optimizer
    // re-spaces to the galvo rate and enforces max_pts_per_frame.
    return curve(o,m,N,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float t=csweep(ph,sp,i,N);
        x=cosf(t)*sc; y=sinf(t)*sc; r=255; g=255; b=255;
    });
}
static size_t p01(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,4,SC*ssc(sz)*.9f,aang(ph,sp),255,255,0);}
static size_t p02(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,3,SC*ssc(sz)*.9f,aang(ph,sp),0,255,255);}
static size_t p03(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,5,SC*ssc(sz)*.9f,aang(ph,sp),255,255,0);}
static size_t p04(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,6,SC*ssc(sz)*.9f,aang(ph,sp),0,255,0);}
static size_t p05(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,8,SC*ssc(sz)*.9f,aang(ph,sp),0,0,255);}
static size_t p06(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,4,s,s*.36f,aang(ph,sp),255,0,0);}
static size_t p07(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,5,s,s*.36f,aang(ph,sp),255,255,0);}
static size_t p08(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,6,s,s*.36f,aang(ph,sp),0,255,0);}
static size_t p09(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,8,s,s*.36f,aang(ph,sp),0,255,255);}

// ─── LINES 10-14 ────────────────────────────────────────────
// These use line() which is now optimizer-backed. Grid p12 benefits
// most: 8 line segments share the flicker budget via one optimize() call
// each, preventing the fixed-cost 40-blank overhead from dominating.
static size_t p10(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f;line(o,n,m,-s,0,s,0,255,0,0,50);line(o,n,m,0,-s,0,s,0,255,0,50);return n;}
static size_t p11(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.65f;line(o,n,m,-s,-s,s,s,0,255,255,50);line(o,n,m,s,-s,-s,s,255,0,255,50);return n;}
static size_t p12(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f,st=s*2.f/3.f;for(int i=0;i<=3;i++){float x=-s+i*st;line(o,n,m,x,-s,x,s,0,255,255,20);}for(int i=0;i<=3;i++){float y=-s+i*st;line(o,n,m,-s,y,s,y,0,255,255,20);}return n;}
static size_t p13(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    // Straight stroke -> line() (already optimizer-backed). The old fixed
    // 60-sample loop is exactly what edgeInteriorCount() derives from length.
    size_t n=0;float s=SC*ssc(sz)*.9f,off=(aang(ph,sp)/PI2*2.f-1.f)*s;
    line(o,n,m,-s,off,s,off,255,255,0);
    return n;
}
static size_t p14(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float s=SC*ssc(sz)*.9f,a=aang(ph,sp);
    const float ca=cosf(a),sa=sinf(a);
    line(o,n,m,-s*ca,-s*sa, s*ca, s*sa, 255,0,255);
    return n;
}

// ─── spirals 15-22 ──────────────────────────────────────────
// Parametric curves — not migrated (no discrete vertices).
static size_t p15(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);const int N=adaptN(sz,200,30,400);
    return curve(o,m,N,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float t=(float)i/N,a=t*PI2*3.5f+off,rr=t*sc;
        x=cosf(a)*rr; y=sinf(a)*rr;
        r=(uint8_t)(t*255); g=(uint8_t)((1-t)*255); b=128;
    });
}
// Point counts raised (2.5-3x, scaled to combined frequency = curve
// complexity) -- galvo (15kpps rated, driven at 30kpps) needs tighter
// per-step spacing on high-curvature Lissajous paths to close cleanly.
static size_t p16(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    // closed=true: the Lissajous ratio makes vertex[N-1] adjacent to vertex[0];
    // the optimizer draws that final edge instead of the old duplicate sample.
    // N is a SHAPE-FIDELITY count, not a point-rate knob: it is the smallest
    // value whose chord sagitta (max deviation of the true curve from the
    // straight chord the optimizer interpolates along) stays <= 40 DAC units
    // -- 0.12% of half-scale, well under the projected spot size, measured at
    // size=255. Oversampling here is actively harmful: the frame budget is the
    // binding constraint, so every surplus input vertex steals budget from
    // chord walking and RAISES the galvo's per-tick step.
    const int N=112;
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=PI2*i/(float)N;
        x=cosf(t+off)*sc; y=sinf(2*t+M_PI/4.f)*sc;
        rr=0; gg=255; bb=255;
    });
}
static size_t p17(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    // closed=true: the Lissajous ratio makes vertex[N-1] adjacent to vertex[0];
    // the optimizer draws that final edge instead of the old duplicate sample.
    // N is a SHAPE-FIDELITY count, not a point-rate knob: it is the smallest
    // value whose chord sagitta (max deviation of the true curve from the
    // straight chord the optimizer interpolates along) stays <= 40 DAC units
    // -- 0.12% of half-scale, well under the projected spot size, measured at
    // size=255. Oversampling here is actively harmful: the frame budget is the
    // binding constraint, so every surplus input vertex steals budget from
    // chord walking and RAISES the galvo's per-tick step.
    const int N=176;
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=PI2*i/(float)N;
        x=cosf(2*t+off)*sc; y=sinf(3*t+M_PI/4.f)*sc;
        rr=0; gg=255; bb=255;
    });
}
static size_t p18(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    // closed=true: the Lissajous ratio makes vertex[N-1] adjacent to vertex[0];
    // the optimizer draws that final edge instead of the old duplicate sample.
    // N is a SHAPE-FIDELITY count, not a point-rate knob: it is the smallest
    // value whose chord sagitta (max deviation of the true curve from the
    // straight chord the optimizer interpolates along) stays <= 40 DAC units
    // -- 0.12% of half-scale, well under the projected spot size, measured at
    // size=255. Oversampling here is actively harmful: the frame budget is the
    // binding constraint, so every surplus input vertex steals budget from
    // chord walking and RAISES the galvo's per-tick step.
    const int N=240;
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=PI2*i/(float)N;
        x=cosf(3*t+off)*sc; y=sinf(4*t+M_PI/3.f)*sc;
        rr=0; gg=255; bb=255;
    });
}
static size_t p19(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    // closed=true: the Lissajous ratio makes vertex[N-1] adjacent to vertex[0];
    // the optimizer draws that final edge instead of the old duplicate sample.
    // N is a SHAPE-FIDELITY count, not a point-rate knob: it is the smallest
    // value whose chord sagitta (max deviation of the true curve from the
    // straight chord the optimizer interpolates along) stays <= 40 DAC units
    // -- 0.12% of half-scale, well under the projected spot size, measured at
    // size=255. Oversampling here is actively harmful: the frame budget is the
    // binding constraint, so every surplus input vertex steals budget from
    // chord walking and RAISES the galvo's per-tick step.
    const int N=288;
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=PI2*i/(float)N;
        x=cosf(3*t+off)*sc; y=sinf(5*t+M_PI/6.f)*sc;
        rr=0; gg=0; bb=255;
    });
}
static size_t p20(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    // closed=true: the Lissajous ratio makes vertex[N-1] adjacent to vertex[0];
    // the optimizer draws that final edge instead of the old duplicate sample.
    // N is a SHAPE-FIDELITY count, not a point-rate knob: it is the smallest
    // value whose chord sagitta (max deviation of the true curve from the
    // straight chord the optimizer interpolates along) stays <= 40 DAC units
    // -- 0.12% of half-scale, well under the projected spot size, measured at
    // size=255. Oversampling here is actively harmful: the frame budget is the
    // binding constraint, so every surplus input vertex steals budget from
    // chord walking and RAISES the galvo's per-tick step.
    const int N=352;
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=PI2*i/(float)N;
        x=cosf(5*t+off)*sc; y=sinf(6*t+PI2/5.f)*sc;
        rr=255; gg=255; bb=0;
    });
}
static size_t p21(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    // Two arms = two open segments in ONE optimize() call, so the blank jump
    // between them is planned inside the shared frame budget rather than
    // emitted blind by the old second loop.
    const int N=150;
    optimizer::PathVertex v[2*N];
    for(int i=0;i<N;i++){float t=i/(float)N,a=t*PI2*3.f+off,r=t*sc;
        v[i]=optimizer::PathVertex(cosf(a)*r,sinf(a)*r,255,80,0,i==0);}
    for(int i=0;i<N;i++){float t=i/(float)N,a=t*PI2*3.f+off+(float)M_PI,r=t*sc;
        v[N+i]=optimizer::PathVertex(cosf(a)*r,sinf(a)*r,0,80,255,i==0);}
    optimizer::PathSegment segs[2]={
        optimizer::PathSegment(v,N,false),
        optimizer::PathSegment(v+N,N,false)};
    return optimizer::optimize(segs,2,o,m,liveOptimizerConfig());
}
static size_t p22(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f;const int N=200;
    // closed=false: csweep() (#2) already guarantees frame-to-frame position
    // continuity; a closing edge would cut a chord across the rose.
    return curve(o,m,N,false,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=csweep(ph,sp,i,N),rad=sc*cosf(3*t);
        x=rad*cosf(t); y=rad*sinf(t);
        rr=255; gg=100; bb=0;
    });
}

// ─── Curves 23-28 ────────────────────────────────────────────
// Parametric curves — not migrated.
static size_t p23(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f;const int N=200;
    // closed=false: csweep() (#2) already guarantees frame-to-frame position
    // continuity; a closing edge would cut a chord across the rose.
    return curve(o,m,N,false,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=csweep(ph,sp,i,N),rad=sc*cosf(4*t);
        x=rad*cosf(t); y=rad*sinf(t);
        rr=255; gg=50; bb=150;
    });
}
static size_t p25(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.045f,a=aang(ph,sp);const int N=adaptN(sz,200,20,300);
    const float ca=cosf(a),sa=sinf(a);
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float t=PI2*i/(float)N;
        float hx=sc*16*powf(sinf(t),3);
        float hy=sc*(13*cosf(t)-5*cosf(2*t)-2*cosf(3*t)-cosf(4*t));
        x=hx*ca-hy*sa; y=hx*sa+hy*ca;
        r=255; g=0; b=80;
    });
}
static size_t p26(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f;const int N=adaptN(sz,500,60,800);
    // closed=false -- csweep (#2) continuity, see p00.
    return curve(o,m,N,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float t=csweep(ph,sp,i,N),d=1+sinf(t)*sinf(t);
        x=sc*cosf(t)/d; y=sc*sinf(t)*cosf(t)/d;
        r=0; g=200; b=255;
    });
}
static size_t p27(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f;const int N=200;
    // The astroid's 4 cusps are true corners -- cornerPointCount() now dwells
    // there instead of the old uniform sampling rounding them off.
    return curve(o,m,N,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float t=csweep(ph,sp,i,N);
        x=sc*powf(cosf(t),3); y=sc*powf(sinf(t),3);
        r=200; g=255; b=50;
    });
}
static size_t p28(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    const float R=3,r=1,d=2.5f,peakNorm=1.f/(R+r+d);
    float sc=SC*ssc(sz)*.9f*peakNorm,off=aang(ph,sp);
    const int N=384;   // shape fidelity only; optimizer sets output density
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=PI2*i/(float)N+off;
        x=sc*((R+r)*cosf(t)-d*cosf((R+r)*t/r));
        y=sc*((R+r)*sinf(t)-d*sinf((R+r)*t/r));
        rr=0; gg=255; bb=100;
    });
}

// ─── 3D 29-34 ────────────────────────────────────────────────
static size_t p29(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return wf(o,m,CV,8,CE,12,aang(ph,sp,1),aang(ph,sp,.4f),SC*ssc(sz)*.65f,0,255,255);}
static size_t p30(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return wf(o,m,CV,8,CE,12,.6f,.4f,SC*ssc(sz)*.65f,255,255,0);}

// p31 Pyramid -- GalvOS v5.3: migrated from raw ap() to wf().
// Previously: separate loop for base quad + 4 apex edges with manual
// blank jumps and fixed 12-step interpolation.
// Now: 5 edges (base quad + 4 apex spokes) via wf(). Base vertices are
// degree 3 (2 base edges + 1 apex spoke each), so buildWfChains() joins
// the base into one closed loop and folds each pair of opposite apex
// spokes into one open chain through the apex -- 3 chains total instead
// of 8 disconnected edges.
static size_t p31(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    // Base quad vertices + apex
    static const P3D V[]={
        {-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1},  // 0-3: base
        {0,1,0}                                     // 4: apex
    };
    static const int E[][2]={
        {0,1},{1,2},{2,3},{3,0},  // base quad
        {0,4},{1,4},{2,4},{3,4}   // apex spokes
    };
    return wf(o,m,V,5,E,8,aang(ph,sp),0.3f,SC*ssc(sz)*.65f,255,255,0);
}

static size_t p32(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    static const P3D V[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const int E[][2]={{0,2},{0,3},{1,2},{1,3},{0,4},{0,5},{1,4},{1,5},{2,4},{2,5},{3,4},{3,5}};
    return wf(o,m,V,6,E,12,aang(ph,sp),.35f,SC*ssc(sz)*.7f,0,255,0);
}
static size_t p33(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    static const P3D V[]={{0,1,0},{.943f,-.333f,0},{-.471f,-.333f,.816f},{-.471f,-.333f,-.816f}};
    static const int E[][2]={{0,1},{0,2},{0,3},{1,2},{1,3},{2,3}};
    return wf(o,m,V,4,E,6,aang(ph,sp,1.2f),.4f,SC*ssc(sz)*.75f,0,0,255);
}
// ─── WELLEN 35-52 ────────────────────────────────────────────
// Parametric continuous curves — not migrated to optimizer.
static size_t p35(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return sinewave(o,m,.55f,1,aang(ph,sp),SC*ssc(sz)*.9f,0,255,255);}
static size_t p36(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float A=fabsf(sinf(aang(ph,sp)))*.8f+.1f;return sinewave(o,m,A,2,0,SC*ssc(sz)*.9f,0,255,0);}
static size_t p37(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    // Three independent optimize() calls: each must be told its share of the
    // frame budget explicitly (see sinewave()'s budget_share note), otherwise
    // all three plan a full frame each and the preset emits ~3x the budget.
    const uint16_t share=(uint16_t)(gOptimizerConfig.max_pts_per_frame/3);
    n+=sinewave(o,   m,        .3f, 1,t,      sc,255,0,0,  120,share);
    n+=sinewave(o+n, m>n?m-n:0,.2f, 2,t*1.5f, sc,0,255,0,  120,share);
    n+=sinewave(o+n, m>n?m-n:0,.15f,3,t*2.f,  sc,0,0,255,  120,share);
    return n;
}
static size_t p38(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: sampled polyline -> one open PathSegment.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float xx=L(-1.f,1.f,i/(float)(N-1));
        x=xx*sc;
        y=wa*(.3f*sinf(4*wf*xx*(float)M_PI+t)
             +.15f*sinf(8*wf*xx*(float)M_PI+t*1.7f)
             +.08f*sinf(16*wf*xx*(float)M_PI+t*2.3f))*sc;
        r=0; g=0; b=255;
    });
}
static size_t p39(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: the sampled polyline becomes one open
    // PathSegment (lift on v0 -> eased blank jump in, Pillar 2). The old
    // fixed N is kept as a shape-fidelity count; the optimizer resamples
    // to the galvo rate and enforces the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float xx=L(-1.f,1.f,i/(float)(N-1));

        x=xx*sc; y=(wa*.45f*(sinf(5*wf*xx*M_PI+t)+sinf(7*wf*xx*M_PI-t))*.5f)*sc;
        rr=255; gg=255; bb=0;
    });
}
static size_t p40(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: the sampled polyline becomes one open
    // PathSegment (lift on v0 -> eased blank jump in, Pillar 2). The old
    // fixed N is kept as a shape-fidelity count; the optimizer resamples
    // to the galvo rate and enforces the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float xx=L(-1.f,1.f,i/(float)(N-1));
        float ph2=fmodf((xx*2.f*wf+t/(float)M_PI),2.f);
        x=xx*sc; y=(wa*.6f*(ph2<1?ph2:ph2-2))*sc;
        rr=255; gg=0; bb=0;
    });
}
static size_t p41(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    // True square wave: explicit horizontal plateaus joined by vertical edges.
    // Migrated to optimizer -- this is the single biggest win in the Waves
    // block: every plateau/edge junction is a 90 deg corner, which is exactly
    // what cornerPointCount() exists for. The old code emitted ONE raw point
    // per corner, so the galvo rounded each transition; the optimizer now
    // dwells there and eases the approach (shapeEdgeT).
    const int cycles=(int)fmaxf(1.f,roundf(3*wf));      // whole cycles across width
    const int steps=cycles*2;                           // half-periods (plateaus)
    const float amp=wa*.55f*sc,phi=fmodf(t,PI2)/PI2;    // phase shifts plateaus
    // Worst case 2 vertices per step (vertical edge + plateau end) + start.
    const int maxv=2*(steps+1)+2;
    if(maxv>CURVE_MAX_PTS) return 0;
    optimizer::PathVertex v[CURVE_MAX_PTS];
    int nv=0;
    float px=-sc,py=((sinf(phi*PI2)>0)?1.f:-1.f)*amp;
    v[nv++]=optimizer::PathVertex(px,py,0,0,255,true);   // lift -> eased jump in
    for(int s=0;s<=steps && nv+2<=CURVE_MAX_PTS;s++){
        float x=L(-1.f,1.f,(float)s/steps)*sc;
        float lvl=(sinf(((float)s/steps+phi)*PI2*cycles)>0?1.f:-1.f)*amp;
        if(lvl!=py){ v[nv++]=optimizer::PathVertex(px,lvl,0,0,255,false); py=lvl; } // vertical edge
        v[nv++]=optimizer::PathVertex(x,py,0,0,255,false);                          // plateau
        px=x;
    }
    return curveEmit(o,m,v,nv,false);
}
static size_t p42(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: the sampled polyline becomes one open
    // PathSegment (lift on v0 -> eased blank jump in, Pillar 2). The old
    // fixed N is kept as a shape-fidelity count; the optimizer resamples
    // to the galvo rate and enforces the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float xx=L(-1.f,1.f,i/(float)(N-1));
        float env=expf(-12*xx*xx);
        x=xx*sc; y=(env*wa*sinf(8*wf*xx*M_PI+t)*.8f)*sc;
        rr=0; gg=255; bb=0;
    });
}
static size_t p43(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: the sampled polyline becomes one open
    // PathSegment (lift on v0 -> eased blank jump in, Pillar 2). The old
    // fixed N is kept as a shape-fidelity count; the optimizer resamples
    // to the galvo rate and enforces the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float xx=L(-1.f,1.f,i/(float)(N-1));

        x=xx*sc; y=(.5f*wa*(sinf(10*wf*xx*M_PI+t)+sinf(11*wf*xx*M_PI+t)))*sc;
        rr=255; gg=255; bb=0;
    });
}
static size_t p44(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    // 4 closed rings in ONE optimize() call: the inter-ring blank jumps are
    // now planned against the shared frame budget (MultiObject profile) rather
    // than emitted per-ring with a hardcoded sample count.
    const int NR=4, NA=80;
    optimizer::PathVertex v[NR*NA];
    optimizer::PathSegment segs[NR];
    for(int ring=1;ring<=NR;ring++){
        float r=ring/(float)NR,R=r*sc;
        optimizer::PathVertex* vr=v+(ring-1)*NA;
        for(int i=0;i<NA;i++){
            float a=PI2*i/(float)NA,rad=R*(1+.12f*wa*sinf(8*wf*a+r*8*wf-t));
            vr[i]=optimizer::PathVertex(cosf(a)*rad,sinf(a)*rad,
                                        0,(uint8_t)(100+155*r),255,i==0);
        }
        segs[ring-1]=optimizer::PathSegment(vr,NA,true);
    }
    return optimizer::optimize(segs,NR,o,m,liveOptimizerConfig());
}
static size_t p45(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: the sampled polyline becomes one open
    // PathSegment (lift on v0 -> eased blank jump in, Pillar 2). The old
    // fixed N is kept as a shape-fidelity count; the optimizer resamples
    // to the galvo rate and enforces the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float xx=L(-1.f,1.f,i/(float)(N-1));

        x=xx*sc; y=(.5f*wa*sinf(6*wf*xx*PI2+4*sinf(wf*xx*PI2*2+t)))*sc;
        rr=0; gg=255; bb=255;
    });
}
static size_t p46(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=200;
    // Migrated to optimizer: sampled polyline -> one open PathSegment.
    // N is now shape fidelity only; the optimizer owns output density,
    // blank-jump easing and the frame budget.
    return curve(o,m,N,false,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=i/(float)N,a=t*PI2*4*wf+off,r=sc*(1-t*.8f),w=.08f*wa*sinf(a*8);
        x=cosf(a)*(r+w*sc); y=sinf(a)*(r+w*sc);
        rr=(uint8_t)(t*255); gg=(uint8_t)((1-t)*200); bb=200;
    });
}
static size_t p47(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=200;
    // Migrated to optimizer: sampled polyline -> one open PathSegment.
    // N is now shape fidelity only; the optimizer owns output density,
    // blank-jump easing and the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float u=L(-1.f,1.f,i/(float)(N-1)),a=u*PI2*3*wf+t;
        x=u*sc; y=wa*sinf(a)*sc*.5f;
        r=0; g=(uint8_t)(128+127*cosf(a)); b=(uint8_t)(128+127*sinf(a));
    });
}
static size_t p48(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    // 5 rows as 5 open segments in one optimize() call -- shared budget,
    // eased row-to-row blank jumps.
    const int NROW=5, NX=61;
    optimizer::PathVertex v[NROW*NX];
    optimizer::PathSegment segs[NROW];
    for(int r0=0;r0<NROW;r0++){
        int row=r0-2; float y0=row*.36f;
        optimizer::PathVertex* vr=v+r0*NX;
        for(int i=0;i<NX;i++){
            float x=L(-1.f,1.f,i/(float)(NX-1));
            float y=y0+.12f*wa*sinf(x*PI2*3*wf+t+row*.7f);
            vr[i]=optimizer::PathVertex(x*sc,y*sc,0,(uint8_t)(128+127*sinf(row+t)),200,i==0);
        }
        segs[r0]=optimizer::PathSegment(vr,NX,false);
    }
    return optimizer::optimize(segs,NROW,o,m,liveOptimizerConfig());
}
static size_t p49(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: sampled polyline -> one open PathSegment.
    // N is now shape fidelity only; the optimizer owns output density,
    // blank-jump easing and the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float xx=L(-1.f,1.f,i/(float)(N-1));float yy=0;
        for(int k=1;k<=5;k+=2) yy+=sinf(k*xx*PI2*1.5f*wf+t)/k;
        x=xx*sc; y=yy*wa*.5f*sc;
        r=0; g=255; b=0;
    });
}
static size_t p50(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: sampled polyline -> one open PathSegment.
    // N is now shape fidelity only; the optimizer owns output density,
    // blank-jump easing and the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float xx=L(-1.f,1.f,i/(float)(N-1)),decay=expf(-2*fabsf(xx));
        x=xx*sc; y=decay*wa*sinf(10*wf*xx*(float)M_PI+t)*(.4f+.4f*fabsf(xx))*sc;
        r=255; g=0; b=255;
    });
}
static size_t p51(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: sampled polyline -> one open PathSegment.
    // N is now shape fidelity only; the optimizer owns output density,
    // blank-jump easing and the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float xx=L(-1.f,1.f,i/(float)(N-1)),build=.5f+.5f*xx;
        x=xx*sc; y=build*.5f*wa*sinf(PI2*(xx*2*wf-t*.3f))*.7f*sc;
        r=0; g=255; b=255;
    });
}
static size_t p52(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    const int N=120;
    // Migrated to optimizer: sampled polyline -> one open PathSegment.
    // N is now shape fidelity only; the optimizer owns output density,
    // blank-jump easing and the frame budget.
    return curve(o,m,N+1,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float xx=L(-1.f,1.f,i/(float)(N-1));float yy=0;
        static const int ks[5]={1,2,3,4,5};
        for(int j=0;j<5;j++) yy+=sinf(ks[j]*wf*xx*PI2+t*(j*.5f+.5f))*.2f/ks[j];
        x=xx*sc; y=yy*wa*sc;
        r=255; g=200; b=50;
    });
}

// ─── KOMPLEX 53-58 ───────────────────────────────────────────
// p53-p55, p58: parametric curves — not migrated.
// p53 Hypotrochoid -- R=6, r=1, d=3: rolling-circle radius 1 with pen offset
// 3 traces a clean 6-petal rounded rosette (a "flower", no loops crossing
// through the center). The earlier R=5/r=3/d=5 combo is Wikipedia's
// canonical hypotrochoid example and is mathematically valid, but it reads
// as a 3-pointed pretzel/star -- visually a near-duplicate of the
// Spirograph 5/3 preset below -- rather than a distinct flower shape.
// gcd(R-r, r) = 1 here, so the curve closes after a single 2*PI sweep
// (no 6*PI multi-revolution sweep needed).
static size_t p53(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    const float R=6,r=1,d=3,peakNorm=1.f/((R-r)+d);
    float sc=SC*ssc(sz)*.9f*peakNorm,off=aang(ph,sp);
    const int N=384;
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=PI2*i/(float)N+off;
        x=sc*((R-r)*cosf(t)+d*cosf((R-r)*t/r));
        y=sc*((R-r)*sinf(t)-d*sinf((R-r)*t/r));
        rr=255; gg=0; bb=255;
    });
}
static size_t p54(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.38f,off=aang(ph,sp);
    const int N=200;
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float t=PI2*i/(float)N;
        float e=expf(cosf(t))-2*cosf(4*t)-powf(sinf(t/12.f),5);
        x=sc*e*sinf(t+off); y=sc*e*cosf(t+off);
        r=255; g=255; b=0;
    });
}
// p55 Spirograph 5/3 -- hypotrochoid, fixed radii R=5, r=3. Curve closes
// after 3 revolutions of the outer parameter (t in [0,6*PI]); the previous
// [0,10*PI] sweep overshot the true period and retraced 2/3 of the curve
// a second time. d was also equal to r, which degenerates the hypotrochoid
// into a 5-cusp hypocycloid -- i.e. a pentagram, not a spirograph rosette.
// d < r now gives the proper looping rosette. Peak radius (R-r)+d is
// normalised so the figure fills the frame. off animates rotation.
static size_t p55(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float off=aang(ph,sp);
    const float R=5.f,r=3.f,d=1.5f,peakNorm=1.f/((R-r)+d);
    float sc=SC*ssc(sz)*.9f*peakNorm;
    const int N=288;
    const float co=cosf(off),so=sinf(off);
    // 6pi sweep -> start != end -> open segment.
    return curve(o,m,N,false,[&](int i,int N,float&ox,float&oy,uint8_t&rr,uint8_t&gg,uint8_t&bb){
        float t=6.f*(float)M_PI*i/(float)N;
        float x=(R-r)*cosf(t)+d*cosf((R-r)/r*t);
        float y=(R-r)*sinf(t)-d*sinf((R-r)/r*t);
        ox=sc*(x*co-y*so); oy=sc*(x*so+y*co);
        rr=0; gg=255; bb=255;
    });
}

// p56 Concentric Rings -- GalvOS v5.3: migrated to optimizer via ngon().
// Previously: raw ap() loop per ring with manual blank jumps.
// Now: each ring is a closed polygon via ngon(). Budget is shared across
// all rings because they are separate optimize() calls but the
// max_pts_per_frame budget cap applies globally per frame anyway.
static size_t p56(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,pulse=.8f+.2f*fabsf(sinf(aang(ph,sp,2)));
    for(int ring=1;ring<=5;ring++){
        float r=sc*ring/5.f*pulse;
        float h=ring/5.f;
        uint8_t cr=(uint8_t)(fabsf(sinf(h*M_PI))*255),cg=(uint8_t)(fabsf(sinf(h*M_PI+2.094f))*255),cb=(uint8_t)(fabsf(sinf(h*M_PI+4.189f))*255);
        // 32 sides gives smooth circle; optimizer handles density
        optimizer::PathVertex verts[32];
        for(int i=0;i<32;i++){
            float a=PI2*i/32.f;
            verts[i].x=cosf(a)*r; verts[i].y=sinf(a)*r;
            verts[i].r=cr; verts[i].g=cg; verts[i].b=cb;
            verts[i].lift=false;
        }
        optimizer::PathSegment seg(verts,32,true);
        n += optimizer::optimize(&seg,1,o+n,m-n,liveOptimizerConfig());
    }
    return n;
}

// p57 Nested Squares -- GalvOS v5.3: migrated to optimizer.
// Each square layer is a closed 4-vertex PathSegment.
static size_t p57(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,br=aang(ph,sp);
    for(int l=0;l<6;l++){
        float s=sc*(6-l)/6.f,rot=br+l*(M_PI/(4.f*6));
        float h=l/6.f;
        uint8_t r=(uint8_t)(fabsf(sinf(h*M_PI))*255),g=(uint8_t)(fabsf(sinf(h*M_PI+2.094f))*255),b=(uint8_t)(fabsf(sinf(h*M_PI+4.189f))*255);
        optimizer::PathVertex verts[4];
        for(int i=0;i<4;i++){
            float a=PI2*i/4.f+rot;
            verts[i].x=cosf(a)*s; verts[i].y=sinf(a)*s;
            verts[i].r=r; verts[i].g=g; verts[i].b=b;
            verts[i].lift=false;
        }
        optimizer::PathSegment seg(verts,4,true);
        n += optimizer::optimize(&seg,1,o+n,m-n,liveOptimizerConfig());
    }
    return n;
}

static size_t p58(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    // 3x point count -- 8-lobe wave modulation needs finer sampling than a
    // plain circle for the galvo to track the wiggles without a closing gap.
    float sc=SC*ssc(sz)*.9f,pulse=sc*(.5f+.5f*fabsf(sinf(aang(ph,sp,3)))),rot=aang(ph,sp,.2f);
    const int N=256;
    return curve(o,m,N,true,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float a=PI2*i/(float)N+rot,wave=1+.15f*sinf(8*a),r2=pulse*wave;
        x=cosf(a)*r2; y=sinf(a)*r2;
        r=(uint8_t)(200+55*sinf(a)); g=0; b=(uint8_t)(200+55*cosf(a));
    });
}

// ─── KOMBI 59-63 ─────────────────────────────────────────────

// p59 Starburst -- GalvOS v5.3: migrated to optimizer.
// Previously: inline ap() loops per spoke.
// Now: each spoke is a 2-vertex open PathSegment. All 24 spokes passed
// to optimize() in one call so budget management covers the full shape.
static size_t p59(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    const int nspokes=24;
    optimizer::PathVertex verts[nspokes*2];
    optimizer::PathSegment segs[nspokes];
    for(int i=0;i<nspokes;i++){
        float a=PI2*i/nspokes+off;
        float inner=sc*.3f,outer=sc*(.7f+.3f*sinf(i*.8f));
        uint8_t cr=(uint8_t)(128+127*sinf(a)),cg=(uint8_t)(128+127*cosf(a));
        // start vertex (inner)
        verts[i*2].x=cosf(a)*inner; verts[i*2].y=sinf(a)*inner;
        verts[i*2].r=cr; verts[i*2].g=cg; verts[i*2].b=255;
        verts[i*2].lift=true;   // blank-jump to inner point of each spoke
        // end vertex (outer)
        verts[i*2+1].x=cosf(a)*outer; verts[i*2+1].y=sinf(a)*outer;
        verts[i*2+1].r=cr; verts[i*2+1].g=cg; verts[i*2+1].b=255;
        verts[i*2+1].lift=false;
        segs[i]=optimizer::PathSegment(&verts[i*2],2,false);
    }
    return optimizer::optimize(segs,nspokes,o,m,liveOptimizerConfig());
}

// p60 Chaos Bouncer -- billiard-style bounce trajectory: triangle waves
// (asinf(sinf(u)) folds a ramp back and forth like a ball bouncing off
// walls) driven by two incommensurate frequencies weave a dense,
// non-repeating trajectory instead of a smooth curve. A prior version used
// integer harmonics (fx=3, fy=2) so the path would close every frame --
// but that turned it into a plain Lissajous oval, not a chaotic bounce.
// The actual bug that forced that compromise was drawing the return-to-
// start jump lit; passing closed=false here makes the optimizer blank that
// jump instead, so the frequencies are free to stay incommensurate and the
// path never has to repeat.
static inline float triWave(float u) { return (2.f/(float)M_PI) * asinf(sinf(u)); }
static size_t p60(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f;
    const float dph=aang(ph,sp,.3f);          // animated phase drift
    const float fx=4.3f, fy=2.9f, cycles=4.f;
    const int N=320;
    return curve(o,m,N,false,[&](int i,int N,float&x,float&y,uint8_t&r,uint8_t&g,uint8_t&b){
        float t=PI2*cycles*i/(float)(N-1);
        x=sc*triWave(fx*t+dph); y=sc*triWave(fy*t);
        r=(uint8_t)(128+127*sinf(t*2+dph));
        g=(uint8_t)(128+127*sinf(t*3+1));
        b=(uint8_t)(128+127*cosf(t*1.5f));
    });
}

// p61 Laser Diamond -- GalvOS v5.3: migrated to optimizer.
// Two counter-rotating squares + a circle, all via optimizer.
// Squares: closed 4-vertex PathSegments.
// Circle: 32-vertex closed PathSegment.
static size_t p61(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,rot=aang(ph,sp);
    // Outer square (counter-clockwise vertices at 45° intervals)
    {
        optimizer::PathVertex verts[4];
        const float ouV[][2]={{0,.9f},{.9f,0},{0,-.9f},{-.9f,0}};
        for(int i=0;i<4;i++){
            float rx=ouV[i][0]*sc*cosf(rot)-ouV[i][1]*sc*sinf(rot);
            float ry=ouV[i][0]*sc*sinf(rot)+ouV[i][1]*sc*cosf(rot);
            verts[i].x=rx; verts[i].y=ry;
            verts[i].r=0; verts[i].g=255; verts[i].b=255;
            verts[i].lift=false;
        }
        optimizer::PathSegment seg(verts,4,true);
        n += optimizer::optimize(&seg,1,o+n,m-n,liveOptimizerConfig());
    }
    // Inner square (counter-rotating)
    {
        optimizer::PathVertex verts[4];
        const float inV[][2]={{0,.55f},{.55f,0},{0,-.55f},{-.55f,0}};
        for(int i=0;i<4;i++){
            float rx=inV[i][0]*sc*cosf(-rot)-inV[i][1]*sc*sinf(-rot);
            float ry=inV[i][0]*sc*sinf(-rot)+inV[i][1]*sc*cosf(-rot);
            verts[i].x=rx; verts[i].y=ry;
            verts[i].r=255; verts[i].g=0; verts[i].b=255;
            verts[i].lift=false;
        }
        optimizer::PathSegment seg(verts,4,true);
        n += optimizer::optimize(&seg,1,o+n,m-n,liveOptimizerConfig());
    }
    // Circle via ngon
    n += ngon(o+n,m-n,32,.72f*sc,rot,0,255,255);
    return n;
}

// p63 Disco Ball -- GalvOS v5.3: migrated.
// Outline circle + latitude rings + equator: all via ngon().
// Longitude spokes: migrated to optimizer PathSegments.
// p63 Confetti Burst (Combo) -- previously a duplicate of the Disco Ball
// code, so this slot never showed confetti. Now: pieces launch from centre
// and drift outward on deterministic per-piece trajectories. Each piece is a
// small rotating quad, blank-jumped to. `t` (0..1) is the burst progress
// derived from a looping phase so the burst repeats.
static size_t p63(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f;
    float burst=fmodf(aang(ph,sp,.5f)/PI2,1.f);        // 0..1 loop
    // 18 confetti pieces as 18 closed quads in ONE optimize() call. The old
    // code emitted a SINGLE blank point as the travel between pieces -- i.e.
    // it commanded the galvo across up to the full field in one tick, which it
    // physically cannot do; the mirror smears through the move and the laser
    // is already back on when it arrives. optimize() replaces that with a
    // distance-proportional, smoothstep-eased blank jump (Pillar 2) and plans
    // all 18 against the shared frame budget.
    const int NP=18, NV=4;
    optimizer::PathVertex v[NP*NV];
    optimizer::PathSegment segs[NP];
    for(int p=0;p<NP;p++){
        float ang=PI2*p/NP+(p*0.37f);                   // fixed launch direction
        float r=burst*(0.35f+0.65f*fmodf(p*0.191f,1.f))*sc;
        float cx=cosf(ang)*r,cy=sinf(ang)*r;
        float spin=ang*3.f+burst*PI2*2.f,pr=0.05f*sc;
        uint8_t cr=(uint8_t)(128+127*sinf(ang));
        uint8_t cg=(uint8_t)(128+127*sinf(ang+2.1f));
        uint8_t cb=(uint8_t)(128+127*sinf(ang+4.2f));
        optimizer::PathVertex* vp=v+p*NV;
        for(int i=0;i<NV;i++){
            float a=spin+PI2*i/(float)NV;
            vp[i]=optimizer::PathVertex(cx+cosf(a)*pr,cy+sinf(a)*pr*.6f,cr,cg,cb,i==0);
        }
        segs[p]=optimizer::PathSegment(vp,NV,true);
    }
    return optimizer::optimize(segs,NP,o,m,liveOptimizerConfig());
}

// p101 Disco Ball 2 (6th Combo preset)
// p101 Disco Ball -- wire sphere. Previous latitudes held y constant (flat
// horizontal scribbles, not rings) and meridians were full-height vertical
// chords, so nothing read as a ball. Now latitudes are foreshortened
// ellipses at height y=sin(lat) with x-radius cos(lat), and meridians are
// vertical ellipse arcs whose horizontal extent shrinks with |sin(lon)|.
static size_t p101(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,rot=aang(ph,sp);
    const float yFore=.5f;                              // vertical foreshortening
    // Silhouette
    n += ngon(o+n,m-n,48,sc,0,255,255,255);
    // Latitude ellipses at ±30°, ±60° and equator.
    for(int row=-2;row<=2;row++){
        float lat=row*30.f*(float)M_PI/180.f;
        float ry=sinf(lat)*sc,rx=cosf(lat)*sc;
        uint8_t cw=(row==0)?255:200;
        optimizer::PathVertex verts[28];
        for(int i=0;i<28;i++){
            float a=PI2*i/28.f;
            verts[i].x=cosf(a)*rx; verts[i].y=ry+sinf(a)*rx*.12f; // thin ellipse band
            verts[i].r=(uint8_t)(cw*(0.5f+0.5f*sinf(a+rot)));
            verts[i].g=(uint8_t)(cw*(0.5f+0.5f*sinf(a+rot+2.094f)));
            verts[i].b=(uint8_t)(cw*(0.5f+0.5f*sinf(a+rot+4.189f)));
            verts[i].lift=false;
        }
        optimizer::PathSegment seg(verts,28,true);
        n += optimizer::optimize(&seg,1,o+n,m-n,liveOptimizerConfig());
    }
    // Meridians: vertical arcs, x = sin(lon)*cos(v)*sc, y = sin(v)*yFore*sc.
    const int NM=6;
    for(int mI=0;mI<NM;mI++){
        float lon=(float)M_PI*mI/NM+rot;
        optimizer::PathVertex verts[20];
        for(int i=0;i<20;i++){
            float v=-(float)(M_PI/2)+(float)M_PI*i/19.f;
            verts[i].x=sinf(lon)*cosf(v)*sc; verts[i].y=sinf(v)*sc*yFore*2.f;
            verts[i].r=(uint8_t)(128+127*cosf(lon*2+rot));
            verts[i].g=(uint8_t)(128+127*sinf(lon*3-rot));
            verts[i].b=255; verts[i].lift=(i==0);
        }
        optimizer::PathSegment seg(verts,20,false);
        n += optimizer::optimize(&seg,1,o+n,m-n,liveOptimizerConfig());
    }
    return n;
}

static size_t p86(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Hibiskus
    float sc=SC*ssc(sz)*.9f,rot=aang(ph,sp,.2f);
    // 5 petals + 1 centre ring = 6 segments in ONE optimize() call, so the
    // petal-to-petal blank jumps are planned against the shared frame budget
    // (MultiObject profile) instead of being emitted per-petal.
    // Each petal is one closed loop: out along +spread, back along -spread.
    const int NP=5, NT=31, PETAL=NT*2, NC=12;
    optimizer::PathVertex v[NP*PETAL+NC];
    optimizer::PathSegment segs[NP+1];
    for(int p=0;p<NP;p++){
        float base=PI2*p/(float)NP+rot;
        optimizer::PathVertex* vp=v+p*PETAL;
        for(int i=0;i<NT;i++){
            float t=i/(float)(NT-1),spread=sinf(t*(float)M_PI),a=base+spread*.4f,r=L(.15f,.65f,t);
            vp[i]=optimizer::PathVertex(cosf(a)*r*sc,sinf(a)*r*sc,
                    255,(uint8_t)(50+t*100),(uint8_t)(100-t*100),i==0);
        }
        for(int i=0;i<NT;i++){
            float t=(NT-1-i)/(float)(NT-1),spread=sinf(t*(float)M_PI),a=base-spread*.4f,r=L(.15f,.65f,t);
            vp[NT+i]=optimizer::PathVertex(cosf(a)*r*sc,sinf(a)*r*sc,
                    255,(uint8_t)(50+t*100),0,false);
        }
        segs[p]=optimizer::PathSegment(vp,PETAL,true);
    }
    optimizer::PathVertex* vc=v+NP*PETAL;
    for(int i=0;i<NC;i++){
        float a=PI2*i/(float)NC+rot*2;
        vc[i]=optimizer::PathVertex(cosf(a)*.12f*sc,sinf(a)*.12f*sc,255,255,0,i==0);
    }
    segs[NP]=optimizer::PathSegment(vc,NC,true);
    return optimizer::optimize(segs,NP+1,o,m,liveOptimizerConfig());
}
static size_t p88(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Starburst (Kombi)
    size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    for(int i=0;i<24;i++){float a=PI2*i/24.f+off,inner=sc*.3f,outer=sc*(.7f+.3f*sinf(i*.8f));line(o,n,m,cosf(a)*inner,sinf(a)*inner,cosf(a)*outer,sinf(a)*outer,(uint8_t)(128+127*sinf(a)),(uint8_t)(128+127*cosf(a)),255,8);}
    return n;
}
// ─── SZENEN 90 ─────────────────────────────────────────────
// p90 Starfield: falling star field, top→bottom with wrap.
// v5.6: blank travel via optimizer::emitBlankTo() -- distance-proportional,
// smoothstep-eased. Star emission order uses greedy nearest-neighbor
// (starting from last galvo position) to minimize total jump distance.
// Each star: optimizer blank jump + manual dwell ticks (lit, same position).
//
// sp  = fall speed (0=slow, 255=fast)
// sz  = star count (0=~20 stars, 255=~100 stars)
static size_t p90(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;
    int nStars = 1 + (int)(sz / 255.f * 99.f);    // 1..100 stars
    const float baseSpd = 1.0f + (sp / 255.f) * 19.0f;

    // cfg first -- needed by both dwell calc and star-count cap.
    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();

    // Dwell: lit ticks at destination so eye integrates a clean dot.
    // ~150us integration window, derived from actual kpps.
    uint16_t kpps = gProjection.galvo_kpps; if (kpps < 12) kpps = 12; if (kpps > 60) kpps = 60;
    const float tick_us = 1000000.f / ((float)kpps * 1000.f);
    int dwell = (int)ceilf(150.f / tick_us);
    if (dwell < 3) dwell = 3;

    // Star-count cap: use blank_samples (worst-case jump) + dwell as
    // per-star budget estimate. Actual blank ticks will be less for
    // short jumps (optimizer scales by distance), so this is conservative.
    int per_star_est = (int)cfg.blank_samples + dwell;
    const size_t budget = (m < (size_t)cfg.max_pts_per_frame)
                              ? m : (size_t)cfg.max_pts_per_frame;
    if ((size_t)(nStars * per_star_est) > budget) {
        nStars = (int)(budget / per_star_est);
        if (nStars < 1) nStars = 1;
    }

    auto fr = [](int seed) -> float {
        float x = sinf((float)seed * 127.1f + 1.f) * 43758.5453f;
        return x - floorf(x);
    };

    // Collect currently-visible stars.
    struct Star { float x, y; uint8_t r, g, b; bool used; };
    static Star stars[128];
    int ns = 0;
    for (int i = 0; i < nStars && ns < 128; i++) {
        const float xPos  = (fr(i * 7)  * 2.f - 1.f) * SC * 0.95f;
        const float iSpd  = baseSpd * (0.3f + fr(i * 3) * 1.4f);
        const float off   = fr(i * 5) * 2.2f;
        const float yNorm = 1.1f - fmodf(ph * iSpd * 0.0004f + off, 2.2f);
        if (yNorm < -1.1f || yNorm > 1.1f) continue;
        const int period  = (int)(ph * iSpd * 0.0004f);
        // Brightness: 20..255 (wider spread than before so dim stars look
        // clearly fainter than bright ones at any master_dimmer level).
        // Star color: bright=blue-white, dim=warm white/orange -- classic
        // stellar color index effect.
        const uint8_t bright = (uint8_t)(20 + (int)(fr(i * 2 + period) * 235.f));
        const uint8_t r_val  = bright;
        const uint8_t g_val  = (uint8_t)(bright * (0.85f + fr(i * 11) * 0.15f));
        const uint8_t b_val  = (bright > 160)
                                   ? (uint8_t)fminf(255.f, bright + 40.f)   // bright: blue tint
                                   : (uint8_t)(bright * 0.6f);              // dim: warm/orange
        stars[ns].x = xPos;
        stars[ns].y = yNorm * SC * 0.95f;
        stars[ns].r = r_val; stars[ns].g = g_val; stars[ns].b = b_val;
        stars[ns].used = false;
        ns++;
    }

    // Greedy nearest-neighbor: start from last known galvo position,
    // always pick the closest unvisited star next. O(n^2) -- fine for n<=100.
    static Star sorted[128];
    float cur_x = (n > 0) ? o[n-1].x : 0.f;
    float cur_y = (n > 0) ? o[n-1].y : 0.f;
    for (int s = 0; s < ns; s++) {
        int best = -1; float best_d2 = 1e18f;
        for (int k = 0; k < ns; k++) {
            if (stars[k].used) continue;
            float dx = stars[k].x - cur_x, dy = stars[k].y - cur_y;
            float d2 = dx*dx + dy*dy;
            if (d2 < best_d2) { best_d2 = d2; best = k; }
        }
        if (best < 0) break;
        stars[best].used = true;
        sorted[s] = stars[best];
        cur_x = sorted[s].x;
        cur_y = sorted[s].y;
    }

    // Emit: distanced-scaled blank jump (optimizer) + dwell (lit copies).
    // dwell*2: survives galvo_out.cpp's LASER_ON_HOLD_TICKS hold-off after
    // the blank jump -- plain dwell can be entirely swallowed by it at low
    // galvo_kpps, leaving the star dark (see p_fireworks spark fix).
    for (int i = 0; i < ns && n < m; i++) {
        const Star& s = sorted[i];
        optimizer::emitBlankTo(o, n, m, s.x, s.y, cfg);
        for (int d = 0; d < dwell * 2 && n < m; d++)
            ap(o, n, m, s.x, s.y, s.r, s.g, s.b, 0);
    }
    return n;
}

// ─── ANIMIERTE FAHRZEUGE 91-98 ───────────────────────────────
// All use line() and circ_draw() which are now optimizer-backed.
// No per-vehicle changes needed.
static size_t p99(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=SC*ssc(sz)*0.7f, ox=scrollX(ph,sp);
    line(o,n,m, ox-sc2,0,            ox+sc2,0,            0,0,255, 20);
    line(o,n,m, ox-sc2*.1f,0,        ox-sc2*.4f, sc2*.45f, 0,0,255, 14);
    line(o,n,m, ox-sc2*.4f,sc2*.45f, ox-sc2*.1f,0,         0,0,255,  6);
    line(o,n,m, ox-sc2*.1f,0,        ox-sc2*.4f,-sc2*.45f, 0,0,255, 14);
    line(o,n,m, ox-sc2*.4f,-sc2*.45f,ox-sc2*.1f,0,         0,0,255,  6);
    line(o,n,m, ox-sc2*.75f,0,       ox-sc2,     sc2*.3f,  0,0,255,  8);
    line(o,n,m, ox-sc2*.75f,0,       ox-sc2,    -sc2*.3f,  0,0,255,  8);
    line(o,n,m, ox+sc2,0,            ox+sc2*.85f, sc2*.08f, 0,0,255,  4);
    line(o,n,m, ox+sc2,0,            ox+sc2*.85f,-sc2*.08f, 0,0,255,  4);
    line(o,n,m, ox-sc2*.15f, sc2*.2f, ox-sc2*.45f, sc2*.2f,  0,255,255,  8);
    line(o,n,m, ox-sc2*.15f,-sc2*.2f, ox-sc2*.45f,-sc2*.2f,  0,255,255,  8);
    return n;
}

// ─── COUNTDOWN TIMER (p100) ─────────────────────────────────────────
// seg7_digit() -- GalvOS v5.3: migrated to optimizer.
// Each lit segment is a 2-vertex open PathSegment.
// blank=1 moves between segments are replaced by lift=true on the
// start vertex of each segment -- optimizer::emitBlankJump() handles
// the inter-segment travel. All segments of one digit are passed to
// optimize() in one call for correct budget management.
static void seg7_digit(LaserPoint*o, size_t&n, size_t m,
                        float xc, float yc, float w, float h,
                        uint8_t digit, uint8_t r, uint8_t g, uint8_t b) {
    if (digit > 9) return;
    const bool seg[10][7] = {
        {1,1,1,1,1,1,0}, // 0
        {0,1,1,0,0,0,0}, // 1
        {1,1,0,1,1,0,1}, // 2
        {1,1,1,1,0,0,1}, // 3
        {0,1,1,0,0,1,1}, // 4
        {1,0,1,1,0,1,1}, // 5
        {1,0,1,1,1,1,1}, // 6
        {1,1,1,0,0,0,0}, // 7
        {1,1,1,1,1,1,1}, // 8
        {1,1,1,1,0,1,1}, // 9
    };
    const float hw = w*0.5f, hh = h*0.5f, mh = h*0.02f;
    struct Seg { float x0,y0,x1,y1; } segs[7] = {
        {xc-hw+mh, yc+hh,  xc+hw-mh, yc+hh      },
        {xc+hw,    yc+mh,  xc+hw,    yc+hh-mh    },
        {xc+hw,    yc-hh+mh, xc+hw,  yc-mh        },
        {xc-hw+mh, yc-hh,  xc+hw-mh, yc-hh        },
        {xc-hw,    yc-hh+mh, xc-hw,  yc-mh         },
        {xc-hw,    yc+mh,  xc-hw,    yc+hh-mh     },
        {xc-hw+mh, yc,     xc+hw-mh, yc            },
    };
    // Count active segments to size the arrays
    int nsegs = 0;
    for (int i = 0; i < 7; i++) if (seg[digit][i]) nsegs++;
    if (nsegs == 0) return;

    optimizer::PathVertex verts[14]; // max 7 segments * 2 vertices
    optimizer::PathSegment psegs[7];
    int sidx = 0;
    for (int i = 0; i < 7; i++) {
        if (!seg[digit][i]) continue;
        verts[sidx*2].x   = segs[i].x0; verts[sidx*2].y   = segs[i].y0;
        verts[sidx*2].r   = r;           verts[sidx*2].g   = g; verts[sidx*2].b = b;
        verts[sidx*2].lift = true;  // blank-jump between segments
        verts[sidx*2+1].x = segs[i].x1; verts[sidx*2+1].y = segs[i].y1;
        verts[sidx*2+1].r = r;           verts[sidx*2+1].g = g; verts[sidx*2+1].b = b;
        verts[sidx*2+1].lift = false;
        psegs[sidx] = optimizer::PathSegment(&verts[sidx*2], 2, false);
        sidx++;
    }
    n += optimizer::optimize(psegs, (size_t)nsegs, o+n, m-n, liveOptimizerConfig());
}

static size_t p100(LaserPoint*o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    size_t n = 0;
    countdown_timer::tick();

    uint32_t rem  = countdown_timer::remaining();
    bool     expr = countdown_timer::expired();

    uint8_t cr, cg, cb;
    if (expr) {
        bool blink = (ph % 60) < 30;
        cr = blink ? 255 : 0; cg = 0; cb = 0;
    } else if (rem == 0) {
        cr = 255; cg = 255; cb = 255;
    } else if (rem <= 10) {
        cr = 255; cg = 0;   cb = 0;
    } else if (rem <= 30) {
        cr = 255; cg = 255; cb = 0;
    } else {
        cr = 0;   cg = 255; cb = 0;
    }

    uint32_t hh = rem / 3600;
    uint32_t mm = (rem % 3600) / 60;
    uint32_t ss = rem % 60;

    float sc2 = ssc(sz) * 0.85f;
    float dw  = SC * 0.18f * sc2;
    float dh  = SC * 0.38f * sc2;
    float gap = SC * 0.06f * sc2;
    float cdot = SC * 0.04f * sc2;

    bool show_h = (hh > 0);
    float total_w = show_h ? (dw*2+gap)*3 + gap*2 : (dw*2+gap)*2 + gap;
    float ox = -total_w * 0.5f + dw + gap * 0.5f;

    // Colon pips: each is a short horizontal stroke -> line() (optimizer
    // backed), so the jump between pips is eased/distance-scaled instead of
    // the old single blank point.
    auto colon = [&](float cx) {
        line(o,n,m, cx-cdot*0.5f,  dh*0.28f, cx+cdot*0.5f,  dh*0.28f, cr,cg,cb);
        line(o,n,m, cx-cdot*0.5f, -dh*0.28f, cx+cdot*0.5f, -dh*0.28f, cr,cg,cb);
    };

    if (show_h) {
        seg7_digit(o,n,m, ox,       0, dw, dh, hh/10%10, cr,cg,cb);
        seg7_digit(o,n,m, ox+dw+gap,0, dw, dh, hh%10,    cr,cg,cb);
        colon(ox + dw*2 + gap*1.5f);
        ox += (dw*2 + gap) + gap*1.5f + gap*0.5f;
    }
    seg7_digit(o,n,m, ox,       0, dw, dh, mm/10, cr,cg,cb);
    seg7_digit(o,n,m, ox+dw+gap,0, dw, dh, mm%10, cr,cg,cb);
    colon(ox + dw*2 + gap*1.5f);
    ox += (dw*2 + gap) + gap*1.5f + gap*0.5f;
    seg7_digit(o,n,m, ox,       0, dw, dh, ss/10, cr,cg,cb);
    seg7_digit(o,n,m, ox+dw+gap,0, dw, dh, ss%10, cr,cg,cb);

    if (countdown_timer::running()) {
        bool blink = (ph % 30) < 15;
        if (blink) {
            float dot_y = -dh * 0.6f;
            line(o,n,m, -SC*0.04f*sc2, dot_y, SC*0.04f*sc2, dot_y, cr,cg,cb);
        }
    }
    return n;
}

// ─── NEUE PRESETS 102-105 ───────────────────────────────────────
// p103 Pentagram -- true 5/2 self-intersecting star polygon, single stroke.
// Vertices placed at 144° (4*PI/5) spacing instead of the plain pentagon's
// 72° -- connecting them in array order via a closed PathSegment produces
// the classic self-crossing star silhouette. Unlike star() (p06-p09,
// alternating outer/inner radius = convex points), this has real internal
// crossings.
static size_t p103(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    optimizer::PathVertex verts[5];
    for(int k=0;k<5;k++){
        float a=off+k*4.f*(float)M_PI/5.f-(float)(M_PI/2);
        verts[k].x=cosf(a)*sc; verts[k].y=sinf(a)*sc;
        verts[k].r=255; verts[k].g=0; verts[k].b=255;
        verts[k].lift=false;
    }
    optimizer::PathSegment seg(verts,5,/*closed=*/true);
    return optimizer::optimize(&seg,1,o,m,liveOptimizerConfig());
}

// p104 DNA Double Helix -- two counter-phase sine strands (backbone) plus
// straight rungs (base pairs) at fixed intervals via line().
// v1.1: strands migrated to PathSegment+optimize() for proper S-curve
// blanking and velocity clamping; stray connecting lines and hot end-
// dwell points eliminated.
static size_t p104(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;
    float sc=SC*ssc(sz)*.9f, amp=sc*.32f, off=aang(ph,sp);
    const int NS=70; const float TURNS=3.f;
    optimizer::OptimizerConfig cfg=liveOptimizerConfig();

    // Strand 1: cyan
    {
        optimizer::PathVertex verts[NS+1];
        for(int i=0;i<=NS;i++){
            float t=(float)i/NS, x=L(-sc,sc,t), a=t*PI2*TURNS+off;
            verts[i].x=x; verts[i].y=amp*sinf(a);
            verts[i].r=0; verts[i].g=255; verts[i].b=255;
            verts[i].lift=(i==0);
        }
        optimizer::PathSegment seg(verts,(size_t)(NS+1),/*closed=*/false);
        n+=optimizer::optimize(&seg,1,o+n,m-n,cfg);
    }
    // Strand 2: magenta (phase-shifted by π)
    {
        optimizer::PathVertex verts[NS+1];
        for(int i=0;i<=NS;i++){
            float t=(float)i/NS, x=L(-sc,sc,t), a=t*PI2*TURNS+off+(float)M_PI;
            verts[i].x=x; verts[i].y=amp*sinf(a);
            verts[i].r=255; verts[i].g=0; verts[i].b=255;
            verts[i].lift=(i==0);
        }
        optimizer::PathSegment seg(verts,(size_t)(NS+1),/*closed=*/false);
        n+=optimizer::optimize(&seg,1,o+n,m-n,cfg);
    }
    // Rungs (base pairs): line() uses lift=true internally
    const int RUNGS=9;
    for(int rI=0;rI<RUNGS;rI++){
        float t=(float)rI/(RUNGS-1), x=L(-sc,sc,t), a=t*PI2*TURNS+off;
        line(o,n,m,x,amp*sinf(a),x,amp*sinf(a+(float)M_PI),255,255,255);
    }
    return n;
}

// p105 Yin-Yang -- outer circle + S-divider (two opposing half-circles) +
// two accent dots. Wireframe laser output can't fill black/white halves,
// so the eye-dots use contrasting colors instead of the traditional fill.
// p105 Yin-Yang -- outer circle + continuous S-divider + two accent dots.
// Fix: the S half-circles and the eye dots are offset from the origin, so
// they must be rotated as a rigid body together with the outer circle.
// Previously only the arc *angle* got `off` while the ±R/2 centre offsets
// stayed axis-aligned, so under animation the S and dots tore away from the
// disc. Every emitted point now passes through one rotation by `off`. Each
// sub-shape is blank-jumped so no stray connecting lines appear.
static size_t p105(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    float R=SC*ssc(sz)*.85f, off=aang(ph,sp);
    const float c=cosf(off),s=sinf(off);
    // 4 segments in one optimize() call: outer circle (closed), the S-divider
    // as ONE open chain (the two half-circles meet at the origin, so they are
    // a single continuous stroke -- emitting them separately forced a blank
    // jump through a point the beam was already sitting on), and the 2 eyes.
    const int NC=64, NS=32, ND=20;
    const int SN=2*NS+1;                  // S-chain: both halves, shared origin
    optimizer::PathVertex v[NC+SN+2*ND];
    optimizer::PathSegment segs[4];
    auto rot=[&](float x,float y)->optimizer::PathVertex{
        optimizer::PathVertex pv; pv.x=x*c-y*s; pv.y=x*s+y*c; return pv; };
    optimizer::PathVertex* vo=v;
    for(int i=0;i<NC;i++){float a=PI2*i/(float)NC;
        vo[i]=rot(R*cosf(a),R*sinf(a));
        vo[i].r=255;vo[i].g=255;vo[i].b=255;vo[i].lift=(i==0);}
    segs[0]=optimizer::PathSegment(vo,NC,true);
    optimizer::PathVertex* vs=v+NC; int k=0;
    for(int i=0;i<=NS;i++){float a=-(float)(M_PI/2)+(float)M_PI*i/NS;
        vs[k]=rot((R*.5f)*cosf(a),(R*.5f)+(R*.5f)*sinf(a));
        vs[k].r=255;vs[k].g=255;vs[k].b=255;vs[k].lift=(k==0);k++;}
    for(int i=1;i<=NS;i++){float a=(float)(M_PI/2)+(float)M_PI*i/NS;
        vs[k]=rot((R*.5f)*cosf(a),-(R*.5f)+(R*.5f)*sinf(a));
        vs[k].r=255;vs[k].g=255;vs[k].b=255;vs[k].lift=false;k++;}
    segs[1]=optimizer::PathSegment(vs,(size_t)k,false);
    float rd=R*.14f;
    optimizer::PathVertex* ve1=v+NC+SN;
    for(int i=0;i<ND;i++){float a=PI2*i/(float)ND;
        ve1[i]=rot(rd*cosf(a),(R*.5f)+rd*sinf(a));
        ve1[i].r=255;ve1[i].g=0;ve1[i].b=0;ve1[i].lift=(i==0);}
    segs[2]=optimizer::PathSegment(ve1,ND,true);
    optimizer::PathVertex* ve2=v+NC+SN+ND;
    for(int i=0;i<ND;i++){float a=PI2*i/(float)ND;
        ve2[i]=rot(rd*cosf(a),-(R*.5f)+rd*sinf(a));
        ve2[i].r=0;ve2[i].g=255;ve2[i].b=255;ve2[i].lift=(i==0);}
    segs[3]=optimizer::PathSegment(ve2,ND,true);
    return optimizer::optimize(segs,4,o,m,liveOptimizerConfig());
}

// ─── SZENEN 106 ────────────────────────────────────────────
// p106 Random Points: flickering point cloud. Up to RANDOM_PTS_MAX_COUNT
// independent "slots" each run their own real-time appear -> hold ->
// disappear -> idle cycle, desynced per slot via a hashed phase offset --
// so the number of points lit at any given instant varies (anywhere from
// 0 up to the slot count), instead of every point being on all the time.
// Position and color are re-rolled every time a slot restarts its cycle
// (hashed by slot index + cycle count), so it isn't the same dots
// re-blinking forever. Runs on millis() rather than `ph` (unlike other
// presets) so Duration/Speed read as real time regardless of frame rate --
// same convention as pattern_engine.cpp's Points-Only mode fade timing.
//
// sp  = fade speed: how fast a point appears/disappears (0=slow, 255=fast)
// sz  = max concurrent points, "Amount" (1..RANDOM_PTS_MAX_COUNT)
// gLivePreset.random_pts_hold_ms = hold time at full brightness, "Duration"
static size_t p106(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    (void)ph;   // real-time (millis) driven -- see header comment above
    size_t n=0;
    int nSlots = 1 + (int)(sz / 255.f * (RANDOM_PTS_MAX_COUNT - 1));  // 1..RANDOM_PTS_MAX_COUNT

    const uint16_t fadeMs = (uint16_t)(750.f - (sp / 255.f) * 650.f);  // 750..100ms
    uint16_t holdMs = gLivePreset.random_pts_hold_ms;
    if (holdMs < 50) holdMs = 50;

    const uint32_t onMs     = (uint32_t)fadeMs * 2 + holdMs;  // appear+hold+disappear
    const uint32_t periodMs = onMs * 2;                       // + idle gap -- desyncs slots
    const uint32_t nowMs    = millis();

    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    uint16_t kpps = gProjection.galvo_kpps; if (kpps < 12) kpps = 12; if (kpps > 60) kpps = 60;
    const float tick_us = 1000000.f / ((float)kpps * 1000.f);
    int dwell = (int)ceilf(150.f / tick_us);
    if (dwell < 3) dwell = 3;

    auto fr = [](uint32_t seed) -> float {
        float x = sinf((float)seed * 127.1f + 1.f) * 43758.5453f;
        return x - floorf(x);
    };

    for (int k = 0; k < nSlots && n < m; k++) {
        const uint32_t slotOff = (uint32_t)(fr((uint32_t)k * 97u + 11u) * periodMs);
        const uint32_t t       = (nowMs + slotOff) % periodMs;
        if (t >= onMs) continue;   // this slot is idle right now -- not drawn

        const uint32_t cycleIdx = (nowMs + slotOff) / periodMs;  // bumps on each restart
        const uint32_t seed     = (uint32_t)k * 10007u + cycleIdx * 997u;

        const float px = (fr(seed * 3u + 1u) * 2.f - 1.f) * SC * 0.9f;
        const float py = (fr(seed * 7u + 2u) * 2.f - 1.f) * SC * 0.9f;
        const float hue = fr(seed * 5u + 3u) * PI2;
        const uint8_t r = (uint8_t)(128 + 127 * sinf(hue));
        const uint8_t g = (uint8_t)(128 + 127 * sinf(hue + 2.094f));
        const uint8_t b = (uint8_t)(128 + 127 * sinf(hue + 4.189f));

        float v;
        if (t < fadeMs)               v = (float)t / fadeMs;                          // fade in
        else if (t < fadeMs + holdMs) v = 1.f;                                        // hold
        else                          v = 1.f - (float)(t - fadeMs - holdMs) / fadeMs; // fade out

        optimizer::emitBlankTo(o, n, m, px, py, cfg);
        const uint8_t vr = (uint8_t)(r * v), vg = (uint8_t)(g * v), vb = (uint8_t)(b * v);
        // dwell*2: survives the post-blank LASER_ON_HOLD_TICKS hold-off (see
        // p_fireworks spark fix) -- plain dwell can be entirely swallowed by
        // it at low galvo_kpps, leaving the dot dark.
        for (int d = 0; d < dwell * 2 && n < m; d++) ap(o, n, m, px, py, vr, vg, vb, 0);
    }
    return n;
}

// p107 Three Circles -- three same-size circles arranged side by side.
// Static layout (no self-rotation) since "side by side" is the defining
// shape; each circle is a separate 32-vertex closed PathSegment, one
// optimize() call per circle -- same style as p56/p57. RGB colouring by
// position (left/mid/right).
static size_t p107(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    (void)ph; (void)sp;  // static layout, no animation
    size_t n=0;const float sc=SC*ssc(sz)*.9f;
    const float cx=0.62f*sc, rad=0.28f*sc;
    const float cxs[3]={-cx,0.f,cx};
    const uint8_t cols[3][3]={{255,0,0},{0,255,0},{0,0,255}};
    for(int c=0;c<3;c++){
        optimizer::PathVertex verts[32];
        for(int i=0;i<32;i++){
            float a=PI2*i/32.f;
            verts[i].x=cxs[c]+cosf(a)*rad; verts[i].y=sinf(a)*rad;
            verts[i].r=cols[c][0]; verts[i].g=cols[c][1]; verts[i].b=cols[c][2];
            verts[i].lift=false;
        }
        optimizer::PathSegment seg(verts,32,true);
        n += optimizer::optimize(&seg,1,o+n,m-n,liveOptimizerConfig());
    }
    return n;
}

// p108 Point Spread -- 1..12 points evenly spread on a circle (N=1: single
// centre point). sz (Size control) selects point COUNT here rather than
// scale -- same convention as p106 Random Points' "Amount" control, since
// presets have no dedicated per-preset parameter slot. Positions/colours
// are fixed per N (no animation); each dot gets a kpps-scaled dwell so it
// actually registers at 30kpps -- same fix as Phyllotaxis' "bad output"
// (invisible dots from a single too-brief sample).
static size_t p108(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    (void)ph; (void)sp;  // static layout, no animation
    size_t n=0;const float sc=SC*.85f;
    int N=1+(sz*11)/255; if(N<1)N=1; if(N>12)N=12;

    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    uint16_t kpps=gProjection.galvo_kpps; if(kpps<12)kpps=12; if(kpps>60)kpps=60;
    const float tick_us=1000000.f/((float)kpps*1000.f);
    int dwell=(int)ceilf(150.f/tick_us); if(dwell<3)dwell=3;

    for(int i=0;i<N && n<m;i++){
        float x,y;
        if(N==1){x=0.f;y=0.f;}
        else{float a=PI2*i/N-(float)M_PI/2.f;x=cosf(a)*sc;y=sinf(a)*sc;}
        float h=(float)i/(float)(N>1?N:1);
        uint8_t r=(uint8_t)(fabsf(sinf(h*M_PI))*255),g=(uint8_t)(fabsf(sinf(h*M_PI+2.094f))*255),b=(uint8_t)(fabsf(sinf(h*M_PI+4.189f))*255);
        optimizer::emitBlankTo(o,n,m,x,y,cfg);
        for(int d=0;d<dwell && n<m;d++) ap(o,n,m,x,y,r,g,b,0);
    }
    return n;
}

static size_t p_solar(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    const float sc   = SC * ssc(sz) * 0.9f;
    const float ap_  = aang(ph, sp);              // planet angle (sp-controlled)
    const float am   = aang(ph, sp, 4.0f);        // moon angle (4x faster)
    const float rSun = sc * 0.16f;
    const float rPla = sc * 0.08f;
    const float rMoo = sc * 0.04f;
    const float oPla = sc * 0.48f;
    const float oMoo = sc * 0.20f;
    const float px   = cosf(ap_) * oPla;
    const float py   = sinf(ap_) * oPla;
    const float mx_  = px + cosf(am) * oMoo;
    const float my_  = py + sinf(am) * oMoo;

    // Build three closed circle PathSegments so optimizer::emitBlankJump()
    // handles inter-object jumps with proper S-curve easing (Pillar 2).
    // lift=true on vertex[0] of each segment triggers the blank jump.
    static optimizer::PathVertex vSun[48], vPla[36], vMoo[24];

    for(int i=0;i<48;i++){
        float a=PI2*i/48.f;
        vSun[i]=optimizer::PathVertex(cosf(a)*rSun,sinf(a)*rSun,255,255,0,i==0);
    }
    for(int i=0;i<36;i++){
        float a=PI2*i/36.f;
        vPla[i]=optimizer::PathVertex(px+cosf(a)*rPla,py+sinf(a)*rPla,0,0,255,i==0);
    }
    for(int i=0;i<24;i++){
        float a=PI2*i/24.f;
        vMoo[i]=optimizer::PathVertex(mx_+cosf(a)*rMoo,my_+sinf(a)*rMoo,255,0,255,i==0);
    }

    optimizer::PathSegment segs[3] = {
        optimizer::PathSegment(vSun, 48, /*closed=*/true),
        optimizer::PathSegment(vPla, 36, /*closed=*/true),
        optimizer::PathSegment(vMoo, 24, /*closed=*/true),
    };
    return optimizer::optimize(segs, 3, o, m, liveOptimizerConfig());
}


// p_bouncing -- Bouncing Points. Up to 8 independent dots bounce inside the
// scan area like a ping-pong game. Each ball has its own direction vector and
// hue (evenly distributed around the colour wheel); when a ball hits a wall it
// reflects on the relevant axis. Trail: up to 12 ghost positions stored per
// ball, drawn with linearly decaying brightness (Fade-out style).
//
// sp  = ball speed (0=very slow, 255=fast)
// sz  = ball count 1..8
// gLivePreset.trail          = trail length 0..12 (0 = no trail)
// gLivePreset.bp_endless     = true -> run forever, false -> time-limited
// gLivePreset.bp_duration_sec = stop after this many seconds (1..90)
#define BP_MAX_BALLS 8
#define BP_TRAIL_MAX 12
static size_t p_bouncing(LaserPoint* o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    (void)ph;  // millis()-driven

    // ── persistent state ──────────────────────────────────────────────────
    struct Ball {
        float x, y;       // current position [-SC..SC]
        float vx, vy;     // velocity direction (unit vector)
        float trailX[BP_TRAIL_MAX];
        float trailY[BP_TRAIL_MAX];
        bool  init;
    };
    static Ball   balls[BP_MAX_BALLS];
    static uint32_t lastMs   = 0;
    static uint32_t startMs  = 0;
    static bool     seeded   = false;

    const uint32_t nowMs = millis();
    if (!seeded) {
        auto fr0 = [](uint32_t seed) -> float {
            float x = sinf((float)seed * 127.1f + 1.f) * 43758.5453f;
            return x - floorf(x);
        };
        for (int k = 0; k < BP_MAX_BALLS; k++) {
            const float ang = fr0((uint32_t)k * 7919u + 1u) * PI2;
            balls[k].x = (fr0((uint32_t)k * 3u + 1u) * 2.f - 1.f) * SC * 0.5f;
            balls[k].y = (fr0((uint32_t)k * 5u + 2u) * 2.f - 1.f) * SC * 0.5f;
            balls[k].vx = cosf(ang);
            balls[k].vy = sinf(ang);
            balls[k].init = true;
            for (int t = 0; t < BP_TRAIL_MAX; t++) {
                balls[k].trailX[t] = balls[k].x;
                balls[k].trailY[t] = balls[k].y;
            }
        }
        seeded  = true;
        lastMs  = nowMs;
        startMs = nowMs;
    }

    const uint32_t elapsed  = nowMs - startMs;
    const uint8_t  trailLen = (gLivePreset.trail < BP_TRAIL_MAX) ? gLivePreset.trail : BP_TRAIL_MAX;
    const bool     endless  = gLivePreset.bp_endless;
    const uint32_t durMs    = (uint32_t)gLivePreset.bp_duration_sec * 1000u;
    // Time-limited mode: run for durMs, then go dark for durMs, then
    // auto-restart at a new random position (re-seed next cycle).
    bool expired = false;
    if (!endless && durMs > 0) {
        const uint32_t cycle = elapsed % (durMs * 2u);
        expired = (cycle >= durMs);
        if (cycle < durMs && elapsed >= durMs) {
            // Crossed into a new run phase -- re-seed balls
            seeded = false;
        }
    }

    // Number of active balls: 1..BP_MAX_BALLS mapped from sz
    int nBalls = 1 + (int)((sz / 255.f) * (BP_MAX_BALLS - 1));
    if (nBalls < 1) nBalls = 1;
    if (nBalls > BP_MAX_BALLS) nBalls = BP_MAX_BALLS;

    // Speed: units per millisecond (4..36 u/ms depending on sp)
    const float spd = 4.f + (sp / 255.f) * 32.f;

    // Delta-time integration
    uint32_t dtMs = (nowMs > lastMs) ? (nowMs - lastMs) : 0;
    if (dtMs > 50) dtMs = 50;  // cap to avoid tunnelling after hiccup
    lastMs = nowMs;

    const float boundary = SC * 0.92f;

    if (!expired) {
        auto fr1 = [](uint32_t seed) -> float {
            float x = sinf((float)seed * 127.1f + 1.f) * 43758.5453f;
            return x - floorf(x);
        };
        for (int k = 0; k < nBalls; k++) {
            Ball& b = balls[k];
            // Re-seed ball if it was just enabled (init still false from a previous lower count)
            if (!b.init) {
                const float ang = fr1((uint32_t)(k * 997u + nowMs * 31u)) * PI2;
                b.vx   = cosf(ang);
                b.vy   = sinf(ang);
                b.init = true;
                for (int t = 0; t < BP_TRAIL_MAX; t++) { b.trailX[t] = b.x; b.trailY[t] = b.y; }
            }
            // Integrate first, then record trail only when ball moved
            // enough -- prevents all trail slots collapsing to the same
            // point at high frame rates.
            b.x += b.vx * spd * (float)dtMs;
            b.y += b.vy * spd * (float)dtMs;
            {
                float tdx = b.x - b.trailX[0], tdy = b.y - b.trailY[0];
                const float kTrailStep2 = 400.f; // shift when moved >20 units
                if (tdx*tdx + tdy*tdy >= kTrailStep2) {
                    for (int t = BP_TRAIL_MAX - 1; t > 0; t--) {
                        b.trailX[t] = b.trailX[t - 1];
                        b.trailY[t] = b.trailY[t - 1];
                    }
                    b.trailX[0] = b.x;
                    b.trailY[0] = b.y;
                }
            }
            // Bounce
            if (b.x >  boundary) { b.x =  2.f * boundary - b.x; b.vx = -fabsf(b.vx); }
            if (b.x < -boundary) { b.x = -2.f * boundary - b.x; b.vx =  fabsf(b.vx); }
            if (b.y >  boundary) { b.y =  2.f * boundary - b.y; b.vy = -fabsf(b.vy); }
            if (b.y < -boundary) { b.y = -2.f * boundary - b.y; b.vy =  fabsf(b.vy); }
        }
    }

    // ── Output ────────────────────────────────────────────────────────────
    if (expired) return 0;   // dark phase: beam off, balls invisible
    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    uint16_t kpps = gProjection.galvo_kpps;
    if (kpps < 12) kpps = 12; if (kpps > 60) kpps = 60;
    const float tick_us = 1000000.f / ((float)kpps * 1000.f);
    int dwell = (int)ceilf(180.f / tick_us);
    if (dwell < 4) dwell = 4;

    size_t n = 0;
    for (int k = 0; k < nBalls && n < m; k++) {
        const Ball& b = balls[k];
        const float hue = PI2 * (float)k / (float)nBalls;
        const uint8_t br_ = (uint8_t)(128 + 127 * sinf(hue));
        const uint8_t bg_ = (uint8_t)(128 + 127 * sinf(hue + 2.094f));
        const uint8_t bb_ = (uint8_t)(128 + 127 * sinf(hue + 4.189f));

        // Draw trail oldest-first so head overwrites (appears brightest)
        if (trailLen > 0) {
            for (int t = trailLen - 1; t >= 0; t--) {
                const float fade = 1.f - (float)(t + 1) / (float)(trailLen + 1);
                const uint8_t tr_ = (uint8_t)(br_ * fade);
                const uint8_t tg_ = (uint8_t)(bg_ * fade);
                const uint8_t tb_ = (uint8_t)(bb_ * fade);
                if (n >= m) break;
                optimizer::emitBlankTo(o, n, m, b.trailX[t], b.trailY[t], cfg);
                for (int d = 0; d < dwell && n < m; d++)
                    ap(o, n, m, b.trailX[t], b.trailY[t], tr_, tg_, tb_, 0);
            }
        }
        // Head. dwell*2: survives the post-blank LASER_ON_HOLD_TICKS
        // hold-off (see p_fireworks spark fix) -- plain dwell can be
        // entirely swallowed by it at low galvo_kpps, leaving the head dark.
        if (n < m) {
            optimizer::emitBlankTo(o, n, m, b.x, b.y, cfg);
            for (int d = 0; d < dwell * 2 && n < m; d++)
                ap(o, n, m, b.x, b.y, br_, bg_, bb_, 0);
        }
    }
    return n;
}
#undef BP_MAX_BALLS
#undef BP_TRAIL_MAX


// p_shooting -- Shooting Stars. Each meteor travels diagonally across the
// scan area; when it exits, a "+" cross flashes briefly at the tail impact
// point and fades out. Trails drawn oldest-to-newest for natural fade.
//
// sp  = travel speed   (0=slow .. 255=fast)
// sz  = meteor count   (0=1 .. 255=8)
#define SS_MAX   8
#define SS_TRAIL 14
static size_t p_shooting(LaserPoint* o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    (void)ph;  // millis()-driven

    struct Meteor {
        float x, y;          // current head position
        float vx, vy;        // unit direction * speed
        float trailX[SS_TRAIL];
        float trailY[SS_TRAIL];
        float plusX, plusY;  // position of the "+" flash
        uint32_t plusStart;  // millis() when "+" started, 0 = inactive
        uint8_t  hue_idx;    // colour slot 0..SS_MAX-1
        bool     active;
    };

    static Meteor  meteors[SS_MAX];
    static uint32_t lastMs = 0;
    static bool     inited = false;

    const uint32_t nowMs = millis();
    const uint32_t dtMs  = (nowMs > lastMs && (nowMs - lastMs) < 100u)
                           ? (nowMs - lastMs) : 16u;
    lastMs = nowMs;

    const int nMeteors = 1 + (int)((sz / 255.f) * (SS_MAX - 1));
    // Speed: units/ms. At sp=128 a meteor crosses the field in ~1 s.
    const float baseSpd = 8.f + (sp / 255.f) * 72.f;

    // Deterministic seed hash
    auto fr = [](uint32_t s) -> float {
        float x = sinf((float)s * 127.1f + 1.f) * 43758.5453f;
        return x - floorf(x);
    };

    // Spawn a meteor at a random entry edge (top or left side), heading
    // diagonally toward the opposite side. Angle ±20..±70° to horizontal.
    auto spawnMeteor = [&](Meteor& met, int idx, uint32_t seed) {
        const float side = fr(seed);            // 0-0.5 = top, 0.5-1 = left
        const float ang  = (0.35f + fr(seed + 7u) * 0.35f) * M_PI; // 63..126°
        met.vx = cosf(ang) * baseSpd;
        met.vy = sinf(ang) * baseSpd;
        // Entry position
        if (side < 0.5f) {
            // spawn at top, heading downward (vy > 0 after flip)
            met.x  = (fr(seed + 1u) * 2.f - 1.f) * SC * 0.9f;
            met.y  = -SC * 0.95f;
            met.vy =  fabsf(met.vy);
        } else {
            // spawn at left, heading rightward
            met.x  = -SC * 0.95f;
            met.y  = (fr(seed + 2u) * 2.f - 1.f) * SC * 0.9f;
            met.vx =  fabsf(met.vx);
        }
        for (int t = 0; t < SS_TRAIL; t++) { met.trailX[t] = met.x; met.trailY[t] = met.y; }
        met.plusX     = met.x;
        met.plusY     = met.y;
        met.plusStart = 0;
        met.hue_idx   = (uint8_t)idx;
        met.active    = true;
    };

    if (!inited) {
        for (int k = 0; k < SS_MAX; k++) {
            // Stagger meteors across the field so they don't all start together
            uint32_t seed = (uint32_t)k * 6271u + 1u;
            spawnMeteor(meteors[k], k, seed);
            // Phase-offset position so they enter at different points
            const float offset = fr(seed + 3u) * 2.f;
            meteors[k].x += meteors[k].vx * offset * 500.f;
            meteors[k].y += meteors[k].vy * offset * 500.f;
            for (int t = 0; t < SS_TRAIL; t++) {
                meteors[k].trailX[t] = meteors[k].x;
                meteors[k].trailY[t] = meteors[k].y;
            }
        }
        inited = true;
    }

    const float boundary = SC * 0.98f;
    const uint32_t plusDurMs = 600u;  // "+" visible for 600 ms

    for (int k = 0; k < nMeteors; k++) {
        Meteor& met = meteors[k];
        if (!met.active) continue;

        // Recompute velocity from current baseSpd (sp may have changed live)
        const float len = sqrtf(met.vx * met.vx + met.vy * met.vy);
        if (len > 0.1f) {
            met.vx = (met.vx / len) * baseSpd;
            met.vy = (met.vy / len) * baseSpd;
        }

        // Advance
        met.x += met.vx * (float)dtMs;
        met.y += met.vy * (float)dtMs;

        // Trail shift when moved enough
        {
            float dx = met.x - met.trailX[0], dy = met.y - met.trailY[0];
            if (dx*dx + dy*dy >= 400.f) {
                for (int t = SS_TRAIL - 1; t > 0; t--) {
                    met.trailX[t] = met.trailX[t-1];
                    met.trailY[t] = met.trailY[t-1];
                }
                met.trailX[0] = met.x;
                met.trailY[0] = met.y;
            }
        }

        // Out-of-bounds: trigger "+" flash, then respawn
        if (met.x >  boundary || met.x < -boundary ||
            met.y >  boundary || met.y < -boundary) {
            met.plusX     = met.x;
            met.plusY     = met.y;
            // Clamp "+" position to visible area
            if (met.plusX >  boundary) met.plusX =  boundary;
            if (met.plusX < -boundary) met.plusX = -boundary;
            if (met.plusY >  boundary) met.plusY =  boundary;
            if (met.plusY < -boundary) met.plusY = -boundary;
            met.plusStart = nowMs;
            spawnMeteor(meteors[k], k, nowMs ^ (uint32_t)k * 6271u);
        }
    }

    // ── Output ────────────────────────────────────────────────────────────
    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    uint16_t kpps = gProjection.galvo_kpps;
    if (kpps < 12) kpps = 12; if (kpps > 60) kpps = 60;
    const float tick_us = 1000000.f / ((float)kpps * 1000.f);
    int dwell = (int)ceilf(120.f / tick_us);
    if (dwell < 2) dwell = 2;

    size_t n = 0;

    for (int k = 0; k < nMeteors; k++) {
        const Meteor& met = meteors[k];
        if (!met.active) continue;

        // Hue: evenly spaced, white-ish blue-white palette
        const float hue = PI2 * (float)met.hue_idx / (float)SS_MAX;
        const uint8_t cr = (uint8_t)(180 + 75 * sinf(hue));
        const uint8_t cg = (uint8_t)(180 + 75 * sinf(hue + 2.094f));
        const uint8_t cb = 255;

        // Trail: oldest (dim) → newest (bright), fade linear
        for (int t = SS_TRAIL - 1; t >= 0; t--) {
            const float fade = 1.f - (float)(t + 1) / (float)(SS_TRAIL + 1);
            const uint8_t tr = (uint8_t)(cr * fade);
            const uint8_t tg = (uint8_t)(cg * fade);
            const uint8_t tb = (uint8_t)(255 * fade);
            if (n >= m) break;
            optimizer::emitBlankTo(o, n, m, met.trailX[t], met.trailY[t], cfg);
            for (int d = 0; d < dwell && n < m; d++)
                ap(o, n, m, met.trailX[t], met.trailY[t], tr, tg, tb, 0);
        }
        // Head (full brightness)
        if (n < m) {
            optimizer::emitBlankTo(o, n, m, met.x, met.y, cfg);
            for (int d = 0; d < dwell * 2 && n < m; d++)
                ap(o, n, m, met.x, met.y, cr, cg, cb, 0);
        }

        // "+" flash: draw cross at plusX/plusY with fade-out
        if (met.plusStart > 0 && (nowMs - met.plusStart) < plusDurMs) {
            const float t_norm = 1.f - (float)(nowMs - met.plusStart) / (float)plusDurMs;
            const float fade   = t_norm * t_norm;  // quadratic fade
            const uint8_t pr   = (uint8_t)(255 * fade);
            const uint8_t pg   = (uint8_t)(255 * fade);
            const uint8_t pb   = (uint8_t)(255 * fade);
            const float armLen = SC * 0.06f;
            // Horizontal arm
            if (n < m) {
                optimizer::emitBlankTo(o, n, m, met.plusX - armLen, met.plusY, cfg);
                for (int d = 0; d < dwell && n < m; d++)
                    ap(o, n, m, met.plusX - armLen, met.plusY, pr, pg, pb, 0);
            }
            if (n < m) {
                optimizer::emitBlankTo(o, n, m, met.plusX, met.plusY, cfg);
                for (int d = 0; d < dwell && n < m; d++)
                    ap(o, n, m, met.plusX, met.plusY, pr, pg, pb, 0);
            }
            if (n < m) {
                optimizer::emitBlankTo(o, n, m, met.plusX + armLen, met.plusY, cfg);
                for (int d = 0; d < dwell && n < m; d++)
                    ap(o, n, m, met.plusX + armLen, met.plusY, pr, pg, pb, 0);
            }
            // Vertical arm
            if (n < m) {
                optimizer::emitBlankTo(o, n, m, met.plusX, met.plusY - armLen, cfg);
                for (int d = 0; d < dwell && n < m; d++)
                    ap(o, n, m, met.plusX, met.plusY - armLen, pr, pg, pb, 0);
            }
            if (n < m) {
                optimizer::emitBlankTo(o, n, m, met.plusX, met.plusY + armLen, cfg);
                for (int d = 0; d < dwell && n < m; d++)
                    ap(o, n, m, met.plusX, met.plusY + armLen, pr, pg, pb, 0);
            }
        }
    }
    return n;
}
#undef SS_MAX
#undef SS_TRAIL

// ─── NEW SCENE PRESETS ───────────────────────────────────────

// Shared deterministic hash: seed -> [0,1). Same fract(sin) trick used by
// the other random presets, so it needs no PRNG state.
static inline float sHash(uint32_t s) {
    float x = sinf((float)s * 127.1f + 1.f) * 43758.5453f;
    return x - floorf(x);
}

// p_endless_spiral -- Endless Spiral. One (or several) Archimedean arms whose
// radius grows outward while the whole figure rotates continuously; points
// fade from centre (dim) to rim (bright) so it reads as flowing outward.
// Continuous-sweep style (registered in isContinuous) -> no seam jump.
//   sp = rotation/flow speed
//   sz = overall scale
//   gLivePreset.spiral_arms = arm count 1..6
static size_t p_endless_spiral(LaserPoint* o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    size_t n = 0;
    const float sc   = SC * ssc(sz) * 0.95f;
    const float flow = aang(ph, sp);                 // continuous outward flow
    uint8_t arms = gLivePreset.spiral_arms; if (arms < 1) arms = 1; if (arms > 6) arms = 6;
    const int perArm = adaptN(sz, 150, 40, 300);
    const float turns = 4.0f;                         // radial turns per arm

    for (int a = 0; a < arms && n < m; a++) {
        const float armPhase = PI2 * (float)a / (float)arms;
        for (int i = 0; i < perArm && n < m; i++) {
            const float t   = (float)i / (float)(perArm - 1);   // 0..1 centre->rim
            const float ang = armPhase + flow + t * turns * PI2;
            const float rad = t * sc;
            const float x   = cosf(ang) * rad;
            const float y   = sinf(ang) * rad;
            const uint8_t v = (uint8_t)(40 + 215 * t);          // dim centre -> bright rim
            ap(o, n, m, x, y, v, 0, (uint8_t)(255 - v), i == 0 ? 1 : 0);
        }
    }
    return n;
}

// p_endless_tunnel -- Endless Tunnel. Concentric rings (polygon or circle)
// scaled around a vanishing point that is offset toward a corner for a
// pseudo-perspective look. Each ring's scale cycles inward continuously so it
// feels like flying through a tunnel; a ring reaching the centre wraps back to
// the outer edge. Brightness increases with ring size (near rings brighter).
//   sp = travel speed
//   sz = tunnel scale
//   gLivePreset.tunnel_rings = ring count 3..12
//   gLivePreset.tunnel_sides = polygon sides 3..10
static size_t p_endless_tunnel(LaserPoint* o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    size_t n = 0;
    const float sc = SC * ssc(sz) * 0.98f;
    uint8_t rings = gLivePreset.tunnel_rings; if (rings < 3) rings = 3; if (rings > 12) rings = 12;
    uint8_t sides = gLivePreset.tunnel_sides; if (sides < 3) sides = 3; if (sides > 10) sides = 10;

    // Vanishing point slightly off-centre -> perspective tunnel
    const float vpx = sc * 0.12f, vpy = -sc * 0.10f;
    // Continuous inward travel phase (0..1), sp-controlled
    const float travel = (sp == 0) ? 0.f : fmodf(ph * (sp / 9000.0f), 1.0f);

    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();

    for (int rIdx = 0; rIdx < rings && n < m; rIdx++) {
        // depth 0..1: 0 = far (small, near vanishing pt), 1 = near (full size)
        float depth = (float)rIdx / (float)rings + travel / (float)rings;
        depth = depth - floorf(depth);                  // wrap 0..1
        const float scale = 0.04f + depth * depth;      // quadratic -> perspective accel
        const float rad   = scale * sc;
        const uint8_t v   = (uint8_t)(30 + 225 * depth);
        const float cx = vpx * (1.f - depth);
        const float cy = vpy * (1.f - depth);

        optimizer::PathVertex verts[10];
        for (int s = 0; s < sides; s++) {
            const float aa = PI2 * (float)s / (float)sides - (float)M_PI / 2.f;
            verts[s] = optimizer::PathVertex(cx + cosf(aa) * rad,
                                             cy + sinf(aa) * rad,
                                             v, (uint8_t)(v / 2), 255, s == 0);
        }
        optimizer::PathSegment seg(verts, sides, /*closed=*/true);
        n += optimizer::optimize(&seg, 1, o + n, m - n, cfg);
    }
    return n;
}

// p_explosion -- Point-to-multipoint spread. Rays fire out from the centre;
// each ray is a short lit segment whose length grows over a cycle, then the
// whole burst resets (fades) and repeats -- reads as a repeating explosion.
//   sp = explosion speed (cycle rate)
//   sz = max radius
//   gLivePreset.explosion_rays = ray count 4..40
static size_t p_explosion(LaserPoint* o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    (void)ph;   // millis()-driven
    size_t n = 0;
    const float sc = SC * ssc(sz) * 0.95f;
    uint8_t rays = gLivePreset.explosion_rays; if (rays < 4) rays = 4; if (rays > 40) rays = 40;

    const uint16_t cycleMs = (uint16_t)(1600.f - (sp / 255.f) * 1400.f);  // 1600..200 ms
    const uint32_t nowMs   = millis();
    const uint32_t cycleId = nowMs / cycleMs;
    const float    prog    = (float)(nowMs % cycleMs) / (float)cycleMs;    // 0..1
    // Ease-out so the shell shoots fast then decelerates
    const float ease  = 1.f - (1.f - prog) * (1.f - prog);
    const float fade  = (prog < 0.7f) ? 1.f : (1.f - (prog - 0.7f) / 0.3f); // tail fade

    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();

    for (int i = 0; i < rays && n < m; i++) {
        // Angle jitter re-rolled per cycle so each explosion differs
        const float baseA = PI2 * (float)i / (float)rays;
        const float jit   = (sHash(cycleId * 131u + i * 17u) - 0.5f) * (PI2 / rays);
        const float ang   = baseA + jit;
        const float rOuter = ease * sc;
        const float rInner = rOuter * 0.55f;            // lit segment (tracer)
        const uint8_t hueI = (uint8_t)(i * 255 / rays);
        const uint8_t r = (uint8_t)(fabsf(sinf(hueI / 40.f)) * 255) ;
        const uint8_t g = (uint8_t)(fabsf(sinf(hueI / 40.f + 2.094f)) * 255);
        const uint8_t b = (uint8_t)(fabsf(sinf(hueI / 40.f + 4.189f)) * 255);
        const uint8_t vr = (uint8_t)(r * fade), vg = (uint8_t)(g * fade), vb = (uint8_t)(b * fade);

        const float ix = cosf(ang) * rInner, iy = sinf(ang) * rInner;
        const float ox = cosf(ang) * rOuter, oy = sinf(ang) * rOuter;
        optimizer::emitBlankTo(o, n, m, ix, iy, cfg);
        // galvo_out.cpp holds the laser off for LASER_ON_HOLD_TICKS (=2) ticks
        // after every blank jump to hide LEDC turn-on latency. This tracer is
        // only 2 lit points long, so without padding those 2 ticks ARE the
        // whole stroke and every ray comes out dark. Pad with extra dwell at
        // the inner point (DAC already parked there from the blank jump) to
        // absorb the hold before the real stroke is drawn.
        for (int k = 0; k < 3 && n < m; k++) ap(o, n, m, ix, iy, vr, vg, vb, 0);
        ap(o, n, m, ox, oy, vr, vg, vb, 0);
    }
    return n;
}

// p_fireworks -- New Year's Fireworks. Each shell: a rocket dot rises from
// the bottom, then detonates with a central white flash followed by radial
// sparks with a glitter (twinkle) overlay. The burst shape is re-rolled per
// shell instance (small circle / large circle / ellipse / golden-rain
// willow), along with angle, burst height and colour, so every launch
// differs. Up to fw_max_shells run concurrently, phase-staggered.
//   sp = launch tempo
//   sz = spark spread radius
//   gLivePreset.fw_max_shells = 1..3 concurrent shells
//   gLivePreset.fw_glitter    = sparkle overlay on/off
static size_t p_fireworks(LaserPoint* o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    (void)ph;   // millis()-driven
    size_t n = 0;
    const float sc = SC * ssc(sz) * 0.9f;
    uint8_t shells = gLivePreset.fw_max_shells; if (shells < 1) shells = 1; if (shells > 3) shells = 3;
    const bool glitter = gLivePreset.fw_glitter;

    const uint16_t riseMs  = (uint16_t)(1800.f - (sp / 255.f) * 1500.f); // 1800..300 ms
    const uint16_t burstMs = 1100;                                       // spark lifetime
    const uint32_t lifeMs  = (uint32_t)riseMs + burstMs;
    const uint32_t periodMs = lifeMs + 400;                              // + gap before relaunch
    const uint32_t nowMs   = millis();

    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    uint16_t kpps = gProjection.galvo_kpps; if (kpps < 12) kpps = 12; if (kpps > 60) kpps = 60;
    const float tick_us = 1000000.f / ((float)kpps * 1000.f);
    int dwell = (int)ceilf(120.f / tick_us); if (dwell < 2) dwell = 2;

    for (int s = 0; s < shells && n < m; s++) {
        const uint32_t off   = (uint32_t)(sHash(s * 733u + 3u) * periodMs);
        const uint32_t t     = (nowMs + off) % periodMs;
        const uint32_t cycle = (nowMs + off) / periodMs;
        if (t >= lifeMs) continue;                       // in the idle gap

        // Per-launch randomised parameters
        const uint32_t seed = s * 10007u + cycle * 997u;
        const float launchX = (sHash(seed + 1u) * 1.6f - 0.8f) * sc;         // -0.8..0.8
        // DAC space: +y = up. Burst height is the upper area (+y); launch is
        // the bottom (-y) -- see corner-color-map calibration note.
        const float burstY  = (0.15f + sHash(seed + 2u) * 0.55f) * sc;       // upper area
        const float hue     = sHash(seed + 3u) * PI2;
        const float startY  = -sc * 0.95f;                                   // bottom
        // Slight diagonal drift + mid-flight bow, so shells don't all rise
        // in a perfectly straight vertical line.
        const float driftX  = (sHash(seed + 5u) - 0.5f) * 0.35f * sc;        // end-point sideways drift
        const float curveX  = (sHash(seed + 6u) - 0.5f) * 0.18f * sc;        // mid-flight bow, gone by burst
        const float finalX  = launchX + driftX;                              // trajectory endpoint = burst X

        if (t < riseMs) {
            // ── Rising: single rocket dot climbing (slightly curved/diagonal path) ──
            const float rp = (float)t / (float)riseMs;
            const float ry = L(startY, burstY, rp);
            const float rx = launchX + driftX * rp + curveX * sinf(rp * (float)M_PI);
            const uint8_t rv = (uint8_t)(180 + 75 * sinf(rp * (float)M_PI));
            optimizer::emitBlankTo(o, n, m, rx, ry, cfg);
            for (int k = 0; k < dwell * 2 && n < m; k++) ap(o, n, m, rx, ry, rv, rv, 255, 0);
        } else {
            // ── Burst: white detonation flash + radial sparks (shape varies) + glitter ──
            const float bp = (float)(t - riseMs) / (float)burstMs;           // 0..1
            const float ease = 1.f - (1.f - bp) * (1.f - bp);
            const float fade = (bp < 0.6f) ? 1.f : (1.f - (bp - 0.6f) / 0.4f);

            // Central white flash at the instant of detonation, fading fast.
            if (bp < 0.12f) {
                const uint8_t fv = (uint8_t)(255 * (1.f - bp / 0.12f));
                optimizer::emitBlankTo(o, n, m, finalX, burstY, cfg);
                for (int k = 0; k < dwell * 2 && n < m; k++) ap(o, n, m, finalX, burstY, fv, fv, fv, 0);
            }

            // Burst shape re-rolled per shell instance for variety.
            const int variant = (int)(sHash(seed + 60u) * 4.f) & 3;
            // 0 = small circle, 1 = large circle, 2 = ellipse, 3 = golden-rain willow
            const int   sparks  = (variant == 3) ? 26 : (variant == 1) ? 24 : 18;
            const float radBase = ease * sc * 0.42f;
            // colour shifts over the burst (hue -> hue+PI)
            const float h = hue + bp * (float)M_PI;

            for (int i = 0; i < sparks && n < m; i++) {
                const float a = PI2 * (float)i / (float)sparks
                              + (sHash(seed + 40u + i) - 0.5f) * 0.3f;
                float gl = 1.f;
                if (glitter) {
                    // twinkle: per-spark on/off flicker driven by time
                    const float tw = sHash((uint32_t)(nowMs / 60u) * 31u + i * 7u + seed);
                    gl = 0.35f + 0.65f * (tw > 0.5f ? 1.f : 0.25f);
                }

                float px, py;
                switch (variant) {
                    case 0: {   // small, tight circle
                        const float rad = radBase * 0.55f;
                        px = finalX + cosf(a) * rad;
                        py = burstY + sinf(a) * rad;
                        break;
                    }
                    case 1: {   // large, wide circle
                        const float rad = radBase * 1.35f;
                        px = finalX + cosf(a) * rad;
                        py = burstY + sinf(a) * rad;
                        break;
                    }
                    case 2: {   // ellipse, random aspect + rotation
                        const float radX = radBase * (0.9f + sHash(seed + 61u) * 0.5f);
                        const float radY = radX * (0.35f + sHash(seed + 62u) * 0.35f);
                        const float rot  = sHash(seed + 63u) * PI2;
                        const float lx = cosf(a) * radX, ly = sinf(a) * radY;
                        px = finalX + lx * cosf(rot) - ly * sinf(rot);
                        py = burstY + lx * sinf(rot) + ly * cosf(rot);
                        break;
                    }
                    default: {  // "Goldregen": embers drift out, then fall under gravity
                        const float outRad = radBase * 0.5f;
                        const float fall   = bp * bp * sc * 0.5f;  // falling = -y (down)
                        px = finalX + cosf(a) * outRad;
                        py = burstY + sinf(a) * outRad * 0.4f - fall;
                        break;
                    }
                }

                uint8_t r, g, b;
                if (variant == 3) {
                    // fixed golden-rain colour, independent of the shell's random hue
                    r = (uint8_t)(255 * fade * gl);
                    g = (uint8_t)(190 * fade * gl);
                    b = (uint8_t)(40  * fade * gl);
                } else {
                    r = (uint8_t)((128 + 127 * sinf(h))          * fade * gl);
                    g = (uint8_t)((128 + 127 * sinf(h + 2.094f)) * fade * gl);
                    b = (uint8_t)((128 + 127 * sinf(h + 4.189f)) * fade * gl);
                }
                optimizer::emitBlankTo(o, n, m, px, py, cfg);
                // galvo_out.cpp holds the laser off for LASER_ON_HOLD_TICKS (=2)
                // ticks after every blank jump (LEDC turn-on latency). At low
                // galvo_kpps, plain `dwell` is only 2 ticks -- the hold-off ate
                // the entire spark, so every spark came out dark and only the
                // rise/flash (which already use dwell*2) were visible. Match
                // those so the hold-off leaves real lit ticks behind.
                for (int k = 0; k < dwell * 2 && n < m; k++) ap(o, n, m, px, py, r, g, b, 0);
            }
        }
    }
    return n;
}

// p_milkyway -- Milky Way galaxy. Up to mw_dots stars laid out on a slowly
// rotating spiral disc, projected in an oblique top-down view (the y-axis is
// compressed by mw_tilt so the disc looks slanted -> pseudo-3D). Star
// brightness varies by radius (bright core -> dim rim) plus a subtle twinkle.
//   sp = disc rotation speed
//   sz = disc scale
//   gLivePreset.mw_dots = star count 10..60
//   gLivePreset.mw_tilt = view tilt 20..80 % (y compression)
static size_t p_milkyway(LaserPoint* o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    size_t n = 0;
    const float sc  = SC * ssc(sz) * 0.92f;
    uint8_t dots = gLivePreset.mw_dots; if (dots < 10) dots = 10; if (dots > 60) dots = 60;
    uint8_t tiltPct = gLivePreset.mw_tilt; if (tiltPct < 20) tiltPct = 20; if (tiltPct > 80) tiltPct = 80;
    const float yScale = tiltPct / 100.f;                 // oblique projection compression
    const float rot    = aang(ph, sp);                    // slow disc rotation

    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    uint16_t kpps = gProjection.galvo_kpps; if (kpps < 12) kpps = 12; if (kpps > 60) kpps = 60;
    const float tick_us = 1000000.f / ((float)kpps * 1000.f);
    int dwell = (int)ceilf(120.f / tick_us); if (dwell < 2) dwell = 2;

    // Two-arm logarithmic spiral distribution (fixed seed -> stable star field)
    const float armTurns = 2.4f;
    for (int i = 0; i < dots && n < m; i++) {
        const float t    = (float)i / (float)(dots - 1);          // 0..1 core->rim
        const int   arm  = i & 1;
        const float armA = (float)arm * (float)M_PI;
        // radius: denser toward core
        const float rad  = powf(t, 0.7f) * sc;
        const float jit  = (sHash(i * 53u + 7u) - 0.5f) * 0.35f;  // scatter off the arm
        const float ang  = armA + rot + t * armTurns * PI2 + jit;
        const float x    = cosf(ang) * rad;
        const float y    = sinf(ang) * rad * yScale;              // oblique squash

        // brightness: bright core, dim rim, + gentle twinkle
        const float coreB = 1.f - 0.6f * t;
        const float tw    = 0.8f + 0.2f * sHash((uint32_t)(ph / 4u) * 17u + i * 3u);
        const uint8_t v   = (uint8_t)(constrain(255.f * coreB * tw, 20.f, 255.f));
        // colour: bluish-white core -> faint blue rim
        const uint8_t r = (uint8_t)(v * (0.85f - 0.35f * t));
        const uint8_t g = (uint8_t)(v * (0.9f  - 0.2f  * t));
        const uint8_t b = v;

        optimizer::emitBlankTo(o, n, m, x, y, cfg);
        // dwell*2: survives the post-blank LASER_ON_HOLD_TICKS hold-off (see
        // p_fireworks spark fix) -- plain dwell can be entirely swallowed by
        // it at low galvo_kpps, leaving the star dark.
        for (int d = 0; d < dwell * 2 && n < m; d++) ap(o, n, m, x, y, r, g, b, 0);
    }
    return n;
}

// ─── DISPATCH ────────────────────────────────────────────────

// presetClassOf() -- maps a Preset to its optimizer profile index.
// Grouped by scanner workload (see PresetClass in preset_patterns.h), so the
// mapping deliberately cuts across the display categories in PRESETS[].
PresetClass presetClassOf(Preset p)
{
    if (p == presets::Preset::None) return presets::PresetClass::Vector;
    using P = presets::Preset;
    switch (p) {
        // ── Vector: closed polygons + straight-line runs ──────────────
        // Sharp vertices with long straight edges between them: corner
        // dwell is the dominant cost, interior density barely matters.
        case P::Circle: case P::Square: case P::Triangle:
        case P::Pentagon: case P::Hexagon: case P::Octagon:
        case P::Star4: case P::Star5: case P::Star6: case P::Star8:
        case P::Pentagram:
        case P::CrossPlus: case P::XShape: case P::Grid3x3:
        case P::HLine: case P::Diagonal:
        case P::ThreeCircles:
            return presets::PresetClass::Vector;

        // ── Smooth: continuous closed curves ──────────────────────────
        // No true corners -- quality is set by interior density alone.
        // Corner dwell here only wastes frame budget.
        case P::EndlessSpiral:
        case P::ArchimedeanSpiral: case P::DoubleSpiral:
        case P::Lissajous1To2: case P::Lissajous2To3: case P::Lissajous3To4:
        case P::Lissajous3To5: case P::Lissajous5To6:
        case P::Rose3: case P::Rose4:
        case P::Heart: case P::Infinity: case P::Astroid: case P::Epitrochoid:
        case P::Hypotrochoid: case P::Butterfly: case P::Spirograph5To3:
        case P::PulsingCircle: case P::DnaHelix: case P::YinYang:
            return presets::PresetClass::Smooth;

        // ── Waves: open polylines, high spatial frequency ─────────────
        // Left-to-right sweeps that never close. Fast direction reversals
        // at every crest make velocity/accel clamping the binding limit.
        case P::SineWave: case P::StandingWave: case P::MultiWave:
        case P::OceanWave: case P::WaveInterference: case P::Sawtooth:
        case P::SquareWave: case P::WavePacket: case P::BeatWave:
        case P::RadialWaves: case P::FmWave: case P::Vortex:
        case P::SineHelix: case P::WaveField: case P::FourierSquare:
        case P::GravityWaves: case P::Tsunami: case P::WaveSpectrum:
            return presets::PresetClass::Waves;

        // ── Wireframe: 3D edge chains ─────────────────────────────────
        // Needs buildWfChains() to see incoming+outgoing edges per vertex.
        case P::RotatingCube: case P::StaticCube: case P::Pyramid:
        case P::Octahedron: case P::Tetrahedron:
            return presets::PresetClass::Wireframe;

        // ── MultiObject: several separate closed objects per frame ────
        // Frame time is dominated by the blanked jumps between objects,
        // not by the objects themselves.
        case P::SolarSystem:
        case P::ConcentricRings: case P::NestedSquares:
        case P::DiscoBall: case P::LaserDiamond:
        case P::Starburst: case P::StarburstParty: case P::Hibiscus:
        case P::ChaosBouncer: case P::CountdownTimer:
        case P::EndlessTunnel:
            return presets::PresetClass::MultiObject;

        // ── Particles: isolated dots, no connecting geometry ──────────
        case P::Starfield: case P::RandomPoints: case P::PointSpread:
        case P::ConfettiBurst: case P::BouncingPoints:
        case P::ExplosionSpread: case P::Fireworks: case P::MilkyWay:
            return presets::PresetClass::Particles;

        // ── Trails: moving dots with connected fade tails ─────────────
        // Same blank-dominated workload as Particles, but the tails are short
        // continuous chains. The pure-dot presets are tuned (on hardware) with
        // a high blank length that looks correct for isolated points but eats
        // enough of the frame to truncate a meteor's tail here, so Trails runs
        // a reduced budget (880) with the Smooth-style density instead.
        case P::ShootingStars:
            return presets::PresetClass::Trails;

        // ── Animated geometric structures ──────────────────────────────────
        case P::PythagorasTree:
            return presets::PresetClass::MultiObject;

        default:
            return presets::PresetClass::Vector;
    }
}

// ─── PROFILE MEMBER LISTS (WebUI) ────────────────────────────
// Names shown in the Optimizer tab's member column. Built from PRESETS[]
// at startup rather than duplicated here, so a preset rename can never
// desync the two.
static uint8_t s_profileMembers[OPT_PROFILE_COUNT][PRESET_COUNT];
static uint8_t s_profileMemberCount[OPT_PROFILE_COUNT];
static bool    s_profileMembersBuilt = false;

static void buildProfileMembers() {
    if (s_profileMembersBuilt) return;
    for (uint8_t i = 0; i < OPT_PROFILE_COUNT; i++) s_profileMemberCount[i] = 0;
    for (uint8_t i = 0; i < PRESET_COUNT; i++) {
        const uint8_t cls = (uint8_t)presetClassOf((Preset)i);
        if (cls >= OPT_PROFILE_COUNT) continue;
        s_profileMembers[cls][s_profileMemberCount[cls]++] = i;
    }
    s_profileMembersBuilt = true;
}

uint8_t profileMemberCount(uint8_t profile) {
    if (profile >= OPT_PROFILE_COUNT) return 0;
    buildProfileMembers();
    return s_profileMemberCount[profile];
}

const char* profileMemberName(uint8_t profile, uint8_t n) {
    if (profile >= OPT_PROFILE_COUNT) return nullptr;
    buildProfileMembers();
    if (n >= s_profileMemberCount[profile]) return nullptr;
    return PRESETS[s_profileMembers[profile][n]].name;
}

// p_pythagoras_tree -- Animated Pythagoras Tree, continuous zoom-in.
//
// Effect: the tree endlessly zooms in -- the camera perpetually dives into
// the canopy. Implemented by scaling baseW down by 2^frac (frac 0..1 loops)
// so every level appears to grow in from below while the top fades out.
//
// Each node is drawn as TWO open line segments (left wall + right wall of the
// square), not a closed rectangle. This avoids the horizontal base-streak
// that appears when the galvo sweeps the root's bottom edge across the screen.
//
// Internal tree-space: x right, y DOWN, origin centre, range +-SC.
// "Up" in tree-space = negative y. DAC space is actually +y = up (confirmed
// via the Corner Color Map calibration pattern), so every vertex's y is
// negated at PathVertex-emission time below to convert tree-space -> DAC
// space; the tree-building math above stays in its own y-down space
// unchanged.
//
// sp  = zoom speed   (0=very slow .. 255=fast)
// sz  = tree scale   (0=small .. 255=full)
//
// Segments: 254 nodes × 2 walls = 508 open segments, 1016 vertices (~12 KB).
// Point count after optimizer resampling fits within the 1300 pt budget.
#define PT_DEPTH  7
#define PT_NSEG   508    // 2 segments per node, (2^(PT_DEPTH+1)-2) nodes
#define PT_NVERT  1016   // 2 vertices per segment
static size_t p_pythagoras_tree(LaserPoint* o, size_t m,
                                uint32_t ph, uint8_t sp, uint8_t sz) {
    // Static storage: 508 segments * 2 vertices * 12 B = ~12 KB in DRAM.
    static optimizer::PathVertex verts[PT_NVERT];
    static optimizer::PathSegment segs[PT_NSEG];

    // Zoom: frac runs 0..1 endlessly (ph frame counter, ~20 fps).
    // spd range: sp=0 -> 0.003 (1 zoom cycle / ~33 s),
    //            sp=255 -> 0.03  (1 zoom cycle / ~3.3 s).
    const float spd  = 0.003f + (sp / 255.0f) * 0.027f;
    const float frac = fmodf((float)ph * spd, 1.0f);

    // Scale: 1.0 (frac=0) -> 0.5 (frac=1) then reset to 1.0.
    // This continuously shrinks the base so the tree appears to zoom in.
    const float zoomScale = powf(0.5f, frac);   // 1.0 .. 0.5

    // Fixed 45-degree split (symmetric tree) for clean infinite zoom.
    const float alpha = (float)M_PI * 0.25f;    // 45°
    const float cosA  = 0.70711f;               // cos(45°)
    const float sinA  = 0.70711f;               // sin(45°)

    // Max base width; zoomScale shrinks it each frame so the root stays
    // within the screen even as the tree grows one extra level per cycle.
    const float baseW = SC * (0.55f + (sz / 255.0f) * 0.35f) * zoomScale;

    struct Frame { float x0, y0, x1, y1; int depth; };
    // DFS stack: max depth = PT_DEPTH+1 levels * 2 branches = 20 entries max.
    // stk[512] (10 KB) crashed the pattern task (12 KB stack). Fixed to 20.
    Frame stk[20];
    int top = 0;

    // Root base: horizontal, centred, anchored near screen bottom.
    stk[top++] = { -baseW * 0.5f, SC * 0.72f,
                    baseW * 0.5f, SC * 0.72f, 0 };

    int segCount = 0;
    int vIdx     = 0;

    const int drawDepth = PT_DEPTH + 1;  // render one extra level for fade-in

    while (top > 0 && segCount + 2 <= PT_NSEG) {
        Frame fr = stk[--top];

        const float ex = fr.x1 - fr.x0;
        const float ey = fr.y1 - fr.y0;
        const float w  = sqrtf(ex * ex + ey * ey);
        if (w < 2.0f) continue;

        // Perpendicular "up": rotate base 90° CW in screen (galvo y-down).
        const float upx =  ey;
        const float upy = -ex;

        const float blx = fr.x0,      bly = fr.y0;
        const float brx = fr.x1,      bry = fr.y1;
        const float trx = brx + upx,  try_ = bry + upy;
        const float tlx = blx + upx,  tly  = bly + upy;

        // Depth colour: red (depth 0) -> green (depth PT_DEPTH).
        // Extra fade-in level (depth=drawDepth) fades in proportional to frac.
        // Shallowest visible level (depth=0 when frac>0) fades out.
        float brightness = 1.0f;
        if (fr.depth == 0) {
            // Root fades out as zoom progresses (disappears off the bottom).
            brightness = 1.0f - frac;
        } else if (fr.depth == drawDepth) {
            // Deepest extra level fades in.
            brightness = frac;
        }
        const int   colorDepth = (fr.depth < PT_DEPTH) ? fr.depth : PT_DEPTH;
        const float tc  = (float)colorDepth / (float)PT_DEPTH;
        const uint8_t cr = (uint8_t)(255.0f * (1.0f - tc) * brightness);
        const uint8_t cg = (uint8_t)(255.0f * tc           * brightness);

        // Draw left wall: BL -> TL  (open segment, 2 vertices)
        // y negated: tree-space (y-down) -> DAC space (y-up).
        verts[vIdx+0] = optimizer::PathVertex(blx, -bly, cr, cg, 0, /*lift=*/true);
        verts[vIdx+1] = optimizer::PathVertex(tlx, -tly, cr, cg, 0);
        segs[segCount] = optimizer::PathSegment(&verts[vIdx], 2, /*closed=*/false);
        vIdx     += 2;
        segCount += 1;

        // Draw right wall: BR -> TR  (open segment, 2 vertices)
        verts[vIdx+0] = optimizer::PathVertex(brx, -bry, cr, cg, 0, /*lift=*/true);
        verts[vIdx+1] = optimizer::PathVertex(trx, -try_, cr, cg, 0);
        segs[segCount] = optimizer::PathSegment(&verts[vIdx], 2, /*closed=*/false);
        vIdx     += 2;
        segCount += 1;

        if (fr.depth < drawDepth && top + 2 <= 18) {  // 18 = stk[20] - 2 guard
            // Child apex via screen-CCW rotation of top-edge vector by alpha.
            // Top-edge vector = (ex, ey) (parallel to base edge).
            // Screen-CCW in galvo y-down: R(v) = (vx*cosA + vy*sinA, -vx*sinA + vy*cosA)
            const float rx   = ex * cosA + ey * sinA;
            const float ry   = -ex * sinA + ey * cosA;
            const float len  = sqrtf(rx * rx + ry * ry);
            const float wL   = w * cosA;
            const float apLx = tlx + rx / len * wL;
            const float apLy = tly + ry / len * wL;

            stk[top++] = { tlx, tly, apLx, apLy, fr.depth + 1 };
            stk[top++] = { apLx, apLy, trx, try_, fr.depth + 1 };
        }
    }

    if (segCount == 0) return 0;
    return optimizer::optimize(segs, (size_t)segCount, o, m, liveOptimizerConfig());
}
#undef PT_DEPTH
#undef PT_NSEG
#undef PT_NVERT


const PresetInfo PRESETS[PRESET_COUNT] = {
    {"Circle","Geometry"},{"Square","Geometry"},{"Triangle","Geometry"},{"Pentagon","Geometry"},{"Hexagon","Geometry"},{"Octagon","Geometry"},{"Star 4","Geometry"},{"Star 5","Geometry"},{"Star 6","Geometry"},{"Star 8","Geometry"},
    {"Cross +","Lines"},{"X Shape","Lines"},{"Grid 3x3","Lines"},{"H Line","Lines"},{"Diagonal","Lines"},
    {"Archimedean Spiral","Spirals"},{"Lissajous 1:2","Spirals"},{"Lissajous 2:3","Spirals"},{"Lissajous 3:4","Spirals"},{"Lissajous 3:5","Spirals"},{"Lissajous 5:6","Spirals"},{"Double Spiral","Spirals"},{"Rose 3","Spirals"},
    {"Rose 4","Curves"},{"Heart","Curves"},{"Infinity","Curves"},{"Astroid","Curves"},{"Epitrochoid","Curves"},
    {"Rotating Cube","3D"},{"Static Cube","3D"},{"Pyramid","3D"},{"Octahedron","3D"},{"Tetrahedron","3D"},
    {"Sine Wave","Waves"},{"Standing Wave","Waves"},{"Multi Wave","Waves"},{"Ocean Wave","Waves"},{"Wave Interference","Waves"},{"Sawtooth","Waves"},{"Square Wave","Waves"},{"Wave Packet","Waves"},{"Beat Wave","Waves"},{"Radial Waves","Waves"},{"FM Wave","Waves"},{"Vortex","Waves"},{"Sine Helix","Waves"},{"Wave Field","Waves"},{"Fourier Square","Waves"},{"Gravity Waves","Waves"},{"Tsunami","Waves"},{"Wave Spectrum","Waves"},
    {"Hypotrochoid","Complex"},{"Butterfly","Complex"},{"Spirograph 5/3","Complex"},{"Concentric Rings","Complex"},{"Nested Squares","Complex"},{"Pulsing Circle","Complex"},
    {"Starburst","Combo"},{"Chaos Bouncer","Combo"},{"Laser Diamond","Combo"},{"Confetti Burst","Combo"},{"Disco Ball","Combo"},
    {"Hibiscus","Party"},{"Starburst Party","Party"},
    {"Starfield","Scenes"},
    {"Countdown Timer","Timers"},
    {"Pentagram","Geometry"},{"DNA Helix","Complex"},{"Yin Yang","Symbols"},
    {"Random Points","Scenes"},
    {"Three Circles","Geometry"},{"Point Spread","Scenes"},
    {"Solar System","Scenes"},
    {"Bouncing Points","Scenes"},
    {"Shooting Stars","Scenes"},
    {"Pythagoras Tree","Scenes"},
    {"Endless Spiral","Scenes"},
    {"Endless Tunnel","Scenes"},
    {"Explosion Spread","Scenes"},
    {"Fireworks","Scenes"},
    {"Milky Way","Scenes"},
};

static const PFn DISPATCH[PRESET_COUNT] = {
    p00,p01,p02,p03,p04,p05,p06,p07,p08,p09,
    p10,p11,p12,p13,p14,
    p15,p16,p17,p18,p19,p20,p21,p22,
    p23,p25,p26,p27,p28,
    p29,p30,p31,p32,p33,
    p35,p36,p37,p38,p39,p40,p41,p42,p43,p44,p45,p46,p47,p48,p49,p50,p51,p52,
    p53,p54,p55,p56,p57,p58,
    p59,p60,p61,p63,p101,
    p86,p88,
    p90,
    p100,

    p103,p104,p105,
    p106,
    p107,p108,
    p_solar,
    p_bouncing,
    p_shooting,
    p_pythagoras_tree,
    p_endless_spiral,
    p_endless_tunnel,
    p_explosion,
    p_fireworks,
    p_milkyway,
};

// ─── STATIC-PRESET CACHE (Phase 2) ───────────────────────────
// A handful of presets produce identical raw geometry every frame: their
// DISPATCH function ignores `phase` and reads no wall-clock/random state, so
// for a fixed (idx, speed, size_val) the DISPATCH[idx]() + closing-blank
// output is byte-identical frame to frame. Re-running the full generator +
// point optimizer for them every frame is pure waste. This caches that raw
// output and memcpy's it back on a hit.
//
// IMPORTANT: only the DISPATCH[idx]() call is cached. The per-frame seam
// bridge, and all downstream stages in pattern_engine.cpp (rotation, color
// animation, scale, kaleidoscope, mirror), still run on the copied points --
// so a "static" preset can still be rotated/recolored live; only its base
// geometry is reused.
//
// isStaticPreset(): the set was established by source analysis -- these are
// the presets whose generator body references neither `ph` nor millis()/random
// state. Kept as an explicit allow-list (not a heuristic) so a future edit
// that makes one of them phase-dependent doesn't silently serve stale frames;
// if that happens, remove it here.
static inline bool isStaticPreset(uint8_t idx) {
    switch (idx) {
        case 10:  // Cross +
        case 11:  // X Shape
        case 12:  // Grid 3x3
        case 30:  // Static Cube
        case 82:  // Three Circles
        case 83:  // Point Spread
            return true;
        default:
            return false;
    }
}

// Single-slot cache: only the currently-selected preset benefits, and the
// active preset changes rarely relative to frame rate, so one slot captures
// essentially all the win without PRESET_COUNT * PATTERN_POINTS_MAX of PSRAM.
static LaserPoint* s_cacheBuf   = nullptr;   // PSRAM, PATTERN_POINTS_MAX points
static size_t      s_cacheCount = 0;
static uint8_t     s_cacheIdx   = 0xFF;      // 0xFF = empty
static uint8_t     s_cacheSpeed = 0;
static uint8_t     s_cacheSize  = 0;
static uint32_t    s_cacheGen   = 0xFFFFFFFF;

// Lazily allocate the cache buffer in PSRAM. Returns false if PSRAM is
// exhausted -- caller then falls back to the uncached path (correct, just
// slower), so a failed alloc never breaks rendering.
static bool ensureCacheBuf() {
    if (s_cacheBuf) return true;
    s_cacheBuf = (LaserPoint*)ps_malloc(PATTERN_POINTS_MAX * sizeof(LaserPoint));
    if (s_cacheBuf) memreg::track("Preset Cache Buffer", PATTERN_POINTS_MAX * sizeof(LaserPoint), true);
    return s_cacheBuf != nullptr;
}

// Runs DISPATCH[idx]() with a cache in front for static presets. For dynamic
// presets (or when PSRAM is unavailable) this is a direct pass-through, so
// their behaviour is byte-identical to the pre-cache code path.
static size_t dispatchCached(uint8_t idx, LaserPoint* out, size_t max_pts,
                             uint32_t safe_phase, uint8_t speed, uint8_t size_val) {
    if (!isStaticPreset(idx) || !ensureCacheBuf()) {
        return DISPATCH[idx](out, max_pts, safe_phase, speed, size_val);
    }

    const uint32_t gen = gPatternCacheGen;
    bool hit = (s_cacheIdx == idx) && (s_cacheSpeed == speed) &&
               (s_cacheSize == size_val) && (s_cacheGen == gen) &&
               (s_cacheCount > 0) && (s_cacheCount <= max_pts);
    if (hit) {
        memcpy(out, s_cacheBuf, s_cacheCount * sizeof(LaserPoint));
        return s_cacheCount;
    }

    // Miss: generate into the caller's buffer, then snapshot into the cache.
    size_t n = DISPATCH[idx](out, max_pts, safe_phase, speed, size_val);
    if (n > 0 && n <= PATTERN_POINTS_MAX) {
        memcpy(s_cacheBuf, out, n * sizeof(LaserPoint));
        s_cacheCount = n;
        s_cacheIdx   = idx;
        s_cacheSpeed = speed;
        s_cacheSize  = size_val;
        s_cacheGen   = gen;
    } else {
        s_cacheIdx = 0xFF;   // don't cache empty/oversized results
    }
    return n;
}

Preset presetFromIndex(int raw) {
    return (raw >= 0 && raw < PRESET_COUNT) ? static_cast<Preset>(raw) : Preset::None;
}

size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t speed, uint8_t size_val) {
    if (idx >= PRESET_COUNT || !out) return 0;
    const uint32_t safe_phase = phase % 0xFFFFFF;
    size_t n = dispatchCached(idx, out, max_pts, safe_phase, speed, size_val);

    // Geometric closing blank -- skipped for continuous-sweep (#2) presets
    // whose open boundary is intentional (bridged by cross-frame continuity).
    if (!isContinuous(idx) && n > 0 && n < max_pts) {
        const LaserPoint& last = out[n - 1];
        const LaserPoint& first = out[0];
        float _cdx = last.x - first.x, _cdy = last.y - first.y;
        bool already_closed = (_cdx*_cdx + _cdy*_cdy) < 100.f;
        if (!already_closed) {
            LaserPoint cl = first; cl.blank = 1;
            for (int k = 0; k < 40 && n < max_pts; k++) out[n++] = cl;
        }
    }

    // Cross-frame seam bridge (#4) for every non-continuous preset. Presets
    // that reset their sweep to t=0 each frame (Lissajous, matrix-rotated,
    // harmonic, multi-subpath) restart at a rotated position while the galvo
    // physically rests at the PREVIOUS frame's end -- a real discontinuity.
    // Eased distance-scaled blank move (emitBlankTo, Pillar 2) settles it in
    // the dark. No up-front reservation: room made by evicting trailing
    // (overdense) points only when needed. #2 presets exempt (seam=0).
    // At speed 0 the jump is ~0 -> below threshold -> skipped (no cost).
    static float sLastX[PRESET_COUNT] = {0};
    static float sLastY[PRESET_COUNT] = {0};
    static bool  sHas[PRESET_COUNT]   = {false};
    static constexpr float kSeamThresh2 = 100.f; // (10 units)^2, above noise only
    if (!isContinuous(idx) && n > 0) {
        size_t f = 0; while (f < n && out[f].blank) f++;
        if (f < n && sHas[idx]) {
            float dx = (float)out[f].x - sLastX[idx];
            float dy = (float)out[f].y - sLastY[idx];
            if (dx*dx + dy*dy > kSeamThresh2) {
                const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
                LaserPoint br[130];
                br[0] = LaserPoint((int16_t)sLastX[idx], (int16_t)sLastY[idx], 0,0,0,1);
                size_t bn = 1;
                optimizer::emitBlankTo(br, bn, 130, (float)out[f].x, (float)out[f].y, cfg);
                size_t jc = bn - 1;
                if (jc > 0 && max_pts > jc) {
                    if (n + jc > max_pts) n = max_pts - jc;
                    memmove(out + jc, out, n * sizeof(LaserPoint));
                    memcpy(out, br + 1, jc * sizeof(LaserPoint));
                    n += jc;
                }
            }
        }
    }
    if (n > 0) { sLastX[idx] = (float)out[n-1].x; sLastY[idx] = (float)out[n-1].y; sHas[idx] = true; }
    return n;
}

} // namespace presets