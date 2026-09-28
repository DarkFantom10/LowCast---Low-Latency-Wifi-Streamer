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
#define _WIN32_WINNT 0x0602
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <timeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>
#include "resource.h"
#include "volume_track_renderer.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <shellapi.h>
#include <shobjidl_core.h>
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
// ---------------------------------------------------------------------------
// One monotonic clock for everything time-critical (packet scheduling, the
// duplicate queue, diagnostics cadence, AND the AirPlay NTP timestamps).
//
// Base: the kernel's interrupt-time tick (QueryUnbiasedInterruptTime): system-
// wide, monotonic, ~1 ms resolution while the 1 ms timer is honored, immune to
// wall-clock steps AND to per-core TSC skew. QueryPerformanceCounter only
// refines the sub-tick part and is trusted only while it agrees with the tick
// clock: the log shows the stream thread's QPC reading ~5.5 s BEHIND for
// several seconds (2026-09-02 23:48, 2026-09-05 02:11) while the capture
// thread's clock stayed correct (i9-12900K, hybrid P/E cores). Each such
// disagreement is counted in g_clock_glitches and shown as clk= in [diag].
// ---------------------------------------------------------------------------
static std::atomic<uint64_t> g_clock_glitches{0};   // QPC disagreed with the tick clock (>20 ms)
static uint64_t mono_100ns() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    static std::atomic<int64_t>  clk_delta{INT64_MIN};  // tick - qpc(100ns) at last re-base
    static std::atomic<uint64_t> clk_last{0};           // monotonic clamp across threads
    static std::atomic<uint64_t> clk_agree{0};          // tick time of the last agreement
    ULONGLONG it = 0; QueryUnbiasedInterruptTime(&it);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const int64_t f = freq.QuadPart;
    int64_t q100 = (q.QuadPart / f) * 10000000LL + (q.QuadPart % f) * 10000000LL / f;
    int64_t delta = clk_delta.load(std::memory_order_relaxed);
    uint64_t v;
    if (delta == INT64_MIN) {
        clk_delta.store((int64_t)it - q100, std::memory_order_relaxed);
        clk_agree.store(it, std::memory_order_relaxed);
        v = it;
    } else {
        // Healthy: est is true time and the tick lags it by up to one tick, so
        // est - it sits in [0, tick]. Beyond +-20 ms QPC is lying (per-core
        // skew): count it and use the tick. More than 3 ms BEHIND the tick can
        // only be QPC running slow: re-base. (Ahead by 3..20 ms is just tick
        // staleness when the 1 ms timer is not honored: still true time.)
        int64_t err = q100 + delta - (int64_t)it;
        if (err > 200000 || err < -200000) {
            g_clock_glitches.fetch_add(1, std::memory_order_relaxed);
            v = it;
            // A lasting offset (anchor taken on a skewed core, S3 re-bias) would
            // glitch forever: if NO thread has agreed for 2 s, re-base. While any
            // healthy thread keeps agreeing, a skewed core never re-bases.
            int64_t agree_age = (int64_t)it - (int64_t)clk_agree.load(std::memory_order_relaxed);
            if (agree_age > 20000000LL) {
                clk_delta.store((int64_t)it - q100, std::memory_order_relaxed);
                clk_agree.store(it, std::memory_order_relaxed);
            }
        } else if (err < -30000) {
            clk_delta.store((int64_t)it - q100, std::memory_order_relaxed);
            clk_agree.store(it, std::memory_order_relaxed);
            v = it;
        } else {
            clk_agree.store(it, std::memory_order_relaxed);
            v = (uint64_t)(q100 + delta);
        }
    }
    uint64_t last = clk_last.load(std::memory_order_relaxed);   // never step backwards
    while (v > last && !clk_last.compare_exchange_weak(last, v, std::memory_order_relaxed)) {}
    return v > last ? v : last;
}
static uint64_t now_ms() { return mono_100ns() / 10000ULL; }

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
    std::atomic<uint32_t> hw_capgap{0};        // worst capture gap this session (ms)
    std::atomic<uint64_t> beep_flash_ms{0};  // set by audio thread when a tick is injected
    // network
    CRITICAL_SECTION   cs;                   // guards clients
    std::vector<Client*> clients;
    std::atomic<int>   http_port{0};
    std::atomic<uint64_t> backlog_ms_x10{0}; // worst client backlog, tenths of ms
    // ui
    HWND hwnd = nullptr;
    HWND log = nullptr, cmb_dev = nullptr, cmb_fmt = nullptr, cmb_rate = nullptr,
         stat = nullptr, flash = nullptr, btn_scan = nullptr, btn_beep = nullptr,
         btn_localbeep = nullptr, cmb_aplat = nullptr,
         sld_apvol = nullptr, lbl_apvol = nullptr, lbl_batt = nullptr;
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
#define IDT_SHUTDOWN    102
static bool g_closing = false;      // UI thread only
static bool g_ui_preview = false;   // safe visual-QA mode: no audio/network/settings
#define IDC_RENDER_BASE 2000
#define IDC_BATTERY_ACK 3900

static std::atomic<bool> g_console_mode{false};
static std::atomic<FILE*> g_probe_file{nullptr};
static FILE* g_sess_log = nullptr;          // owned by the logging worker
static CRITICAL_SECTION g_sess_log_cs;      // queue ONLY; never held over I/O
static std::atomic<HWND> g_log_hwnd{nullptr};
struct LogEntry { SYSTEMTIME time; wchar_t text[2048]; };
static const size_t LOG_QUEUE_SIZE = 256;
static LogEntry g_log_queue[LOG_QUEUE_SIZE];
static size_t g_log_head = 0, g_log_count = 0;
static std::atomic<uint64_t> g_log_dropped{0};
static std::atomic<bool> g_log_accept{false}, g_log_stop{false};
static HANDLE g_log_event = nullptr, g_log_thread = nullptr;

