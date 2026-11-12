// SFX-Parser - cut sound effects out of long recordings. Pure C++ / Win32 / GDI, one standalone exe.
// Decoding (WAV/MP3/FLAC) and MP3 encoding go through Windows Media Foundation, so no extra DLLs.
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shobjidl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

typedef long long i64;

// ----------------------------------------------------------------------------------------- settings
const bool kZoomCentersOnCursor = true;   // spec: the spot under the mouse becomes the centre of the view. false = keep it under the mouse
const int  kMp3Kbps   = 256;
const int  kBlock     = 128;              // frames per peak block
const int  kRuler     = 26, kBar = 36, kTab = 28, kTabW = 170, kStatus = 22, kGrip = 9, kTol = 6;
const wchar_t* kTitle = L"SFX-Parser";

enum { IDM_OPEN = 101, IDM_SAVE, IDM_EXIT, IDM_CLEAR, IDM_HELP,
       WM_LOADED = WM_APP + 1, WM_STATUS, WM_EXPORTED };

// ----------------------------------------------------------------------------------------- document
struct Doc {
    std::vector<float> pcm;               // interleaved stereo
    int rate = 44100;
    i64 frames = 0;
    std::vector<float> pk;                // min,max per block, both channels combined
    std::wstring path, name;              // name = file name without extension
};
std::shared_ptr<Doc> g_doc;

struct Cut { i64 s = 0, e = 0; bool sImp = false, eImp = false; };   // *Imp: boundary is implicit (neighbour / file edge), not a marker
std::vector<Cut> g_cuts;

// ----------------------------------------------------------------------------------------- state
HWND g_wnd;
HBITMAP g_bmp; int g_bmpW = 0, g_bmpH = 0;
double g_vs = 0, g_spp = 100;             // view start (frames), frames per pixel
double g_gain = 1.0;
bool g_loading = false, g_exporting = false;
std::vector<std::wstring> g_queue;                 // files waiting to be decoded
struct Tab { std::shared_ptr<Doc> doc; std::vector<Cut> cuts; double vs = 0, spp = 100, gain = 1; };
std::vector<Tab> g_tabs; int g_cur = -1;
std::wstring g_msg;
int g_mouseX = -1;
struct Hit { int cut = -1; bool end = false; };
Hit g_hover, g_drag; int g_dragOff = 0;
bool g_gripDrag = false; int g_gripY = 0; double g_gripGain = 1;
bool g_btnHover = false, g_panMoved = false; int g_panY = 0;
bool g_pan = false; int g_panX = 0; double g_panVs = 0;

// ----------------------------------------------------------------------------------------- helpers
std::wstring fmtTime(double sec) {
    if (sec < 0) sec = 0;
    int h = (int)(sec / 3600); sec -= h * 3600.0;
    int m = (int)(sec / 60);   sec -= m * 60.0;
    wchar_t b[64];
    if (h) swprintf(b, 64, L"%d:%02d:%06.3f", h, m, sec); else swprintf(b, 64, L"%d:%06.3f", m, sec);
    return b;
}
std::wstring hrs(const wchar_t* what, HRESULT h) { wchar_t b[200]; swprintf(b, 200, L"%ls failed (0x%08lx)", what, (unsigned long)h); return b; }
template <class T> struct Rel { T* p = nullptr; ~Rel() { if (p) p->Release(); } T** operator&() { return &p; } T* operator->() { return p; } };

void postStatus(const std::wstring& s) { PostMessageW(g_wnd, WM_STATUS, 0, (LPARAM) new std::wstring(s)); }

// ----------------------------------------------------------------------------------------- decode
std::shared_ptr<Doc> decodeFile(const std::wstring& path, std::wstring& err) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT h = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    std::shared_ptr<Doc> doc;
    if (FAILED(h)) { err = hrs(L"MFStartup", h); CoUninitialize(); return doc; }
    {
        Rel<IMFSourceReader> rd;
        h = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &rd);
        if (FAILED(h)) { err = hrs(L"Opening the file", h); goto done; }
        rd->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        rd->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

        UINT32 nativeRate = 44100;
        { Rel<IMFMediaType> nt; if (SUCCEEDED(rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &nt)))
              nt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &nativeRate); }
        const int rate = (nativeRate % 48000 == 0) ? 48000 : 44100;     // the MP3 encoder takes 44.1 / 48 kHz only
        Rel<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 8);
        h = rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, out.p);
        if (FAILED(h)) { err = hrs(L"Setting the decoded format", h); goto done; }

        doc = std::make_shared<Doc>();
        doc->rate = rate;
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(rd->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
            doc->pcm.reserve((size_t)((double)pv.uhVal.QuadPart / 1e7 * rate * 2) + 4096);
        PropVariantClear(&pv);

        i64 lastReport = 0;
        for (;;) {
            DWORD idx = 0, flags = 0; LONGLONG ts = 0;
            Rel<IMFSample> smp;
            h = rd->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &idx, &flags, &ts, &smp);
            if (FAILED(h)) { err = hrs(L"Decoding", h); doc.reset(); goto done; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!smp.p) continue;
            Rel<IMFMediaBuffer> buf;
            if (FAILED(smp->ConvertToContiguousBuffer(&buf))) continue;
            BYTE* d = nullptr; DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&d, nullptr, &len))) {
                const float* f = (const float*)d;
                doc->pcm.insert(doc->pcm.end(), f, f + len / 4);
                buf->Unlock();
            }
            if ((i64)doc->pcm.size() - lastReport > rate * 2 * 30) {
                lastReport = (i64)doc->pcm.size();
                postStatus(L"Decoding... " + fmtTime((double)doc->pcm.size() / 2 / rate));
            }
        }
        doc->pcm.resize(doc->pcm.size() / 2 * 2);
        doc->frames = (i64)doc->pcm.size() / 2;
        if (doc->frames < 2) { err = L"The file contains no audio."; doc.reset(); goto done; }
        const i64 nb = (doc->frames + kBlock - 1) / kBlock;
        doc->pk.resize((size_t)nb * 2);
        for (i64 b = 0; b < nb; b++) {
            float mn = 1e9f, mx = -1e9f;
            const i64 a = b * kBlock, z = std::min(doc->frames, a + kBlock);
            for (i64 i = a * 2; i < z * 2; i++) { float v = doc->pcm[(size_t)i]; mn = std::min(mn, v); mx = std::max(mx, v); }
            doc->pk[(size_t)b * 2] = mn; doc->pk[(size_t)b * 2 + 1] = mx;
        }
    }
