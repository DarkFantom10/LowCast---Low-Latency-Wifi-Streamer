// ============================================================================
// LowCast — minimal-latency system-audio -> DLNA/UPnP WiFi streamer
// Built for HiFiMAN HE1000 WiFi (works with any UPnP AV MediaRenderer).
//
// Design goals:
//   * ZERO sender-side buffering: WASAPI loopback packets are encoded and
//     pushed onto the socket the instant they are captured (TCP_NODELAY,
//     chunked transfer, no accumulation, no FLAC encode delay).
//   * Uncompressed LPCM/WAV only  -> no codec latency on either end.
//   * The only latency left is the renderer firmware's own pre-buffer.
//
// DLNA handshake (SSDP / SOAP / DIDL / HTTP headers) modelled on swyh-rs
// (MIT licensed, https://github.com/dheijl/swyh-rs).
//
// Single-file Win32 app, no dependencies. Compile with mingw-w64:
//   x86_64-w64-mingw32-g++ -O2 -DUNICODE -D_UNICODE -municode lowcast.cpp
//       -o LowCast.exe -mwindows -static -lws2_32 -lole32 -lcomctl32 -lgdi32
//       -luser32 -liphlpapi -lwinmm
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <timeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------------------
// GUIDs (declared manually so we don't depend on uuid import libs)
// ---------------------------------------------------------------------------
DEFINE_GUID(CLSID_MMDeviceEnumerator_L, 0xBCDE0395, 0xE52F, 0x467C,
            0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator_L, 0xA95664D2, 0x9614, 0x4F35,
            0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient_L, 0x1CB9AD4C, 0xDBFA, 0x4C32,
            0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioCaptureClient_L, 0xC8ADBD64, 0xE71E, 0x48A0,
            0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(PKEY_Device_FriendlyName_fmtid, 0xA45C254E, 0xDF1C, 0x4EFD,
            0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_L, 0x00000003, 0x0000, 0x0010,
            0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM_L, 0x00000001, 0x0000, 0x0010,
            0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string xml_escape(const std::string& in) {
    std::string out; out.reserve(in.size() + 32);
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}
static uint64_t now_ms() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart * 1000ULL / (uint64_t)freq.QuadPart);
}

// seeded PRNG for session identifiers. rand() was never seeded, so every app
// launch produced the SAME DACP-ID / SSRC / seq / rtptime (and rand()*rand()
// is signed overflow). Not cryptographic — just unique-per-run.
static uint32_t rng32() {
    static std::atomic<uint32_t> st{0};
    uint32_t x = st.load();
    if (!x) {
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        x = (uint32_t)(t.QuadPart ^ (t.QuadPart >> 32)) ^ (GetCurrentProcessId() * 2654435761u);
        if (!x) x = 0x6B8B4567u;
    }
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;      // xorshift32
    st.store(x);
    return x;
}

// case-insensitive find inside HTTP/XML text
static size_t ifind(const std::string& hay, const std::string& needle, size_t from = 0) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j)
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) break;
        if (j == needle.size()) return i;
    }
    return std::string::npos;
}
// extract text between <tag> and </tag> (first occurrence at/after `from`)
static std::string xml_tag(const std::string& xml, const std::string& tag, size_t from = 0) {
    std::string open = "<" + tag + ">", close = "</" + tag + ">";
    size_t a = ifind(xml, open, from);
    if (a == std::string::npos) return {};
    a += open.size();
    size_t b = ifind(xml, close, a);
    if (b == std::string::npos) return {};
    return xml.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// Streaming format selection
// ---------------------------------------------------------------------------
enum class Fmt { LPCM16 = 0, LPCM24 = 1, WAV16 = 2, WAV24 = 3 };
static int  fmt_bits(Fmt f)   { return (f == Fmt::LPCM24 || f == Fmt::WAV24) ? 24 : 16; }
static bool fmt_is_wav(Fmt f) { return f == Fmt::WAV16 || f == Fmt::WAV24; }

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
struct Client {                       // one connected renderer HTTP stream
    SOCKET      sock = INVALID_SOCKET;
    std::string remote;
    std::deque<uint8_t> q;            // encoded bytes waiting for the socket
    uint64_t    sent_bytes = 0;
    bool        dead = false;
    HANDLE      evt = nullptr;        // auto-reset: signaled when q gains data
};

struct Renderer {
    std::string name;
    std::string location;             // SSDP LOCATION (description xml url)
    std::string control_url;          // absolute AVTransport control URL
    std::string oh_url;               // absolute OpenHome Playlist control URL (preferred)
    std::string cm_url;               // absolute ConnectionManager control URL (diagnostics)
    std::string host;                 // ip of renderer
    bool        streaming = false;
    HWND        button = nullptr;
};

struct AirplayDev {
    std::string name, host, txt;
    int port = 0;
    bool streaming = false;
    HWND button = nullptr;
};
static std::vector<AirplayDev> g_airplay;         // guarded by G.cs

static struct {
    // audio
    std::atomic<bool>  capture_run{false};
    std::atomic<int>   capture_dev{-1};      // index into device id list, -1 = default
    std::atomic<int>   fmt{(int)Fmt::LPCM16};
    std::atomic<uint32_t> sample_rate{48000};   // ADVERTISED rate (native * mult)
    std::atomic<uint32_t> native_rate{48000};   // capture device NOMINAL mix rate
    std::atomic<double> measured_rate{48000.0}; // capture device MEASURED true rate
    std::atomic<int>   upmult{1};               // 1, 2 or 4  (latency divider)
    std::atomic<uint64_t> frames_captured{0};
    std::atomic<uint64_t> silence_frames{0};
    std::atomic<bool>  beep_on{false};
    std::atomic<bool>  beep_local{false};      // tick to PC output instead of stream
    std::atomic<int>   ap_latency_ms{150};     // AirPlay sender-side latency
    std::atomic<int>   ap_start_vol{-1};       // % at session start; -1 = leave alone
    std::atomic<uint64_t> beep_flash_ms{0};  // set by audio thread when a tick is injected
    // network
    CRITICAL_SECTION   cs;                   // guards clients
    std::vector<Client*> clients;
    std::atomic<int>   http_port{16600};
    std::atomic<uint64_t> backlog_ms_x10{0}; // worst client backlog, tenths of ms
    // ui
    HWND hwnd = nullptr;
    HWND log = nullptr, cmb_dev = nullptr, cmb_fmt = nullptr, cmb_rate = nullptr,
         stat = nullptr, flash = nullptr, btn_scan = nullptr, btn_beep = nullptr,
         btn_localbeep = nullptr, cmb_aplat = nullptr,
         sld_apvol = nullptr, lbl_apvol = nullptr;
    std::vector<Renderer> renderers;
    std::vector<std::wstring> dev_names;
    std::vector<std::wstring> dev_ids;
    bool flash_state = false;
} G;

#define WM_APP_LOG      (WM_APP + 1)   // lParam = new'd std::wstring*
#define WM_APP_RENDERS  (WM_APP + 2)   // renderer list updated
#define WM_APP_FLASH    (WM_APP + 3)   // metronome tick
#define IDT_STATS       100
#define IDT_FLASHOFF    101
#define IDC_RENDER_BASE 2000

static bool g_console_mode = false;
static FILE* g_probe_file = nullptr;
static FILE* g_sess_log = nullptr;          // persistent diagnostics: lowcast.log
static CRITICAL_SECTION g_sess_log_cs;
static void sess_log_open() {
    InitializeCriticalSection(&g_sess_log_cs);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\') --n;
    wcscpy_s(path + n, MAX_PATH - n, L"lowcast.log");
    g_sess_log = _wfopen(path, L"a");
}
static void ui_log(const std::wstring& msg) {
    if (g_sess_log) {
        SYSTEMTIME st; GetLocalTime(&st);
        std::string u8 = wide_to_utf8(msg);
        EnterCriticalSection(&g_sess_log_cs);
        fprintf(g_sess_log, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, u8.c_str());
        fflush(g_sess_log);
        LeaveCriticalSection(&g_sess_log_cs);
    }
    if (g_console_mode) {
        fwprintf(stdout, L"%ls\n", msg.c_str()); fflush(stdout);
        if (g_probe_file) {
            std::string u8 = wide_to_utf8(msg);
            fprintf(g_probe_file, "%s\n", u8.c_str()); fflush(g_probe_file);
        }
    }
    if (!G.hwnd) return;
    PostMessageW(G.hwnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(msg));
}
static void ui_log8(const std::string& msg) { ui_log(utf8_to_wide(msg)); }

// ---------------------------------------------------------------------------
// Encoded-audio fan-out: capture thread encodes once, pushes to every client
// ---------------------------------------------------------------------------
static const size_t MAX_CLIENT_BACKLOG = 4 * 1024 * 1024;  // ~21s @48k/16 — stalled client

static void fanout(const uint8_t* data, size_t len) {
    EnterCriticalSection(&G.cs);
    uint64_t worst = 0;
    uint32_t rate = G.sample_rate.load();
    int bytes_per_frame = (fmt_bits((Fmt)G.fmt.load()) / 8) * 2;
    for (Client* c : G.clients) {
        if (c->dead) continue;
        if (c->q.size() + len > MAX_CLIENT_BACKLOG) {
            c->dead = true;
            if (c->evt) SetEvent(c->evt);          // unblock so it can exit
            continue;
        }
        c->q.insert(c->q.end(), data, data + len);
        if (c->evt) SetEvent(c->evt);              // wake sender NOW (no 2 ms poll)
        uint64_t ms10 = (uint64_t)c->q.size() * 10000ULL / ((uint64_t)rate * bytes_per_frame);
        if (ms10 > worst) worst = ms10;
    }
    G.backlog_ms_x10.store(worst);
    LeaveCriticalSection(&G.cs);
}

// mark every connected client dead and wake it (format/rate changed under it);
// returns how many were ALIVE until now (0 = nothing actually ended)
static int kill_all_clients() {
    int killed = 0;
    EnterCriticalSection(&G.cs);
    for (Client* c : G.clients) {
        if (!c->dead) { c->dead = true; killed++; }
        if (c->evt) SetEvent(c->evt);
    }
    LeaveCriticalSection(&G.cs);
    return killed;
}

// ---------------------------------------------------------------------------
// WASAPI loopback capture thread
//  - polls every 3 ms, converts to selected bit depth, injects wall-clock
//    silence when nothing is playing (keeps renderer fed at realtime),
//    optional 1 kHz metronome tick for latency measurement.
// ---------------------------------------------------------------------------
static float read_sample(const uint8_t* p, int bits, bool is_float) {
    if (is_float) { float f; memcpy(&f, p, 4); return f; }
    if (bits == 16) { int16_t v; memcpy(&v, p, 2); return v / 32768.0f; }
    if (bits == 24) {
        int32_t v = (p[0] | (p[1] << 8) | (p[2] << 16));
        if (v & 0x800000) v |= 0xFF000000;
        return v / 8388608.0f;
    }
    if (bits == 32) { int32_t v; memcpy(&v, p, 4); return v / 2147483648.0f; }
    return 0.f;
}

struct Encoder {
    // stereo float -> LPCM/WAV bytes. LPCM (audio/L16, L24) is BIG-endian,
    // WAV is little-endian.
    Fmt  fmt;
    bool big_endian;
    int  bits;
    uint32_t dseed = 0x243F6A88u;
    std::vector<uint8_t> out;
    void begin(Fmt f) {
        fmt = f; bits = fmt_bits(f); big_endian = !fmt_is_wav(f);
        out.clear();
    }
    inline void push(float l, float r) {
        float s[2] = { l, r };
        for (int ch = 0; ch < 2; ++ch) {
            float v = s[ch];
            if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
            if (bits == 16) {
                dseed = dseed * 1664525u + 1013904223u;
                float d1 = (float)(dseed >> 8) * (1.0f / 16777216.0f);
                dseed = dseed * 1664525u + 1013904223u;
                float dt = d1 - (float)(dseed >> 8) * (1.0f / 16777216.0f);
                long ql = lrintf(v * 32767.f + dt);       // TPDF dither
                if (ql > 32767) ql = 32767; else if (ql < -32768) ql = -32768;
                int16_t q = (int16_t)ql;
                uint8_t b0 = (uint8_t)(q & 0xFF), b1 = (uint8_t)((q >> 8) & 0xFF);
                if (big_endian) { out.push_back(b1); out.push_back(b0); }
                else            { out.push_back(b0); out.push_back(b1); }
            } else {
                int32_t q = (int32_t)lrint((double)v * 8388607.0);
                uint8_t b0 = (uint8_t)(q & 0xFF), b1 = (uint8_t)((q >> 8) & 0xFF),
                        b2 = (uint8_t)((q >> 16) & 0xFF);
                if (big_endian) { out.push_back(b2); out.push_back(b1); out.push_back(b0); }
                else            { out.push_back(b0); out.push_back(b1); out.push_back(b2); }
            }
        }
    }
};

static void raop_pipe_push(float l, float r);
static void raop_send_volume_pct(int vp);    // one RAOP volume command, guarded
static void raop_push_volume(int pct);       // queue a volume for the live session
static bool raop_is_running();
static void raop_stats(double& secs_sent, double& hb_age_s, unsigned long long& resends,
                       unsigned& reconnects);

DEFINE_GUID(IID_IAudioClient3_L, 0x7ED4EE07, 0x8E67, 0x4CD4,
            0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42);
DEFINE_GUID(IID_IAudioRenderClient_L2, 0xF294ACFC, 0x3146, 0x4483,
            0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

static std::atomic<bool> g_period_driver_run{false};
struct PeriodDriverArgs { IMMDevice* dev; };

// Keeps a minimum-period silent render stream open on the capture device so
// the shared audio engine ticks at its fastest rate -> loopback delivers
// audio in ~3 ms chunks instead of ~10 ms. Fails silently where unsupported.
static DWORD WINAPI period_driver_thread(LPVOID p) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    PeriodDriverArgs* a = (PeriodDriverArgs*)p;
    IMMDevice* dev = a->dev;
    delete a;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IAudioClient3* ac3 = nullptr;
    WAVEFORMATEX* wf = nullptr;
    IAudioRenderClient* rc = nullptr;
    HANDLE evt = nullptr;
    do {
        if (FAILED(dev->Activate(IID_IAudioClient3_L, CLSCTX_ALL, nullptr, (void**)&ac3)))
            { ui_log(L"[audio] IAudioClient3 unavailable — engine period unchanged"); break; }
        if (FAILED(ac3->GetMixFormat(&wf))) break;
        UINT32 def = 0, fund = 0, mn = 0, mx = 0;
        if (FAILED(ac3->GetSharedModeEnginePeriod(wf, &def, &fund, &mn, &mx))) break;
        if (mn >= def) { ui_log(L"[audio] engine already at minimum period"); break; }
        HRESULT hr = ac3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                      mn, wf, nullptr);
        if (FAILED(hr)) { ui_log(L"[audio] low-period stream rejected by driver"); break; }
        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ac3->SetEventHandle(evt);
        if (FAILED(ac3->GetService(IID_IAudioRenderClient_L2, (void**)&rc))) break;
        UINT32 bufsz = 0; ac3->GetBufferSize(&bufsz);
        ac3->Start();
        {
            wchar_t b[160];
            swprintf(b, 160, L"[audio] engine period driver active: %u -> %u frames "
                     L"(~%.1f ms) — loopback now low-latency", def, mn,
                     mn * 1000.0 / wf->nSamplesPerSec);
            ui_log(b);
        }
        while (g_period_driver_run.load()) {
            WaitForSingleObject(evt, 20);
            UINT32 pad = 0; ac3->GetCurrentPadding(&pad);
            UINT32 want = bufsz - pad;
            if (!want) continue;
            BYTE* out = nullptr;
            if (SUCCEEDED(rc->GetBuffer(want, &out))) {
                rc->ReleaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
        ac3->Stop();
    } while (false);
    if (rc) rc->Release();
    if (wf) CoTaskMemFree(wf);
    if (ac3) ac3->Release();
    if (evt) CloseHandle(evt);
    dev->Release();
    CoUninitialize();
    return 0;
}
static void raop_start(const std::string& host, int port, uint32_t latency_ms);
static void raop_stop();
static void raop_resolve();

// DMX_BEGIN
// ---------------------------------------------------------------------------
// Multichannel -> stereo downmix (ITU-style coefficients).
// Previously only channels 0/1 were kept: on a 7.1 device (e.g. Sonar's
// 8-channel endpoint) that DISCARDED center (most game dialog!), sides,
// and rears. Coefficients per speaker position:
//   FL/FR 1.0    FC -3dB into both    SL/SR & BL/BR -3dB to their side
//   BC -6dB into both    LFE omitted (standard ITU practice: avoids boom
//   and clipping; sub-bass content is normally mirrored in the mains)
// Normalized only when the coefficient sum exceeds 1.707 (the classic 5.1
// budget), so plain stereo passes through at exactly unity as before.
// ---------------------------------------------------------------------------
static void build_downmix(uint32_t mask, int ch, float* cl, float* cr) {
    enum { FL=0x1, FR=0x2, FC=0x4, LFE=0x8, BL=0x10, BR=0x20,
           FLC=0x40, FRC=0x80, BC=0x100, SL=0x200, SR=0x400 };
    static const uint32_t def_order[8] = { FL, FR, FC, LFE, BL, BR, SL, SR };
    if (ch == 1) { cl[0] = cr[0] = 1.0f; return; }
    const float C = 0.70710678f;
    // walk the mask's set bits in order; fall back to the standard layout
    uint32_t bits = mask;
    for (int c = 0; c < ch && c < 32; ++c) {
        uint32_t pos = 0;
        if (bits) {
            pos = bits & (~bits + 1u);           // lowest set bit
            bits &= bits - 1u;                   // clear it
        } else {
            pos = (c < 8) ? def_order[c] : 0;    // maskless: assume std order
        }
        float l = 0.f, r = 0.f;
        switch (pos) {
            case FL:  l = 1.0f;          break;
            case FR:  r = 1.0f;          break;
            case FC:  l = C;    r = C;   break;
            case LFE:                     break;   // omitted (see header)
            case BL: case SL: case FLC: l = C;  break;
            case BR: case SR: case FRC: r = C;  break;
            case BC:  l = 0.5f; r = 0.5f; break;
            default:                      break;   // heights/unknown: dropped
        }
        cl[c] = l; cr[c] = r;
    }
    float suml = 0.f, sumr = 0.f;
    for (int c = 0; c < ch && c < 32; ++c) { suml += cl[c]; sumr += cr[c]; }
    float s = suml > sumr ? suml : sumr;
    if (s > 1.707f) {                    // headroom cap; stereo stays at unity
        float k = 1.707f / s;
        for (int c = 0; c < ch && c < 32; ++c) { cl[c] *= k; cr[c] *= k; }
    }
}
// DMX_END

static DWORD WINAPI capture_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* denum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_L, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator_L, (void**)&denum);
    if (FAILED(hr)) { ui_log(L"[audio] device enumerator failed");
                      G.capture_run.store(false); CoUninitialize(); return 1; }

    IMMDevice* dev = nullptr;
    int idx = G.capture_dev.load();
    bool following_default = !(idx >= 0 && idx < (int)G.dev_ids.size());
    if (!following_default)
        hr = denum->GetDevice(G.dev_ids[idx].c_str(), &dev);
    else
        hr = denum->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (FAILED(hr) || !dev) { ui_log(L"[audio] cannot open capture device");
                              denum->Release(); G.capture_run.store(false);
                              CoUninitialize(); return 1; }
    // remember which endpoint we're on so we can notice if the default moves
    std::wstring opened_id;
    { LPWSTR wid = nullptr; if (SUCCEEDED(dev->GetId(&wid)) && wid) { opened_id = wid; CoTaskMemFree(wid); } }

    // spin up the engine-period driver on this device (AddRef'd handle)
    if (!g_period_driver_run.exchange(true)) {
        dev->AddRef();
        PeriodDriverArgs* pda = new PeriodDriverArgs{ dev };
        CloseHandle(CreateThread(nullptr, 0, period_driver_thread, pda, 0, nullptr));
    }

    IAudioClient* ac = nullptr;
    hr = dev->Activate(IID_IAudioClient_L, CLSCTX_ALL, nullptr, (void**)&ac);
    WAVEFORMATEX* wf = nullptr;
    if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&wf);
    if (FAILED(hr)) {
        ui_log(L"[audio] mix format failed");
        if (ac) ac->Release();
        dev->Release(); denum->Release();
        G.capture_run.store(false); CoUninitialize(); return 1;
    }

    bool src_float = false; int src_bits = wf->wBitsPerSample; int src_ch = wf->nChannels;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) src_float = true;
    else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* we = (WAVEFORMATEXTENSIBLE*)wf;
        src_float = IsEqualGUID(we->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_L);
    }
    G.native_rate.store(wf->nSamplesPerSec);
    G.measured_rate.store((double)wf->nSamplesPerSec);
    {
        // If the advertised rate changed (device switch, e.g. 44.1k -> 48k
        // headset), streams negotiated at the OLD rate would now play at the
        // wrong speed/pitch. Close them like the format-change path does.
        uint32_t adv = wf->nSamplesPerSec * (uint32_t)G.upmult.load();
        uint32_t old = G.sample_rate.exchange(adv);
        if (old != adv && kill_all_clients() > 0)
            ui_log(L"[audio] stream rate changed \x2014 active streams closed; press START again");
    }

    // Event-driven loopback: the smallest shared-mode buffer Windows allows,
    // and we wake the instant audio is ready rather than polling on a timer.
    // (Loopback REQUIRES shared mode — exclusive mode cannot capture the system
    //  mix, so shared is not a limitation here, it is mandatory.)
    HANDLE audio_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        0 /*min buffer*/, 0, wf, nullptr);
    bool event_mode = SUCCEEDED(hr);
    if (event_mode) hr = ac->SetEventHandle(audio_evt);
    if (!event_mode || FAILED(hr)) {
        // fallback: some drivers reject event-mode loopback -> polled capture
        event_mode = false;
        if (ac) { ac->Release(); ac = nullptr; }
        if (wf) { CoTaskMemFree(wf); wf = nullptr; }
        hr = dev->Activate(IID_IAudioClient_L, CLSCTX_ALL, nullptr, (void**)&ac);
        if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&wf);
        if (SUCCEEDED(hr))
            hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                100000, 0, wf, nullptr);
        ui_log(L"[audio] event-mode unavailable, using polled capture");
    } else {
        ui_log(L"[audio] event-driven capture active");
    }
    IAudioCaptureClient* cap = nullptr;
    if (SUCCEEDED(hr)) hr = ac->GetService(IID_IAudioCaptureClient_L, (void**)&cap);
    if (SUCCEEDED(hr)) hr = ac->Start();
    if (FAILED(hr)) {
        ui_log(L"[audio] WASAPI loopback init failed");
        if (cap) cap->Release();
        if (ac) ac->Release();
        if (wf) CoTaskMemFree(wf);
        if (audio_evt) CloseHandle(audio_evt);
        dev->Release(); denum->Release();
        G.capture_run.store(false); CoUninitialize(); return 1;
    }

    {
        wchar_t buf[128];
        swprintf(buf, 128, L"[audio] capturing: %u Hz, %d ch, %s%d-bit source",
                 wf->nSamplesPerSec, src_ch, src_float ? L"float " : L"", src_bits);
        ui_log(buf);
    }

    const uint32_t rate = wf->nSamplesPerSec;      // native pacing rate
    const int  mult = G.upmult.load();             // linear-interp upsampling
    const int src_bpf = wf->nBlockAlign;
    // stereo downmix coefficients for ALL source channels (see build_downmix)
    uint32_t chmask = 0;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
        chmask = ((WAVEFORMATEXTENSIBLE*)wf)->dwChannelMask;
    float dmx_l[32] = {0}, dmx_r[32] = {0};
    int dmx_ch = src_ch > 32 ? 32 : src_ch;
    build_downmix(chmask, dmx_ch, dmx_l, dmx_r);
    if (src_ch > 2) {
        wchar_t mb[96];
        swprintf(mb, 96, L"[audio] downmixing %d channels (mask 0x%X) \x2192 stereo",
                 src_ch, chmask);
        ui_log(mb);
    }
    Encoder enc;
    float prev_l = 0.f, prev_r = 0.f;
    auto emit = [&](float l, float r) {            // upsample + encode one frame
        for (int k = 1; k <= mult; ++k) {
            float t = (float)k / (float)mult;
            enc.push(prev_l + (l - prev_l) * t, prev_r + (r - prev_r) * t);
        }
        prev_l = l; prev_r = r;
    };
    uint64_t t0 = now_ms();
    uint64_t total_frames = 0;          // frames pushed downstream (real + silence)
    double   beep_phase = 0.0;
    uint64_t next_beep_frame = 0;
    uint64_t beep_until_frame = 0;
    const double beep_step = 2.0 * 3.14159265358979 * 1000.0 / rate;

    bool beep_prev = false;
    auto beep_mix = [&](float& l, float& r) {
        bool on = G.beep_on.load() && !G.beep_local.load();
        if (on && !beep_prev) {                     // just enabled: schedule ahead
            next_beep_frame = total_frames + rate / 10;
            beep_until_frame = 0;
        }
        beep_prev = on;
        if (!on) return;
        if (total_frames >= next_beep_frame) {
            beep_until_frame = next_beep_frame + rate / 16;      // ~62 ms tick
            next_beep_frame += rate * 2;                          // every 2 s
            beep_phase = 0.0;
            G.beep_flash_ms.store(now_ms());
            PostMessageW(G.hwnd, WM_APP_FLASH, 0, 0);
        }
        if (total_frames < beep_until_frame) {
            float b = 0.30f * (float)sin(beep_phase);
            beep_phase += beep_step;
            l += b; r += b;
        }
    };

    bool device_changed = false;
    uint64_t last_devcheck = now_ms();
    // true-rate measurement state. Locals (not statics): a fresh capture
    // thread (device switch) must never blend the old device's clock in.
    uint64_t meas_t0 = 0, meas_frames0 = 0, real_frame_count = 0;
    // [diag] capture health counters, reported every 10 s (read-only telemetry)
    uint64_t dg_pkts = 0, dg_disc = 0, dg_gapmax = 0, dg_lastpkt = 0, dg_fill = 0;
    uint64_t dg_next = now_ms() + 10000;
    while (G.capture_run.load()) {
        // detect a default-output switch (e.g. user changes Windows output device)
        if (following_default && now_ms() - last_devcheck > 500) {
            last_devcheck = now_ms();
            IMMDevice* cur = nullptr;
            if (SUCCEEDED(denum->GetDefaultAudioEndpoint(eRender, eConsole, &cur)) && cur) {
                LPWSTR cid = nullptr;
                if (SUCCEEDED(cur->GetId(&cid)) && cid) {
                    if (opened_id != cid) { device_changed = true; }
                    CoTaskMemFree(cid);
                }
                cur->Release();
            }
            if (device_changed) {
                ui_log(L"[audio] default output changed \x2014 switching capture device");
                break;
            }
        }
        Fmt f = (Fmt)G.fmt.load();
        enc.begin(f);

        // 1) drain everything WASAPI has for us
        UINT32 pkt = 0;
        bool got_real = false;
        uint64_t got_real_frames = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
            got_real = true;
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            dg_pkts++;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) dg_disc++;   // WASAPI glitched
            { uint64_t nn = now_ms();
              if (dg_lastpkt && nn - dg_lastpkt > dg_gapmax) dg_gapmax = nn - dg_lastpkt;
              dg_lastpkt = nn; }
            for (UINT32 i = 0; i < frames; ++i) {
                float l = 0.f, r = 0.f;
                if (!silent && data) {
                    const uint8_t* p = data + (size_t)i * src_bpf;
                    if (src_ch <= 2) {               // stereo/mono: exact passthrough
                        l = read_sample(p, src_bits, src_float);
                        r = (src_ch > 1) ? read_sample(p + src_bits / 8, src_bits, src_float) : l;
                    } else {                          // multichannel: full downmix
                        for (int c = 0; c < dmx_ch; ++c) {
                            float v = read_sample(p + (size_t)c * (src_bits / 8),
                                                  src_bits, src_float);
                            l += v * dmx_l[c];
                            r += v * dmx_r[c];
                        }
                    }
                }
                beep_mix(l, r);
                raop_pipe_push(l, r);
                emit(l, r);
                total_frames++;
            }
            got_real_frames += frames;
            cap->ReleaseBuffer(frames);
        }

        // 2) wall-clock silence fill: if nothing is playing, WASAPI delivers no
        //    packets, but the renderer must keep receiving realtime audio or it
        //    stalls and re-buffers (adding latency back). Keep it fed.
        //    Re-anchor the clock whenever real audio flows so soundcard-vs-QPC
        //    drift can never accumulate into an audible mid-music gap.
        if (got_real) t0 = now_ms() - total_frames * 1000ULL / rate;

        // measure the soundcard's TRUE sample rate over a long window: real frames
        // delivered by WASAPI vs wall-clock elapsed. This is the ppm-accurate rate
        // the RAOP resampler must use to avoid slow buffer drift at the receiver.
        {
            real_frame_count += got_real_frames;
            uint64_t nowm = now_ms();
            if (meas_t0 == 0) { meas_t0 = nowm; meas_frames0 = real_frame_count; }
            uint64_t win = nowm - meas_t0;
            if (win >= 4000) {                        // update every 4 s
                uint64_t df = real_frame_count - meas_frames0;
                double measured = (double)df * 1000.0 / (double)win;
                // sanity clamp: within 2% of nominal, else keep previous
                if (measured > rate * 0.98 && measured < rate * 1.02) {
                    double prev = G.measured_rate.load();
                    G.measured_rate.store(prev * 0.85 + measured * 0.15);  // EMA
                }
                meas_t0 = nowm; meas_frames0 = real_frame_count;
            }
        }
        uint64_t expected = (now_ms() - t0) * rate / 1000ULL;
        if (expected > total_frames + rate / 50) {                // >20 ms behind
            uint64_t fill = expected - total_frames;
            if (fill > rate / 4) fill = rate / 4;                 // cap 250 ms burst
            for (uint64_t i = 0; i < fill; ++i) {
                float l = 0.f, r = 0.f;
                beep_mix(l, r);
                raop_pipe_push(l, r);
                emit(l, r);
                total_frames++;
            }
            G.silence_frames.fetch_add(fill);
            dg_fill += fill;
            // A window containing wall-clock fill has real frames MISSING from
            // it: a sub-2% pause (e.g. 70 ms in 4 s) passes the sanity clamp
            // with a rate up to 2% low — 20,000 ppm of error against a servo
            // whose authority is ±400 ppm. The pipe then swells for tens of
            // seconds (latency creep). Silence-fill => the window is invalid.
            meas_t0 = 0;
        }

        if (!enc.out.empty()) {
            fanout(enc.out.data(), enc.out.size());
            G.frames_captured.store(total_frames);
        }
        if (now_ms() >= dg_next) {                       // [diag] 10 s capture report
            wchar_t db[160];
            swprintf(db, 160, L"[diag] cap: pkts=%llu disc=%llu maxgap=%llums fill=%llums",
                     (unsigned long long)dg_pkts, (unsigned long long)dg_disc,
                     (unsigned long long)dg_gapmax,
                     (unsigned long long)(dg_fill * 1000 / rate));
            ui_log(db);
            dg_pkts = dg_disc = dg_gapmax = dg_fill = 0;
            dg_next += 10000;
        }
        if (event_mode) WaitForSingleObject(audio_evt, 5);   // wake on audio-ready
        else            Sleep(1);
    }

    ac->Stop();
    if (audio_evt) CloseHandle(audio_evt);
    cap->Release(); ac->Release(); CoTaskMemFree(wf); dev->Release(); denum->Release();
    CoUninitialize();
    // seamless follow: if the default device moved (not a user Stop), the engine
    // period driver is tied to the old device, so retire it and relaunch capture.
    if (device_changed && G.capture_run.load()) {
        g_period_driver_run.store(false);
        Sleep(150);
        extern HANDLE g_capture_handle;
        HANDLE nh = CreateThread(nullptr, 0, capture_thread, nullptr, 0, nullptr);
        HANDLE old = g_capture_handle;
        g_capture_handle = nh;
        if (old) CloseHandle(old);   // release the (now-exiting) prior handle
    }
    return 0;
}

