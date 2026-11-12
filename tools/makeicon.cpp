// Renders the SFX-Parser icon (waveform with a cut region between a start and an end marker) into src/app.ico.
// Build: g++ -O2 tools/makeicon.cpp -o build/makeicon.exe && build/makeicon.exe src/app.ico
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
using namespace std;

struct C { float r, g, b, a; };
static C over(C d, C s) { float a = s.a + d.a * (1 - s.a); if (a <= 0) return {0,0,0,0};
    return {(s.r*s.a + d.r*d.a*(1-s.a))/a, (s.g*s.a + d.g*d.a*(1-s.a))/a, (s.b*s.a + d.b*d.a*(1-s.a))/a, a}; }

static bool inRR(float x, float y, float x0, float y0, float x1, float y1, float r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    float cx = x < x0 + r ? x0 + r : (x > x1 - r ? x1 - r : x), cy = y < y0 + r ? y0 + r : (y > y1 - r ? y1 - r : y);
    return (x-cx)*(x-cx) + (y-cy)*(y-cy) <= r*r;
}
static bool inTri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
    auto s = [](float x1,float y1,float x2,float y2,float x3,float y3){ return (x1-x3)*(y2-y3)-(x2-x3)*(y1-y3); };
    float d1 = s(px,py,ax,ay,bx,by), d2 = s(px,py,bx,by,cx,cy), d3 = s(px,py,cx,cy,ax,ay);
    bool neg = d1<0||d2<0||d3<0, pos = d1>0||d2>0||d3>0; return !(neg && pos);
}

static C shade(float u, float v) {
    if (!inRR(u, v, 0.02f, 0.02f, 0.98f, 0.98f, 0.20f)) return {0,0,0,0};
    C c = {0.08f + 0.06f*v, 0.10f + 0.10f*v, 0.17f + 0.20f*v, 1};                 // dark navy gradient
    const float mx0 = 0.36f, mx1 = 0.70f;
    if (u > mx0 && u < mx1) c = over(c, {0.25f, 0.55f, 0.85f, 0.28f});            // the cut region
    // waveform bars
    static const float h[] = {0.10f,0.22f,0.38f,0.26f,0.44f,0.30f,0.18f,0.34f,0.20f,0.09f};
    const int n = 10; const float x0 = 0.13f, x1 = 0.87f, pitch = (x1 - x0) / n, bw = pitch * 0.62f;
    for (int i = 0; i < n; i++) {
        float cx = x0 + pitch * (i + 0.5f);
        if (inRR(u, v, cx - bw/2, 0.54f - h[i], cx + bw/2, 0.54f + h[i], bw/2)) {
            bool in = cx > mx0 && cx < mx1;
            c = over(c, in ? C{0.55f, 0.88f, 1.0f, 1} : C{0.38f, 0.62f, 0.85f, 1});
        }
    }
    // markers: start (green, flag right) and end (red, flag left)
    const float lw = 0.018f, top = 0.14f, bot = 0.90f;
    if (fabsf(u - mx0) < lw && v > top && v < bot) c = {0.35f, 0.90f, 0.50f, 1};
    if (fabsf(u - mx1) < lw && v > top && v < bot) c = {0.95f, 0.36f, 0.36f, 1};
    if (inTri(u, v, mx0 - lw, top, mx0 + 0.15f, top, mx0 - lw, top + 0.15f)) c = {0.35f, 0.90f, 0.50f, 1};
    if (inTri(u, v, mx1 + lw, bot, mx1 - 0.15f, bot, mx1 + lw, bot - 0.15f)) c = {0.95f, 0.36f, 0.36f, 1};
    return c;
}

int main(int argc, char** argv) {
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256}, N = 7;
    vector<vector<uint8_t>> img(N);
    for (int k = 0; k < N; k++) {
        int s = sizes[k]; const int SS = 6;
        vector<uint8_t>& b = img[k];
        b.resize(40 + s*s*4 + s*((s+31)/32)*4);
        uint32_t* hd = (uint32_t*)b.data();
        hd[0] = 40; hd[1] = s; hd[2] = s*2; ((uint16_t*)b.data())[6] = 1; ((uint16_t*)b.data())[7] = 32; hd[4] = 0; hd[5] = s*s*4;
        uint8_t* px = b.data() + 40;
        for (int y = 0; y < s; y++) for (int x = 0; x < s; x++) {
            float r = 0, g = 0, bl = 0, a = 0;
            for (int j = 0; j < SS; j++) for (int i = 0; i < SS; i++) {
                C c = shade((x + (i + .5f)/SS)/s, (y + (j + .5f)/SS)/s);
                r += c.r*c.a; g += c.g*c.a; bl += c.b*c.a; a += c.a;
            }
            float inv = 1.f/(SS*SS); uint8_t* o = px + ((s-1-y)*s + x)*4;
            if (a > 0) { o[0] = (uint8_t)(bl/a*255+.5f); o[1] = (uint8_t)(g/a*255+.5f); o[2] = (uint8_t)(r/a*255+.5f); }
            o[3] = (uint8_t)(a*inv*255+.5f);
        }
    }
    FILE* f = fopen(argc > 1 ? argv[1] : "app.ico", "wb");
    if (!f) return 1;
    uint16_t hdr[3] = {0, 1, (uint16_t)N}; fwrite(hdr, 2, 3, f);
    uint32_t off = 6 + 16*N;
    for (int k = 0; k < N; k++) {
        int s = sizes[k]; uint8_t e[16] = {(uint8_t)(s==256?0:s), (uint8_t)(s==256?0:s), 0, 0, 1, 0, 32, 0};
        *(uint32_t*)(e+8) = (uint32_t)img[k].size(); *(uint32_t*)(e+12) = off; off += (uint32_t)img[k].size(); fwrite(e, 1, 16, f);
    }
    for (auto& b : img) fwrite(b.data(), 1, b.size(), f);
    fclose(f); return 0;
}
