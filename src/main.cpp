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
#include <atomic>
#include <mutex>
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
const int  kRuler     = 26, kBar = 48, kTab = 28, kTabW = 170, kStatus = 22, kGrip = 9, kTol = 6;
const wchar_t* kTitle = L"SFX-Parser";

enum { IDM_OPEN = 101, IDM_SAVE, IDM_EXIT, IDM_CLEAR, IDM_HELP, IDM_SAVEAS, IDM_UNDO, IDM_REDO,
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

// *Imp: boundary is implicit (neighbour / file edge), not a marker. vol = linear volume applied when playing and saving.
struct Cut { i64 s = 0, e = 0; bool sImp = false, eImp = false; int id = 0; float vol = 1.f; };
std::vector<Cut> g_cuts;
int g_nextId = 1;
struct Fx { int id; float oldV, newV; };                 // one applied sound effect (a volume change) for undo / redo
std::vector<Fx> g_undo, g_redo;
int g_lastPlayed = -1, g_target = -1;                    // cut ids: last played / last hovered
double g_playPos = 0;                                    // play marker, absolute frame inside the last played cut
double g_speed = 1;                                      // 1, 3 or -3

// audio engine: waveOut fed by a thread. step = source frames per output frame (negative = backwards)
const int kEngFrames = 1536;
struct Doc;
struct Engine {
    HWAVEOUT hwo = nullptr; WAVEHDR hdr[4]{}; bool queued[4]{}; std::vector<int16_t> buf[4];
    HANDLE ev = nullptr; std::thread th; std::mutex m; std::atomic<bool> quit{false};
    std::shared_ptr<Doc> doc; i64 s = 0, e = 0; double src = 0, step = 1; i64 written = 0; bool eof = false;
    std::atomic<float> gain{1.f}; bool active = false, paused = false; int cutId = -1;
};
Engine g_eng;
void stopPlay();

// ----------------------------------------------------------------------------------------- state
HWND g_wnd;
HBITMAP g_bmp; int g_bmpW = 0, g_bmpH = 0;
double g_vs = 0, g_spp = 100;             // view start (frames), frames per pixel
double g_gain = 1.0;
bool g_loading = false, g_exporting = false;
std::vector<std::wstring> g_queue;                 // files waiting to be decoded
struct Tab { std::shared_ptr<Doc> doc; std::vector<Cut> cuts; double vs = 0, spp = 100, gain = 1;
             std::vector<Fx> undo, redo; int lastPlayed = -1, target = -1; };
std::vector<Tab> g_tabs; int g_cur = -1;
std::wstring g_msg;
int g_mouseX = -1;
struct Hit { int cut = -1; bool end = false; };
Hit g_hover, g_drag; int g_dragOff = 0;
bool g_gripDrag = false; int g_gripY = 0; double g_gripGain = 1;
bool g_panMoved = false; int g_panY = 0;
int g_hoverBtn = -1;                                     // toolbar button under the mouse
bool g_playDrag = false, g_playDragWas = false; int g_playDragOff = 0;
bool g_volDrag = false; float g_volOld = 1.f; int g_volId = -1;
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
bool writeMp3(const std::wstring& path, const float* pcm, i64 frames, int rate, int kbps, float gain, std::wstring& err) {
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
                float v = pcm[(f0 * channels) + i] * gain; v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
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
    Cut c; c.s = f; c.eImp = true; c.id = g_nextId++;                  // end = next start / end of file
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
    Cut c; c.e = f; c.sImp = true; c.id = g_nextId++;                  // start = previous end / start of file
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
    if (g_eng.active) stopPlay();                                      // the range being played may change
    Cut& c = g_cuts[h.cut];
    (h.end ? c.e : c.s) = f;
    normalize();
}
void removeMarker(const Hit& h) {
    if (h.cut < 0) return;
    if (g_eng.active) stopPlay();
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
unsigned long long g_msgTick = 0;
void setStatus(const std::wstring& s) { g_msg = s; g_msgTick = GetTickCount64(); InvalidateRect(g_wnd, nullptr, FALSE); }

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
Cut* cutById(int id) { for (auto& c : g_cuts) if (c.id == id) return &c; return nullptr; }
int cutIndexById(int id) { for (size_t i = 0; i < g_cuts.size(); i++) if (g_cuts[i].id == id) return (int)i; return -1; }
std::vector<int> hlIds();                                          // defined with the toolbar
void drawPlayhead(HDC dc, int W, int top, int bot);

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
    const std::vector<int> hl = hlIds();                  // subsamples the hovered toolbar button will affect
    auto isHl = [&](int id) { return std::find(hl.begin(), hl.end(), id) != hl.end(); };
    HBRUSH reg = CreateSolidBrush(RGB(30, 52, 72)), regHl = CreateSolidBrush(RGB(96, 82, 26));
    for (auto& c : g_cuts) {
        int x0 = std::max(0, xOf(c.s)), x1 = std::min(W, xOf(c.e));
        if (x1 > x0) { RECT r{x0, top, x1, bot}; FillRect(dc, &r, isHl(c.id) ? regHl : reg); }
    }
    DeleteObject(reg); DeleteObject(regHl);

    SaveDC(dc);
    IntersectClipRect(dc, 0, top, W, bot);
    HPEN cl = CreatePen(PS_SOLID, 1, RGB(60, 64, 72)); HGDIOBJ old = SelectObject(dc, cl);
    MoveToEx(dc, 0, mid, nullptr); LineTo(dc, W, mid);
    SelectObject(dc, old); DeleteObject(cl);

    auto yOf = [&](float v) { int y = mid - (int)std::lround(v * g_gain * half); return std::max(top, std::min(bot - 1, y)); };
    HPEN wp = CreatePen(PS_SOLID, 1, RGB(110, 190, 245)), dimP = CreatePen(PS_SOLID, 1, RGB(62, 78, 96)), clipP = CreatePen(PS_SOLID, 1, RGB(255, 90, 80));
    old = SelectObject(dc, wp);
    auto volAt = [&](i64 f) { for (auto& c : g_cuts) if (f >= c.s && f < c.e) return c.vol; return 1.f; };
    if (g_spp < 1.0) {                                     // below one sample per pixel: connect the samples
        const i64 a = std::max<i64>(0, (i64)std::floor(g_vs)), z = std::min(d.frames - 1, (i64)std::ceil(g_vs + W * g_spp) + 1);
        bool first = true;
        for (i64 i = a; i <= z; i++) {
            const float v = (d.pcm[(size_t)i * 2] + d.pcm[(size_t)i * 2 + 1]) * 0.5f * volAt(i);
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
            const float vol = volAt(fa);
            if (vol != 1.f) {                                  // faint original under the volume-scaled wave
                SelectObject(dc, dimP);
                int o0 = yOf(mx), o1 = yOf(mn); if (o1 <= o0) o1 = o0 + 1;
                MoveToEx(dc, x, o0, nullptr); LineTo(dc, x, o1);
            }
            SelectObject(dc, std::max(std::fabs(mn), std::fabs(mx)) * vol > 1.0005f ? clipP : wp);   // red where the new volume clips
            int y0 = yOf(mx * vol), y1 = yOf(mn * vol); if (y1 <= y0) y1 = y0 + 1;
            MoveToEx(dc, x, y0, nullptr); LineTo(dc, x, y1);
        }
    }
    SelectObject(dc, old); DeleteObject(wp); DeleteObject(dimP); DeleteObject(clipP);

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
    // outline of the subsamples that the hovered button will affect
    { HBRUSH fb = CreateSolidBrush(RGB(255, 210, 70));
      for (auto& c : g_cuts) if (isHl(c.id)) {
          const int x0 = xOf(c.s), x1 = xOf(c.e);
          RECT r{x0, top, x1 + 1, bot}; FrameRect(dc, &r, fb);
          RECT r2{x0 + 1, top + 1, x1, bot - 1}; FrameRect(dc, &r2, fb);
      }
      DeleteObject(fb); }
    drawPlayhead(dc, W, top, bot);
    RestoreDC(dc, -1);
}

// ----------------------------------------------------------------------------------------- tabs
void saveCur() {
    if (g_cur < 0 || g_cur >= (int)g_tabs.size()) return;
    Tab& t = g_tabs[g_cur]; t.cuts = g_cuts; t.vs = g_vs; t.spp = g_spp; t.gain = g_gain;
    t.undo = g_undo; t.redo = g_redo; t.lastPlayed = g_lastPlayed; t.target = g_target;
}
void showTab(int i) {
    g_drag = Hit(); g_hover = Hit(); g_cur = i; g_playDrag = false; g_volDrag = false;
    if (i < 0) { g_doc.reset(); g_cuts.clear(); g_undo.clear(); g_redo.clear(); g_lastPlayed = g_target = -1; SetWindowTextW(g_wnd, kTitle); }
    else {
        Tab& t = g_tabs[i]; g_doc = t.doc; g_cuts = t.cuts; g_vs = t.vs; g_spp = t.spp; g_gain = t.gain;
        g_undo = t.undo; g_redo = t.redo; g_lastPlayed = t.lastPlayed; g_target = t.target;
        if (Cut* c = cutById(g_lastPlayed)) g_playPos = (double)c->s;
        clampView(); SetWindowTextW(g_wnd, (g_doc->name + L" - " + kTitle).c_str());
    }
    updateScroll(); InvalidateRect(g_wnd, nullptr, FALSE);
}
void switchTab(int i) { if (i != g_cur) { stopPlay(); saveCur(); showTab(i); } }
void addTab(const std::shared_ptr<Doc>& d) {
    stopPlay(); saveCur();
    Tab t; t.doc = d; g_tabs.push_back(t);
    showTab((int)g_tabs.size() - 1);
    fitAll();
}
void closeTab(int i) {
    if (i == g_cur) stopPlay();                                    // closing the file cuts its playback
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

// ----------------------------------------------------------------------------------------- audio engine
i64 engPlayed() {                                  // output frames the device has played since the last reset (lock held)
    MMTIME t{}; t.wType = TIME_SAMPLES;
    if (!g_eng.hwo || waveOutGetPosition(g_eng.hwo, &t, sizeof t) != MMSYSERR_NOERROR || t.wType != TIME_SAMPLES) return 0;
    return (i64)t.u.sample;
}
int engFill(int i) {                               // lock held; returns frames produced
    Engine& E = g_eng; std::vector<int16_t>& b = E.buf[i];
    const float g = E.gain.load(); const int st = std::max(1, (int)std::lround(std::fabs(E.step))), dir = E.step < 0 ? -1 : 1;
    const float* pcm = E.doc->pcm.data();
    int n = 0;
    for (; n < kEngFrames; n++) {
        const i64 p = (i64)std::floor(E.src);
        if (dir > 0 ? p >= E.e : p < E.s) { E.eof = true; break; }
        float l = 0, r = 0; int cnt = 0;
        for (int k = 0; k < st; k++) { const i64 q = p + dir * k; if (q < E.s || q >= E.e) break; l += pcm[q * 2]; r += pcm[q * 2 + 1]; cnt++; }
        if (cnt) { l = l / cnt * g; r = r / cnt * g; }
        l = std::max(-1.f, std::min(1.f, l)); r = std::max(-1.f, std::min(1.f, r));
        b[(size_t)n * 2] = (int16_t)std::lround(l * 32767.f); b[(size_t)n * 2 + 1] = (int16_t)std::lround(r * 32767.f);
        E.src += E.step;
    }
    E.written += n;
    return n;
}
void engThread() {
    Engine& E = g_eng;
    while (!E.quit) {
        WaitForSingleObject(E.ev, 30);
        std::lock_guard<std::mutex> lk(E.m);
        if (!E.active) continue;
        for (int i = 0; i < 4 && !E.eof; i++) {
            if (E.queued[i] && !(E.hdr[i].dwFlags & WHDR_DONE)) continue;
            const int n = engFill(i);
            if (n <= 0) { E.queued[i] = false; continue; }
            E.hdr[i].dwBufferLength = (DWORD)n * 4;
            E.queued[i] = true;
            waveOutWrite(E.hwo, &E.hdr[i], sizeof(WAVEHDR));
        }
    }
}
void engStop() {
    Engine& E = g_eng;
    if (!E.hwo) return;
    { std::lock_guard<std::mutex> lk(E.m); E.active = false; }
    E.quit = true; SetEvent(E.ev);
    if (E.th.joinable()) E.th.join();
    waveOutReset(E.hwo);
    for (int i = 0; i < 4; i++) waveOutUnprepareHeader(E.hwo, &E.hdr[i], sizeof(WAVEHDR));
    waveOutClose(E.hwo); CloseHandle(E.ev);
    E.hwo = nullptr; E.ev = nullptr; E.doc.reset(); E.cutId = -1; E.paused = false;
}
bool engStart(const std::shared_ptr<Doc>& doc, int cutId, i64 s, i64 e, double from, double step, float gain) {
    Engine& E = g_eng;
    engStop();
    WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = doc->rate; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = doc->rate * 4;
    E.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&E.hwo, WAVE_MAPPER, &wf, (DWORD_PTR)E.ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) { CloseHandle(E.ev); E.ev = nullptr; E.hwo = nullptr; return false; }
    for (int i = 0; i < 4; i++) {
        E.buf[i].assign((size_t)kEngFrames * 2, 0);
        E.hdr[i] = WAVEHDR{}; E.hdr[i].lpData = (LPSTR)E.buf[i].data(); E.hdr[i].dwBufferLength = kEngFrames * 4;
        waveOutPrepareHeader(E.hwo, &E.hdr[i], sizeof(WAVEHDR)); E.queued[i] = false;
    }
    E.doc = doc; E.s = s; E.e = e; E.src = from; E.step = step; E.written = 0; E.eof = false; E.gain = gain;
    E.cutId = cutId; E.paused = false; E.quit = false; E.active = true;
    E.th = std::thread(engThread);
    SetEvent(E.ev);
    return true;
}
double engPosLocked() {
    Engine& E = g_eng;
    const i64 q = std::max<i64>(0, E.written - engPlayed());
    return std::max((double)E.s, std::min((double)E.e - 1, E.src - (double)q * E.step));
}
double engPos() { std::lock_guard<std::mutex> lk(g_eng.m); return g_eng.active ? engPosLocked() : -1; }
bool engEnded() { std::lock_guard<std::mutex> lk(g_eng.m); return g_eng.active && g_eng.eof && engPlayed() >= g_eng.written; }
void engPause()  { std::lock_guard<std::mutex> lk(g_eng.m); if (g_eng.active && !g_eng.paused) { waveOutPause(g_eng.hwo); g_eng.paused = true; } }
void engResume() { std::lock_guard<std::mutex> lk(g_eng.m); if (g_eng.active && g_eng.paused) { waveOutRestart(g_eng.hwo); g_eng.paused = false; } }
void engSeekLocked(double pos, double step) {
    Engine& E = g_eng;
    waveOutReset(E.hwo);                           // flushes the queue, rewinds the device position to 0
    E.written = 0; E.eof = false; E.step = step;
    E.src = std::max((double)E.s, std::min((double)E.e - 1, pos));
    for (int i = 0; i < 4; i++) E.queued[i] = false;
    if (E.paused) waveOutPause(E.hwo);
    SetEvent(E.ev);
}
void engSeek(double pos) { std::lock_guard<std::mutex> lk(g_eng.m); if (g_eng.active) engSeekLocked(pos, g_eng.step); }
void engSetStep(double step) { std::lock_guard<std::mutex> lk(g_eng.m); if (g_eng.active) engSeekLocked(engPosLocked(), step); }

void stopPlay() {
    if (g_eng.active) engStop();
    if (Cut* c = cutById(g_lastPlayed)) g_playPos = (double)c->s;
    g_speed = 1; g_playDrag = false;
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}
void setVol(int id, float v) {
    if (Cut* c = cutById(id)) c->vol = v;
    if (g_eng.active && g_eng.cutId == id) g_eng.gain = v;
}
void pushFx(int id, float oldV, float newV) {
    if (std::fabs(oldV - newV) < 1e-6f) return;
    g_undo.push_back({id, oldV, newV}); g_redo.clear();
}
// start playing a cut; from = absolute frame, step = 1 / 3 / -3
void playCut(int id, double from, double step) {
    Cut* c = cutById(id);
    if (!c || !g_doc) return;
    if (step > 0 && from >= (double)c->e - 1) from = (double)c->s;
    if (step < 0 && from <= (double)c->s) from = (double)c->e - 1;
    if (!engStart(g_doc, id, c->s, c->e, from, step, c->vol)) { setStatus(L"Cannot open the audio device"); return; }
    g_lastPlayed = id; g_target = id; g_playPos = from; g_speed = step; g_playDrag = false;
    InvalidateRect(g_wnd, nullptr, FALSE);
}
int cutAtFrame(i64 f) { for (auto& c : g_cuts) if (f >= c.s && f < c.e) return c.id; return -1; }
bool playCutAt(int x) {                            // restart the subsample under the mouse from its start
    const int id = cutAtFrame((i64)std::floor(frameAt(x)));
    if (id < 0) return false;
    playCut(id, (double)cutById(id)->s, 1);
    return true;
}
int volTarget() { if (cutById(g_lastPlayed)) return g_lastPlayed; if (cutById(g_target)) return g_target; return -1; }
int playTarget() {
    if (g_eng.active && cutById(g_eng.cutId)) return g_eng.cutId;
    if (cutById(g_target)) return g_target;
    if (cutById(g_lastPlayed)) return g_lastPlayed;
    return -1;
}
void togglePlay() {                                // Play / Pause
    if (g_eng.active) {
        g_playPos = engPos();
        if (g_eng.paused) engResume(); else engPause();
        InvalidateRect(g_wnd, nullptr, FALSE); return;
    }
    const int id = playTarget();
    if (id < 0) return;
    playCut(id, id == g_lastPlayed ? g_playPos : (double)cutById(id)->s, 1);
}
void fastPlay(double step) {                       // 3x forward / backward, pressing again stops it
    if (g_eng.active) {
        if (g_speed == step && !g_eng.paused) { g_playPos = engPos(); engPause(); }
        else { g_playPos = engPos(); engSetStep(step); engResume(); g_speed = step; }
        InvalidateRect(g_wnd, nullptr, FALSE); return;
    }
    const int id = playTarget();
    if (id < 0) return;
    playCut(id, id == g_lastPlayed ? g_playPos : (double)cutById(id)->s, step);
}
int jumpIndex(int dir) {
    const int n = (int)g_cuts.size();
    if (!n) return -1;
    int ref = cutIndexById(g_lastPlayed);
    if (ref < 0) ref = cutIndexById(g_target);
    const int ni = ref < 0 ? (dir > 0 ? 0 : n - 1) : ref + dir;
    return (ni < 0 || ni >= n) ? -1 : ni;
}
void ensureVisible(const Cut& c) {
    const double vis = viewW() * g_spp;
    if ((double)c.s >= g_vs && (double)c.e <= g_vs + vis) return;
    g_vs = (double)(c.e - c.s) < vis ? (double)(c.s + c.e) / 2 - vis / 2 : (double)c.s;
    clampView(); updateScroll();
}
void jump(int dir) {
    const int ni = jumpIndex(dir);
    if (ni < 0) return;
    ensureVisible(g_cuts[ni]);
    playCut(g_cuts[ni].id, (double)g_cuts[ni].s, 1);
}
void doUndo() {
    while (!g_undo.empty()) {
        Fx f = g_undo.back(); g_undo.pop_back();
        if (!cutById(f.id)) continue;
        setVol(f.id, f.oldV); g_redo.push_back(f); g_target = f.id; break;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}
void doRedo() {
    while (!g_redo.empty()) {
        Fx f = g_redo.back(); g_redo.pop_back();
        if (!cutById(f.id)) continue;
        setVol(f.id, f.newV); g_undo.push_back(f); g_target = f.id; break;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}
void doMaximize() {                                // raise the volume until the loudest sample touches full scale
    const int id = volTarget();
    Cut* c = cutById(id);
    if (!c || !g_doc) return;
    float peak = 0;
    for (i64 i = c->s * 2; i < c->e * 2; i++) peak = std::max(peak, std::fabs(g_doc->pcm[(size_t)i]));
    if (peak < 1e-6f) return;
    const float old = c->vol; setVol(id, 1.f / peak); pushFx(id, old, 1.f / peak);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

// play marker (draggable, inside the last played subsample)
double playPosClamped() {
    Cut* c = cutById(g_lastPlayed);
    return c ? std::max((double)c->s, std::min((double)c->e - 1, g_playPos)) : 0;
}
int playheadX() { return (int)std::floor((playPosClamped() - g_vs) / g_spp + 0.5); }
bool hitPlayheadGrip(int x, int y) { return cutById(g_lastPlayed) && y >= waveBot() - 20 && y < waveBot() && std::abs(x - playheadX()) <= 9; }
bool hitPlayheadLine(int x, int y) { return cutById(g_lastPlayed) && y >= waveTop() && y < waveBot() && std::abs(x - playheadX()) <= kTol; }
void drawPlayhead(HDC dc, int W, int top, int bot) {
    Cut* c = cutById(g_lastPlayed);
    if (!c || !g_doc) return;
    const int x = playheadX();
    if (x < -20 || x > W + 20) return;
    const COLORREF col = RGB(255, 222, 90);
    HPEN p = CreatePen(PS_SOLID, 2, col); HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x, top, nullptr); LineTo(dc, x, bot);
    SelectObject(dc, op); DeleteObject(p);
    HBRUSH b = CreateSolidBrush(col); HGDIOBJ ob = SelectObject(dc, b), opn = SelectObject(dc, GetStockObject(NULL_PEN));
    POINT tri[3] = {{x - 8, bot}, {x + 8, bot}, {x, bot - 15}};
    Polygon(dc, tri, 3);
    SelectObject(dc, opn); SelectObject(dc, ob); DeleteObject(b);
    wchar_t t[48]; swprintf(t, 48, L"%.3f s", (playPosClamped() - (double)c->s) / g_doc->rate);
    if (x + 80 < W) drawText(dc, x + 10, bot - 32, t, col); else drawText(dc, x - 10, bot - 32, t, col, TA_RIGHT);
}

// ----------------------------------------------------------------------------------------- toolbar
enum Btn { B_OPEN, B_SAVE, B_SAVEAS, B_UNDO, B_REDO, B_PREV, B_RW, B_PLAY, B_STOP, B_FF, B_NEXT, B_VOL, B_MAX, B_COUNT };
const int kBtnW = 38, kVolW = 206;
RECT btnRect(int b) {
    int x = 8;
    for (int i = 0; i < b; i++) x += (i == B_VOL ? kVolW : kBtnW) + ((i == B_SAVEAS || i == B_REDO || i == B_NEXT) ? 16 : 4);
    return RECT{x, 5, x + (b == B_VOL ? kVolW : kBtnW), kBar - 5};
}
int btnAt(int x, int y) {
    for (int b = 0; b < B_COUNT; b++) { RECT r = btnRect(b); if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return b; }
    return -1;
}
bool btnEnabled(int b) {
    const bool cuts = g_doc && !g_cuts.empty();
    switch (b) {
    case B_OPEN: return true;
    case B_SAVE: case B_SAVEAS: return cuts && !g_exporting;
    case B_UNDO: return !g_undo.empty();
    case B_REDO: return !g_redo.empty();
    case B_STOP: return g_eng.active;
    case B_VOL: case B_MAX: return cuts && volTarget() >= 0;
    default: return cuts;
    }
}
void volTrack(int& x0, int& x1) { RECT r = btnRect(B_VOL); x0 = r.left + 34; x1 = r.right - 66; }
const double kDbMin = -30, kDbMax = 30;
float volFromX(int x) {
    int x0, x1; volTrack(x0, x1);
    double db = kDbMin + (kDbMax - kDbMin) * std::max(0.0, std::min(1.0, (double)(x - x0) / (x1 - x0)));
    db = std::round(db * 2) / 2; if (std::fabs(db) < 0.75) db = 0;
    return (float)std::pow(10.0, db / 20);
}
std::vector<int> hlIds() {
    std::vector<int> v;
    if (g_hoverBtn < 0 || !g_doc) return v;
    switch (g_hoverBtn) {
    case B_SAVE: case B_SAVEAS: for (auto& c : g_cuts) v.push_back(c.id); break;
    case B_UNDO: for (size_t i = g_undo.size(); i-- > 0;) if (cutById(g_undo[i].id)) { v.push_back(g_undo[i].id); break; } break;
    case B_REDO: for (size_t i = g_redo.size(); i-- > 0;) if (cutById(g_redo[i].id)) { v.push_back(g_redo[i].id); break; } break;
    case B_PREV: case B_NEXT: { const int ni = jumpIndex(g_hoverBtn == B_NEXT ? 1 : -1); if (ni >= 0) v.push_back(g_cuts[ni].id); break; }
    case B_PLAY: case B_STOP: case B_RW: case B_FF: { const int id = playTarget(); if (id >= 0) v.push_back(id); break; }
    case B_VOL: case B_MAX: { const int id = volTarget(); if (id >= 0) v.push_back(id); break; }
    }
    return v;
}
const wchar_t* btnTip(int b) {
    switch (b) {
    case B_OPEN: return L"Open file(s)   Ctrl+O";
    case B_SAVE: return L"Save all subsamples of this tab   Ctrl+S";
    case B_SAVEAS: return L"Save all subsamples as...  (asks for a new name, then the folder)";
    case B_UNDO: return L"Undo the last applied effect   Ctrl+Z";
    case B_REDO: return L"Redo the last undone effect   Ctrl+Y";
    case B_PREV: return L"Previous subsample: jump to it and play";
    case B_RW: return L"Fast backwards 3x: start / stop";
    case B_PLAY: return L"Play / pause the last played or last hovered subsample";
    case B_STOP: return L"Stop";
    case B_FF: return L"Fast forward 3x: start / stop";
    case B_NEXT: return L"Next subsample: jump to it and play";
    case B_VOL: return L"Volume of the last played subsample (saved with it).  Double-click: reset";
    case B_MAX: return L"Maximize the volume of the last played subsample";
    }
    return L"";
}

void drawIcon(HDC dc, int b, const RECT& r, COLORREF col, bool playing) {
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const COLORREF dark = RGB(GetRValue(col) * 5 / 10, GetGValue(col) * 5 / 10, GetBValue(col) * 5 / 10);
    auto P = [&](int x, int y) { return POINT{cx + x, cy + y}; };
    auto poly = [&](std::vector<POINT> p, COLORREF c) {
        HBRUSH br = CreateSolidBrush(c); HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, p.data(), (int)p.size()); SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br);
    };
    auto rc = [&](int l, int t, int rr, int bb, COLORREF c) {
        RECT q{cx + l, cy + t, cx + rr, cy + bb}; HBRUSH br = CreateSolidBrush(c); FillRect(dc, &q, br); DeleteObject(br);
    };
    switch (b) {
    case B_OPEN:
        poly({P(-10, -8), P(-3, -8), P(-1, -5), P(9, -5), P(9, 8), P(-10, 8)}, dark);
        poly({P(-10, 8), P(-7, -2), P(12, -2), P(9, 8)}, col);
        break;
    case B_SAVE: case B_SAVEAS: {
        const int o = b == B_SAVEAS ? -2 : 0;
        poly({P(o - 9, o - 9), P(o + 5, o - 9), P(o + 9, o - 5), P(o + 9, o + 9), P(o - 9, o + 9)}, col);
        rc(o - 5, o - 9, o + 4, o - 3, RGB(28, 32, 42)); rc(o + 1, o - 8, o + 3, o - 4, col);
        rc(o - 6, o + 1, o + 6, o + 9, RGB(236, 241, 250)); rc(o - 4, o + 4, o + 4, o + 5, dark); rc(o - 4, o + 6, o + 2, o + 7, dark);
        if (b == B_SAVEAS) {
            HPEN pen = CreatePen(PS_SOLID, 4, RGB(255, 214, 80)); HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, cx + 4, cy + 9, nullptr); LineTo(dc, cx + 10, cy + 3);
            SelectObject(dc, op); DeleteObject(pen);
            poly({P(1, 13), P(2, 8), P(6, 12)}, RGB(250, 250, 250));
        }
        break; }
    case B_UNDO: case B_REDO: {
        const int sg = b == B_UNDO ? 1 : -1; const double R = 7.5;
        HPEN pen = CreatePen(PS_SOLID, 3, col); HGDIOBJ op = SelectObject(dc, pen);
        for (int a = 270, first = 1; a <= 500; a += 10, first = 0) {
            const double t = a * 3.14159265358979 / 180;
            const int x = cx + (int)std::lround(sg * R * std::cos(t)), y = cy + 1 + (int)std::lround(R * std::sin(t));
            if (first) MoveToEx(dc, x, y, nullptr); else LineTo(dc, x, y);
        }
        SelectObject(dc, op); DeleteObject(pen);
        poly({P(-8 * sg, -6), P(sg, -12), P(sg, 0)}, col);
        break; }
    case B_PREV:
        rc(-9, -8, -6, 8, col); poly({P(10, -8), P(10, 8), P(-5, 0)}, col); break;
    case B_NEXT:
        rc(6, -8, 9, 8, col); poly({P(-10, -8), P(-10, 8), P(5, 0)}, col); break;
    case B_RW:
        poly({P(-1, -8), P(-1, 8), P(-11, 0)}, col); poly({P(10, -8), P(10, 8), P(0, 0)}, col); break;
    case B_FF:
        poly({P(1, -8), P(1, 8), P(11, 0)}, col); poly({P(-10, -8), P(-10, 8), P(0, 0)}, col); break;
    case B_PLAY:
        if (playing) { rc(-7, -8, -2, 8, col); rc(2, -8, 7, 8, col); }
        else poly({P(-6, -9), P(-6, 9), P(9, 0)}, col);
        break;
    case B_STOP: rc(-7, -7, 7, 7, col); break;
    case B_MAX:
        rc(-9, -11, 9, -8, col); poly({P(-8, -1), P(8, -1), P(0, -8)}, col); rc(-3, -1, 3, 9, col); break;
    }
}
void drawSpeaker(HDC dc, int x, int cy, COLORREF col) {
    HBRUSH br = CreateSolidBrush(col); HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    RECT q{x, cy - 3, x + 5, cy + 4}; FillRect(dc, &q, br);
    POINT t[4] = {{x + 5, cy - 3}, {x + 11, cy - 8}, {x + 11, cy + 8}, {x + 5, cy + 4}};
    Polygon(dc, t, 4);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 2, col); op = SelectObject(dc, pen);
    Arc(dc, x + 6, cy - 5, x + 16, cy + 5, x + 11, cy - 5, x + 11, cy + 5);
    SelectObject(dc, op); DeleteObject(pen);
}
void paintToolbar(HDC dc, int W) {
    HBRUSH b = CreateSolidBrush(RGB(30, 32, 37)); RECT r{0, 0, W, kBar}; FillRect(dc, &r, b); DeleteObject(b);
    const bool playing = g_eng.active && !g_eng.paused;
    for (int i = 0; i < B_COUNT; i++) {
        const RECT br = btnRect(i); const bool en = btnEnabled(i), hov = g_hoverBtn == i && en;
        const bool on = (i == B_PLAY && playing) || (i == B_FF && playing && g_speed > 1) || (i == B_RW && playing && g_speed < 0);
        HBRUSH bb = CreateSolidBrush(!en ? RGB(36, 39, 45) : (on ? RGB(40, 84, 60) : (hov ? RGB(66, 74, 92) : RGB(46, 50, 60))));
        HGDIOBJ ob = SelectObject(dc, bb), op = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, br.left, br.top, br.right + 1, br.bottom + 1, 9, 9);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(bb);
        COLORREF col = RGB(225, 230, 240);
        switch (i) {
        case B_OPEN: col = RGB(255, 196, 80); break;
        case B_SAVE: case B_SAVEAS: col = RGB(110, 190, 245); break;
        case B_UNDO: case B_REDO: col = RGB(190, 160, 255); break;
        case B_PLAY: col = RGB(110, 230, 140); break;
        case B_STOP: col = RGB(240, 100, 100); break;
        case B_MAX: col = RGB(255, 160, 60); break;
        }
        if (!en) col = RGB(88, 92, 102);
        if (i == B_VOL) {
            const int cy = (br.top + br.bottom) / 2; int x0, x1; volTrack(x0, x1);
            drawSpeaker(dc, br.left + 8, cy, en ? RGB(150, 210, 255) : col);
            const Cut* c = cutById(volTarget());
            const double db = c ? 20 * std::log10((double)c->vol) : 0;
            const double fr = std::max(0.0, std::min(1.0, (db - kDbMin) / (kDbMax - kDbMin)));
            HPEN tp = CreatePen(PS_SOLID, 3, RGB(70, 76, 90)); HGDIOBJ o2 = SelectObject(dc, tp);
            MoveToEx(dc, x0, cy, nullptr); LineTo(dc, x1, cy); SelectObject(dc, o2); DeleteObject(tp);
            const int hx = x0 + (int)std::lround(fr * (x1 - x0));
            if (en) { HPEN fp = CreatePen(PS_SOLID, 3, RGB(110, 190, 245)); o2 = SelectObject(dc, fp);
                      const int ux = x0 + (int)std::lround((0 - kDbMin) / (kDbMax - kDbMin) * (x1 - x0));
                      MoveToEx(dc, std::min(ux, hx), cy, nullptr); LineTo(dc, std::max(ux, hx), cy); SelectObject(dc, o2); DeleteObject(fp); }
            HBRUSH hb = CreateSolidBrush(en ? RGB(240, 245, 252) : RGB(88, 92, 102)); HGDIOBJ o3 = SelectObject(dc, hb), o4 = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, hx - 6, cy - 6, hx + 7, cy + 7); SelectObject(dc, o4); SelectObject(dc, o3); DeleteObject(hb);
            wchar_t t[32]; if (c) swprintf(t, 32, L"%+.1f dB", db); else swprintf(t, 32, L"-");
            drawText(dc, br.right - 6, cy - 7, t, en ? RGB(210, 216, 228) : col, TA_RIGHT);
        } else drawIcon(dc, i, br, col, playing);
    }
    HPEN sp = CreatePen(PS_SOLID, 1, RGB(64, 68, 80)); HGDIOBJ o = SelectObject(dc, sp);
    for (int i : {B_SAVEAS, B_REDO, B_NEXT}) { RECT br = btnRect(i); MoveToEx(dc, br.right + 8, 10, nullptr); LineTo(dc, br.right + 8, kBar - 10); }
    SelectObject(dc, o); DeleteObject(sp);
}
void drawTip(HDC dc, int W) {
    if (g_hoverBtn < 0) return;
    const wchar_t* t = btnTip(g_hoverBtn);
    SIZE sz{}; GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    RECT br = btnRect(g_hoverBtn);
    int x = std::max(4, std::min(W - (int)sz.cx - 16, (int)br.left)), y = kBar + 4;
    RECT q{x, y, x + sz.cx + 12, y + sz.cy + 8};
    HBRUSH f = CreateSolidBrush(RGB(250, 246, 210)), fr = CreateSolidBrush(RGB(120, 110, 60));
    FillRect(dc, &q, f); FrameRect(dc, &q, fr); DeleteObject(f); DeleteObject(fr);
    drawText(dc, x + 6, y + 4, t, RGB(30, 30, 30));
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
        wchar_t b[256]; swprintf(b, 256, L"%ls   %ls   %d Hz", g_doc->name.c_str(), fmtTime((double)g_doc->frames / g_doc->rate).c_str(), g_doc->rate);
        left = b;
    } else left = L"No file";
    if (g_doc) {
        left += L"   |   Subsamples: " + std::to_wstring(g_cuts.size());
        const int ci = cutIndexById(g_lastPlayed);
        if (ci >= 0) {
            const Cut& c = g_cuts[ci]; const double rate = g_doc->rate;
            wchar_t b[256]; swprintf(b, 256, L"   |   Subsample #%d   length %.3f s   play marker %.3f s   volume %+.1f dB", ci + 1, (double)(c.e - c.s) / rate,
                                     (playPosClamped() - (double)c.s) / rate, 20 * std::log10((double)c.vol));
            left += b;
        }
    }
    drawText(dc, 8, H - kStatus + 4, left, RGB(200, 204, 214));
    if (g_doc && g_mouseX >= 0) drawText(dc, W - 8, H - kStatus + 4, fmtTime(std::max(0.0, frameAt(g_mouseX)) / g_doc->rate), RGB(200, 204, 214), TA_RIGHT);
    drawTip(dc, W);

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

