$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = '10.0.26100.0'
$outDir = Join-Path $PSScriptRoot 'offscreen'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$env:PATH = "$toolRoot\bin\Hostx64\x64;$sdkRoot\bin\$sdkVersion\x64;$env:PATH"
$env:INCLUDE = "$toolRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\shared"
$env:LIB = "$toolRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
$compiler = Join-Path $toolRoot 'bin\Hostx64\x64\cl.exe'
$source = Join-Path $PSScriptRoot 'render-volume-test.cpp'
$exe = Join-Path $outDir 'render-volume-test.exe'
$obj = Join-Path $outDir 'render-volume-test.obj'
$bmp = Join-Path $outDir 'volume-track-offscreen.bmp'
$png = Join-Path $outDir 'volume-track-offscreen.png'
$evidence = Join-Path $outDir 'volume-track-pixels.txt'

$proc = [Diagnostics.Process]::GetCurrentProcess()
$prior = $proc.PriorityClass
try {
    $proc.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
    & $compiler /nologo /O2 /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE `
        "/I$root" "/Fo$obj" "/Fe$exe" $source /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw "Offscreen renderer build failed: $LASTEXITCODE" }
    & $exe $bmp | Tee-Object -FilePath $evidence
    if ($LASTEXITCODE -ne 0) { throw "Offscreen pixel test failed: $LASTEXITCODE" }
    Add-Type -AssemblyName System.Drawing
    $image = [Drawing.Image]::FromFile($bmp)
    try {
        if ($image.Width -ne 192 -or $image.Height -ne 128) { throw 'Unexpected offscreen dimensions.' }
        $image.Save($png, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $image.Dispose()
    }
    $signature = [IO.File]::ReadAllBytes($png)[0..7]
    if ([BitConverter]::ToString($signature) -ne '89-50-4E-47-0D-0A-1A-0A') {
        throw 'PNG signature verification failed.'
    }
    'PASS: offscreen track PNG written at 192x128; three neutral normal/hot/pressed panels.'
} finally {
    $proc.PriorityClass = $prior
    Remove-Item -LiteralPath $obj, $exe -Force -ErrorAction SilentlyContinue
}
