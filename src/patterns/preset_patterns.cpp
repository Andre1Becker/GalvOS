#include "preset_patterns.h"
#include "countdown_timer.h"
#include "point_optimizer.h"
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
        case 0: case 22: case 23: case 24: case 26: case 27: case 100: return true;
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

// sinewave() -- parametric continuous curve, no discrete vertices.
// Not migrated to optimizer (see design doc Section 9.2).
static size_t sinewave(LaserPoint*o,size_t mx,float A,float f,float ph_off,float sc,uint8_t r,uint8_t g,uint8_t b,int N=120){
    size_t n=0;
    const float effA=A*gLivePreset.wave_amp, effF=f*gLivePreset.wave_freq;
    float sx=-1.f*sc, sy=effA*sinf(effF*-1.f*PI2+ph_off)*sc;
    for(int k=0;k<40 && n<mx;k++) ap(o,n,mx,sx,sy,0,0,0,1);
    for(int i=0;i<=N;i++){float x=L(-1.f,1.f,i/float(N));ap(o,n,mx,x*sc,effA*sinf(effF*x*PI2+ph_off)*sc,r,g,b,0);}
    return n;
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
    size_t n=0;float sc=SC*ssc(sz)*.9f;int N=adaptN(sz,360,60,900);
    for(int i=0;i<=N;i++){float t=csweep(ph,sp,i,N);ap(o,n,m,cosf(t)*sc,sinf(t)*sc,255,255,255,0);}
    return n;
}
static size_t p01(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,4,SC*ssc(sz)*.9f,aang(ph,sp),255,240,0);}
static size_t p02(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,3,SC*ssc(sz)*.9f,aang(ph,sp),0,255,240);}
static size_t p03(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,5,SC*ssc(sz)*.9f,aang(ph,sp),255,128,0);}
static size_t p04(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,6,SC*ssc(sz)*.9f,aang(ph,sp),0,255,128);}
static size_t p05(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,8,SC*ssc(sz)*.9f,aang(ph,sp),128,0,255);}
static size_t p06(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,4,s,s*.36f,aang(ph,sp),255,0,0);}
static size_t p07(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,5,s,s*.36f,aang(ph,sp),255,220,0);}
static size_t p08(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,6,s,s*.36f,aang(ph,sp),0,255,0);}
static size_t p09(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,8,s,s*.36f,aang(ph,sp),0,128,255);}

// ─── LINES 10-14 ────────────────────────────────────────────
// These use line() which is now optimizer-backed. Grid p12 benefits
// most: 8 line segments share the flicker budget via one optimize() call
// each, preventing the fixed-cost 40-blank overhead from dominating.
static size_t p10(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f;line(o,n,m,-s,0,s,0,255,60,60,50);line(o,n,m,0,-s,0,s,60,255,60,50);return n;}
static size_t p11(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.65f;line(o,n,m,-s,-s,s,s,0,255,255,50);line(o,n,m,s,-s,-s,s,255,0,255,50);return n;}
static size_t p12(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f,st=s*2.f/3.f;for(int i=0;i<=3;i++){float x=-s+i*st;line(o,n,m,x,-s,x,s,0,200,200,20);}for(int i=0;i<=3;i++){float y=-s+i*st;line(o,n,m,-s,y,s,y,0,200,200,20);}return n;}
static size_t p13(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f,off=(aang(ph,sp)/PI2*2.f-1.f)*s;for(int i=0;i<=60;i++)ap(o,n,m,L(-s,s,i/60.f),off,255,128,0,i==0?1:0);return n;}
static size_t p14(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f,a=aang(ph,sp);for(int i=0;i<=60;i++)ap(o,n,m,L(-s,s,i/60.f)*cosf(a),L(-s,s,i/60.f)*sinf(a),255,0,128,i==0?1:0);return n;}

// ─── spirals 15-22 ──────────────────────────────────────────
// Parametric curves — not migrated (no discrete vertices).
static size_t p15(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);const int N=adaptN(sz,200,30,400);for(int i=0;i<N;i++){float t=(float)i/N,a=t*PI2*3.5f+off,r=t*sc;ap(o,n,m,cosf(a)*r,sinf(a)*r,(uint8_t)(t*255),(uint8_t)((1-t)*255),128,i==0?1:0);}return n;}
// Point counts raised (2.5-3x, scaled to combined frequency = curve
// complexity) -- galvo (15kpps rated, driven at 30kpps) needs tighter
// per-step spacing on high-curvature Lissajous paths to close cleanly.
static size_t p16(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=500;i++){float t=PI2*i/500.f;ap(o,n,m,cosf(t+off)*sc,sinf(2*t+M_PI/4.f)*sc,0,200,255,i==0?1:0);}return n;}
static size_t p17(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=700;i++){float t=PI2*i/700.f;ap(o,n,m,cosf(2*t+off)*sc,sinf(3*t+M_PI/4.f)*sc,0,180,255,i==0?1:0);}return n;}
static size_t p18(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=900;i++){float t=PI2*i/900.f;ap(o,n,m,cosf(3*t+off)*sc,sinf(4*t+M_PI/3.f)*sc,100,200,255,i==0?1:0);}return n;}
static size_t p19(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=1100;i++){float t=PI2*i/1100.f;ap(o,n,m,cosf(3*t+off)*sc,sinf(5*t+M_PI/6.f)*sc,150,100,255,i==0?1:0);}return n;}
static size_t p20(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=1300;i++){float t=PI2*i/1300.f;ap(o,n,m,cosf(5*t+off)*sc,sinf(6*t+PI2/5.f)*sc,255,150,0,i==0?1:0);}return n;}
static size_t p21(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<150;i++){float t=i/150.f,a=t*PI2*3.f+off,r=t*sc;ap(o,n,m,cosf(a)*r,sinf(a)*r,255,80,0,i==0?1:0);}for(int i=0;i<150;i++){float t=i/150.f,a=t*PI2*3.f+off+M_PI,r=t*sc;ap(o,n,m,cosf(a)*r,sinf(a)*r,0,80,255,i==0?1:0);}return n;}
static size_t p22(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f;const int N=200;for(int i=0;i<=N;i++){float t=csweep(ph,sp,i,N),r=sc*cosf(3*t);ap(o,n,m,r*cosf(t),r*sinf(t),255,100,0,0);}return n;}

// ─── Curves 23-28 ────────────────────────────────────────────
// Parametric curves — not migrated.
static size_t p23(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f;const int N=200;for(int i=0;i<=N;i++){float t=csweep(ph,sp,i,N),r=sc*cosf(4*t);ap(o,n,m,r*cosf(t),r*sinf(t),255,50,150,0);}return n;}
static size_t p24(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.45f;const int N=200;for(int i=0;i<=N;i++){float t=csweep(ph,sp,i,N),r=sc*(1.f-cosf(t));ap(o,n,m,r*cosf(t),r*sinf(t),255,0,100,0);}return n;}
static size_t p25(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.045f,a=aang(ph,sp);const int N=adaptN(sz,200,20,300);for(int i=0;i<=N;i++){float t=PI2*i/N,x=sc*16*powf(sinf(t),3),y=sc*(13*cosf(t)-5*cosf(2*t)-2*cosf(3*t)-cosf(4*t));ap(o,n,m,x*cosf(a)-y*sinf(a),x*sinf(a)+y*cosf(a),255,0,80,i==0?1:0);}return n;}
static size_t p26(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f;const int N=adaptN(sz,500,60,800);for(int i=0;i<=N;i++){float t=csweep(ph,sp,i,N),d=1+sinf(t)*sinf(t);ap(o,n,m,sc*cosf(t)/d,sc*sinf(t)*cosf(t)/d,0,200,255,0);}return n;}
static size_t p27(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f;const int N=200;for(int i=0;i<=N;i++){float t=csweep(ph,sp,i,N);ap(o,n,m,sc*powf(cosf(t),3),sc*powf(sinf(t),3),200,255,50,0);}return n;}
static size_t p28(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;const float R=3,r=1,d=2.5f,peakNorm=1.f/(R+r+d);float sc=SC*ssc(sz)*.9f*peakNorm,off=aang(ph,sp);for(int i=0;i<=800;i++){float t=PI2*i/800.f+off;ap(o,n,m,sc*((R+r)*cosf(t)-d*cosf((R+r)*t/r)),sc*((R+r)*sinf(t)-d*sinf((R+r)*t/r)),0,255,100,i==0?1:0);}return n;}

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
    return wf(o,m,V,5,E,8,aang(ph,sp),0.3f,SC*ssc(sz)*.65f,255,128,0);
}

