[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Installer
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process-utils.ps1')

if (
    $env:GITHUB_ACTIONS -ne 'true' -or
    $env:RUNNER_ENVIRONMENT -ne 'github-hosted'
) {
    throw 'Installer smoke tests may only run on disposable GitHub-hosted runners.'
}
if (-not (Test-Path -LiteralPath $Installer -PathType Leaf)) {
    throw "The Windows installer is missing: $Installer"
}

$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$workDirectory = Join-Path $env:RUNNER_TEMP (
    'studio-duo-install-smoke-' + [guid]::NewGuid().ToString('N')
)
$installDirectory = Join-Path $workDirectory 'app'
New-Item -ItemType Directory -Path $workDirectory | Out-Null
Write-Host "Installer smoke-test diagnostics: $workDirectory"

try {
    Invoke-StudioDuoProcess `
        -FilePath $installerPath `
        -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOICONS',
            "/DIR=$installDirectory",
            "/LOG=$(Join-Path $workDirectory 'install.log')"
        ) `
        -Description 'Installing Studio Duo into the smoke-test directory' `
        -TimeoutSeconds 180

    Invoke-StudioDuoProcess `
        -FilePath (Join-Path $installDirectory 'Studio Duo.exe') `
        -ArgumentList @('--startup-self-test') `
        -Description 'Starting the installed Studio Duo window without hardware drivers' `
        -TimeoutSeconds 30
} finally {
    try {
        $applicationLogs = Join-Path $env:APPDATA 'Studio Duo\Logs'
        if (Test-Path -LiteralPath $applicationLogs -PathType Container) {
            Copy-Item `
                -LiteralPath $applicationLogs `
                -Destination (Join-Path $workDirectory 'AppLogs') `
                -Recurse
        }
    } finally {
        $uninstaller = Join-Path $installDirectory 'unins000.exe'
        if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
            Invoke-StudioDuoProcess `
                -FilePath $uninstaller `
                -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') `
                -Description 'Removing the smoke-test installation' `
                -TimeoutSeconds 120
        }
    }
}