static void log_write(const LogEntry& entry) {
    const SYSTEMTIME& st = entry.time;
    std::string u8 = wide_to_utf8(entry.text);
    if (g_sess_log)
        fprintf(g_sess_log, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, u8.c_str());
    if (g_console_mode.load()) {
        fwprintf(stdout, L"%ls\n", entry.text);
        FILE* probe = g_probe_file.load();
        if (probe) fprintf(probe, "%s\n", u8.c_str());
    }
    HWND hwnd = g_log_hwnd.load();
    if (hwnd) {
        std::wstring* msg = new std::wstring(entry.text);
        if (!PostMessageW(hwnd, WM_APP_LOG, 0, (LPARAM)msg)) delete msg;
    }
}
static DWORD WINAPI log_worker(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        WaitForSingleObject(g_log_event, 250);
        // A bounded batch lets shutdown/drop reporting run even under a flood.
        for (size_t i = 0; i < LOG_QUEUE_SIZE; ++i) {
            LogEntry entry{};
            EnterCriticalSection(&g_sess_log_cs);
            bool have = g_log_count != 0;
            if (have) {
                entry = g_log_queue[g_log_head];
                g_log_head = (g_log_head + 1) % LOG_QUEUE_SIZE;
                --g_log_count;
            }
            LeaveCriticalSection(&g_sess_log_cs);
            if (!have) break;
            log_write(entry);
        }
        uint64_t dropped = g_log_dropped.exchange(0);
        if (dropped) {
            LogEntry entry{}; GetLocalTime(&entry.time);
            swprintf(entry.text, 2048, L"[log] dropped %llu messages (queue busy/full); audio never waits for log I/O",
                     (unsigned long long)dropped);
            log_write(entry);
        }
        if (g_sess_log) fflush(g_sess_log);
        if (g_console_mode.load()) {
            fflush(stdout);
            FILE* probe = g_probe_file.load();
            if (probe) fflush(probe);
        }
        EnterCriticalSection(&g_sess_log_cs);
        bool empty = g_log_count == 0;
        LeaveCriticalSection(&g_sess_log_cs);
        if (g_log_stop.load() && empty) break;
        if (!empty) SetEvent(g_log_event);
    }
    if (g_sess_log) { fclose(g_sess_log); g_sess_log = nullptr; }
    return 0;
}
static void sess_log_open() {
    InitializeCriticalSection(&g_sess_log_cs);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n && n < MAX_PATH) {
        while (n > 0 && path[n - 1] != L'\\') --n;
        if (wcscpy_s(path + n, MAX_PATH - n, L"lowcast.log") == 0)
            g_sess_log = _wfopen(path, L"a");
    }
    g_log_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_log_event) g_log_thread = CreateThread(nullptr, 0, log_worker, nullptr, 0, nullptr);
    if (g_log_thread) g_log_accept.store(true);
    else {
        if (g_sess_log) { fclose(g_sess_log); g_sess_log = nullptr; }
        if (g_log_event) { CloseHandle(g_log_event); g_log_event = nullptr; }
        OutputDebugStringW(L"LowCast: async logger could not start; file logging disabled\n");
    }
}
static void sess_log_close() {
    // UI/main-thread cleanup only. Leave process-lifetime queue objects intact:
    // detached discovery/battery workers may finish a late log call on exit.
    g_log_accept.store(false);
    EnterCriticalSection(&g_sess_log_cs);
    g_log_stop.store(true);
    LeaveCriticalSection(&g_sess_log_cs);
    if (g_log_event) SetEvent(g_log_event);
    if (g_log_thread && WaitForSingleObject(g_log_thread, 2000) == WAIT_OBJECT_0) {
        CloseHandle(g_log_thread); g_log_thread = nullptr;
    }
    // On a stuck storage write do not close FILE/event beneath the worker.
}
static void ui_log(const std::wstring& msg) {
    if (!g_log_accept.load()) return;
    SYSTEMTIME time; GetLocalTime(&time);      // event time, not disk-write time
    if (!TryEnterCriticalSection(&g_sess_log_cs)) {
        g_log_dropped.fetch_add(1); return;
    }
    if (!g_log_accept.load()) { LeaveCriticalSection(&g_sess_log_cs); return; }
    if (g_log_count == LOG_QUEUE_SIZE) {
        LeaveCriticalSection(&g_sess_log_cs);
        g_log_dropped.fetch_add(1); return;
    }
    LogEntry& entry = g_log_queue[(g_log_head + g_log_count) % LOG_QUEUE_SIZE];
    entry.time = time;
    size_t len = msg.size() < 2047 ? msg.size() : 2047;
    memcpy(entry.text, msg.data(), len * sizeof(wchar_t));
    entry.text[len] = 0;
    if (msg.size() > len) { entry.text[len - 3] = L'.'; entry.text[len - 2] = L'.'; entry.text[len - 1] = L'.'; }
    ++g_log_count;
    LeaveCriticalSection(&g_sess_log_cs);
    SetEvent(g_log_event);
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
static void raop_send_volume_pct(int vp, bool logit = true);   // one guarded RAOP volume cmd
static int  raop_suggested_floor_ms();       // measured min buffer; -1 if no session
static void raop_push_volume(int pct);       // queue a volume for the live session
static bool raop_is_running();
static void raop_stats(double& secs_sent, double& hb_age_s, unsigned long long& resends,
                       unsigned& reconnects);

DEFINE_GUID(IID_IAudioClient3_L, 0x7ED4EE07, 0x8E67, 0x4CD4,
            0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42);
DEFINE_GUID(IID_IAudioRenderClient_L2, 0xF294ACFC, 0x3146, 0x4483,
            0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

static std::atomic<int>  g_cap_relaunches{0};      // capture relaunches since the last real packet
static std::atomic<uint32_t> g_capture_gen{0};
struct CaptureContext {
    uint32_t generation;
    HANDLE stop;
    bool retry = false;                          // owned by the capture thread
};
static bool capture_active(const CaptureContext* ctx) {
    return G.capture_run.load() && g_capture_gen.load() == ctx->generation &&
           WaitForSingleObject(ctx->stop, 0) == WAIT_TIMEOUT;
}
struct PeriodDriverArgs { IMMDevice* dev; HANDLE stop; HANDLE capture_stop; };

// Keeps a minimum-period silent render stream open on the capture device so
// the shared audio engine ticks at its fastest rate -> loopback delivers
// audio in ~3 ms chunks instead of ~10 ms. Fails silently where unsupported.
static DWORD WINAPI period_driver_thread(LPVOID p) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    PeriodDriverArgs* a = (PeriodDriverArgs*)p;
    IMMDevice* dev = a->dev;
    HANDLE stop = a->stop, capture_stop = a->capture_stop;
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
        if (!evt || FAILED(ac3->SetEventHandle(evt))) break;
        if (FAILED(ac3->GetService(IID_IAudioRenderClient_L2, (void**)&rc))) break;
        UINT32 bufsz = 0;
        if (FAILED(ac3->GetBufferSize(&bufsz))) break;
        if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT ||
            WaitForSingleObject(capture_stop, 0) != WAIT_TIMEOUT) break;
        if (FAILED(ac3->Start())) break;
        {
            wchar_t b[160];
            swprintf(b, 160, L"[audio] engine period driver active: %u -> %u frames "
                     L"(~%.1f ms) — loopback now low-latency", def, mn,
                     mn * 1000.0 / wf->nSamplesPerSec);
            ui_log(b);
        }
        HANDLE waits[] = { stop, capture_stop, evt };
        for (;;) {
            DWORD wake = WaitForMultipleObjects(3, waits, FALSE, 20);
            if (wake == WAIT_OBJECT_0 || wake == WAIT_OBJECT_0 + 1 || wake == WAIT_FAILED) break;
            UINT32 pad = 0;
            if (FAILED(ac3->GetCurrentPadding(&pad)) || pad > bufsz) break;
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
// The capture worker owns and joins its period worker, including failed-open
// paths. Cancellation events are never reset/reused by a replacement session.
struct PeriodDriver {
    HANDLE thread = nullptr, stop = nullptr;
    void start(IMMDevice* dev, HANDLE capture_stop) {
        stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop) return;
        dev->AddRef();
        PeriodDriverArgs* args = new PeriodDriverArgs{ dev, stop, capture_stop };
        thread = CreateThread(nullptr, 0, period_driver_thread, args, 0, nullptr);
        if (!thread) { dev->Release(); delete args; CloseHandle(stop); stop = nullptr; }
    }
    ~PeriodDriver() {
        if (stop) SetEvent(stop);
        if (thread) {
            // Background cleanup may wait on a driver. The UI keeps the capture
            // handle on its bounded join timeout and refuses overlapping starts.
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        if (stop) CloseHandle(stop);
    }
};
static bool raop_start(const std::string& host, int port, uint32_t latency_ms);
static bool raop_stop(bool wait_full = false);
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

// Reopen on the same owned capture thread. Both GUI and console modes retain
// one join handle; no exiting worker publishes a replacement handle.
static void capture_schedule_relaunch(CaptureContext* ctx) {
    if (!capture_active(ctx)) return;
    int n = g_cap_relaunches.fetch_add(1);
    DWORD back = 150u << (n < 5 ? n : 5);            // ~200 ms, 300, 600 ... 4.8 s
    if (WaitForSingleObject(ctx->stop, back) == WAIT_TIMEOUT && capture_active(ctx))
        ctx->retry = true;
}
// Open-time failure. On a relaunch (device flapping after DEVICE_INVALIDATED or
// a driver reset) keep retrying with backoff; on a first start report and stop.
static void capture_open_failed(CaptureContext* ctx) {
    if (!capture_active(ctx)) return;
    if (g_cap_relaunches.load() > 0) capture_schedule_relaunch(ctx);
    else G.capture_run.store(false);
}

static bool capture_source_format(const WAVEFORMATEX* wf, bool& is_float,
                                  int& bits, int& channels, uint32_t& mask) {
    if (!wf || !wf->nSamplesPerSec || !wf->nChannels) return false;
    bits = wf->wBitsPerSample; channels = wf->nChannels; mask = 0;
    is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcm = wf->wFormatTag == WAVE_FORMAT_PCM;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wf->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
        const WAVEFORMATEXTENSIBLE* we = (const WAVEFORMATEXTENSIBLE*)wf;
        is_float = IsEqualGUID(we->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_L);
        pcm = IsEqualGUID(we->SubFormat, KSDATAFORMAT_SUBTYPE_PCM_L);
        mask = we->dwChannelMask;
    }
    if (!(is_float ? bits == 32 : pcm && (bits == 16 || bits == 24 || bits == 32))) return false;
    return wf->nBlockAlign >= channels * (bits / 8);
}

static DWORD capture_attempt(CaptureContext* ctx) {
    if (!capture_active(ctx)) return 0;
    PeriodDriver period_driver;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* denum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_L, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator_L, (void**)&denum);
    if (FAILED(hr)) { ui_log(L"[audio] device enumerator failed");
                      capture_open_failed(ctx); CoUninitialize(); return 1; }

    IMMDevice* dev = nullptr;
    int idx = G.capture_dev.load();
    bool following_default = !(idx >= 0 && idx < (int)G.dev_ids.size());
    if (!following_default)
        hr = denum->GetDevice(G.dev_ids[idx].c_str(), &dev);
    else
        hr = denum->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (FAILED(hr) || !dev) { ui_log(L"[audio] cannot open capture device");
                              denum->Release(); capture_open_failed(ctx);
                              CoUninitialize(); return 1; }
    // remember which endpoint we're on so we can notice if the default moves
    std::wstring opened_id;
    { LPWSTR wid = nullptr; if (SUCCEEDED(dev->GetId(&wid)) && wid) { opened_id = wid; CoTaskMemFree(wid); } }

    if (!capture_active(ctx)) { dev->Release(); denum->Release(); CoUninitialize(); return 0; }
    period_driver.start(dev, ctx->stop);

    IAudioClient* ac = nullptr;
    hr = dev->Activate(IID_IAudioClient_L, CLSCTX_ALL, nullptr, (void**)&ac);
    WAVEFORMATEX* wf = nullptr;
    if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&wf);
    if (FAILED(hr)) {
        ui_log(L"[audio] mix format failed");
        if (ac) ac->Release();
        dev->Release(); denum->Release();
        capture_open_failed(ctx); CoUninitialize(); return 1;
    }

    if (!capture_active(ctx)) {
        CoTaskMemFree(wf); ac->Release(); dev->Release(); denum->Release();
        CoUninitialize(); return 0;
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
    // Reactivation above can return a different mix format. Decode and publish
    // only this final format, never metadata from the failed event-mode client.
    bool src_float = false; int src_bits = 0, src_ch = 0;
    uint32_t chmask = 0;
    if (SUCCEEDED(hr) && !capture_source_format(wf, src_float, src_bits, src_ch, chmask)) {
        ui_log(L"[audio] unsupported or malformed capture mix format");
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
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
        capture_open_failed(ctx); CoUninitialize(); return 1;
    }

    if (!capture_active(ctx)) {
        ac->Stop(); cap->Release(); ac->Release(); CoTaskMemFree(wf);
        if (audio_evt) CloseHandle(audio_evt);
        dev->Release(); denum->Release(); CoUninitialize(); return 0;
    }
    G.native_rate.store(wf->nSamplesPerSec);
    G.measured_rate.store((double)wf->nSamplesPerSec);
    {
        uint32_t adv = wf->nSamplesPerSec * (uint32_t)G.upmult.load();
        uint32_t old = G.sample_rate.exchange(adv);
        if (old != adv && kill_all_clients() > 0)
            ui_log(L"[audio] stream rate changed \x2014 active streams closed; press START again");
    }

    {
        wchar_t buf[128];
        swprintf(buf, 128, L"[audio] capturing: %u Hz, %d ch, %ls%d-bit source",
                 wf->nSamplesPerSec, src_ch, src_float ? L"float " : L"", src_bits);
        ui_log(buf);
    }

    const uint32_t rate = wf->nSamplesPerSec;      // native pacing rate
    const int  mult = G.upmult.load();             // linear-interp upsampling
    const int src_bpf = wf->nBlockAlign;
    // stereo downmix coefficients for ALL source channels (see build_downmix)
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
    bool dg_idle = false;   // silence-fill fired since the last real packet
    uint64_t dg_next = now_ms() + 10000;
    while (capture_active(ctx)) {
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
        HRESULT chr = S_OK;
        while (SUCCEEDED(chr = cap->GetNextPacketSize(&pkt)) && pkt > 0) {
            got_real = true;
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(chr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            dg_pkts++;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) dg_disc++;   // WASAPI glitched
            { uint64_t nn = now_ms();
              if (dg_lastpkt) {
                  uint64_t g = nn - dg_lastpkt;
                  if (dg_idle) {
                      // The gap spans a silence-fill stretch: WASAPI delivers
                      // nothing while NO app renders, so this measures "game
                      // was quiet", not "Windows stalled" (field case: an
                      // 18.8s idle recorded as cap gap poisoned the floor for
                      // 5.5 hours). Real stalls freeze this thread entirely,
                      // so no fill runs during them and they still record.
                      dg_idle = false;
                  } else {
                      if (g > dg_gapmax) dg_gapmax = g;
                      uint32_t hw = G.hw_capgap.load();
                      if (g < 2000 && g > hw)   // sleep/freeze-class events are
                          G.hw_capgap.store((uint32_t)g);   // not schedulable
                  }                                         // jitter either
              }
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
        if (FAILED(chr)) {
            // AUDCLNT_E_DEVICE_INVALIDATED (format change in Sound settings,
            // driver restart, device removed) used to be folded into "no data":
            // capture degraded to permanent silence fill with no log line. Treat
            // any capture-client failure as a device change and reopen.
            wchar_t ce[96];
            swprintf(ce, 96, L"[audio] capture client failed (hr=0x%08lX) \x2014 reopening device",
                     (unsigned long)chr);
            ui_log(ce);
            device_changed = true;
            break;
        }

        // 2) wall-clock silence fill: if nothing is playing, WASAPI delivers no
        //    packets, but the renderer must keep receiving realtime audio or it
        //    stalls and re-buffers (adding latency back). Keep it fed.
        //    Re-anchor the clock whenever real audio flows so soundcard-vs-QPC
        //    drift can never accumulate into an audible mid-music gap.
        if (got_real) { t0 = now_ms() - total_frames * 1000ULL / rate; g_cap_relaunches.store(0); }

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
        if (expected > total_frames + rate / 2) {
            // > 500 ms behind = the process/system was frozen or asleep, not
            // "nothing is playing". Replaying that deficit as silence blasted it
            // at ~45x realtime in 250 ms bursts (TRIM storm, poisoned floor
            // telemetry, receiver/HTTP flood) for audio the receiver could never
            // use. Drop the deficit and re-anchor to what was actually produced.
            uint64_t deficit = expected - total_frames;
            uint64_t lost_ms = deficit * 1000ULL / rate;
            // DLNA/HTTP clients consume a byte stream and keep their buffer
            // margin only if the timeline stays continuous: hand them the
            // deficit as silence but keep it OUT of the RAOP pipe, where
            // old-timestamped frames would only be trimmed. Capped at 1 s:
            // one fanout() push above ~4 MB (MAX_CLIENT_BACKLOG) would drop
            // the client instead (3 s at 4x/24-bit is 3.5 MB).
            uint64_t http_fill = deficit > (uint64_t)rate ? (uint64_t)rate : deficit;
            for (uint64_t i = 0; i < http_fill; ++i) { float zl = 0.f, zr = 0.f; emit(zl, zr); }
            t0 = now_ms() - total_frames * 1000ULL / rate;
            expected = total_frames;
            meas_t0 = 0;                                          // window invalid
            wchar_t fz[144];
            swprintf(fz, 144, L"[audio] capture clock jumped %llu ms (freeze/sleep) "
                              L"\x2014 re-anchored; AirPlay deficit dropped, DLNA fed <=1 s silence",
                     (unsigned long long)lost_ms);
            ui_log(fz);
        }
        // Threshold 50 ms (was 20 = two engine periods). A late WASAPI delivery
        // is not silence: at 20 ms a routine 25 ms hiccup spliced ~23 ms of zeros
        // INTO continuous audio and the delayed content then landed on top of
        // it. Log histogram: routine delivery gaps <= 32 ms. The receiver absorbs
        // an in-order sender stall of up to ~200 ms (it waits on an empty ring
        // with its DAC cushion intact), so a later first fill at silence onset
        // costs nothing audible.
        if (expected > total_frames + rate / 20) {                // >50 ms behind
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
            dg_idle = true;
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
            bool consumers;                              // anyone actually listening?
            EnterCriticalSection(&G.cs);
            consumers = !G.clients.empty();
            LeaveCriticalSection(&G.cs);
            if (consumers || raop_is_running()) {        // idle telemetry is just noise
                wchar_t db[160];
                swprintf(db, 160, L"[diag] cap: pkts=%llu disc=%llu maxgap=%llums fill=%llums",
                         (unsigned long long)dg_pkts, (unsigned long long)dg_disc,
                         (unsigned long long)dg_gapmax,
                         (unsigned long long)(dg_fill * 1000 / rate));
                ui_log(db);
            }
            dg_pkts = dg_disc = dg_gapmax = dg_fill = 0;  // window resets either way
            dg_next += 10000;
        }
        if (event_mode) WaitForSingleObject(audio_evt, 5);   // wake on audio-ready
        else            Sleep(1);
    }

    ac->Stop();
    if (audio_evt) CloseHandle(audio_evt);
    cap->Release(); ac->Release(); CoTaskMemFree(wf); dev->Release(); denum->Release();
    CoUninitialize();
    if (device_changed) capture_schedule_relaunch(ctx);
    return 0;
}
static DWORD WINAPI capture_thread(LPVOID param) {
    CaptureContext* ctx = (CaptureContext*)param;
    DWORD result;
    do {
        ctx->retry = false;
        result = capture_attempt(ctx);           // also joins this attempt's period worker
    } while (ctx->retry && capture_active(ctx));
    return result;
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
    if (ls == INVALID_SOCKET) { ui_log(L"[http] cannot create server socket"); return 1; }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY;
    int port = 16600;
    bool bound = false;
    for (; port < 16620; ++port) {
        a.sin_port = htons((u_short)port);
        if (bind(ls, (sockaddr*)&a, sizeof(a)) == 0) { bound = true; break; }
    }
    if (!bound) { closesocket(ls); ui_log(L"[http] ports 16600-16619 are unavailable"); return 1; }
    if (listen(ls, 8) != 0) { closesocket(ls); ui_log(L"[http] listen failed"); return 1; }
    G.http_port.store(port);
    {
        wchar_t b[96]; swprintf(b, 96, L"[http] server listening on port %d", port);
        ui_log(b);
    }
    for (;;) {
        SOCKET cs = accept(ls, nullptr, nullptr);
        if (cs == INVALID_SOCKET) break;
        HANDLE worker = CreateThread(nullptr, 0, client_thread, (LPVOID)(uintptr_t)cs, 0, nullptr);
        if (worker) CloseHandle(worker);
        else closesocket(cs);
    }
    closesocket(ls);
    int expected = port;
    G.http_port.compare_exchange_strong(expected, 0);
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
    if (s == INVALID_SOCKET) return {};
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(9);
    if (inet_pton(AF_INET, target_host.c_str(), &a.sin_addr) != 1) {
        addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
        if (getaddrinfo(target_host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            closesocket(s); return {};
        }
        a.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return {}; }
    sockaddr_in me{}; int ml = sizeof(me);
    if (getsockname(s, (sockaddr*)&me, &ml) != 0) { closesocket(s); return {}; }
    char ip[64]{};
    bool converted = inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip)) != nullptr;
    closesocket(s);
    return converted ? std::string(ip) : std::string();
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
    if (socks.empty()) { ui_log(L"[ssdp] cannot create a discovery socket on any interface"); return; }

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
        std::string ctrl, oh_ctrl, cm_ctrl, svc_inventory;
        size_t pos = 0;
        while (true) {
            size_t sb = ifind(xml, "<service>", pos);
            if (sb == std::string::npos) break;
            size_t se = ifind(xml, "</service>", sb);
            if (se == std::string::npos) break;
            std::string block = xml.substr(sb, se - sb);
            pos = se + 10;
            std::string type = xml_tag(block, "serviceType");
            if (!type.empty()) {                 // full inventory for the log:
                std::string shortt = type;       // vendor-custom services (a
                size_t u = shortt.rfind("service:");   // battery/status API?)
                if (u != std::string::npos) shortt = shortt.substr(u + 8);
                if (!svc_inventory.empty()) svc_inventory += ", ";
                svc_inventory += shortt;
            }
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
        if (!svc_inventory.empty())
            ui_log8("[ssdp]   services: " + svc_inventory);
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
    if (s == INVALID_SOCKET) { ui_log(L"[mdns] cannot create discovery socket"); return; }
    sockaddr_in ba{}; ba.sin_family = AF_INET; ba.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr*)&ba, sizeof(ba)) != 0) {
        closesocket(s); ui_log(L"[mdns] cannot bind discovery socket"); return;
    }
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
    // header set and order mirror swyh-rs (ureq), plus "Connection: close":
    // http_request has no chunked-terminator detection, so a keep-alive peer
    // answering chunked would otherwise cost the 5 s socket timeout per call.
    // Some embedded SOAP stacks are irrationally picky, so otherwise match a
    // client known to work.
    char hdr[1024];
    snprintf(hdr, sizeof(hdr),
             "POST %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "User-Agent: swyh-rs/1.20.5\r\n"
             "Accept: */*\r\n"
             "SOAPAction: \"%s\"\r\n"
             "Content-Type: text/xml; charset=\"utf-8\"\r\n"
             "Connection: close\r\n"
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
    int http_port = G.http_port.load();
    if (http_port <= 0) {
        ui_log(L"[upnp] local stream server is unavailable");
        return false;
    }
    std::string ip = local_ip_for(r.host);
    if (ip.empty()) {
        ui_log8("[upnp] cannot determine the local address used to reach " + r.host);
        return false;
    }
    char uri[256];
    snprintf(uri, sizeof(uri), "http://%s:%d/stream.%s", ip.c_str(), http_port,
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
    // The format is process-global: switch it BEFORE the renderer's GET can
    // arrive, and end any client still streaming the old format first (as the
    // manual dropdown path does), or a live client gets an endianness /
    // container flip mid-stream.
    // (That includes this renderer's own first-attempt client, which carries
    // the rejected format.)
    if (kill_all_clients() > 0)
        ui_log(L"[upnp] active streams closed for the format fallback \x2014 press START on other renderers again");
    G.fmt.store((int)alt);                       // HTTP server must serve what we promised
    PostMessageW(G.hwnd, WM_APP_RENDERS, 2, 0);  // sync the format dropdown
    if (!try_cast_format(r, alt)) {
        // Rejected too. The renderer may have pre-fetched the stream during
        // SetAVTransportURI, so end it BEFORE flipping the format back, and
        // restore only if nobody changed the dropdown while the SOAP calls
        // were in flight (start_cast runs on a worker thread outside G.cs).
        kill_all_clients();
        int e = (int)alt;
        if (G.fmt.compare_exchange_strong(e, (int)sel))
            PostMessageW(G.hwnd, WM_APP_RENDERS, 3, 0);   // "fallback rejected"
        return false;
    }
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
static CaptureContext* g_capture_context = nullptr; // UI/console owner; never changed by worker
static bool stop_capture() {
    G.capture_run.store(false);
    if (g_capture_context) SetEvent(g_capture_context->stop);
    if (g_capture_handle) {
        if (WaitForSingleObject(g_capture_handle, 3000) != WAIT_OBJECT_0) return false;
        CloseHandle(g_capture_handle);
        g_capture_handle = nullptr;
    }
    if (g_capture_context) {
        CloseHandle(g_capture_context->stop);
        delete g_capture_context;
        g_capture_context = nullptr;
    }
    return true;
}
static void start_capture() {
    if (!stop_capture()) {
        ui_log(L"[audio] previous capture driver is still closing; retry the source selection shortly");
        return;
    }
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) { ui_log(L"[audio] cannot create capture cancellation event"); return; }
    uint32_t gen = g_capture_gen.fetch_add(1) + 1;
    g_capture_context = new CaptureContext{ gen, stop };
    g_cap_relaunches.store(0);
    G.capture_run.store(true);
    G.frames_captured.store(0);
    G.silence_frames.store(0);
    g_capture_handle = CreateThread(nullptr, 0, capture_thread,
                                    g_capture_context, 0, nullptr);
    if (!g_capture_handle) {
        G.capture_run.store(false);
        CloseHandle(stop); delete g_capture_context; g_capture_context = nullptr;
        ui_log(L"[audio] cannot create capture worker");
    }
}

static void raop_resolve();
static DWORD WINAPI discover_thread(LPVOID) { ssdp_discover(); raop_resolve(); return 0; }
static bool start_detached_thread(LPTHREAD_START_ROUTINE entry, const wchar_t* failure) {
    HANDLE thread = CreateThread(nullptr, 0, entry, nullptr, 0, nullptr);
    if (!thread) { ui_log(failure); return false; }
    CloseHandle(thread);
    return true;
}
struct CastJob { Renderer renderer; bool start; };
static DWORD WINAPI cast_thread(LPVOID p);

static bool same_renderer(const Renderer& a, const Renderer& b) {
    return a.control_url == b.control_url && a.oh_url == b.oh_url;
}

// A device has ONE DAC: a DLNA session and an AirPlay session to the same host
// are mutually exclusive. Stop any DLNA renderer bound to `host` before we hand
// the device to RAOP (or the RAOP audio streams into a DAC still owned by DLNA).
static void stop_dlna_to_host(const std::string& host) {
    std::vector<Renderer> victims;
    EnterCriticalSection(&G.cs);
    for (const auto& renderer : G.renderers)
        if (renderer.streaming && renderer.host == host)
            victims.push_back(renderer);
    LeaveCriticalSection(&G.cs);
    for (const auto& renderer : victims) {
        ui_log8("[raop] stopping DLNA session to " + host +
                " first (one device, one output)");
        CastJob* job = new CastJob{ renderer, false };
        HANDLE t = CreateThread(nullptr, 0, cast_thread, job, 0, nullptr);
        if (t) { WaitForSingleObject(t, 4000); CloseHandle(t); }
        else { delete job; ui_log(L"[cast] cannot create DLNA stop worker"); }
    }
    if (!victims.empty()) Sleep(600);   // let the firmware release the pipeline
}
static DWORD WINAPI cast_thread(LPVOID p) {
    CastJob* job = (CastJob*)p;
    Renderer r = job->renderer;
    bool ok = true;
    if (job->start) ok = start_cast(r);
    else stop_cast(r);
    EnterCriticalSection(&G.cs);
    for (auto& live : G.renderers) {
        if (same_renderer(live, r)) {
            live.streaming = job->start && ok;
            break;
        }
    }
    LeaveCriticalSection(&G.cs);
    PostMessageW(G.hwnd, WM_APP_RENDERS, 1, 0);   // refresh button labels only
    delete job;
    return 0;
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------
static HFONT g_font = nullptr, g_font_big = nullptr;

// Low-glare dark palette for light-sensitive/night use. Avoid pure black,
// pure white and high-energy blue accents.
static const COLORREF CLR_BG        = RGB(22, 24, 27);    // #16181B
static const COLORREF CLR_SURFACE   = RGB(34, 37, 42);    // #22252A
static const COLORREF CLR_HOVER     = RGB(42, 45, 51);    // #2A2D33
static const COLORREF CLR_PRESSED   = RGB(47, 41, 35);    // restrained brown
static const COLORREF CLR_CHECKED   = RGB(61, 49, 39);
static const COLORREF CLR_BORDER    = RGB(56, 61, 69);    // #383D45
static const COLORREF CLR_FOCUS     = RGB(132, 99, 65);   // leather-brown
static const COLORREF CLR_TEXT      = RGB(200, 204, 210); // #C8CCD2
static const COLORREF CLR_SECONDARY = RGB(158, 165, 175); // #9EA5AF
static const COLORREF CLR_DISABLED  = RGB(111, 116, 124);
static const COLORREF CLR_FLASH_ON  = RGB(115, 86, 50);   // #735632 subdued ochre
static const COLORREF CLR_FLASH_TXT = RGB(210, 213, 218); // soft gray, never white
static const COLORREF CLR_TRACK     = RGB(74, 80, 90);
static const COLORREF CLR_THUMB     = RGB(98, 105, 117);
static const COLORREF CLR_THUMB_HOT = RGB(116, 123, 136);

static HBRUSH g_br_bg = nullptr, g_br_surface = nullptr, g_br_hover = nullptr,
              g_br_pressed = nullptr, g_br_checked = nullptr,
              g_br_border = nullptr, g_br_flash_on = nullptr,
              g_br_track = nullptr, g_br_thumb = nullptr,
              g_br_thumb_hot = nullptr;
static HPEN g_pen_border = nullptr, g_pen_focus = nullptr, g_pen_track = nullptr,
            g_pen_thumb = nullptr;
static bool g_beep_button_checked = false, g_localbeep_button_checked = false;

static void init_dark_resources() {
    if (g_br_bg) return;
    g_br_bg       = CreateSolidBrush(CLR_BG);
    g_br_surface  = CreateSolidBrush(CLR_SURFACE);
    g_br_hover    = CreateSolidBrush(CLR_HOVER);
    g_br_pressed  = CreateSolidBrush(CLR_PRESSED);
    g_br_checked  = CreateSolidBrush(CLR_CHECKED);
    g_br_border   = CreateSolidBrush(CLR_BORDER);
    g_br_flash_on = CreateSolidBrush(CLR_FLASH_ON);
    g_br_track    = CreateSolidBrush(CLR_TRACK);
    g_br_thumb    = CreateSolidBrush(CLR_THUMB);
    g_br_thumb_hot = CreateSolidBrush(CLR_THUMB_HOT);
    g_pen_border  = CreatePen(PS_SOLID, 1, CLR_BORDER);
    g_pen_focus   = CreatePen(PS_SOLID, 1, CLR_FOCUS);
    g_pen_track   = CreatePen(PS_SOLID, 2, CLR_TRACK);
    g_pen_thumb   = CreatePen(PS_SOLID, 1, CLR_THUMB);
}

static void free_dark_resources() {
    for (HGDIOBJ o : { (HGDIOBJ)g_pen_thumb, (HGDIOBJ)g_pen_track,
                       (HGDIOBJ)g_pen_focus,
                       (HGDIOBJ)g_pen_border,
                       (HGDIOBJ)g_br_thumb_hot, (HGDIOBJ)g_br_thumb,
                       (HGDIOBJ)g_br_track,
                       (HGDIOBJ)g_br_flash_on, (HGDIOBJ)g_br_border,
                       (HGDIOBJ)g_br_checked, (HGDIOBJ)g_br_pressed,
                       (HGDIOBJ)g_br_hover, (HGDIOBJ)g_br_surface,
                       (HGDIOBJ)g_br_bg, (HGDIOBJ)g_font_big,
                       (HGDIOBJ)g_font })
        if (o) DeleteObject(o);
    g_pen_thumb = g_pen_track = g_pen_focus = g_pen_border = nullptr;
    g_br_thumb_hot = g_br_thumb = g_br_track = nullptr;
    g_br_flash_on = g_br_border = nullptr;
    g_br_checked = g_br_pressed = nullptr;
    g_br_hover = g_br_surface = g_br_bg = nullptr;
    g_font_big = g_font = nullptr;
}

static void apply_dark_titlebar(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *PDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
    auto set_attr = (PDwmSetWindowAttribute)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (set_attr) {
        BOOL dark = TRUE;
        if (FAILED(set_attr(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                            &dark, sizeof(dark))))
            set_attr(hwnd, 19 /*older Windows 10 value*/, &dark, sizeof(dark));
        COLORREF border = CLR_BORDER, caption = CLR_BG, text = CLR_TEXT;
        set_attr(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
        set_attr(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
        set_attr(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
    }
    FreeLibrary(dwm);
}

static void apply_control_dark_theme(HWND hwnd, const wchar_t* theme = L"DarkMode_Explorer") {
    typedef HRESULT (WINAPI *PSetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    static PSetWindowTheme set_theme = ux
        ? (PSetWindowTheme)(void*)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (set_theme) set_theme(hwnd, theme, nullptr); // custom drawing remains fallback
}

static bool move_tab_focus(HWND current, bool reverse) {
    HWND parent = GetParent(current);
    std::vector<HWND> tabs;
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        LONG_PTR style = GetWindowLongPtrW(c, GWL_STYLE);
        if ((style & WS_TABSTOP) && IsWindowVisible(c) && IsWindowEnabled(c))
            tabs.push_back(c);
    }
    if (tabs.empty()) return false;
    size_t at = 0;
    while (at < tabs.size() && tabs[at] != current) ++at;
    if (at == tabs.size()) at = 0;
    else if (reverse) at = (at + tabs.size() - 1) % tabs.size();
    else at = (at + 1) % tabs.size();
    SetFocus(tabs[at]);
    return true;
}

static LRESULT CALLBACK dark_button_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                              UINT_PTR id, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_TAB &&
        move_tab_focus(hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0))
        return 0;
    if (msg == WM_MOUSEMOVE && !GetPropW(hwnd, L"LowCastDarkHot")) {
        SetPropW(hwnd, L"LowCastDarkHot", (HANDLE)1);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE) {
        RemovePropW(hwnd, L"LowCastDarkHot");
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE) {
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"LowCastDarkHot");
        RemoveWindowSubclass(hwnd, dark_button_subclass, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void apply_dark_button(HWND hwnd) {
    apply_control_dark_theme(hwnd);
    SetWindowSubclass(hwnd, dark_button_subclass, 2, 0);
}

static void draw_focus_ring(HDC dc, RECT rc);
static bool draw_dark_button_item(const DRAWITEMSTRUCT* dis);

static void paint_dark_combo(HWND hwnd, HDC dc) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    bool disabled = !IsWindowEnabled(hwnd);
    bool focused = GetFocus() == hwnd;
    bool hot = GetPropW(hwnd, L"LowCastComboHot") != nullptr;
    bool dropped = SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
    FillRect(dc, &rc, disabled ? g_br_bg : g_br_surface);

    int arrow_w = GetSystemMetrics(SM_CXVSCROLL);
    if (arrow_w < 18) arrow_w = 18;
    RECT arrow{ rc.right - arrow_w, rc.top, rc.right, rc.bottom };
    FillRect(dc, &arrow, disabled ? g_br_bg : dropped ? g_br_pressed :
             hot ? g_br_hover : g_br_surface);
    RECT divider{ arrow.left, rc.top + 2, arrow.left + 1, rc.bottom - 2 };
    FillRect(dc, &divider, g_br_border);

    wchar_t text[512] = L"";
    int sel = (int)SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
    if (sel >= 0) SendMessageW(hwnd, CB_GETLBTEXT, sel, (LPARAM)text);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? CLR_DISABLED : CLR_TEXT);
    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    RECT tr{ rc.left + 7, rc.top, arrow.left - 5, rc.bottom };
    DrawTextW(dc, text, -1, &tr,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (old_font) SelectObject(dc, old_font);

    int cx = (arrow.left + arrow.right) / 2;
    int cy = (arrow.top + arrow.bottom) / 2;
    HGDIOBJ old_pen = SelectObject(dc, g_pen_track);
    MoveToEx(dc, cx - 4, cy - 2, nullptr);
    LineTo(dc, cx, cy + 2);
    LineTo(dc, cx + 4, cy - 2);
    SelectObject(dc, old_pen);
    FrameRect(dc, &rc, focused ? g_br_checked : g_br_border);
    if (focused && !disabled) draw_focus_ring(dc, rc);
}

static LRESULT CALLBACK dark_combo_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                             UINT_PTR id, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_TAB &&
        move_tab_focus(hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0))
        return 0;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        paint_dark_combo(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_PRINTCLIENT) {
        paint_dark_combo(hwnd, (HDC)wp);
        return 0;
    }
    if (msg == WM_MOUSEMOVE && !GetPropW(hwnd, L"LowCastComboHot")) {
        SetPropW(hwnd, L"LowCastComboHot", (HANDLE)1);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE) {
        RemovePropW(hwnd, L"LowCastComboHot");
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE ||
               msg == CB_SHOWDROPDOWN || msg == CB_SETCURSEL) {
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"LowCastComboHot");
        RemoveWindowSubclass(hwnd, dark_combo_subclass, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void apply_dark_combo(HWND hwnd) {
    apply_control_dark_theme(hwnd, L"DarkMode_CFD");
    SetWindowSubclass(hwnd, dark_combo_subclass, 3, 0);
}

static std::vector<HWND> g_render_buttons;   // owned by the UI thread only
static std::vector<Renderer> g_dlna_button_targets; // stable identities for posted clicks
static std::vector<AirplayDev> g_airplay_button_targets;

// AirPlay combo mappings — shared by the UI handlers and settings load/save
static const int g_ap_lats[10] = { 25, 30, 40, 50, 75, 100, 150, 250, 275, 350 };

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
    {   int lsel = (int)SendMessageW(G.cmb_aplat, CB_GETCURSEL, 0, 0);
        put(L"aplatms", (lsel >= 0 && lsel < 10) ? g_ap_lats[lsel] : 150);  // by VALUE
    }
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
    int lms = geti(L"aplatms", -1);
    if (lms < 0) {
        // migrate pre-value-format saves: old key stored the dropdown INDEX
        // into the original 7-entry table. Honor it so selections survive.
        static const int old_lats[7] = { 25, 50, 75, 100, 150, 250, 350 };
        int oidx = geti(L"aplat", -1);
        lms = (oidx >= 0 && oidx < 7) ? old_lats[oidx] : 150;
    }
    for (int i = 0; i < 10; ++i)
        if (g_ap_lats[i] == lms) {
            SendMessageW(G.cmb_aplat, CB_SETCURSEL, i, 0);
            G.ap_latency_ms.store(lms);
            break;
        }
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
    g_dlna_button_targets.clear();
    g_airplay_button_targets.clear();
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
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 12, y, 560, 34, hwnd,
                                 (HMENU)(uintptr_t)(IDC_RENDER_BASE + i),
                                 nullptr, nullptr);
        SendMessageW(r.button, WM_SETFONT, (WPARAM)g_font_big, TRUE);
        apply_dark_button(r.button);
        g_render_buttons.push_back(r.button);
        g_dlna_button_targets.push_back(r);
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
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 12, y, 560, 34, hwnd,
                                 (HMENU)(uintptr_t)(IDC_RENDER_BASE + 1000 + i),
                                 nullptr, nullptr);
        SendMessageW(d.button, WM_SETFONT, (WPARAM)g_font_big, TRUE);
        apply_dark_button(d.button);
        g_render_buttons.push_back(d.button);
        g_airplay_button_targets.push_back(d);
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
    bool running = raop_is_running();
    if (!running) {
        // A handshake can fail between two 250 ms UI ticks. Clear stale
        // optimistic flags on every no-session tick so the next click starts.
        bool reset = false;
        EnterCriticalSection(&G.cs);
        for (auto& d : g_airplay)
            if (d.streaming) { d.streaming = false; reset = true; }
        LeaveCriticalSection(&G.cs);
        if (reset) ui_log(L"[raop] session ended \x2014 button reset");
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
}

// ---------------------------------------------------------------------------
// Headset battery, piggybacked on the PC's Bluetooth pairing. Windows reads
// it over BT (Settings shows the %) and publishes it in the PnP device tree
// as DEVPKEY_Bluetooth_Battery on the BTHENUM/BTHLE node. cfgmgr32 is
// dynamically loaded; no new link dependency. CAVEAT shown to the user via
// naming: the value is only FRESH while the BT link is connected — if the
// headset drops Bluetooth in WiFi mode, this is "last known", not live.
// ---------------------------------------------------------------------------
static std::atomic<int> g_hp_batt{-1};

static void poll_bt_battery() {
    typedef DWORD (WINAPI *PSizeW)(PULONG, PCWSTR, ULONG);
    typedef DWORD (WINAPI *PListW)(PCWSTR, PWCHAR, ULONG, ULONG);
    typedef DWORD (WINAPI *PLocate)(DWORD*, PWCHAR, ULONG);
    typedef DWORD (WINAPI *PGetProp)(DWORD, const void*, PULONG, PBYTE, PULONG, ULONG);
    static PSizeW pSize; static PListW pList; static PLocate pLoc; static PGetProp pProp;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"cfgmgr32.dll");
        if (m) {
            pSize = (PSizeW)GetProcAddress(m, "CM_Get_Device_ID_List_SizeW");
            pList = (PListW)GetProcAddress(m, "CM_Get_Device_ID_ListW");
            pLoc  = (PLocate)GetProcAddress(m, "CM_Locate_DevNodeW");
            pProp = (PGetProp)GetProcAddress(m, "CM_Get_DevNode_PropertyW");
        }
    }
    if (!pSize || !pList || !pLoc || !pProp) return;
    struct PK { GUID g; ULONG pid; };
    static const PK KEY_BATT = { {0x104EA319,0x6EE2,0x4701,
                                  {0xBD,0x47,0x8D,0xDB,0xF4,0x25,0xBB,0xE5}}, 2 };
    static const PK KEY_NAME = { {0xA45C254E,0xDF1C,0x4EFD,
                                  {0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}}, 14 };
    const wchar_t* enums[3] = { L"BTHENUM", L"BTHLE", L"BTHLEDevice" };
    int best = -1;
    wchar_t best_name[256] = L"";
    bool best_is_he = false;
    for (int e = 0; e < 3; ++e) {
        ULONG len = 0;
        if (pSize(&len, enums[e], 0x1 /*FILTER_ENUMERATOR*/) != 0 || len < 2) continue;
        std::vector<wchar_t> ids(len);
        if (pList(enums[e], ids.data(), len, 0x1) != 0) continue;
        for (wchar_t* p = ids.data(); *p; p += wcslen(p) + 1) {
            DWORD dn = 0;
            if (pLoc(&dn, p, 0) != 0) continue;
            BYTE bv = 0; ULONG sz = sizeof(bv), ty = 0;
            if (pProp(dn, &KEY_BATT, &ty, &bv, &sz, 0) != 0 || bv > 100) continue;
            static const PK KEY_NAME2 = { {0xB725F130,0x47EF,0x101A,
                                           {0xA5,0xF1,0x02,0x60,0x8C,0x9E,0xEB,0xAC}}, 10 };
            static const PK KEY_DESC  = { {0xA45C254E,0xDF1C,0x4EFD,
                                           {0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}}, 2 };
            wchar_t nm[256] = L""; sz = sizeof(nm); ty = 0;
            pProp(dn, &KEY_NAME, &ty, (PBYTE)nm, &sz, 0);
            nm[255] = 0;
            if (wcslen(nm) < 3) {                 // stub/absent FriendlyName:
                sz = sizeof(nm); ty = 0;          // fall back through NAME then
                pProp(dn, &KEY_NAME2, &ty, (PBYTE)nm, &sz, 0);   // DeviceDesc
                nm[255] = 0;
            }
            if (wcslen(nm) < 3) {
                sz = sizeof(nm); ty = 0;
                pProp(dn, &KEY_DESC, &ty, (PBYTE)nm, &sz, 0);
                nm[255] = 0;
            }
            bool is_he = wcsstr(nm, L"HE1000") || wcsstr(nm, L"HIFIMAN") ||
                         wcsstr(nm, L"HiFiMAN") || wcsstr(nm, L"Hifiman");
            if (best < 0 || (is_he && !best_is_he)) {
                best = bv; best_is_he = is_he;
                wcsncpy(best_name, nm, 255); best_name[255] = 0;
            }
        }
    }
    int prev = g_hp_batt.load();
    if (best >= 0) prev = g_hp_batt.exchange(best); // retain the last known value
    if (best >= 0 && prev < 0) {                  // announce the source ONCE so
        wchar_t lb[320];                          // a wrong device pick is visible
        swprintf(lb, 320, L"[batt] %ls battery: %d%%",
                 best_name[0] ? best_name : L"headset", best);
        ui_log(lb);
    }
    static bool said_none = false;
    if (best < 0 && prev < 0 && !said_none) {     // the NEGATIVE is logged too:
        said_none = true;                         // "clipped" vs "not found" must
        ui_log(L"[batt] no Bluetooth battery property found \x2014 "
               L"is the headset's Bluetooth link connected?");
    }
}