done:
    MFShutdown();
    CoUninitialize();
    return doc;
}

// ----------------------------------------------------------------------------------------- MP3 encode
bool writeMp3(const std::wstring& path, const float* pcm, i64 frames, int rate, int kbps, std::wstring& err) {
    HRESULT h = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(h)) { err = hrs(L"MFStartup", h); return false; }
    bool ok = false;
    const int channels = 2;
    {
        Rel<IMFSinkWriter> writer;
        h = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer);
        if (FAILED(h)) { err = hrs(L"Creating the MP3 writer", h); goto done; }
        Rel<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kbps * 125);
        out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        DWORD stream = 0;
        h = writer->AddStream(out.p, &stream);
        if (FAILED(h)) { err = hrs(L"This Windows has no MP3 encoder for the format", h); goto done; }
        Rel<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
        h = writer->SetInputMediaType(stream, in.p, nullptr);
        if (FAILED(h)) { err = hrs(L"MP3 encoder input type", h); goto done; }
        h = writer->BeginWriting();
        if (FAILED(h)) { err = hrs(L"Starting the MP3 writer", h); goto done; }
        const i64 chunk = rate;
        for (i64 f0 = 0; f0 < frames; f0 += chunk) {
            const i64 n = std::min(chunk, frames - f0);
            const DWORD bytes = (DWORD)(n * channels * 2);
            Rel<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) { err = L"Out of memory"; goto done; }
            BYTE* dst = nullptr;
            buf->Lock(&dst, nullptr, nullptr);
            int16_t* o = (int16_t*)dst;
            for (i64 i = 0; i < n * channels; i++) {
                float v = pcm[(f0 * channels) + i]; v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
                o[i] = (int16_t)(v * 32767.f + (v >= 0 ? 0.5f : -0.5f));
            }
            buf->Unlock();
            buf->SetCurrentLength(bytes);
            Rel<IMFSample> smp;
            MFCreateSample(&smp);
            smp->AddBuffer(buf.p);
            smp->SetSampleTime((LONGLONG)((double)f0 / rate * 1e7));
            smp->SetSampleDuration((LONGLONG)((double)n / rate * 1e7));
            h = writer->WriteSample(stream, smp.p);
            if (FAILED(h)) { err = hrs(L"Writing audio", h); goto done; }
        }
        h = writer->Finalize();
        if (FAILED(h)) { err = hrs(L"Finishing the MP3 file", h); goto done; }
        ok = true;
    }
done:
    MFShutdown();
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// ----------------------------------------------------------------------------------------- cuts model
i64 total() { return g_doc ? g_doc->frames : 0; }

void normalize() {
    const size_t n = g_cuts.size();
    for (size_t i = 0; i < n; i++) {
        Cut& c = g_cuts[i];
        if (c.sImp) c.s = i ? g_cuts[i - 1].e : 0;
        if (c.eImp) c.e = i + 1 < n ? g_cuts[i + 1].s : total();
    }
    // an implicit start can depend on the previous implicit end: resolve left to right once more
    for (size_t i = 1; i < n; i++) if (g_cuts[i].sImp) g_cuts[i].s = g_cuts[i - 1].e;
}
bool circular() {
    for (size_t i = 0; i + 1 < g_cuts.size(); i++) if (g_cuts[i].eImp && g_cuts[i + 1].sImp) return true;
    return false;
}
size_t insertIndex(i64 f) {
    size_t i = 0;
    while (i < g_cuts.size() && g_cuts[i].s <= f) i++;
    return i;
}