static size_t p32(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    static const P3D V[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const int E[][2]={{0,2},{0,3},{1,2},{1,3},{0,4},{0,5},{1,4},{1,5},{2,4},{2,5},{3,4},{3,5}};
    return wf(o,m,V,6,E,12,aang(ph,sp),.35f,SC*ssc(sz)*.7f,100,255,100);
}
static size_t p33(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    static const P3D V[]={{0,1,0},{.943f,-.333f,0},{-.471f,-.333f,.816f},{-.471f,-.333f,-.816f}};
    static const int E[][2]={{0,1},{0,2},{0,3},{1,2},{1,3},{2,3}};
    return wf(o,m,V,4,E,6,aang(ph,sp,1.2f),.4f,SC*ssc(sz)*.75f,200,0,255);
}
// ─── WELLEN 35-52 ────────────────────────────────────────────
// Parametric continuous curves — not migrated to optimizer.
static size_t p35(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return sinewave(o,m,.55f,1,aang(ph,sp),SC*ssc(sz)*.9f,0,220,255);}
static size_t p36(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float A=fabsf(sinf(aang(ph,sp)))*.8f+.1f;return sinewave(o,m,A,2,0,SC*ssc(sz)*.9f,0,255,150);}
static size_t p37(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    n+=sinewave(o,m,.3f,1,t,sc,255,80,80);
    n+=sinewave(o+n,m-n,.2f,2,t*1.5f,sc,80,255,80);
    n+=sinewave(o+n,m-n,.15f,3,t*2.f,sc,80,80,255);
    return n;
}
static size_t p38(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),y=wa*(.3f*sinf(4*wf*x*M_PI+t)+.15f*sinf(8*wf*x*M_PI+t*1.7f)+.08f*sinf(16*wf*x*M_PI+t*2.3f));ap(o,n,m,x*sc,y*sc,0,100,255,i==0?1:0);}
    return n;
}
static size_t p39(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),y=wa*.45f*(sinf(5*wf*x*M_PI+t)+sinf(7*wf*x*M_PI-t))*.5f;ap(o,n,m,x*sc,y*sc,255,200,0,i==0?1:0);}
    return n;
}
static size_t p40(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),ph2=fmodf((x*2.f*wf+t/M_PI),2.f),y=wa*.6f*(ph2<1?ph2:ph2-2);ap(o,n,m,x*sc,y*sc,255,100,0,i==0?1:0);}
    return n;
}
static size_t p41(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    // True square wave: explicit horizontal plateaus joined by vertical edges.
    // Previous version sampled sign(sin) at 200 evenly-spaced x -> the galvo
    // sloped diagonally through each transition instead of snapping vertically.
    const int cycles=(int)fmaxf(1.f,roundf(3*wf));      // whole cycles across width
    const int steps=cycles*2;                           // half-periods (plateaus)
    const float amp=wa*.55f*sc,phi=fmodf(t,PI2)/PI2;    // phase shifts plateaus
    float px=-sc,py=((sinf(phi*PI2)>0)?1.f:-1.f)*amp;
    ap(o,n,m,px,py,200,100,255,1);                      // start
    for(int s=0;s<=steps;s++){
        float x=L(-1.f,1.f,(float)s/steps)*sc;
        float lvl=(sinf(((float)s/steps+phi)*PI2*cycles)>0?1.f:-1.f)*amp;
        if(lvl!=py){ap(o,n,m,px,lvl,200,100,255,0);py=lvl;} // vertical edge
        ap(o,n,m,x,py,200,100,255,0);                   // horizontal plateau
        px=x;
    }
    return n;
}
static size_t p42(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),env=expf(-12*x*x),y=env*wa*sinf(8*wf*x*M_PI+t)*.8f;ap(o,n,m,x*sc,y*sc,0,255,200,i==0?1:0);}
    return n;
}
static size_t p43(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),y=.5f*wa*(sinf(10*wf*x*M_PI+t)+sinf(11*wf*x*M_PI+t));ap(o,n,m,x*sc,y*sc,255,150,0,i==0?1:0);}
    return n;
}
static size_t p44(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int ring=1;ring<=4;ring++){float r=ring/4.f,R=r*sc;for(int i=0;i<=80;i++){float a=PI2*i/80.f,rad=R*(1+.12f*wa*sinf(8*wf*a+r*8*wf-t));ap(o,n,m,cosf(a)*rad,sinf(a)*rad,0,(uint8_t)(100+155*r),255,i==0?1:0);}}
    return n;
}
static size_t p45(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),y=.5f*wa*sinf(6*wf*x*PI2+4*sinf(wf*x*PI2*2+t));ap(o,n,m,x*sc,y*sc,0,255,255,i==0?1:0);}
    return n;
}
static size_t p46(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<200;i++){float t=i/200.f,a=t*PI2*4*wf+off,r=sc*(1-t*.8f),w=.08f*wa*sinf(a*8);ap(o,n,m,cosf(a)*(r+w*sc),sinf(a)*(r+w*sc),(uint8_t)(t*255),(uint8_t)((1-t)*200),200,i==0?1:0);}
    return n;
}
static size_t p47(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=200;i++){float u=L(-1.f,1.f,i/200.f),a=u*PI2*3*wf+t;ap(o,n,m,u*sc,wa*sinf(a)*sc*.5f,0,(uint8_t)(128+127*cosf(a)),(uint8_t)(128+127*sinf(a)),i==0?1:0);}
    return n;
}
static size_t p48(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int row=-2;row<=2;row++){float y0=row*.36f;for(int i=0;i<=60;i++){float x=L(-1.f,1.f,i/60.f),y=y0+.12f*wa*sinf(x*PI2*3*wf+t+row*.7f);ap(o,n,m,x*sc,y*sc,0,(uint8_t)(128+127*sinf(row+t)),200,i==0?1:0);}}
    return n;
}
static size_t p49(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f);float y=0;for(int k=1;k<=5;k+=2)y+=sinf(k*x*PI2*1.5f*wf+t)/k;ap(o,n,m,x*sc,y*wa*.5f*sc,100,255,100,i==0?1:0);}
    return n;
}
static size_t p50(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),decay=expf(-2*fabsf(x)),y=decay*wa*sinf(10*wf*x*M_PI+t)*(.4f+.4f*fabsf(x));ap(o,n,m,x*sc,y*sc,255,80,200,i==0?1:0);}
    return n;
}
static size_t p51(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),build=.5f+.5f*x,y=build*.5f*wa*sinf(PI2*(x*2*wf-t*.3f))*.7f;ap(o,n,m,x*sc,y*sc,0,150,255,i==0?1:0);}
    return n;
}
static size_t p52(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    const float wa=gLivePreset.wave_amp,wf=gLivePreset.wave_freq;
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f);float y=0;int ks[]={1,2,3,4,5};for(int j=0;j<5;j++)y+=sinf(ks[j]*wf*x*PI2+t*(j*.5f+.5f))*.2f/ks[j];ap(o,n,m,x*sc,y*wa*sc,255,200,50,i==0?1:0);}
    return n;
}

// ─── KOMPLEX 53-58 ───────────────────────────────────────────
// p53-p55, p58: parametric curves — not migrated.
// p53 Hypotrochoid -- R=5, r=3, d=5. Curve closes after 3 revolutions of
// the outer parameter (t in [0,6*PI]); previous version swept only
// [0,2*PI], drawing an incomplete, lopsided single lobe instead of the
// full 3-lobed flower. Also lacked peak-radius normalisation (peak
// (R-r)+d = 7), rendering far larger than sibling Complex presets.
// Both fixed, matching the p55 normalisation pattern below.
static size_t p53(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;const float R=5,r=3,d=5,peakNorm=1.f/((R-r)+d);
    float sc=SC*ssc(sz)*.9f*peakNorm,off=aang(ph,sp);
    const int N=600;
    for(int i=0;i<=N;i++){
        float t=6.f*(float)M_PI*i/N+off;
        ap(o,n,m,sc*((R-r)*cosf(t)+d*cosf((R-r)*t/r)),sc*((R-r)*sinf(t)-d*sinf((R-r)*t/r)),255,100,200,i==0?1:0);
    }
    return n;
}
static size_t p54(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.38f,off=aang(ph,sp);for(int i=0;i<=200;i++){float t=PI2*i/200.f,e=expf(cosf(t))-2*cosf(4*t)-powf(sinf(t/12.f),5);ap(o,n,m,sc*e*sinf(t+off),sc*e*cosf(t+off),255,165,0,i==0?1:0);}return n;}
// p55 Spirograph 5/3 -- hypotrochoid, fixed radii R=5, r=3. Curve closes
// after 3 revolutions of the outer parameter (t in [0,6*PI]); the previous
// [0,10*PI] sweep overshot the true period and retraced 2/3 of the curve
// a second time. d was also equal to r, which degenerates the hypotrochoid
// into a 5-cusp hypocycloid -- i.e. a pentagram, not a spirograph rosette.
// d < r now gives the proper looping rosette. Peak radius (R-r)+d is
// normalised so the figure fills the frame. off animates rotation.
static size_t p55(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float off=aang(ph,sp);
    const float R=5.f,r=3.f,d=1.5f,peakNorm=1.f/((R-r)+d);
    float sc=SC*ssc(sz)*.9f*peakNorm;
    const int N=360;
    for(int i=0;i<=N;i++){
        float t=6.f*(float)M_PI*i/N;
        float x=(R-r)*cosf(t)+d*cosf((R-r)/r*t);
        float y=(R-r)*sinf(t)-d*sinf((R-r)/r*t);
        ap(o,n,m,sc*(x*cosf(off)-y*sinf(off)),sc*(x*sinf(off)+y*cosf(off)),0,200,255,i==0?1:0);
    }
    return n;
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
    size_t n=0;float sc=SC*ssc(sz)*.9f,pulse=sc*(.5f+.5f*fabsf(sinf(aang(ph,sp,3)))),rot=aang(ph,sp,.2f);
    for(int i=0;i<=384;i++){float a=PI2*i/384.f+rot,wave=1+.15f*sinf(8*a),r2=pulse*wave;ap(o,n,m,cosf(a)*r2,sinf(a)*r2,(uint8_t)(200+55*sinf(a)),0,(uint8_t)(200+55*cosf(a)),i==0?1:0);}
    return n;
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

// p60 Chaos Bouncer -- a closed Lissajous knot. Previous version multiplied
// incommensurate frequencies (3.1, .7, 2.1, 1.3) so the path never returned
// to its start over one 2*PI sweep -> a lit jump every frame. Now uses
// integer harmonics (fx=3, fy=2) with a slowly drifting phase (dph) so the
// figure morphs but always closes: at i=0 and i=N the sample is identical.
static size_t p60(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f;
    const float dph=aang(ph,sp,.5f);          // animated relative phase
    const int N=256;
    for(int i=0;i<=N;i++){
        float t=PI2*i/N;
        float x=sinf(3.f*t+dph),y=sinf(2.f*t);
        ap(o,n,m,sc*x,sc*y,
           (uint8_t)(128+127*sinf(t*2+dph)),
           (uint8_t)(128+127*sinf(t*3+1)),
           (uint8_t)(128+127*cosf(t*1.5f)),i==0?1:0);
    }
    return n;
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
            verts[i].r=0; verts[i].g=240; verts[i].b=255;
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
            verts[i].r=255; verts[i].g=80; verts[i].b=200;
            verts[i].lift=false;
        }
        optimizer::PathSegment seg(verts,4,true);
        n += optimizer::optimize(&seg,1,o+n,m-n,liveOptimizerConfig());
    }
    // Circle via ngon
    n += ngon(o+n,m-n,32,.72f*sc,rot,100,200,255);
    return n;
}