// ---------------------------------------------------------------------------
// HTTP streaming server
//  - one thread per connection; sends headers immediately, then chunked
//    audio the instant it arrives from the capture thread. TCP_NODELAY on,
//    small socket send buffer so the kernel can't hide latency either.
// ---------------------------------------------------------------------------
static bool send_all(SOCKET s, const void* buf, int len) {
    const char* p = (const char*)buf;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}
// WAV header for an endless stream (0xFFFFFFFF sizes, the streaming convention)
static std::vector<uint8_t> wav_header(uint32_t rate, int bits) {
    auto le16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(x&0xFF); v.push_back(x>>8); };
    auto le32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i=0;i<4;++i) v.push_back((x>>(8*i))&0xFF); };
    std::vector<uint8_t> h;
    uint16_t ch = 2, ba = (uint16_t)(ch * bits / 8);
    h.insert(h.end(), {'R','I','F','F'}); le32(h, 0xFFFFFFFF);
    h.insert(h.end(), {'W','A','V','E','f','m','t',' '}); le32(h, 16);
    le16(h, 1); le16(h, ch); le32(h, rate);
    le32(h, rate * ba); le16(h, ba); le16(h, (uint16_t)bits);
    h.insert(h.end(), {'d','a','t','a'}); le32(h, 0xFFFFFFFF);
    return h;
}

static std::string content_type(Fmt f, uint32_t rate) {
    char buf[96];
    if (fmt_is_wav(f)) return "audio/vnd.wave;codec=1";
    snprintf(buf, sizeof(buf), "audio/L%d;rate=%u;channels=2", fmt_bits(f), rate);
    return buf;
}

static void qos_tag_voice(SOCKET s, const sockaddr_in& dst);   // defined in RAOP section

static DWORD WINAPI client_thread(LPVOID param) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SOCKET s = (SOCKET)(uintptr_t)param;

    // latency-critical socket options
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    int sndbuf = 32 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

    sockaddr_in peer{}; int plen = sizeof(peer);
    getpeername(s, (sockaddr*)&peer, &plen);
    char peer_ip[64]; inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
    qos_tag_voice(s, peer);       // DSCP EF / WMM airtime priority for DLNA too

    // read request (headers only)
    std::string req; char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { closesocket(s); return 0; }
        req.append(buf, n);
        if (req.size() > 16384) break;
    }
    bool is_head = (req.rfind("HEAD", 0) == 0);
    ui_log8(std::string("[http] ") + (is_head ? "HEAD" : "GET") + " from " + peer_ip);

    Fmt f = (Fmt)G.fmt.load();
    uint32_t rate = G.sample_rate.load();
    std::string hdr =
        "HTTP/1.1 200 OK\r\n"
        "Server: LowCast\r\n"
        "Content-Type: " + content_type(f, rate) + "\r\n"
        "TransferMode.dlna.org: Streaming\r\n"
        "Connection: close\r\n";
    if (!is_head) hdr += "Transfer-Encoding: chunked\r\n";
    hdr += "\r\n";
    if (!send_all(s, hdr.data(), (int)hdr.size())) { closesocket(s); return 0; }
    if (is_head) { closesocket(s); return 0; }

    Client* c = new Client();
    c->sock = s; c->remote = peer_ip;
    c->evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    if (fmt_is_wav(f)) {
        auto wh = wav_header(rate, fmt_bits(f));
        c->q.insert(c->q.end(), wh.begin(), wh.end());
    }
    EnterCriticalSection(&G.cs);
    G.clients.push_back(c);
    LeaveCriticalSection(&G.cs);
    ui_log8(std::string("[http] streaming to ") + peer_ip + " (" + content_type(f, rate) + ")");

    // pump: woken by fanout() the instant bytes are queued, ship immediately.
    // (Previously polled with Sleep(2): up to 2 ms of avoidable queue-sit.)
    // The chunk is framed IN-PLACE (hex length + payload + CRLF) and sent
    // with ONE send(): with TCP_NODELAY, three send() calls could emit three
    // separate segments = three WiFi airtime grabs per chunk.
    std::vector<uint8_t> local;
    bool ok = true;
    while (ok) {
        local.clear();
        size_t qlen = 0;
        EnterCriticalSection(&G.cs);
        if (c->dead) ok = false;
        else if (!c->q.empty()) {
            qlen = c->q.size();
            char chdr[16];
            int hl = snprintf(chdr, sizeof(chdr), "%zx\r\n", qlen);
            local.reserve(qlen + hl + 2);
            local.insert(local.end(), chdr, chdr + hl);
            local.insert(local.end(), c->q.begin(), c->q.end());
            c->q.clear();
            local.push_back('\r'); local.push_back('\n');
        }
        LeaveCriticalSection(&G.cs);
        if (!ok) break;
        if (local.empty()) {
            if (c->evt) WaitForSingleObject(c->evt, 100);
            else Sleep(2);                       // event creation failed: poll
            continue;
        }
        ok = send_all(s, local.data(), (int)local.size());
        if (ok) c->sent_bytes += qlen;
    }
    send_all(s, "0\r\n\r\n", 5);
    closesocket(s);

    EnterCriticalSection(&G.cs);
    for (size_t i = 0; i < G.clients.size(); ++i)
        if (G.clients[i] == c) { G.clients.erase(G.clients.begin() + i); break; }
    LeaveCriticalSection(&G.cs);
    ui_log8(std::string("[http] client ") + peer_ip + " disconnected");
    if (c->evt) CloseHandle(c->evt);
    delete c;
    return 0;
}

// plays the metronome tick through the DEFAULT Windows output device with the
// same on-screen flash. Point Windows at your Bluetooth headphones and this
// measures the BT path latency for an apples-to-apples comparison with WiFi.
static DWORD WINAPI local_beep_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* de = nullptr; IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr;
    WAVEFORMATEX* wf = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator_L, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator_L, (void**)&de))) return 1;
    if (FAILED(de->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) { de->Release(); return 1; }
    if (FAILED(dev->Activate(IID_IAudioClient_L, CLSCTX_ALL, nullptr, (void**)&ac))) return 1;
    if (FAILED(ac->GetMixFormat(&wf))) return 1;
    if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 400000 /*40ms*/, 0, wf, nullptr))) return 1;
    static const GUID IID_IAudioRenderClient_L =
        { 0xF294ACFC, 0x3146, 0x4483, {0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2} };
    if (FAILED(ac->GetService(IID_IAudioRenderClient_L, (void**)&rc))) return 1;
    UINT32 bufsz = 0; ac->GetBufferSize(&bufsz);
    ac->Start();
    const uint32_t rate = wf->nSamplesPerSec;
    const int ch = wf->nChannels;
    bool is_float = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(((WAVEFORMATEXTENSIBLE*)wf)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_L));
    uint64_t frame = 0, next_tick = rate / 4, tick_end = 0;
    double ph = 0.0, step = 2.0 * 3.14159265358979 * 1000.0 / rate;
    ui_log(L"[test] local beep: ticking on the DEFAULT Windows output (2 s period)");
    while (G.beep_local.load()) {
        UINT32 pad = 0; ac->GetCurrentPadding(&pad);
        UINT32 want = bufsz - pad;
        if (want == 0) { Sleep(4); continue; }
        BYTE* out = nullptr;
        if (FAILED(rc->GetBuffer(want, &out))) break;
        for (UINT32 i = 0; i < want; ++i) {
            if (frame >= next_tick) {
                tick_end = next_tick + rate / 16;
                next_tick += rate * 2;
                ph = 0.0;
                G.beep_flash_ms.store(now_ms());
                PostMessageW(G.hwnd, WM_APP_FLASH, 0, 0);
            }
            float v = 0.f;
            if (frame < tick_end) { v = 0.30f * (float)sin(ph); ph += step; }
            for (int c = 0; c < ch; ++c) {
                if (is_float) memcpy(out + (i * ch + c) * 4, &v, 4);
                else { int16_t s16 = (int16_t)lrintf(v * 32767.f);
                       memcpy(out + (i * ch + c) * 2, &s16, 2); }
            }
            frame++;
        }
        rc->ReleaseBuffer(want, 0);
        Sleep(8);
    }
    ac->Stop();
    rc->Release(); ac->Release(); CoTaskMemFree(wf); dev->Release(); de->Release();
    CoUninitialize();
    return 0;
}