// left click: new start point
void createStart(i64 f) {
    if (f < 0 || f >= total() - 1) return;
    for (auto& c : g_cuts) {
        if (f >= c.s && f < c.e) {
            if (c.sImp) { c.s = f; c.sImp = false; normalize(); }     // connects to this cut's pending end point
            return;                                                    // otherwise inside a subsample: never intersect
        }
    }
    Cut c; c.s = f; c.eImp = true;                                     // end = next start / end of file
    g_cuts.insert(g_cuts.begin() + insertIndex(f), c);
    normalize();
}
// right click: new end point
void createEnd(i64 f) {
    if (f <= 0 || f > total() - 1) return;
    for (auto& c : g_cuts) {
        if (f > c.s && f < c.e) {
            if (c.eImp) { c.e = f; c.eImp = false; normalize(); }     // connects to this cut's pending start point
            return;
        }
    }
    i64 prevEnd = 0;
    for (auto& c : g_cuts) if (c.e <= f) prevEnd = std::max(prevEnd, c.e);
    if (f <= prevEnd) return;
    Cut c; c.e = f; c.sImp = true;                                     // start = previous end / start of file
    g_cuts.insert(g_cuts.begin() + insertIndex(f), c);
    normalize();
}
void bounds(const Hit& h, i64& lo, i64& hi) {
    const Cut& c = g_cuts[h.cut];
    const size_t i = (size_t)h.cut, n = g_cuts.size();
    if (!h.end) { lo = i ? (g_cuts[i - 1].eImp ? g_cuts[i - 1].s + 1 : g_cuts[i - 1].e) : 0; hi = c.e - 1; }
    else        { lo = c.s + 1; hi = i + 1 < n ? (g_cuts[i + 1].sImp ? g_cuts[i + 1].e - 1 : g_cuts[i + 1].s) : total(); }
}
void moveMarker(const Hit& h, i64 f) {
    i64 lo, hi; bounds(h, lo, hi);
    f = std::max(lo, std::min(hi, f));
    Cut& c = g_cuts[h.cut];
    (h.end ? c.e : c.s) = f;
    normalize();
}
void removeMarker(const Hit& h) {
    if (h.cut < 0) return;
    auto snap = g_cuts;
    Cut& c = g_cuts[h.cut];
    if (h.end) { if (c.sImp) g_cuts.erase(g_cuts.begin() + h.cut); else c.eImp = true; }
    else       { if (c.eImp) g_cuts.erase(g_cuts.begin() + h.cut); else c.sImp = true; }
    if (circular()) { g_cuts = snap; return; }
    normalize();
}

// ----------------------------------------------------------------------------------------- view
RECT clientRc() { RECT r; GetClientRect(g_wnd, &r); return r; }
int waveTop() { return kBar + kTab + kRuler; }
int waveBot() { RECT r = clientRc(); return std::max(waveTop() + 20, (int)r.bottom - kStatus - kGrip); }
int viewW()   { RECT r = clientRc(); return std::max(1, (int)r.right); }
double frameAt(int x) { return g_vs + x * g_spp; }
int xOf(i64 f) { return (int)std::floor(((double)f - g_vs) / g_spp + 0.5); }

void clampView() {
    if (!g_doc) return;
    const double maxSpp = std::max(0.05, (double)g_doc->frames / viewW());
    g_spp = std::max(0.02, std::min(maxSpp, g_spp));
    const double maxVs = std::max(0.0, (double)g_doc->frames - viewW() * g_spp);
    g_vs = std::max(0.0, std::min(maxVs, g_vs));
}
void updateScroll() {
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    si.nMin = 0;
    if (g_doc) { si.nMax = (int)std::min<i64>(g_doc->frames, 2000000000); si.nPage = (UINT)std::min<double>(viewW() * g_spp, si.nMax); si.nPos = (int)g_vs; }
    SetScrollInfo(g_wnd, SB_HORZ, &si, TRUE);
}
void fitAll() { if (!g_doc) return; g_spp = (double)g_doc->frames / viewW(); g_vs = 0; clampView(); updateScroll(); }
void setStatus(const std::wstring& s) { g_msg = s; InvalidateRect(g_wnd, nullptr, FALSE); }

Hit hitTest(int mx, bool ctrl) {
    Hit hs, he; int bs = 1 << 30, be = 1 << 30;
    for (size_t i = 0; i < g_cuts.size(); i++) {
        const Cut& c = g_cuts[i];
        if (!c.sImp) { int d = std::abs(xOf(c.s) - mx); if (d <= kTol && d < bs) { bs = d; hs = {(int)i, false}; } }
        if (!c.eImp) { int d = std::abs(xOf(c.e) - mx); if (d <= kTol && d < be) { be = d; he = {(int)i, true}; } }
    }
    if (hs.cut >= 0 && he.cut >= 0) return ctrl ? hs : he;     // overlapping markers: end wins, Ctrl picks the start
    return hs.cut >= 0 ? hs : he;
}

// ----------------------------------------------------------------------------------------- paint
void drawText(HDC dc, int x, int y, const std::wstring& s, COLORREF col, UINT align = TA_LEFT) {
    SetTextColor(dc, col); SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, s.c_str(), (int)s.size());
}

void paintRuler(HDC dc, int W) {
    HBRUSH b = CreateSolidBrush(RGB(36, 38, 44)); RECT r{0, 0, W, kRuler}; FillRect(dc, &r, b); DeleteObject(b);
    if (!g_doc) return;
    static const double steps[] = {0.001,0.002,0.005,0.01,0.02,0.05,0.1,0.2,0.5,1,2,5,10,15,30,60,120,300,600,1800,3600};
    double step = steps[20];
    for (double s : steps) if (s * g_doc->rate / g_spp >= 90) { step = s; break; }
    const double t0 = std::ceil(g_vs / g_doc->rate / step) * step, tEnd = (g_vs + W * g_spp) / g_doc->rate;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 124, 134)); HGDIOBJ old = SelectObject(dc, pen);
    for (double t = t0; t <= tEnd + step; t += step) {
        int x = xOf((i64)std::llround(t * g_doc->rate));
        MoveToEx(dc, x, kRuler - 8, nullptr); LineTo(dc, x, kRuler);
        drawText(dc, x + 3, 3, fmtTime(t), RGB(190, 194, 204));
    }
    SelectObject(dc, old); DeleteObject(pen);
}

