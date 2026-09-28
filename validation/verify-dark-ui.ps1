$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$sourcePath = Join-Path $root 'LowCast.cpp'
$buildPath = Join-Path $root 'build.ps1'
$exePath = Join-Path $root 'LowCast-Dark.exe'
$volumeRendererPath = Join-Path $root 'volume_track_renderer.h'
$volumeTestPath = Join-Path $PSScriptRoot 'render-volume-test.cpp'
$source = Get-Content -Raw -LiteralPath $sourcePath
$build = Get-Content -Raw -LiteralPath $buildPath
$volumeRenderer = Get-Content -Raw -LiteralPath $volumeRendererPath
$volumeTest = Get-Content -Raw -LiteralPath $volumeTestPath

function Require-Source([string]$text) {
    if (-not $source.Contains($text)) { throw "Missing source wiring: $text" }
}

foreach ($text in @(
    'CLR_BG        = RGB(22, 24, 27)',
    'CLR_SURFACE   = RGB(34, 37, 42)',
    'CLR_TEXT      = RGB(200, 204, 210)',
    'CLR_SECONDARY = RGB(158, 165, 175)',
    'CLR_FOCUS     = RGB(132, 99, 65)',
    'CLR_FLASH_ON  = RGB(115, 86, 50)',
    'CLR_TRACK     = RGB(74, 80, 90)',
    'CLR_THUMB     = RGB(98, 105, 117)',
    'CBS_OWNERDRAWFIXED',
    'BS_OWNERDRAW',
    'WS_TABSTOP',
    'case WM_DRAWITEM:',
    'case WM_CTLCOLORSTATIC:',
    'case WM_CTLCOLOREDIT:',
    'case WM_CTLCOLORLISTBOX:',
    'TRACKBAR_CLASSW',
    'paint_dark_volume_control(HWND hw, HDC dc)',
    'paint_lowcast_volume_track(dc, bounds, thumb, g_br_bg, g_br_thumb',
    'paint_dark_combo(HWND hwnd, HDC dc)',
    'draw_dark_button_item(const DRAWITEMSTRUCT* dis)',
    'move_tab_focus(HWND current, bool reverse)',
    'SendMessageW(toggle, BM_SETCHECK',
    'DWMWA_USE_IMMERSIVE_DARK_MODE',
    'SetCurrentProcessExplicitAppUserModelID(L"LowCast.WiFiAudio")',
    'wc.hIcon = g_app_icon;',
    'wc.hIconSm = g_app_icon_small;',
    'WM_SETICON, ICON_BIG',
    'WM_SETICON, ICON_SMALL'
)) { Require-Source $text }

$buttonCreates = [regex]::Matches($source, 'CreateWindowW\(L"BUTTON"')
$ownerDrawButtons = [regex]::Matches($source,
    'CreateWindowW\(L"BUTTON"[\s\S]{0,220}?BS_OWNERDRAW')
if ($buttonCreates.Count -ne 6 -or $ownerDrawButtons.Count -ne $buttonCreates.Count) {
    throw "All 6 fixed/dynamic/alert buttons must be owner-drawn (found $($ownerDrawButtons.Count)/$($buttonCreates.Count))."
}
if ($source.Contains('BS_AUTOCHECKBOX') -or $source.Contains('BS_PUSHLIKE')) {
    throw 'Legacy automatic checkbox styles would conflict with BS_OWNERDRAW.'
}
$comboCreates = [regex]::Matches($source,
    'CreateWindowW\(L"COMBOBOX"[\s\S]{0,220}?WS_TABSTOP[\s\S]{0,220}?CBS_OWNERDRAWFIXED')
if ($comboCreates.Count -ne 4) { throw "Expected four tab-stop owner-drawn combos; found $($comboCreates.Count)." }
if ($source -notmatch 'TRACKBAR_CLASSW[\s\S]{0,180}?WS_TABSTOP') {
    throw 'Volume trackbar is missing WS_TABSTOP.'
}
$trackPaint = [regex]::Match($source,
    'static void paint_dark_volume_control\(HWND hw, HDC dc\) \{[\s\S]*?\n\}').Value
if (-not $trackPaint) { throw 'Full-client volume paint function is missing.' }
foreach ($text in @('GetClientRect(hw, &bounds)', 'g_br_bg, g_br_thumb',
                     'thumb_fill, g_br_track, g_pen_thumb')) {
    if (-not $trackPaint.Contains($text)) { throw "Volume paint wiring missing: $text" }
}
if ($trackPaint -match 'g_br_pressed|g_br_checked|g_pen_focus|CLR_PRESSED|CLR_CHECKED|CLR_FOCUS') {
    throw 'Volume paint path still references a brown LowCast resource.'
}
if ($source -match 'NM_CUSTOMDRAW') {
    throw 'Trackbar still depends on the partial common-control custom-draw path.'
}
foreach ($text in @('FillRect(dc, &channel, neutral_gray);',
                     'FillRect(dc, &thumb, thumb_fill);',
                     'center_lowcast_volume_thumb',
                     'centered.left = cx - width / 2;',
                     'centered.right = centered.left + width;',
                     'lowcast_volume_channel_rect',
                     'cx - 2, bounds.top + 5, cx + 2')) {
    if (-not $volumeRenderer.Contains($text)) { throw "Shared volume renderer missing: $text" }
}
foreach ($text in @('RECT native_thumb{ x0 + 18, 54, x0 + 40, 70 };',
                     'center_error2 == 0',
                     'channel_center2 == control_center2',
                     'thumb_center2 == control_center2',
                     'channel_max_x - channel_min_x + 1 == 4')) {
    if (-not $volumeTest.Contains($text)) { throw "Offset-centering pixel test missing: $text" }
}

