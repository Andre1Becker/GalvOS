#include "preset_patterns.h"
#include "countdown_timer.h"
#include <math.h>
#include <string.h>
#include <Arduino.h>

namespace presets {

static constexpr float PI2  = 2.0f * M_PI;
static constexpr float SC   = 18000.0f;

static inline float ssc(uint8_t s) { return 0.25f + (s / 255.0f) * 1.5f; }
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

/* adaptN -- scales point count with size_val.
 * Small sizes need fewer points (prevents galvo overheating).
 * Large projections need more points for smooth curves.
 * base     = default point count at size=128
 * min_pts  = Minimum (never less)
 * max_pts  = Maximum (never more)
 */
static inline int adaptN(uint8_t sz, int base, int min_pts=8, int max_pts=512){
    float factor = 0.4f + (sz / 255.f) * 1.2f;   // 0.4 at sz=0, 1.6 at sz=255
    int n = (int)(base * factor);
    if (n < min_pts) n = min_pts;
    if (n > max_pts) n = max_pts;
    return n;
}

static void line(LaserPoint*o,size_t&n,size_t mx,
                 float x0,float y0,float x1,float y1,
                 uint8_t r,uint8_t g,uint8_t b,
                 int S=20)
    {
    // Move to Target point first
    for(int k=0;k<40 && n<mx;k++)
        ap(o,n,mx,x0,y0,0,0,0,1);

    // Linie zeichnen
    for(int i=0;i<=S;i++)
    {
        ap(o,n,mx,
           L(x0,x1,i/float(S)),
           L(y0,y1,i/float(S)),
           r,g,b,
           0);
    }
    // stop and endpoint
    for(int k=0;k<40 && n<mx;k++)
        ap(o,n,mx,x1,y1,0,0,0,1);

}
static size_t ngon(LaserPoint*o,size_t mx,int sides,float sc,float off,uint8_t r,uint8_t g,uint8_t b){
    size_t n=0;
    // Interpolate per side so galvo can follow straight edges accurately.
    // Minimum 8 steps per side; more sides = fewer steps needed.
    int sps = (sides <= 4) ? 20 : (sides <= 6) ? 14 : 10;
    for(int s=0;s<sides;s++){
        float a0=PI2*s/sides+off, a1=PI2*(s+1)/sides+off;
        float x0=cosf(a0)*sc, y0=sinf(a0)*sc;
        float x1=cosf(a1)*sc, y1=sinf(a1)*sc;
        for(int k=0;k<=sps;k++){
            float t=(float)k/sps;
            ap(o,n,mx, x0+(x1-x0)*t, y0+(y1-y0)*t, r,g,b, (s==0&&k==0)?1:0);
        }
    }
    // Closing blank: move back to start with laser off to prevent lit retrace on next frame
    if(n>0 && n<mx){LaserPoint cl=o[0];cl.blank=1;o[n++]=cl;}
    return n;
}
static size_t star(LaserPoint*o,size_t mx,int pts,float outer,float inner,float off,uint8_t r,uint8_t g,uint8_t b){
    size_t n=0;
    // Interpolate per segment so galvo can follow straight edges accurately.
    const int sps = 16;
    int segs = pts*2;
    for(int s=0;s<segs;s++){
        float a0=PI2*s/segs+off-(float)(M_PI/2), a1=PI2*(s+1)/segs+off-(float)(M_PI/2);
        float r0=(s%2==0)?outer:inner, r1=((s+1)%2==0)?outer:inner;
        float x0=cosf(a0)*r0, y0=sinf(a0)*r0;
        float x1=cosf(a1)*r1, y1=sinf(a1)*r1;
        for(int k=0;k<=sps;k++){
            float t=(float)k/sps;
            ap(o,n,mx, x0+(x1-x0)*t, y0+(y1-y0)*t, r,g,b, (s==0&&k==0)?1:0);
        }
    }
    // Closing blank: prevent lit retrace on next frame
    if(n>0 && n<mx){LaserPoint cl=o[0];cl.blank=1;o[n++]=cl;}
    return n;
}

struct P3D{float x,y,z;};
static void prj(P3D v,float ry,float rx,float sc,float&ox,float&oy){
    float rx2=v.x*cosf(ry)+v.z*sinf(ry),rz=-v.x*sinf(ry)+v.z*cosf(ry);
    ox=(rx2)*sc; oy=(v.y*cosf(rx)-rz*sinf(rx))*sc;
}
static const P3D CV[]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
static const int CE[][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
static size_t wf(LaserPoint*o,size_t mx,const P3D*V,int nv,const int(*E)[2],int ne,float ry,float rx,float sc,uint8_t r,uint8_t g,uint8_t b){
    size_t n=0;
    for(int e=0;e<ne;e++){for(int k=0;k<=28;k++){float t=k/28.f;P3D v={L(V[E[e][0]].x,V[E[e][1]].x,t),L(V[E[e][0]].y,V[E[e][1]].y,t),L(V[E[e][0]].z,V[E[e][1]].z,t)};float ox,oy;prj(v,ry,rx,sc,ox,oy);ap(o,n,mx,ox,oy,r,g,b,k==0?1:0);}}
    return n;
}

static size_t sinewave(LaserPoint*o,size_t mx,float A,float f,float ph_off,float sc,uint8_t r,uint8_t g,uint8_t b,int N=120){
    size_t n=0;
    const float effA=A*gLivePreset.wave_amp, effF=f*gLivePreset.wave_freq;
    for(int i=0;i<=N;i++){float x=L(-1.f,1.f,i/float(N));ap(o,n,mx,x*sc,effA*sinf(effF*x*PI2+ph_off)*sc,r,g,b,i==0?1:0);}
    return n;
}

typedef size_t(*PFn)(LaserPoint*,size_t,uint32_t,uint8_t,uint8_t);

// ─── GEOMETRIE 0-9 ──────────────────────────────────────────
static size_t p00(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,adaptN(sz,80,12,200),SC*ssc(sz)*.9f,aang(ph,sp),255,255,255);}
static size_t p01(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,4,SC*ssc(sz)*.9f,aang(ph,sp),255,240,0);}
static size_t p02(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,3,SC*ssc(sz)*.9f,aang(ph,sp),0,255,240);}
static size_t p03(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,5,SC*ssc(sz)*.9f,aang(ph,sp),255,128,0);}
static size_t p04(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,6,SC*ssc(sz)*.9f,aang(ph,sp),0,255,128);}
static size_t p05(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return ngon(o,m,8,SC*ssc(sz)*.9f,aang(ph,sp),128,0,255);}
static size_t p06(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,4,s,s*.36f,aang(ph,sp),255,0,0);}
static size_t p07(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,5,s,s*.36f,aang(ph,sp),255,220,0);}
static size_t p08(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,6,s,s*.36f,aang(ph,sp),0,255,0);}
static size_t p09(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){float s=SC*ssc(sz)*.9f;return star(o,m,8,s,s*.36f,aang(ph,sp),0,128,255);}

// ─── LINIEN 10-14 ────────────────────────────────────────────
static size_t p10(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f;line(o,n,m,-s,0,s,0,255,60,60,50);line(o,n,m,0,-s,0,s,60,255,60,50);return n;}
static size_t p11(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.65f;line(o,n,m,-s,-s,s,s,0,255,255,50);line(o,n,m,s,-s,-s,s,255,0,255,50);return n;}
static size_t p12(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f,st=s*2.f/3.f;for(int i=0;i<=3;i++){float x=-s+i*st;line(o,n,m,x,-s,x,s,0,200,200,20);}for(int i=0;i<=3;i++){float y=-s+i*st;line(o,n,m,-s,y,s,y,0,200,200,20);}return n;}
static size_t p13(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f,off=(aang(ph,sp)/PI2*2.f-1.f)*s;for(int i=0;i<=60;i++)ap(o,n,m,L(-s,s,i/60.f),off,255,128,0,i==0?1:0);return n;}
static size_t p14(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float s=SC*ssc(sz)*.9f,a=aang(ph,sp);for(int i=0;i<=60;i++)ap(o,n,m,L(-s,s,i/60.f)*cosf(a),L(-s,s,i/60.f)*sinf(a),255,0,128,i==0?1:0);return n;}