// small modal "enter a name" dialog built in memory
INT_PTR CALLBACK nameDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INITDIALOG) {
        SetWindowLongPtrW(h, DWLP_USER, l);
        SetDlgItemTextW(h, 1001, ((std::wstring*)l)->c_str());
        SendDlgItemMessageW(h, 1001, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(h, 1001));
        return FALSE;
    }
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK) {
            wchar_t b[260]; GetDlgItemTextW(h, 1001, b, 260);
            *(std::wstring*)GetWindowLongPtrW(h, DWLP_USER) = b; EndDialog(h, 1); return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
    }
    return FALSE;
}
bool askName(std::wstring& name) {
    std::vector<WORD> d;
    auto al = [&] { if (d.size() & 1) d.push_back(0); };
    auto d32 = [&](DWORD v) { al(); d.push_back(LOWORD(v)); d.push_back(HIWORD(v)); };
    auto str = [&](const wchar_t* s) { while (*s) d.push_back(*s++); d.push_back(0); };
    auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        al(); d32(style); d32(0); d.push_back(x); d.push_back(y); d.push_back(cx); d.push_back(cy); d.push_back(id);
        d.push_back(0xFFFF); d.push_back(cls); str(text); d.push_back(0);
    };
    d32(DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU); d32(0);
    d.push_back(4); d.push_back(0); d.push_back(0); d.push_back(220); d.push_back(70);   // 4 items, x, y, cx, cy
    d.push_back(0); d.push_back(0); str(L"Save subsamples as");
    d.push_back(9); str(L"Segoe UI");
    item(WS_CHILD | WS_VISIBLE, 8, 8, 204, 10, 1002, 0x0082, L"New base name (files become <name>_01.mp3, <name>_02.mp3 ...):");
    item(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 22, 204, 14, 1001, 0x0081, L"");
    item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 100, 46, 54, 16, IDOK, 0x0080, L"OK");
    item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 158, 46, 54, 16, IDCANCEL, 0x0080, L"Cancel");
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)d.data(), g_wnd, nameDlgProc, (LPARAM)&name) == 1;
}

