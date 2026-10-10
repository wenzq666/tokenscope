[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [string]$ToolchainRoot,
    [ValidatePattern('^TokenScope(?:-[0-9]+\.[0-9]+\.[0-9]+)?\.exe$')][string]$ExecutableName = 'TokenScope.exe',
    [switch]$Tests,
    [switch]$ViewTests,
    [switch]$SkipApp,
    # Optional Authenticode signing. Use a certificate from the current user's store by thumbprint,
    # or a .pfx file whose password is read from the TOKENSCOPE_SIGN_PASSWORD environment variable.
    [string]$SignCertificateThumbprint,
    [string]$SignPfx,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [string]$SignTool
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $ToolchainRoot) {
    $ToolchainRoot = Join-Path $repoRoot '.tools\llvm-mingw-20261006-ucrt-x86_64'
}
$toolBin = Join-Path $ToolchainRoot 'bin'
$cxx = Join-Path $toolBin 'x86_64-w64-mingw32-clang++.exe'
$cc = Join-Path $toolBin 'x86_64-w64-mingw32-clang.exe'
$windres = Join-Path $toolBin 'x86_64-w64-mingw32-windres.exe'
$strip = Join-Path $toolBin 'llvm-strip.exe'
$readobj = Join-Path $toolBin 'llvm-readobj.exe'
foreach ($tool in @($cxx, $cc, $windres, $strip, $readobj)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Build tool missing: $tool. Run scripts/setup-toolchain.ps1 first."
    }
}
$buildDir = Join-Path $repoRoot "build\$Configuration"
$distDir = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Force $buildDir, $distDir | Out-Null

function Invoke-Compiler([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE): $Program" }
}

$common = @('-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
    '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-I', (Join-Path $repoRoot 'vendor'),
    '-I', (Join-Path $repoRoot 'src'), '-ffunction-sections', '-fdata-sections')
if ($Configuration -eq 'Release') {
    $common += @('-Oz', '-DNDEBUG', '-flto')
} else {
    $common += @('-O0', '-g')
}
$cxxFlags = $common + @('-std=c++20', '-Wall', '-Wextra', '-Wpedantic')
# Release application code may disable assertions; test translation units must
# retain them, otherwise the view/calendar regressions would silently disappear.
$testFlags = $cxxFlags + @('-UNDEBUG')
$sqliteFlags = $common + @('-std=c11', '-DSQLITE_THREADSAFE=1', '-DSQLITE_DQS=0',
    '-DSQLITE_DEFAULT_MEMSTATUS=0', '-DSQLITE_OMIT_LOAD_EXTENSION', '-DSQLITE_OMIT_DEPRECATED',
    '-DSQLITE_OMIT_SHARED_CACHE', '-DSQLITE_OMIT_UTF16', '-DSQLITE_LIKE_DOESNT_MATCH_BLOBS')
$yyjsonFlags = $common + @('-std=c11', '-DYYJSON_DISABLE_WRITER=1', '-DYYJSON_DISABLE_UTILS=1')
$linkFlags = @('-static', '-Wl,--gc-sections', '-Wl,--nxcompat', '-Wl,--dynamicbase',
    '-Wl,--high-entropy-va', '-Wl,--no-insert-timestamp')
if ($Configuration -eq 'Release') { $linkFlags += @('-flto', '-Wl,--icf=all') }
$systemLibraries = @('-ld2d1', '-ldwrite', '-ldwmapi', '-lshlwapi', '-lcomctl32', '-lcomdlg32',
    '-lshell32', '-lole32', '-luuid', '-lwinhttp', '-lwindowscodecs', '-lbcrypt', '-lcrypt32',
    '-ladvapi32', '-luser32', '-lgdi32', '-lmsimg32')