void paintWave(HDC dc, int W) {
    const int top = waveTop(), bot = waveBot(), mid = (top + bot) / 2, half = (bot - top) / 2 - 2;
    HBRUSH bg = CreateSolidBrush(RGB(22, 24, 28)); RECT wr{0, top, W, bot}; FillRect(dc, &wr, bg); DeleteObject(bg);
    if (!g_doc) { drawText(dc, W / 2, mid - 8, L"File > Open (Ctrl+O) to load a WAV, MP3 or FLAC file", RGB(130, 134, 144), TA_CENTER); return; }
    const Doc& d = *g_doc;

    // subsample regions
    HBRUSH reg = CreateSolidBrush(RGB(30, 52, 72));
    for (auto& c : g_cuts) {
        int x0 = std::max(0, xOf(c.s)), x1 = std::min(W, xOf(c.e));
        if (x1 > x0) { RECT r{x0, top, x1, bot}; FillRect(dc, &r, reg); }
    }
    DeleteObject(reg);

    SaveDC(dc);
    IntersectClipRect(dc, 0, top, W, bot);
    HPEN cl = CreatePen(PS_SOLID, 1, RGB(60, 64, 72)); HGDIOBJ old = SelectObject(dc, cl);
    MoveToEx(dc, 0, mid, nullptr); LineTo(dc, W, mid);
    SelectObject(dc, old); DeleteObject(cl);

    auto yOf = [&](float v) { int y = mid - (int)std::lround(v * g_gain * half); return std::max(top, std::min(bot - 1, y)); };
    HPEN wp = CreatePen(PS_SOLID, 1, RGB(110, 190, 245)); old = SelectObject(dc, wp);
    if (g_spp < 1.0) {                                     // below one sample per pixel: connect the samples
        const i64 a = std::max<i64>(0, (i64)std::floor(g_vs)), z = std::min(d.frames - 1, (i64)std::ceil(g_vs + W * g_spp) + 1);
        bool first = true;
        for (i64 i = a; i <= z; i++) {
            const float v = (d.pcm[(size_t)i * 2] + d.pcm[(size_t)i * 2 + 1]) * 0.5f;
            const int x = xOf(i), y = yOf(v);
            if (first) { MoveToEx(dc, x, y, nullptr); first = false; } else LineTo(dc, x, y);
            if (g_spp < 0.2) { RECT r{x - 2, y - 2, x + 3, y + 3}; FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH)); }
        }
    } else {
        for (int x = 0; x < W; x++) {
            const i64 fa = (i64)std::floor(frameAt(x));
            if (fa >= d.frames) break;
            if (fa < 0) continue;
            const i64 fb = std::min(d.frames, std::max(fa + 1, (i64)std::ceil(frameAt(x + 1))));
            float mn = 1e9f, mx = -1e9f;
            if (g_spp >= kBlock * 2) {
                for (i64 b = fa / kBlock, bz = (fb - 1) / kBlock; b <= bz; b++) { mn = std::min(mn, d.pk[(size_t)b * 2]); mx = std::max(mx, d.pk[(size_t)b * 2 + 1]); }
            } else {
                for (i64 i = fa * 2; i < fb * 2; i++) { float v = d.pcm[(size_t)i]; mn = std::min(mn, v); mx = std::max(mx, v); }
            }
            int y0 = yOf(mx), y1 = yOf(mn); if (y1 <= y0) y1 = y0 + 1;
            MoveToEx(dc, x, y0, nullptr); LineTo(dc, x, y1);
        }
    }
    SelectObject(dc, old); DeleteObject(wp);

    // markers
    for (size_t i = 0; i < g_cuts.size(); i++) {
        for (int e = 0; e < 2; e++) {
            const Cut& c = g_cuts[i];
            if (e ? c.eImp : c.sImp) continue;
            const int x = xOf(e ? c.e : c.s);
            if (x < -20 || x > W + 20) continue;
            const bool act = (g_drag.cut == (int)i && g_drag.end == (e != 0)) || (g_drag.cut < 0 && g_hover.cut == (int)i && g_hover.end == (e != 0));
            const COLORREF col = e ? RGB(240, 90, 90) : RGB(90, 220, 120);
            HPEN p = CreatePen(PS_SOLID, act ? 3 : 1, act ? RGB(255, 255, 255) : col); HGDIOBJ o = SelectObject(dc, p);
            MoveToEx(dc, x, top, nullptr); LineTo(dc, x, bot);
            SelectObject(dc, o); DeleteObject(p);
            HBRUSH fb = CreateSolidBrush(col); HGDIOBJ ob = SelectObject(dc, fb); HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            const int s = e ? -1 : 1;
            POINT tri[3] = {{x, top}, {x + s * 12, top}, {x, top + 12}};
            Polygon(dc, tri, 3);
            SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(fb);
            if (!e) drawText(dc, x + 14, top + 1, std::to_wstring(i + 1), RGB(190, 240, 205));
            else    drawText(dc, x - 14, top + 1, std::to_wstring(i + 1), RGB(250, 190, 190), TA_RIGHT);
        }
    }
    RestoreDC(dc, -1);
}