void doSave(bool asNew = false) {
    stopPlay();                                    // saving always stops the playback
    if (g_exporting) return;
    if (!g_doc || g_cuts.empty()) { MessageBoxW(g_wnd, L"Nothing to save - create some cuts first.", kTitle, MB_ICONINFORMATION); return; }
    std::wstring base = g_doc->name;
    if (asNew) {
        if (!askName(base)) return;
        for (auto& ch : base) if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'_';
        while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) base.pop_back();
        if (base.empty()) { MessageBoxW(g_wnd, L"The name is empty.", kTitle, MB_ICONINFORMATION); return; }
    }
    std::wstring folder;
    if (!pickFolder(folder)) return;
    g_exporting = true;
    auto doc = g_doc; auto cuts = g_cuts;
    setStatus(L"Saving cuts...");
    std::thread([doc, cuts, folder, base] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring err; int done = 0;
        const int digits = std::max(2, (int)std::to_wstring(cuts.size()).size());
        for (size_t i = 0; i < cuts.size(); i++) {
            wchar_t name[64]; swprintf(name, 64, L"_%0*d.mp3", digits, (int)i + 1);
            std::wstring path = folder + L"\\" + base + name;
            postStatus(L"Saving " + path.substr(path.find_last_of(L'\\') + 1) + L" ...");
            if (!writeMp3(path, &doc->pcm[(size_t)cuts[i].s * 2], cuts[i].e - cuts[i].s, doc->rate, kMp3Kbps, cuts[i].vol, err)) break;
            done++;
        }
        CoUninitialize();
        wchar_t b[512];
        if (done == (int)cuts.size()) swprintf(b, 512, L"Saved %d cut(s) to %ls", done, folder.c_str());
        else swprintf(b, 512, L"Stopped after %d of %d cuts: %ls", done, (int)cuts.size(), err.c_str());
        PostMessageW(g_wnd, WM_EXPORTED, done == (int)cuts.size(), (LPARAM) new std::wstring(b));
    }).detach();
}