foreach ($text in @(
    'g_ui_preview = cmdline && wcsstr(cmdline, L"uipreview") != nullptr;',
    'start_detached_thread(battery_thread, L"[batt] cannot create battery polling worker");',
    'battery_alert_process_reading(g_battery_alert, g_hp_batt.load(), now_ms(),',
    'WS_EX_TOPMOST | WS_EX_TOOLWINDOW',
    'LOWORD(wp) == IDC_BATTERY_ACK',
    'battery_alert_shutdown(g_battery_alert);',
    'append_log_line(G.log, *s);',
    'build 2026-09-26-regression',
    'if (!g_ui_preview && cmdline && wcsstr(cmdline, L"raoptest"))',
    'if (!g_ui_preview && cmdline && wcsstr(cmdline, L"probe"))',
    'UI PREVIEW ONLY - audio, discovery, HTTP and battery polling are off.',
    'return 0; // safe preview: never start capture, discovery or a receiver',
    'if (!g_ui_preview) {`r`n            save_settings(h);'
)) {
    # Normalize the one multiline assertion below separately for LF/CRLF.
    if ($text.Contains('`r`n')) { continue }
    Require-Source $text
}
if ($source -notmatch 'if \(!g_ui_preview\) \{\s*save_settings\(h\);') {
    throw 'Preview destruction is not guarded from settings writes.'
}

$batteryWorkerAt = $source.IndexOf('static DWORD WINAPI battery_thread')
$statsAt = $source.IndexOf('static void update_stats()', $batteryWorkerAt)
if ($batteryWorkerAt -lt 0 -or $statsAt -le $batteryWorkerAt) {
    throw 'Battery worker/UI timer boundaries are missing.'
}
$batteryWorker = $source.Substring($batteryWorkerAt, $statsAt - $batteryWorkerAt)
if ($batteryWorker.Contains('battery_alert_process_reading') -or
    $source.Contains('Shell_NotifyIconW')) {
    throw 'Battery delivery must remain on the UI timer through the application alert window.'
}

$previewAt = $source.IndexOf('if (g_ui_preview) {', $source.IndexOf('case WM_CREATE:'))
$loadAt = $source.IndexOf('load_settings();', $previewAt)
$captureAt = $source.IndexOf('start_capture();', $previewAt)
$httpAt = $source.IndexOf('http_server_thread', $previewAt)
$discoverAt = $source.IndexOf('discover_thread', $previewAt)
$returnAt = $source.IndexOf('return 0;', $previewAt)
if ($previewAt -lt 0 -or $returnAt -lt 0 -or $loadAt -lt 0 -or
    $returnAt -gt $loadAt -or $returnAt -gt $captureAt -or
    $returnAt -gt $httpAt -or $returnAt -gt $discoverAt) {
    throw 'WM_CREATE preview does not return before normal settings/audio/network startup.'
}

foreach ($text in @("'LowCast-Dark'", "'LowCast-Dark.exe'")) {
    if (-not $build.Contains($text)) { throw "Build output wiring missing: $text" }
}
if (-not (Test-Path -LiteralPath $exePath)) { throw "Executable not found: $exePath" }

$toolRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$dumpbin = Join-Path $toolRoot 'bin\Hostx64\x64\dumpbin.exe'
$imports = (& $dumpbin /nologo /imports $exePath 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed: $LASTEXITCODE" }
foreach ($symbol in @('SetCurrentProcessExplicitAppUserModelID', 'RegisterClassExW', 'LoadImageW', 'SendMessageW')) {
    if (-not $imports.Contains($symbol)) { throw "PE import missing: $symbol" }
}

$exeBytes = [IO.File]::ReadAllBytes($exePath)
foreach ($string in @('LowCast.WiFiAudio', 'uipreview', 'LowCast Dark UI Preview')) {
    $needle = [Text.Encoding]::Unicode.GetBytes($string)
    $found = $false
    for ($i = 0; $i -le $exeBytes.Length - $needle.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $needle.Length; $j++) {
            if ($exeBytes[$i + $j] -ne $needle[$j]) { $match = $false; break }
        }
        if ($match) { $found = $true; break }
    }
    if (-not $found) { throw "Embedded UTF-16 string missing: $string" }
}

'PASS: dark palette/control drawing, title-bar wiring, fresh build identity, icon/AppUserModelID wiring, and preview startup guards are present.'
