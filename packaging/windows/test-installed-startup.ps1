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
$applicationData = Join-Path $env:APPDATA 'Studio Duo'
New-Item -ItemType Directory -Path $applicationData -Force | Out-Null
$userDataFixture = Join-Path $applicationData (
    'install-smoke-synthetic-' + [guid]::NewGuid().ToString('N') + '.json'
)
Set-Content -LiteralPath $userDataFixture -Value '{"syntheticUserData":true}' -Encoding utf8
$userDataHash = (Get-FileHash -LiteralPath $userDataFixture -Algorithm SHA256).Hash
$installedExecutable = Join-Path $installDirectory 'Studio Duo.exe'
$installArguments = @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOICONS',
    "/DIR=$installDirectory"
)

try {
    Invoke-StudioDuoProcess `
        -FilePath $installerPath `
        -ArgumentList ($installArguments + @(
            "/LOG=$(Join-Path $workDirectory 'install.log')"
        )) `
        -Description 'Installing Studio Duo into the smoke-test directory' `
        -TimeoutSeconds 180

    foreach ($licenseName in @(
        'lame-COPYING.txt', 'lame-NOTICE.txt', 'lame-config.h',
        'lame-4.0-source.tar.gz', 'studio-duo-source.tar.gz',
        'signalsmith-stretch-LICENSE.txt', 'signalsmith-linear-LICENSE.txt'
    )) {
        $licensePath = Join-Path $installDirectory "licenses\$licenseName"
        if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) {
            throw "Installed dependency source/license material is missing: $licensePath"
        }
        if ((Get-Item -LiteralPath $licensePath).Length -eq 0) {
            throw "Installed dependency source/license material is empty: $licensePath"
        }
    }

    Invoke-StudioDuoProcess `
        -FilePath $installedExecutable `
        -ArgumentList @('--startup-self-test') `
        -Description 'Starting the installed Studio Duo window without hardware drivers' `
        -TimeoutSeconds 30

    $executableHash = (Get-FileHash -LiteralPath $installedExecutable -Algorithm SHA256).Hash
    Invoke-StudioDuoProcess `
        -FilePath $installerPath `
        -ArgumentList ($installArguments + @(
            "/LOG=$(Join-Path $workDirectory 'reinstall.log')"
        )) `
        -Description 'Reinstalling the same version to validate the replacement path' `
        -TimeoutSeconds 180
    if ((Get-FileHash -LiteralPath $installedExecutable -Algorithm SHA256).Hash -ne $executableHash) {
        throw 'Reinstallation changed the installed application executable.'
    }
    if (
        -not (Test-Path -LiteralPath $userDataFixture -PathType Leaf) -or
        (Get-FileHash -LiteralPath $userDataFixture -Algorithm SHA256).Hash -ne $userDataHash
    ) {
        throw 'Reinstallation changed or removed existing user data.'
    }
    Invoke-StudioDuoProcess `
        -FilePath $installedExecutable `
        -ArgumentList @('--startup-self-test') `
        -Description 'Starting the reinstalled application without hardware drivers' `
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
        try {
            if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
                Invoke-StudioDuoProcess `
                    -FilePath $uninstaller `
                    -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') `
                    -Description 'Removing the smoke-test installation' `
                    -TimeoutSeconds 120
            }
        } finally {
            try {
                $cleanupDeadline = (Get-Date).AddSeconds(10)
                while (
                    (
                        (Test-Path -LiteralPath $installedExecutable) -or
                        (Test-Path -LiteralPath $uninstaller)
                    ) -and
                    (Get-Date) -lt $cleanupDeadline
                ) {
                    Start-Sleep -Milliseconds 100
                }
                if (
                    (Test-Path -LiteralPath $installedExecutable) -or
                    (Test-Path -LiteralPath $uninstaller)
                ) {
                    throw 'Uninstallation left application executables in the smoke-test directory.'
                }
                if (
                    -not (Test-Path -LiteralPath $userDataFixture -PathType Leaf) -or
                    (Get-FileHash -LiteralPath $userDataFixture -Algorithm SHA256).Hash -ne $userDataHash
                ) {
                    throw 'Uninstallation changed or removed user data.'
                }
            } finally {
                if (Test-Path -LiteralPath $userDataFixture -PathType Leaf) {
                    Remove-Item -LiteralPath $userDataFixture -Force -ErrorAction Stop
                }
            }
        }
    }
}