// ─── SPIRALEN 15-22 ──────────────────────────────────────────
static size_t p15(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);const int N=adaptN(sz,200,30,400);for(int i=0;i<N;i++){float t=(float)i/N,a=t*PI2*3.5f+off,r=t*sc;ap(o,n,m,cosf(a)*r,sinf(a)*r,(uint8_t)(t*255),(uint8_t)((1-t)*255),128,i==0?1:0);}return n;}
static size_t p16(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=200;i++){float t=PI2*i/200.f;ap(o,n,m,cosf(t+off)*sc,sinf(2*t+M_PI/4.f)*sc,0,200,255,i==0?1:0);}return n;}
static size_t p17(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=250;i++){float t=PI2*i/250.f;ap(o,n,m,cosf(2*t+off)*sc,sinf(3*t+M_PI/4.f)*sc,0,180,255,i==0?1:0);}return n;}
static size_t p18(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=300;i++){float t=PI2*i/300.f;ap(o,n,m,cosf(3*t+off)*sc,sinf(4*t+M_PI/3.f)*sc,100,200,255,i==0?1:0);}return n;}
static size_t p19(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=400;i++){float t=PI2*i/400.f;ap(o,n,m,cosf(3*t+off)*sc,sinf(5*t+M_PI/6.f)*sc,150,100,255,i==0?1:0);}return n;}
static size_t p20(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=500;i++){float t=PI2*i/500.f;ap(o,n,m,cosf(5*t+off)*sc,sinf(6*t+PI2/5.f)*sc,255,150,0,i==0?1:0);}return n;}
static size_t p21(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<150;i++){float t=i/150.f,a=t*PI2*3.f+off,r=t*sc;ap(o,n,m,cosf(a)*r,sinf(a)*r,255,80,0,i==0?1:0);}for(int i=0;i<150;i++){float t=i/150.f,a=t*PI2*3.f+off+M_PI,r=t*sc;ap(o,n,m,cosf(a)*r,sinf(a)*r,0,80,255,i==0?1:0);}return n;}
static size_t p22(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);const int N=200;for(int i=0;i<=N;i++){float t=PI2*i/N+off,r=sc*cosf(3*t);ap(o,n,m,r*cosf(t),r*sinf(t),255,100,0,i==0?1:0);}return n;}

// ─── KURVEN 23-28 ────────────────────────────────────────────
static size_t p23(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=200;i++){float t=PI2*i/200.f+off,r=sc*cosf(4*t);ap(o,n,m,r*cosf(t),r*sinf(t),255,50,150,i==0?1:0);}return n;}
static size_t p24(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.45f,off=aang(ph,sp);for(int i=0;i<=200;i++){float t=PI2*i/200.f+off,r=sc*(1.f-cosf(t));ap(o,n,m,r*cosf(t),r*sinf(t),255,0,100,i==0?1:0);}return n;}
static size_t p25(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.045f,a=aang(ph,sp);const int N=adaptN(sz,200,20,300);for(int i=0;i<=N;i++){float t=PI2*i/N,x=sc*16*powf(sinf(t),3),y=sc*(13*cosf(t)-5*cosf(2*t)-2*cosf(3*t)-cosf(4*t));ap(o,n,m,x*cosf(a)-y*sinf(a),x*sinf(a)+y*cosf(a),255,0,80,i==0?1:0);}return n;}
static size_t p26(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);const int N=adaptN(sz,200,30,300);for(int i=0;i<=N;i++){float t=PI2*i/N+off,d=1+sinf(t)*sinf(t);ap(o,n,m,sc*cosf(t)/d,sc*sinf(t)*cosf(t)/d,0,200,255,i==0?1:0);}return n;}
static size_t p27(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);for(int i=0;i<=200;i++){float t=PI2*i/200.f+off;ap(o,n,m,sc*powf(cosf(t),3),sc*powf(sinf(t),3),200,255,50,i==0?1:0);}return n;}
static size_t p28(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.4f,off=aang(ph,sp);const float R=3,r=1,d=2.5f;for(int i=0;i<=300;i++){float t=PI2*i/300.f+off;ap(o,n,m,sc*((R+r)*cosf(t)-d*cosf((R+r)*t/r)),sc*((R+r)*sinf(t)-d*sinf((R+r)*t/r)),0,255,100,i==0?1:0);}return n;}