// p62 Champagne Bubbles -- rising ring cluster. Each bubble is a closed
// 16-gon. Previous version drew all bubbles back-to-back; the per-frame
// closing blank then linked the last bubble to the first as a lit diagonal
// ("weird lines"). Fix: emit an explicit blank move to each bubble's start
// (dark travel) and a trailing blank so the closing bridge starts/ends dark.
static size_t p62(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    static const struct{float x,spd,off,r;}b[]={{-.4f,.3f,1.3f,.08f},{-.2f,.5f,2.1f,.06f},{.1f,.7f,.7f,.09f},{.3f,.4f,1.8f,.07f},{-.1f,.6f,3.2f,.05f},{.5f,.2f,2.5f,.08f},{-.5f,.8f,.4f,.07f}};
    for(auto& bl:b){
        float ph2=fmodf(t*bl.spd+bl.off,PI2),y=L(-1.f,1.f,ph2/PI2),wob=sinf(ph2*8)*.02f;
        float cx=(bl.x+wob)*sc,cy=y*sc,rr=bl.r*sc;
        ap(o,n,m,cx+rr,cy,0,0,0,1);                     // blank travel to ring start
        for(int i=0;i<=16;i++){float a=PI2*i/16.f;ap(o,n,m,cx+cosf(a)*rr,cy+sinf(a)*rr,200,220,255,0);}
    }
    if(n>0)ap(o,n,m,o[n-1].x,o[n-1].y,0,0,0,1);         // end dark
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
    size_t n=0;float sc=SC*ssc(sz)*.9f;
    float burst=fmodf(aang(ph,sp,.5f)/PI2,1.f);        // 0..1 loop
    const int NP=18;
    for(int p=0;p<NP;p++){
        float ang=PI2*p/NP+ (p*0.37f);                  // fixed launch direction
        float r=burst*(0.35f+0.65f*fmodf(p*0.191f,1.f))*sc;
        float cx=cosf(ang)*r,cy=sinf(ang)*r;
        float spin=ang*3.f+burst*PI2*2.f,pr=0.05f*sc;
        uint8_t cr=(uint8_t)(128+127*sinf(ang)),cg=(uint8_t)(128+127*sinf(ang+2.1f)),cb=(uint8_t)(128+127*sinf(ang+4.2f));
        ap(o,n,m,cx+cosf(spin)*pr,cy+sinf(spin)*pr,0,0,0,1); // blank travel
        for(int i=0;i<=4;i++){float a=spin+PI2*i/4.f;ap(o,n,m,cx+cosf(a)*pr,cy+sinf(a)*pr*.6f,cr,cg,cb,0);}
    }
    if(n>0)ap(o,n,m,o[n-1].x,o[n-1].y,0,0,0,1);
    return n;
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
    n += ngon(o+n,m-n,48,sc,0,220,220,220);
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

// ─── PARTY-SILHOUETTEN 64-89 ─────────────────────────────────
// These use line() and circ_draw() which are now optimizer-backed.
// No per-preset changes needed — migration is transparent.
static size_t p64(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Martini
    size_t n=0;float sc=SC*ssc(sz)*.9f;
    for(int i=0;i<=40;i++){float a=M_PI*i/40.f;ap(o,n,m,cosf(a)*.65f*sc,(.55f+sinf(a)*.04f)*sc,0,220,255,i==0?1:0);}
    for(int k=0;k<=16;k++)ap(o,n,m,L(.65f,0,k/16.f)*sc,L(.55f,-.12f,k/16.f)*sc,0,220,255,k==0?1:0);
    for(int k=0;k<=16;k++)ap(o,n,m,L(-.65f,0,k/16.f)*sc,L(.55f,-.12f,k/16.f)*sc,0,220,255,k==0?1:0);
    for(int k=0;k<=14;k++)ap(o,n,m,0,L(-.12f,-.72f,k/14.f)*sc,100,200,255,k==0?1:0);
    for(int k=0;k<=14;k++)ap(o,n,m,L(-.32f,.32f,k/14.f)*sc,-.72f*sc,100,200,255,k==0?1:0);
    return n;
}
static size_t p65(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Weinglas
    size_t n=0;float sc=SC*ssc(sz)*.9f;
    for(int i=0;i<=50;i++){float t=M_PI*i/50.f;ap(o,n,m,cosf(t)*.4f*sc,(.4f+sinf(t)*.4f)*sc,200,50,100,i==0?1:0);}
    for(int k=0;k<=12;k++)ap(o,n,m,L(.4f,.08f,k/12.f)*sc,L(.4f,-.25f,k/12.f)*sc,200,50,100,k==0?1:0);
    for(int k=0;k<=12;k++)ap(o,n,m,.08f*sc*(1-k/12.f),L(-.25f,-.7f,k/12.f)*sc,180,50,80,k==0?1:0);
    for(int k=0;k<=14;k++)ap(o,n,m,L(-.3f,.3f,k/14.f)*sc,-.7f*sc,180,50,80,k==0?1:0);
    for(int k=0;k<=12;k++)ap(o,n,m,L(0,-.08f,k/12.f)*sc,L(-.7f,-.25f,k/12.f)*sc,180,50,80,k==0?1:0);
    for(int k=0;k<=12;k++)ap(o,n,m,L(-.08f,-.4f,k/12.f)*sc,L(-.25f,.4f,k/12.f)*sc,200,50,100,k==0?1:0);
    return n;
}
static size_t p66(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Champagner
    size_t n=0;float sc=SC*ssc(sz)*.9f;
    auto lft=[](float y)->float{if(y<-.4f)return-.12f-.15f*(y+.7f);if(y<.2f)return-.12f+.08f*(y+.4f)/.6f;return-.12f+.08f+.12f*(y-.2f)/.6f;};
    for(int i=0;i<=40;i++){float y=L(-.7f,.8f,i/40.f);ap(o,n,m,lft(y)*sc,y*sc,200,200,80,i==0?1:0);}
    for(int i=0;i<=40;i++){float y=L(.8f,-.7f,i/40.f);ap(o,n,m,-lft(y)*sc,y*sc,200,200,80,i==0?1:0);}
    for(int k=0;k<=10;k++)ap(o,n,m,L(-.27f,.27f,k/10.f)*sc,-.7f*sc,200,200,80,k==0?1:0);
    return n;
}
static size_t p67(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Tropical Cocktail
    size_t n=0;float sc=SC*ssc(sz)*.9f,wob=sinf(aang(ph,sp))*.05f;
    for(int i=0;i<=60;i++){float a=M_PI+M_PI*i/60.f;ap(o,n,m,cosf(a)*.45f*sc,(sinf(a)*.45f-.2f)*sc,0,200,255,i==0?1:0);}
    for(int i=0;i<=20;i++){float a=-M_PI*i/20.f;ap(o,n,m,cosf(a)*.45f*sc,(sinf(a)*.04f-.2f+.45f)*sc,0,200,255,i==0?1:0);}
    for(int k=0;k<=14;k++)ap(o,n,m,(.1f+k*.04f+wob)*sc,L(.25f,.85f,k/14.f)*sc,255,200,0,k==0?1:0);
    for(int i=0;i<=20;i++){float a=M_PI+M_PI*.6f*(i/20.f-.5f);ap(o,n,m,(.66f+wob+cosf(a)*.18f)*sc,(.85f+sinf(a)*.12f)*sc,255,80,150,i==0?1:0);}
    return n;
}
static size_t p68(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Palme
    size_t n=0;float sc=SC*ssc(sz)*.9f,sw=sinf(aang(ph,sp,.5f))*.04f;
    for(int i=0;i<=20;i++){float y=L(-.8f,0,i/20.f);ap(o,n,m,(-.06f+y*.1f+sw*y)*sc,y*sc,160,100,50,i==0?1:0);}
    float fronds[][4]={{.0f,.9f,-.45f,.5f},{.3f,.8f,.55f,.4f},{-.3f,.8f,-.55f,.4f},{.5f,.5f,.85f,.1f},{-.5f,.5f,-.85f,.1f}};
    for(auto&f:fronds){for(int k=0;k<=14;k++){float t=k/14.f,bx=L(f[0],f[2],t)*sc,by=(L(f[1],f[3],t)+sinf(t*M_PI)*.15f)*sc;ap(o,n,m,bx+sw*sc*2,by,0,(uint8_t)(150+105*t),0,k==0?1:0);}}
    return n;
}
static size_t p69(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Flamingo
    size_t n=0;float sc=SC*ssc(sz)*.9f,bob=sinf(aang(ph,sp,.5f))*.03f;
    for(int i=0;i<=30;i++){float a=PI2*i/30.f;ap(o,n,m,(cosf(a)*.28f+.05f)*sc,(sinf(a)*.2f+.15f+bob)*sc,255,120,180,i==0?1:0);}
    float nx[]={.15f,.2f,.1f,.05f,.15f},ny[]={.3f,.45f,.6f,.7f,.8f};
    for(int i=1;i<5;i++){for(int k=1;k<=8;k++)ap(o,n,m,L(nx[i-1],nx[i],k/8.f)*sc,L(ny[i-1],ny[i],k/8.f)*sc,255,150,200,k==1?1:0);}
    for(int i=0;i<=16;i++){float a=PI2*i/16.f;ap(o,n,m,(cosf(a)*.06f+.15f)*sc,(sinf(a)*.05f+.83f+bob)*sc,255,120,180,i==0?1:0);}
    for(int k=0;k<=6;k++)ap(o,n,m,L(.21f,.38f,k/6.f)*sc,(L(.83f,.78f,k/6.f)+bob)*sc,255,165,0,k==0?1:0);
    for(int k=0;k<=12;k++)ap(o,n,m,L(.1f,.08f,k/12.f)*sc,L(.02f,-.8f,k/12.f)*sc,255,150,180,k==0?1:0);
    for(int k=0;k<=12;k++)ap(o,n,m,L(-.02f,-.04f,k/12.f)*sc,L(.02f,-.8f,k/12.f)*sc,255,150,180,k==0?1:0);
    return n;
}
static size_t p70(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Tropischer Fisch
    size_t n=0;float sc=SC*ssc(sz)*.9f,sw=sinf(aang(ph,sp,.8f))*.08f;
    for(int i=0;i<=50;i++){float a=PI2*i/50.f;ap(o,n,m,(cosf(a)*.45f+sw)*sc,sinf(a)*.3f*sc,255,150,0,i==0?1:0);}
    for(int k=0;k<=10;k++)ap(o,n,m,L(-.45f+sw,-.75f+sw,k/10.f)*sc,L(0,.35f,k/10.f)*sc,255,100,0,k==0?1:0);
    for(int k=0;k<=10;k++)ap(o,n,m,L(-.75f+sw,-.45f+sw,k/10.f)*sc,L(.35f,-.35f,k/10.f)*sc,255,100,0,k==0?1:0);
    for(int k=0;k<=10;k++)ap(o,n,m,L(-.45f+sw,-.75f+sw,k/10.f)*sc,L(0,-.35f,k/10.f)*sc,255,100,0,k==0?1:0);
    for(int i=0;i<=12;i++){float a=PI2*i/12.f;ap(o,n,m,(cosf(a)*.04f+.25f+sw)*sc,(sinf(a)*.04f+.08f)*sc,0,0,0,i==0?1:0);}
    return n;
}
static size_t p71(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Wasser-Splash
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<8;i++){float a=PI2*i/8.f,tipH=.5f+.25f*sinf(i*1.3f+t);ap(o,n,m,cosf(a-.15f)*.35f*sc,sinf(a-.15f)*.35f*sc,0,150,255,1);for(int k=1;k<=8;k++){float ang=L(a-.15f,a,k/8.f),rad=L(.35f,.35f+tipH,k/8.f);ap(o,n,m,cosf(ang)*rad*sc,sinf(ang)*rad*sc,0,(uint8_t)(150+100*(k/8.f)),255,0);}for(int k=0;k<=8;k++){float ang=L(a,a+.15f,k/8.f),rad=L(.35f+tipH,.35f,k/8.f);ap(o,n,m,cosf(ang)*rad*sc,sinf(ang)*rad*sc,0,220,255,0);}}
    float dy=.1f*fabsf(sinf(t*1.5f));for(int i=0;i<=20;i++){float a=PI2*i/20.f;ap(o,n,m,cosf(a)*.12f*sc,(sinf(a)*.12f+dy)*sc,100,220,255,i==0?1:0);}
    return n;
}
static size_t p72(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Pool-Wellen
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int ring=1;ring<=4;ring++){float r=(.15f+ring*.18f)*sc,amp=.03f*expf(-ring*.3f)*sc;for(int i=0;i<=60;i++){float a=PI2*i/60.f,wave=amp*sinf(8*a+ring*.8f-t);ap(o,n,m,cosf(a)*(r+wave),sinf(a)*(r+wave)*.3f,0,(uint8_t)(180-ring*30),255,i==0?1:0);}}
    for(int i=0;i<=80;i++){float x=L(-1.f,1.f,i/80.f),y=-.55f+.05f*sinf(x*PI2*3+t)+.02f*sinf(x*PI2*7+t*1.7f);ap(o,n,m,x*sc,y*sc,0,100,200,i==0?1:0);}
    return n;
}
static size_t p73(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Tropische Sonne
    size_t n=0;float sc=SC*ssc(sz)*.9f,rot=aang(ph,sp,.3f);
    n += ngon(o+n,m-n,32,.35f*sc,0,255,220,0);
    for(int i=0;i<12;i++){float a=PI2*i/12.f+rot,len=.25f+.1f*sinf(i*2.3f+rot*2);line(o,n,m,cosf(a)*.38f*sc,sinf(a)*.38f*sc,cosf(a)*(.38f+len)*sc,sinf(a)*(.38f+len)*sc,255,(uint8_t)(200-len*100/0.35f),0);}
    return n;
}
static size_t p74(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Ananas
    size_t n=0;float sc=SC*ssc(sz)*.9f;
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,cosf(a)*.32f*sc,(sinf(a)*.45f-.15f)*sc,255,180,0,i==0?1:0);}
    for(int r=-3;r<=3;r++)for(int c=-2;c<=2;c++){float cx=c*.16f,cy=r*.2f-.15f;if(cx*cx/(.28f*.28f)+(cy+.15f)*(cy+.15f)/(.45f*.45f)<=.9f){ap(o,n,m,cx*sc,(cy+.04f)*sc,200,130,0,1);ap(o,n,m,(cx+.08f)*sc,cy*sc,200,130,0,0);ap(o,n,m,cx*sc,(cy-.04f)*sc,200,130,0,0);ap(o,n,m,(cx-.08f)*sc,cy*sc,200,130,0,0);ap(o,n,m,cx*sc,(cy+.04f)*sc,200,130,0,0);}}
    float lv[][2]={{0,.45f},{.15f,.65f},{-.15f,.65f},{.28f,.55f},{-.28f,.55f}};
    for(auto&lf:lv){for(int k=0;k<=8;k++)ap(o,n,m,L(0,lf[0],k/8.f)*sc,L(.3f,lf[1],k/8.f)*sc,0,180,0,k==0?1:0);}
    return n;
}
static size_t p75(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Musiknote
    size_t n=0;float sc=SC*ssc(sz)*.9f,bob=sinf(aang(ph,sp))*.05f;
    for(int i=0;i<=30;i++){float a=PI2*i/30.f;ap(o,n,m,(cosf(a)*.17f+sinf(a)*.05f-.1f)*sc,(sinf(a)*.13f+bob-.55f)*sc,255,255,255,i==0?1:0);}
    for(int k=0;k<=20;k++)ap(o,n,m,.07f*sc,L(-.55f+bob,.5f+bob,k/20.f)*sc,255,255,255,k==0?1:0);
    for(int k=0;k<=12;k++){float t=k/12.f;ap(o,n,m,(.07f+t*.35f)*sc,(.5f-t*.4f+bob)*sc,255,255,255,k==0?1:0);}
    for(int k=0;k<=12;k++){float t=k/12.f;ap(o,n,m,(.07f+t*.28f)*sc,(.3f-t*.35f+bob)*sc,255,255,200,k==0?1:0);}
    return n;
}
static size_t p76(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Ballon
    size_t n=0;float sc=SC*ssc(sz)*.9f,sw=sinf(aang(ph,sp,.6f))*.06f;
    for(int i=0;i<=50;i++){float a=PI2*i/50.f-M_PI/2,r=a>0?.4f:.35f;ap(o,n,m,(cosf(a)*r+sw)*sc,(sinf(a)*r*.55f+.2f)*sc,255,80,150,i==0?1:0);}
    for(int i=0;i<=10;i++){float a=PI2*i/10.f;ap(o,n,m,(cosf(a)*.04f+sw)*sc,(sinf(a)*.04f-.18f)*sc,255,50,120,i==0?1:0);}
    for(int k=0;k<=20;k++){float t=k/20.f;ap(o,n,m,(sw+sinf(t*M_PI*3)*.04f)*sc,L(-.22f,-.85f,t)*sc,255,150,200,k==0?1:0);}
    return n;
}
static size_t p77(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Krone
    size_t n=0;float sc=SC*ssc(sz)*.9f,pulse=1+.05f*sinf(aang(ph,sp,2));
    for(int k=0;k<=30;k++)ap(o,n,m,L(-.65f,.65f,k/30.f)*sc,-.35f*sc*pulse,255,200,0,k==0?1:0);
    float tips[]={-.65f,-.325f,0,.325f,.65f},heights[]={.05f,.35f,.55f,.35f,.05f};
    for(int i=0;i<5;i++){ap(o,n,m,tips[i]*sc,-.35f*sc*pulse,255,200,0,1);for(int k=0;k<=10;k++)ap(o,n,m,tips[i]*sc,L(-.35f,.35f+heights[i],k/10.f)*sc*pulse,255,220,0,0);for(int k=0;k<=10;k++)ap(o,n,m,tips[i]*sc,L(.35f+heights[i],-.35f,k/10.f)*sc*pulse,255,200,0,0);}
    float gems[][2]={{-.45f,-.15f},{0,-.05f},{.45f,-.15f}};
    for(auto&g:gems){for(int i=0;i<=12;i++){float a=PI2*i/12.f;ap(o,n,m,(cosf(a)*.05f+g[0])*sc,(sinf(a)*.05f+g[1])*sc,255,100,200,i==0?1:0);}}
    return n;
}
static size_t p78(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Diamant
    size_t n=0;float sc=SC*ssc(sz)*.9f*.75f,rot=aang(ph,sp,.2f);
    const uint8_t R=180,G=220,B=255;
    auto rp=[&](float x,float y,uint8_t bl){ap(o,n,m,(x*cosf(rot)-y*sinf(rot))*sc,(x*sinf(rot)+y*cosf(rot))*sc,R,G,B,bl);};
    // Outline: table (top flat) + crown girdle + pavilion to culet, closed loop.
    static const float out[][2]={{-.3f,.8f},{.3f,.8f},{.55f,.25f},{.55f,-.1f},{0,-.9f},{-.55f,-.1f},{-.55f,.25f},{-.3f,.8f}};
    for(size_t i=0;i<8;i++)rp(out[i][0],out[i][1],i==0?1:0);
    // Facet lines: each is a blank-jumped 2-point segment.
    static const float fac[][4]={
        {-.3f,.8f, -.2f,.25f},{.3f,.8f, .2f,.25f},   // table corners down to girdle
        {-.55f,.25f, .55f,.25f},                     // girdle line
        {-.2f,.25f, .2f,.25f},                       // table bottom edge
        {-.55f,-.1f, 0,-.9f},{.55f,-.1f, 0,-.9f},    // pavilion edges to culet
        {-.2f,.25f, 0,-.9f},{.2f,.25f, 0,-.9f}};     // inner pavilion facets
    for(auto&fseg:fac){rp(fseg[0],fseg[1],1);rp(fseg[2],fseg[3],0);}
    if(n>0)ap(o,n,m,o[n-1].x,o[n-1].y,0,0,0,1);
    return n;
}
static size_t p79(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Cocktail-Schirm
    size_t n=0;float sc=SC*ssc(sz)*.9f,tilt=sinf(aang(ph,sp,.4f))*.15f;
    for(int i=0;i<=60;i++){float a=M_PI*i/60.f;ap(o,n,m,(cosf(a)*.7f+tilt)*sc,(sinf(a)*.4f+.2f)*sc,255,80,150,i==0?1:0);}
    for(int s=0;s<=6;s++){float a=M_PI*s/6.f;for(int k=0;k<=10;k++)ap(o,n,m,L(tilt,(cosf(a)*.7f+tilt),k/10.f)*sc,L(.2f,(sinf(a)*.4f+.2f),k/10.f)*sc,255,150,200,k==0?1:0);}
    for(int k=0;k<=20;k++)ap(o,n,m,tilt*sc,L(.2f,-.8f,k/20.f)*sc,200,150,100,k==0?1:0);
    return n;
}
static size_t p80(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Wasser-Tropfen
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp),pulse=1+.05f*sinf(t*2);
    for(int i=0;i<=60;i++){float a=PI2*i/60.f-M_PI/2,r=.4f*(1-sinf(a))*.5f+.25f;ap(o,n,m,cosf(a)*r*sc*pulse,sinf(a)*r*sc*pulse+.1f*sc,0,180,255,i==0?1:0);}
    for(int i=0;i<=16;i++){float a=PI2*i/16.f;ap(o,n,m,(cosf(a)*.1f-.08f)*sc,(sinf(a)*.08f+.25f)*sc,150,230,255,i==0?1:0);}
    return n;
}
static size_t p81(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Champagner-Blasen
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    float b[][4]={{-.4f,.3f,1.3f,.08f},{-.2f,.5f,2.1f,.06f},{.1f,.7f,.7f,.09f},{.3f,.4f,1.8f,.07f},{-.1f,.6f,3.2f,.05f},{.5f,.2f,2.5f,.08f},{-.5f,.8f,.4f,.07f}};
    for(auto&bl:b){float ph2=fmodf(t*bl[1]+bl[2],PI2),y=L(-1.f,1.f,ph2/PI2),wob=sinf(ph2*8)*.02f;for(int i=0;i<=16;i++){float a=PI2*i/16.f;ap(o,n,m,(cosf(a)*bl[3]+bl[0]+wob)*sc,(sinf(a)*bl[3]+y)*sc,200,220,255,i==0?1:0);}}
    return n;
}
static size_t p82(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Konfetti
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    // {startAngle, radiusFrac, phaseOffset}. Previously the start angle field
    // was misused as an angular *speed* (fmod(t*angle+off)), so pieces spun
    // in orbits instead of drifting. Now pieces fall with a gentle sway and
    // wrap vertically; each quad is blank-jumped to.
    float s[][3]={{.3f,.6f,1.1f},{-.4f,.7f,2.3f},{.7f,.3f,3.5f},{-.6f,.4f,4.7f},{.2f,.8f,.8f},{-.3f,.5f,5.2f},{.5f,.6f,1.8f},{-.5f,.3f,2.9f},{.4f,.9f,4.1f},{-.2f,.7f,3.3f},{.6f,.2f,.5f},{-.7f,.6f,5.8f},{0,.9f,1.4f},{.8f,.4f,2.1f},{-.4f,.8f,3.8f}};
    for(auto&bl:s){
        float fall=fmodf(t*.3f+bl[2],PI2)/PI2;          // 0..1 fall progress
        float cx=(bl[0]+.06f*sinf(t+bl[2]*3.f))*sc;
        float cy=L(1.f,-1.f,fall)*sc*bl[1];
        float spin=t*2.f+bl[2];
        uint8_t cr=(uint8_t)(128+127*sinf(bl[2])),cg=(uint8_t)(128+127*sinf(bl[2]+2.1f)),cb=(uint8_t)(128+127*sinf(bl[2]+4.2f));
        ap(o,n,m,cx,cy,0,0,0,1);                        // blank travel
        for(int i=0;i<=4;i++){float a=spin+PI2*i/4.f;ap(o,n,m,cx+cosf(a)*.05f*sc,cy+sinf(a)*.035f*sc,cr,cg,cb,0);}
    }
    if(n>0)ap(o,n,m,o[n-1].x,o[n-1].y,0,0,0,1);
    return n;
}
static size_t p83(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return p63(o,m,ph,sp,sz);} // Disco-Ball = p63
static size_t p84(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Sonnenuntergang
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=40;i++){float a=M_PI*i/40.f;ap(o,n,m,cosf(a)*.5f*sc,(sinf(a)*.5f-.3f)*sc,255,(uint8_t)(100+80*sinf(t)),0,i==0?1:0);}
    for(int i=0;i<=80;i++){float x=L(-1.f,1.f,i/80.f),y=-.5f+.04f*sinf(x*PI2*4+t);ap(o,n,m,x*sc,y*sc,0,100,200,i==0?1:0);}
    return n;
}
static size_t p85(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Seestern
    float s=SC*ssc(sz)*.9f;return star(o,m,5,s,s*.35f,aang(ph,sp,.3f),255,150,0);
}
static size_t p86(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Hibiskus
    size_t n=0;float sc=SC*ssc(sz)*.9f,rot=aang(ph,sp,.2f);
    for(int p=0;p<5;p++){float base=PI2*p/5.f+rot;for(int i=0;i<=30;i++){float t=i/30.f,spread=sinf(t*M_PI),a=base+spread*.4f,r=L(.15f,.65f,t);ap(o,n,m,cosf(a)*r*sc,sinf(a)*r*sc,255,(uint8_t)(50+t*100),(uint8_t)(100-t*100),i==0?1:0);}for(int i=30;i>=0;i--){float t=i/30.f,spread=sinf(t*M_PI),a=base-spread*.4f,r=L(.15f,.65f,t);ap(o,n,m,cosf(a)*r*sc,sinf(a)*r*sc,255,(uint8_t)(50+t*100),0,0);}}
    for(int i=0;i<=12;i++){float a=PI2*i/12.f+rot*2;ap(o,n,m,cosf(a)*.12f*sc,sinf(a)*.12f*sc,255,255,0,i==0?1:0);}
    return n;
}
static size_t p87(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Kokospalme
    size_t n=0;float sc=SC*ssc(sz)*.9f,sw=sinf(aang(ph,sp,.4f))*.03f;
    for(int k=0;k<=20;k++){float t=k/20.f;ap(o,n,m,sw*t*sc,L(-.8f,.1f,t)*sc,160,100,50,k==0?1:0);}
    float fronds[][2]={{.6f,.6f},{-.6f,.6f},{0,.7f}};
    for(auto&f:fronds){for(int k=0;k<=16;k++)ap(o,n,m,(sw+L(0,f[0],k/16.f))*sc,L(.1f,f[1],k/16.f)*sc,0,180,0,k==0?1:0);}
    float ccs[][2]={{-.2f,.05f},{.1f,.1f},{.25f,-.05f}};
    for(auto&c:ccs){for(int i=0;i<=16;i++){float a=PI2*i/16.f;ap(o,n,m,(cosf(a)*.07f+c[0]+sw)*sc,(sinf(a)*.07f+c[1])*sc,100,70,30,i==0?1:0);}}
    return n;
}
static size_t p88(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Starburst (Kombi)
    size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    for(int i=0;i<24;i++){float a=PI2*i/24.f+off,inner=sc*.3f,outer=sc*(.7f+.3f*sinf(i*.8f));line(o,n,m,cosf(a)*inner,sinf(a)*inner,cosf(a)*outer,sinf(a)*outer,(uint8_t)(128+127*sinf(a)),(uint8_t)(128+127*cosf(a)),255,8);}
    return n;
}
static size_t p89(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Party-Finale / Lorenz
    size_t n=0;float sc=SC*ssc(sz)*.04f,t=aang(ph,sp)*10;
    float x=.1f,y=0,z=0,dt=.005f;
    for(int i=0;i<350;i++){float dx=10*(y-x),dy=x*(28-z)-y,dz=x*y-8.f/3*z;x+=dx*dt;y+=dy*dt;z+=dz*dt;float rot=t*.01f,px=x*cosf(rot)-z*sinf(rot);ap(o,n,m,px*sc,(y-20)*sc,(uint8_t)(128+127*sinf(i*.02f)),(uint8_t)(128+127*cosf(i*.02f)),150,i==0?1:0);}
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
    for (int i = 0; i < ns && n < m; i++) {
        const Star& s = sorted[i];
        optimizer::emitBlankTo(o, n, m, s.x, s.y, cfg);
        for (int d = 0; d < dwell && n < m; d++)
            ap(o, n, m, s.x, s.y, s.r, s.g, s.b, 0);
    }
    return n;
}