// ---- persistent low-battery application alert (40%, 20%, 10%) -------------
// The worker publishes readings only. The UI timer owns this state and every
// HWND operation, so a launch-time reading cannot spend an alert before the UI
// exists. A failed create/update stays pending and is retried.
static HICON g_app_icon = nullptr;
static HICON g_app_icon_small = nullptr;
enum class BatteryAlertPresentResult { Failed, Created, Updated };
struct BatteryAlertState {
    int delivered_level = 0;       // 0, 1=40%, 2=20%, 3=10% this charge cycle
    int visible_level = 0;         // only Acknowledge returns this to zero
    int retry_level = 0;
    uint64_t retry_after_ms = 0;
    HWND hwnd = nullptr;
    HWND message = nullptr;
    HWND acknowledge = nullptr;
    bool shutting_down = false;
};
static BatteryAlertState g_battery_alert;
static const wchar_t* BATTERY_ALERT_CLASS = L"LowCastBatteryAlertWnd";
static const uint64_t BATTERY_ALERT_RETRY_MS = 5000;

static int battery_alert_level(int pct) {
    if (pct < 0 || pct > 100) return 0;
    if (pct <= 10) return 3;
    if (pct <= 20) return 2;
    if (pct <= 40) return 1;
    return 0;
}

static int battery_alert_threshold(int level) {
    return level == 3 ? 10 : level == 2 ? 20 : level == 1 ? 40 : 0;
}

static const wchar_t* battery_alert_severity(int level) {
    return level == 3 ? L"URGENT" : level == 2 ? L"CRITICAL" : L"LOW";
}

static int battery_alert_candidate(BatteryAlertState& state, int pct,
                                   uint64_t now) {
    if (pct < 0 || pct > 100) return 0;
    if (pct >= 80) {
        state.delivered_level = 0;
        state.retry_level = 0;
        state.retry_after_ms = 0;
        return 0;                   // an existing alert still awaits its button
    }
    int level = battery_alert_level(pct);
    if (!level || level <= state.delivered_level) return 0;
    // A more urgent threshold bypasses an older failed-delivery backoff.
    if (level <= state.retry_level && now < state.retry_after_ms) return 0;
    return level;
}

static void battery_alert_text(int level, int pct, wchar_t* title,
                               size_t title_count, wchar_t* message,
                               size_t message_count) {
    swprintf(title, title_count, L"LowCast \x2014 Headset battery %ls",
             battery_alert_severity(level));
    swprintf(message, message_count,
             L"Headset battery: %d%% remaining\r\n\r\n"
             L"Charge the headset %ls. This alert stays open until you "
             L"acknowledge it.",
             pct, level >= 2 ? L"now" : L"soon");
}