// ----------------------------------------------------------------------------------------- tabs
void saveCur() {
    if (g_cur < 0 || g_cur >= (int)g_tabs.size()) return;
    Tab& t = g_tabs[g_cur]; t.cuts = g_cuts; t.vs = g_vs; t.spp = g_spp; t.gain = g_gain;
}
void showTab(int i) {
    g_drag = Hit(); g_hover = Hit(); g_cur = i;
    if (i < 0) { g_doc.reset(); g_cuts.clear(); SetWindowTextW(g_wnd, kTitle); }
    else {
        Tab& t = g_tabs[i]; g_doc = t.doc; g_cuts = t.cuts; g_vs = t.vs; g_spp = t.spp; g_gain = t.gain;
        clampView(); SetWindowTextW(g_wnd, (g_doc->name + L" - " + kTitle).c_str());
    }
    updateScroll(); InvalidateRect(g_wnd, nullptr, FALSE);
}
void switchTab(int i) { if (i != g_cur) { saveCur(); showTab(i); } }
void addTab(const std::shared_ptr<Doc>& d) {
    saveCur();
    Tab t; t.doc = d; g_tabs.push_back(t);
    showTab((int)g_tabs.size() - 1);
    fitAll();
}
void closeTab(int i) {
    saveCur();
    g_tabs.erase(g_tabs.begin() + i);
    const int n = (int)g_tabs.size();
    int nc = g_cur;
    if (i < g_cur) nc = g_cur - 1; else if (i == g_cur) nc = std::min(i, n - 1);
    showTab(nc);
}
int tabW() { return g_tabs.empty() ? kTabW : std::min(kTabW, viewW() / (int)g_tabs.size()); }
// returns tab index under x (or -1); close = the x button was hit
int tabAt(int x, bool& close) {
    close = false;
    const int w = tabW(), i = x / std::max(1, w);
    if (i < 0 || i >= (int)g_tabs.size()) return -1;
    close = x - i * w >= w - 22;
    return i;
}
void paintTabs(HDC dc, int W) {
    HBRUSH b = CreateSolidBrush(RGB(30, 32, 37)); RECT r{0, 0, W, kTab}; FillRect(dc, &r, b); DeleteObject(b);
    const int tw = tabW();
    for (int i = 0; i < (int)g_tabs.size(); i++) {
        RECT t{i * tw, 3, (i + 1) * tw - 2, kTab};
        HBRUSH tb = CreateSolidBrush(i == g_cur ? RGB(52, 58, 72) : RGB(40, 43, 50)); FillRect(dc, &t, tb); DeleteObject(tb);
        if (i == g_cur) { RECT a{t.left, t.top, t.right, t.top + 2}; HBRUSH ab = CreateSolidBrush(RGB(110, 190, 245)); FillRect(dc, &a, ab); DeleteObject(ab); }
        RECT tr{t.left + 8, t.top, t.right - 22, t.bottom};
        SetTextColor(dc, i == g_cur ? RGB(235, 238, 245) : RGB(160, 165, 178));
        const std::wstring& nm = g_tabs[i].doc->name;
        DrawTextW(dc, nm.c_str(), (int)nm.size(), &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(dc, RGB(170, 174, 186)); SetTextAlign(dc, TA_CENTER | TA_TOP);
        TextOutW(dc, t.right - 11, t.top + 6, L"\x00D7", 1);
    }
}

// ----------------------------------------------------------------------------------------- toolbar
RECT saveBtnRect() { return RECT{8, 5, 190, kBar - 5}; }
bool saveEnabled() { return g_doc && !g_cuts.empty() && !g_exporting; }
void paintToolbar(HDC dc, int W) {
    HBRUSH b = CreateSolidBrush(RGB(30, 32, 37)); RECT r{0, 0, W, kBar}; FillRect(dc, &r, b); DeleteObject(b);
    const RECT br = saveBtnRect(); const bool en = saveEnabled();
    HBRUSH bb = CreateSolidBrush(!en ? RGB(44, 47, 54) : (g_btnHover ? RGB(70, 130, 190) : RGB(52, 100, 150)));
    HPEN np = (HPEN)GetStockObject(NULL_PEN); HGDIOBJ op = SelectObject(dc, np), ob = SelectObject(dc, bb);
    RoundRect(dc, br.left, br.top, br.right, br.bottom, 8, 8);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(bb);
    std::wstring t = L"Save Subsamples";
    if (g_doc && !g_cuts.empty()) t += L" (" + std::to_wstring(g_cuts.size()) + L")";
    SetTextColor(dc, en ? RGB(240, 245, 252) : RGB(120, 124, 134)); SetTextAlign(dc, TA_CENTER | TA_TOP);
    TextOutW(dc, (br.left + br.right) / 2, br.top + 7, t.c_str(), (int)t.size());
}
bool inSaveBtn(int x, int y) { RECT r = saveBtnRect(); return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// ----------------------------------------------------------------------------------------- playback
std::vector<uint8_t> g_playBuf;
int g_playCut = -1;
void stopPlay() { PlaySoundW(nullptr, nullptr, 0); g_playCut = -1; }
void playCutAt(int x) {
    if (!g_doc) return;
    const i64 f = (i64)std::floor(frameAt(x));
    int ci = -1;
    for (size_t i = 0; i < g_cuts.size(); i++) if (f >= g_cuts[i].s && f < g_cuts[i].e) ci = (int)i;
    if (ci < 0) { stopPlay(); return; }
    const Cut& c = g_cuts[ci];
    const i64 n = c.e - c.s; const uint32_t bytes = (uint32_t)(n * 4);
    stopPlay();
    g_playBuf.assign(44 + bytes, 0);
    uint8_t* p = g_playBuf.data();
    auto put32 = [&](int o, uint32_t v) { memcpy(p + o, &v, 4); };
    auto put16 = [&](int o, uint16_t v) { memcpy(p + o, &v, 2); };
    memcpy(p, "RIFF", 4); put32(4, 36 + bytes); memcpy(p + 8, "WAVEfmt ", 8); put32(16, 16); put16(20, 1); put16(22, 2);
    put32(24, g_doc->rate); put32(28, g_doc->rate * 4); put16(32, 4); put16(34, 16); memcpy(p + 36, "data", 4); put32(40, bytes);
    int16_t* o = (int16_t*)(p + 44);
    const float* s = &g_doc->pcm[(size_t)c.s * 2];
    for (i64 i = 0; i < n * 2; i++) { float v = std::max(-1.f, std::min(1.f, s[i])); o[i] = (int16_t)std::lround(v * 32767.f); }
    PlaySoundW((LPCWSTR)g_playBuf.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    g_playCut = ci;
}

void paintAll(HDC out) {
    RECT cr = clientRc(); const int W = cr.right, H = cr.bottom;
    if (!g_bmp || g_bmpW != W || g_bmpH != H) {
        if (g_bmp) DeleteObject(g_bmp);
        g_bmp = CreateCompatibleBitmap(out, W, H); g_bmpW = W; g_bmpH = H;
    }
    HDC dc = CreateCompatibleDC(out); HGDIOBJ ob = SelectObject(dc, g_bmp);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    HBRUSH bg = CreateSolidBrush(RGB(22, 24, 28)); FillRect(dc, &cr, bg); DeleteObject(bg);

    SetViewportOrgEx(dc, 0, kBar + kTab, nullptr); paintRuler(dc, W);
    SetViewportOrgEx(dc, 0, kBar, nullptr); paintTabs(dc, W);
    SetViewportOrgEx(dc, 0, 0, nullptr); paintToolbar(dc, W);
    paintWave(dc, W);

    // grip (bottom border of the waveform: drag = vertical scale)
    const int gt = waveBot();
    HBRUSH gb = CreateSolidBrush(g_gripDrag ? RGB(90, 120, 170) : RGB(52, 56, 66)); RECT gr{0, gt, W, gt + kGrip}; FillRect(dc, &gr, gb); DeleteObject(gb);
    HPEN gp = CreatePen(PS_SOLID, 1, RGB(130, 136, 150)); HGDIOBJ op = SelectObject(dc, gp);
    for (int k = -1; k <= 1; k++) { MoveToEx(dc, W / 2 - 24, gt + 4 + k * 2, nullptr); LineTo(dc, W / 2 + 24, gt + 4 + k * 2); }
    SelectObject(dc, op); DeleteObject(gp);

    // status
    HBRUSH sb = CreateSolidBrush(RGB(36, 38, 44)); RECT sr{0, H - kStatus, W, H}; FillRect(dc, &sr, sb); DeleteObject(sb);
    std::wstring left;
    if (!g_msg.empty()) left = g_msg;
    else if (g_doc) {
        wchar_t b[256]; swprintf(b, 256, L"%ls   %ls   %d Hz   cuts: %d   gain x%.2f", g_doc->name.c_str(), fmtTime((double)g_doc->frames / g_doc->rate).c_str(),
                                 g_doc->rate, (int)g_cuts.size(), g_gain);
        left = b;
    } else left = L"No file";
    drawText(dc, 8, H - kStatus + 4, left, RGB(200, 204, 214));
    if (g_doc && g_mouseX >= 0) drawText(dc, W - 8, H - kStatus + 4, fmtTime(std::max(0.0, frameAt(g_mouseX)) / g_doc->rate), RGB(200, 204, 214), TA_RIGHT);

    BitBlt(out, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob); DeleteDC(dc);
}

// ----------------------------------------------------------------------------------------- file actions
void pumpQueue() {
    if (g_loading || g_queue.empty()) return;
    const std::wstring path = g_queue.front(); g_queue.erase(g_queue.begin());
    g_loading = true;
    setStatus(L"Decoding " + path + L" ...");
    std::thread([path] {
        std::wstring err;
        auto doc = decodeFile(path, err);
        if (doc) {
            size_t sl = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.');
            if (dot == std::wstring::npos || (sl != std::wstring::npos && dot < sl)) dot = path.size();
            const size_t b0 = sl == std::wstring::npos ? 0 : sl + 1;
            doc->path = path; doc->name = path.substr(b0, dot - b0);
        }
        PostMessageW(g_wnd, WM_LOADED, doc ? 1 : 0, (LPARAM) new std::pair<std::shared_ptr<Doc>, std::wstring>(doc, err));
    }).detach();
}
void startLoad(const std::wstring& path) { g_queue.push_back(path); pumpQueue(); }
void doOpen() {
    static wchar_t buf[MAX_PATH * 40];
    buf[0] = 0;
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = g_wnd; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH * 40;
    ofn.lpstrFilter = L"Audio files (*.wav;*.mp3;*.flac)\0*.wav;*.mp3;*.flac\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    const std::wstring first = buf;
    const wchar_t* p = buf + first.size() + 1;
    if (!*p) startLoad(first);                                  // single file: full path
    else for (; *p; p += wcslen(p) + 1) startLoad(first + L"\\" + p);   // folder + names
}

bool pickFolder(std::wstring& out) {
    Rel<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return false;
    DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Save cuts to folder");
    if (d->Show(g_wnd) != S_OK) return false;
    Rel<IShellItem> it;
    if (FAILED(d->GetResult(&it))) return false;
    PWSTR p = nullptr;
    if (FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p; CoTaskMemFree(p);
    return true;
}

void doSave() {
    if (g_exporting) return;
    if (!g_doc || g_cuts.empty()) { MessageBoxW(g_wnd, L"Nothing to save - create some cuts first.", kTitle, MB_ICONINFORMATION); return; }
    std::wstring folder;
    if (!pickFolder(folder)) return;
    g_exporting = true;
    auto doc = g_doc; auto cuts = g_cuts;
    setStatus(L"Saving cuts...");
    std::thread([doc, cuts, folder] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring err; int done = 0;
        const int digits = std::max(2, (int)std::to_wstring(cuts.size()).size());
        for (size_t i = 0; i < cuts.size(); i++) {
            wchar_t name[64]; swprintf(name, 64, L"_%0*d.mp3", digits, (int)i + 1);
            std::wstring path = folder + L"\\" + doc->name + name;
            postStatus(L"Saving " + path.substr(path.find_last_of(L'\\') + 1) + L" ...");
            if (!writeMp3(path, &doc->pcm[(size_t)cuts[i].s * 2], cuts[i].e - cuts[i].s, doc->rate, kMp3Kbps, err)) break;
            done++;
        }
        CoUninitialize();
        wchar_t b[512];
        if (done == (int)cuts.size()) swprintf(b, 512, L"Saved %d cut(s) to %ls", done, folder.c_str());
        else swprintf(b, 512, L"Stopped after %d of %d cuts: %ls", done, (int)cuts.size(), err.c_str());
        PostMessageW(g_wnd, WM_EXPORTED, done == (int)cuts.size(), (LPARAM) new std::wstring(b));
    }).detach();
}

// ----------------------------------------------------------------------------------------- window
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_OPEN: doOpen(); break;
        case IDM_SAVE: doSave(); break;
        case IDM_CLEAR: g_cuts.clear(); InvalidateRect(h, nullptr, FALSE); break;
        case IDM_EXIT: DestroyWindow(h); break;
        case IDM_HELP:
            MessageBoxW(h,
                L"Left click: new start point (joins the nearest end point)\n"
                L"Right click: new end point (joins the nearest start point)\n"
                L"Drag a marker to move it; Ctrl while dragging picks the start point when markers overlap\n"
                L"Delete while hovering a marker: remove it\n\n"
                L"Mouse wheel: zoom   Shift+wheel / middle drag: scroll\n"
                L"Ctrl+wheel or drag the bar under the waveform: vertical scale (double-click resets)\n"
                L"Home: show the whole file\n\n"
                L"Cuts are saved as MP3, 256 kbps.", L"Help", MB_ICONINFORMATION);
            break;
        }
        return 0;
    case WM_LOADED: {
        auto* p = (std::pair<std::shared_ptr<Doc>, std::wstring>*)l;
        g_loading = false; g_msg.clear();
        if (w) addTab(p->first);
        else MessageBoxW(h, p->second.c_str(), kTitle, MB_ICONERROR);
        delete p; pumpQueue();
        InvalidateRect(h, nullptr, FALSE);
        return 0; }
    case WM_STATUS: { auto* s = (std::wstring*)l; g_msg = *s; delete s; InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_EXPORTED: {
        auto* s = (std::wstring*)l; g_exporting = false; g_msg = *s;
        if (!w) MessageBoxW(h, s->c_str(), kTitle, MB_ICONERROR);
        delete s; InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paintAll(dc); EndPaint(h, &ps); return 0; }
    case WM_SIZE: clampView(); updateScroll(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DROPFILES: {
        wchar_t buf[MAX_PATH * 2];
        const UINT n = DragQueryFileW((HDROP)w, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) if (DragQueryFileW((HDROP)w, i, buf, MAX_PATH * 2)) startLoad(buf);
        DragFinish((HDROP)w); return 0; }
    case WM_HSCROLL: {
        if (!g_doc) return 0;
        SCROLLINFO si{sizeof si, SIF_ALL}; GetScrollInfo(h, SB_HORZ, &si);
        double vs = g_vs; const double page = viewW() * g_spp;
        switch (LOWORD(w)) {
        case SB_LINELEFT: vs -= page / 10; break;
        case SB_LINERIGHT: vs += page / 10; break;
        case SB_PAGELEFT: vs -= page * 0.9; break;
        case SB_PAGERIGHT: vs += page * 0.9; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: vs = si.nTrackPos; break;
        }
        g_vs = vs; clampView(); updateScroll(); InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_MOUSEWHEEL: {
        if (!g_doc) return 0;
        POINT pt{(short)LOWORD(l), (short)HIWORD(l)}; ScreenToClient(h, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(w);
        const WORD keys = LOWORD(w);
        if (keys & MK_CONTROL) { g_gain = std::max(0.1, std::min(200.0, g_gain * (delta > 0 ? 1.2 : 1 / 1.2))); }
        else if (keys & MK_SHIFT) { g_vs -= delta / 120.0 * viewW() * g_spp * 0.1; clampView(); }
        else {
            const double f = frameAt(pt.x);
            g_spp *= delta > 0 ? 1 / 1.25 : 1.25;
            const double maxSpp = std::max(0.05, (double)g_doc->frames / viewW());
            g_spp = std::max(0.02, std::min(maxSpp, g_spp));
            g_vs = kZoomCentersOnCursor ? f - viewW() * 0.5 * g_spp : f - pt.x * g_spp;
            clampView();
        }
        updateScroll(); InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        const int x = (short)LOWORD(l), y = (short)HIWORD(l);
        SetFocus(h);
        if (!g_doc) return 0;
        if (y < kBar) { if (m == WM_LBUTTONDOWN && inSaveBtn(x, y) && saveEnabled()) doSave(); return 0; }
        if (y < kBar + kTab) {
            bool cl; const int ti = tabAt(x, cl);
            if (ti >= 0 && (cl || m == WM_MBUTTONDOWN)) closeTab(ti); else if (ti >= 0 && m == WM_LBUTTONDOWN) switchTab(ti);
            return 0;
        }
        if (m == WM_MBUTTONDOWN) { g_pan = true; g_panMoved = false; g_panY = y; g_panX = x; g_panVs = g_vs; SetCapture(h); return 0; }
        if (m == WM_LBUTTONDOWN && y >= waveBot() && y < waveBot() + kGrip) { g_gripDrag = true; g_gripY = y; g_gripGain = g_gain; SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (y < waveTop() || y >= waveBot()) return 0;
        if (m == WM_LBUTTONDOWN) {
            Hit hit = hitTest(x, (w & MK_CONTROL) != 0);
            if (hit.cut >= 0) {                                         // dragging beats creating
                g_drag = hit; const Cut& c = g_cuts[hit.cut];
                g_dragOff = xOf(hit.end ? c.e : c.s) - x;
                SetCapture(h);
            } else createStart((i64)std::llround(frameAt(x)));
        } else if (m == WM_RBUTTONDOWN) {
            if (hitTest(x, false).cut < 0) createEnd((i64)std::llround(frameAt(x)));
        }
        InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_LBUTTONDBLCLK: {
        const int y = (short)HIWORD(l);
        if (y >= waveBot() && y < waveBot() + kGrip) { g_gain = 1; InvalidateRect(h, nullptr, FALSE); }
        return 0; }
    case WM_MOUSEMOVE: {
        const int x = (short)LOWORD(l), y = (short)HIWORD(l);
        g_mouseX = x;
        { const bool hb = inSaveBtn(x, y) && saveEnabled(); if (hb != g_btnHover) g_btnHover = hb; }
        if (g_gripDrag) { g_gain = std::max(0.1, std::min(200.0, g_gripGain * std::pow(1.012, g_gripY - y))); }
        else if (g_pan) { if (std::abs(x - g_panX) > 3) g_panMoved = true; g_vs = g_panVs - (x - g_panX) * g_spp; clampView(); updateScroll(); }
        else if (g_drag.cut >= 0) { moveMarker(g_drag, (i64)std::llround(frameAt(x + g_dragOff))); }
        else g_hover = (g_doc && y >= waveTop() && y < waveBot()) ? hitTest(x, (GetKeyState(VK_CONTROL) & 0x8000) != 0) : Hit();
        InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_LBUTTONUP: case WM_MBUTTONUP:
        if (m == WM_MBUTTONUP && g_pan && !g_panMoved && g_panY >= waveTop() && g_panY < waveBot()) playCutAt((short)LOWORD(l));
        if (g_drag.cut >= 0 || g_gripDrag || g_pan) ReleaseCapture();
        g_drag = Hit(); g_gripDrag = false; g_pan = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSELEAVE: g_mouseX = -1; return 0;
    case WM_KEYDOWN:
        if (w == VK_DELETE && g_hover.cut >= 0 && g_drag.cut < 0) { removeMarker(g_hover); g_hover = Hit(); InvalidateRect(h, nullptr, FALSE); }
        else if (w == VK_SPACE) { POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt); if (pt.y >= waveTop() && pt.y < waveBot()) playCutAt(pt.x); else stopPlay(); }
        else if (w == VK_HOME) { fitAll(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            LPCWSTR c = IDC_ARROW;
            if (pt.y < kBar && inSaveBtn(pt.x, pt.y) && saveEnabled()) c = IDC_HAND;
            if (g_gripDrag || (pt.y >= waveBot() && pt.y < waveBot() + kGrip)) c = IDC_SIZENS;
            else if (g_pan) c = IDC_SIZEALL;
            else if (g_drag.cut >= 0 || (g_doc && pt.y >= waveTop() && pt.y < waveBot() && hitTest(pt.x, (GetKeyState(VK_CONTROL) & 0x8000) != 0).cut >= 0)) c = IDC_SIZEWE;
            else if (g_doc && pt.y >= waveTop() && pt.y < waveBot()) c = IDC_CROSS;
            SetCursor(LoadCursorW(nullptr, c)); return TRUE;
        }
        break;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)l)->ptMinTrackSize = {480, 260}; return 0;
    case WM_DESTROY: stopPlay(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"SFXParserWnd";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    HMENU bar = CreateMenu(), file = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE, L"&Save Cuts...\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_CLEAR, L"&Clear all cuts");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(help, MF_STRING, IDM_HELP, L"&Controls");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");

    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 520, nullptr, bar, inst, nullptr);
    ACCEL acc[] = {{FVIRTKEY | FCONTROL, 'O', IDM_OPEN}, {FVIRTKEY | FCONTROL, 'S', IDM_SAVE}};
    HACCEL ha = CreateAcceleratorTableW(acc, 2);
    updateScroll();
    ShowWindow(g_wnd, show);

    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) startLoad(argv[i]);
    LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(g_wnd, ha, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
