[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$toolsDir = Join-Path $repoRoot '.tools'
$toolchainName = 'llvm-mingw-20261006-ucrt-x86_64'
$archivePath = Join-Path $toolsDir "$toolchainName.zip"
$expectedHash = '317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a'
$url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/$toolchainName.zip"
New-Item -ItemType Directory -Force $toolsDir | Out-Null
if (-not (Test-Path -LiteralPath $archivePath)) {
    Invoke-WebRequest -Uri $url -OutFile $archivePath -TimeoutSec 300
} elseif ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    Invoke-WebRequest -Uri "$url`?download=1" -OutFile $archivePath -Resume -TimeoutSec 300
}
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'Toolchain SHA-256 mismatch. Refusing to extract or execute the downloaded archive.'
}
$compilerPath = Join-Path $toolsDir "$toolchainName\bin\x86_64-w64-mingw32-clang++.exe"
if (-not (Test-Path -LiteralPath $compilerPath)) {
    [IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $toolsDir, $true)
}
& $compilerPath --version
if ($LASTEXITCODE -ne 0) { throw 'Toolchain verification failed.' }
Write-Host "Portable toolchain ready in $toolsDir. System PATH and registry were not changed."