static LRESULT CALLBACK battery_alert_wndproc(HWND h, UINT m, WPARAM wp,
                                               LPARAM lp) {
    BatteryAlertState* state =
        reinterpret_cast<BatteryAlertState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = reinterpret_cast<BatteryAlertState*>(create->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (m) {
    case WM_CREATE: {
        if (!state) return -1;
        state->message = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            20, 18, 390, 78, h, nullptr, nullptr, nullptr);
        state->acknowledge = CreateWindowW(L"BUTTON", L"Acknowledge",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            274, 105, 136, 32, h, (HMENU)(uintptr_t)IDC_BATTERY_ACK,
            nullptr, nullptr);
        if (!state->message || !state->acknowledge) return -1;
        HFONT font = g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(state->message, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(state->acknowledge, WM_SETFONT, (WPARAM)font, TRUE);
        apply_dark_button(state->acknowledge);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc{}; GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, g_br_bg ? g_br_bg : GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, g_br_bg ? CLR_TEXT : GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)(g_br_bg ? g_br_bg : GetSysColorBrush(COLOR_WINDOW));
    case WM_DRAWITEM:
        if (draw_dark_button_item(reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
        break;
    case WM_COMMAND:
        if (state && LOWORD(wp) == IDC_BATTERY_ACK && HIWORD(wp) == BN_CLICKED) {
            wchar_t line[128];
            swprintf(line, 128,
                     L"[batt] application alert acknowledged at %d%% threshold",
                     battery_alert_threshold(state->visible_level));
            ui_log(line);
            state->visible_level = 0;
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        // X/Alt+F4 are not acknowledgment. Shutdown uses the helper below.
        if (state && state->shutting_down) DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        if (state) {
            state->hwnd = nullptr;
            state->message = nullptr;
            state->acknowledge = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static bool battery_alert_register_class() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = battery_alert_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = BATTERY_ALERT_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_app_icon_small ? g_app_icon_small : LoadIconW(nullptr, IDI_WARNING);
    wc.hbrBackground = nullptr;
    if (RegisterClassW(&wc)) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

struct BatteryAlertWindowOptions { bool show = true; };
static BatteryAlertPresentResult battery_alert_present_window(
    BatteryAlertState& state, int level, int pct, void* context) {
    BatteryAlertWindowOptions* options =
        reinterpret_cast<BatteryAlertWindowOptions*>(context);
    bool show = !options || options->show;
    bool created = false;
    if (state.hwnd && !IsWindow(state.hwnd)) state.hwnd = nullptr;
    if (!state.hwnd) {
        if (!battery_alert_register_class()) return BatteryAlertPresentResult::Failed;
        state.shutting_down = false;
        state.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            BATTERY_ALERT_CLASS, L"LowCast \x2014 Headset battery",
            WS_POPUP | WS_CAPTION,
            0, 0, 440, 184, nullptr, nullptr, GetModuleHandleW(nullptr), &state);
        if (!state.hwnd) return BatteryAlertPresentResult::Failed;
        apply_dark_titlebar(state.hwnd);
        created = true;
    }
    wchar_t title[96], message[320];
    battery_alert_text(level, pct, title, 96, message, 320);
    if (!SetWindowTextW(state.hwnd, title) ||
        !SetWindowTextW(state.message, message)) {
        if (created) {
            state.shutting_down = true;
            DestroyWindow(state.hwnd);
            state.shutting_down = false;
        }
        return BatteryAlertPresentResult::Failed;
    }
    if (show) {
        RECT work{};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) ||
            work.right <= work.left || work.bottom <= work.top) {
            work.left = work.top = 0;
            work.right = GetSystemMetrics(SM_CXSCREEN);
            work.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        int x = work.right - 440 - 24;
        int y = work.bottom - 184 - 24;
        if (!SetWindowPos(state.hwnd, HWND_TOPMOST, x, y, 440, 184,
                          SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
            if (created) {
                state.shutting_down = true;
                DestroyWindow(state.hwnd);
                state.shutting_down = false;
            }
            return BatteryAlertPresentResult::Failed;
        }
    }
    return created ? BatteryAlertPresentResult::Created
                   : BatteryAlertPresentResult::Updated;
}

typedef BatteryAlertPresentResult (*BatteryAlertPresenter)(
    BatteryAlertState&, int, int, void*);
static bool battery_alert_process_reading(BatteryAlertState& state, int pct,
                                          uint64_t now,
                                          BatteryAlertPresenter presenter,
                                          void* context) {
    if (state.shutting_down) return false;
    int level = battery_alert_candidate(state, pct, now);
    if (!level) return false;
    BatteryAlertPresentResult result = presenter(state, level, pct, context);
    if (result == BatteryAlertPresentResult::Failed) {
        state.retry_level = level;
        state.retry_after_ms = now + BATTERY_ALERT_RETRY_MS;
        wchar_t line[160];
        swprintf(line, 160,
                 L"[batt] application alert create/update failed for %d%% "
                 L"threshold; retrying in 5 seconds",
                 battery_alert_threshold(level));
        ui_log(line);
        return false;
    }
    state.delivered_level = level;
    state.visible_level = level;
    state.retry_level = 0;
    state.retry_after_ms = 0;
    wchar_t line[192];
    swprintf(line, 192,
             L"[batt] application alert %ls for %d%% threshold (reading %d%%); "
             L"awaiting acknowledgment",
             result == BatteryAlertPresentResult::Created ? L"created" : L"updated",
             battery_alert_threshold(level), pct);
    ui_log(line);
    return true;
}

static void battery_alert_shutdown(BatteryAlertState& state) {
    state.shutting_down = true;
    if (state.hwnd && IsWindow(state.hwnd)) DestroyWindow(state.hwnd);
    state.hwnd = nullptr;
    state.message = nullptr;
    state.acknowledge = nullptr;
    state.visible_level = 0;
}

static DWORD WINAPI battery_thread(LPVOID) {
    int ticks = 0, last_logged = -2;
    for (;;) {
        poll_bt_battery();
        int cur = g_hp_batt.load();
        if (last_logged == -2) last_logged = cur;   // first value is announced by
                                                    // poll_bt_battery; arm change
                                                    // logging now, not after 1 h
        bool hourly = (++ticks % 120) == 0;       // 120 x 30s = 1 h heartbeat
        if (cur != last_logged) {
            // log on any VALUE CHANGE and (below) on the hourly mark: builds the
            // battery timeline needed to correlate late-night radio deaths
            // with charge state (and exposes a stale never-changing property).
            // A -1 -> valid transition was just announced by poll_bt_battery.
            if (!(last_logged < 0 && cur >= 0)) {
                wchar_t bl[64];
                if (cur >= 0) swprintf(bl, 64, L"[batt] headset battery: %d%%", cur);
                else          swprintf(bl, 64, L"[batt] headset battery: unavailable");
                ui_log(bl);
            }
            last_logged = cur;
        } else if (hourly) {
            wchar_t bl[64];
            if (cur >= 0) swprintf(bl, 64, L"[batt] headset battery: %d%% (hourly)", cur);
            else          swprintf(bl, 64, L"[batt] headset battery: unavailable (hourly)");
            ui_log(bl);
        }
        Sleep(30000);
    }
}

static void update_stats() {
    battery_alert_process_reading(g_battery_alert, g_hp_batt.load(), now_ms(),
                                  battery_alert_present_window, nullptr);
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
        int fl = raop_suggested_floor_ms();
        wchar_t flb[32];
        if (fl < 0) swprintf(flb, 32, L"measuring\x2026");
        else        swprintf(flb, 32, L"\x2248%dms", fl);
        swprintf(b, 512,
            L"AirPlay LIVE: %.1f s of audio sent   heartbeat: %ls%.1fs ago   "
            L"resends: %llu   reconnects: %u   observed sender cushion %ls\r\n"
            L"DLNA: %u Hz / %d-bit / %ls   connections: %d   sent: %.1f MB\r\n"
            L"Sender counters cannot confirm receiver playback. "
            L"If silent, check the session log or restart the stream.",
            secs_sent, hb_age < 0 ? L"NONE " : L"", hb_age < 0 ? 0.0 : hb_age,
            rs, rc, flb,
            rate, fmt_bits(f), fmt_is_wav(f) ? L"WAV" : L"LPCM", nclients,
            sent / 1048576.0);
    } else {
        swprintf(b, 512,
            L"Stream: %u Hz / %d-bit / %ls  (%.1f Mbit/s)      Renderer connections: %d\r\n"
            L"Sender-side latency (capture\x2192network): %.1f ms      Sent: %.1f MB\r\n"
            L"End-to-end = sender + headphone firmware buffer \x2192 use the beep test",
            rate, fmt_bits(f), fmt_is_wav(f) ? L"WAV" : L"LPCM", mbit, nclients,
            backlog10 / 10.0 + 3.0 /*capture poll*/, sent / 1048576.0);
    }
    SetWindowTextW(G.stat, b);
    {
        int hpb = g_hp_batt.load();
        wchar_t bt[24];
        int level = battery_alert_level(hpb);
        if (level == 3) swprintf(bt, 24, L"URGENT: %d%%", hpb);
        else if (level == 2) swprintf(bt, 24, L"CRIT: %d%%", hpb);
        else if (level == 1) swprintf(bt, 24, L"LOW: %d%%", hpb);
        else if (hpb >= 0) swprintf(bt, 24, L"bat: %d%%", hpb);
        else               swprintf(bt, 24, L"bat: \x2014");
        SetWindowTextW(G.lbl_batt, bt);
    }
}

// Trackbar subclass: stock trackbars treat a click on the channel as a PAGE
// step toward the click; we want click-anywhere-to-grab. On button-down the
// thumb is moved to the clicked position first, then the SAME message is
// handed to the default proc — which now finds the thumb under the cursor
// and begins a native drag, so the motion continues while the mouse is held.
static void paint_dark_volume_control(HWND hw, HDC dc) {
    RECT bounds{}, thumb{};
    GetClientRect(hw, &bounds); // NMCUSTOMDRAW.rc can be only the invalid region
    SendMessageW(hw, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    bool hot = GetPropW(hw, L"LowCastVolumeHot") != nullptr;
    bool pressed = GetCapture() == hw && (GetKeyState(VK_LBUTTON) & 0x8000);
    HBRUSH thumb_fill = pressed ? g_br_track : hot ? g_br_thumb_hot : g_br_thumb;
    paint_lowcast_volume_track(dc, bounds, thumb, g_br_bg, g_br_thumb,
                               thumb_fill, g_br_track, g_pen_thumb,
                               GetFocus() == hw);
}

static LRESULT CALLBACK apvol_subclass(HWND hw, UINT m, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hw, &ps);
        paint_dark_volume_control(hw, dc);
        EndPaint(hw, &ps);
        return 0;
    }
    if (m == WM_PRINTCLIENT) {
        paint_dark_volume_control(hw, (HDC)wp);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_KEYDOWN && wp == VK_TAB &&
        move_tab_focus(hw, (GetKeyState(VK_SHIFT) & 0x8000) != 0))
        return 0;
    if (m == WM_MOUSEMOVE && !GetPropW(hw, L"LowCastVolumeHot")) {
        SetPropW(hw, L"LowCastVolumeHot", (HANDLE)1);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hw, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(hw, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        RemovePropW(hw, L"LowCastVolumeHot");
        InvalidateRect(hw, nullptr, FALSE);
    }
    if (m == WM_LBUTTONDOWN) {
        RECT ch{}, th{};
        SendMessageW(hw, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
        SendMessageW(hw, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        int pos = lowcast_vertical_volume_pos(ch, th, (int)(short)HIWORD(lp));
        if (pos >= 0) {
            SendMessageW(hw, TBM_SETPOS, TRUE, pos);
            SendMessageW(GetParent(hw), WM_VSCROLL,
                         MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM)hw);
        }
        // The painted thumb is centered, while the native hit-test remains a
        // few pixels left. Forward x inside the native thumb so a press on the
        // visible edge or margin begins a real drag; vertical dragging uses y.
        SendMessageW(hw, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        int x = (int)(short)LOWORD(lp);
        if (th.right > th.left) {
            if (x < th.left) x = th.left;
            if (x >= th.right) x = th.right - 1;
        }
        LRESULT result = DefSubclassProc(hw, m, wp, MAKELPARAM(x, HIWORD(lp)));
        InvalidateRect(hw, nullptr, FALSE);
        return result;
    }
    if (m == WM_LBUTTONUP || m == WM_SETFOCUS || m == WM_KILLFOCUS ||
        m == WM_ENABLE) {
        LRESULT result = DefSubclassProc(hw, m, wp, lp);
        InvalidateRect(hw, nullptr, FALSE);
        return result;
    }
    if (m == WM_NCDESTROY) {
        RemovePropW(hw, L"LowCastVolumeHot");
        RemoveWindowSubclass(hw, apvol_subclass, id);
    }
    return DefSubclassProc(hw, m, wp, lp);
}

static void draw_focus_ring(HDC dc, RECT rc) {
    InflateRect(&rc, -3, -3);
    HGDIOBJ old_pen = SelectObject(dc, g_pen_focus);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
}

static bool draw_dark_combo_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_COMBOBOX) return false;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    FillRect(dis->hDC, &dis->rcItem, selected ? g_br_checked : g_br_surface);
    wchar_t text[512] = L"";
    int item = dis->itemID == (UINT)-1
        ? (int)SendMessageW(dis->hwndItem, CB_GETCURSEL, 0, 0)
        : (int)dis->itemID;
    if (item >= 0) SendMessageW(dis->hwndItem, CB_GETLBTEXT, item, (LPARAM)text);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, disabled ? CLR_DISABLED : CLR_TEXT);
    HFONT font = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ old_font = font ? SelectObject(dis->hDC, font) : nullptr;
    RECT tr = dis->rcItem; tr.left += 7; tr.right -= 5;
    DrawTextW(dis->hDC, text, -1, &tr,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (old_font) SelectObject(dis->hDC, old_font);
    if (dis->itemState & ODS_FOCUS) draw_focus_ring(dis->hDC, dis->rcItem);
    return true;
}

static bool draw_dark_button_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_BUTTON) return false;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;
    bool hot = GetPropW(dis->hwndItem, L"LowCastDarkHot") != nullptr;
    bool checked = dis->hwndItem == G.btn_beep ? g_beep_button_checked :
                   dis->hwndItem == G.btn_localbeep ? g_localbeep_button_checked :
                   SendMessageW(dis->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
    HBRUSH fill = disabled ? g_br_bg : pressed ? g_br_pressed :
                  checked ? g_br_checked : hot ? g_br_hover : g_br_surface;
    FillRect(dis->hDC, &dis->rcItem, fill);
    FrameRect(dis->hDC, &dis->rcItem, (checked || focused) ? g_br_checked : g_br_border);

    wchar_t text[512] = L"";
    GetWindowTextW(dis->hwndItem, text, 512);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, disabled ? CLR_DISABLED : CLR_TEXT);
    HFONT font = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ old_font = font ? SelectObject(dis->hDC, font) : nullptr;
    RECT tr = dis->rcItem; InflateRect(&tr, -8, -2);
    if (pressed) OffsetRect(&tr, 1, 1);
    DrawTextW(dis->hDC, text, -1, &tr,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (old_font) SelectObject(dis->hDC, old_font);
    if (checked) {
        int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
        RECT mark{ dis->rcItem.left + 9, cy - 5, dis->rcItem.left + 19, cy + 5 };
        FrameRect(dis->hDC, &mark, g_br_track);
        HGDIOBJ old_pen = SelectObject(dis->hDC, g_pen_focus);
        MoveToEx(dis->hDC, mark.left + 2, cy, nullptr);
        LineTo(dis->hDC, mark.left + 4, cy + 2);
        LineTo(dis->hDC, mark.right - 2, cy - 3);
        SelectObject(dis->hDC, old_pen);
    }
    if (focused && !disabled) draw_focus_ring(dis->hDC, dis->rcItem);
    return true;
}

// EM_REPLACESEL stops silently at the edit limit. Drop the oldest half at a
// complete line before appending so current diagnostics and alerts stay visible.
static void append_log_line(HWND edit, const std::wstring& text) {
    std::wstring line = text + L"\r\n";
    int len = GetWindowTextLengthW(edit);
    int limit = (int)SendMessageW(edit, EM_GETLIMITTEXT, 0, 0);
    if (len + (int)line.size() > limit) {
        std::wstring all((size_t)len + 1, L'\0');
        all.resize((size_t)GetWindowTextW(edit, &all[0], len + 1));
        size_t cut = all.find(L"\r\n", all.size() / 2);
        cut = cut == std::wstring::npos ? all.size() : cut + 2;
        SendMessageW(edit, EM_SETSEL, 0, (LPARAM)cut);
        SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(edit);
    }
    SendMessageW(edit, EM_SETSEL, len, len);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        G.hwnd = h;
        if (!g_ui_preview) g_log_hwnd.store(h);
        init_dark_resources();
        g_font = CreateFontW(-15, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
                             0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_big = CreateFontW(-16, 0,0,0, FW_SEMIBOLD, 0,0,0, DEFAULT_CHARSET,
                             0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        HWND lbl1 = CreateWindowW(L"STATIC", L"Capture source (all system audio):",
                      WS_CHILD | WS_VISIBLE, 12, 10, 300, 18, h, nullptr, nullptr, nullptr);
        G.cmb_dev = CreateWindowW(L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                      CBS_HASSTRINGS | WS_VSCROLL,
                      12, 30, 350, 300, h, (HMENU)1, nullptr, nullptr);
        HWND lbl2 = CreateWindowW(L"STATIC", L"Audio type:",
                      WS_CHILD | WS_VISIBLE, 12, 66, 90, 18, h, nullptr, nullptr, nullptr);
        G.cmb_fmt = CreateWindowW(L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                      CBS_HASSTRINGS,
                      100, 62, 262, 200, h, (HMENU)2, nullptr, nullptr);
        G.btn_scan = CreateWindowW(L"BUTTON", L"Rescan",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                      372, 30, 118, 26, h, (HMENU)3, nullptr, nullptr);
        G.btn_beep = CreateWindowW(L"BUTTON", L"Latency beep test",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                      372, 62, 118, 26, h, (HMENU)4, nullptr, nullptr);
        HWND lbl3 = CreateWindowW(L"STATIC", L"Rate:",
                      WS_CHILD | WS_VISIBLE, 12, 98, 90, 18, h, nullptr, nullptr, nullptr);
        SendMessageW(lbl3, WM_SETFONT, (WPARAM)0, TRUE);
        G.cmb_rate = CreateWindowW(L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                      CBS_HASSTRINGS,
                      100, 94, 262, 200, h, (HMENU)6, nullptr, nullptr);
        G.btn_localbeep = CreateWindowW(L"BUTTON", L"Beep via PC out",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                      372, 94, 118, 26, h, (HMENU)7, nullptr, nullptr);
        HWND lbl4 = CreateWindowW(L"STATIC", L"AirPlay buffer:",
                      WS_CHILD | WS_VISIBLE, 12, 130, 96, 18, h, nullptr, nullptr, nullptr);
        SendMessageW(lbl4, WM_SETFONT, (WPARAM)g_font, TRUE);
        G.cmb_aplat = CreateWindowW(L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                      CBS_HASSTRINGS | WS_VSCROLL,
                      110, 126, 380, 336, h, (HMENU)8, nullptr, nullptr);
                      // 336: droplist tall enough for all 9 presets; VSCROLL
                      // as a safety net so future entries can never be
                      // scrolled out of existence again
        SendMessageW(G.cmb_aplat, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"25 ms - floor probe (loss-repair verified)");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"30 ms - very tight");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"40 ms - tight");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"50 ms - insane");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"75 ms - extreme");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"100 ms - aggressive");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"150 ms - fast");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"250 ms - recommended");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"275 ms - parity (old build's 25 ms, true-latency twin)");
        SendMessageW(G.cmb_aplat, CB_ADDSTRING, 0, (LPARAM)L"350 ms - safe");
        SendMessageW(G.cmb_aplat, CB_SETCURSEL, 6, 0);   // default 150 ms
        // vertical volume column, right of the Rescan / beep buttons.
        // NOTE: vertical trackbars put position 0 at the TOP and report via
        // WM_VSCROLL, so displayed percent = 100 - position (up = louder).
        HWND lbl5 = CreateWindowW(L"STATIC", L"Vol",
                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                      500, 10, 56, 16, h, nullptr, nullptr, nullptr);
        SendMessageW(lbl5, WM_SETFONT, (WPARAM)g_font, TRUE);
        G.sld_apvol = CreateWindowW(TRACKBAR_CLASSW, nullptr,
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_VERT | TBS_NOTICKS,
                      512, 28, 32, 96, h, (HMENU)9, nullptr, nullptr);
        SendMessageW(G.sld_apvol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(G.sld_apvol, TBM_SETPOS, TRUE, 0);   // top = 100% / auto
        SetWindowSubclass(G.sld_apvol, apvol_subclass, 1, 0);  // click-anywhere grab
        G.lbl_apvol = CreateWindowW(L"STATIC", L"auto",
                      WS_CHILD | WS_VISIBLE | SS_CENTER, 500, 128, 56, 18,
                      h, nullptr, nullptr, nullptr);
        SendMessageW(G.lbl_apvol, WM_SETFONT, (WPARAM)g_font, TRUE);
        G.lbl_batt = CreateWindowW(L"STATIC", L"",     // battery: own spot, no
                      WS_CHILD | WS_VISIBLE | SS_CENTER, 486, 148, 84, 16,
                      h, nullptr, nullptr, nullptr);   // 84px: "bat: 100%" fits
        SendMessageW(G.lbl_batt, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(G.cmb_dev,   CB_SETDROPPEDWIDTH, 390, 0);  // long device names
        SendMessageW(G.cmb_fmt,   CB_SETDROPPEDWIDTH, 390, 0);
        SendMessageW(G.cmb_rate,  CB_SETDROPPEDWIDTH, 410, 0);
        SendMessageW(G.cmb_aplat, CB_SETDROPPEDWIDTH, 440, 0);
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
        for (HWND c : { lbl1, lbl2, lbl3, lbl4, lbl5, G.cmb_dev, G.cmb_fmt,
                        G.cmb_rate, G.cmb_aplat, G.btn_scan, G.btn_beep,
                        G.btn_localbeep, G.sld_apvol, G.lbl_apvol, G.lbl_batt,
                        G.flash, G.stat, G.log })
            apply_control_dark_theme(c,
                (c == G.cmb_dev || c == G.cmb_fmt || c == G.cmb_rate || c == G.cmb_aplat)
                    ? L"DarkMode_CFD" : L"DarkMode_Explorer");
        for (HWND b : { G.btn_scan, G.btn_beep, G.btn_localbeep })
            apply_dark_button(b);
        for (HWND c : { G.cmb_dev, G.cmb_fmt, G.cmb_rate, G.cmb_aplat })
            apply_dark_combo(c);

        SendMessageW(G.cmb_rate, CB_ADDSTRING, 0, (LPARAM)L"DLNA: Native rate  (48 kHz typical)");
        SendMessageW(G.cmb_rate, CB_ADDSTRING, 0, (LPARAM)L"DLNA: 2\x00D7 upsample \x2014 halves firmware-buffer latency");
        SendMessageW(G.cmb_rate, CB_ADDSTRING, 0, (LPARAM)L"DLNA: 4\x00D7 upsample \x2014 quarters it (~9 Mbit/s max)");
        SendMessageW(G.cmb_rate, CB_SETCURSEL, 0, 0);

        SendMessageW(G.cmb_fmt, CB_ADDSTRING, 0, (LPARAM)L"DLNA: LPCM 16-bit  (lowest latency — recommended)");
        SendMessageW(G.cmb_fmt, CB_ADDSTRING, 0, (LPARAM)L"DLNA: LPCM 24-bit");
        SendMessageW(G.cmb_fmt, CB_ADDSTRING, 0, (LPARAM)L"DLNA: WAV 16-bit   (best compatibility)");
        SendMessageW(G.cmb_fmt, CB_ADDSTRING, 0, (LPARAM)L"DLNA: WAV 24-bit");
        SendMessageW(G.cmb_fmt, CB_SETCURSEL, 0, 0);

        SendMessageW(G.cmb_dev, CB_ADDSTRING, 0, (LPARAM)L"Default output device");
        if (!g_ui_preview) {
            enum_render_devices();
            for (auto& n : G.dev_names)
                SendMessageW(G.cmb_dev, CB_ADDSTRING, 0, (LPARAM)n.c_str());
        } else {
            SendMessageW(G.cmb_dev, CB_ADDSTRING, 0,
                         (LPARAM)L"[PREVIEW] Mock system-audio endpoint");
        }
        SendMessageW(G.cmb_dev, CB_SETCURSEL, 0, 0);

        if (g_ui_preview) {
            Renderer ready; ready.name = "[PREVIEW] DLNA receiver - ready";
            Renderer disabled; disabled.name = "[PREVIEW] DLNA receiver - disabled";
            disabled.streaming = true;
            G.renderers.push_back(ready);
            G.renderers.push_back(disabled);
            AirplayDev air; air.name = "[PREVIEW] AirPlay receiver"; air.host = "127.0.0.1";
            air.port = 5000;
            g_airplay.push_back(air);
            layout_renderer_buttons(h);
            if (G.renderers.size() > 1 && G.renderers[1].button)
                EnableWindow(G.renderers[1].button, FALSE);
            g_localbeep_button_checked = true;
            SendMessageW(G.btn_localbeep, BM_SETCHECK, BST_CHECKED, 0);
            SetWindowTextW(G.stat,
                L"UI PREVIEW ONLY - audio, discovery, HTTP and battery polling are off.\r\n"
                L"Mock rows exercise ready, active and disabled button states.");
            SetWindowTextW(G.log,
                L"[preview] Safe local mock data; no streaming state or settings are written.\r\n"
                L"[preview] Click Latency beep test to inspect subdued flash on/off colors.\r\n"
                L"[preview] Tab through controls to inspect the low-glare focus treatment.\r\n");
            SetFocus(G.btn_scan);
            return 0;
        }

        load_settings();                 // restore saved prefs BEFORE first capture
        SetTimer(h, IDT_STATS, 250, nullptr);
        start_capture();
        start_detached_thread(http_server_thread, L"[http] cannot create server worker");
        start_detached_thread(discover_thread, L"[scan] cannot create discovery worker");
        ui_log(L"[app] LowCast started — zero sender buffering, TCP_NODELAY, LPCM");
        ui_log(L"[app] NOTE: DLNA has an inherent receiver-side buffer (often "
               L"several hundred ms) fixed in the device firmware — it is the "
               L"high-fidelity path, not the low-latency one. For gaming/low "
               L"latency use the AirPlay button.");
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc{}; GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, g_br_bg);
        return 1;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi && mi->CtlType == ODT_COMBOBOX) {
            UINT menu_h = (UINT)GetSystemMetrics(SM_CYMENU);
            mi->itemHeight = menu_h > 22 ? menu_h : 22;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM:
        if (draw_dark_button_item((DRAWITEMSTRUCT*)lp)) return TRUE;
        if (draw_dark_combo_item((DRAWITEMSTRUCT*)lp)) return TRUE;
        break;
    case WM_COMMAND: {
        if (g_closing) return 0;
        int id = LOWORD(wp);
        if ((id == 4 || id == 7) && HIWORD(wp) == BN_CLICKED) {
            HWND toggle = id == 4 ? G.btn_beep : G.btn_localbeep;
            bool& checked = id == 4 ? g_beep_button_checked : g_localbeep_button_checked;
            checked = !checked;
            SendMessageW(toggle, BM_SETCHECK,
                         checked ? BST_CHECKED : BST_UNCHECKED, 0);
            (void)SendMessageW(toggle, BM_GETCHECK, 0, 0); // keep native state mirrored
            InvalidateRect(toggle, nullptr, FALSE);
        }
        if (g_ui_preview) {
            if (id == 4 && HIWORD(wp) == BN_CLICKED) {
                G.flash_state = g_beep_button_checked;
                InvalidateRect(G.flash, nullptr, TRUE);
            }
            return 0; // safe preview: never start capture, discovery or a receiver
        }
        if (id == 3 && HIWORD(wp) == BN_CLICKED) {           // rescan
            start_detached_thread(discover_thread, L"[scan] cannot create discovery worker");
        } else if (id == 4 && HIWORD(wp) == BN_CLICKED) {    // beep test toggle
            bool on = g_beep_button_checked;
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
            if (sel >= 0 && sel < 10) G.ap_latency_ms.store(g_ap_lats[sel]);
            ui_log(L"[raop] buffer changed \x2014 takes effect on next AirPlay START");
        } else if (id == 7 && HIWORD(wp) == BN_CLICKED) {   // local beep toggle
            bool on = g_localbeep_button_checked;
            bool was = G.beep_local.exchange(on);
            if (on && !was && !start_detached_thread(
                    local_beep_thread, L"[test] cannot create local beep worker")) {
                G.beep_local.store(false);
                g_localbeep_button_checked = false;
                SendMessageW(G.btn_localbeep, BM_SETCHECK, BST_UNCHECKED, 0);
                InvalidateRect(G.btn_localbeep, nullptr, FALSE);
            }
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
            AirplayDev target;
            EnterCriticalSection(&G.cs);
            bool valid = idx >= 0 && idx < (int)g_airplay_button_targets.size();
            bool was = false;
            std::string host;
            int port = 0;
            if (valid) {
                target = g_airplay_button_targets[idx];
                valid = false;
                for (auto& device : g_airplay) {
                    if (device.host == target.host && device.port == target.port) {
                        was = device.streaming;
                        host = device.host;
                        port = device.port;
                        device.streaming = !was;
                        valid = true;
                        break;
                    }
                }
            }
            LeaveCriticalSection(&G.cs);
            if (valid) {
                if (was) raop_stop();
                else {
                    // Refuse BEFORE touching this host's DLNA stream: raop_start
                    // declines when another session is live (or the previous one
                    // is still closing), and the optimistic flag must then be
                    // undone, or this button shows STOP and its next click kills
                    // the OTHER receiver's live session.
                    bool ok = !raop_is_running();
                    if (!ok) ui_log(L"[raop] an AirPlay session is already live \x2014 stop it first");
                    else ok = raop_start(host, port, (uint32_t)G.ap_latency_ms.load());
                    // (raop_start ends this host's DLNA stream itself, and only
                    // once it knows it will start: its "previous session still
                    // closing" refusal must not cost the host that stream.)
                    if (!ok) {
                        EnterCriticalSection(&G.cs);
                        for (auto& device : g_airplay)
                            if (device.host == host && device.port == port)
                                device.streaming = false;
                        LeaveCriticalSection(&G.cs);
                    }
                }
                PostMessageW(G.hwnd, WM_APP_RENDERS, 1, 0);
            }
        } else if (id >= IDC_RENDER_BASE && HIWORD(wp) == BN_CLICKED) {
            int idx = id - IDC_RENDER_BASE;
            Renderer target;
            EnterCriticalSection(&G.cs);
            bool valid = idx >= 0 && idx < (int)g_dlna_button_targets.size();
            bool was = false;
            if (valid) {
                target = g_dlna_button_targets[idx];
                valid = false;
                for (const auto& renderer : G.renderers) {
                    if (same_renderer(renderer, target)) {
                        target = renderer;
                        was = renderer.streaming;
                        valid = true;
                        break;
                    }
                }
            }
            if (valid) EnableWindow((HWND)lp, FALSE);
            LeaveCriticalSection(&G.cs);
            if (valid) {
                if (!was && raop_is_running()) {
                    // symmetric: don't start DLNA while AirPlay owns a device
                    std::string ah = target.host;
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
                CastJob* job = new CastJob{ target, !was };
                HANDLE worker = CreateThread(nullptr, 0, cast_thread, job, 0, nullptr);
                if (worker) CloseHandle(worker);
                else {
                    delete job;
                    EnableWindow((HWND)lp, TRUE);
                    ui_log(L"[cast] cannot create DLNA control worker");
                }
            }
        }
        return 0;
    }
    case WM_APP_LOG: {
        std::wstring* s = (std::wstring*)lp;
        append_log_line(G.log, *s);
        delete s;
        return 0;
    }
    case WM_APP_RENDERS:
        if (wp == 2) { // format auto-fallback happened: reflect it in the UI
            SendMessageW(G.cmb_fmt, CB_SETCURSEL, G.fmt.load(), 0);
            ui_log(L"[audio] format switched automatically (renderer compatibility)");
            return 0;
        }
        if (wp == 3) { // the fallback was rejected as well: format restored
            SendMessageW(G.cmb_fmt, CB_SETCURSEL, G.fmt.load(), 0);
            ui_log(L"[audio] fallback rejected \x2014 format restored");
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
        if (wp == IDT_SHUTDOWN) {
            // A pending driver/connect is owned until it finishes. Do not
            // destroy globals beneath it or repeatedly block the UI polling.
            bool cap_done = !g_capture_handle || WaitForSingleObject(g_capture_handle, 0) == WAIT_OBJECT_0;
            if (cap_done && raop_stop(true) && stop_capture()) {
                KillTimer(h, IDT_SHUTDOWN);
                DestroyWindow(h);
            }
        }
        else if (wp == IDT_STATS) update_stats();
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
            SetTextColor(dc, G.flash_state ? CLR_FLASH_TXT : CLR_TEXT);
            return (LRESULT)(G.flash_state ? g_br_flash_on : g_br_surface);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp,
                     ((HWND)lp == G.stat || (HWND)lp == G.lbl_batt)
                         ? CLR_SECONDARY : CLR_TEXT);
        return (LRESULT)g_br_bg;
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, CLR_SURFACE);
        SetTextColor((HDC)wp, CLR_TEXT);
        return (LRESULT)g_br_surface;
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wp, CLR_SURFACE);
        SetTextColor((HDC)wp, CLR_TEXT);
        return (LRESULT)g_br_surface;
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, CLR_SURFACE);
        SetTextColor((HDC)wp, CLR_TEXT);
        return (LRESULT)g_br_surface;
    case WM_VSCROLL:
        if (g_closing) return 0;
        if ((HWND)lp == G.sld_apvol) {
            int pos = (int)SendMessageW(G.sld_apvol, TBM_GETPOS, 0, 0);
            int pct = 100 - pos;                  // vertical: top = 100%
            wchar_t vt[16];
            swprintf(vt, 16, L"%d%%", pct);
            SetWindowTextW(G.lbl_apvol, vt);      // moving readout while dragging
            G.ap_start_vol.store(pct);
            if (raop_is_running()) {
                // TACTILE: every thumb movement queues a push, so the audio
                // level follows the drag in real time. The liveness thread
                // coalesces the flood to <=10 commands/s over RTSP.
                raop_push_volume(pct);
                if (LOWORD(wp) == TB_ENDTRACK) {  // one summary line at rest
                    wchar_t fl[48];
                    swprintf(fl, 48, L"[raop] volume: %d%%", pct);
                    ui_log(fl);
                }
            }
        }
        return 0;
    case WM_CLOSE: {
        if (g_closing) return 0;
        g_closing = true;
        if (g_ui_preview) {
            DestroyWindow(h);
            return 0;
        }
        g_battery_alert.shutting_down = true;
        bool ra_done = raop_stop(true);
        bool cap_done = stop_capture();
        if (ra_done && cap_done) DestroyWindow(h);
        else {
            SetWindowTextW(h, L"LowCast - closing audio and network workers...");
            ui_log(L"[app] closing: waiting for audio/network workers to release their resources");
            if (!SetTimer(h, IDT_SHUTDOWN, 250, nullptr)) {
                // Resource-exhaustion fallback: preserve ownership even if a
                // timer cannot be created. Ordinary shutdown stays responsive.
                while (!raop_stop(true) || !stop_capture()) Sleep(100);
                DestroyWindow(h);
            }
        }
        return 0;
    }
    case WM_DESTROY:
        battery_alert_shutdown(g_battery_alert);
        g_log_hwnd.store(nullptr);
        if (!g_ui_preview) {
            save_settings(h);
            raop_stop(true);             // graceful TEARDOWN (was: process kill
            stop_capture();              // left the receiver holding the session)
        }
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
    // Anchored ONCE to the wall clock (from wWinMain, single-threaded), then
    // advanced by the monotonic clock. The receiver only needs our sync and
    // timing-reply timestamps to be mutually consistent, never true wall time.
    // Reading the wall clock per call let a Windows Time step (event log:
    // -1239 ms at 2026-09-04 23:15:45) shift every later sync by that much,
    // which derailed the receiver until the session was restarted by hand.
    static uint64_t base_ntp = 0, base_mono = 0;
    if (!base_ntp) {
        FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
        base_mono = mono_100ns();
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; // 100ns since 1601
        const uint64_t EPOCH_DELTA = 9435484800ULL;                        // 1601 -> 1900
        uint64_t secs = t / 10000000ULL - EPOCH_DELTA;
        uint32_t frac = (uint32_t)((t % 10000000ULL) * 4294967296ULL / 10000000ULL);
        base_ntp = (secs << 32) | frac;
    }
    uint64_t d = mono_100ns() - base_mono;                      // 100 ns since anchor
    uint64_t dsec = d / 10000000ULL;
    uint64_t dfrac = ((d % 10000000ULL) << 32) / 10000000ULL;  // < 2^32
    return base_ntp + (dsec << 32) + dfrac;                     // carry is natural
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
    uint32_t cseq = 0;
    bool last_send_failed = false;
    std::string pending;
    static constexpr size_t MAX_RESPONSE = 64 * 1024;
    // Retain incomplete replies across timeouts. Only a complete, matching
    // CSeq can answer a request; blindly draining TCP loses framing.
    int extract_response(std::string& response, uint32_t& response_cseq) {
        size_t he = pending.find("\r\n\r\n");
        if (he == std::string::npos) return pending.size() >= MAX_RESPONSE ? -1 : 0;
        size_t first = pending.find("\r\n");
        if (first == std::string::npos || first < 12 ||
            pending.compare(0, 9, "RTSP/1.0 ") != 0 ||
            pending[9] < '0' || pending[9] > '9' ||
            pending[10] < '0' || pending[10] > '9' ||
            pending[11] < '0' || pending[11] > '9' ||
            (first > 12 && pending[12] != ' ')) return -1;
        uint64_t blen = 0, seq = 0;
        bool have_seq = false, have_length = false;
        for (size_t p = first + 2; p < he;) {
            size_t end = pending.find("\r\n", p);
            if (end == std::string::npos || end > he) return -1;
            size_t colon = pending.find(':', p);
            if (colon == std::string::npos || colon >= end) return -1;
            std::string key = pending.substr(p, colon - p);
            bool is_seq = ifind(key, "cseq") == 0 && key.size() == 4;
            bool is_length = ifind(key, "content-length") == 0 && key.size() == 14;
            if (is_seq || is_length) {
                if ((is_seq && have_seq) || (is_length && have_length)) return -1;
                size_t q = colon + 1;
                while (q < end && (pending[q] == ' ' || pending[q] == '\t')) ++q;
                size_t digits = q;
                uint64_t value = 0, limit = is_seq ? UINT32_MAX : MAX_RESPONSE;
                while (q < end && pending[q] >= '0' && pending[q] <= '9') {
                    unsigned digit = pending[q++] - '0';
                    if (value > (limit - digit) / 10) return -1;
                    value = value * 10 + digit;
                }
                if (q == digits) return -1;
                while (q < end && (pending[q] == ' ' || pending[q] == '\t')) ++q;
                if (q != end) return -1;
                if (is_seq) { seq = value; have_seq = true; }
                else { blen = value; have_length = true; }
            }
            p = end + 2;
        }
        if (!have_seq || he + 4 > MAX_RESPONSE || blen > MAX_RESPONSE - (he + 4)) return -1;
        size_t total = he + 4 + (size_t)blen;
        if (pending.size() < total) return 0;
        response.assign(pending, 0, total);
        pending.erase(0, total);
        response_cseq = (uint32_t)seq;
        return 1;
    }
    std::string session, ua = "iTunes/7.6.2 (Windows; N;)", cid;
    std::string uri;
    bool open(const std::string& host, int port) {
        pending.clear();
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        DWORD tmo = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
        if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
            addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                closesocket(s); s = INVALID_SOCKET;   // no handle leak per retry
                return false;
            }
            a.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        // Plain BLOCKING connect — deliberately. A non-blocking connect +
        // select was tried twice (3 s and 8 s deadlines) and never completed
        // on this system even with the receiver provably awake, while this
        // path connects in milliseconds. Something in the local Winsock
        // layering does not honor non-blocking connect semantics; do not
        // reintroduce it without testing on THIS machine.
        if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
            closesocket(s); s = INVALID_SOCKET;       // no handle leak on failure
            return false;
        }
        cid = stable_client_id();
        return true;
    }
    // returns full response, or empty on failure; checks RTSP/1.0 200
    std::string request(const char* method, const std::string& ruri,
                        const std::string& extra_hdrs, const std::string& body,
                        bool* ok_out = nullptr) {
        char hdr[1024];
        snprintf(hdr, sizeof(hdr),
                 "%s %s RTSP/1.0\r\nCSeq: %u\r\nUser-Agent: %s\r\n"
                 "Client-Instance: %s\r\nDACP-ID: %s\r\n%s%s%s"
                 "Content-Length: %zu\r\n\r\n",
                 method, ruri.c_str(), ++cseq, ua.c_str(), cid.c_str(), cid.c_str(),
                 session.empty() ? "" : ("Session: " + session + "\r\n").c_str(),
                 extra_hdrs.c_str(), extra_hdrs.empty() ? "" : "\r\n",
                 body.size());
        std::string req = std::string(hdr) + body;
        last_send_failed = false;
        if (ok_out) *ok_out = false;
        // The budget covers the entire exchange, including partial sends and
        // stale replies. A trickle cannot renew a five-second recv timeout.
        ULONGLONG deadline = GetTickCount64() + 5000;
        for (size_t sent = 0; sent < req.size();) {
            ULONGLONG tick = GetTickCount64();
            if (tick >= deadline) { last_send_failed = true; return {}; }
            DWORD remaining = (DWORD)(deadline - tick);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&remaining, sizeof(remaining));
            int n = send(s, req.data() + sent, (int)(req.size() - sent), 0);
            if (n <= 0) { last_send_failed = true; return {}; }
            sent += (size_t)n;
        }
        std::string resp; char buf[2048];
        bool matched = false;
        while (true) {
            ULONGLONG tick = GetTickCount64();
            if (tick >= deadline) break;
            uint32_t reply_seq = 0;
            int framed = extract_response(resp, reply_seq);
            if (framed < 0) { last_send_failed = true; pending.clear(); break; }
            if (framed > 0) {
                if (reply_seq == cseq) { matched = true; break; }
                resp.clear(); // complete delayed reply belongs to an older request
                continue;
            }
            DWORD remaining = (DWORD)(deadline - tick);
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&remaining, sizeof(remaining));
            int capacity = (int)(MAX_RESPONSE - pending.size());
            int n = recv(s, buf, capacity < (int)sizeof(buf) ? capacity : (int)sizeof(buf), 0);
            if (n <= 0) {
                if (n == 0 || WSAGetLastError() != WSAETIMEDOUT) last_send_failed = true;
                break;
            }
            pending.append(buf, n);
        }
        if (!matched) resp.clear();
        bool ok = matched && resp.compare(0, 12, "RTSP/1.0 200") == 0;
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
    void close() { if (s != INVALID_SOCKET) closesocket(s); s = INVALID_SOCKET; pending.clear(); }
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
    std::atomic<bool> udp_run{false}; // sockets remain unchanged until both workers join
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
    uint64_t hist_ts[1024] = {};              // send time per slot (hist_cs)
    // worst resend-request age this session: a DIRECT measurement of the
    // buffer the receiver needed for that loss repair to be useful
    std::atomic<uint32_t> hw_resend_age{0};
    std::atomic<uint32_t> hw_depth_ms{0};     // session-worst pipe depth (ms)
    std::atomic<uint64_t> sess_t0{0};         // session start (soak gate)
};
static RaopSession RA;