// ─── 3D 29-34 ────────────────────────────────────────────────
static size_t p29(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return wf(o,m,CV,8,CE,12,aang(ph,sp,1),aang(ph,sp,.4f),SC*ssc(sz)*.65f,0,255,255);}
static size_t p30(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){return wf(o,m,CV,8,CE,12,.6f,.4f,SC*ssc(sz)*.65f,255,255,0);}
static size_t p31(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    static const P3D V[]={{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}};static const P3D apex={0,1,0};
    size_t n=0;float sc=SC*ssc(sz)*.65f,ry=aang(ph,sp);
    for(int i=0;i<4;i++){int j=(i+1)%4;for(int k=0;k<=12;k++){float t=k/12.f;P3D v={L(V[i].x,V[j].x,t),L(V[i].y,V[j].y,t),L(V[i].z,V[j].z,t)};float ox,oy;prj(v,ry,.3f,sc,ox,oy);ap(o,n,m,ox,oy,255,128,0,k==0?1:0);}}
    for(int i=0;i<4;i++){for(int k=0;k<=12;k++){float t=k/12.f;P3D v={L(apex.x,V[i].x,t),L(apex.y,V[i].y,t),L(apex.z,V[i].z,t)};float ox,oy;prj(v,ry,.3f,sc,ox,oy);ap(o,n,m,ox,oy,255,200,0,k==0?1:0);}}
    return n;
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
static size_t p34(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    const float phi=(1+sqrtf(5))/2,i1=1/phi;
    static const P3D V[]={{1,1,1},{1,1,-1},{1,-1,1},{1,-1,-1},{-1,1,1},{-1,1,-1},{-1,-1,1},{-1,-1,-1},{0,(float)(1.618f),(float)(0.618f)},{0,(float)(1.618f),(float)(-0.618f)},{0,(float)(-1.618f),(float)(0.618f)},{0,(float)(-1.618f),(float)(-0.618f)},{(float)(0.618f),0,(float)(1.618f)},{(float)(-0.618f),0,(float)(1.618f)},{(float)(0.618f),0,(float)(-1.618f)},{(float)(-0.618f),0,(float)(-1.618f)},{(float)(1.618f),(float)(0.618f),0},{(float)(1.618f),(float)(-0.618f),0},{(float)(-1.618f),(float)(0.618f),0},{(float)(-1.618f),(float)(-0.618f),0}};
    static const int E[][2]={{0,8},{0,12},{0,16},{1,9},{1,14},{1,16},{2,10},{2,12},{2,17},{3,11},{3,14},{3,17},{4,8},{4,13},{4,18},{5,9},{5,15},{5,18},{6,10},{6,13},{6,19},{7,11},{7,15},{7,19},{8,9},{10,11},{12,13},{14,15},{16,17},{18,19}};
    return wf(o,m,V,20,E,30,aang(ph,sp,.8f),.3f,SC*ssc(sz)*.55f,255,180,0);
}

// ─── WELLEN 35-52 ────────────────────────────────────────────
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
    for(int i=0;i<=200;i++){float x=L(-1.f,1.f,i/200.f),y=(sinf(3*wf*x*PI2+t)>0?1.f:-1.f)*wa*.55f;ap(o,n,m,x*sc,y*sc,200,100,255,i==0?1:0);}
    return n;
}
static size_t p42(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),env=expf(-12*x*x),y=env*sinf(8*x*M_PI+t)*.8f;ap(o,n,m,x*sc,y*sc,0,255,200,i==0?1:0);}
    return n;
}
static size_t p43(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),y=.5f*(sinf(10*x*M_PI+t)+sinf(11*x*M_PI+t));ap(o,n,m,x*sc,y*sc,255,150,0,i==0?1:0);}
    return n;
}
static size_t p44(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int ring=1;ring<=4;ring++){float r=ring/4.f,R=r*sc;for(int i=0;i<=80;i++){float a=PI2*i/80.f,rad=R*(1+.12f*sinf(8*a+r*8-t));ap(o,n,m,cosf(a)*rad,sinf(a)*rad,0,(uint8_t)(100+155*r),255,i==0?1:0);}}
    return n;
}
static size_t p45(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),y=.5f*sinf(6*x*PI2+4*sinf(x*PI2*2+t));ap(o,n,m,x*sc,y*sc,0,255,255,i==0?1:0);}
    return n;
}
static size_t p46(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    for(int i=0;i<200;i++){float t=i/200.f,a=t*PI2*4+off,r=sc*(1-t*.8f),w=.08f*sinf(a*8);ap(o,n,m,cosf(a)*(r+w*sc),sinf(a)*(r+w*sc),(uint8_t)(t*255),(uint8_t)((1-t)*200),200,i==0?1:0);}
    return n;
}
static size_t p47(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=200;i++){float u=L(-1.f,1.f,i/200.f),a=u*PI2*3+t;ap(o,n,m,u*sc,sinf(a)*sc*.5f,0,(uint8_t)(128+127*cosf(a)),(uint8_t)(128+127*sinf(a)),i==0?1:0);}
    return n;
}
static size_t p48(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int row=-2;row<=2;row++){float y0=row*.36f;for(int i=0;i<=60;i++){float x=L(-1.f,1.f,i/60.f),y=y0+.12f*sinf(x*PI2*3+t+row*.7f);ap(o,n,m,x*sc,y*sc,0,(uint8_t)(128+127*sinf(row+t)),200,i==0?1:0);}}
    return n;
}
static size_t p49(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f);float y=0;for(int k=1;k<=5;k+=2)y+=sinf(k*x*PI2*1.5f+t)/k;ap(o,n,m,x*sc,y*.5f*sc,100,255,100,i==0?1:0);}
    return n;
}
static size_t p50(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),decay=expf(-2*fabsf(x)),y=decay*sinf(10*x*M_PI+t)*(.4f+.4f*fabsf(x));ap(o,n,m,x*sc,y*sc,255,80,200,i==0?1:0);}
    return n;
}
static size_t p51(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f),build=.5f+.5f*x,y=build*.5f*sinf(PI2*(x*2-t*.3f))*.7f;ap(o,n,m,x*sc,y*sc,0,150,255,i==0?1:0);}
    return n;
}
static size_t p52(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    for(int i=0;i<=120;i++){float x=L(-1.f,1.f,i/120.f);float y=0;int ks[]={1,2,3,4,5};for(int j=0;j<5;j++)y+=sinf(ks[j]*x*PI2+t*(j*.5f+.5f))*.2f/ks[j];ap(o,n,m,x*sc,y*sc,255,200,50,i==0?1:0);}
    return n;
}

// ─── KOMPLEX 53-58 ───────────────────────────────────────────
static size_t p53(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.38f,off=aang(ph,sp);const float R=5,r=3,d=5;for(int i=0;i<=400;i++){float t=PI2*i/400.f+off;ap(o,n,m,sc*((R-r)*cosf(t)+d*cosf((R-r)*t/r)),sc*((R-r)*sinf(t)-d*sinf((R-r)*t/r)),255,100,200,i==0?1:0);}return n;}
static size_t p54(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.38f,off=aang(ph,sp);for(int i=0;i<=200;i++){float t=PI2*i/200.f,e=expf(cosf(t))-2*cosf(4*t)-powf(sinf(t/12.f),5);ap(o,n,m,sc*e*sinf(t+off),sc*e*cosf(t+off),255,165,0,i==0?1:0);}return n;}
static size_t p55(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){size_t n=0;float sc=SC*ssc(sz)*.38f,off=aang(ph,sp);const float R=5,k=3.f/5,l=.7f;for(int i=0;i<=400;i++){float t=PI2*i/400.f+off;ap(o,n,m,sc*R*((1-k)*cosf(t)+l*k*cosf((1-k)*t/k)),sc*R*((1-k)*sinf(t)-l*k*sinf((1-k)*t/k)),0,200,255,i==0?1:0);}return n;}
static size_t p56(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,pulse=.8f+.2f*fabsf(sinf(aang(ph,sp,2)));
    for(int ring=1;ring<=5;ring++){float r=sc*ring/5.f*pulse;float h=ring/5.f;uint8_t cr=(uint8_t)(fabsf(sinf(h*M_PI))*255),cg=(uint8_t)(fabsf(sinf(h*M_PI+2.094f))*255),cb=(uint8_t)(fabsf(sinf(h*M_PI+4.189f))*255);for(int i=0;i<=64;i++){float a=PI2*i/64.f;ap(o,n,m,cosf(a)*r,sinf(a)*r,cr,cg,cb,i==0?1:0);}}
    return n;
}
static size_t p57(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,br=aang(ph,sp);
    for(int l=0;l<6;l++){float s=sc*(6-l)/6.f,rot=br+l*(M_PI/(4.f*6));float h=l/6.f;uint8_t r=(uint8_t)(fabsf(sinf(h*M_PI))*255),g=(uint8_t)(fabsf(sinf(h*M_PI+2.094f))*255),b=(uint8_t)(fabsf(sinf(h*M_PI+4.189f))*255);for(int i=0;i<=4;i++){int j=i%4,k2=(j+1)%4;float a0=PI2*j/4.f+rot,a1=PI2*k2/4.f+rot;for(int ss=0;ss<=12;ss++){float t=ss/12.f;ap(o,n,m,L(cosf(a0),cosf(a1),t)*s,L(sinf(a0),sinf(a1),t)*s,r,g,b,ss==0?1:0);}}}
    return n;
}
static size_t p58(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,pulse=sc*(.5f+.5f*fabsf(sinf(aang(ph,sp,3)))),rot=aang(ph,sp,.2f);
    for(int i=0;i<=128;i++){float a=PI2*i/128.f+rot,wave=1+.15f*sinf(8*a),r2=pulse*wave;ap(o,n,m,cosf(a)*r2,sinf(a)*r2,(uint8_t)(200+55*sinf(a)),0,(uint8_t)(200+55*cosf(a)),i==0?1:0);}
    return n;
}