// ─── ANIMIERTE FAHRZEUGE 91-98 ───────────────────────────────
// All use line() and circ_draw() which are now optimizer-backed.
// No per-vehicle changes needed.
static size_t p91(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.85f, ox=scrollX(ph,sp);
    float bw=SC*.14f*sc2, bh=SC*.38f*sc2;
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,ox+cosf(a)*bw,sinf(a)*bh,220,220,255,i==0?1:0);}
    line(o,n,m,ox-bw,bh,ox,bh+bh*.7f,255,200,200,10);
    line(o,n,m,ox+bw,bh,ox,bh+bh*.7f,255,200,200,10);
    line(o,n,m,ox-bw,-bh*.55f,ox-bw*2.5f,-bh,200,150,255,8);
    line(o,n,m,ox-bw*2.5f,-bh,ox-bw,-bh,200,150,255,8);
    line(o,n,m,ox+bw,-bh*.55f,ox+bw*2.5f,-bh,200,150,255,8);
    line(o,n,m,ox+bw*2.5f,-bh,ox+bw,-bh,200,150,255,8);
    float fl=bh*.5f+bh*.15f*fabsf(sinf(ph*0.18f));
    line(o,n,m,ox-bw*.6f,-bh,ox,-bh-fl,255,140,0,6);
    line(o,n,m,ox+bw*.6f,-bh,ox,-bh-fl,255,200,0,6);
    line(o,n,m,ox,-bh,ox,-bh-fl*1.2f,255,80,0,5);
    circ_draw(o,n,m,ox,bh*.45f,bw*.52f,100,220,255,16);
    return n;
}
static size_t p92(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.7f, ox=scrollX(ph,sp);
    float lw=SC*.42f*sc2, lh=SC*.20f*sc2, ly=SC*.06f*sc2;
    rect_outline(o,n,m,ox-lw,ly-lh,ox+lw,ly+lh,200,110,50,10);
    rect_outline(o,n,m,ox+lw*.25f,ly+lh,ox+lw,ly+lh*1.8f,190,100,45,8);
    line(o,n,m,ox-lw*.5f,ly+lh,ox-lw*.5f,ly+lh*1.9f,160,80,40,6);
    circ_draw(o,n,m,ox-lw*.5f,ly+lh*2.0f,lw*.1f,160,80,40,12);
    for(int k=0;k<3;k++){float sy=ly+lh*2.2f+k*lw*.22f,sx=ox-lw*.5f+k*lw*.14f*(((int)(ph*.01f)+k)%2?1.f:-1.f);circ_draw(o,n,m,sx,sy,lw*.09f*(1.f+k*.25f),170,170,170,8);}
    float wy=ly-lh, wr=SC*.1f*sc2;
    for(int k=0;k<4;k++){float wx=ox-lw*.75f+k*lw*.5f;circ_draw(o,n,m,wx,wy,wr,70,70,70,16);float rot=ph*0.06f;ap(o,n,m,wx+cosf(rot)*wr,wy+sinf(rot)*wr,120,120,120,1);ap(o,n,m,wx+cosf(rot+M_PI)*wr,wy+sinf(rot+M_PI)*wr,120,120,120,0);}
    line(o,n,m,-SC*.98f,wy-wr*1.1f,SC*.98f,wy-wr*1.1f,100,80,60,30);
    line(o,n,m,-SC*.98f,wy-wr*1.25f,SC*.98f,wy-wr*1.25f,100,80,60,30);
    return n;
}
static size_t p93(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.75f, ox=scrollX(ph,sp);
    float bw=SC*.5f*sc2, bh=SC*.13f*sc2, by=-SC*.04f*sc2;
    line(o,n,m,ox-bw,by,ox+bw,by,230,30,30,16);
    line(o,n,m,ox+bw,by,ox+bw,by+bh,230,30,30,6);
    line(o,n,m,ox+bw,by+bh,ox+bw*.1f,by+bh*1.9f,230,30,30,8);
    line(o,n,m,ox+bw*.1f,by+bh*1.9f,ox-bw*.35f,by+bh*1.6f,230,30,30,6);
    line(o,n,m,ox-bw*.35f,by+bh*1.6f,ox-bw,by,230,30,30,8);
    line(o,n,m,ox+bw*.65f,by+bh*1.3f,ox+bw*1.12f,by+bh*1.3f,255,255,255,6);
    line(o,n,m,ox+bw*.9f,by,ox+bw*.9f,by+bh*1.3f,200,200,200,4);
    float wr=SC*.11f*sc2, wrot=ph*0.07f;
    for(int w=0;w<2;w++){
        float wx=w?ox+bw*.55f:ox-bw*.45f;
        circ_draw(o,n,m,wx,by,wr,50,50,50,18);
        ap(o,n,m,wx+cosf(wrot)*wr*.7f,by+sinf(wrot)*wr*.7f,120,120,120,1);
        ap(o,n,m,wx+cosf(wrot+M_PI)*wr*.7f,by+sinf(wrot+M_PI)*wr*.7f,120,120,120,0);
    }
    float spd2=0.15f+sp/255.f*.5f;
    for(int k=1;k<=5;k++){float len=SC*k*.06f*spd2;ap(o,n,m,ox-bw-len,by+bh*k*.14f,200,20,20,1);ap(o,n,m,ox-bw,by+bh*k*.14f,200,20,20,0);}
    return n;
}
static size_t p94(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.85f, ox=scrollX(ph,sp);
    float hover=sinf(ph*0.035f)*SC*.08f*sc2;
    float dw=SC*.45f*sc2, dh=SC*.12f*sc2;
    for(int i=0;i<=50;i++){float a=PI2*i/50.f;ap(o,n,m,ox+cosf(a)*dw,sinf(a)*dh+hover,0,200,255,i==0?1:0);}
    for(int i=0;i<=25;i++){float a=M_PI*i/25.f;ap(o,n,m,ox+cosf(a)*dw*.44f,sinf(a)*dh*1.5f+dh+hover,150,230,255,i==0?1:0);}
    for(int k=-2;k<=2;k++){circ_draw(o,n,m,ox+k*dw*.38f,hover,dh*.35f,0,255,200,10);}
    line(o,n,m,ox-dw*.25f,-dh+hover,ox-dw*.75f,-SC*.8f*sc2,0,150,255,8);
    line(o,n,m,ox+dw*.25f,-dh+hover,ox+dw*.75f,-SC*.8f*sc2,0,150,255,8);
    for(int k=0;k<6;k++){float a=PI2*k/6.f+ph*0.06f;uint8_t br=(uint8_t)(128+127*sinf(a+ph*.1f));ap(o,n,m,ox+cosf(a)*dw,sinf(a)*dh+hover,br,br,0,0);}
    return n;
}
static size_t p95(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.8f, ox=scrollX(ph,sp);
    float rock=sinf(ph*0.025f)*SC*.04f*sc2;
    float hw=SC*.52f*sc2, hh=SC*.12f*sc2, hy=-SC*.1f*sc2+rock;
    line(o,n,m,ox-hw,hy,ox+hw,hy,180,140,80,16);
    line(o,n,m,ox-hw,hy,ox-hw*.7f,hy-hh,180,140,80,8);
    line(o,n,m,ox+hw,hy,ox+hw*.7f,hy-hh,180,140,80,8);
    line(o,n,m,ox-hw*.7f,hy-hh,ox+hw*.7f,hy-hh,180,140,80,12);
    line(o,n,m,ox,hy-hh,ox,hy-hh+SC*.88f*sc2,200,200,200,10);
    float mt=hy-hh+SC*.85f*sc2, mb=hy-hh+SC*.1f*sc2;
    line(o,n,m,ox,mt,ox+hw*.95f,hy,255,255,240,10);
    line(o,n,m,ox,mb,ox+hw*.95f,hy,255,255,240,10);
    line(o,n,m,ox,mb,ox,mt,255,255,240,6);
    line(o,n,m,ox,mt*.35f+mb*.65f,ox-hw*.65f,hy*.2f+mb*.8f,200,240,255,8);
    line(o,n,m,ox,mb,ox-hw*.65f,hy*.2f+mb*.8f,200,240,255,5);
    for(int i=0;i<=50;i++){float wx=L(-SC*.98f,SC*.98f,i/50.f),wy=hy+hh*.5f+sinf(wx*.00015f+ph*.03f)*SC*.04f*sc2;ap(o,n,m,wx,wy,0,100,200,i==0?1:0);}
    return n;
}
static size_t p96(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.75f, ox=scrollX(ph,sp);
    float wr=SC*.24f*sc2, ws=SC*.55f*sc2;
    float lw=ox-ws*.5f, rw=ox+ws*.5f, cy=0;
    float rot=ph*0.07f;
    for(int w=0;w<2;w++){
        float wx=w?rw:lw;
        circ_draw(o,n,m,wx,cy,wr,160,120,60,24);
        for(int s=0;s<4;s++){float a=rot+PI2*s/4.f;ap(o,n,m,wx+cosf(a)*wr,cy+sinf(a)*wr,120,90,50,1);ap(o,n,m,wx,cy,120,90,50,0);}
        circ_draw(o,n,m,wx,cy,wr*.15f,140,100,60,8);
    }
    float bbx=lw+ws*.45f, bby=cy;
    line(o,n,m,bbx,bby,lw,cy,140,140,140,8);
    line(o,n,m,bbx,bby,bbx,bby+wr*.8f,140,140,140,6);
    line(o,n,m,bbx,bby+wr*.8f,rw-wr*.15f,cy+wr*.4f+wr*.5f,140,140,140,8);
    line(o,n,m,bbx,bby,rw-wr*.15f,cy+wr*.4f+wr*.5f,140,140,140,8);
    line(o,n,m,rw-wr*.15f,cy+wr*.4f+wr*.5f,rw-wr*.15f,cy+wr*.1f,140,140,140,6);
    line(o,n,m,bbx-wr*.28f,bby+wr*.82f,bbx+wr*.28f,bby+wr*.82f,100,100,200,5);
    float hbx=rw-wr*.15f, hby=cy+wr*.5f+wr*.5f;
    line(o,n,m,hbx-wr*.22f,hby+wr*.35f,hbx+wr*.22f,hby+wr*.4f,180,180,255,5);
    return n;
}
static size_t p97(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.82f, ox=scrollX(ph,sp);
    float bank=sinf(ph*0.022f)*SC*.05f*sc2;
    float fw=SC*.55f*sc2, fh=SC*.1f*sc2;
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,ox+cosf(a)*fw,sinf(a)*fh+bank,220,230,255,i==0?1:0);}
    line(o,n,m,ox+fw,bank,ox+fw*1.42f,bank,220,230,255,8);
    float wy=ox-fw*.1f;
    line(o,n,m,wy,bank-fh*.2f,wy-fw*1.3f,bank+fh*1.5f,220,230,255,14);
    line(o,n,m,wy,bank+fh*.45f,wy-fw*1.3f,bank+fh*1.5f,200,210,240,6);
    line(o,n,m,wy,bank-fh*.2f,wy+fw*.6f,bank+fh*1.5f,220,230,255,14);
    line(o,n,m,wy,bank+fh*.45f,wy+fw*.6f,bank+fh*1.5f,200,210,240,6);
    line(o,n,m,ox-fw*.82f,bank,ox-fw*1.0f,bank+fh*2.8f,200,210,255,8);
    line(o,n,m,ox-fw*1.0f,bank+fh*2.8f,ox-fw*.6f,bank,200,210,255,5);
    line(o,n,m,ox-fw*.72f,bank,ox-fw*1.18f,bank+fh*1.2f,200,210,255,8);
    line(o,n,m,ox-fw*.72f,bank,ox-fw*.35f,bank+fh*1.2f,200,210,255,8);
    for(int e=-1;e<=1;e+=2){float ex=ox+e*SC*.28f*sc2;circ_draw(o,n,m,ex,bank-fh*1.3f,fh*.65f,180,190,220,12);line(o,n,m,ex-fh,bank-fh*1.3f,ex+fh*1.8f,bank-fh*1.3f,180,190,220,8);}
    for(int k=1;k<=5;k++){ap(o,n,m,ox-fw-SC*k*.07f*sc2,bank+(k%2?fh*.15f:-fh*.15f),210,220,255,1);ap(o,n,m,ox-fw-SC*(k+.5f)*.07f*sc2,bank,190,200,240,0);}
    return n;
}
static size_t p98(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.72f, ox=scrollX(ph,sp);
    float tw=SC*.18f*sc2, th=SC*.5f*sc2;
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,ox+cosf(a)*tw,sinf(a)*th,200,110,50,i==0?1:0);}
    line(o,n,m,ox-tw,th,ox,th+tw*1.5f,200,110,50,8);
    line(o,n,m,ox+tw,th,ox,th+tw*1.5f,200,110,50,8);
    for(int i=0;i<=30;i++){float a=PI2*i/30.f;ap(o,n,m,ox+tw*2.0f+cosf(a)*tw*.7f,sinf(a)*th*.6f+th*.25f,210,215,230,i==0?1:0);}
    line(o,n,m,ox+tw*1.3f,th*.85f,ox+tw*2.7f,th*1.1f,210,215,230,6);
    line(o,n,m,ox+tw*2.7f,th*1.1f,ox+tw*2.0f,th*1.35f,210,215,230,6);
    line(o,n,m,ox+tw*1.3f,-th*.15f,ox+tw*3.4f,-th*.35f,180,185,210,10);
    line(o,n,m,ox+tw*2.7f,th*.25f,ox+tw*3.4f,-th*.35f,180,185,210,6);
    for(int i=0;i<=30;i++){float a=PI2*i/30.f;ap(o,n,m,ox-tw*2.0f+cosf(a)*tw*.55f,sinf(a)*th*.72f,215,215,215,i==0?1:0);}
    float fl=th*.35f+th*.1f*fabsf(sinf(ph*0.22f));
    for(int e=-1;e<=1;e++){float ex=ox+e*tw*.85f;line(o,n,m,ex-tw*.2f,-th,ex,-th-fl,255,(uint8_t)(100+50*fabsf(sinf(ph*.25f+e))),0,6);line(o,n,m,ex+tw*.2f,-th,ex,-th-fl,255,200,0,4);}
    line(o,n,m,ox-tw*2.4f,-th*.72f,ox-tw*2.0f,-th*.72f-fl*.85f,255,150,0,6);
    line(o,n,m,ox-tw*1.6f,-th*.72f,ox-tw*2.0f,-th*.72f-fl*.85f,255,210,0,4);
    return n;
}