// Tag a UDP flow as voice traffic (DSCP EF) so WiFi WMM gives it airtime
// priority. Dynamic-loaded; silently skipped where qWave is unavailable.
static constexpr int QOS_VOICE_TRAFFIC_TYPE = 4;
using QosAddSocket = BOOL (WINAPI *)(HANDLE, SOCKET, sockaddr*, int, DWORD, UINT32*);
static bool qos_assign_voice(QosAddSocket add, HANDLE handle, SOCKET s,
                             const sockaddr_in& dst, DWORD& error) {
    UINT32 flow = 0;
    if (add(handle, s, (sockaddr*)&dst, QOS_VOICE_TRAFFIC_TYPE, 2, &flow)) {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    // A null destination is valid only for an already connected socket.
    sockaddr_in peer{}; int length = sizeof(peer);
    if (getpeername(s, (sockaddr*)&peer, &length) == 0) {
        flow = 0;
        if (add(handle, s, nullptr, QOS_VOICE_TRAFFIC_TYPE, 2, &flow)) {
            error = ERROR_SUCCESS;
            return true;
        }
        error = GetLastError();
    }
    return false;
}
static void qos_tag_voice(SOCKET s, const sockaddr_in& dst) {
    // minimal qWave declarations (mingw's qos2.h is broken)
    struct QOS_VERSION { USHORT MajorVersion, MinorVersion; };
    typedef UINT32 QOS_FLOWID;
    typedef BOOL (WINAPI *PQOSCreateHandle)(QOS_VERSION*, PHANDLE);
    typedef BOOL (WINAPI *PQOSAddSocketToFlow)(HANDLE, SOCKET, sockaddr*,
                                               int, DWORD, QOS_FLOWID*);
    struct Api { HANDLE handle = nullptr; PQOSAddSocketToFlow add = nullptr;
                 DWORD error = ERROR_PROC_NOT_FOUND; };
    static const Api api = [] {
        Api result;
        HMODULE m = LoadLibraryW(L"qwave.dll");
        if (m) {
            PQOSCreateHandle pCreate = (PQOSCreateHandle)GetProcAddress(m, "QOSCreateHandle");
            result.add = (PQOSAddSocketToFlow)GetProcAddress(m, "QOSAddSocketToFlow");
            QOS_VERSION v{ 1, 0 };
            if (pCreate && result.add) {
                if (pCreate(&v, &result.handle)) result.error = ERROR_SUCCESS;
                else result.error = GetLastError();
            }
        } else {
            result.error = GetLastError();
        }
        return result;
    }();
    DWORD error = api.error;
    bool ok = api.handle && api.add && qos_assign_voice(api.add, api.handle, s, dst, error);
    char address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &dst.sin_addr, address, sizeof(address));
    wchar_t line[256];
    swprintf(line, 256, L"[net] QoS voice flow %ls for %hs:%u; error=%lu "
             L"(API acceptance does not confirm router priority)",
             ok ? L"accepted" : L"FAILED", address, (unsigned)ntohs(dst.sin_port), error);
    ui_log(line);
}

