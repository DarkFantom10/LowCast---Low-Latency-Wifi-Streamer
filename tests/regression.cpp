// Focused regression checks. The application source is compiled into this
// console program; the main app, audio, discovery and receiver paths never run.
// UI checks use message-only controls or a deliberately hidden alert window.
#define wWinMain lowcast_application_entry_for_tests
#include "../LowCast.cpp"
#undef wWinMain

static int checks = 0;
static int failures = 0;
#define EXPECT(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::printf("FAIL %s line %d: %s\n", __func__, __LINE__, #condition); } } while (0)

static HWND message_only(const wchar_t* cls, DWORD style) {
    return CreateWindowExW(0, cls, L"", style, 0, 0, 560, 150, HWND_MESSAGE,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
}

struct FakeBatteryPresenter {
    int calls = 0;
    int successes = 0;
    int last_level = 0;
    int last_pct = -1;
    int failures_remaining = 0;
};

static BatteryAlertPresentResult fake_battery_presenter(
    BatteryAlertState&, int level, int pct, void* context) {
    FakeBatteryPresenter* fake = static_cast<FakeBatteryPresenter*>(context);
    ++fake->calls;
    fake->last_level = level;
    fake->last_pct = pct;
    if (fake->failures_remaining) {
        --fake->failures_remaining;
        return BatteryAlertPresentResult::Failed;
    }
    return fake->successes++ ? BatteryAlertPresentResult::Updated
                             : BatteryAlertPresentResult::Created;
}

static void test_battery_alert_gate() {
    BatteryAlertState state;
    FakeBatteryPresenter fake;
    EXPECT(!battery_alert_process_reading(state, -1, 100, fake_battery_presenter, &fake));
    EXPECT(!battery_alert_process_reading(state, 101, 200, fake_battery_presenter, &fake));
    EXPECT(!battery_alert_process_reading(state, 41, 300, fake_battery_presenter, &fake));
    EXPECT(fake.calls == 0 && state.delivered_level == 0);

    EXPECT(battery_alert_process_reading(state, 40, 400, fake_battery_presenter, &fake));
    EXPECT(fake.calls == 1 && fake.last_level == 1 && fake.last_pct == 40);
    EXPECT(state.delivered_level == 1 && state.visible_level == 1);
    for (int pct : { 39, 60, 40, -1, 60 })
        EXPECT(!battery_alert_process_reading(state, pct, 500 + pct,
                                              fake_battery_presenter, &fake));
    EXPECT(fake.calls == 1 && state.visible_level == 1);

    state.visible_level = 0; // exact state transition made by Acknowledge
    EXPECT(battery_alert_process_reading(state, 20, 800, fake_battery_presenter, &fake));
    EXPECT(state.delivered_level == 2 && state.visible_level == 2 &&
           fake.last_level == 2);
    EXPECT(battery_alert_process_reading(state, 10, 900, fake_battery_presenter, &fake));
    EXPECT(state.delivered_level == 3 && state.visible_level == 3 &&
           fake.last_level == 3);

    BatteryAlertState skipped;
    FakeBatteryPresenter skipped_fake;
    EXPECT(battery_alert_process_reading(skipped, 9, 1000,
                                         fake_battery_presenter, &skipped_fake));
    EXPECT(skipped_fake.last_level == 3 && skipped.delivered_level == 3);

    // Negative control for launch/create failure: a failed delivery is not
    // consumed, is held for five seconds, then succeeds on retry.
    BatteryAlertState retry;
    FakeBatteryPresenter retry_fake;
    retry_fake.failures_remaining = 1;
    EXPECT(!battery_alert_process_reading(retry, 40, 2000,
                                          fake_battery_presenter, &retry_fake));
    EXPECT(retry.delivered_level == 0 && retry.retry_after_ms == 7000);
    EXPECT(!battery_alert_process_reading(retry, 40, 6999,
                                          fake_battery_presenter, &retry_fake));
    EXPECT(battery_alert_process_reading(retry, 40, 7000,
                                         fake_battery_presenter, &retry_fake));
    EXPECT(retry_fake.calls == 2 && retry.delivered_level == 1 &&
           retry.retry_after_ms == 0);

    BatteryAlertState urgent_retry;
    FakeBatteryPresenter urgent_fake;
    urgent_fake.failures_remaining = 1;
    EXPECT(!battery_alert_process_reading(urgent_retry, 40, 8000,
                                          fake_battery_presenter, &urgent_fake));
    EXPECT(battery_alert_process_reading(urgent_retry, 20, 8001,
                                         fake_battery_presenter, &urgent_fake));
    EXPECT(urgent_fake.calls == 2 && urgent_fake.last_level == 2 &&
           urgent_retry.delivered_level == 2);

    BatteryAlertState stopping;
    FakeBatteryPresenter stopping_fake;
    stopping.shutting_down = true;
    EXPECT(!battery_alert_process_reading(stopping, 10, 9000,
                                          fake_battery_presenter, &stopping_fake));
    EXPECT(stopping_fake.calls == 0 && stopping.delivered_level == 0);

    EXPECT(!battery_alert_process_reading(state, 60, 10000,
                                          fake_battery_presenter, &fake));
    EXPECT(!battery_alert_process_reading(state, 80, 10200,
                                          fake_battery_presenter, &fake));
    EXPECT(state.delivered_level == 0 && state.visible_level == 3);
    EXPECT(battery_alert_process_reading(state, 40, 10300,
                                         fake_battery_presenter, &fake));
    EXPECT(state.delivered_level == 1 && state.visible_level == 1);
}

static void test_hidden_battery_alert_window() {
    BatteryAlertState state;
    BatteryAlertWindowOptions hidden{false};
    EXPECT(battery_alert_process_reading(state, 40, 20000,
                                         battery_alert_present_window, &hidden));
    HWND first = state.hwnd;
    LONG_PTR exstyle = first ? GetWindowLongPtrW(first, GWL_EXSTYLE) : 0;
    LONG_PTR button_style = state.acknowledge
        ? GetWindowLongPtrW(state.acknowledge, GWL_STYLE) : 0;
    EXPECT(first && IsWindow(first) && !IsWindowVisible(first));
    EXPECT(GetWindow(first, GW_OWNER) == nullptr && (exstyle & WS_EX_TOPMOST));
    EXPECT(state.acknowledge && IsWindow(state.acknowledge) &&
           (button_style & BS_TYPEMASK) == BS_OWNERDRAW);

    SendMessageW(first, WM_KEYDOWN, VK_ESCAPE, 0);
    SendMessageW(first, WM_CLOSE, 0, 0);
    EXPECT(IsWindow(first) && state.visible_level == 1);

    HWND dummy_main = CreateWindowExW(0, L"STATIC", L"test main",
        WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    EXPECT(dummy_main != nullptr && !IsWindowVisible(dummy_main));
    EXPECT(battery_alert_process_reading(state, 20, 20100,
                                         battery_alert_present_window, &hidden));
    wchar_t title[128]{};
    wchar_t message[320]{};
    GetWindowTextW(state.hwnd, title, 128);
    GetWindowTextW(state.message, message, 320);
    EXPECT(dummy_main && !IsWindowVisible(dummy_main) && state.hwnd == first &&
           GetWindow(state.hwnd, GW_OWNER) == nullptr);
    EXPECT(wcsstr(title, L"CRITICAL") && wcsstr(message, L"20%"));
    if (dummy_main) DestroyWindow(dummy_main);

    HWND acknowledge = state.acknowledge;
    SendMessageW(first, WM_COMMAND, MAKEWPARAM(IDC_BATTERY_ACK, BN_CLICKED),
                 reinterpret_cast<LPARAM>(acknowledge));
    EXPECT(!IsWindow(first) && !state.hwnd && state.visible_level == 0);
    EXPECT(state.delivered_level == 2);

    EXPECT(battery_alert_process_reading(state, 10, 20200,
                                         battery_alert_present_window, &hidden));
    HWND second = state.hwnd;
    EXPECT(second && second != first && !IsWindowVisible(second));
    battery_alert_shutdown(state);
    EXPECT(!IsWindow(second) && !state.hwnd && state.visible_level == 0);
}

static void test_airplay_flag_clears_without_session() {
    g_airplay.clear();
    AirplayDev first; first.name = "first"; first.host = "192.0.2.10";
    first.port = 5000; first.streaming = true;
    AirplayDev second; second.name = "second"; second.host = "192.0.2.11";
    second.port = 5001; second.streaming = false;
    g_airplay.push_back(first);
    g_airplay.push_back(second);
    RA.run.store(false);
    sync_airplay_button_state();
    EXPECT(!g_airplay[0].streaming && !g_airplay[1].streaming);

    g_airplay[0].streaming = true;
    RA.run.store(true);
    sync_airplay_button_state();
    EXPECT(g_airplay[0].streaming);
    RA.run.store(false);
    sync_airplay_button_state();
    EXPECT(!g_airplay[0].streaming);
    g_airplay.clear();
}

static std::wstring edit_text(HWND edit) {
    int len = GetWindowTextLengthW(edit);
    std::wstring text((size_t)len + 1, L'\0');
    text.resize((size_t)GetWindowTextW(edit, &text[0], len + 1));
    return text;
}

static void raw_append(HWND edit, const std::wstring& text) {
    int len = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, len, len);
    std::wstring line = text + L"\r\n";
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

static void test_log_trim_keeps_newest_complete_lines() {
    const DWORD style = WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL;
    HWND raw = message_only(L"EDIT", style);
    HWND bounded = message_only(L"EDIT", style);
    EXPECT(raw && bounded);
    if (!raw || !bounded) {
        if (raw) DestroyWindow(raw);
        if (bounded) DestroyWindow(bounded);
        return;
    }
    int limit = (int)SendMessageW(bounded, EM_GETLIMITTEXT, 0, 0);
    std::wstring last;
    for (int i = 0; i < 1500; ++i) {
        wchar_t line[160];
        swprintf(line, 160, L"[diag] line %05d raop depth 8.1-12.3ms "
                            L"starve=0 trim=0 resync=0", i);
        raw_append(raw, line);
        append_log_line(bounded, line);
        last = line;
    }
    std::wstring raw_text = edit_text(raw);
    std::wstring bounded_text = edit_text(bounded);
    std::wstring tail = last + L"\r\n";
    // Negative control: raw EM_REPLACESEL reaches the edit limit and loses the tail.
    EXPECT((int)raw_text.size() <= limit && raw_text.find(last) == std::wstring::npos);
    EXPECT((int)bounded_text.size() <= limit && (int)bounded_text.size() > limit / 3);
    EXPECT(bounded_text.size() >= tail.size() &&
           bounded_text.compare(bounded_text.size() - tail.size(), tail.size(), tail) == 0);
    EXPECT(bounded_text.rfind(L"[diag] line ", 0) == 0);
    DestroyWindow(raw);
    DestroyWindow(bounded);
}

struct EditColorProbe {
    int static_messages = 0;
    int edit_messages = 0;
};

static LRESULT CALLBACK edit_color_probe_proc(HWND hwnd, UINT message,
                                               WPARAM wp, LPARAM lp) {
    EditColorProbe* probe = reinterpret_cast<EditColorProbe*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_CTLCOLORSTATIC) {
        if (probe) ++probe->static_messages;
        return (LRESULT)GetStockObject(BLACK_BRUSH);
    }
    if (message == WM_CTLCOLOREDIT) {
        if (probe) ++probe->edit_messages;
        return (LRESULT)GetStockObject(BLACK_BRUSH);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

static void test_readonly_log_uses_opaque_static_colors() {
    const wchar_t* class_name = L"LowCastEditColorProbe";
    WNDCLASSW wc{};
    wc.lpfnWndProc = edit_color_probe_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = class_name;
    ATOM atom = RegisterClassW(&wc);
    EXPECT(atom != 0);
    if (!atom) return;

    EditColorProbe probe;
    HWND parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        class_name, L"", WS_POPUP | WS_VISIBLE, -32000, -32000, 320, 160,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (parent) SetWindowLongPtrW(parent, GWLP_USERDATA, (LONG_PTR)&probe);
    HWND readonly_edit = CreateWindowW(L"EDIT", L"paint probe",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
        0, 0, 320, 75, parent, nullptr, wc.hInstance, nullptr);
    HWND writable_edit = CreateWindowW(L"EDIT", L"editable probe",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE,
        0, 80, 320, 75, parent, nullptr, wc.hInstance, nullptr);
    EXPECT(parent && readonly_edit && writable_edit);
    if (parent && readonly_edit && writable_edit) {
        InvalidateRect(readonly_edit, nullptr, TRUE);
        UpdateWindow(readonly_edit);
        EXPECT(probe.static_messages > 0);
        EXPECT(probe.edit_messages == 0);
        InvalidateRect(writable_edit, nullptr, TRUE);
        UpdateWindow(writable_edit);
        EXPECT(probe.edit_messages > 0);
    }

    HDC dc = CreateCompatibleDC(nullptr);
    EXPECT(dc != nullptr);
    if (dc && readonly_edit) {
        HWND old_log = G.log;
        HWND old_flash = G.flash;
        HWND old_stat = G.stat;
        HWND old_batt = G.lbl_batt;
        G.log = readonly_edit;
        G.flash = nullptr;
        G.stat = nullptr;
        G.lbl_batt = nullptr;
        SetBkMode(dc, TRANSPARENT);
        HBRUSH brush = apply_main_static_colors(readonly_edit, dc);
        EXPECT(brush == g_br_bg);
        EXPECT(GetBkMode(dc) == OPAQUE);
        EXPECT(GetBkColor(dc) == CLR_BG);
        EXPECT(GetTextColor(dc) == CLR_TEXT);

        HWND ordinary_static = parent;
        SetBkMode(dc, OPAQUE);
        brush = apply_main_static_colors(ordinary_static, dc);
        EXPECT(brush == g_br_bg);
        EXPECT(GetBkMode(dc) == TRANSPARENT);
        G.log = old_log;
        G.flash = old_flash;
        G.stat = old_stat;
        G.lbl_batt = old_batt;
        DeleteDC(dc);
    } else if (dc) {
        DeleteDC(dc);
    }

    if (writable_edit) DestroyWindow(writable_edit);
    if (readonly_edit) DestroyWindow(readonly_edit);
    if (parent) DestroyWindow(parent);
    UnregisterClassW(class_name, wc.hInstance);
}

static bool volume_press_drags(HWND track, int x, int y) {
    SendMessageW(track, TBM_SETPOS, TRUE, 50);
    SendMessageW(track, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    int pressed = (int)SendMessageW(track, TBM_GETPOS, 0, 0);
    SendMessageW(track, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y + 20));
    int dragged = (int)SendMessageW(track, TBM_GETPOS, 0, 0);
    SendMessageW(track, WM_LBUTTONUP, 0, MAKELPARAM(x, y + 20));
    if (GetCapture() == track) ReleaseCapture();
    return dragged > pressed;
}

static void test_volume_visible_edge_starts_drag() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    HWND parent = message_only(L"STATIC", 0);
    HWND track = CreateWindowW(TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_VERT | TBS_NOTICKS,
        512, 28, 32, 96, parent, (HMENU)9, nullptr, nullptr);
    EXPECT(parent && track);
    if (!parent || !track) {
        if (parent) DestroyWindow(parent);
        return;
    }
    SendMessageW(track, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SetWindowSubclass(track, apvol_subclass, 1, 0);
    SendMessageW(track, TBM_SETPOS, TRUE, 50);
    RECT client{}, native{};
    GetClientRect(track, &client);
    SendMessageW(track, TBM_GETTHUMBRECT, 0, (LPARAM)&native);
    RECT painted = center_lowcast_volume_thumb(client, native);
    int cy = (native.top + native.bottom) / 2;
    EXPECT(volume_press_drags(track, (painted.left + painted.right) / 2, cy));
    EXPECT(volume_press_drags(track, painted.right - 1, cy));
    EXPECT(volume_press_drags(track, client.right - 2, cy - 25));
    EXPECT(volume_press_drags(track, client.left + 1, cy + 10));

    RECT channel{};
    SendMessageW(track, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
    EXPECT(lowcast_vertical_volume_pos(channel, native, -100) == 0);
    EXPECT(lowcast_vertical_volume_pos(channel, native, 1000) == 100);
    DestroyWindow(parent);
}

int main() {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    InitializeCriticalSection(&G.cs);
    init_dark_resources();
    test_battery_alert_gate();
    test_hidden_battery_alert_window();
    test_airplay_flag_clears_without_session();
    test_log_trim_keeps_newest_complete_lines();
    test_readonly_log_uses_opaque_static_colors();
    test_volume_visible_edge_starts_drag();
    UnregisterClassW(BATTERY_ALERT_CLASS, GetModuleHandleW(nullptr));
    free_dark_resources();
    DeleteCriticalSection(&G.cs);
    std::printf("%s: %d checks, %d failed\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