// ─── KOMBI 59-63 ─────────────────────────────────────────────
static size_t p59(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,off=aang(ph,sp);
    for(int i=0;i<24;i++){float a=PI2*i/24.f+off,inner=sc*.3f,outer=sc*(.7f+.3f*sinf(i*.8f));for(int k=0;k<=10;k++){ap(o,n,m,cosf(a)*(inner+(outer-inner)*k/10.f),sinf(a)*(inner+(outer-inner)*k/10.f),(uint8_t)(128+127*sinf(a)),(uint8_t)(128+127*cosf(a)),255,k==0?1:0);}}
    return n;
}
static size_t p60(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t0=aang(ph,sp,.5f);
    for(int i=0;i<128;i++){float t=t0+PI2*i/128.f;ap(o,n,m,sc*.9f*sinf(3.1f*t)*cosf(.7f*t),sc*.9f*cosf(2.1f*t)*sinf(1.3f*t),(uint8_t)(128+127*sinf(t*2)),(uint8_t)(128+127*sinf(t*3+1)),(uint8_t)(128+127*cosf(t*1.5f)),i==0?1:0);}
    return n;
}
static size_t p61(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,rot=aang(ph,sp);
    const float ouV[][2]={{0,.9f},{.9f,0},{0,-.9f},{-.9f,0}};
    const float inV[][2]={{0,.55f},{.55f,0},{0,-.55f},{-.55f,0}};
    for(int i=0;i<=4;i++){int j=i%4,k2=(j+1)%4;for(int s=0;s<=16;s++){float t=s/16.f;float rx=L(ouV[j][0],ouV[k2][0],t)*sc*cosf(rot)-L(ouV[j][1],ouV[k2][1],t)*sc*sinf(rot);float ry=L(ouV[j][0],ouV[k2][0],t)*sc*sinf(rot)+L(ouV[j][1],ouV[k2][1],t)*sc*cosf(rot);ap(o,n,m,rx,ry,0,240,255,s==0?1:0);}}
    for(int i=0;i<=4;i++){int j=i%4,k2=(j+1)%4;for(int s=0;s<=16;s++){float t=s/16.f;float rx=L(inV[j][0],inV[k2][0],t)*sc*cosf(-rot)-L(inV[j][1],inV[k2][1],t)*sc*sinf(-rot);float ry=L(inV[j][0],inV[k2][0],t)*sc*sinf(-rot)+L(inV[j][1],inV[k2][1],t)*sc*cosf(-rot);ap(o,n,m,rx,ry,255,80,200,s==0?1:0);}}
    for(int i=0;i<=60;i++){float a=PI2*i/60.f+rot;ap(o,n,m,cosf(a)*.72f*sc,sinf(a)*.72f*sc,100,200,255,i==0?1:0);}
    return n;
}
static size_t p62(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    static const struct{float x,spd,off,r;}b[]={{-.4f,.3f,1.3f,.08f},{-.2f,.5f,2.1f,.06f},{.1f,.7f,.7f,.09f},{.3f,.4f,1.8f,.07f},{-.1f,.6f,3.2f,.05f},{.5f,.2f,2.5f,.08f},{-.5f,.8f,.4f,.07f}};
    for(auto& bl:b){float ph2=fmodf(t*bl.spd+bl.off,PI2),y=L(-1.f,1.f,ph2/PI2),wob=sinf(ph2*8)*.02f;for(int i=0;i<=16;i++){float a=PI2*i/16.f;ap(o,n,m,(cosf(a)*bl.r+bl.x+wob)*sc,(sinf(a)*bl.r+y)*sc,200,220,255,i==0?1:0);}}
    return n;
}
static size_t p63(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;float sc=SC*ssc(sz)*.9f,rot=aang(ph,sp);
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,cosf(a)*.7f*sc,sinf(a)*.7f*sc,200,200,200,i==0?1:0);}
    for(int row=-3;row<=3;row++){float y=row*.2f,rx=sqrtf(fmaxf(0.f,.49f-y*y));for(int i=0;i<=30;i++){float a=PI2*i/30.f+rot;ap(o,n,m,cosf(a)*rx*sc,y*sc,(uint8_t)(128+127*cosf(a+rot*2)),(uint8_t)(128+127*sinf(a+rot*3)),200,i==0?1:0);}}
    for(int i=0;i<8;i++){float a=PI2*i/8.f+rot*2,blen=.2f+.1f*fabsf(sinf(rot+i));for(int k=0;k<=6;k++)ap(o,n,m,(cosf(a)*.75f+cosf(a)*k/6.f*blen)*sc,(sinf(a)*.75f+sinf(a)*k/6.f*blen)*sc,255,255,255,k==0?1:0);}
    return n;
}