static DWORD WINAPI http_server_thread(LPVOID) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
    int port = 16600;
    for (; port < 16620; ++port) {
        a.sin_port = htons((u_short)port);
        if (bind(ls, (sockaddr*)&a, sizeof(a)) == 0) break;
    }
    G.http_port.store(port);
    listen(ls, 8);
    {
        wchar_t b[96]; swprintf(b, 96, L"[http] server listening on port %d", port);
        ui_log(b);
    }
    for (;;) {
        SOCKET cs = accept(ls, nullptr, nullptr);
        if (cs == INVALID_SOCKET) break;
        CloseHandle(CreateThread(nullptr, 0, client_thread, (LPVOID)(uintptr_t)cs, 0, nullptr));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Tiny HTTP client (for fetching device descriptions + SOAP POSTs)
// ---------------------------------------------------------------------------
struct Url { std::string host; int port = 80; std::string path = "/"; };
static bool parse_url(const std::string& u, Url& out) {
    size_t p = ifind(u, "http://");
    if (p != 0) return false;
    size_t hs = 7, he = u.find('/', hs);
    std::string hostport = (he == std::string::npos) ? u.substr(hs) : u.substr(hs, he - hs);
    out.path = (he == std::string::npos) ? "/" : u.substr(he);
    size_t c = hostport.find(':');
    if (c == std::string::npos) { out.host = hostport; out.port = 80; }
    else { out.host = hostport.substr(0, c); out.port = atoi(hostport.c_str() + c + 1); }
    return !out.host.empty();
}
static bool http_request(const Url& u, const std::string& raw, std::string& resp, int timeout_ms = 5000) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD tmo = timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)u.port);
    if (inet_pton(AF_INET, u.host.c_str(), &a.sin_addr) != 1) {
        addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
        if (getaddrinfo(u.host.c_str(), nullptr, &hints, &res) != 0 || !res) { closesocket(s); return false; }
        a.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return false; }
    if (!send_all(s, raw.data(), (int)raw.size())) { closesocket(s); return false; }
    char buf[4096]; int n;
    resp.clear();
    long long want = -1;                     // total bytes once Content-Length known
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, n);
        if (want < 0) {
            size_t he = resp.find("\r\n\r\n");
            if (he != std::string::npos) {
                size_t cl = ifind(resp, "content-length:");
                if (cl != std::string::npos && cl < he)
                    want = (long long)(he + 4) + atoll(resp.c_str() + cl + 15);
            }
        }
        if (want >= 0 && (long long)resp.size() >= want) break;   // keep-alive: done
        if (resp.size() > 2 * 1024 * 1024) break;
    }
    closesocket(s);
    return !resp.empty();
}
static std::string dechunk(const std::string& b) {
    std::string out; size_t p = 0;
    while (p < b.size()) {
        size_t e = b.find("\r\n", p);
        if (e == std::string::npos) break;
        long len = strtol(b.c_str() + p, nullptr, 16);
        if (len <= 0) break;
        p = e + 2;
        if (p + len > b.size()) { out.append(b, p, b.size() - p); break; }
        out.append(b, p, len);
        p += len + 2;
    }
    return out;
}
static std::string http_get(const std::string& url) {
    Url u; if (!parse_url(url, u)) return {};
    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nUser-Agent: LowCast\r\n\r\n",
             u.path.c_str(), u.host.c_str(), u.port);
    std::string resp;
    if (!http_request(u, req, resp)) return {};
    size_t body = resp.find("\r\n\r\n");
    if (body == std::string::npos) return {};
    std::string hdrs = resp.substr(0, body);
    std::string b = resp.substr(body + 4);
    if (ifind(hdrs, "transfer-encoding: chunked") != std::string::npos) b = dechunk(b);
    return b;
}

// local IP used to reach a given renderer host (handles multi-NIC correctly)
static std::string local_ip_for(const std::string& target_host) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(9);
    if (inet_pton(AF_INET, target_host.c_str(), &a.sin_addr) != 1) {
        addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
        if (getaddrinfo(target_host.c_str(), nullptr, &hints, &res) == 0 && res) {
            a.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
    }
    connect(s, (sockaddr*)&a, sizeof(a));
    sockaddr_in me{}; int ml = sizeof(me);
    getsockname(s, (sockaddr*)&me, &ml);
    closesocket(s);
    char ip[64]; inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip));
    return ip;
}

// ---------------------------------------------------------------------------
// SSDP discovery + UPnP AVTransport control (handshake per swyh-rs)
// ---------------------------------------------------------------------------
#include <iphlpapi.h>
static std::vector<std::string> local_ipv4s() {
    std::vector<std::string> ips;
    ULONG sz = 16 * 1024;
    std::vector<uint8_t> buf(sz);
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &sz) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(sz); aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &sz) != NO_ERROR) return ips;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[64];
            inet_ntop(AF_INET, &((sockaddr_in*)ua->Address.lpSockaddr)->sin_addr, ip, sizeof(ip));
            if (strncmp(ip, "169.254.", 8) == 0) continue;   // link-local
            ips.push_back(ip);
        }
    }
    return ips;
}

static void ssdp_discover() {
    std::vector<std::string> ifs = local_ipv4s();
    if (ifs.empty()) { ui_log(L"[ssdp] no usable network interfaces found"); return; }
    {
        std::string s = "[ssdp] searching on:";
        for (auto& ip : ifs) s += " " + ip;
        ui_log8(s + "  (3.5 s)");
    }

    // one socket per interface, multicast pinned to that interface
    std::vector<SOCKET> socks;
    sockaddr_in mcast{}; mcast.sin_family = AF_INET; mcast.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &mcast.sin_addr);
    const char* sts[4] = { "urn:schemas-upnp-org:device:MediaRenderer:1",
                           "urn:schemas-upnp-org:service:AVTransport:1",
                           "urn:av-openhome-org:service:Product:1",
                           "ssdp:all" };
    for (auto& ipstr : ifs) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        sockaddr_in ba{}; ba.sin_family = AF_INET; ba.sin_port = 0;
        inet_pton(AF_INET, ipstr.c_str(), &ba.sin_addr);
        if (bind(s, (sockaddr*)&ba, sizeof(ba)) != 0) { closesocket(s); continue; }
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ba.sin_addr, sizeof(ba.sin_addr));
        int ttl = 4;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        for (const char* st : sts) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "M-SEARCH * HTTP/1.1\r\nHost: 239.255.255.250:1900\r\n"
                     "Man: \"ssdp:discover\"\r\nST: %s\r\nMX: 2\r\n\r\n", st);
            // send twice; SSDP is lossy UDP
            sendto(s, msg, (int)strlen(msg), 0, (sockaddr*)&mcast, sizeof(mcast));
            sendto(s, msg, (int)strlen(msg), 0, (sockaddr*)&mcast, sizeof(mcast));
        }
        socks.push_back(s);
    }

    std::vector<std::string> locations;
    uint64_t start = now_ms();
    char buf[8192];
    while (now_ms() - start < 3500) {
        fd_set rd; FD_ZERO(&rd);
        for (SOCKET s : socks) FD_SET(s, &rd);
        timeval tv{ 0, 200000 };
        int r = select(0, &rd, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        for (SOCKET s : socks) {
            if (!FD_ISSET(s, &rd)) continue;
            sockaddr_in from{}; int fl = sizeof(from);
            int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;
            buf[n] = 0;
            std::string resp(buf);
            size_t lp = ifind(resp, "location:");
            if (lp == std::string::npos) continue;
            lp += 9;
            while (lp < resp.size() && resp[lp] == ' ') ++lp;
            size_t le = resp.find("\r\n", lp);
            std::string loc = resp.substr(lp, le - lp);
            bool known = false;
            for (auto& k : locations) if (k == loc) { known = true; break; }
            if (!known) {
                locations.push_back(loc);
                char fip[64]; inet_ntop(AF_INET, &from.sin_addr, fip, sizeof(fip));
                ui_log8(std::string("[ssdp] response from ") + fip + " -> " + loc);
            }
        }
    }
    for (SOCKET s : socks) closesocket(s);
    if (locations.empty())
        ui_log(L"[ssdp] no SSDP responses at all — check Windows Firewall allowed LowCast on Private networks");

    std::vector<Renderer> found;
    for (auto& loc : locations) {
        std::string xml = http_get(loc);
        if (xml.empty()) { ui_log8("[ssdp] " + loc + ": description fetch FAILED"); continue; }
        if (g_console_mode) {                       // probe: keep raw XML for debugging
            FILE* fx = fopen("lowcast-desc.xml", "a");
            if (fx) { fprintf(fx, "<!-- %s -->\n%s\n\n", loc.c_str(), xml.c_str()); fclose(fx); }
        }
        // walk each <service> block; a block may list controlURL BEFORE
        // serviceType (order is not guaranteed by UPnP), so match per-block.
        std::string ctrl, oh_ctrl, cm_ctrl;
        size_t pos = 0;
        while (true) {
            size_t sb = ifind(xml, "<service>", pos);
            if (sb == std::string::npos) break;
            size_t se = ifind(xml, "</service>", sb);
            if (se == std::string::npos) break;
            std::string block = xml.substr(sb, se - sb);
            pos = se + 10;
            std::string type = xml_tag(block, "serviceType");
            if (ifind(type, "urn:schemas-upnp-org:service:AVTransport") != std::string::npos
                && ctrl.empty())
                ctrl = xml_tag(block, "controlURL");
            if (ifind(type, "urn:av-openhome-org:service:Playlist") != std::string::npos
                && oh_ctrl.empty())
                oh_ctrl = xml_tag(block, "controlURL");
            if (ifind(type, "urn:schemas-upnp-org:service:ConnectionManager") != std::string::npos
                && cm_ctrl.empty())
                cm_ctrl = xml_tag(block, "controlURL");
        }
        if (ctrl.empty() && oh_ctrl.empty()) {
            std::string nm = xml_tag(xml, "friendlyName");
            ui_log8("[ssdp] skipping " + (nm.empty() ? loc : nm)
                    + " (no AVTransport service/controlURL)");
            continue;
        }
        Renderer r;
        r.location = loc;
        r.name = xml_tag(xml, "friendlyName");
        if (r.name.empty()) r.name = loc;
        Url base; if (!parse_url(loc, base)) continue;
        auto resolve = [&](const std::string& c) -> std::string {
            if (c.empty()) return {};
            if (ifind(c, "http://") == 0) return c;
            char cb[512];
            if (c[0] == '/')
                snprintf(cb, sizeof(cb), "http://%s:%d%s", base.host.c_str(), base.port, c.c_str());
            else {
                std::string dir = base.path.substr(0, base.path.rfind('/') + 1);
                snprintf(cb, sizeof(cb), "http://%s:%d%s%s", base.host.c_str(), base.port, dir.c_str(), c.c_str());
            }
            return cb;
        };
        r.control_url = resolve(ctrl);
        r.oh_url = resolve(oh_ctrl);
        r.cm_url = resolve(cm_ctrl);
        r.host = base.host;
        if (!r.oh_url.empty())
            ui_log8("[ssdp]   OpenHome Playlist control: " + r.oh_url + "  (preferred)");
        if (!r.control_url.empty())
            ui_log8("[ssdp]   AVTransport control: " + r.control_url);
        // dedup (a device can answer several STs / interfaces)
        bool dup = false;
        for (auto& f2 : found)
            if (f2.control_url + f2.oh_url == r.control_url + r.oh_url) { dup = true; break; }
        if (dup) continue;
        found.push_back(r);
        ui_log8("[ssdp] found renderer: " + r.name + " @ " + r.host);
    }
    if (found.empty()) ui_log(L"[ssdp] no renderers found — is the headphone in WiFi mode on the same network?");

    EnterCriticalSection(&G.cs);
    for (auto& nr : found)
        for (auto& old : G.renderers)
            if (old.control_url + old.oh_url == nr.control_url + nr.oh_url)
                nr.streaming = old.streaming;
    G.renderers = found;
    LeaveCriticalSection(&G.cs);
    PostMessageW(G.hwnd, WM_APP_RENDERS, 0, 0);
}

// minimal mDNS service enumeration: legacy unicast query for the service list.
// AirPlay/Chromecast/Spotify announce over mDNS, NOT SSDP, so the SSDP scan
// cannot see them. This answers "does the device have a realtime pipeline?"
static SOCKET mdns_listener();
static void mdns_sweep() {
    ui_log(L"[mdns] sweeping for AirPlay/Chromecast/realtime services (2 s)...");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in ba{}; ba.sin_family = AF_INET; ba.sin_addr.s_addr = INADDR_ANY;
    bind(s, (sockaddr*)&ba, sizeof(ba));
    DWORD tmo = 300;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    int ttl = 4;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    // DNS query: PTR _services._dns-sd._udp.local (enumerate all service types)
    static const uint8_t q[] = {
        0x00,0x00, 0x00,0x00, 0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
        9,'_','s','e','r','v','i','c','e','s',
        7,'_','d','n','s','-','s','d',
        4,'_','u','d','p',
        5,'l','o','c','a','l', 0,
        0x00,0x0C, 0x00,0x01 };
    sockaddr_in mc{}; mc.sin_family = AF_INET; mc.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &mc.sin_addr);
    sendto(s, (const char*)q, sizeof(q), 0, (sockaddr*)&mc, sizeof(mc));
    sendto(s, (const char*)q, sizeof(q), 0, (sockaddr*)&mc, sizeof(mc));

    struct Marker { const char* tag; const wchar_t* label; };
    static const Marker marks[] = {
        { "_raop",            L"AirPlay audio (RAOP) — REALTIME protocol!" },
        { "_airplay",         L"AirPlay" },
        { "_googlecast",      L"Chromecast" },
        { "_spotify-connect", L"Spotify Connect" },
        { "_qplay",           L"QPlay" },
        { "_roon",            L"Roon" },
    };
    SOCKET m = mdns_listener();
    if (m != INVALID_SOCKET)   // standard (QM) query from 5353: multicast reply
        sendto(m, (const char*)q, sizeof(q), 0, (sockaddr*)&mc, sizeof(mc));
    bool any = false;
    uint64_t start = now_ms();
    char buf[4096];
    while (now_ms() - start < 2000) {
        fd_set rd; FD_ZERO(&rd);
        FD_SET(s, &rd);
        if (m != INVALID_SOCKET) FD_SET(m, &rd);
        timeval tv{ 0, 200000 };
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET rs = FD_ISSET(s, &rd) ? s : m;
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(rs, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        char fip[64]; inet_ntop(AF_INET, &from.sin_addr, fip, sizeof(fip));
        std::string pkt(buf, buf + n);
        for (auto& m : marks) {
            if (pkt.find(m.tag) != std::string::npos) {
                wchar_t line[256];
                swprintf(line, 256, L"[mdns] %hs announces: %ls", fip, m.label);
                ui_log(line);
                any = true;
            }
        }
    }
    closesocket(s);
    if (m != INVALID_SOCKET) closesocket(m);
    if (!any) ui_log(L"[mdns] no mDNS traffic heard on either unicast or multicast path");
}

static bool soap_post_url(const std::string& control_url, const std::string& soapaction,
                          const std::string& body, std::string* out_resp = nullptr) {
    Url u; if (!parse_url(control_url, u)) return false;
    const char* action = soapaction.c_str();
    // header set and order mirror swyh-rs (ureq) byte-for-byte: some embedded
    // SOAP stacks are irrationally picky, so match a client known to work.
    char hdr[1024];
    snprintf(hdr, sizeof(hdr),
             "POST %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "User-Agent: swyh-rs/1.20.5\r\n"
             "Accept: */*\r\n"
             "SOAPAction: \"%s\"\r\n"
             "Content-Type: text/xml; charset=\"utf-8\"\r\n"
             "Content-Length: %zu\r\n\r\n",
             u.path.c_str(), u.host.c_str(), u.port, action, body.size());
    std::string resp;
    if (!http_request(u, std::string(hdr) + body, resp)) {
        ui_log8(std::string("[upnp] ") + action + ": no response");
        return false;
    }
    bool ok = resp.find("200 OK") != std::string::npos;
    if (!ok) {
        size_t e = resp.find("\r\n");
        std::string code = xml_tag(resp, "errorCode");
        std::string desc = xml_tag(resp, "errorDescription");
        std::string msg = std::string("[upnp] ") + action + " failed: " + resp.substr(0, e);
        if (!code.empty()) msg += "  (UPnP error " + code + (desc.empty() ? "" : " — " + desc) + ")";
        ui_log8(msg);
        if (g_console_mode) {
            size_t b = resp.find("\r\n\r\n");
            std::string body_txt = (b == std::string::npos) ? resp : resp.substr(b + 4);
            if (body_txt.size() > 500) body_txt.resize(500);
            ui_log8("[upnp]   fault body: " + body_txt);
        }
    }
    if (out_resp) *out_resp = resp;
    return ok;
}

static bool soap_post(const Renderer& r, const char* action, const std::string& body) {
    return soap_post_url(r.control_url,
        std::string("urn:schemas-upnp-org:service:AVTransport:1#") + action, body);
}
static bool soap_post_oh(const Renderer& r, const char* action, const std::string& body) {
    return soap_post_url(r.oh_url,
        std::string("urn:av-openhome-org:service:Playlist:1#") + action, body);
}

static const char* OH_PLAY_BODY =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
    "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
    "<s:Body><u:Play xmlns:u=\"urn:av-openhome-org:service:Playlist:1\"/></s:Body></s:Envelope>";
static const char* OH_DELETE_BODY =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
    "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
    "<s:Body><u:DeleteAll xmlns:u=\"urn:av-openhome-org:service:Playlist:1\"/></s:Body></s:Envelope>";

