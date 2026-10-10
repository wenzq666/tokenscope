[CmdletBinding()]
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release', [string]$ToolchainRoot)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'check-translations.ps1')
$buildArgs = @{ Configuration = $Configuration; Tests = $true; SkipApp = $true }
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot '..\tests\view_tests.cpp')) { $buildArgs.ViewTests = $true }
if ($ToolchainRoot) { $buildArgs.ToolchainRoot = $ToolchainRoot }
& (Join-Path $PSScriptRoot 'build.ps1') @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Test build failed: $LASTEXITCODE" }
foreach ($testName in @('core_tests', 'domestic_tests', 'cli_tests', 'export_tests', 'sources_integration_tests', 'pricing_tests', 'extended_tests', 'extended_family_tests', 'pricing_store_tests', 'exchange_tests', 'source_config_tests', 'connections_tests', 'credentials_tests', 'main_ui_tests')) {
    & (Join-Path $PSScriptRoot "..\build\$Configuration\$testName.exe")
    if ($LASTEXITCODE -ne 0) { throw "$testName failed: $LASTEXITCODE" }
}
if ($buildArgs.ViewTests) {
    & (Join-Path $PSScriptRoot "..\build\$Configuration\view_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "View tests failed: $LASTEXITCODE" }
}