// ─── PARTY-SILHOUETTEN 64-89 ─────────────────────────────────
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
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,cosf(a)*.35f*sc,sinf(a)*.35f*sc,255,220,0,i==0?1:0);}
    for(int i=0;i<12;i++){float a=PI2*i/12.f+rot,len=.25f+.1f*sinf(i*2.3f+rot*2);for(int k=0;k<=8;k++){float t=k/8.f,r=L(.38f,.38f+len,t);ap(o,n,m,cosf(a)*r*sc,sinf(a)*r*sc,255,(uint8_t)(200-t*100),0,k==0?1:0);}}
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
    size_t n=0;float sc=SC*ssc(sz)*.9f,rot=aang(ph,sp,.2f);
    float gem[][2]={{0,.8f},{.55f,.25f},{.55f,-.1f},{0,-.9f},{-.55f,-.1f},{-.55f,.25f},{0,.8f},{.2f,.25f},{.55f,.25f},{-.2f,.25f},{-.55f,.25f},{0,.25f},{.55f,-.1f},{0,.25f},{-.55f,-.1f},{0,.25f},{0,-.9f}};
    for(size_t i=0;i<17;i++){float x=gem[i][0],y=gem[i][1];ap(o,n,m,(x*cosf(rot)-y*sinf(rot))*sc*.75f,(x*sinf(rot)+y*cosf(rot))*sc*.75f,180,220,255,i==0||i>=7?1:0);}
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
static size_t p82(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Konfetti-Burst
    size_t n=0;float sc=SC*ssc(sz)*.9f,t=aang(ph,sp);
    float s[][3]={{.3f,.6f,1.1f},{-.4f,.7f,2.3f},{.7f,.3f,3.5f},{-.6f,.4f,4.7f},{.2f,.8f,.8f},{-.3f,.5f,5.2f},{.5f,.6f,1.8f},{-.5f,.3f,2.9f},{.4f,.9f,4.1f},{-.2f,.7f,3.3f},{.6f,.2f,.5f},{-.7f,.6f,5.8f},{0,.9f,1.4f},{.8f,.4f,2.1f},{-.4f,.8f,3.8f}};
    for(auto&bl:s){float ang=fmodf(t*bl[0]+bl[2],PI2),r=bl[1]*sc,cx=cosf(ang)*r,cy=sinf(ang)*r,cp=ang+bl[2];uint8_t cr=(uint8_t)(128+127*sinf(cp)),cg=(uint8_t)(128+127*sinf(cp+2.1f)),cb=(uint8_t)(128+127*sinf(cp+4.2f));for(int i=0;i<=4;i++){float a=PI2*i/4.f+ang*3;ap(o,n,m,cx+cosf(a)*.06f*sc,cy+sinf(a)*.04f*sc,cr,cg,cb,i==0?1:0);}}
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
static size_t p89(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){ // Party-Finale
    // Lorenz attractor with color gradient
    size_t n=0;float sc=SC*ssc(sz)*.04f,t=aang(ph,sp)*10;
    float x=.1f,y=0,z=0,dt=.005f;
    for(int i=0;i<350;i++){float dx=10*(y-x),dy=x*(28-z)-y,dz=x*y-8.f/3*z;x+=dx*dt;y+=dy*dt;z+=dz*dt;float rot=t*.01f,px=x*cosf(rot)-z*sinf(rot);ap(o,n,m,px*sc,(y-20)*sc,(uint8_t)(128+127*sinf(i*.02f)),(uint8_t)(128+127*cosf(i*.02f)),150,i==0?1:0);}
    return n;
}



// ─── SZENEN 90 ─────────────────────────────────────────────
// Starfield: nStars points fall evenly from top (+Y) to bottom (-Y).
// Each star has a deterministic-random X position and individual
// fall speed. No Kreuz/line — pure single point per star.
// sp  = base speed (0..255 → Faktor 0..~5×)
// sz  = Sternanzahl (0→~20 stars, 255→~200 stars)
static size_t p90(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0;
    // Count stars from Size: 20..200
    const int nStars = 20 + (int)(sz / 255.f * 180.f);
    // base speed from Speed
    const float baseSpd = 0.3f + (sp / 255.f) * 4.7f;
    // Deterministische Pseudo-Zufallsfunktion (Wichmann-Hill aehnlich)
    auto fr = [](int seed) -> float {
        float x = sinf((float)seed * 127.1f + 1.f) * 43758.5453f;
        return x - floorf(x);
    };
    for (int i = 0; i < nStars; i++) {
        // Fixed X position per star, -1..+1 scaled to SC
        const float xPos  = (fr(i * 7)  * 2.f - 1.f) * SC * 0.95f;
        // Individual speed: 30%..170% of the base speed
        const float iSpd  = baseSpd * (0.3f + fr(i * 3) * 1.4f);
        // Phase offset so not all start at the top
        const float off   = fr(i * 5) * 2.2f;
        // Y runs from +1.1 to -1.1, then wraps -> top to bottom
        const float yNorm = 1.1f - fmodf(ph * iSpd * 0.0004f + off, 2.2f);
        if (yNorm < -1.1f || yNorm > 1.1f) continue;
        const float yPos  = yNorm * SC * 0.95f;
        // brightness slightly flickering (Seed from current period)
        const int period = (int)(ph * iSpd * 0.0004f);
        const uint8_t bright = (uint8_t)(80 + (int)(fr(i * 2 + period) * 175.f));
        const uint8_t blue   = (uint8_t)fminf(255.f, bright + 60.f);
        ap(o, n, m, xPos, yPos, 0,      0,      0,    1); // blank=1: move to star position (laser off)
        ap(o, n, m, xPos, yPos, bright, bright, blue, 0); // blank=0: single point dot (laser on)
    }
    return n;
}

// ─── ANIMIERTE FAHRZEUGE 91-98 ───────────────────────────────
// ph drives horizontal position: vehicle scrolls left→right, wraps.
// Speed (sp) controls scroll rate. Size (sz) scales the shape.

// Helper: draw a filled rectangle outline (4 segments)
static void rect_outline(LaserPoint*o,size_t&n,size_t m,float x0,float y0,float x1,float y1,uint8_t r,uint8_t g,uint8_t b,int S=10){
    line(o,n,m,x0,y0,x1,y0,r,g,b,S); line(o,n,m,x1,y0,x1,y1,r,g,b,S);
    line(o,n,m,x1,y1,x0,y1,r,g,b,S); line(o,n,m,x0,y1,x0,y0,r,g,b,S);
}
// Helper: full circle
static void circ_draw(LaserPoint*o,size_t&n,size_t m,float cx,float cy,float r2,uint8_t cr,uint8_t cg,uint8_t cb,int S=24){
    for(int i=0;i<=S;i++){float a=PI2*i/S;ap(o,n,m,cx+cosf(a)*r2,cy+sinf(a)*r2,cr,cg,cb,i==0?1:0);}
}
// Scroll X: vehicle travels left→right, wraps at screen edge
static float scrollX(uint32_t ph,uint8_t sp){
    float spd=0.06f+(sp/255.f)*0.44f;
    float t=fmodf(ph*spd*0.00025f,1.f);
    return (t*2.f-1.f)*SC*1.25f;
}

// p91 — Rocket
static size_t p91(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.85f, ox=scrollX(ph,sp);
    float bw=SC*.14f*sc2, bh=SC*.38f*sc2;
    // Body ellipse
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,ox+cosf(a)*bw,sinf(a)*bh,220,220,255,i==0?1:0);}
    // Nose cone
    line(o,n,m,ox-bw,bh,ox,bh+bh*.7f,255,200,200,10);
    line(o,n,m,ox+bw,bh,ox,bh+bh*.7f,255,200,200,10);
    // Fins
    line(o,n,m,ox-bw,-bh*.55f,ox-bw*2.5f,-bh,200,150,255,8);
    line(o,n,m,ox-bw*2.5f,-bh,ox-bw,-bh,200,150,255,8);
    line(o,n,m,ox+bw,-bh*.55f,ox+bw*2.5f,-bh,200,150,255,8);
    line(o,n,m,ox+bw*2.5f,-bh,ox+bw,-bh,200,150,255,8);
    // Exhaust flame
    float fl=bh*.5f+bh*.15f*fabsf(sinf(ph*0.18f));
    line(o,n,m,ox-bw*.6f,-bh,ox,-bh-fl,255,140,0,6);
    line(o,n,m,ox+bw*.6f,-bh,ox,-bh-fl,255,200,0,6);
    line(o,n,m,ox,-bh,ox,-bh-fl*1.2f,255,80,0,5);
    // Window
    circ_draw(o,n,m,ox,bh*.45f,bw*.52f,100,220,255,16);
    return n;
}

// p92 — Passenger Train
static size_t p92(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.7f, ox=scrollX(ph,sp);
    float lw=SC*.42f*sc2, lh=SC*.20f*sc2, ly=SC*.06f*sc2;
    // Loco body
    rect_outline(o,n,m,ox-lw,ly-lh,ox+lw,ly+lh,200,110,50,10);
    // Cab
    rect_outline(o,n,m,ox+lw*.25f,ly+lh,ox+lw,ly+lh*1.8f,190,100,45,8);
    // Chimney
    line(o,n,m,ox-lw*.5f,ly+lh,ox-lw*.5f,ly+lh*1.9f,160,80,40,6);
    circ_draw(o,n,m,ox-lw*.5f,ly+lh*2.0f,lw*.1f,160,80,40,12);
    // Smoke puffs
    for(int k=0;k<3;k++){float sy=ly+lh*2.2f+k*lw*.22f,sx=ox-lw*.5f+k*lw*.14f*(((int)(ph*.01f)+k)%2?1.f:-1.f);circ_draw(o,n,m,sx,sy,lw*.09f*(1.f+k*.25f),170,170,170,8);}
    // Wheels
    float wy=ly-lh, wr=SC*.1f*sc2;
    for(int k=0;k<4;k++){float wx=ox-lw*.75f+k*lw*.5f;circ_draw(o,n,m,wx,wy,wr,70,70,70,16);float rot=ph*0.06f;ap(o,n,m,wx+cosf(rot)*wr,wy+sinf(rot)*wr,120,120,120,1);ap(o,n,m,wx+cosf(rot+M_PI)*wr,wy+sinf(rot+M_PI)*wr,120,120,120,0);}
    // Rails
    line(o,n,m,-SC*.98f,wy-wr*1.1f,SC*.98f,wy-wr*1.1f,100,80,60,30);
    line(o,n,m,-SC*.98f,wy-wr*1.25f,SC*.98f,wy-wr*1.25f,100,80,60,30);
    return n;
}