static size_t p99(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=SC*ssc(sz)*0.7f, ox=scrollX(ph,sp);
    line(o,n,m, ox-sc2,0,            ox+sc2,0,            200,200,255, 20);
    line(o,n,m, ox-sc2*.1f,0,        ox-sc2*.4f, sc2*.45f, 200,200,255, 14);
    line(o,n,m, ox-sc2*.4f,sc2*.45f, ox-sc2*.1f,0,         200,200,255,  6);
    line(o,n,m, ox-sc2*.1f,0,        ox-sc2*.4f,-sc2*.45f, 200,200,255, 14);
    line(o,n,m, ox-sc2*.4f,-sc2*.45f,ox-sc2*.1f,0,         200,200,255,  6);
    line(o,n,m, ox-sc2*.75f,0,       ox-sc2,     sc2*.3f,  200,200,255,  8);
    line(o,n,m, ox-sc2*.75f,0,       ox-sc2,    -sc2*.3f,  200,200,255,  8);
    line(o,n,m, ox+sc2,0,            ox+sc2*.85f, sc2*.08f, 200,200,255,  4);
    line(o,n,m, ox+sc2,0,            ox+sc2*.85f,-sc2*.08f, 200,200,255,  4);
    line(o,n,m, ox-sc2*.15f, sc2*.2f, ox-sc2*.45f, sc2*.2f,  180,220,255,  8);
    line(o,n,m, ox-sc2*.15f,-sc2*.2f, ox-sc2*.45f,-sc2*.2f,  180,220,255,  8);
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
        cr = 80; cg = 80; cb = 80;
    } else if (rem <= 10) {
        cr = 255; cg = 40;  cb = 0;
    } else if (rem <= 30) {
        cr = 255; cg = 180; cb = 0;
    } else {
        cr = 0;   cg = 220; cb = 80;
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

    auto colon = [&](float cx) {
        ap(o,n,m, cx-cdot*0.5f, dh*0.28f, cr,cg,cb, 1);
        ap(o,n,m, cx+cdot*0.5f, dh*0.28f, cr,cg,cb, 0);
        ap(o,n,m, cx-cdot*0.5f, -dh*0.28f, cr,cg,cb, 1);
        ap(o,n,m, cx+cdot*0.5f, -dh*0.28f, cr,cg,cb, 0);
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
            ap(o,n,m, -SC*0.04f*sc2, dot_y, cr,cg,cb, 1);
            ap(o,n,m,  SC*0.04f*sc2, dot_y, cr,cg,cb, 0);
        }
    }
    return n;
}