// Fixed, lock-free counters; no formatting/allocation/logging on send paths.
enum class UdpKind { Audio, Duplicate, Resend, Timing, Sync, Count };
static constexpr int UDP_ERRORS[] = {WSAEWOULDBLOCK, WSAENOBUFS, WSAEMSGSIZE,
    WSAENETUNREACH, WSAEHOSTUNREACH, WSAENOTSOCK, WSAETIMEDOUT};
struct UdpSendStats {
    std::atomic<uint64_t> ok{0}, failed{0}, short_send{0}, max_ms{0};
    std::atomic<int> last_error{0};
    std::atomic<uint64_t> errors[8]{}; // seven named codes plus other
};
static UdpSendStats g_udp_stats[(int)UdpKind::Count];
static bool record_udp_result(UdpSendStats& stats, int length, int sent,
                               int error, uint64_t elapsed) {
    uint64_t old = stats.max_ms.load(std::memory_order_relaxed);
    while (old < elapsed && !stats.max_ms.compare_exchange_weak(
               old, elapsed, std::memory_order_relaxed)) {}
    if (sent == length) {
        stats.ok.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    stats.failed.fetch_add(1, std::memory_order_relaxed);
    if (sent == SOCKET_ERROR) {
        stats.last_error.store(error, std::memory_order_relaxed);
        int bucket = 7;
        for (int i = 0; i < 7; ++i) if (UDP_ERRORS[i] == error) { bucket = i; break; }
        stats.errors[bucket].fetch_add(1, std::memory_order_relaxed);
    } else {
        stats.short_send.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}
static bool diagnosed_udp_send(UdpKind kind, SOCKET s, const char* data, int length,
                                const sockaddr* destination, int address_length) {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{}; QueryPerformanceFrequency(&value);
        return (uint64_t)value.QuadPart;
    }();
    LARGE_INTEGER start{}, end{}; QueryPerformanceCounter(&start);
    int sent = sendto(s, data, length, 0, destination, address_length);
    int error = sent == SOCKET_ERROR ? WSAGetLastError() : 0;
    QueryPerformanceCounter(&end);
    return record_udp_result(g_udp_stats[(int)kind], length, sent, error,
                             (uint64_t)(end.QuadPart - start.QuadPart) * 1000 / frequency);
}
static void report_udp_sends() {
    static const wchar_t* names[] = {L"audio", L"duplicate", L"resend", L"timing", L"sync"};
    for (int k = 0; k < (int)UdpKind::Count; ++k) {
        auto& stats = g_udp_stats[k];
        uint64_t ok = stats.ok.exchange(0), failed = stats.failed.exchange(0);
        uint64_t short_send = stats.short_send.exchange(0), max_ms = stats.max_ms.exchange(0);
        int last_error = stats.last_error.exchange(0);
        std::wstring errors;
        for (int i = 0; i < 8; ++i) {
            uint64_t count = stats.errors[i].exchange(0);
            if (count) {
                wchar_t item[64];
                swprintf(item, 64, L" %d:%llu", i == 7 ? -1 : UDP_ERRORS[i],
                         (unsigned long long)count);
                errors += item;
            }
        }
        if (!ok && !failed && !max_ms && errors.empty()) continue;
        wchar_t line[512];
        swprintf(line, 512, L"[send] %ls: ok=%llu failed=%llu short=%llu "
                 L"max_ms=%llu last_wsa=%d errors{%ls}", names[k],
                 (unsigned long long)ok, (unsigned long long)failed,
                 (unsigned long long)short_send, (unsigned long long)max_ms,
                 last_error, errors.c_str());
        ui_log(line);
    }
}

static int bind_udp(SOCKET& s) {
    SOCKET n = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (n == INVALID_SOCKET) return 0;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = 0;
    if (bind(n, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(n); return 0; }
    DWORD tmo = 500;                 // reader threads re-check RA.run every 500 ms.
    if (setsockopt(n, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo)) != 0 ||
        setsockopt(n, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo)) != 0) {
        closesocket(n); return 0;
    }
    sockaddr_in me{}; int ml = sizeof(me);
    if (getsockname(n, (sockaddr*)&me, &ml) != 0) { closesocket(n); return 0; }
    s = n;                           // publish only once fully configured: a reader
    return ntohs(me.sin_port);       // must never pick up a socket with no timeout
}
// Called only after workers have joined; no cached socket can outlive its owner.
static void close_udp(SOCKET& s) {
    SOCKET old = s;
    s = INVALID_SOCKET;
    if (old != INVALID_SOCKET) closesocket(old);
}

static DWORD WINAPI raop_timing_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // answer NTP timing requests (type 0xD2) with 0xD3 replies
    char buf[64];
    const SOCKET s = RA.tim_sock;
    while (RA.run.load() && RA.udp_run.load()) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (!RA.udp_run.load() || !RA.run.load()) break;
        if (n < 0) {                 // 500 ms timeout is the normal idle path;
            if (WSAGetLastError() != WSAETIMEDOUT) Sleep(20);   // anything else
            continue;                // (socket closed under us): never spin
        }
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
        diagnosed_udp_send(UdpKind::Timing, s, (const char*)rep, 32, (sockaddr*)&from, fl);
    }
    return 0;
}

static DWORD WINAPI raop_control_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    char buf[64];
    const SOCKET s = RA.ctrl_sock;
    // Replies go to the request's source address: shairport-sync sends 0xD5 from
    // its control socket, so `from` is always the CURRENT control port, even after
    // a reconnect re-SETUPs onto different ports (a dst cached here went stale and
    // every retransmit then hit a dead port while still counting as sent).
    while (RA.run.load() && RA.udp_run.load()) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (!RA.udp_run.load() || !RA.run.load()) break;
        if (n < 0) {
            if (WSAGetLastError() != WSAETIMEDOUT) Sleep(20);
            continue;
        }
        if (n < 8) continue;
        if ((uint8_t)buf[1] != 0xD5) continue;   // resend request
        uint16_t first = ((uint8_t)buf[4] << 8) | (uint8_t)buf[5];
        uint16_t count = ((uint8_t)buf[6] << 8) | (uint8_t)buf[7];
        if (count > 128) count = 128;
        for (uint16_t i = 0; i < count; ++i) {
            if (!RA.udp_run.load() || !RA.run.load()) break;
            uint16_t want = (uint16_t)(first + i);
            std::vector<uint8_t> pkt;
            uint64_t sent_at = 0;
            EnterCriticalSection(&RA.hist_cs);
            std::vector<uint8_t>& h = RA.hist[want & 1023];
            if (h.size() > 4 &&
                ((h[2] << 8) | h[3]) == want) {   // seq matches: still in history
                pkt = h;
                sent_at = RA.hist_ts[want & 1023];
            }
            LeaveCriticalSection(&RA.hist_cs);
            if (pkt.empty()) continue;
            if (sent_at) {                        // measured network-repair demand
                uint32_t age = (uint32_t)(now_ms() - sent_at);
                // >2s is unrepairable within any gaming buffer: it belongs in
                // the resend COUNT, not the sizing statistic
                if (age < 2000 && age > RA.hw_resend_age.load())
                    RA.hw_resend_age.store(age);
            }
            std::vector<uint8_t> re;
            re.reserve(pkt.size() + 4);
            re.push_back(0x80); re.push_back(0xD6);
            re.push_back((uint8_t)(want >> 8)); re.push_back((uint8_t)want);
            re.insert(re.end(), pkt.begin(), pkt.end());
            if (diagnosed_udp_send(UdpKind::Resend, s, (const char*)re.data(), (int)re.size(),
                                   (sockaddr*)&from, fl)) RA.resends.fetch_add(1);
        }
    }
    return 0;
}

static void raop_stop_udp_workers() {
    RA.udp_run.store(false);
    // Each socket has a 500 ms I/O timeout. Never close/recycle its handle
    // while a worker can still be executing recvfrom or sendto with it.
    if (RA.tim_thread) {
        WaitForSingleObject(RA.tim_thread, INFINITE);
        CloseHandle(RA.tim_thread); RA.tim_thread = nullptr;
    }
    if (RA.ctrl_thread) {
        WaitForSingleObject(RA.ctrl_thread, INFINITE);
        CloseHandle(RA.ctrl_thread); RA.ctrl_thread = nullptr;
    }
}

static void raop_send_sync(bool first) {
    uint8_t p[20];
    p[0] = first ? 0x90 : 0x80;
    p[1] = 0xD4; p[2] = 0x00; p[3] = 0x04;
    // ^ LITERAL-LATENCY MODE: flags=4 (the value Apple's own sender uses)
    // instead of 7 — shairport-family receivers then skip the fixed
    // +11025-frame (250 ms) offset and honor our announced latency exactly.
    // NOTE: the similar-looking rep[3] = 0x07 in the resend-reply builder is
    // a different field and must stay 0x07.
    uint32_t now_ts = RA.rtptime;                 // frame that "should play now"...
    uint32_t now_minus_lat = now_ts - RA.latency_frames;
    for (int i = 0; i < 4; ++i) p[4 + i] = (uint8_t)(now_minus_lat >> (24 - 8 * i));
    uint64_t ntp = ntp_now();
    for (int i = 0; i < 8; ++i) p[8 + i] = (uint8_t)(ntp >> (56 - 8 * i));
    for (int i = 0; i < 4; ++i) p[16 + i] = (uint8_t)(now_ts >> (24 - 8 * i));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)RA.srv_ctrl);
    inet_pton(AF_INET, RA.host.c_str(), &a.sin_addr);
    diagnosed_udp_send(UdpKind::Sync, RA.ctrl_sock, (const char*)p, 20, (sockaddr*)&a, sizeof(a));
}