// p93 — Racing Car
static size_t p93(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.75f, ox=scrollX(ph,sp);
    float bw=SC*.5f*sc2, bh=SC*.13f*sc2, by=-SC*.04f*sc2;
    // Body outline
    line(o,n,m,ox-bw,by,ox+bw,by,230,30,30,16);
    line(o,n,m,ox+bw,by,ox+bw,by+bh,230,30,30,6);
    line(o,n,m,ox+bw,by+bh,ox+bw*.1f,by+bh*1.9f,230,30,30,8);
    line(o,n,m,ox+bw*.1f,by+bh*1.9f,ox-bw*.35f,by+bh*1.6f,230,30,30,6);
    line(o,n,m,ox-bw*.35f,by+bh*1.6f,ox-bw,by,230,30,30,8);
    // Rear wing
    line(o,n,m,ox+bw*.65f,by+bh*1.3f,ox+bw*1.12f,by+bh*1.3f,255,255,255,6);
    line(o,n,m,ox+bw*.9f,by,ox+bw*.9f,by+bh*1.3f,200,200,200,4);
    // Wheels
    float wr=SC*.11f*sc2, wrot=ph*0.07f;
    for(int w=0;w<2;w++){
        float wx=w?ox+bw*.55f:ox-bw*.45f;
        circ_draw(o,n,m,wx,by,wr,50,50,50,18);
        ap(o,n,m,wx+cosf(wrot)*wr*.7f,by+sinf(wrot)*wr*.7f,120,120,120,1);
        ap(o,n,m,wx+cosf(wrot+M_PI)*wr*.7f,by+sinf(wrot+M_PI)*wr*.7f,120,120,120,0);
    }
    // Speed lines
    float spd2=0.15f+sp/255.f*.5f;
    for(int k=1;k<=5;k++){float len=SC*k*.06f*spd2;ap(o,n,m,ox-bw-len,by+bh*k*.14f,200,20,20,1);ap(o,n,m,ox-bw,by+bh*k*.14f,200,20,20,0);}
    return n;
}

// p94 — UFO / Flying Saucer
static size_t p94(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.85f, ox=scrollX(ph,sp);
    float hover=sinf(ph*0.035f)*SC*.08f*sc2;
    float dw=SC*.45f*sc2, dh=SC*.12f*sc2;
    // Disc body
    for(int i=0;i<=50;i++){float a=PI2*i/50.f;ap(o,n,m,ox+cosf(a)*dw,sinf(a)*dh+hover,0,200,255,i==0?1:0);}
    // Dome
    for(int i=0;i<=25;i++){float a=M_PI*i/25.f;ap(o,n,m,ox+cosf(a)*dw*.44f,sinf(a)*dh*1.5f+dh+hover,150,230,255,i==0?1:0);}
    // Portholes
    for(int k=-2;k<=2;k++){circ_draw(o,n,m,ox+k*dw*.38f,hover,dh*.35f,0,255,200,10);}
    // Tractor beam
    line(o,n,m,ox-dw*.25f,-dh+hover,ox-dw*.75f,-SC*.8f*sc2,0,150,255,8);
    line(o,n,m,ox+dw*.25f,-dh+hover,ox+dw*.75f,-SC*.8f*sc2,0,150,255,8);
    // Rotating lights
    for(int k=0;k<6;k++){float a=PI2*k/6.f+ph*0.06f;uint8_t br=(uint8_t)(128+127*sinf(a+ph*.1f));ap(o,n,m,ox+cosf(a)*dw,sinf(a)*dh+hover,br,br,0,0);}
    return n;
}

// p95 — Sailing Boat
static size_t p95(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.8f, ox=scrollX(ph,sp);
    float rock=sinf(ph*0.025f)*SC*.04f*sc2;
    float hw=SC*.52f*sc2, hh=SC*.12f*sc2, hy=-SC*.1f*sc2+rock;
    // Hull
    line(o,n,m,ox-hw,hy,ox+hw,hy,180,140,80,16);
    line(o,n,m,ox-hw,hy,ox-hw*.7f,hy-hh,180,140,80,8);
    line(o,n,m,ox+hw,hy,ox+hw*.7f,hy-hh,180,140,80,8);
    line(o,n,m,ox-hw*.7f,hy-hh,ox+hw*.7f,hy-hh,180,140,80,12);
    // Mast
    line(o,n,m,ox,hy-hh,ox,hy-hh+SC*.88f*sc2,200,200,200,10);
    // Main sail
    float mt=hy-hh+SC*.85f*sc2, mb=hy-hh+SC*.1f*sc2;
    line(o,n,m,ox,mt,ox+hw*.95f,hy,255,255,240,10);
    line(o,n,m,ox,mb,ox+hw*.95f,hy,255,255,240,10);
    line(o,n,m,ox,mb,ox,mt,255,255,240,6);
    // Jib
    line(o,n,m,ox,mt*.35f+mb*.65f,ox-hw*.65f,hy*.2f+mb*.8f,200,240,255,8);
    line(o,n,m,ox,mb,ox-hw*.65f,hy*.2f+mb*.8f,200,240,255,5);
    // Water waves
    for(int i=0;i<=50;i++){float wx=L(-SC*.98f,SC*.98f,i/50.f),wy=hy+hh*.5f+sinf(wx*.00015f+ph*.03f)*SC*.04f*sc2;ap(o,n,m,wx,wy,0,100,200,i==0?1:0);}
    return n;
}

// p96 — Bicycle
static size_t p96(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.75f, ox=scrollX(ph,sp);
    float wr=SC*.24f*sc2, ws=SC*.55f*sc2;
    float lw=ox-ws*.5f, rw=ox+ws*.5f, cy=0;
    float rot=ph*0.07f;
    // Wheels with rotating spokes
    for(int w=0;w<2;w++){
        float wx=w?rw:lw;
        circ_draw(o,n,m,wx,cy,wr,160,120,60,24);
        for(int s=0;s<4;s++){float a=rot+PI2*s/4.f;ap(o,n,m,wx+cosf(a)*wr,cy+sinf(a)*wr,120,90,50,1);ap(o,n,m,wx,cy,120,90,50,0);}
        // Hub
        circ_draw(o,n,m,wx,cy,wr*.15f,140,100,60,8);
    }
    // Bottom bracket
    float bbx=lw+ws*.45f, bby=cy;
    // Frame
    line(o,n,m,bbx,bby,lw,cy,140,140,140,8);              // chain stay
    line(o,n,m,bbx,bby,bbx,bby+wr*.8f,140,140,140,6);    // seat tube
    line(o,n,m,bbx,bby+wr*.8f,rw-wr*.15f,cy+wr*.4f+wr*.5f,140,140,140,8); // top tube
    line(o,n,m,bbx,bby,rw-wr*.15f,cy+wr*.4f+wr*.5f,140,140,140,8);        // down tube
    line(o,n,m,rw-wr*.15f,cy+wr*.4f+wr*.5f,rw-wr*.15f,cy+wr*.1f,140,140,140,6); // fork
    // Seat
    line(o,n,m,bbx-wr*.28f,bby+wr*.82f,bbx+wr*.28f,bby+wr*.82f,100,100,200,5);
    // Handlebar
    float hbx=rw-wr*.15f, hby=cy+wr*.5f+wr*.5f;
    line(o,n,m,hbx-wr*.22f,hby+wr*.35f,hbx+wr*.22f,hby+wr*.4f,180,180,255,5);
    return n;
}