// ─── NEUE PRESETS 102-105 ───────────────────────────────────────
// p102 Torus Knot (2,3) -- 2D projection: r(t)=R+cos(q*t), (R=2,q=3,p=2).
// Peak radius R+1=3 -> normalized by 1/3. Single closed continuous curve,
// uses csweep() like the other Curves-group presets (p22-p27).
static size_t p102(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f*(1.f/3.f);const int N=280;
    for(int i=0;i<=N;i++){float t=csweep(ph,sp,i,N),r=2.f+cosf(3.f*t);
        ap(o,n,m,sc*r*cosf(2.f*t),sc*r*sinf(2.f*t),0,220,255,i==0?1:0);}
    return n;
}

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
        verts[k].r=255; verts[k].g=0; verts[k].b=80;
        verts[k].lift=false;
    }
    optimizer::PathSegment seg(verts,5,/*closed=*/true);
    return optimizer::optimize(&seg,1,o,m,liveOptimizerConfig());
}

// p104 DNA Double Helix -- two counter-phase sine strands (backbone) plus
// straight rungs (base pairs) at fixed intervals via line().
static size_t p104(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;
    float sc=SC*ssc(sz)*.9f, amp=sc*.32f, off=aang(ph,sp);
    const int NS=70; const float TURNS=3.f;
    for(int i=0;i<=NS;i++){float t=(float)i/NS,x=L(-sc,sc,t),a=t*PI2*TURNS+off;
        ap(o,n,m,x,amp*sinf(a),0,180,255,i==0?1:0);}
    for(int i=0;i<=NS;i++){float t=(float)i/NS,x=L(-sc,sc,t),a=t*PI2*TURNS+off;
        ap(o,n,m,x,amp*sinf(a+(float)M_PI),255,0,150,i==0?1:0);}
    const int RUNGS=9;
    for(int rI=0;rI<RUNGS;rI++){
        float t=(float)rI/(RUNGS-1),x=L(-sc,sc,t),a=t*PI2*TURNS+off;
        line(o,n,m,x,amp*sinf(a),x,amp*sinf(a+(float)M_PI),120,120,120);
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
    size_t n=0;
    float R=SC*ssc(sz)*.85f, off=aang(ph,sp);
    const float c=cosf(off),s=sinf(off);
    auto rot=[&](float x,float y,uint8_t r,uint8_t g,uint8_t b,uint8_t bl){
        ap(o,n,m,x*c-y*s,x*s+y*c,r,g,b,bl);};
    // Outer circle
    const int NC=64;
    for(int i=0;i<=NC;i++){float a=PI2*i/NC;rot(R*cosf(a),R*sinf(a),255,255,255,i==0?1:0);}
    // S-divider: upper half-circle (centre 0,+R/2) then lower (centre 0,-R/2),
    // meeting at the origin to form one continuous S along the local y-axis.
    const int NS=32;
    for(int i=0;i<=NS;i++){float a=-(float)(M_PI/2)+(float)M_PI*i/NS;
        rot((R*.5f)*cosf(a),(R*.5f)+(R*.5f)*sinf(a),255,255,255,i==0?1:0);}
    for(int i=0;i<=NS;i++){float a=(float)(M_PI/2)+(float)M_PI*i/NS;
        rot((R*.5f)*cosf(a),-(R*.5f)+(R*.5f)*sinf(a),255,255,255,i==0?1:0);}
    // Eye dots
    const int ND=20; float rd=R*.14f;
    for(int i=0;i<=ND;i++){float a=PI2*i/ND;rot(rd*cosf(a),(R*.5f)+rd*sinf(a),255,60,60,i==0?1:0);}
    for(int i=0;i<=ND;i++){float a=PI2*i/ND;rot(rd*cosf(a),-(R*.5f)+rd*sinf(a),60,180,255,i==0?1:0);}
    if(n>0)ap(o,n,m,o[n-1].x,o[n-1].y,0,0,0,1);
    return n;
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
        for (int d = 0; d < dwell && n < m; d++) ap(o, n, m, px, py, vr, vg, vb, 0);
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

// ─── DISPATCH ────────────────────────────────────────────────

// presetClassOf() -- maps a Preset to its optimizer profile index.
presets::PresetClass presets::presetClassOf(presets::Preset p) {
    if (p == presets::Preset::None) return presets::PresetClass::Scenes;
    using P = presets::Preset;
    switch (p) {
        // ── Simple: Geometry + Lines ──────────────────────────────────
        case P::Circle: case P::Square: case P::Triangle:
        case P::Pentagon: case P::Hexagon: case P::Octagon:
        case P::Star4: case P::Star5: case P::Star6: case P::Star8:
        case P::CrossPlus: case P::XShape: case P::Grid3x3:
        case P::HLine: case P::Diagonal:
        case P::ThreeCircles:
            return presets::PresetClass::Simple;
        // ── Curves: Spirals + Curves + Waves + Complex + Combo ────────
        case P::ArchimedeanSpiral: case P::Lissajous1To2: case P::Lissajous2To3:
        case P::Lissajous3To4: case P::Lissajous3To5: case P::Lissajous5To6:
        case P::DoubleSpiral: case P::Rose3:
        case P::Rose4: case P::Cardioid: case P::Heart: case P::Infinity:
        case P::Astroid: case P::Epitrochoid: case P::TorusKnot:
        case P::SineWave: case P::StandingWave: case P::MultiWave:
        case P::OceanWave: case P::WaveInterference: case P::Sawtooth:
        case P::SquareWave: case P::WavePacket: case P::BeatWave:
        case P::RadialWaves: case P::FmWave: case P::Vortex:
        case P::SineHelix: case P::WaveField: case P::FourierSquare:
        case P::GravityWaves: case P::Tsunami: case P::WaveSpectrum:
        case P::Hypotrochoid: case P::Butterfly: case P::Spirograph5To3:
        case P::ConcentricRings: case P::NestedSquares: case P::PulsingCircle:
        case P::Starburst: case P::ChaosBouncer: case P::LaserDiamond:
        case P::ChampagneBubbles: case P::ConfettiBurst: case P::DiscoBall:
        case P::Pentagram: case P::DnaHelix: case P::YinYang:
            return presets::PresetClass::Curves;
        // ── ThreeD: 3D wireframes ─────────────────────────────────────
        case P::RotatingCube: case P::StaticCube: case P::Pyramid:
        case P::Octahedron: case P::Tetrahedron:
            return presets::PresetClass::ThreeD;
        // ── Scenes: everything else ───────────────────────────────────
        default:
            return presets::PresetClass::Scenes;
    }
}

const PresetInfo PRESETS[PRESET_COUNT] = {
    {"Circle","Geometry"},{"Square","Geometry"},{"Triangle","Geometry"},{"Pentagon","Geometry"},{"Hexagon","Geometry"},{"Octagon","Geometry"},{"Star 4","Geometry"},{"Star 5","Geometry"},{"Star 6","Geometry"},{"Star 8","Geometry"},
    {"Cross +","Lines"},{"X Shape","Lines"},{"Grid 3x3","Lines"},{"H Line","Lines"},{"Diagonal","Lines"},
    {"Archimedean Spiral","Spirals"},{"Lissajous 1:2","Spirals"},{"Lissajous 2:3","Spirals"},{"Lissajous 3:4","Spirals"},{"Lissajous 3:5","Spirals"},{"Lissajous 5:6","Spirals"},{"Double Spiral","Spirals"},{"Rose 3","Spirals"},
    {"Rose 4","Curves"},{"Cardioid","Curves"},{"Heart","Curves"},{"Infinity","Curves"},{"Astroid","Curves"},{"Epitrochoid","Curves"},
    {"Rotating Cube","3D"},{"Static Cube","3D"},{"Pyramid","3D"},{"Octahedron","3D"},{"Tetrahedron","3D"},
    {"Sine Wave","Waves"},{"Standing Wave","Waves"},{"Multi Wave","Waves"},{"Ocean Wave","Waves"},{"Wave Interference","Waves"},{"Sawtooth","Waves"},{"Square Wave","Waves"},{"Wave Packet","Waves"},{"Beat Wave","Waves"},{"Radial Waves","Waves"},{"FM Wave","Waves"},{"Vortex","Waves"},{"Sine Helix","Waves"},{"Wave Field","Waves"},{"Fourier Square","Waves"},{"Gravity Waves","Waves"},{"Tsunami","Waves"},{"Wave Spectrum","Waves"},
    {"Hypotrochoid","Complex"},{"Butterfly","Complex"},{"Spirograph 5/3","Complex"},{"Concentric Rings","Complex"},{"Nested Squares","Complex"},{"Pulsing Circle","Complex"},
    {"Starburst","Combo"},{"Chaos Bouncer","Combo"},{"Laser Diamond","Combo"},{"Champagne Bubbles","Combo"},{"Confetti Burst","Combo"},{"Disco Ball","Combo"},
    {"Martini Glass","Party"},{"Wine Glass","Party"},{"Champagne Flute","Party"},{"Tropical Cocktail","Party"},{"Palm Tree","Party"},{"Flamingo","Party"},{"Tropical Fish","Party"},{"Water Splash","Party"},{"Pool Waves","Party"},{"Tropical Sun","Party"},{"Pineapple","Party"},{"Music Note","Party"},{"Balloon","Party"},{"Crown","Party"},{"Diamond","Party"},{"Cocktail Umbrella","Party"},{"Water Drop","Party"},{"Rising Bubbles","Party"},{"Confetti","Party"},{"Disco Ball 2","Party"},{"Sunset","Party"},{"Starfish","Party"},{"Hibiscus","Party"},{"Coconut Palm","Party"},{"Starburst Party","Party"},{"Party Finale","Party"},
    {"Starfield","Scenes"},
    {"Countdown Timer","Timers"},
    {"Rocket","Vehicles"},{"Train","Vehicles"},{"Racing Car","Vehicles"},{"UFO","Vehicles"},{"Sailing Boat","Vehicles"},{"Bicycle","Vehicles"},{"Airplane","Vehicles"},{"Space Shuttle","Vehicles"},
    {"Torus Knot","Curves"},{"Pentagram","Geometry"},{"DNA Helix","Complex"},{"Yin Yang","Symbols"},
    {"Random Points","Scenes"},
    {"Three Circles","Geometry"},{"Point Spread","Scenes"},
};

static const PFn DISPATCH[PRESET_COUNT] = {
    p00,p01,p02,p03,p04,p05,p06,p07,p08,p09,
    p10,p11,p12,p13,p14,
    p15,p16,p17,p18,p19,p20,p21,p22,
    p23,p24,p25,p26,p27,p28,
    p29,p30,p31,p32,p33,
    p35,p36,p37,p38,p39,p40,p41,p42,p43,p44,p45,p46,p47,p48,p49,p50,p51,p52,
    p53,p54,p55,p56,p57,p58,
    p59,p60,p61,p62,p63,p101,
    p64,p65,p66,p67,p68,p69,p70,p71,p72,p73,p74,p75,p76,p77,p78,p79,p80,p81,p82,p83,p84,p85,p86,p87,p88,p89,
    p90,
    p100,
    p91,p92,p93,p94,p95,p96,p97,p98,
    p102,p103,p104,p105,
    p106,
    p107,p108,
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
        case 64:  // Martini Glass
        case 65:  // Wine Glass
        case 66:  // Champagne Flute
        case 74:  // Pineapple
        case 105: // Three Circles
        case 106: // Point Spread
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