void onBtn(int b) {
    switch (b) {
    case B_OPEN: doOpen(); break;
    case B_SAVE: doSave(false); break;
    case B_SAVEAS: doSave(true); break;
    case B_UNDO: doUndo(); break;
    case B_REDO: doRedo(); break;
    case B_PREV: jump(-1); break;
    case B_NEXT: jump(1); break;
    case B_RW: fastPlay(-3); break;
    case B_FF: fastPlay(3); break;
    case B_PLAY: togglePlay(); break;
    case B_STOP: stopPlay(); break;
    case B_MAX: doMaximize(); break;
    }
}

// ----------------------------------------------------------------------------------------- window
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_OPEN: doOpen(); break;
        case IDM_SAVE: doSave(false); break;
        case IDM_SAVEAS: doSave(true); break;
        case IDM_UNDO: doUndo(); break;
        case IDM_REDO: doRedo(); break;
        case IDM_CLEAR: stopPlay(); g_cuts.clear(); g_undo.clear(); g_redo.clear(); g_lastPlayed = g_target = -1; InvalidateRect(h, nullptr, FALSE); break;
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
    case WM_STATUS: { auto* s = (std::wstring*)l; g_msg = *s; g_msgTick = GetTickCount64(); delete s; InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_EXPORTED: {
        auto* s = (std::wstring*)l; g_exporting = false; g_msg = *s; g_msgTick = GetTickCount64();
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
        if (y < kBar) {
            if (m != WM_LBUTTONDOWN) return 0;
            const int b = btnAt(x, y);
            if (b < 0 || !btnEnabled(b)) return 0;
            if (b == B_VOL) {
                int x0, x1; volTrack(x0, x1);
                Cut* c = cutById(volTarget());
                if (c && x >= x0 - 8 && x <= x1 + 8) { g_volDrag = true; g_volId = c->id; g_volOld = c->vol; setVol(c->id, volFromX(x)); SetCapture(h); }
            } else onBtn(b);
            InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (!g_doc) return 0;
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
            if (hitPlayheadGrip(x, y) || (hit.cut < 0 && hitPlayheadLine(x, y))) {   // the play marker
                if (g_eng.active) g_playPos = engPos();
                g_playDrag = true; g_playDragWas = g_eng.active && !g_eng.paused;
                if (g_playDragWas) engPause();
                g_playDragOff = playheadX() - x;
                SetCapture(h);
            } else if (hit.cut >= 0) {                                  // dragging beats creating
                g_drag = hit; const Cut& c = g_cuts[hit.cut];
                g_dragOff = xOf(hit.end ? c.e : c.s) - x;
                SetCapture(h);
            } else createStart((i64)std::llround(frameAt(x)));
        } else if (m == WM_RBUTTONDOWN) {
            if (hitTest(x, false).cut < 0) createEnd((i64)std::llround(frameAt(x)));
        }
        InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_LBUTTONDBLCLK: {
        const int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (y < kBar && btnAt(x, y) == B_VOL && btnEnabled(B_VOL)) {          // reset the volume to 0 dB
            int x0, x1; volTrack(x0, x1);
            if (x >= x0 - 8 && x <= x1 + 8) { Cut* c = cutById(volTarget()); if (c) { pushFx(c->id, c->vol, 1.f); setVol(c->id, 1.f); } }
            InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (y >= waveBot() && y < waveBot() + kGrip) { g_gain = 1; InvalidateRect(h, nullptr, FALSE); return 0; }
        return WndProc(h, WM_LBUTTONDOWN, w, l); }                            // any other double-click is just a second click
    case WM_MOUSEMOVE: {
        const int x = (short)LOWORD(l), y = (short)HIWORD(l);
        g_mouseX = x;
        { TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); }
        g_hoverBtn = (y >= 0 && y < kBar) ? btnAt(x, y) : -1;
        if (g_volDrag) { setVol(g_volId, volFromX(x)); }
        else if (g_playDrag) { if (Cut* c = cutById(g_lastPlayed)) g_playPos = std::max((double)c->s, std::min((double)c->e - 1, frameAt(x + g_playDragOff))); }
        else if (g_gripDrag) { g_gain = std::max(0.1, std::min(200.0, g_gripGain * std::pow(1.012, g_gripY - y))); }
        else if (g_pan) { if (std::abs(x - g_panX) > 3) g_panMoved = true; g_vs = g_panVs - (x - g_panX) * g_spp; clampView(); updateScroll(); }
        else if (g_drag.cut >= 0) { moveMarker(g_drag, (i64)std::llround(frameAt(x + g_dragOff))); }
        else {
            const bool inWave = g_doc && y >= waveTop() && y < waveBot();
            g_hover = inWave ? hitTest(x, (GetKeyState(VK_CONTROL) & 0x8000) != 0) : Hit();
            if (inWave) { const int id = cutAtFrame((i64)std::floor(frameAt(x))); if (id >= 0) g_target = id; }   // last hovered subsample
        }
        InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_LBUTTONUP: case WM_MBUTTONUP:
        if (m == WM_MBUTTONUP && g_pan && !g_panMoved && g_panY >= waveTop() && g_panY < waveBot()) playCutAt((short)LOWORD(l));
        if (g_volDrag) { if (Cut* c = cutById(g_volId)) pushFx(g_volId, g_volOld, c->vol); g_volDrag = false; ReleaseCapture(); }
        if (g_playDrag) {
            g_playDrag = false; ReleaseCapture();
            if (g_eng.active) { engSeek(g_playPos); if (g_playDragWas) engResume(); }
        }
        if (g_drag.cut >= 0 || g_gripDrag || g_pan) ReleaseCapture();
        g_drag = Hit(); g_gripDrag = false; g_pan = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSELEAVE: g_mouseX = -1; g_hoverBtn = -1; InvalidateRect(h, nullptr, FALSE); { /* allow tracking again */ } return 0;
    case WM_TIMER:
        if (g_eng.active && !g_eng.paused && !g_playDrag) {
            const double p = engPos();
            if (p >= 0) g_playPos = p;
            if (engEnded()) stopPlay();                                      // reached the end: marker back to the start
            InvalidateRect(h, nullptr, FALSE);
        }
        if (!g_msg.empty() && !g_loading && !g_exporting && GetTickCount64() - g_msgTick > 6000) { g_msg.clear(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_DELETE && g_hover.cut >= 0 && g_drag.cut < 0) { removeMarker(g_hover); g_hover = Hit(); InvalidateRect(h, nullptr, FALSE); }
        else if (w == VK_SPACE) {
            if (l & 0x40000000) return 0;                                    // ignore key repeat
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            const bool inWave = pt.y >= waveTop() && pt.y < waveBot();
            const int under = inWave ? cutAtFrame((i64)std::floor(frameAt(pt.x))) : -1;
            if (g_eng.active) {
                if (under >= 0 && under != g_eng.cutId) playCutAt(pt.x);    // another subsample under the mouse: switch to it at once
                else stopPlay();                                             // same one (or none): space stops
            } else if (!(under >= 0 && playCutAt(pt.x))) togglePlay();
            InvalidateRect(h, nullptr, FALSE);
        }
        else if (w == VK_ESCAPE) stopPlay();
        else if (w == VK_HOME) { fitAll(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            LPCWSTR c = IDC_ARROW;
            if (pt.y < kBar) { const int b = btnAt(pt.x, pt.y); if (b >= 0 && btnEnabled(b)) c = IDC_HAND; }
            else if (g_playDrag || hitPlayheadGrip(pt.x, pt.y) || (hitPlayheadLine(pt.x, pt.y) && hitTest(pt.x, false).cut < 0)) c = IDC_SIZEWE;
            else if (g_gripDrag || (pt.y >= waveBot() && pt.y < waveBot() + kGrip)) c = IDC_SIZENS;
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
    AppendMenuW(file, MF_STRING, IDM_SAVEAS, L"Save Cuts &As...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_UNDO, L"&Undo effect\tCtrl+Z");
    AppendMenuW(file, MF_STRING, IDM_REDO, L"&Redo effect\tCtrl+Y");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_CLEAR, L"&Clear all cuts");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(help, MF_STRING, IDM_HELP, L"&Controls");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");

    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 520, nullptr, bar, inst, nullptr);
    ACCEL acc[] = {{FVIRTKEY | FCONTROL, 'O', IDM_OPEN}, {FVIRTKEY | FCONTROL, 'S', IDM_SAVE},
                   {FVIRTKEY | FCONTROL, 'Z', IDM_UNDO}, {FVIRTKEY | FCONTROL, 'Y', IDM_REDO}};
    HACCEL ha = CreateAcceleratorTableW(acc, 4);
    SetTimer(g_wnd, 1, 33, nullptr);
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