static bool try_cast_format(Renderer& r, Fmt f) {
    uint32_t rate = G.sample_rate.load();
    std::string ip = local_ip_for(r.host);
    char uri[256];
    snprintf(uri, sizeof(uri), "http://%s:%d/stream.%s", ip.c_str(), G.http_port.load(),
             fmt_is_wav(f) ? "wav" : "pcm");

    // protocolInfo strings per swyh-rs
    char prot[256];
    if (fmt_is_wav(f))
        snprintf(prot, sizeof(prot),
                 "http-get:*:audio/wav:DLNA.ORG_PN=WAV;DLNA.ORG_OP=01;DLNA.ORG_CI=0;"
                 "DLNA.ORG_FLAGS=03700000000000000000000000000000");
    else
        snprintf(prot, sizeof(prot),
                 "http-get:*:audio/L%d;rate=%u;channels=2:DLNA.ORG_PN=LPCM",
                 fmt_bits(f), rate);

    char didl[1024];
    snprintf(didl, sizeof(didl),
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
        "<item id=\"1\" parentID=\"0\" restricted=\"0\">"
        "<dc:title>LowCast</dc:title>"
        "<res bitsPerSample=\"%d\" nrAudioChannels=\"2\" sampleFrequency=\"%u\" "
        "protocolInfo=\"%s\" duration=\"00:00:00\">%s</res>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "</item></DIDL-Lite>",
        fmt_bits(f), rate, prot, uri);

    // ---- OpenHome path (preferred when the device exposes Playlist) ----
    if (!r.oh_url.empty()) {
        std::string insert_body =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
            "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Body><u:Insert xmlns:u=\"urn:av-openhome-org:service:Playlist:1\">"
            "<AfterId>0</AfterId>"
            "<Uri>" + std::string(uri) + "</Uri>"
            "<Metadata>" + xml_escape(didl) + "</Metadata>"
            "</u:Insert></s:Body></s:Envelope>";
        ui_log8("[upnp/oh] casting " + std::string(uri) + " -> " + r.name);
        soap_post_oh(r, "DeleteAll", OH_DELETE_BODY);       // Moode et al. need this
        if (!soap_post_oh(r, "Insert", insert_body)) return false;
        return soap_post_oh(r, "Play", OH_PLAY_BODY);
    }

    // ---- classic UPnP AVTransport path ----
    std::string set_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID>"
        "<CurrentURI>" + std::string(uri) + "</CurrentURI>"
        "<CurrentURIMetaData>" + xml_escape(didl) + "</CurrentURIMetaData>"
        "</u:SetAVTransportURI></s:Body></s:Envelope>";

    std::string stop_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:Stop xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID></u:Stop></s:Body></s:Envelope>";

    std::string play_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID><Speed>1</Speed></u:Play></s:Body></s:Envelope>";

    ui_log8("[upnp/av] casting " + std::string(uri) + " -> " + r.name);
    soap_post(r, "Stop", stop_body);            // prevents 705 transport-locked
    bool set_ok = soap_post(r, "SetAVTransportURI", set_body);
    if (!set_ok) {
        // classic workaround for renderers with broken DIDL parsers
        // (same trick as BubbleUPnP's "don't send metadata" option)
        ui_log(L"[upnp/av] retrying SetAVTransportURI with empty metadata");
        std::string bare_body =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
            "<InstanceID>0</InstanceID>"
            "<CurrentURI>" + std::string(uri) + "</CurrentURI>"
            "<CurrentURIMetaData></CurrentURIMetaData>"
            "</u:SetAVTransportURI></s:Body></s:Envelope>";
        set_ok = soap_post(r, "SetAVTransportURI", bare_body);
    }
    if (!set_ok) return false;
    Sleep(100);
    return soap_post(r, "Play", play_body);
}

// read-only device interrogation used when a cast handshake fails
static void interrogate_device(const Renderer& r) {
    ui_log(L"[diag] --- device interrogation (read-only queries) ---");
    // fetch the AVTransport SCPD: the device's own list of implemented actions
    if (!r.control_url.empty()) {
        std::string scpd_url = r.control_url;
        size_t p = scpd_url.rfind("/ctl-");
        if (p != std::string::npos) {
            scpd_url = scpd_url.substr(0, p) + "/" +
                       "urn-schemas-upnp-org-service-AVTransport-1.xml";
            std::string scpd = http_get(scpd_url);
            if (!scpd.empty()) {
                std::string actions = "[diag] SCPD actions:";
                size_t q = 0;
                while (true) {
                    size_t a = ifind(scpd, "<action>", q);
                    if (a == std::string::npos) break;
                    size_t e = ifind(scpd, "</action>", a);
                    if (e == std::string::npos) break;
                    std::string nm = xml_tag(scpd.substr(a, e - a + 9), "name");
                    if (!nm.empty()) actions += " " + nm;
                    q = e + 9;
                }
                ui_log8(actions);
            } else {
                ui_log8("[diag] SCPD fetch failed: " + scpd_url);
            }
        }
    }
    if (!r.control_url.empty()) {
        std::string body =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:GetTransportInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
            "<InstanceID>0</InstanceID></u:GetTransportInfo></s:Body></s:Envelope>";
        std::string resp;
        bool ok = soap_post_url(r.control_url,
            "urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo", body, &resp);
        if (ok) {
            std::string st = xml_tag(resp, "CurrentTransportState");
            std::string ss = xml_tag(resp, "CurrentTransportStatus");
            ui_log8("[diag] GetTransportInfo OK — state=" + st + " status=" + ss
                    + "  (transport ALIVE: write actions are being refused, not the service)");
        } else {
            ui_log8("[diag] GetTransportInfo ALSO fails — the whole AVTransport service is "
                    "dead/stubbed in the device's current state (power-cycle the headphone, "
                    "make sure no other app/session — Spotify Connect, HiFiMAN app — owns it)");
        }
    }
    if (!r.cm_url.empty()) {
        std::string body =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:GetProtocolInfo xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\"/>"
            "</s:Body></s:Envelope>";
        std::string resp;
        bool ok = soap_post_url(r.cm_url,
            "urn:schemas-upnp-org:service:ConnectionManager:1#GetProtocolInfo", body, &resp);
        if (ok) {
            std::string sink = xml_tag(resp, "Sink");
            if (sink.size() > 700) sink.resize(700);
            ui_log8("[diag] device-declared supported formats (Sink): " + sink);
        } else {
            ui_log8("[diag] GetProtocolInfo failed too");
        }
    } else {
        ui_log(L"[diag] no ConnectionManager service advertised");
    }
    ui_log(L"[diag] --- end interrogation ---");
}

static bool start_cast(Renderer& r) {
    Fmt sel = (Fmt)G.fmt.load();
    if (try_cast_format(r, sel)) return true;

    // Renderer refused the proposal. Uncompressed WAV is the most widely
    // accepted container and adds no latency — try it before giving up.
    Fmt alt = (fmt_bits(sel) == 24) ? Fmt::WAV24 : Fmt::WAV16;
    if (alt == sel) {
        alt = (fmt_bits(sel) == 24) ? Fmt::WAV16 : Fmt::WAV24;  // last resort: other depth
        if (alt == sel) return false;
    }
    ui_log8(std::string("[upnp] renderer rejected ") + (fmt_is_wav(sel) ? "WAV" : "LPCM")
            + " — retrying as WAV " + (fmt_bits(alt) == 24 ? "24" : "16") + "-bit");
    if (!try_cast_format(r, alt)) return false;
    G.fmt.store((int)alt);                       // HTTP server must serve what we promised
    PostMessageW(G.hwnd, WM_APP_RENDERS, 2, 0);  // sync the format dropdown
    return true;
}

static void stop_cast(Renderer& r) {
    if (!r.oh_url.empty()) {
        soap_post_oh(r, "DeleteAll", OH_DELETE_BODY);
        ui_log8("[upnp/oh] stopped " + r.name);
        return;
    }
    std::string stop_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:Stop xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID></u:Stop></s:Body></s:Envelope>";
    soap_post(r, "Stop", stop_body);
    ui_log8("[upnp] stopped " + r.name);
}

// ---------------------------------------------------------------------------
// Audio device enumeration for the capture-source dropdown
// ---------------------------------------------------------------------------
#include <propsys.h>
static void enum_render_devices() {
    G.dev_names.clear(); G.dev_ids.clear();
    IMMDeviceEnumerator* denum = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator_L, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator_L, (void**)&denum))) return;
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(denum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
        UINT n = 0; coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d))) continue;
            LPWSTR id = nullptr; d->GetId(&id);
            IPropertyStore* ps = nullptr;
            std::wstring name = L"(unknown device)";
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPERTYKEY key; key.fmtid = PKEY_Device_FriendlyName_fmtid; key.pid = 14;
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt == VT_LPWSTR) name = pv.pwszVal;
                PropVariantClear(&pv);
                ps->Release();
            }
            G.dev_names.push_back(name);
            G.dev_ids.push_back(id ? id : L"");
            if (id) CoTaskMemFree(id);
            d->Release();
        }
        coll->Release();
    }
    denum->Release();
}

// ---------------------------------------------------------------------------
// Capture restart (device/format change)
// ---------------------------------------------------------------------------
HANDLE g_capture_handle = nullptr;
static void stop_capture() {
    g_period_driver_run.store(false);
    G.capture_run.store(false);
    if (g_capture_handle) {
        WaitForSingleObject(g_capture_handle, 3000);
        CloseHandle(g_capture_handle);
        g_capture_handle = nullptr;
    }
}
static void start_capture() {
    stop_capture();
    G.capture_run.store(true);
    G.frames_captured.store(0);
    G.silence_frames.store(0);
    g_capture_handle = CreateThread(nullptr, 0, capture_thread, nullptr, 0, nullptr);
}

static void raop_resolve();
static DWORD WINAPI discover_thread(LPVOID) { ssdp_discover(); raop_resolve(); return 0; }
struct CastJob { int index; bool start; };
static DWORD WINAPI cast_thread(LPVOID p);