Push-Location $repoRoot
try {
    $sqliteObject = Join-Path $buildDir 'sqlite3.o'
    $jsonObject = Join-Path $buildDir 'yyjson.o'
    Invoke-Compiler $cc ($sqliteFlags + @('-c', 'vendor/sqlite3.c', '-o', $sqliteObject))
    Invoke-Compiler $cc ($yyjsonFlags + @('-c', 'vendor/yyjson.c', '-o', $jsonObject))
    $zstdObject = Join-Path $buildDir 'zstddeclib.o'
    Invoke-Compiler $cc ($common + @('-std=c11', '-c', 'vendor/zstddeclib.c', '-o', $zstdObject))
    $coreObject = Join-Path $buildDir 'core.o'
    Invoke-Compiler $cxx ($cxxFlags + @('-c', 'src/core.cpp', '-o', $coreObject))
    $dataObjects = @($coreObject, $sqliteObject, $jsonObject, $zstdObject)
    $pricingObject = Join-Path $buildDir 'pricing.o'
    Invoke-Compiler $cxx ($cxxFlags + @('-c', 'src/pricing.cpp', '-o', $pricingObject))
    $dataObjects += $pricingObject
    $exchangeObject = Join-Path $buildDir 'exchange.o'
    Invoke-Compiler $cxx ($cxxFlags + @('-c', 'src/exchange.cpp', '-o', $exchangeObject))
    $dataObjects += $exchangeObject
    foreach ($module in @('connections','credentials','http','i18n')) {
        $moduleObject = Join-Path $buildDir "$module.o"
        Invoke-Compiler $cxx ($cxxFlags + @('-c', "src/$module.cpp", '-o', $moduleObject))
        $dataObjects += $moduleObject
    }
    foreach ($adapter in @('catalog', 'cli', 'domestic', 'export', 'extended', 'extended_families')) {
        $adapterObject = Join-Path $buildDir "adapter-$adapter.o"
        Invoke-Compiler $cxx ($cxxFlags + @('-c', "src/adapters/$adapter.cpp", '-o', $adapterObject))
        $dataObjects += $adapterObject
    }

    if ((-not $SkipApp) -or $ViewTests -or $Tests) {
        $resourceObject = Join-Path $buildDir 'app-resource.o'
        $resourceFlags = @('-I', 'resources', '-I', 'src', '-i', 'resources/app.rc', '-o', $resourceObject, '-O', 'coff', '--codepage=65001')
        if (Test-Path -LiteralPath 'resources/app.ico') { $resourceFlags += '-DTOKENSCOPE_HAVE_ICON=1' }
        Invoke-Compiler $windres $resourceFlags
        $viewObject = Join-Path $buildDir 'view.o'
        Invoke-Compiler $cxx ($cxxFlags + @('-c', 'src/view.cpp', '-o', $viewObject))
    }
    # Native shell modules (window, tray, dialogs, workers). Linked by the app and by main_ui_tests.
    $shellObjects = @()
    if ((-not $SkipApp) -or $Tests) {
        foreach ($module in @('state', 'settings', 'workers', 'watcher', 'budget', 'theme', 'dialogs', 'sources', 'window')) {
            $shellObject = Join-Path $buildDir "shell-$module.o"
            Invoke-Compiler $cxx ($cxxFlags + @('-c', "src/shell/$module.cpp", '-o', $shellObject))
            $shellObjects += $shellObject
        }
    }
    if (-not $SkipApp) {
        $mainObject = Join-Path $buildDir 'main.o'
        Invoke-Compiler $cxx ($cxxFlags + @('-c', 'src/main.cpp', '-o', $mainObject))
        $exe = Join-Path $distDir $ExecutableName
        Invoke-Compiler $cxx (@($mainObject, $viewObject, $resourceObject) + $shellObjects + $dataObjects +
            $linkFlags + @('-municode', '-mwindows', '-o', $exe) + $systemLibraries)
        if ($Configuration -eq 'Release') { Invoke-Compiler $strip @('--strip-all', $exe) }
        $signed = $false
        if ($SignCertificateThumbprint -or $SignPfx) {
            # Sign after stripping and before hashing, so the report describes the shipped bytes.
            if (-not $SignTool) {
                $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
                $SignTool = Get-ChildItem -LiteralPath $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
                    Where-Object { $_.Directory.Name -eq 'x64' } | Sort-Object FullName -Descending |
                    Select-Object -First 1 -ExpandProperty FullName
            }
            if (-not $SignTool -or -not (Test-Path -LiteralPath $SignTool)) {
                throw 'signtool.exe not found. Install the Windows SDK or pass -SignTool <path>.'
            }
            $signArgs = @('sign', '/fd', 'SHA256', '/tr', $TimestampUrl, '/td', 'SHA256', '/d', 'TokenScope')
            if ($SignPfx) {
                if (-not $env:TOKENSCOPE_SIGN_PASSWORD) { throw 'Set TOKENSCOPE_SIGN_PASSWORD for -SignPfx.' }
                $signArgs += @('/f', $SignPfx, '/p', $env:TOKENSCOPE_SIGN_PASSWORD)
            } else {
                $signArgs += @('/sha1', $SignCertificateThumbprint)
            }
            & $SignTool @($signArgs + @($exe))
            if ($LASTEXITCODE -ne 0) { throw "Code signing failed ($LASTEXITCODE)." }
            & $SignTool verify /pa $exe | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Signature verification failed.' }
            $signed = $true
        }
        $file = Get-Item -LiteralPath $exe
        $sizeGatePassed = $file.Length -le 10000000
        if (($Configuration -eq 'Release') -and (-not $sizeGatePassed)) { throw "EXE size gate failed: $($file.Length) bytes exceeds 10,000,000." }
        $imports = & $readobj '--coff-imports' $exe
        if ($LASTEXITCODE -ne 0) { throw 'Could not inspect executable imports.' }
        $importNames = @($imports | Select-String '^\s*Name: (.+\.dll)\s*$' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
        if ($importNames | Where-Object { $_ -match '^(libc\+\+|libunwind|libwinpthread|libgcc|msvcp[0-9]|vcruntime)' }) {
            throw "Portable runtime gate failed; non-system runtime DLL detected: $($importNames -join ', ')"
        }
        $systemDlls = @('advapi32.dll', 'bcrypt.dll', 'comctl32.dll', 'comdlg32.dll', 'crypt32.dll',
            'd2d1.dll', 'dwrite.dll', 'dwmapi.dll', 'gdi32.dll', 'imm32.dll', 'kernel32.dll',
            'msimg32.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'propsys.dll', 'rpcrt4.dll',
            'secur32.dll', 'shell32.dll', 'shlwapi.dll', 'ucrtbase.dll', 'user32.dll', 'userenv.dll',
            'version.dll', 'windowscodecs.dll', 'winhttp.dll', 'winmm.dll', 'ws2_32.dll')
        $unexpectedDlls = @($importNames | Where-Object {
            ($_ -notmatch '^(api-ms-win-|ext-ms-win-)') -and ($_.ToLowerInvariant() -notin $systemDlls)
        })
        if ($unexpectedDlls.Count -gt 0) { throw "Portable runtime gate failed; DLL not in Windows system allowlist: $($unexpectedDlls -join ', ')" }
        $report = [ordered]@{
            product = 'TokenScope'; executable = $ExecutableName; configuration = $Configuration; architecture = 'x64';
            bytes = $file.Length; maximumBytes = 10000000; sizeGatePassed = $sizeGatePassed;
            sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant();
            imports = $importNames; toolchain = 'LLVM-MinGW 20261006 UCRT x86_64'; signed = $signed;
            builtAtUtc = [DateTime]::UtcNow.ToString('o')
        }
        $reportName = if ($ExecutableName -eq 'TokenScope.exe') { 'build-report.json' } else { [IO.Path]::GetFileNameWithoutExtension($ExecutableName) + '.build-report.json' }
        $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $distDir $reportName) -Encoding UTF8
        Write-Host "Built $exe ($($file.Length) bytes; size gate: $sizeGatePassed; system-DLL gate passed)."
    }
    if ($Tests) {
        foreach ($testName in @('core_tests', 'domestic_tests', 'cli_tests', 'export_tests', 'sources_integration_tests', 'pricing_tests', 'extended_tests', 'extended_family_tests', 'pricing_store_tests', 'exchange_tests', 'source_config_tests', 'connections_tests', 'credentials_tests')) {
            $testObject = Join-Path $buildDir "$testName.o"
            Invoke-Compiler $cxx ($testFlags + @('-c', "tests/$testName.cpp", '-o', $testObject))
            Invoke-Compiler $cxx (@($testObject) + $dataObjects +
                $linkFlags + @('-o', (Join-Path $buildDir "$testName.exe")) + $systemLibraries)
        }
        # Exercises the actual native dialogs and preference lifecycle in its
        # own test process; it links the shell modules but not main.o (wWinMain).
        $mainUiObject = Join-Path $buildDir 'main_ui_tests.o'
        Invoke-Compiler $cxx ($testFlags + @('-c', 'tests/main_ui_tests.cpp', '-o', $mainUiObject))
        Invoke-Compiler $cxx (@($mainUiObject, $viewObject, $resourceObject) + $shellObjects + $dataObjects +
            $linkFlags + @('-o', (Join-Path $buildDir 'main_ui_tests.exe')) + $systemLibraries)
    }
    if ($ViewTests) {
        $viewTestObject = Join-Path $buildDir 'view-tests.o'
        Invoke-Compiler $cxx ($testFlags + @('-c', 'tests/view_tests.cpp', '-o', $viewTestObject))
        Invoke-Compiler $cxx (@($viewTestObject, $viewObject, $resourceObject) + $dataObjects +
            $linkFlags + @('-o', (Join-Path $buildDir 'view_tests.exe')) + $systemLibraries)
    }
} finally {
    Pop-Location
}