// float-frame pipe from the capture thread to the RAOP sender
static CRITICAL_SECTION g_raop_cs;
static std::deque<float> g_raop_pipe;             // interleaved L,R at native rate
static std::atomic<bool> g_raop_pipe_on{false};

static void raop_pipe_push(float l, float r) {
    if (!g_raop_pipe_on.load()) return;
    EnterCriticalSection(&g_raop_cs);
    if (g_raop_pipe_on.load() && g_raop_pipe.size() < 48000 * 2 * 4) { // cap ~4 s
        g_raop_pipe.push_back(l);
        g_raop_pipe.push_back(r);
    }
    LeaveCriticalSection(&g_raop_cs);
}

static bool raop_handshake() {
    RtspConn& c = RA.rtsp;
    c = RtspConn();
    if (!c.open(RA.host, RA.rtsp_port)) { ui_log(L"[raop] RTSP connect failed"); return false; }
    if (!RA.run.load()) return false;
    std::string my_ip = local_ip_for(RA.host);
    if (my_ip.empty()) { ui_log(L"[raop] cannot determine the local route to the receiver"); return false; }
    char sid[32]; snprintf(sid, sizeof(sid), "%u", rng32());
    c.uri = "rtsp://" + my_ip + "/" + sid;

    bool ok = false;
    c.request("OPTIONS", "*", "", "", &ok);
    if (!RA.run.load()) return false;
    if (!ok) ui_log(L"[raop] OPTIONS not 200 (continuing)");

    char sdp[512];
    snprintf(sdp, sizeof(sdp),
        "v=0\r\no=iTunes %s 0 IN IP4 %s\r\ns=iTunes\r\nc=IN IP4 %s\r\nt=0 0\r\n"
        "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n",
        sid, my_ip.c_str(), RA.host.c_str());
    c.request("ANNOUNCE", c.uri, "Content-Type: application/sdp", sdp, &ok);
    if (!RA.run.load()) return false;
    if (!ok) { ui_log(L"[raop] ANNOUNCE rejected (receiver may require encryption)"); return false; }

    int my_ctrl = bind_udp(RA.ctrl_sock);
    int my_tim  = bind_udp(RA.tim_sock);
    int my_audio = bind_udp(RA.audio_sock);
    if (!my_ctrl || !my_tim || !my_audio) { ui_log(L"[raop] UDP socket setup failed"); return false; }
    DWORD tmo = 500;
    setsockopt(RA.ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(RA.tim_sock,  SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    char thdr[256];
    snprintf(thdr, sizeof(thdr),
             "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
             "control_port=%d;timing_port=%d", my_ctrl, my_tim);
    std::string resp = c.request("SETUP", c.uri, thdr, "", &ok);
    if (!RA.run.load()) return false;
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
    if (RA.srv_audio < 1 || RA.srv_audio > 65535 || RA.srv_ctrl < 1 || RA.srv_ctrl > 65535) {
        ui_log(L"[raop] missing or invalid audio/control port in SETUP reply"); return false;
    }
    ui_log8("[raop] SETUP ok — audio:" + std::to_string(RA.srv_audio) +
            " ctrl:" + std::to_string(RA.srv_ctrl) + " session:" + c.session);

    // A new worker must never answer a new session's retransmit request with
    // an old session packet that happens to reuse the same 16-bit sequence.
    EnterCriticalSection(&RA.hist_cs);
    for (int hi = 0; hi < 1024; ++hi) {
        RA.hist[hi].clear(); RA.hist_ts[hi] = 0;
    }
    LeaveCriticalSection(&RA.hist_cs);
    RA.last_timing_ms.store(0);
    RA.udp_run.store(true);
    RA.tim_thread = CreateThread(nullptr, 0, raop_timing_thread, nullptr, 0, nullptr);
    RA.ctrl_thread = CreateThread(nullptr, 0, raop_control_thread, nullptr, 0, nullptr);
    if (!RA.tim_thread || !RA.ctrl_thread) {
        raop_stop_udp_workers();
        ui_log(L"[raop] cannot start timing/control worker"); return false;
    }

    RA.seq = (uint16_t)rng32();
    RA.rtptime = (uint32_t)rng32();
    RA.ssrc = rng32();
    char rihdr[128];
    snprintf(rihdr, sizeof(rihdr), "Range: npt=0-\r\nRTP-Info: seq=%u;rtptime=%u",
             RA.seq, RA.rtptime);
    resp = c.request("RECORD", c.uri, rihdr, "", &ok);
    if (!RA.run.load()) return false;
    if (!ok) { ui_log(L"[raop] RECORD rejected"); return false; }
    std::string al = rtsp_header(resp, "Audio-Latency");
    ui_log8("[raop] RECORD ok — receiver Audio-Latency: " + (al.empty() ? "?" : al) +
            " frames; using sender latency " + std::to_string(RA.latency_frames) +
            " (" + std::to_string(RA.latency_frames * 1000 / 44100) + " ms)");
    // LITERAL-LATENCY MODE: the receiver still advertises Audio-Latency 11025
    // in its RECORD response, but with sync flags=4 it never ADDS that offset —
    // our announced latency is used verbatim. The old below-advertised-minimum
    // warning would cry wolf at every low setting, so log the mode instead.
    ui_log(L"[raop] literal-latency build: sync flags=4 \x2014 receiver honors the "
           L"latency setting exactly (no hidden +250 ms)");
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
static bool raop_heartbeat_fresh(uint64_t now, uint64_t heartbeat) {
    // A heartbeat may be sampled just after the caller sampled now.
    return heartbeat && (heartbeat >= now || now - heartbeat < 8000);
}

static bool raop_should_reconnect(bool alive, bool connection_failed,
                                  uint64_t now, uint64_t heartbeat) {
    return connection_failed || (!alive && !raop_heartbeat_fresh(now, heartbeat));
}

static bool raop_ka_request(bool* connection_failed = nullptr) { // guarded round-trip
    // OPTIONS is the canonical side-effect-free RTSP ping. The previous
    // keepalive was SET_PARAMETER volume -0.0 — literally "set volume to
    // maximum" every 20 s, which fought the receiver's own volume buttons
    // (each ping snapped the mixer back to 100%).
    bool alive = false;
    EnterCriticalSection(&RA.rtsp_cs);
    RA.rtsp.request("OPTIONS", "*", "", "", &alive);
    if (connection_failed) *connection_failed = RA.rtsp.last_send_failed;
    LeaveCriticalSection(&RA.rtsp_cs);
    return alive;
}

// Measured minimum AirPlay buffer: max(sender demand, network demand) + 10ms
// residue for what the sender cannot observe (no-loss late arrivals, receiver
// wake). Single source of truth for the diag ticker and the stats panel.
static int raop_suggested_floor_ms() {
    if (!RA.run.load()) return -1;
    uint64_t t0 = RA.sess_t0.load();          // <30s of evidence is not a floor:
    if (!t0 || now_ms() - t0 < 30000) return -1;   // report "measuring" instead
    double worst = (double)RA.hw_depth_ms.load();
    double cg = (double)G.hw_capgap.load();
    if (cg > worst) worst = cg;
    uint32_t ra_age = RA.hw_resend_age.load();
    double net = ra_age ? (double)ra_age + 8.0 : 0.0;
    if (net > worst) worst = net;
    return (((int)(worst + 10.0) + 4) / 5) * 5;
}

// Send one RAOP volume command (percent mapped linearly onto -30..0 dB).
// Takes rtsp_cs itself; Windows critical sections are recursive, so calling
// this from handshake (which already holds the lock) is safe.
static void raop_send_volume_pct(int vp, bool logit) {
    double db = -30.0 * (double)(100 - vp) / 100.0;
    char vb[48]; snprintf(vb, sizeof(vb), "volume: %.2f\r\n", db);
    bool vok = false;
    EnterCriticalSection(&RA.rtsp_cs);
    RA.rtsp.request("SET_PARAMETER", RA.rtsp.uri, "Content-Type: text/parameters", vb, &vok);
    LeaveCriticalSection(&RA.rtsp_cs);
    if (logit) {
        wchar_t lb[80];
        swprintf(lb, 80, L"[raop] volume set to %d%% (%.1f dB)", vp, db);
        ui_log(lb);
    }
}

static void raop_push_volume(int pct) { RA.vol_push.store(pct); }

static DWORD WINAPI raop_liveness_thread(LPVOID) {
    uint64_t last_ka = now_ms(), last_probe = 0;
    while (RA.ka_run.load()) {
        Sleep(100);       // 100 ms tick: live volume drags track at up to 10 cmd/s
        if (!RA.ka_run.load()) break;
        if (RA.ka_rearm.exchange(false)) { last_ka = now_ms(); last_probe = 0; }
        if (RA.ka_dead.load()) continue;     // verdict pending; reconnect owns RTSP
        { int vpush = RA.vol_push.exchange(-2);   // UI-queued live volume change
          if (vpush >= 0) raop_send_volume_pct(vpush, false); }
        uint64_t nowm = now_ms();
        uint64_t lt = RA.last_timing_ms.load();
        bool hb_stale = lt && !raop_heartbeat_fresh(nowm, lt);
        bool do_ka = nowm - last_ka >= 20000;
        bool do_probe = !do_ka && hb_stale && nowm - last_probe >= 10000;
        if (!do_ka && !do_probe) continue;
        if (do_ka) last_ka = nowm; else last_probe = nowm;

        bool connection_failed = false;
        bool alive = raop_ka_request(&connection_failed);
        if (!alive && !connection_failed) {  // retry a stall, not a closed connection
            Sleep(700);                      // harmless here: audio keeps flowing
            if (!RA.ka_run.load()) break;
            alive = raop_ka_request(&connection_failed);
        }
        if (!RA.ka_run.load()) break;
        if (alive) {
            // OPTIONS proves only TCP reachability. Only the timing worker
            // writes a received heartbeat; last_probe already rate-limits us.
            continue;
        }
        // Re-evaluate AFTER blocking I/O: timing may have recovered while
        // either OPTIONS request was waiting. Both probe paths use this rule.
        uint64_t lt2 = RA.last_timing_ms.load();
        if (raop_should_reconnect(alive, connection_failed, now_ms(), lt2)) {
            ui_log(connection_failed
                ? L"[raop] RTSP connection failed \x2014 full session reconnect required"
                : L"[raop] keepalive failed AND heartbeat stale \x2014 session lost");
            RA.ka_dead.store(true);
            continue;
        }
        ui_log(L"[raop] keepalive reply stalled (heartbeat fresh) \x2014 keeping session");
    }
    return 0;
}

static DWORD WINAPI raop_stream_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    uint64_t hs0_t0 = now_ms();
    EnterCriticalSection(&RA.rtsp_cs);           // a zombie liveness thread from a
    bool hs0 = raop_handshake();                 // prior session must not overlap
    LeaveCriticalSection(&RA.rtsp_cs);
    if (!hs0) {
        if (RA.run.load()) {                     // real failure, not a user STOP
            uint64_t d0 = now_ms() - hs0_t0;     // duration classifies the radio
            ui_log(d0 > 15000
                ? L"[raop] receiver did not answer, but its radio IS on "
                  L"(suspended) \x2014 wake the headset and try again"
                : L"[raop] receiver did not answer and its radio is GONE "
                  L"(powered off / battery dead?) \x2014 check the headset");
        }
        RA.run = false;
        // Mirror the normal teardown. A handshake that died after SETUP has live
        // timing/control threads and three bound sockets; left as-is the non-null
        // thread handles make every later session skip creating them (no timing
        // replies, no resend server) and the sockets leak.
        RA.rtsp.close();
        raop_stop_udp_workers();
        close_udp(RA.audio_sock); close_udp(RA.ctrl_sock); close_udp(RA.tim_sock);
        return 1;
    }
    RtspConn& c = RA.rtsp;
    RA.ka_dead.store(false); RA.ka_rearm.store(false);
    RA.vol_push.store(-2);
    RA.ka_run.store(true);
    RA.ka_thread = CreateThread(nullptr, 0, raop_liveness_thread, nullptr, 0, nullptr);
    if (!RA.ka_thread) {
        ui_log(L"[raop] cannot start liveness worker");
        RA.run.store(false);
    }

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
    uint64_t rg_dup_late_ev = 0, rg_dup_late_max = 0;   // duplicate wake lateness
    double rg_dmin = 1e18, rg_dmax = 0.0;
    // session high-water marks -> empirically suggested minimum AirPlay buffer
    double hw_depth = 0.0;
    // ONE timestamp feeds both the soak gate and the ticker schedule: the
    // gate must be provably open by the time the first report prints.
    uint64_t sess_start = now_ms();
    RA.sess_t0.store(sess_start);
    uint64_t hw_next = sess_start + 30000;
    // Spin-up exclusion: the first seconds contain pipeline-fill and servo
    // settling transients (fill bursts, 37ms depth spikes) that say nothing
    // about scheduler behavior. All floor marks re-arm at +3s; reconnect
    // refills mute the depth mark the same way (a reconnect audibly gaps
    // regardless of buffer, so its spike must not price the floor).
    uint64_t hw_mute_until = sess_start + 3000;
    bool hw_armed = false;
    G.hw_capgap.store(0);
    RA.hw_resend_age.store(0);
    RA.hw_depth_ms.store(0);
    uint64_t hw_resends0 = RA.resends.load();
    // --- proactive duplicate transmission (ALWAYS ON in this build) ------
    // Every packet is sent twice. The copy's delay is derived per session
    // from the true receiver latency (setting + the hidden 250 ms flags==7
    // offset): late enough to outlive a loss burst, early enough to beat
    // the receiver's ~200 ms output-buffer hand-off. Doubles audio
    // bandwidth. NOTE: duplicates heal gaps before the receiver notices
    // them — use a non-duplicating build for resend-meter runs.
    // LITERAL-LATENCY MODE: true receiver latency == our setting (no +250).
    // the razor experiment (margin 220, 30 ms umbrella at the 250 setting)
    // audibly lost the race — departures on the ~8 ms packet cadence plus a
    // WiFi retry pushed too many copies past the hand-off. Back to 230
    // (200 ms ALSA buffer + ~10 flight + ~20 spare): every armed setting
    // now lands with real margin. Consequences: 250 is dup-OFF again
    // (stock receiver genuinely can't hold an umbrella there), and 275
    // carries D=45 — the exact twin of the old offset build's 25-setting
    // (true 275, D=44) for controlled A/B. Rebuild with ~90 once the
    // receiver config edit (0.06 s buffer + fast resends) is applied.
    static const int DUP_HANDOFF_MARGIN_MS = 230;
    int dup_delay = (int)(RA.latency_frames * 1000ull / 44100) - DUP_HANDOFF_MARGIN_MS;
    if (dup_delay > 350) dup_delay = 350;
    if (dup_delay < 30)  dup_delay = 0;   // can't beat the hand-off at this
                                          // setting; all copies would arrive
                                          // late, so save the bandwidth
    std::deque<std::pair<uint16_t, uint64_t>> dupq;   // {seq, due_ms}
    if (dup_delay) {
        wchar_t dl[96];
        swprintf(dl, 96, L"[dup] duplicate transmission ON, delay %d ms \x2014 "
                 L"covers bursts up to ~%d ms (literal build)", dup_delay, dup_delay);
        ui_log(dl);
    } else {
        ui_log(L"[dup] duplicates OFF at this setting \x2014 they cannot beat the "
               L"receiver's output hand-off until its config is tuned");
    }
    // Transmit-timeline PLL: locks the OUTPUT frame rate to exactly 44100 per
    // wall-second. measured_rate is only the seed; the servo nulls its noise.
    // Publish a clean producer transition under the queue lock. The second
    // flag check in raop_pipe_push prevents an old pre-lock producer from
    // appending a stopped session's tail after this clear.
    EnterCriticalSection(&g_raop_cs);
    g_raop_pipe.clear();
    g_raop_pipe_on.store(true);
    LeaveCriticalSection(&g_raop_cs);

    while (RA.run.load()) {
        // Recovery must also run when capture supplies no frames. A verdict
        // that arrives during the sender block is handled on the next tick.
        if (!RA.ka_dead.load()) {
        // Send any due duplicates first (runs on the starved path too). Same
        // seq-verification and copy-under-lock idiom as the resend server.
        if (dup_delay) {
            uint64_t dnow = now_ms();
            while (!dupq.empty() && dupq.front().second <= dnow) {
                uint16_t want = dupq.front().first;
                uint64_t late = dnow - dupq.front().second;   // wake lateness vs due
                dupq.pop_front();
                if (late > rg_dup_late_max) rg_dup_late_max = late;
                if (late >= 5) rg_dup_late_ev++;   // beyond a 1 ms-timer wake; the
                                                   // Win11 timer-throttle signature
                                                   // is 8-15 ms here
                std::vector<uint8_t> dpkt;
                EnterCriticalSection(&RA.hist_cs);
                std::vector<uint8_t>& h = RA.hist[want & 1023];
                if (h.size() > 4 &&
                    ((h[2] << 8) | h[3]) == want)     // slot still holds this seq
                    dpkt = h;
                LeaveCriticalSection(&RA.hist_cs);
                if (!dpkt.empty())
                    diagnosed_udp_send(UdpKind::Duplicate, RA.audio_sock,
                        (const char*)dpkt.data(), (int)dpkt.size(), (sockaddr*)&dst, sizeof(dst));
            }
        }
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
          if (dms < rg_dmin) rg_dmin = dms;               // per-window range stays
          if (dms > rg_dmax) rg_dmax = dms;               // UNGATED: it shows truth
          uint64_t nowh = now_ms();
          if (!hw_armed && nowh >= sess_start + 3000) {
              hw_armed = true;                            // spin-up over: floor
              hw_depth = 0.0;                             // measurement begins
              RA.hw_depth_ms.store(0);
              G.hw_capgap.store(0);
              RA.hw_resend_age.store(0);
              hw_resends0 = RA.resends.load();
          }
          if (hw_armed && nowh >= hw_mute_until && dms > hw_depth) {
              hw_depth = dms;
              RA.hw_depth_ms.store((uint32_t)dms);
          } }
        if (now_ms() >= hw_next) {                    // [diag] measured buffer floor
            // sender demand: worst capture gap / pipe excursion (measured).
            // network demand: worst resend-request age + ~8ms repair flight
            // (measured; 0 when the air lost nothing). Remaining +10ms covers
            // only what cannot be measured from here: no-loss late arrivals
            // and receiver thread wake.
            uint32_t cg = G.hw_capgap.load();
            uint32_t ra_age = RA.hw_resend_age.load();
            unsigned long long rs = RA.resends.load() - hw_resends0;
            int sug = raop_suggested_floor_ms();
            if (sug >= 0) {                   // never fabricate a number: if the
                wchar_t hb[224];              // gate is somehow shut, defer the
                swprintf(hb, 224,             // report to the next window
                    L"[diag] steady worst: cap gap %ums, pipe %.0fms, resend age %ums (%llu resends) "
                    L"\x2192 %ls ~%dms (current %dms)",
                    cg, hw_depth, ra_age, rs,
                    (RA.latency_frames * 1000ull / 44100) < 400
                        ? L"sender-only floor (resend gate shut below ~400 ms)"
                        : (rs ? L"suggested min buffer"
                              : L"suggested min buffer (no resends yet)"),
                    sug, (int)(RA.latency_frames * 1000ull / 44100));
                ui_log(hb);
            }
            hw_next += 30000;
        }
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
            report_udp_sends();
            double meas = G.measured_rate.load();
            int ppm = (int)((meas / (double)nrate - 1.0) * 1e6);
            wchar_t rb[192];
            swprintf(rb, 192,
                L"[diag] raop: depth %.1f-%.1fms meas %+dppm starve=%llu(max %llums) trim=%llu(%llums) resync=%llu duplate=%llu(max %llums) clk=%llu",
                rg_dmin >= 1e17 ? 0.0 : rg_dmin, rg_dmax, ppm,
                (unsigned long long)rg_starve_ev, (unsigned long long)rg_starve_max,
                (unsigned long long)rg_trim_ev, (unsigned long long)rg_trim_ms,
                (unsigned long long)rg_resync,
                (unsigned long long)rg_dup_late_ev, (unsigned long long)rg_dup_late_max,
                (unsigned long long)g_clock_glitches.exchange(0));
            ui_log(rb);
            rg_starve_ev = rg_starve_max = rg_trim_ev = rg_trim_ms = rg_resync = 0;
            rg_dup_late_ev = rg_dup_late_max = 0;
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
        diagnosed_udp_send(UdpKind::Audio, RA.audio_sock, (const char*)pkt.data(),
                            (int)pkt.size(), (sockaddr*)&dst, sizeof(dst));
        EnterCriticalSection(&RA.hist_cs);
        RA.hist[RA.seq & 1023] = pkt;
        RA.hist_ts[RA.seq & 1023] = now_ms();
        LeaveCriticalSection(&RA.hist_cs);
        if (dup_delay) {
            // The drain runs on EVERY loop iteration (~1 ms on the starved
            // path, not the 8 ms packet cadence the old comment assumed), so a
            // copy leaves ~1 ms after its due time whenever the 1 ms timer is
            // honored. The -4 is therefore a fixed ~3 ms EARLY bias: effective
            // spacing ~42 ms at the 275 setting, the value validated by ear
            // (margin 220 = ~52 ms audibly lost the race). Kept on purpose;
            // retune from the [diag] duplate meter, not by guesswork.
            dupq.emplace_back((uint16_t)RA.seq,
                              now_ms() + (uint64_t)(dup_delay - 4));
            if (dupq.size() > 512) dupq.pop_front();   // paranoia bound
        }
        first_pkt = false;
        RA.seq++;
        RA.rtptime += FR;
        sent_frames += FR;
        RA.frames_sent.store(sent_frames);

        uint64_t sync_interval = (now_ms() < resync_burst_until) ? 50 : 1000;
        if (now_ms() - last_sync >= sync_interval) { raop_send_sync(false); last_sync = now_ms(); }
        }

        // Liveness verdicts come from the dedicated liveness thread (keepalive
        // every 20 s + heartbeat-stale probes); this loop never blocks on RTSP.
        bool session_dead = RA.ka_dead.load();

        if (session_dead) {
            // PERSISTENT reconnect: a session ends when the user says so, not
            // when the network hiccups. Retry until STOP is pressed.
            // rtsp_cs held across each attempt: the liveness thread must not
            // touch the RTSP connection while it is being rebuilt.
            ui_log(L"[raop] session lost \x2014 reconnecting until it returns...");
            {
                int hb3 = g_hp_batt.load();
                wchar_t bl[96];
                if (hb3 >= 0)
                    swprintf(bl, 96, L"[batt] last known headset battery: %d%% "
                                     L"(Bluetooth-sourced; may be stale)", hb3);
                else
                    swprintf(bl, 96, L"[batt] headset battery unknown at loss time");
                ui_log(bl);
            }
            int attempt = 0;
            bool back = false, told_offnet = false;
            int radio_cls = 0;   // 0 unknown, 1 answering (suspend), 2 gone
            while (RA.run.load()) {
                EnterCriticalSection(&RA.rtsp_cs);
                raop_stop_udp_workers();
                c.close();
                close_udp(RA.ctrl_sock); close_udp(RA.tim_sock); close_udp(RA.audio_sock);
                LeaveCriticalSection(&RA.rtsp_cs);
                Sleep(attempt == 0 ? 400 : 2500);
                if (!RA.run.load()) break;       // STOP/exit during the sleep: do not
                                                 // enter a ~21 s blocking connect
                attempt++;
                uint64_t t_hs = now_ms();
                EnterCriticalSection(&RA.rtsp_cs);
                bool hs = raop_handshake();
                LeaveCriticalSection(&RA.rtsp_cs);
                if (hs) { back = true; break; }
                // A failed connect's DURATION tells us the radio state: a full
                // ~21s SYN burn means ARP answered (radio alive: suspend); a
                // ~3-6s failure means ARP itself died (powered off / battery
                // dead). Log transitions — a mid-storm alive->gone flip is the
                // battery-death signature (seen 2026-07-27 02:36:39->42).
                uint64_t hdur = now_ms() - t_hs;
                int cls = hdur > 15000 ? 1 : (hdur < 8000 ? 2 : 0);
                if (cls && cls != radio_cls) {
                    radio_cls = cls;
                    ui_log(cls == 1
                        ? L"[raop] receiver's radio IS answering (suspended, not powered off)"
                        : L"[raop] receiver's radio is GONE (powered off or battery dead)");
                }
                if (attempt == 3 && !told_offnet) {
                    // three consecutive connect failures (~70 s) = the receiver
                    // is not on the network at all (standby, powered off, or
                    // dropped WiFi). Say so plainly, once.
                    told_offnet = true;
                    ui_log(L"[raop] receiver is OFF THE NETWORK (headset standby/power/WiFi) "
                           L"\x2014 will resume automatically the moment it returns");
                }
                if (attempt == 1 || attempt % 20 == 0) {
                    wchar_t b[96];
                    swprintf(b, 96, L"[raop] still trying (attempt %d)...", attempt);
                    ui_log(b);
                }
            }
            if (back) {
                EnterCriticalSection(&RA.hist_cs);   // old-timeline packets must
                for (int hi = 0; hi < 1024; ++hi) {  // never answer new-seq requests
                    RA.hist[hi].clear();
                    RA.hist_ts[hi] = 0;
                }
                LeaveCriticalSection(&RA.hist_cs);
                // The capture thread kept feeding the pipe through the whole
                // outage, so it now holds outage-duration STALE audio (log
                // evidence: 457ms backlog after a 0.44s outage, 1424ms after
                // 1.7s — trim=1 each time). Previously resume worked only
                // because the 120ms hard-trim discarded it; flush it here so
                // resume begins at live audio by design.
                EnterCriticalSection(&g_raop_cs);
                g_raop_pipe.clear();
                LeaveCriticalSection(&g_raop_cs);
                rs.design(44100.0 / (double)G.native_rate.load());
                rs_rate = G.native_rate.load();
                hw_mute_until = now_ms() + 3000;   // refill spike: not floor data
                dst.sin_port = htons((u_short)RA.srv_audio);
                inet_pton(AF_INET, RA.host.c_str(), &dst.sin_addr);
                qos_tag_voice(RA.audio_sock, dst);       // the handshake bound fresh
                {   sockaddr_in cdst = dst;              // sockets: without re-tagging
                    cdst.sin_port = htons((u_short)RA.srv_ctrl);  // the rest of the
                    qos_tag_voice(RA.ctrl_sock, cdst); } // session runs untagged
                rg_next = now_ms() + 10000;    // else an N-minute outage is followed
                hw_next = now_ms() + 30000;    // by ~6N stale [diag] lines in a burst
                dupq.clear();                  // copies queued before the outage
                                               // are dead; sending them only
                                               // pollutes the duplate meter
                first_pkt = true;
                sent_frames = 0;
                was_streaming = false;
                discontinuity = false;
                starve_since = 0;
                last_sync = now_ms();
                resync_burst_until = 0;
                RA.reconnects.fetch_add(1);
                RA.ka_rearm.store(true);       // liveness timers restart fresh
                RA.vol_push.store(-2);         // handshake already re-sent volume
                RA.ka_dead.store(false);       // publish only after rearm/reset
                ui_log(L"[raop] reconnected \x2014 stream resumed");
            }
        }
    }

    EnterCriticalSection(&g_raop_cs);
    g_raop_pipe_on.store(false);
    g_raop_pipe.clear();
    LeaveCriticalSection(&g_raop_cs);
    RA.ka_run.store(false);                      // liveness thread down first:
    if (RA.ka_thread) {                          // TEARDOWN must own the conn
        WaitForSingleObject(RA.ka_thread, INFINITE); // requests have an absolute deadline
        CloseHandle(RA.ka_thread); RA.ka_thread = nullptr;
    }
    bool tok = false;
    EnterCriticalSection(&RA.rtsp_cs);
    c.request("TEARDOWN", c.uri, "", "", &tok);
    LeaveCriticalSection(&RA.rtsp_cs);
    c.close();
    raop_stop_udp_workers();
    for (auto& hh : RA.hist) hh.clear();
    for (auto& ht : RA.hist_ts) ht = 0;          // hygiene: no stale timestamps
    close_udp(RA.audio_sock); close_udp(RA.ctrl_sock); close_udp(RA.tim_sock);
    ui_log(L"[raop] session ended");
    return 0;
}

static bool raop_start(const std::string& host, int port, uint32_t latency_ms) {
    if (RA.run.load()) {
        ui_log(L"[raop] an AirPlay session is already live \x2014 stop it first");
        return false;
    }
    // A rapid STOP->START can land here while the old stream thread is still
    // tearing down (TEARDOWN + two joins can exceed raop_stop's 3 s wait).
    // Two threads sharing RA would race on the RTSP conn and sockets, so the
    // old one must be fully gone before a new session touches RA.
    if (RA.thread) {
        if (WaitForSingleObject(RA.thread, 15000) != WAIT_OBJECT_0) {
            ui_log(L"[raop] previous session is still closing \x2014 try again in a moment");
            return false;
        }
        CloseHandle(RA.thread); RA.thread = nullptr;
    }
    RA.frames_sent.store(0);
    RA.resends.store(0);
    RA.reconnects.store(0);
    stop_dlna_to_host(host);         // only now that we know we will start
    RA.host = host; RA.rtsp_port = port;
    // LITERAL-LATENCY MODE floor: below ~100 ms the receiver's 50 ms sync
    // watchdog trips on ordinary jitter and "repairs" itself by skipping
    // frames / inserting silence in a loop. Hard-clamp to 100 ms.
    if (latency_ms < 100) {
        wchar_t w[120];
        swprintf(w, 120, L"[raop] latency %d ms clamped to 100 ms (receiver floor)",
                 latency_ms);
        ui_log(w);
        latency_ms = 100;
    } else if (latency_ms < 250) {
        ui_log(L"[raop] note: below 250 ms the stock receiver has no repair "
               L"coverage \x2014 thin jitter cushion, and duplicates cannot beat "
               L"its ~200 ms output hand-off until the receiver config is tuned");
    }
    RA.latency_frames = latency_ms * 44100 / 1000;
    RA.last_timing_ms.store(0);      // else the new session inherits the previous
                                     // one's heartbeat and starts out "stale"
    RA.run.store(true);
    RA.thread = CreateThread(nullptr, 0, raop_stream_thread, nullptr, 0, nullptr);
    if (!RA.thread) { RA.run.store(false); ui_log(L"[raop] cannot start stream worker"); return false; }
    ui_log8("[raop] starting AirPlay session to " + host + ":" + std::to_string(port));
    return true;
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
static bool raop_stop(bool wait_full) {
    // During window close, poll and let the UI defer destruction until true.
    // A normal button STOP may wait briefly, retaining any unfinished worker.
    RA.run.store(false);
    // The stream worker owns connection replacement. Do not read/shutdown
    // its changing socket here; legacy connect retains the Windows timeout.
    if (RA.thread && WaitForSingleObject(RA.thread, wait_full ? 0 : 3000) == WAIT_OBJECT_0) {
        CloseHandle(RA.thread); RA.thread = nullptr;
    }
    return RA.thread == nullptr;
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
    if (s == INVALID_SOCKET) { ui_log(L"[mdns] cannot create AirPlay resolver socket"); return; }
    sockaddr_in ba{}; ba.sin_family = AF_INET; ba.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr*)&ba, sizeof(ba)) != 0) {
        closesocket(s); ui_log(L"[mdns] cannot bind AirPlay resolver socket"); return;
    }
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
    g_ui_preview = cmdline && wcsstr(cmdline, L"uipreview") != nullptr;
    HRESULT app_id_result = SetCurrentProcessExplicitAppUserModelID(L"LowCast.WiFiAudio");
    if (!g_ui_preview) {
        sess_log_open();
        ui_log(L"[app] build 2026-09-26-regression: persistent battery alerts and verified regression fixes");
        if (FAILED(app_id_result)) ui_log(L"[app] warning: explicit taskbar identity was not accepted by Windows");
        timeBeginPeriod(1);   // Sleep(1) is ~15.6 ms by default on Windows without this
        now_ms(); ntp_now();  // anchor the monotonic + NTP clocks while single-threaded
    }
    if (!g_ui_preview) { // Windows 11 silently ignores that request for a process whose window is
        // minimized/occluded unless the process opts out. The duplicate scheduler
        // and the 1 ms starved-path wake depend on it: throttled, copies leave up
        // to ~15 ms late, past the receiver's hand-off. Resolved at runtime so it
        // is harmless on Windows 10 and on headers that lack the definitions.
        typedef BOOL (WINAPI *PSetProcInfo)(HANDLE, int, LPVOID, DWORD);
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        PSetProcInfo pSPI = k32 ? (PSetProcInfo)(void*)GetProcAddress(k32, "SetProcessInformation") : nullptr;
        struct { ULONG Version, ControlMask, StateMask; } pt{ 1, 0x4, 0 };
        // Version 1; ControlMask PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
        // (0x4) with StateMask 0 = "disable the ignore policy": always honor it.
        bool ok = pSPI && pSPI(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &pt, sizeof(pt));
        ui_log(ok ? L"[app] timer-resolution throttling opt-out: ok (1 ms wakes even when minimized)"
                  : L"[app] timer-resolution throttling opt-out: unavailable (pre-Windows 11: not needed)");
    }
    WSADATA wsa{};
    if (!g_ui_preview) {
        WSAStartup(MAKEWORD(2, 2), &wsa);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    InitializeCriticalSection(&G.cs);
    InitializeCriticalSection(&g_raop_cs);
    InitializeCriticalSection(&RA.hist_cs);
    InitializeCriticalSection(&RA.rtsp_cs);
    if (!g_ui_preview)
        start_detached_thread(battery_thread, L"[batt] cannot create battery polling worker");

    // Diagnostic mode:  LowCast.exe probe
    // Runs discovery + casts to the first renderer found, printing every step.
    if (!g_ui_preview && cmdline && wcsstr(cmdline, L"raoptest")) {
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
        while (!raop_stop(true)) Sleep(50);
        sess_log_close();
        return fs > 44100 ? 0 : 3;
    }
    if (!g_ui_preview && cmdline && wcsstr(cmdline, L"probe")) {
        g_console_mode = true;
        g_probe_file = fopen("lowcast-probe.log", "w");
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {     // launched from a terminal
            FILE* f; freopen_s(&f, "CONOUT$", "w", stdout);
        }                                               // else keep inherited stdout
        ui_log(L"[probe] LowCast diagnostic mode");
        start_detached_thread(http_server_thread, L"[http] cannot create server worker");
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
        while (!raop_stop(true) || !stop_capture()) Sleep(50);
        sess_log_close();
        return n ? 0 : 2;
    }
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    init_dark_resources();

    g_app_icon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_LOWCAST), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    g_app_icon_small = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_LOWCAST), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"LowCastWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_app_icon;
    wc.hIconSm = g_app_icon_small;
    wc.hbrBackground = g_br_bg;
    RegisterClassExW(&wc);

    int wx = g_ui_preview ? -100000 :
        (int)GetPrivateProfileIntW(L"LowCast", L"winx", -100000, ini_path().c_str());
    int wy = g_ui_preview ? -100000 :
        (int)GetPrivateProfileIntW(L"LowCast", L"winy", -100000, ini_path().c_str());
    bool havepos = !g_ui_preview && wx > -3000 && wx < 8000 && wy > -3000 && wy < 8000;
    HWND h = CreateWindowW(L"LowCastWnd",
        g_ui_preview ? L"LowCast Dark UI Preview - SAFE MOCK DATA (no audio/network)" :
        L"LowCast: Minimal-Latency WiFi Audio Streamer (DLNA & AirPlay)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        havepos ? wx : CW_USEDEFAULT, havepos ? wy : CW_USEDEFAULT,
        600, 540, nullptr, nullptr, hi, nullptr);
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g_app_icon);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g_app_icon_small);
    apply_dark_titlebar(h);
    ShowWindow(h, ncmd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!g_ui_preview) sess_log_close();
    // Window destruction discards posted messages without freeing lParam.
    // The logger no longer targets it; reclaim any final queued log strings.
    while (PeekMessageW(&msg, nullptr, WM_APP_LOG, WM_APP_LOG, PM_REMOVE))
        delete (std::wstring*)msg.lParam;
    UnregisterClassW(BATTERY_ALERT_CLASS, hi);
    if (g_app_icon_small) DestroyIcon(g_app_icon_small);
    if (g_app_icon) DestroyIcon(g_app_icon);
    UnregisterClassW(L"LowCastWnd", hi);
    free_dark_resources();
    if (!g_ui_preview) {
        WSACleanup();
        timeEndPeriod(1);
    }
    return 0;
}