// A device has ONE DAC: a DLNA session and an AirPlay session to the same host
// are mutually exclusive. Stop any DLNA renderer bound to `host` before we hand
// the device to RAOP (or the RAOP audio streams into a DAC still owned by DLNA).
static void stop_dlna_to_host(const std::string& host) {
    std::vector<int> victims;
    EnterCriticalSection(&G.cs);
    for (size_t i = 0; i < G.renderers.size(); ++i)
        if (G.renderers[i].streaming && G.renderers[i].host == host)
            victims.push_back((int)i);
    LeaveCriticalSection(&G.cs);
    for (int idx : victims) {
        ui_log8("[raop] stopping DLNA session to " + host +
                " first (one device, one output)");
        CastJob* job = new CastJob{ idx, false };
        HANDLE t = CreateThread(nullptr, 0, cast_thread, job, 0, nullptr);
        if (t) { WaitForSingleObject(t, 4000); CloseHandle(t); }
    }
    if (!victims.empty()) Sleep(600);   // let the firmware release the pipeline
}
static DWORD WINAPI cast_thread(LPVOID p) {
    CastJob* job = (CastJob*)p;
    EnterCriticalSection(&G.cs);
    bool valid = job->index >= 0 && job->index < (int)G.renderers.size();
    Renderer r = valid ? G.renderers[job->index] : Renderer();
    LeaveCriticalSection(&G.cs);
    if (valid) {
        bool ok = true;
        if (job->start) ok = start_cast(r);
        else stop_cast(r);
        EnterCriticalSection(&G.cs);
        if (job->index < (int)G.renderers.size())
            G.renderers[job->index].streaming = job->start && ok;
        LeaveCriticalSection(&G.cs);
        PostMessageW(G.hwnd, WM_APP_RENDERS, 1, 0);   // refresh button labels only
    }
    delete job;
    return 0;
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------
static HFONT g_font = nullptr, g_font_big = nullptr;
static HBRUSH g_flash_on = nullptr, g_flash_off = nullptr;

static std::vector<HWND> g_render_buttons;   // owned by the UI thread only

// AirPlay combo mappings — shared by the UI handlers and settings load/save
static const int g_ap_lats[7] = { 25, 50, 75, 100, 150, 250, 350 };

// ---------------------------------------------------------------------------
// Settings persistence: %APPDATA%\LowCast.ini — format, rates, AirPlay
// buffer & start volume, capture device (by name), window position.
// ---------------------------------------------------------------------------
static std::wstring ini_path() {
    wchar_t p[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%APPDATA%\\LowCast.ini", p, MAX_PATH) || p[0] == L'%')
        return L".\\LowCast.ini";
    return p;
}

static void save_settings(HWND h) {
    std::wstring ini = ini_path();
    wchar_t v[64];
    auto put = [&](const wchar_t* k, int val) {
        swprintf(v, 64, L"%d", val);
        WritePrivateProfileStringW(L"LowCast", k, v, ini.c_str());
    };
    put(L"fmt",   (int)SendMessageW(G.cmb_fmt,   CB_GETCURSEL, 0, 0));
    put(L"rate",  (int)SendMessageW(G.cmb_rate,  CB_GETCURSEL, 0, 0));
    put(L"aplat", (int)SendMessageW(G.cmb_aplat, CB_GETCURSEL, 0, 0));
    put(L"apvolpct", G.ap_start_vol.load());   // percent; -1 = never touched
    int ds = (int)SendMessageW(G.cmb_dev, CB_GETCURSEL, 0, 0);
    wchar_t dn[256] = L"";
    if (ds > 0 && SendMessageW(G.cmb_dev, CB_GETLBTEXTLEN, ds, 0) < 255)
        SendMessageW(G.cmb_dev, CB_GETLBTEXT, ds, (LPARAM)dn);
    WritePrivateProfileStringW(L"LowCast", L"device", dn, ini.c_str());
    RECT r;
    if (GetWindowRect(h, &r)) { put(L"winx", r.left); put(L"winy", r.top); }
}

static void load_settings() {
    std::wstring ini = ini_path();
    auto geti = [&](const wchar_t* k, int def) {
        return (int)GetPrivateProfileIntW(L"LowCast", k, def, ini.c_str());
    };
    int fs = geti(L"fmt", 0);
    if (fs >= 0 && fs < 4) { SendMessageW(G.cmb_fmt, CB_SETCURSEL, fs, 0); G.fmt.store(fs); }
    int rsel = geti(L"rate", 0);
    if (rsel >= 0 && rsel < 3) {
        SendMessageW(G.cmb_rate, CB_SETCURSEL, rsel, 0);
        G.upmult.store(rsel == 2 ? 4 : rsel == 1 ? 2 : 1);
    }
    int ls = geti(L"aplat", 4);
    if (ls >= 0 && ls < 7) { SendMessageW(G.cmb_aplat, CB_SETCURSEL, ls, 0); G.ap_latency_ms.store(g_ap_lats[ls]); }
    int vs = geti(L"apvolpct", -1);
    if (vs >= 0 && vs <= 100) {
        SendMessageW(G.sld_apvol, TBM_SETPOS, TRUE, 100 - vs);   // top = 100%
        wchar_t vt[16]; swprintf(vt, 16, L"%d%%", vs);
        SetWindowTextW(G.lbl_apvol, vt);
        G.ap_start_vol.store(vs);
    }                                              // else: slider shows "auto"
    wchar_t dn[256] = L"";
    GetPrivateProfileStringW(L"LowCast", L"device", L"", dn, 256, ini.c_str());
    if (dn[0]) {
        int n = (int)SendMessageW(G.cmb_dev, CB_GETCOUNT, 0, 0);
        for (int i = 1; i < n; ++i) {
            wchar_t t[256] = L"";
            if (SendMessageW(G.cmb_dev, CB_GETLBTEXTLEN, i, 0) >= 255) continue;
            SendMessageW(G.cmb_dev, CB_GETLBTEXT, i, (LPARAM)t);
            if (wcscmp(t, dn) == 0) {
                SendMessageW(G.cmb_dev, CB_SETCURSEL, i, 0);
                G.capture_dev.store(i - 1);
                ui_log(L"[app] restored saved capture device");
                break;                     // absent device: stays on Default
            }
        }
    }
}

static void layout_renderer_buttons(HWND hwnd) {
    // destroy old buttons (UI thread owns these; survives renderer list swaps)
    for (HWND b : g_render_buttons) if (b) DestroyWindow(b);
    g_render_buttons.clear();
    EnterCriticalSection(&G.cs);
    for (auto& r : G.renderers) r.button = nullptr;
    for (auto& d : g_airplay) d.button = nullptr;
    int count = (int)G.renderers.size();
    LeaveCriticalSection(&G.cs);

    int y = 182;
    EnterCriticalSection(&G.cs);
    // re-check the live size: the list can be swapped by a discovery thread
    // between the two lock holds (count alone would index out of bounds)
    for (int i = 0; i < count && i < (int)G.renderers.size(); ++i) {
        Renderer& r = G.renderers[i];
        std::wstring label = (r.streaming ? L"\x25A0  STOP   —  " : L"\x25B6  START  —  ")
                             + utf8_to_wide(r.name);
        r.button = CreateWindowW(L"BUTTON", label.c_str(),
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 12, y, 560, 34, hwnd,
                                 (HMENU)(uintptr_t)(IDC_RENDER_BASE + i),
                                 nullptr, nullptr);
        SendMessageW(r.button, WM_SETFONT, (WPARAM)g_font_big, TRUE);
        g_render_buttons.push_back(r.button);
        y += 40;
    }
    LeaveCriticalSection(&G.cs);

    // AirPlay realtime buttons
    EnterCriticalSection(&G.cs);
    for (size_t i = 0; i < g_airplay.size(); ++i) {
        AirplayDev& d = g_airplay[i];
        bool live = d.streaming && raop_is_running();
        std::wstring label = (live ? L"\x25A0  STOP   \x2014  \x26A1 AirPlay: "
                                   : L"\x25B6  START  \x2014  \x26A1 AirPlay: ")
                             + utf8_to_wide(d.name);
        d.button = CreateWindowW(L"BUTTON", label.c_str(),
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 12, y, 560, 34, hwnd,
                                 (HMENU)(uintptr_t)(IDC_RENDER_BASE + 1000 + i),
                                 nullptr, nullptr);
        SendMessageW(d.button, WM_SETFONT, (WPARAM)g_font_big, TRUE);
        g_render_buttons.push_back(d.button);
        y += 40;
    }
    LeaveCriticalSection(&G.cs);
    if (count == 0 && g_airplay.empty()) y += 4;

    // move the controls that sit below the button list
    SetWindowPos(G.flash, nullptr, 12, y + 4, 560, 26, SWP_NOZORDER);
    SetWindowPos(G.stat,  nullptr, 12, y + 36, 560, 58, SWP_NOZORDER);
    SetWindowPos(G.log,   nullptr, 12, y + 98, 560, 150, SWP_NOZORDER);
    RECT rc; GetWindowRect(hwnd, &rc);
    SetWindowPos(hwnd, nullptr, 0, 0, 600, y + 300, SWP_NOZORDER | SWP_NOMOVE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void sync_airplay_button_state() {
    static bool was_running = false;
    bool running = raop_is_running();
    if (was_running && !running) {
        EnterCriticalSection(&G.cs);
        for (auto& d : g_airplay) d.streaming = false;
        LeaveCriticalSection(&G.cs);
        ui_log(L"[raop] session ended \x2014 button reset");
    }
    EnterCriticalSection(&G.cs);
    for (auto& d : g_airplay) {
        if (!d.button) continue;
        bool live = d.streaming && running;
        std::wstring want = (live ? L"\x25A0  STOP   \x2014  \x26A1 AirPlay: "
                                  : L"\x25B6  START  \x2014  \x26A1 AirPlay: ")
                            + utf8_to_wide(d.name);
        wchar_t cur[256]; GetWindowTextW(d.button, cur, 256);
        if (want != cur) SetWindowTextW(d.button, want.c_str());
    }
    LeaveCriticalSection(&G.cs);
    was_running = running;
}

static void update_stats() {
    sync_airplay_button_state();
    Fmt f = (Fmt)G.fmt.load();
    uint32_t rate = G.sample_rate.load();
    uint64_t backlog10 = G.backlog_ms_x10.load();
    EnterCriticalSection(&G.cs);
    int nclients = (int)G.clients.size();
    uint64_t sent = 0;
    for (Client* c : G.clients) sent += c->sent_bytes;
    LeaveCriticalSection(&G.cs);
    double mbit = rate * 2.0 * fmt_bits(f) / 1e6;
    wchar_t b[512];
    if (raop_is_running()) {
        double secs_sent, hb_age; unsigned long long rs; unsigned rc;
        raop_stats(secs_sent, hb_age, rs, rc);
        swprintf(b, 512,
            L"AirPlay LIVE: %.1f s of audio sent   heartbeat: %s%.1fs ago   "
            L"resends: %llu   reconnects: %u\r\n"
            L"DLNA: %u Hz / %d-bit / %s   connections: %d   sent: %.1f MB\r\n"
            L"If 'audio sent' climbs but you hear silence, another session "
            L"(Spotify?) owns the receiver.",
            secs_sent, hb_age < 0 ? L"NONE " : L"", hb_age < 0 ? 0.0 : hb_age,
            rs, rc,
            rate, fmt_bits(f), fmt_is_wav(f) ? L"WAV" : L"LPCM", nclients,
            sent / 1048576.0);
    } else {
        swprintf(b, 512,
            L"Stream: %u Hz / %d-bit / %s  (%.1f Mbit/s)      Renderer connections: %d\r\n"
            L"Sender-side latency (capture\x2192network): %.1f ms      Sent: %.1f MB\r\n"
            L"End-to-end = sender + headphone firmware buffer \x2192 use the beep test",
            rate, fmt_bits(f), fmt_is_wav(f) ? L"WAV" : L"LPCM", mbit, nclients,
            backlog10 / 10.0 + 3.0 /*capture poll*/, sent / 1048576.0);
    }
    SetWindowTextW(G.stat, b);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        G.hwnd = h;
        g_font = CreateFontW(-15, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
                             0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_big = CreateFontW(-16, 0,0,0, FW_SEMIBOLD, 0,0,0, DEFAULT_CHARSET,
                             0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_flash_on  = CreateSolidBrush(RGB(255, 220, 40));
        g_flash_off = CreateSolidBrush(RGB(60, 60, 66));

        HWND lbl1 = CreateWindowW(L"STATIC", L"Capture source (all system audio):",
                      WS_CHILD | WS_VISIBLE, 12, 10, 300, 18, h, nullptr, nullptr, nullptr);
        G.cmb_dev = CreateWindowW(L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                      12, 30, 350, 300, h, (HMENU)1, nullptr, nullptr);
        HWND lbl2 = CreateWindowW(L"STATIC", L"Audio type (DLNA only):",
                      WS_CHILD | WS_VISIBLE, 12, 66, 90, 18, h, nullptr, nullptr, nullptr);
        G.cmb_fmt = CreateWindowW(L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                      100, 62, 262, 200, h, (HMENU)2, nullptr, nullptr);
        G.btn_scan = CreateWindowW(L"BUTTON", L"Rescan",
                      WS_CHILD | WS_VISIBLE, 372, 30, 118, 26, h, (HMENU)3, nullptr, nullptr);
        G.btn_beep = CreateWindowW(L"BUTTON", L"Latency beep test",
                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                      372, 62, 118, 26, h, (HMENU)4, nullptr, nullptr);
        HWND lbl3 = CreateWindowW(L"STATIC", L"Rate:",
                      WS_CHILD | WS_VISIBLE, 12, 98, 90, 18, h, nullptr, nullptr, nullptr);
        SendMessageW(lbl3, WM_SETFONT, (WPARAM)0, TRUE);
        G.cmb_rate = CreateWindowW(L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                      100, 94, 262, 200, h, (HMENU)6, nullptr, nullptr);
        G.btn_localbeep = CreateWindowW(L"BUTTON", L"Beep via PC out",
                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                      372, 94, 118, 26, h, (HMENU)7, nullptr, nullptr);
        HWND lbl4 = CreateWindowW(L"STATIC", L"AirPlay buffer:",
                      WS_CHILD | WS_VISIBLE, 12, 130, 90, 18, h, nullptr, nullptr, nullptr);
        SendMessageW(lbl4, WM_SETFONT, (WPARAM)g_font, TRUE);
        G.cmb_aplat = CreateWindowW(L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                      100, 126, 390, 200, h, (HMENU)8, nullptr, nullptr);
        SendMessageW(G.cmb_aplat, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"25 ms - floor probe (loss-repair verified)");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"50 ms - insane");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"75 ms - extreme");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"100 ms - aggressive");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"150 ms - fast");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"250 ms - recommended");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"350 ms - safe");
        SendMessageW(G.cmb_aplat, CB_SETCURSEL, 4, 0);   // default 150 ms
        // vertical volume column, right of the Rescan / beep buttons.
        // NOTE: vertical trackbars put position 0 at the TOP and report via
        // WM_VSCROLL, so displayed percent = 100 - position (up = louder).
        HWND lbl5 = CreateWindowW(L"STATIC", L"Vol",
                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                      500, 10, 56, 16, h, nullptr, nullptr, nullptr);
        SendMessageW(lbl5, WM_SETFONT, (WPARAM)g_font, TRUE);
        G.sld_apvol = CreateWindowW(TRACKBAR_CLASSW, nullptr,
                      WS_CHILD | WS_VISIBLE | TBS_VERT | TBS_NOTICKS,
                      512, 28, 32, 96, h, (HMENU)9, nullptr, nullptr);
        SendMessageW(G.sld_apvol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(G.sld_apvol, TBM_SETPOS, TRUE, 0);   // top = 100% / auto
        G.lbl_apvol = CreateWindowW(L"STATIC", L"auto",
                      WS_CHILD | WS_VISIBLE | SS_CENTER, 500, 128, 56, 18,
                      h, nullptr, nullptr, nullptr);
        SendMessageW(G.lbl_apvol, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(G.cmb_dev,   CB_SETDROPPEDWIDTH, 390, 0);  // long device names
        G.flash = CreateWindowW(L"STATIC", L"  beep flash indicator (every 2 s) — flash\x2192tick gap = latency",
                      WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                      12, 154, 560, 26, h, (HMENU)5, nullptr, nullptr);
        G.stat = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                      12, 154, 560, 58, h, nullptr, nullptr, nullptr);
        G.log = CreateWindowW(L"EDIT", L"",
                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                      12, 216, 560, 150, h, nullptr, nullptr, nullptr);
        for (HWND c : { lbl1, lbl2, lbl3, G.cmb_dev, G.cmb_fmt, G.cmb_rate,
                        G.btn_scan, G.btn_beep, G.btn_localbeep, G.flash, G.stat, G.log })
            SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);

        SendMessageW(G.cmb_rate, CB_ADDSTRING, 0, (LPARAM)L"DLNA: Native rate  (48 kHz typical)");
        SendMessageW(G.cmb_rate, CB_ADDSTRING, 0, (LPARAM)L"DLNA: 2\x00D7 upsample \x2014 halves firmware-buffer latency");
        SendMessageW(G.cmb_rate, CB_ADDSTRING, 0, (LPARAM)L"DLNA: 4\x00D7 upsample \x2014 quarters it (~9 Mbit/s max)");
        SendMessageW(G.cmb_rate, CB_SETCURSEL, 0, 0);

        SendMessageW(G.cmb_fmt, CB_ADDSTRING, 0, (LPARAM)L"DLNA: LPCM 16-bit  (lowest latency — recommended)");
        SendMessageW(G.cmb_fmt, CB_ADDSTRING, 0, (LPARAM)L"DLNA: LPCM 24-bit");
        SendMessageW(G.cmb_fmt, CB_ADDSTRING, 0, (LPARAM)L"DLNA: WAV 16-bit   (best compatibility)");
        SendMessageW(G.cmb_fmt, CB_ADDSTRING, 0, (LPARAM)L"DLNA: WAV 24-bit");
        SendMessageW(G.cmb_fmt, CB_SETCURSEL, 0, 0);

        enum_render_devices();
        SendMessageW(G.cmb_dev, CB_ADDSTRING, 0, (LPARAM)L"Default output device");
        for (auto& n : G.dev_names)
            SendMessageW(G.cmb_dev, CB_ADDSTRING, 0, (LPARAM)n.c_str());
        SendMessageW(G.cmb_dev, CB_SETCURSEL, 0, 0);

        load_settings();                 // restore saved prefs BEFORE first capture
        SetTimer(h, IDT_STATS, 250, nullptr);
        start_capture();
        CloseHandle(CreateThread(nullptr, 0, http_server_thread, nullptr, 0, nullptr));
        CloseHandle(CreateThread(nullptr, 0, discover_thread, nullptr, 0, nullptr));
        ui_log(L"[app] LowCast started — zero sender buffering, TCP_NODELAY, LPCM");
        ui_log(L"[app] NOTE: DLNA has an inherent receiver-side buffer (often "
               L"several hundred ms) fixed in the device firmware — it is the "
               L"high-fidelity path, not the low-latency one. For gaming/low "
               L"latency use the AirPlay button.");
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 3 && HIWORD(wp) == BN_CLICKED) {           // rescan
            CloseHandle(CreateThread(nullptr, 0, discover_thread, nullptr, 0, nullptr));
        } else if (id == 4 && HIWORD(wp) == BN_CLICKED) {    // beep test toggle
            bool on = SendMessageW(G.btn_beep, BM_GETCHECK, 0, 0) == BST_CHECKED;
            G.beep_on.store(on);
            ui_log(on ? L"[test] metronome ON — 1 kHz tick every 2 s; latency = flash\x2192sound gap"
                      : L"[test] metronome OFF");
        } else if (id == 1 && HIWORD(wp) == CBN_SELCHANGE) { // capture device
            int sel = (int)SendMessageW(G.cmb_dev, CB_GETCURSEL, 0, 0);
            G.capture_dev.store(sel - 1);                    // 0 = default -> -1
            ui_log(L"[audio] switching capture device...");
            start_capture();
        } else if (id == 8 && HIWORD(wp) == CBN_SELCHANGE) { // AirPlay buffer
            int sel = (int)SendMessageW(G.cmb_aplat, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < 7) G.ap_latency_ms.store(g_ap_lats[sel]);
            ui_log(L"[raop] buffer changed \x2014 takes effect on next AirPlay START");
        } else if (id == 7 && HIWORD(wp) == BN_CLICKED) {   // local beep toggle
            bool on = SendMessageW(G.btn_localbeep, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool was = G.beep_local.exchange(on);
            if (on && !was)
                CloseHandle(CreateThread(nullptr, 0, local_beep_thread, nullptr, 0, nullptr));
            if (!on) ui_log(L"[test] local beep off");
        } else if (id == 6 && HIWORD(wp) == CBN_SELCHANGE) { // upsample rate
            int sel = (int)SendMessageW(G.cmb_rate, CB_GETCURSEL, 0, 0);
            G.upmult.store(sel == 2 ? 4 : sel == 1 ? 2 : 1);
            kill_all_clients();
            ui_log(L"[audio] rate changed \x2014 active streams closed; press START again");
            start_capture();
        } else if (id == 2 && HIWORD(wp) == CBN_SELCHANGE) { // format
            int sel = (int)SendMessageW(G.cmb_fmt, CB_GETCURSEL, 0, 0);
            G.fmt.store(sel);
            kill_all_clients();                           // old-format streams end
            ui_log(L"[audio] format changed — active streams closed; press START again");
        } else if (id >= IDC_RENDER_BASE + 1000 && HIWORD(wp) == BN_CLICKED) {
            int idx = id - (IDC_RENDER_BASE + 1000);
            EnterCriticalSection(&G.cs);
            bool valid = idx < (int)g_airplay.size();
            bool was = valid ? g_airplay[idx].streaming : false;
            std::string host = valid ? g_airplay[idx].host : "";
            int port = valid ? g_airplay[idx].port : 0;
            if (valid) g_airplay[idx].streaming = !was;
            LeaveCriticalSection(&G.cs);
            if (valid) {
                if (was) raop_stop();
                else {
                    stop_dlna_to_host(host);
                    raop_start(host, port, (uint32_t)G.ap_latency_ms.load());
                }
                PostMessageW(G.hwnd, WM_APP_RENDERS, 1, 0);
            }
        } else if (id >= IDC_RENDER_BASE && HIWORD(wp) == BN_CLICKED) {
            int idx = id - IDC_RENDER_BASE;
            EnterCriticalSection(&G.cs);
            bool valid = idx < (int)G.renderers.size();
            bool was = valid ? G.renderers[idx].streaming : false;
            if (valid && G.renderers[idx].button)
                EnableWindow(G.renderers[idx].button, FALSE);
            LeaveCriticalSection(&G.cs);
            if (valid) {
                if (!was && raop_is_running()) {
                    // symmetric: don't start DLNA while AirPlay owns a device
                    std::string ah;
                    EnterCriticalSection(&G.cs);
                    ah = G.renderers[idx].host;
                    LeaveCriticalSection(&G.cs);
                    bool conflict = false;
                    EnterCriticalSection(&G.cs);
                    for (auto& d : g_airplay)
                        if (d.streaming && d.host == ah) conflict = true;
                    LeaveCriticalSection(&G.cs);
                    if (conflict) {
                        ui_log(L"[cast] stopping AirPlay session to this device first");
                        raop_stop();
                        EnterCriticalSection(&G.cs);
                        for (auto& d : g_airplay) if (d.host == ah) d.streaming = false;
                        LeaveCriticalSection(&G.cs);
                        Sleep(600);
                    }
                }
                CastJob* job = new CastJob{ idx, !was };
                CloseHandle(CreateThread(nullptr, 0, cast_thread, job, 0, nullptr));
            }
        }
        return 0;
    }
    case WM_APP_LOG: {
        std::wstring* s = (std::wstring*)lp;
        int len = GetWindowTextLengthW(G.log);
        SendMessageW(G.log, EM_SETSEL, len, len);
        std::wstring line = *s + L"\r\n";
        SendMessageW(G.log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
        delete s;
        return 0;
    }
    case WM_APP_RENDERS:
        if (wp == 2) { // format auto-fallback happened: reflect it in the UI
            SendMessageW(G.cmb_fmt, CB_SETCURSEL, G.fmt.load(), 0);
            ui_log(L"[audio] format switched automatically (renderer compatibility)");
            return 0;
        }
        if (wp == 1) { // just relabel + re-enable
            EnterCriticalSection(&G.cs);
            for (auto& r : G.renderers) {
                if (!r.button) continue;
                std::wstring label = (r.streaming ? L"\x25A0  STOP   —  " : L"\x25B6  START  —  ")
                                     + utf8_to_wide(r.name);
                SetWindowTextW(r.button, label.c_str());
                EnableWindow(r.button, TRUE);
            }
            for (auto& d : g_airplay) {
                if (!d.button) continue;
                bool live = d.streaming && raop_is_running();
                std::wstring label = (live ? L"\x25A0  STOP   \x2014  \x26A1 AirPlay: "
                                           : L"\x25B6  START  \x2014  \x26A1 AirPlay: ")
                                     + utf8_to_wide(d.name);
                SetWindowTextW(d.button, label.c_str());
                EnableWindow(d.button, TRUE);
            }
            LeaveCriticalSection(&G.cs);
        } else {
            layout_renderer_buttons(h);
        }
        return 0;
    case WM_APP_FLASH:
        G.flash_state = true;
        InvalidateRect(G.flash, nullptr, TRUE);
        SetTimer(h, IDT_FLASHOFF, 120, nullptr);
        return 0;
    case WM_TIMER:
        if (wp == IDT_STATS) update_stats();
        else if (wp == IDT_FLASHOFF) {
            KillTimer(h, IDT_FLASHOFF);
            G.flash_state = false;
            InvalidateRect(G.flash, nullptr, TRUE);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == G.flash) {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, G.flash_state ? RGB(0,0,0) : RGB(200,200,200));
            return (LRESULT)(G.flash_state ? g_flash_on : g_flash_off);
        }
        break;
    case WM_VSCROLL:
        if ((HWND)lp == G.sld_apvol) {
            int pos = (int)SendMessageW(G.sld_apvol, TBM_GETPOS, 0, 0);
            int pct = 100 - pos;                  // vertical: top = 100%
            wchar_t vt[16];
            swprintf(vt, 16, L"%d%%", pct);
            SetWindowTextW(G.lbl_apvol, vt);      // moving readout while dragging
            G.ap_start_vol.store(pct);
            // apply on release / arrow / page step; TB_THUMBTRACK is the drag
            // itself (the liveness thread coalesces pushes anyway)
            if (LOWORD(wp) != TB_THUMBTRACK && raop_is_running())
                raop_push_volume(pct);
        }
        return 0;
    case WM_DESTROY:
        save_settings(h);
        raop_stop();                     // graceful TEARDOWN (was: process kill
        stop_capture();                  // left the receiver holding the session)
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ===========================================================================
// RAOP (AirPlay v1 audio) sender — REAL-TIME pipeline.
// RTSP handshake (unencrypted) + ALAC-wrapped PCM over RTP, with the
// timing responder and control-port sync packets receivers require.
// The sync packets let *us* choose the playback latency.
// ===========================================================================

static uint64_t ntp_now() {                       // 64-bit NTP timestamp
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; // 100ns since 1601
    // seconds between 1601 and 1900 epochs
    const uint64_t EPOCH_DELTA = 9435484800ULL;
    uint64_t secs = t / 10000000ULL - EPOCH_DELTA;
    uint32_t frac = (uint32_t)((t % 10000000ULL) * 4294967296ULL / 10000000ULL);
    return (secs << 32) | frac;
}

struct BitWriter {
    std::vector<uint8_t>& out;
    int bitpos = 0;
    BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t val, int nbits) {
        for (int i = nbits - 1; i >= 0; --i) {
            if (bitpos % 8 == 0) out.push_back(0);
            if ((val >> i) & 1) out.back() |= (uint8_t)(0x80 >> (bitpos % 8));
            bitpos++;
        }
    }
};

// wrap 352 stereo 16-bit samples in an uncompressed ("verbatim") ALAC frame —
// the standard trick used by raop_play / node_airtunes / owntone senders.
static void alac_wrap(const int16_t* lr, int frames, std::vector<uint8_t>& out) {
    out.clear();
    BitWriter bw(out);
    bw.put(1, 3);     // channel index: 1 = stereo
    bw.put(0, 4);     // reserved
    bw.put(0, 8);     // reserved
    bw.put(0, 4);     // reserved
    bw.put(0, 1);     // has-size flag: 0
    bw.put(0, 2);     // unused
    bw.put(1, 1);     // is-not-compressed: 1  -> raw big-endian samples follow
    for (int i = 0; i < frames * 2; ++i)
        bw.put((uint16_t)lr[i], 16);
}

// Stable per-machine AirPlay client identity (DACP-ID / Client-Instance).
// Derived from the first physical adapter's MAC: the SAME identity every
// launch and every RTSP reopen, so receivers that remember per-client state
// (like the last volume you set) can recognize us. Random-per-launch IDs
// gave the receiver amnesia about this sender.
static const char* stable_client_id() {
    static char id[24] = {0};
    if (id[0]) return id;
    uint32_t h1 = 2166136261u, h2 = 40389u;
    ULONG sz = 0; DWORD fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_UNICAST;
    GetAdaptersAddresses(AF_INET, fl, nullptr, nullptr, &sz);
    std::vector<uint8_t> buf(sz ? sz : 1);
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    bool got = false;
    if (sz && GetAdaptersAddresses(AF_INET, fl, nullptr, aa, &sz) == NO_ERROR) {
        for (auto* a = aa; a; a = a->Next) {
            if (a->PhysicalAddressLength >= 6 &&
                a->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
                for (ULONG i = 0; i < a->PhysicalAddressLength; ++i) {
                    h1 = (h1 ^ a->PhysicalAddress[i]) * 16777619u;
                    h2 = h2 * 31u + a->PhysicalAddress[i];
                }
                got = true; break;
            }
        }
    }
    if (!got) { h1 = rng32(); h2 = rng32(); }     // headless fallback: per-run
    snprintf(id, sizeof(id), "%08X%08X", h1, h2);
    return id;
}