// p97 — Airplane
static size_t p97(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.82f, ox=scrollX(ph,sp);
    float bank=sinf(ph*0.022f)*SC*.05f*sc2;
    float fw=SC*.55f*sc2, fh=SC*.1f*sc2;
    // Fuselage ellipse
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,ox+cosf(a)*fw,sinf(a)*fh+bank,220,230,255,i==0?1:0);}
    // Nose
    line(o,n,m,ox+fw,bank,ox+fw*1.42f,bank,220,230,255,8);
    // Main wings
    float wy=ox-fw*.1f;
    line(o,n,m,wy,bank-fh*.2f,wy-fw*1.3f,bank+fh*1.5f,220,230,255,14);
    line(o,n,m,wy,bank+fh*.45f,wy-fw*1.3f,bank+fh*1.5f,200,210,240,6);
    line(o,n,m,wy,bank-fh*.2f,wy+fw*.6f,bank+fh*1.5f,220,230,255,14);
    line(o,n,m,wy,bank+fh*.45f,wy+fw*.6f,bank+fh*1.5f,200,210,240,6);
    // Tail fin (vertical)
    line(o,n,m,ox-fw*.82f,bank,ox-fw*1.0f,bank+fh*2.8f,200,210,255,8);
    line(o,n,m,ox-fw*1.0f,bank+fh*2.8f,ox-fw*.6f,bank,200,210,255,5);
    // Tail wings
    line(o,n,m,ox-fw*.72f,bank,ox-fw*1.18f,bank+fh*1.2f,200,210,255,8);
    line(o,n,m,ox-fw*.72f,bank,ox-fw*.35f,bank+fh*1.2f,200,210,255,8);
    // Engines (2)
    for(int e=-1;e<=1;e+=2){float ex=ox+e*SC*.28f*sc2;circ_draw(o,n,m,ex,bank-fh*1.3f,fh*.65f,180,190,220,12);line(o,n,m,ex-fh,bank-fh*1.3f,ex+fh*1.8f,bank-fh*1.3f,180,190,220,8);}
    // Contrail
    for(int k=1;k<=5;k++){ap(o,n,m,ox-fw-SC*k*.07f*sc2,bank+(k%2?fh*.15f:-fh*.15f),210,220,255,1);ap(o,n,m,ox-fw-SC*(k+.5f)*.07f*sc2,bank,190,200,240,0);}
    return n;
}

// p98 — Space Shuttle (side view)
static size_t p98(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=ssc(sz)*0.72f, ox=scrollX(ph,sp);
    float tw=SC*.18f*sc2, th=SC*.5f*sc2;
    // External tank (center, orange)
    for(int i=0;i<=40;i++){float a=PI2*i/40.f;ap(o,n,m,ox+cosf(a)*tw,sinf(a)*th,200,110,50,i==0?1:0);}
    line(o,n,m,ox-tw,th,ox,th+tw*1.5f,200,110,50,8);
    line(o,n,m,ox+tw,th,ox,th+tw*1.5f,200,110,50,8);
    // Orbiter body (right, white)
    for(int i=0;i<=30;i++){float a=PI2*i/30.f;ap(o,n,m,ox+tw*2.0f+cosf(a)*tw*.7f,sinf(a)*th*.6f+th*.25f,210,215,230,i==0?1:0);}
    line(o,n,m,ox+tw*1.3f,th*.85f,ox+tw*2.7f,th*1.1f,210,215,230,6);
    line(o,n,m,ox+tw*2.7f,th*1.1f,ox+tw*2.0f,th*1.35f,210,215,230,6);
    // Delta wing
    line(o,n,m,ox+tw*1.3f,-th*.15f,ox+tw*3.4f,-th*.35f,180,185,210,10);
    line(o,n,m,ox+tw*2.7f,th*.25f,ox+tw*3.4f,-th*.35f,180,185,210,6);
    // SRB (left, white cylinder)
    for(int i=0;i<=30;i++){float a=PI2*i/30.f;ap(o,n,m,ox-tw*2.0f+cosf(a)*tw*.55f,sinf(a)*th*.72f,215,215,215,i==0?1:0);}
    // Main engine flames (3)
    float fl=th*.35f+th*.1f*fabsf(sinf(ph*0.22f));
    for(int e=-1;e<=1;e++){float ex=ox+e*tw*.85f;line(o,n,m,ex-tw*.2f,-th,ex,-th-fl,255,(uint8_t)(100+50*fabsf(sinf(ph*.25f+e))),0,6);line(o,n,m,ex+tw*.2f,-th,ex,-th-fl,255,200,0,4);}
    // SRB flame
    line(o,n,m,ox-tw*2.4f,-th*.72f,ox-tw*2.0f,-th*.72f-fl*.85f,255,150,0,6);
    line(o,n,m,ox-tw*1.6f,-th*.72f,ox-tw*2.0f,-th*.72f-fl*.85f,255,210,0,4);
    return n;
}

// ─── p99 — Airplane ────────────────────────────────────────────────────
static size_t p99(LaserPoint*o,size_t m,uint32_t ph,uint8_t sp,uint8_t sz){
    size_t n=0; float sc2=SC*ssc(sz)*0.7f, ox=scrollX(ph,sp);
    // Fuselage
    line(o,n,m, ox-sc2,0,            ox+sc2,0,            200,200,255, 20);
    // Wings
    line(o,n,m, ox-sc2*.1f,0,        ox-sc2*.4f, sc2*.45f, 200,200,255, 14);
    line(o,n,m, ox-sc2*.4f,sc2*.45f, ox-sc2*.1f,0,         200,200,255,  6);
    line(o,n,m, ox-sc2*.1f,0,        ox-sc2*.4f,-sc2*.45f, 200,200,255, 14);
    line(o,n,m, ox-sc2*.4f,-sc2*.45f,ox-sc2*.1f,0,         200,200,255,  6);
    // Tail fins
    line(o,n,m, ox-sc2*.75f,0,       ox-sc2,     sc2*.3f,  200,200,255,  8);
    line(o,n,m, ox-sc2*.75f,0,       ox-sc2,    -sc2*.3f,  200,200,255,  8);
    // Nose cone
    line(o,n,m, ox+sc2,0,            ox+sc2*.85f, sc2*.08f, 200,200,255,  4);
    line(o,n,m, ox+sc2,0,            ox+sc2*.85f,-sc2*.08f, 200,200,255,  4);
    // Engines under wings
    line(o,n,m, ox-sc2*.15f, sc2*.2f, ox-sc2*.45f, sc2*.2f,  180,220,255,  8);
    line(o,n,m, ox-sc2*.15f,-sc2*.2f, ox-sc2*.45f,-sc2*.2f,  180,220,255,  8);
    if(n>0&&n<m){LaserPoint cl=o[0];cl.blank=1;o[n++]=cl;}
    return n;
}

// ─── COUNTDOWN TIMER (p100) ─────────────────────────────────────────
// Renders a 7-segment-style countdown on the galvo.
// Actual countdown state is managed externally (countdown_timer namespace).
// This function only DRAWS the current remaining time.

// countdown_timer implementation is in countdown_timer.cpp

// 7-segment digit drawing — 4 segments left/right + 3 horizontal
// xc,yc = center; w,h = digit size; segments 0-6 = a-g (standard 7seg order)
static void seg7_digit(LaserPoint*o, size_t&n, size_t m,
                        float xc, float yc, float w, float h,
                        uint8_t digit, uint8_t r, uint8_t g, uint8_t b) {
    if (digit > 9) return;
    // Segment map: a=top, b=top-right, c=bot-right, d=bot, e=bot-left, f=top-left, g=mid
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
    // segment endpoints (a-g)
    struct Seg { float x0,y0,x1,y1; } segs[7] = {
        {xc-hw+mh, yc+hh,  xc+hw-mh, yc+hh      }, // a top
        {xc+hw,    yc+mh,  xc+hw,    yc+hh-mh    }, // b top-right
        {xc+hw,    yc-hh+mh, xc+hw,  yc-mh        }, // c bot-right
        {xc-hw+mh, yc-hh,  xc+hw-mh, yc-hh        }, // d bottom
        {xc-hw,    yc-hh+mh, xc-hw,  yc-mh         }, // e bot-left
        {xc-hw,    yc+mh,  xc-hw,    yc+hh-mh     }, // f top-left
        {xc-hw+mh, yc,     xc+hw-mh, yc            }, // g middle
    };
    for (int i = 0; i < 7; i++) {
        if (!seg[digit][i]) continue;
        ap(o,n,m, segs[i].x0, segs[i].y0, 0,0,0, 1); // blank move
        ap(o,n,m, segs[i].x1, segs[i].y1, r,g,b, 0); // lit segment
    }
}

