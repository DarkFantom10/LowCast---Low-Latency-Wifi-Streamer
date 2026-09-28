param([switch]$Tests)
$ErrorActionPreference = 'Stop'
$toolRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = '10.0.26100.0'
$buildDir = Join-Path $PSScriptRoot 'build'
$tempDir = Join-Path $buildDir 'temp'
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$env:PATH = "$toolRoot\bin\Hostx64\x64;$sdkRoot\bin\$sdkVersion\x64;$env:PATH"
$env:INCLUDE = "$toolRoot\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\um;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\winrt"
$env:LIB = "$toolRoot\lib\x64;$sdkRoot\Lib\$sdkVersion\ucrt\x64;$sdkRoot\Lib\$sdkVersion\um\x64"
$env:TEMP = $tempDir
$env:TMP = $tempDir
$compiler = Join-Path $toolRoot 'bin\Hostx64\x64\cl.exe'
$resourceCompiler = Join-Path $sdkRoot "bin\$sdkVersion\x64\rc.exe"
if (-not (Test-Path -LiteralPath $compiler)) { throw 'MSVC toolset not found; update toolRoot for this PC.' }
if (-not (Test-Path -LiteralPath $resourceCompiler)) { throw 'Windows resource compiler not found; update sdkRoot/sdkVersion for this PC.' }
# One compiler process, no /MP. Lower priority is inherited from this wrapper.
$thisProcess = [Diagnostics.Process]::GetCurrentProcess()
$priorPriority = $thisProcess.PriorityClass
try {
    $thisProcess.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
    Write-Output ('Build mode: serial x64; process priority: ' + $thisProcess.PriorityClass)
    $name = if ($Tests) { 'regression' } else { 'LowCast-Dark' }
    $source = if ($Tests) { Join-Path $PSScriptRoot 'tests\regression.cpp' } else { Join-Path $PSScriptRoot 'LowCast.cpp' }
    $binary = if ($Tests) { Join-Path $buildDir 'regression.exe' } else { Join-Path $PSScriptRoot 'LowCast-Dark.exe' }
    $resourceScript = Join-Path $PSScriptRoot 'LowCast.rc'
    $resource = Join-Path $buildDir ($name + '.res')
    $resourceStdout = Join-Path $buildDir ($name + '-resource.log')
    $resourceStderr = Join-Path $buildDir ($name + '-resource-errors.log')
    $resourceOptions = @('/nologo', ('/fo"{0}"' -f $resource), ('"{0}"' -f $resourceScript))
    $resourceProcess = Start-Process -FilePath $resourceCompiler -ArgumentList $resourceOptions -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $resourceStdout -RedirectStandardError $resourceStderr
    Wait-Process -InputObject $resourceProcess
    if (Test-Path -LiteralPath $resourceStdout) { Get-Content -LiteralPath $resourceStdout }
    if (Test-Path -LiteralPath $resourceStderr) { Get-Content -LiteralPath $resourceStderr }
    if ($resourceProcess.ExitCode -ne 0) { throw ('Resource build failed: ' + $resourceProcess.ExitCode) }
    $subsystem = if ($Tests) { 'CONSOLE' } else { 'WINDOWS' }
    $options = @('/nologo', '/O2', '/std:c++17', '/DUNICODE', '/D_UNICODE', '/EHsc', '/W4', '/MT', '/utf-8',
        ('/Fo"{0}"' -f (Join-Path $buildDir ($name + '.obj'))), ('/Fe"{0}"' -f $binary),
        ('"{0}"' -f $source), ('"{0}"' -f $resource), '/link', ('/SUBSYSTEM:' + $subsystem),
        'ws2_32.lib', 'iphlpapi.lib', 'ole32.lib', 'comctl32.lib', 'winmm.lib',
        'user32.lib', 'gdi32.lib', 'shell32.lib', 'avrt.lib', 'uuid.lib', 'advapi32.lib')
    $stdout = Join-Path $buildDir ($name + '-build.log')
    $stderr = Join-Path $buildDir ($name + '-build-errors.log')
    $process = Start-Process -FilePath $compiler -ArgumentList $options -WorkingDirectory $buildDir -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    Wait-Process -InputObject $process
    Get-Content -LiteralPath $stdout
    Get-Content -LiteralPath $stderr
    if ($process.ExitCode -ne 0) { throw ('Build failed: ' + $process.ExitCode) }
    Write-Output ('Built ' + $binary)
} finally {
    $thisProcess.PriorityClass = $priorPriority
}