struct RtspConn {
    SOCKET s = INVALID_SOCKET;
    int cseq = 0;
    bool last_send_failed = false;
    // Discard bytes left over from replies that arrived after a recv timeout,
    // so a stalled round-trip cannot desync request/response pairing.
    void drain() {
        if (s == INVALID_SOCKET) return;
        u_long avail = 0; char tmp[512];
        while (ioctlsocket(s, FIONREAD, &avail) == 0 && avail > 0) {
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
        }
    }
    std::string session, ua = "iTunes/7.6.2 (Windows; N;)", cid;
    std::string uri;
    bool open(const std::string& host, int port) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        DWORD tmo = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
        if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
            addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
            a.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) return false;
        cid = stable_client_id();
        return true;
    }
    // returns full response, or empty on failure; checks RTSP/1.0 200
    std::string request(const char* method, const std::string& ruri,
                        const std::string& extra_hdrs, const std::string& body,
                        bool* ok_out = nullptr) {
        char hdr[1024];
        snprintf(hdr, sizeof(hdr),
                 "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: %s\r\n"
                 "Client-Instance: %s\r\nDACP-ID: %s\r\n%s%s%s"
                 "Content-Length: %zu\r\n\r\n",
                 method, ruri.c_str(), ++cseq, ua.c_str(), cid.c_str(), cid.c_str(),
                 session.empty() ? "" : ("Session: " + session + "\r\n").c_str(),
                 extra_hdrs.c_str(), extra_hdrs.empty() ? "" : "\r\n",
                 body.size());
        std::string req = std::string(hdr) + body;
        last_send_failed = false;
        drain();
        if (!send_all(s, req.data(), (int)req.size())) {
            last_send_failed = true;
            if (ok_out) *ok_out = false; return {};
        }
        std::string resp; char buf[2048];
        long long want = -1;
        while (true) {
            size_t he = resp.find("\r\n\r\n");
            if (he != std::string::npos && want < 0) {
                size_t cl = ifind(resp, "content-length:");
                long long blen = 0;
                if (cl != std::string::npos && cl < he) blen = atoll(resp.c_str() + cl + 15);
                want = (long long)(he + 4) + blen;
            }
            if (want >= 0 && (long long)resp.size() >= want) break;
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, n);
        }
        bool ok = resp.rfind("RTSP/1.0 200", 0) == 0;
        if (g_console_mode) {
            size_t e = resp.find("\r\n");
            std::string status = (e == std::string::npos) ? "(no response)" : resp.substr(0, e);
            ui_log8(std::string("[rtsp] ") + method + " -> " + status);
            if (!ok && !resp.empty()) {
                size_t b = resp.find("\r\n\r\n");
                std::string body = (b == std::string::npos) ? "" : resp.substr(b + 4);
                if (body.size() > 300) body.resize(300);
                if (!body.empty()) ui_log8("[rtsp]   body: " + body);
            }
        }
        if (ok_out) *ok_out = ok;
        return resp;
    }
    void close() { if (s != INVALID_SOCKET) closesocket(s); s = INVALID_SOCKET; }
};

static std::string rtsp_header(const std::string& resp, const char* name) {
    size_t p = ifind(resp, std::string(name) + ":");
    if (p == std::string::npos) return {};
    p += strlen(name) + 1;
    while (p < resp.size() && resp[p] == ' ') ++p;
    size_t e = resp.find("\r\n", p);
    return resp.substr(p, e - p);
}

struct RaopSession {
    std::atomic<bool> run{false};
    std::string host; int rtsp_port = 5000;
    RtspConn rtsp;
    SOCKET audio_sock = INVALID_SOCKET, ctrl_sock = INVALID_SOCKET, tim_sock = INVALID_SOCKET;
    int srv_audio = 0, srv_ctrl = 0, srv_tim = 0;
    uint16_t seq = 0; uint32_t rtptime = 0, ssrc = 0;
    uint32_t latency_frames = 15435;             // ~350 ms @44.1k — OUR choice
    std::atomic<uint64_t> frames_sent{0};
    std::atomic<uint64_t> resends{0};
    std::atomic<uint64_t> last_timing_ms{0};     // receiver heartbeat (0xD2 seen)
    std::atomic<uint32_t> reconnects{0};
    HANDLE thread = nullptr, tim_thread = nullptr, ctrl_thread = nullptr;
    // liveness thread: owns keepalive/probe so the TRANSMIT loop never blocks
    // on RTSP I/O. rtsp_cs guards RtspConn between liveness and reconnect.
    HANDLE ka_thread = nullptr;
    std::atomic<bool> ka_run{false};    // liveness thread lifecycle
    std::atomic<bool> ka_dead{false};   // verdict: session lost, please reconnect
    std::atomic<bool> ka_rearm{false};  // reconnected: reset liveness timers
    std::atomic<int>  vol_push{-2};     // UI-queued live volume (%); -2 = none
    CRITICAL_SECTION rtsp_cs;
    // retransmit history: last 1024 RTP packets (~8 s), guarded by hist_cs
    CRITICAL_SECTION hist_cs;
    std::vector<uint8_t> hist[1024];
};
static RaopSession RA;

// Tag a UDP flow as voice traffic (DSCP EF) so WiFi WMM gives it airtime
// priority. Dynamic-loaded; silently skipped where qWave is unavailable.
static void qos_tag_voice(SOCKET s, const sockaddr_in& dst) {
    // minimal qWave declarations (mingw's qos2.h is broken)
    struct QOS_VERSION { USHORT MajorVersion, MinorVersion; };
    typedef UINT32 QOS_FLOWID;
    enum { QOSTrafficTypeVoice_ = 6, QOS_NON_ADAPTIVE_FLOW_ = 0x00000002 };
    typedef BOOL (WINAPI *PQOSCreateHandle)(QOS_VERSION*, PHANDLE);
    typedef BOOL (WINAPI *PQOSAddSocketToFlow)(HANDLE, SOCKET, sockaddr*,
                                               int, DWORD, QOS_FLOWID*);
    static HANDLE qh = nullptr;
    static PQOSAddSocketToFlow pAdd = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"qwave.dll");
        if (m) {
            PQOSCreateHandle pCreate = (PQOSCreateHandle)GetProcAddress(m, "QOSCreateHandle");
            pAdd = (PQOSAddSocketToFlow)GetProcAddress(m, "QOSAddSocketToFlow");
            QOS_VERSION v{ 1, 0 };
            if (!pCreate || !pCreate(&v, &qh)) { qh = nullptr; pAdd = nullptr; }
        }
        ui_log(qh ? L"[net] QoS voice tagging active (DSCP EF / WMM priority)"
                  : L"[net] QoS tagging unavailable — continuing untagged");
    }
    if (qh && pAdd) {
        QOS_FLOWID fid = 0;
        if (!pAdd(qh, s, (sockaddr*)&dst, QOSTrafficTypeVoice_, QOS_NON_ADAPTIVE_FLOW_, &fid)) {
            // connected sockets (the DLNA TCP stream) want a NULL dest addr
            fid = 0;
            pAdd(qh, s, nullptr, QOSTrafficTypeVoice_, QOS_NON_ADAPTIVE_FLOW_, &fid);
        }
    }
}

static int bind_udp(SOCKET& s) {
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = 0;
    bind(s, (sockaddr*)&a, sizeof(a));
    sockaddr_in me{}; int ml = sizeof(me);
    getsockname(s, (sockaddr*)&me, &ml);
    return ntohs(me.sin_port);
}

static DWORD WINAPI raop_timing_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // answer NTP timing requests (type 0xD2) with 0xD3 replies
    char buf[64];
    DWORD tmo = 500;
    setsockopt(RA.tim_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    while (RA.run.load()) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(RA.tim_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n < 32) continue;
        if ((uint8_t)buf[1] != 0xD2) continue;
        {
            uint64_t prev = RA.last_timing_ms.exchange(now_ms());
            if (prev == 0)
                ui_log(L"[raop] receiver engaged (timing heartbeat started)");
            else if (now_ms() - prev > 5000) {
                wchar_t w[80];
                swprintf(w, 80, L"[raop] timing gap: %lu ms (receiver polls slowly or dozed)",
                         (unsigned long)(now_ms() - prev));
                ui_log(w);
            }
        }
        uint8_t rep[32] = {0};
        rep[0] = 0x80; rep[1] = 0xD3; rep[2] = 0x00; rep[3] = 0x07;
        memcpy(rep + 8, buf + 24, 8);            // originate = their transmit
        uint64_t now = ntp_now();
        for (int i = 0; i < 8; ++i) {            // receive time (big-endian)
            rep[16 + i] = (uint8_t)(now >> (56 - 8 * i));
            rep[24 + i] = (uint8_t)(now >> (56 - 8 * i));   // transmit time
        }
        sendto(RA.tim_sock, (const char*)rep, 32, 0, (sockaddr*)&from, fl);
    }
    return 0;
}

static DWORD WINAPI raop_control_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    char buf[64];
    DWORD tmo = 500;
    setsockopt(RA.ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons((u_short)RA.srv_ctrl);
    inet_pton(AF_INET, RA.host.c_str(), &dst.sin_addr);
    while (RA.run.load()) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(RA.ctrl_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n < 8) continue;
        if ((uint8_t)buf[1] != 0xD5) continue;   // resend request
        uint16_t first = ((uint8_t)buf[4] << 8) | (uint8_t)buf[5];
        uint16_t count = ((uint8_t)buf[6] << 8) | (uint8_t)buf[7];
        if (count > 128) count = 128;
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t want = (uint16_t)(first + i);
            std::vector<uint8_t> pkt;
            EnterCriticalSection(&RA.hist_cs);
            std::vector<uint8_t>& h = RA.hist[want & 1023];
            if (h.size() > 4 &&
                ((h[2] << 8) | h[3]) == want)     // seq matches: still in history
                pkt = h;
            LeaveCriticalSection(&RA.hist_cs);
            if (pkt.empty()) continue;
            std::vector<uint8_t> re;
            re.reserve(pkt.size() + 4);
            re.push_back(0x80); re.push_back(0xD6);
            re.push_back((uint8_t)(want >> 8)); re.push_back((uint8_t)want);
            re.insert(re.end(), pkt.begin(), pkt.end());
            sendto(RA.ctrl_sock, (const char*)re.data(), (int)re.size(), 0,
                   (sockaddr*)&dst, sizeof(dst));
            RA.resends.fetch_add(1);
        }
    }
    return 0;
}

static void raop_send_sync(bool first) {
    uint8_t p[20];
    p[0] = first ? 0x90 : 0x80;
    p[1] = 0xD4; p[2] = 0x00; p[3] = 0x07;
    uint32_t now_ts = RA.rtptime;                 // frame that "should play now"...
    uint32_t now_minus_lat = now_ts - RA.latency_frames;
    for (int i = 0; i < 4; ++i) p[4 + i] = (uint8_t)(now_minus_lat >> (24 - 8 * i));
    uint64_t ntp = ntp_now();
    for (int i = 0; i < 8; ++i) p[8 + i] = (uint8_t)(ntp >> (56 - 8 * i));
    for (int i = 0; i < 4; ++i) p[16 + i] = (uint8_t)(now_ts >> (24 - 8 * i));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)RA.srv_ctrl);
    inet_pton(AF_INET, RA.host.c_str(), &a.sin_addr);
    sendto(RA.ctrl_sock, (const char*)p, 20, 0, (sockaddr*)&a, sizeof(a));
}

// float-frame pipe from the capture thread to the RAOP sender
static CRITICAL_SECTION g_raop_cs;
static std::deque<float> g_raop_pipe;             // interleaved L,R at native rate
static std::atomic<bool> g_raop_pipe_on{false};

static void raop_pipe_push(float l, float r) {
    if (!g_raop_pipe_on.load()) return;
    EnterCriticalSection(&g_raop_cs);
    if (g_raop_pipe.size() < 48000 * 2 * 4) {     // cap ~4 s
        g_raop_pipe.push_back(l);
        g_raop_pipe.push_back(r);
    }
    LeaveCriticalSection(&g_raop_cs);
}