static size_t p100(LaserPoint*o, size_t m, uint32_t ph, uint8_t sp, uint8_t sz) {
    size_t n = 0;
    countdown_timer::tick();

    uint32_t rem  = countdown_timer::remaining();
    bool     expr = countdown_timer::expired();

    // Color: green→yellow→red as time runs out
    uint8_t cr, cg, cb;
    if (expr) {
        // Blink red when expired
        bool blink = (ph % 60) < 30;
        cr = blink ? 255 : 0; cg = 0; cb = 0;
    } else if (rem == 0) {
        cr = 80; cg = 80; cb = 80;  // grey = stopped/reset
    } else if (rem <= 10) {
        cr = 255; cg = 40;  cb = 0;   // red: last 10s
    } else if (rem <= 30) {
        cr = 255; cg = 180; cb = 0;   // amber: last 30s
    } else {
        cr = 0;   cg = 220; cb = 80;  // green
    }

    uint32_t hh = rem / 3600;
    uint32_t mm = (rem % 3600) / 60;
    uint32_t ss = rem % 60;

    float sc2 = ssc(sz) * 0.85f;
    float dw  = SC * 0.18f * sc2;   // digit width
    float dh  = SC * 0.38f * sc2;   // digit height
    float gap = SC * 0.06f * sc2;   // gap between digits
    float cdot = SC * 0.04f * sc2;  // colon dot radius

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

    // Running indicator: small dot below if running
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

// ─── DISPATCH ────────────────────────────────────────────────
const PresetInfo PRESETS[PRESET_COUNT] = {
    {"Circle","Geometry"},{"Square","Geometry"},{"Triangle","Geometry"},{"Pentagon","Geometry"},{"Hexagon","Geometry"},{"Octagon","Geometry"},{"Star 4","Geometry"},{"Star 5","Geometry"},{"Star 6","Geometry"},{"Star 8","Geometry"},
    {"Cross +","Lines"},{"X Shape","Lines"},{"Grid 3x3","Lines"},{"H Line","Lines"},{"Diagonal","Lines"},
    {"Archimedean Spiral","Spirals"},{"Lissajous 1:2","Spirals"},{"Lissajous 2:3","Spirals"},{"Lissajous 3:4","Spirals"},{"Lissajous 3:5","Spirals"},{"Lissajous 5:6","Spirals"},{"Double Spiral","Spirals"},{"Rose 3","Spirals"},
    {"Rose 4","Curves"},{"Cardioid","Curves"},{"Heart","Curves"},{"Infinity","Curves"},{"Astroid","Curves"},{"Epitrochoid","Curves"},
    {"Rotating Cube","3D"},{"Static Cube","3D"},{"Pyramid","3D"},{"Octahedron","3D"},{"Tetrahedron","3D"},{"Dodecahedron","3D"},
    {"Sine Wave","Waves"},{"Standing Wave","Waves"},{"Multi Wave","Waves"},{"Ocean Wave","Waves"},{"Wave Interference","Waves"},{"Sawtooth","Waves"},{"Square Wave","Waves"},{"Wave Packet","Waves"},{"Beat Wave","Waves"},{"Radial Waves","Waves"},{"FM Wave","Waves"},{"Vortex","Waves"},{"Sine Helix","Waves"},{"Wave Field","Waves"},{"Fourier Square","Waves"},{"Gravity Waves","Waves"},{"Tsunami","Waves"},{"Wave Spectrum","Waves"},
    {"Hypotrochoid","Complex"},{"Butterfly","Complex"},{"Spirograph 5/3","Complex"},{"Concentric Rings","Complex"},{"Nested Squares","Complex"},{"Pulsing Circle","Complex"},
    {"Starburst","Combo"},{"Chaos Bouncer","Combo"},{"Laser Diamond","Combo"},{"Champagne Bubbles","Combo"},{"Confetti Burst","Combo"},{"Disco Ball","Combo"},
    {"Martini Glass","Party"},{"Wine Glass","Party"},{"Champagne Flute","Party"},{"Tropical Cocktail","Party"},{"Palm Tree","Party"},{"Flamingo","Party"},{"Tropical Fish","Party"},{"Water Splash","Party"},{"Pool Waves","Party"},{"Tropical Sun","Party"},{"Pineapple","Party"},{"Music Note","Party"},{"Balloon","Party"},{"Crown","Party"},{"Diamond","Party"},{"Cocktail Umbrella","Party"},{"Water Drop","Party"},{"Rising Bubbles","Party"},{"Confetti","Party"},{"Disco Ball 2","Party"},{"Sunset","Party"},{"Starfish","Party"},{"Hibiscus","Party"},{"Coconut Palm","Party"},{"Starburst Party","Party"},{"Party Finale","Party"},
    {"Starfield","Scenes"},
    {"Countdown Timer","Timers"},
    {"Rocket","Vehicles"},{"Train","Vehicles"},{"Racing Car","Vehicles"},{"UFO","Vehicles"},{"Sailing Boat","Vehicles"},{"Bicycle","Vehicles"},{"Airplane","Vehicles"},{"Space Shuttle","Vehicles"},
};

static const PFn DISPATCH[PRESET_COUNT] = {
    p00,p01,p02,p03,p04,p05,p06,p07,p08,p09,
    p10,p11,p12,p13,p14,
    p15,p16,p17,p18,p19,p20,p21,p22,
    p23,p24,p25,p26,p27,p28,
    p29,p30,p31,p32,p33,p34,
    p35,p36,p37,p38,p39,p40,p41,p42,p43,p44,p45,p46,p47,p48,p49,p50,p51,p52,
    p53,p54,p55,p56,p57,p58,
    p59,p60,p61,p62,p63,p83,
    p64,p65,p66,p67,p68,p69,p70,p71,p72,p73,p74,p75,p76,p77,p78,p79,p80,p81,p82,p83,p84,p85,p86,p87,p88,p89,
    p90,
    p100,
    p91,p92,p93,p94,p95,p96,p97,p98,
};

size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t speed, uint8_t size_val) {
    if (idx >= PRESET_COUNT || !out) return 0;
    // Phase overflow: after ~49 days at 1kHz phase would be > 4e9.
    // fmodf(ph * f) stays precise, but at very large ph
    // ph * factor may lose float precision. Clamp to safe range.
    const uint32_t safe_phase = phase % 0xFFFFFF;  // ~194 Tage @ 1kHz
    size_t n = DISPATCH[idx](out, max_pts, safe_phase, speed, size_val);

    // Centralized closing blank: many open-path presets (waves, spirals,
    // multi-segment silhouettes) draw from out[0] to out[n-1] without
    // returning to the start. When the engine loops the frame, the galvo
    // then jumps straight from the last lit point back to out[0] with the
    // laser still on -- a visible diagonal "retrace" line (dotted due to
    // galvo settling, seen on wireframes/waves/pyramids).
    // ngon()/star()/wf()/sinewave() already append their own closing
    // blank point identical to out[0], so this is a no-op for them.
    if (n > 0 && n < max_pts) {
        const LaserPoint& last = out[n - 1];
        const LaserPoint& first = out[0];
        bool already_closed = last.blank && last.x == first.x && last.y == first.y;
        if (!already_closed) {
            LaserPoint cl = first;
            cl.blank = 1;
            out[n++] = cl;
        }
    }
    return n;
}

} // namespace presets