static bool raop_handshake() {
    RtspConn& c = RA.rtsp;
    c = RtspConn();
    if (!c.open(RA.host, RA.rtsp_port)) { ui_log(L"[raop] RTSP connect failed"); return false; }
    std::string my_ip = local_ip_for(RA.host);
    char sid[32]; snprintf(sid, sizeof(sid), "%u", rng32());
    c.uri = "rtsp://" + my_ip + "/" + sid;

    bool ok = false;
    c.request("OPTIONS", "*", "", "", &ok);
    if (!ok) ui_log(L"[raop] OPTIONS not 200 (continuing)");

    char sdp[512];
    snprintf(sdp, sizeof(sdp),
        "v=0\r\no=iTunes %s 0 IN IP4 %s\r\ns=iTunes\r\nc=IN IP4 %s\r\nt=0 0\r\n"
        "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n",
        sid, my_ip.c_str(), RA.host.c_str());
    c.request("ANNOUNCE", c.uri, "Content-Type: application/sdp", sdp, &ok);
    if (!ok) { ui_log(L"[raop] ANNOUNCE rejected (receiver may require encryption)"); return false; }

    int my_ctrl = bind_udp(RA.ctrl_sock);
    int my_tim  = bind_udp(RA.tim_sock);
    bind_udp(RA.audio_sock);
    DWORD tmo = 500;
    setsockopt(RA.ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(RA.tim_sock,  SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    char thdr[256];
    snprintf(thdr, sizeof(thdr),
             "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
             "control_port=%d;timing_port=%d", my_ctrl, my_tim);
    std::string resp = c.request("SETUP", c.uri, thdr, "", &ok);
    if (!ok) { ui_log(L"[raop] SETUP rejected"); return false; }
    c.session = rtsp_header(resp, "Session");
    std::string tr = rtsp_header(resp, "Transport");
    auto tval = [&](const char* k) -> int {
        size_t p = ifind(tr, std::string(k) + "=");
        return (p == std::string::npos) ? 0 : atoi(tr.c_str() + p + strlen(k) + 1);
    };
    RA.srv_audio = tval("server_port");
    RA.srv_ctrl  = tval("control_port");
    RA.srv_tim   = tval("timing_port");
    if (!RA.srv_audio) { ui_log(L"[raop] no server_port in SETUP reply"); return false; }
    ui_log8("[raop] SETUP ok — audio:" + std::to_string(RA.srv_audio) +
            " ctrl:" + std::to_string(RA.srv_ctrl) + " session:" + c.session);

    if (!RA.tim_thread)
        RA.tim_thread = CreateThread(nullptr, 0, raop_timing_thread, nullptr, 0, nullptr);
    if (!RA.ctrl_thread)
        RA.ctrl_thread = CreateThread(nullptr, 0, raop_control_thread, nullptr, 0, nullptr);

    RA.seq = (uint16_t)rng32();
    RA.rtptime = (uint32_t)rng32();
    RA.ssrc = rng32();
    char rihdr[128];
    snprintf(rihdr, sizeof(rihdr), "Range: npt=0-\r\nRTP-Info: seq=%u;rtptime=%u",
             RA.seq, RA.rtptime);
    resp = c.request("RECORD", c.uri, rihdr, "", &ok);
    if (!ok) { ui_log(L"[raop] RECORD rejected"); return false; }
    std::string al = rtsp_header(resp, "Audio-Latency");
    ui_log8("[raop] RECORD ok — receiver Audio-Latency: " + (al.empty() ? "?" : al) +
            " frames; using sender latency " + std::to_string(RA.latency_frames) +
            " (" + std::to_string(RA.latency_frames * 1000 / 44100) + " ms)");
    long adv = al.empty() ? 0 : atol(al.c_str());
    if (adv > 0 && (uint32_t)adv > RA.latency_frames) {
        wchar_t w[200];
        swprintf(w, 200, L"[raop] WARNING: requested buffer (%u ms) is below the "
                 L"receiver's advertised minimum (%ld ms). Some firmwares play "
                 L"SILENCE instead of clamping — if you hear nothing, increase the "
                 L"AirPlay latency setting.", RA.latency_frames * 1000 / 44100, adv * 1000 / 44100);
        ui_log(w);
    }
    // Start volume: -1 = never touch it (receiver's mixer / your headphone
    // buttons own it). Otherwise send ONE RAOP volume command mapped onto
    // the standard -30..0 dB range \x2014 the receiver honors these (the old
    // forced-100% bug proved that), and it is sent exactly once per
    // session start or reconnect, never again.
    {
        int vp = G.ap_start_vol.load();
        if (vp >= 0) {
            raop_send_volume_pct(vp);
        } else {
            ui_log(L"[raop] volume is under receiver control (headphone buttons)");
        }
    }
    raop_send_sync(true);
    return true;
}

// ---------------------------------------------------------------------------
// SINC_RS_BEGIN
// Windowed-sinc polyphase resampler (Kaiser, 96 taps, 512 phases with linear
// phase blending). Replaces the 2-point linear interpolator, whose aliasing
// floor measured -22 dBc at 10 kHz with -4.7 dB rolloff at 19 kHz on the
// 48k->44.1k path. This kernel: passband flat to ~19.8 kHz, alias/image
// rejection ~-73 dB. CPU cost ~17M MAC/s -- negligible.
// Also: TPDF dither at every float->16-bit quantization (RAOP + LPCM16),
// replacing plain rounding (undithered truncation distortion).
// ---------------------------------------------------------------------------
static inline float tpdf_dither(uint32_t& st) {
    st = st * 1664525u + 1013904223u; float a = (float)(st >> 8) * (1.0f / 16777216.0f);
    st = st * 1664525u + 1013904223u; float b = (float)(st >> 8) * (1.0f / 16777216.0f);
    return a - b;                                  // triangular, +/-1 LSB
}
static inline int16_t quant16(float v, uint32_t& seed) {
    long q = lrintf(v * 32767.f + tpdf_dither(seed));
    if (q > 32767) q = 32767; else if (q < -32768) q = -32768;
    return (int16_t)q;
}
struct SincResampler {
    enum { TAPS = 96, HALF = TAPS / 2, PHASES = 512 };
    std::vector<float> tab;        // (PHASES+1) rows x TAPS
    std::vector<float> buf;        // interleaved L,R input frames (history + pending)
    double rpos = 0.0;             // fractional read position, in frames
    static double bessel_i0(double x) {
        double s = 1.0, t = 1.0;
        for (int k = 1; k < 32; ++k) { t *= (x * x) / (4.0 * k * k); s += t; if (t < 1e-12 * s) break; }
        return s;
    }
    void design(double ratio) {    // ratio = out_rate / in_rate
        double cut = (ratio < 0.999) ? 0.90 * ratio : 0.97;   // fraction of INPUT Nyquist
        const double beta = 9.0, i0b = bessel_i0(beta);
        tab.assign((size_t)(PHASES + 1) * TAPS, 0.f);
        for (int p = 0; p <= PHASES; ++p) {
            double frac = (double)p / PHASES, sum = 0.0, w[TAPS];
            for (int t = 0; t < TAPS; ++t) {
                double x = (double)(t - (HALF - 1)) - frac;          // offset from center
                double sx = cut * x * 3.14159265358979323846;
                double snc = (fabs(sx) < 1e-9) ? 1.0 : sin(sx) / sx;
                double u = x / HALF; if (u < -1) u = -1; if (u > 1) u = 1;
                double win = bessel_i0(beta * sqrt(1.0 - u * u)) / i0b;
                w[t] = cut * snc * win; sum += w[t];
            }
            for (int t = 0; t < TAPS; ++t)
                tab[(size_t)p * TAPS + t] = (float)(w[t] / sum);      // unity DC gain
        }
        buf.assign((size_t)(HALF - 1) * 2, 0.f);   // zero history so taps never underrun
        rpos = HALF - 1;
    }
    void feed(const float* lr, size_t frames) { buf.insert(buf.end(), lr, lr + frames * 2); }
    int    frames_total() const { return (int)(buf.size() / 2); }
    double frames_ahead() const { return (double)frames_total() - rpos; }
    bool   can_pull()     const { return (int)rpos + HALF < frames_total(); }
    void pull(float& l, float& r, double step) {
        int    i0   = (int)rpos - (HALF - 1);
        double frac = rpos - (int)rpos;
        double fp   = frac * PHASES;
        int    p    = (int)fp;
        float  pf   = (float)(fp - p);
        const float* w0 = &tab[(size_t)p * TAPS];
        const float* w1 = w0 + TAPS;
        const float* s  = &buf[(size_t)i0 * 2];
        float al = 0.f, ar = 0.f;
        for (int t = 0; t < TAPS; ++t) {
            float w = w0[t] + (w1[t] - w0[t]) * pf;
            al += w * s[t * 2]; ar += w * s[t * 2 + 1];
        }
        l = al; r = ar; rpos += step;
    }
    void skip(double frames) { rpos += frames; }   // backlog trim: jump forward in time
    void compact() {                               // drop history no tap can reach
        int drop = (int)rpos - (HALF - 1);
        if (drop <= 0) return;
        buf.erase(buf.begin(), buf.begin() + (size_t)drop * 2);
        rpos -= drop;
    }
};
// SINC_RS_END

// ---------------------------------------------------------------------------
// Liveness thread: keepalive + probe run HERE so their blocking I/O (an RTSP
// round-trip, and a 700 ms retry pause when one fails) can never stall the
// transmit loop. Previously these ran inline in the stream thread — one
// hiccuped keepalive meant a guaranteed >=700 ms hole in the audio.
// Policy is IDENTICAL to the old inline version:
//   - RTSP keepalive every 20 s, one retry 700 ms later
//   - keepalive verdict trusted only when the UDP timing heartbeat agrees
//   - stale heartbeat (>8 s) triggers an RTSP probe (>=10 s apart);
//     only a failed probe (with retry) declares the session dead
// The verdict is posted via RA.ka_dead; the stream thread reconnects.
// ---------------------------------------------------------------------------
static bool raop_ka_request() {              // one guarded keepalive round-trip
    // OPTIONS is the canonical side-effect-free RTSP ping. The previous
    // keepalive was SET_PARAMETER volume -0.0 — literally "set volume to
    // maximum" every 20 s, which fought the receiver's own volume buttons
    // (each ping snapped the mixer back to 100%).
    bool alive = false;
    EnterCriticalSection(&RA.rtsp_cs);
    RA.rtsp.request("OPTIONS", "*", "", "", &alive);
    LeaveCriticalSection(&RA.rtsp_cs);
    return alive;
}

// Send one RAOP volume command (percent mapped linearly onto -30..0 dB).
// Takes rtsp_cs itself; Windows critical sections are recursive, so calling
// this from handshake (which already holds the lock) is safe.
static void raop_send_volume_pct(int vp) {
    double db = -30.0 * (double)(100 - vp) / 100.0;
    char vb[48]; snprintf(vb, sizeof(vb), "volume: %.2f\r\n", db);
    bool vok = false;
    EnterCriticalSection(&RA.rtsp_cs);
    RA.rtsp.request("SET_PARAMETER", RA.rtsp.uri, "Content-Type: text/parameters", vb, &vok);
    LeaveCriticalSection(&RA.rtsp_cs);
    wchar_t lb[80];
    swprintf(lb, 80, L"[raop] volume set to %d%% (%.1f dB)", vp, db);
    ui_log(lb);
}

static void raop_push_volume(int pct) { RA.vol_push.store(pct); }

static DWORD WINAPI raop_liveness_thread(LPVOID) {
    uint64_t last_ka = now_ms(), last_probe = 0;
    while (RA.ka_run.load()) {
        Sleep(250);
        if (!RA.ka_run.load()) break;
        if (RA.ka_rearm.exchange(false)) { last_ka = now_ms(); last_probe = 0; }
        if (RA.ka_dead.load()) continue;     // verdict pending; reconnect owns RTSP
        { int vpush = RA.vol_push.exchange(-2);   // UI-queued live volume change
          if (vpush >= 0) raop_send_volume_pct(vpush); }
        uint64_t nowm = now_ms();
        uint64_t lt = RA.last_timing_ms.load();
        bool hb_stale = lt && nowm - lt > 8000;
        bool do_ka = nowm - last_ka >= 20000;
        bool do_probe = !do_ka && hb_stale && nowm - last_probe >= 10000;
        if (!do_ka && !do_probe) continue;
        if (do_ka) last_ka = nowm; else last_probe = nowm;

        bool alive = raop_ka_request();
        if (!alive) {                        // one benefit-of-the-doubt retry
            Sleep(700);                      // harmless here: audio keeps flowing
            if (!RA.ka_run.load()) break;
            alive = raop_ka_request();
        }
        if (alive) {
            if (do_probe) RA.last_timing_ms.store(now_ms());  // proof of life; rearm
            continue;
        }
        if (do_probe) { RA.ka_dead.store(true); continue; }   // stale hb + dead RTSP

        // Keepalive failed: the RTSP verdict is only trusted when the UDP side
        // agrees. WiFi receivers (headphones especially) doze for seconds at a
        // time; a stalled TCP round-trip while timing heartbeats still flow
        // means the RTSP channel hiccupped, not the session.
        uint64_t lt2 = RA.last_timing_ms.load();
        bool udp_fresh = lt2 && now_ms() - lt2 < 8000;
        if (!udp_fresh) { RA.ka_dead.store(true); continue; }
        EnterCriticalSection(&RA.rtsp_cs);
        if (RA.rtsp.last_send_failed) {
            // TCP is already gone; nothing left to preserve. Shairport
            // receivers tie the session to this connection, so it is most
            // likely ending anyway; have a channel ready.
            ui_log(L"[raop] keepalive send failed; reopening RTSP channel");
            RA.rtsp.close();
            if (!RA.rtsp.open(RA.host, RA.rtsp_port))
                ui_log(L"[raop] RTSP reopen failed; will retry next cycle");
        } else {
            // Reply stalled but the receiver is demonstrably alive. Shairport
            // ties the session to THIS connection, so keep the socket open:
            // closing it here would end the very session we are trying to
            // protect. Only a stale heartbeat can end the session now.
            ui_log(L"[raop] keepalive reply stalled (heartbeat fresh) \x2014 keeping session");
        }
        LeaveCriticalSection(&RA.rtsp_cs);
    }
    return 0;
}

static DWORD WINAPI raop_stream_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    EnterCriticalSection(&RA.rtsp_cs);           // a zombie liveness thread from a
    bool hs0 = raop_handshake();                 // prior session must not overlap
    LeaveCriticalSection(&RA.rtsp_cs);
    if (!hs0) { RA.run = false; return 1; }
    RtspConn& c = RA.rtsp;
    RA.ka_dead.store(false); RA.ka_rearm.store(false);
    RA.vol_push.store(-2);
    RA.ka_run.store(true);
    RA.ka_thread = CreateThread(nullptr, 0, raop_liveness_thread, nullptr, 0, nullptr);

    // CLOCK-SLAVED streaming loop: a packet is transmitted the moment one
    // packet's worth of source audio exists. No wall-clock pacing, no cushion —
    // the transmit rate is a direct function of the capture clock, so
    // producer/consumer drift cannot exist by construction. Sync packets
    // (generated from the live rtptime + NTP each second) keep the receiver
    // anchored to real time.
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons((u_short)RA.srv_audio);
    inet_pton(AF_INET, RA.host.c_str(), &dst.sin_addr);
    qos_tag_voice(RA.audio_sock, dst);
    {
        sockaddr_in cdst = dst; cdst.sin_port = htons((u_short)RA.srv_ctrl);
        qos_tag_voice(RA.ctrl_sock, cdst);
    }

    const int FR = 352;
    int16_t frame_buf[FR * 2];
    std::vector<uint8_t> alac, pkt;
    SincResampler rs;
    uint32_t rs_rate = G.native_rate.load();      // rate the kernel was designed for
    rs.design(44100.0 / (double)rs_rate);
    uint32_t dseed = 0x9E3779B9u;
    bool first_pkt = true;
    uint64_t sent_frames = 0, last_sync = now_ms();
    bool was_streaming = false, discontinuity = false;
    uint64_t resync_burst_until = 0;
    uint64_t starve_since = 0;                   // gate: real gap vs momentary poll
    // [diag] servo health counters, reported every 10 s (read-only telemetry)
    uint64_t rg_next = now_ms() + 10000;
    uint64_t rg_starve_ev = 0, rg_starve_max = 0, rg_trim_ev = 0, rg_trim_ms = 0, rg_resync = 0;
    double rg_dmin = 1e18, rg_dmax = 0.0;
    // Transmit-timeline PLL: locks the OUTPUT frame rate to exactly 44100 per
    // wall-second. measured_rate is only the seed; the servo nulls its noise.
    g_raop_pipe_on.store(true);

    while (RA.run.load()) {
        // CLOCK-SLAVED transmission: a packet leaves the moment one packet's
        // worth of source audio exists. No wall-clock pacing and no cushion —
        // the transmit rate is a direct function of the capture clock, so
        // producer/consumer drift cannot exist by construction. (The capture
        // thread's wall-clock silence-fill guarantees the pipe advances at
        // realtime even when the system is silent, so cadence is preserved.)
        // ACCURACY: smoothed measured clock ratio as feedforward base.
        // BOUNDEDNESS: proportional depth servo (±400 ppm) pins the pipe at
        // ~2 ms above one packet — depth IS the added latency, so depth is the
        // quantity regulated. (Replaces a rate-locking PLL which left depth
        // neutrally stable and free to random-walk.)
        double base = G.measured_rate.load() / 44100.0;
        uint32_t nrate = G.native_rate.load();
        if (nrate != rs_rate) {
            // Device switch changed the input rate. The servo keeps pitch
            // correct regardless, but the anti-alias cutoff was designed for
            // the OLD ratio (48k->96k would alias 22-40 kHz content into the
            // audible band). Rebuild the kernel; treat as a discontinuity.
            rs.design(44100.0 / (double)nrate);
            rs_rate = nrate;
            if (was_streaming) discontinuity = true;   // never CLEAR a pending one
        }
        // move every pending capture frame into the resampler's window
        EnterCriticalSection(&g_raop_cs);
        if (!g_raop_pipe.empty()) {
            std::vector<float> tmp(g_raop_pipe.begin(), g_raop_pipe.end());
            g_raop_pipe.clear();
            LeaveCriticalSection(&g_raop_cs);
            rs.feed(tmp.data(), tmp.size() / 2);
        } else LeaveCriticalSection(&g_raop_cs);

        int need_min = (int)(FR * base) + SincResampler::HALF + 4;
        int tgt = need_min + (int)(nrate * 2 / 1000);
        double depth = rs.frames_ahead();
        { double dms = depth * 1000.0 / nrate;        // [diag] depth range pre-trim
          if (dms < rg_dmin) rg_dmin = dms;
          if (dms > rg_dmax) rg_dmax = dms; }
        if (depth > nrate * 120 / 1000) {             // stall backlog: hard trim
            wchar_t tb[128];                          // [diag] every trim is audible
            swprintf(tb, 128, L"[diag] TRIM: pipe %.0fms -> %.0fms (audio skipped)",
                     depth * 1000.0 / nrate, (double)tgt * 1000.0 / nrate);
            ui_log(tb);
            rg_trim_ev++;
            rg_trim_ms += (uint64_t)((depth - tgt) * 1000.0 / nrate);
            rs.skip(depth - tgt);                     // jump forward in time
            rs.compact();
            depth = rs.frames_ahead();
        }
        if (now_ms() >= rg_next) {                    // [diag] 10 s servo report
            double meas = G.measured_rate.load();
            int ppm = (int)((meas / (double)nrate - 1.0) * 1e6);
            wchar_t rb[192];
            swprintf(rb, 192,
                L"[diag] raop: depth %.1f-%.1fms meas %+dppm starve=%llu(max %llums) trim=%llu(%llums) resync=%llu",
                rg_dmin >= 1e17 ? 0.0 : rg_dmin, rg_dmax, ppm,
                (unsigned long long)rg_starve_ev, (unsigned long long)rg_starve_max,
                (unsigned long long)rg_trim_ev, (unsigned long long)rg_trim_ms,
                (unsigned long long)rg_resync);
            ui_log(rb);
            rg_starve_ev = rg_starve_max = rg_trim_ev = rg_trim_ms = rg_resync = 0;
            rg_dmin = 1e18; rg_dmax = 0.0;
            rg_next += 10000;
        }
        double adj = (depth - tgt) / (double)nrate * 0.04;
        if (adj > 0.0004) adj = 0.0004; else if (adj < -0.0004) adj = -0.0004;
        double step = base * (1.0 + adj);

        bool have = depth >= FR * step + SincResampler::HALF + 2;
        if (!have) {                              // audio clock hasn't ticked yet
            // Only a SUSTAINED starvation (>150 ms) is a genuine discontinuity
            // (capture restart). Momentary empties between capture deliveries
            // happen every packet cycle and must NOT trigger re-anchor storms.
            uint64_t nowg = now_ms();
            if (starve_since == 0) starve_since = nowg;
            if (was_streaming && nowg - starve_since > 150) {
                discontinuity = true; was_streaming = false;
                rg_starve_ev++;                       // [diag] sustained starvation
                ui_log(L"[diag] STARVE: pipe empty >150ms \x2014 resync scheduled");
            }
            if (nowg - last_sync >= 500) { raop_send_sync(false); last_sync = nowg; }
            Sleep(1);
            continue;
        }
        if (starve_since) {                       // [diag] longest momentary gap
            uint64_t sd = now_ms() - starve_since;
            if (sd > rg_starve_max) rg_starve_max = sd;
        }
        starve_since = 0;
        was_streaming = true;
        for (int i = 0; i < FR; ++i) {
            float l, r;
            rs.pull(l, r, step);                  // 96-tap windowed-sinc kernel
            frame_buf[i * 2]     = quant16(l, dseed);   // TPDF-dithered
            frame_buf[i * 2 + 1] = quant16(r, dseed);
        }
        rs.compact();

                bool mark = first_pkt || discontinuity;
        if (discontinuity) {
            rg_resync++;                              // [diag]
            // re-assert timing exactly at the resume point, then burst-sync for 300 ms
            raop_send_sync(true);
            last_sync = now_ms();
            resync_burst_until = now_ms() + 300;
            discontinuity = false;
        }
        alac_wrap(frame_buf, FR, alac);
        pkt.clear();
        pkt.push_back(0x80);
        pkt.push_back(mark ? 0xE0 : 0x60);       // marker bit on resync/first
        pkt.push_back((uint8_t)(RA.seq >> 8)); pkt.push_back((uint8_t)RA.seq);
        for (int i = 0; i < 4; ++i) pkt.push_back((uint8_t)(RA.rtptime >> (24 - 8 * i)));
        for (int i = 0; i < 4; ++i) pkt.push_back((uint8_t)(RA.ssrc >> (24 - 8 * i)));
        pkt.insert(pkt.end(), alac.begin(), alac.end());
        sendto(RA.audio_sock, (const char*)pkt.data(), (int)pkt.size(), 0,
               (sockaddr*)&dst, sizeof(dst));
        EnterCriticalSection(&RA.hist_cs);
        RA.hist[RA.seq & 1023] = pkt;
        LeaveCriticalSection(&RA.hist_cs);
        first_pkt = false;
        RA.seq++;
        RA.rtptime += FR;
        sent_frames += FR;
        RA.frames_sent.store(sent_frames);

        uint64_t sync_interval = (now_ms() < resync_burst_until) ? 50 : 1000;
        if (now_ms() - last_sync >= sync_interval) { raop_send_sync(false); last_sync = now_ms(); }

        // Liveness verdicts come from the dedicated liveness thread (keepalive
        // every 20 s + heartbeat-stale probes); this loop never blocks on RTSP.
        bool session_dead = RA.ka_dead.load();

        if (session_dead) {
            // PERSISTENT reconnect: a session ends when the user says so, not
            // when the network hiccups. Retry until STOP is pressed.
            // rtsp_cs held across each attempt: the liveness thread must not
            // touch the RTSP connection while it is being rebuilt.
            ui_log(L"[raop] session lost \x2014 reconnecting until it returns...");
            int attempt = 0;
            bool back = false;
            while (RA.run.load()) {
                EnterCriticalSection(&RA.rtsp_cs);
                c.close();
                closesocket(RA.ctrl_sock); closesocket(RA.tim_sock); closesocket(RA.audio_sock);
                LeaveCriticalSection(&RA.rtsp_cs);
                Sleep(attempt == 0 ? 400 : 2500);
                attempt++;
                EnterCriticalSection(&RA.rtsp_cs);
                bool hs = raop_handshake();
                LeaveCriticalSection(&RA.rtsp_cs);
                if (hs) { back = true; break; }
                if (attempt == 1 || attempt % 6 == 0) {
                    wchar_t b[96];
                    swprintf(b, 96, L"[raop] still trying (attempt %d)...", attempt);
                    ui_log(b);
                }
            }
            if (back) {
                dst.sin_port = htons((u_short)RA.srv_audio);
                inet_pton(AF_INET, RA.host.c_str(), &dst.sin_addr);
                first_pkt = true;
                sent_frames = 0;
                RA.last_timing_ms.store(0);
                RA.reconnects.fetch_add(1);
                RA.ka_dead.store(false);       // verdict consumed
                RA.ka_rearm.store(true);       // liveness timers restart fresh
                RA.vol_push.store(-2);         // handshake already re-sent volume
                ui_log(L"[raop] reconnected \x2014 stream resumed");
            }
        }
    }

    g_raop_pipe_on.store(false);
    RA.ka_run.store(false);                      // liveness thread down first:
    if (RA.ka_thread) {                          // TEARDOWN must own the conn
        if (WaitForSingleObject(RA.ka_thread, 3000) != WAIT_OBJECT_0) {
            shutdown(RA.rtsp.s, SD_BOTH);        // abort an in-flight keepalive recv
            WaitForSingleObject(RA.ka_thread, 8000);
        }
        CloseHandle(RA.ka_thread); RA.ka_thread = nullptr;
    }
    bool tok = false;
    EnterCriticalSection(&RA.rtsp_cs);
    c.request("TEARDOWN", c.uri, "", "", &tok);
    LeaveCriticalSection(&RA.rtsp_cs);
    c.close();
    if (RA.tim_thread) { WaitForSingleObject(RA.tim_thread, 2000); CloseHandle(RA.tim_thread); RA.tim_thread = nullptr; }
    if (RA.ctrl_thread) { WaitForSingleObject(RA.ctrl_thread, 2000); CloseHandle(RA.ctrl_thread); RA.ctrl_thread = nullptr; }
    for (auto& hh : RA.hist) hh.clear();
    closesocket(RA.audio_sock); closesocket(RA.ctrl_sock); closesocket(RA.tim_sock);
    ui_log(L"[raop] session ended");
    return 0;
}

static void raop_start(const std::string& host, int port, uint32_t latency_ms) {
    if (RA.run.load()) return;
    // A rapid STOP->START can land here while the old stream thread is still
    // tearing down (TEARDOWN + two joins can exceed raop_stop's 3 s wait).
    // Two threads sharing RA would race on the RTSP conn and sockets, so the
    // old one must be fully gone before a new session touches RA.
    if (RA.thread) {
        if (WaitForSingleObject(RA.thread, 15000) != WAIT_OBJECT_0) {
            ui_log(L"[raop] previous session is still closing \x2014 try again in a moment");
            return;
        }
        CloseHandle(RA.thread); RA.thread = nullptr;
    }
    RA.host = host; RA.rtsp_port = port;
    RA.latency_frames = latency_ms * 44100 / 1000;
    RA.run.store(true);
    RA.thread = CreateThread(nullptr, 0, raop_stream_thread, nullptr, 0, nullptr);
    ui_log8("[raop] starting AirPlay session to " + host + ":" + std::to_string(port));
}
static bool raop_is_running() { return RA.run.load(); }
static void raop_stats(double& secs_sent, double& hb_age_s, unsigned long long& resends,
                       unsigned& reconnects) {
    secs_sent = RA.frames_sent.load() / 44100.0;
    uint64_t hb = RA.last_timing_ms.load();
    hb_age_s = hb ? (now_ms() - hb) / 1000.0 : -1.0;
    resends = (unsigned long long)RA.resends.load();
    reconnects = RA.reconnects.load();
}
static void raop_stop() {
    if (!RA.run.load()) return;
    RA.run.store(false);
    if (RA.thread && WaitForSingleObject(RA.thread, 3000) == WAIT_OBJECT_0) {
        CloseHandle(RA.thread); RA.thread = nullptr;
    }   // on timeout keep the handle: raop_start drains it before reuse
}

// ---------------------------------------------------------------------------
// mDNS resolution of _raop._tcp AirPlay receivers (PTR -> SRV -> A)
// ---------------------------------------------------------------------------

// compression-aware DNS name reader
static std::string dns_name(const uint8_t* pkt, int len, int& pos) {
    std::string out;
    int p = pos, jumps = 0;
    bool jumped = false;
    while (p < len && jumps < 8) {
        uint8_t l = pkt[p];
        if (l == 0) { p++; break; }
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= len) break;
            int off = ((l & 0x3F) << 8) | pkt[p + 1];
            if (!jumped) pos = p + 2;
            jumped = true; jumps++;
            p = off;
            continue;
        }
        if (p + 1 + l > len) break;
        if (!out.empty()) out += ".";
        out.append((const char*)pkt + p + 1, l);
        p += 1 + l;
    }
    if (!jumped) pos = p;
    return out;
}

// full-resolver socket: bound to 5353 with the group joined on every
// interface, so multicast-only responders and gratuitous announcements are
// heard. Returns INVALID_SOCKET if 5353 can't be shared (falls back to legacy).
static SOCKET mdns_listener() {
    SOCKET m = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m == INVALID_SOCKET) return m;
    int one = 1;
    setsockopt(m, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in ba{}; ba.sin_family = AF_INET;
    ba.sin_addr.s_addr = INADDR_ANY; ba.sin_port = htons(5353);
    if (bind(m, (sockaddr*)&ba, sizeof(ba)) != 0) { closesocket(m); return INVALID_SOCKET; }
    in_addr grp{}; inet_pton(AF_INET, "224.0.0.251", &grp);
    for (auto& ipstr : local_ipv4s()) {
        ip_mreq mr{}; mr.imr_multiaddr = grp;
        inet_pton(AF_INET, ipstr.c_str(), &mr.imr_interface);
        setsockopt(m, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof(mr));
    }
    u_long nb = 1; ioctlsocket(m, FIONBIO, &nb);
    return m;
}

static void raop_resolve() {
    ui_log(L"[mdns] resolving AirPlay receivers (_raop._tcp)...");
    std::vector<std::string> ifs = local_ipv4s();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in ba{}; ba.sin_family = AF_INET; ba.sin_addr.s_addr = INADDR_ANY;
    bind(s, (sockaddr*)&ba, sizeof(ba));
    DWORD tmo = 300;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    int ttl = 4;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    static const uint8_t q[] = {
        0x00,0x00, 0x00,0x00, 0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
        5,'_','r','a','o','p', 4,'_','t','c','p', 5,'l','o','c','a','l', 0,
        0x00,0x0C, 0x80,0x01 };                       // QU bit set on class
    sockaddr_in mc{}; mc.sin_family = AF_INET; mc.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &mc.sin_addr);

    // FIX (rescan bug, part 1/3): pin outgoing multicast to EVERY interface,
    // exactly as ssdp_discover already does. Previously the query egressed
    // only the default-route interface, so on multi-homed machines (VPN,
    // Hyper-V/WSL, VirtualBox adapters) it often never reached the LAN and
    // discovery silently depended on overhearing the receiver's MULTICAST
    // answers to OTHER hosts' queries. Per RFC 6762 §5.4 a receiver only
    // multicasts an answer if the record was NOT multicast within 1/4 of its
    // TTL — up to ~19 min for the _raop PTR/TXT records. So the first scan
    // often got a "stale-record" multicast answer and worked, while a rescan
    // minutes later got unicast answers aimed at other queriers (or at a port
    // the firewall drops) and found nothing.
    auto send_on_all_ifs = [&](SOCKET sock, const uint8_t* pkt, int len) {
        if (ifs.empty()) { sendto(sock, (const char*)pkt, len, 0, (sockaddr*)&mc, sizeof(mc)); return; }
        for (auto& ipstr : ifs) {
            in_addr ifa{}; inet_pton(AF_INET, ipstr.c_str(), &ifa);
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof(ifa));
            sendto(sock, (const char*)pkt, len, 0, (sockaddr*)&mc, sizeof(mc));
        }
    };
    send_on_all_ifs(s, q, sizeof(q));

    // full-resolver path: standard query FROM port 5353 (responders answer
    // this via multicast, which the same socket then receives)
    SOCKET m = mdns_listener();
    uint8_t qm[sizeof(q)]; memcpy(qm, q, sizeof(q));
    qm[sizeof(q) - 2] = 0x00;                         // QM: standard class IN
    if (m != INVALID_SOCKET) {
        send_on_all_ifs(m, qm, sizeof(qm));
    } else {
        ui_log(L"[mdns] port 5353 unavailable — legacy-unicast queries only");
    }

    // FIX (rescan bug, part 2/3): accumulate records ACROSS packets instead
    // of requiring PTR and SRV to arrive bundled in one packet. First-boot
    // announcements bundle PTR+SRV+TXT+A, but rescan answers are refreshes:
    // SRV/A (TTL 120 s) and PTR/TXT (TTL 4500 s) are re-answered on
    // different schedules and may arrive split, unicast, or both. The old
    // per-packet inst/srv_port state discarded those — "ignoring airplay
    // receivers on a rescan", literally.
    struct Partial {
        std::string inst, a_ip, txt, fip, srv_target;
        int srv_port = 0; bool via_mcast = false;
    };
    std::vector<Partial> parts;
    std::vector<std::pair<std::string, std::string>> a_cache;   // hostname -> IPv4
    auto part_for = [&](const std::string& inst) -> Partial& {
        for (auto& p : parts) if (p.inst == inst) return p;
        parts.push_back(Partial{});
        parts.back().inst = inst;
        return parts.back();
    };

    bool requeried = false;
    uint64_t start = now_ms();
    uint8_t buf[4096];
    while (now_ms() - start < 3500) {
        // FIX (rescan bug, part 3/3): re-query once mid-window. The original
        // back-to-back double-send collides with the responder's per-record
        // 1-second multicast rate limit (RFC 6762 §6): the second copy of the
        // answer is suppressed, so one lost first answer killed the scan.
        if (!requeried && now_ms() - start > 1200) {
            requeried = true;
            send_on_all_ifs(s, q, sizeof(q));
            if (m != INVALID_SOCKET) send_on_all_ifs(m, qm, sizeof(qm));
        }
        fd_set rd; FD_ZERO(&rd);
        FD_SET(s, &rd);
        if (m != INVALID_SOCKET) FD_SET(m, &rd);
        timeval tv{ 0, 200000 };
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET rs = FD_ISSET(s, &rd) ? s : m;
        bool via_mcast = (rs == m);
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(rs, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n < 12) continue;
        if (!(buf[2] & 0x80)) continue;               // QR=0: a query, not a response
        char fip[64]; inet_ntop(AF_INET, &from.sin_addr, fip, sizeof(fip));

        int qd = (buf[4] << 8) | buf[5];
        int an = (buf[6] << 8) | buf[7];
        int ns = (buf[8] << 8) | buf[9];
        int ar = (buf[10] << 8) | buf[11];
        int pos = 12;
        for (int i = 0; i < qd && pos < n; ++i) {     // skip questions
            dns_name(buf, n, pos); pos += 4;
        }
        std::vector<std::string> pkt_insts;           // instances seen in THIS packet
        std::string pkt_a_ip;
        int total = an + ns + ar;
        for (int i = 0; i < total && pos + 10 <= n; ++i) {
            std::string nm = dns_name(buf, n, pos);
            if (pos + 10 > n) break;
            int type = (buf[pos] << 8) | buf[pos + 1];
            int rdlen = (buf[pos + 8] << 8) | buf[pos + 9];
            pos += 10;
            int rstart = pos;
            if (pos + rdlen > n) break;
            if (type == 12 && nm.find("_raop") != std::string::npos) {   // PTR
                int rp = rstart;
                std::string target = dns_name(buf, n, rp);
                size_t dot = target.find("._raop");
                if (dot != std::string::npos) {
                    Partial& p = part_for(target.substr(0, dot));
                    p.fip = fip; p.via_mcast |= via_mcast;
                    pkt_insts.push_back(p.inst);
                }
            } else if (type == 33) {                                     // SRV
                size_t dot = nm.find("._raop");
                if (dot != std::string::npos && rdlen >= 6) {
                    Partial& p = part_for(nm.substr(0, dot));
                    p.srv_port = (buf[rstart + 4] << 8) | buf[rstart + 5];
                    int tp = rstart + 6;
                    p.srv_target = dns_name(buf, n, tp);
                    p.fip = fip; p.via_mcast |= via_mcast;
                    pkt_insts.push_back(p.inst);
                }
            } else if (type == 1 && rdlen == 4) {                        // A
                char ipb[32];
                snprintf(ipb, sizeof(ipb), "%u.%u.%u.%u",
                         buf[rstart], buf[rstart+1], buf[rstart+2], buf[rstart+3]);
                pkt_a_ip = ipb;
                a_cache.push_back({ nm, ipb });
            } else if (type == 16 && nm.find("_raop") != std::string::npos) { // TXT
                std::string txt;
                int tp = rstart, tend = rstart + rdlen;
                while (tp < tend) {
                    int len = buf[tp++];
                    if (len <= 0 || tp + len > tend) break;
                    if (!txt.empty()) txt += " ";
                    txt.append((const char*)buf + tp, len);
                    tp += len;
                }
                size_t dot = nm.find("._raop");
                if (dot != std::string::npos && !txt.empty()) {
                    Partial& p = part_for(nm.substr(0, dot));
                    if (txt.size() > p.txt.size()) p.txt = txt;
                    p.fip = fip; p.via_mcast |= via_mcast;
                    pkt_insts.push_back(p.inst);
                }
            }
            pos = rstart + rdlen;
        }
        // an A record in this packet belongs to the services announced with it
        if (!pkt_a_ip.empty())
            for (auto& in : pkt_insts) {
                Partial& p = part_for(in);
                if (p.a_ip.empty()) p.a_ip = pkt_a_ip;
            }
    }
    closesocket(s);
    if (m != INVALID_SOCKET) closesocket(m);

    // materialize accumulated partials into devices
    std::vector<AirplayDev> found;
    for (auto& p : parts) {
        if (p.srv_port == 0 || p.inst.empty()) continue;   // never saw the SRV
        AirplayDev d;
        d.port = p.srv_port;
        d.txt = p.txt;
        d.host = p.a_ip;
        if (d.host.empty())                                // cross-packet A via SRV target
            for (auto& a : a_cache)
                if (a.first == p.srv_target) { d.host = a.second; break; }
        if (d.host.empty()) d.host = p.fip;                // sender of the SRV packet
        // instance is usually "MAC@Friendly Name"
        size_t at = p.inst.find('@');
        d.name = (at != std::string::npos) ? p.inst.substr(at + 1)
                                           : (p.inst.empty() ? d.host : p.inst);
        bool dup = false;
        for (auto& f : found)
            if (f.host == d.host && f.port == d.port) { dup = true; break; }
        if (dup) continue;
        found.push_back(d);
        ui_log8("[mdns] AirPlay receiver: " + d.name + " @ " + d.host + ":" +
                std::to_string(d.port) +
                (p.via_mcast ? "  (via multicast listener)" : "  (unicast reply)"));
        if (!d.txt.empty()) {
            ui_log8("[mdns]   TXT: " + d.txt);
            auto field = [&](const std::string& key) -> std::string {
                size_t fp = d.txt.find(key + "=");
                if (fp == std::string::npos) return "";
                fp += key.size() + 1;
                size_t e = d.txt.find(' ', fp);
                return d.txt.substr(fp, e == std::string::npos ? std::string::npos : e - fp);
            };
            std::string et = field("et"), pk = field("pk"), ft = field("ft");
            if (!et.empty()) {
                bool none_ok = (et == "0" || et.find("0,") != std::string::npos ||
                                et.find(",0") != std::string::npos);
                ui_log8("[mdns]   encryption types (et): " + et +
                        (none_ok ? "  -> unencrypted ACCEPTED (LowCast compatible)"
                                 : "  -> unencrypted NOT offered (LowCast cannot stream)"));
            }
            if (!pk.empty())
                ui_log(L"[mdns]   public key (pk) present -> AirPlay-2 PAIRING "
                       L"required. This needs FairPlay/HomeKit crypto LowCast "
                       L"does not implement. Firmware rollback is the practical fix.");
            if (et.empty() && pk.empty())
                ui_log(L"[mdns]   no et/pk fields -> encryption not the blocker; "
                       L"look elsewhere (volume, routing, codec).");
        } else {
            ui_log(L"[mdns]   (no TXT record captured this pass — rerun probe)");
        }
    }
    if (found.empty()) ui_log(L"[mdns] no AirPlay receivers resolved");

    EnterCriticalSection(&G.cs);
    for (auto& nd : found)
        for (auto& old : g_airplay)
            if (old.host == nd.host && old.port == nd.port) nd.streaming = old.streaming;
    // FIX: never drop a device with a live session just because one scan
    // missed it — the STOP button must survive a bad rescan.
    for (auto& old : g_airplay) {
        if (!old.streaming) continue;
        bool present = false;
        for (auto& nd : found)
            if (nd.host == old.host && nd.port == old.port) { present = true; break; }
        if (!present) {
            old.button = nullptr;
            found.push_back(old);
            ui_log8("[mdns] keeping live session entry: " + old.name +
                    " @ " + old.host + " (not seen this scan)");
        }
    }
    g_airplay = found;
    LeaveCriticalSection(&G.cs);
    PostMessageW(G.hwnd, WM_APP_RENDERS, 0, 0);
}


int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmdline, int ncmd) {
    sess_log_open();
    timeBeginPeriod(1);   // Sleep(1) is ~15.6 ms by default on Windows without this
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    InitializeCriticalSection(&G.cs);
    InitializeCriticalSection(&g_raop_cs);
    InitializeCriticalSection(&RA.hist_cs);
    InitializeCriticalSection(&RA.rtsp_cs);

    // Diagnostic mode:  LowCast.exe probe
    // Runs discovery + casts to the first renderer found, printing every step.
    if (cmdline && wcsstr(cmdline, L"raoptest")) {
        g_console_mode = true;
        g_probe_file = fopen("lowcast-raop.log", "w");
        char host[64] = "127.0.0.1"; int port = 5000, lat = 350, secs = 8;
        int drift_ppm = 0, meas_err_ppm = 0;
        {
            char cl[256]; wcstombs(cl, cmdline, sizeof(cl));
            sscanf(cl, "%*s %63s %d %d %d %d %d", host, &port, &lat, &secs,
                   &drift_ppm, &meas_err_ppm);
        }
        ui_log8(std::string("[raoptest] target ") + host + ":" + std::to_string(port)
                + " latency " + std::to_string(lat) + " ms");
        raop_start(host, port, (uint32_t)lat);
        // tone generator: 440 Hz into the RAOP pipe at realtime
        uint64_t t0 = now_ms(); double ph = 0.0;
        uint64_t pushed = 0;
        uint32_t nr = G.native_rate.load();
        // simulate a soundcard whose TRUE rate is nr*(1+ppm): push that many
        // frames per wall-second, and report it via measured_rate exactly as the
        // real capture thread would after its 4 s measurement window.
        double true_rate = nr * (1.0 + drift_ppm / 1000000.0);
        // meas_err_ppm injects deliberate measurement ERROR: the depth servo
        // must absorb it (its authority is ±400 ppm) for the test to pass
        G.measured_rate.store(true_rate * (1.0 + meas_err_ppm / 1000000.0));
        bool do_gaps = wcsstr(cmdline, L"gaps") != nullptr;
        uint64_t next_depth_log = 10000;
        while ((int)((now_ms() - t0) / 1000) < secs && RA.run.load()) {
            uint64_t elapsed = now_ms() - t0;
            if (elapsed >= next_depth_log) {
                next_depth_log += 10000;
                EnterCriticalSection(&g_raop_cs);
                int d = (int)(g_raop_pipe.size() / 2);
                LeaveCriticalSection(&g_raop_cs);
                ui_log8("[raoptest] t=" + std::to_string(elapsed / 1000)
                        + "s  sender pipe depth: "
                        + std::to_string(d * 1000 / (int)nr) + " ms  ("
                        + std::to_string(d) + " frames)");
            }
            // simulate a track change: 150 ms silence gap every 4 s
            if (do_gaps && (elapsed % 4000) < 150) { Sleep(5); continue; }
            uint64_t want = (uint64_t)(elapsed * true_rate / 1000.0);
            while (pushed < want) {
                float v = 0.25f * (float)sin(ph);
                ph += 2.0 * 3.14159265358979 * 440.0 / true_rate;
                raop_pipe_push(v, v);
                pushed++;
            }
            Sleep(3);
        }
        uint64_t fs = RA.frames_sent.load();
        ui_log8("[raoptest] frames sent: " + std::to_string(fs) +
                " (" + std::to_string(fs / 44100) + " s of audio), retransmitted " +
                std::to_string(RA.resends.load()) + " packets on request");
        raop_stop();
        return fs > 44100 ? 0 : 3;
    }
    if (cmdline && wcsstr(cmdline, L"probe")) {
        g_console_mode = true;
        g_probe_file = fopen("lowcast-probe.log", "w");
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {     // launched from a terminal
            FILE* f; freopen_s(&f, "CONOUT$", "w", stdout);
        }                                               // else keep inherited stdout
        ui_log(L"[probe] LowCast diagnostic mode");
        CloseHandle(CreateThread(nullptr, 0, http_server_thread, nullptr, 0, nullptr));
        Sleep(300);
        ssdp_discover();
        mdns_sweep();
        raop_resolve();
        EnterCriticalSection(&G.cs);
        int n = (int)G.renderers.size();
        Renderer first = n ? G.renderers[0] : Renderer();
        LeaveCriticalSection(&G.cs);
        if (n) {
            ui_log8("[probe] casting to: " + first.name);
            bool ok = start_cast(first);
            ui_log(ok ? L"[probe] cast handshake OK — renderer accepted SetAVTransportURI+Play"
                      : L"[probe] cast handshake FAILED");
            if (!ok) interrogate_device(first);
            Sleep(4000);
            stop_cast(first);
        }
        // --- AirPlay end-to-end test against the resolved receiver ---
        EnterCriticalSection(&G.cs);
        AirplayDev ap = g_airplay.empty() ? AirplayDev() : g_airplay[0];
        LeaveCriticalSection(&G.cs);
        if (ap.port) {
            ui_log8("[probe] testing AirPlay path: " + ap.name + " @ " + ap.host
                    + ":" + std::to_string(ap.port));
            start_capture();                      // feed the pipe (silence is fine)
            Sleep(500);
            raop_start(ap.host, ap.port, 250);
            Sleep(6000);
            uint64_t hb = RA.last_timing_ms.load();
            uint64_t fs = RA.frames_sent.load();
            ui_log8(std::string("[probe] AirPlay heartbeat: ")
                    + (hb ? "YES — receiver engaged (timing requests arriving)"
                          : "NO — receiver never sent timing requests"));
            ui_log8("[probe] AirPlay frames sent in 6 s: " + std::to_string(fs));
            if (RA.run.load() && hb)
                ui_log(fs > 44100
                    ? L"[probe] AirPlay path WORKING end-to-end (audio flowing)"
                    : L"[probe] AirPlay path WORKING (receiver engaged; no local"
                      L" audio captured this run)");
            else if (!RA.run.load())
                ui_log(L"[probe] AirPlay session failed during handshake (see [rtsp] lines)");
            else
                ui_log(L"[probe] AirPlay handshake accepted but receiver not consuming"
                       L" — likely needs encryption or AirPlay-2 pairing");
            raop_stop();
            stop_capture();
        }
        ui_log(L"[probe] done");
        return n ? 0 : 2;
    }
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"LowCastWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    int wx = (int)GetPrivateProfileIntW(L"LowCast", L"winx", -100000, ini_path().c_str());
    int wy = (int)GetPrivateProfileIntW(L"LowCast", L"winy", -100000, ini_path().c_str());
    bool havepos = wx > -3000 && wx < 8000 && wy > -3000 && wy < 8000;
    HWND h = CreateWindowW(L"LowCastWnd",
        L"LowCast: Minimal-Latency WiFi Audio Streamer (DLNA & AirPlay)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        havepos ? wx : CW_USEDEFAULT, havepos ? wy : CW_USEDEFAULT,
        600, 540, nullptr, nullptr, hi, nullptr);
    ShowWindow(h, ncmd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WSACleanup();
    timeEndPeriod(1);
    return 0;
}

