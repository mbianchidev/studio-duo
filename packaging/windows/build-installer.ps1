[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $Version,

    [Parameter(Mandatory = $true)]
    [string] $Executable,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory,

    [string] $RepositoryRoot = (Join-Path $PSScriptRoot '..\..')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process-utils.ps1')

$innoVersion = '7.1.0'
$innoInstallerUri =
    "https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/innosetup-$innoVersion-x64.exe"
$innoInstallerSha256 =
    '0362A383ED217D4C4239B5933866DD96D3EB2102737DA92F80F6057A4B40DF2F'
$vcRedistUri = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string] $Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath(
        (Join-Path (Get-Location).Path $Path)
    )
}

$repositoryRootPath = Get-FullPath $RepositoryRoot
$executablePath = Get-FullPath $Executable
$licenseDirectory = Join-Path (Split-Path -Parent $executablePath) 'licenses'
$outputDirectoryPath = Get-FullPath $OutputDirectory
$installerScript = Join-Path $repositoryRootPath 'packaging\windows\StudioDuo.iss'

foreach ($requiredFile in @(
    $executablePath,
    $installerScript,
    (Join-Path $repositoryRootPath 'README.md'),
    (Join-Path $repositoryRootPath 'LICENSE'),
    (Join-Path $licenseDirectory 'lame-COPYING.txt'),
    (Join-Path $licenseDirectory 'lame-NOTICE.txt'),
    (Join-Path $licenseDirectory 'lame-config.h'),
    (Join-Path $licenseDirectory 'lame-3.100-source.tar.gz'),
    (Join-Path $licenseDirectory 'studio-duo-source.tar.gz'),
    (Join-Path $repositoryRootPath 'assets\branding\StudioDuo.ico')
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required installer input is missing: $requiredFile"
    }
}

New-Item -ItemType Directory -Path $outputDirectoryPath -Force | Out-Null
$temporaryRoot = if ($env:RUNNER_TEMP) {
    $env:RUNNER_TEMP
} else {
    [System.IO.Path]::GetTempPath()
}
$workDirectory = Join-Path $temporaryRoot "studio-duo-installer-$PID"
New-Item -ItemType Directory -Path $workDirectory -Force | Out-Null

try {
    $innoInstaller = Join-Path $workDirectory "innosetup-$innoVersion-x64.exe"
    Write-Host "Downloading Inno Setup $innoVersion"
    Invoke-WebRequest `
        -Uri $innoInstallerUri `
        -OutFile $innoInstaller `
        -TimeoutSec 120
    $actualInnoHash = (Get-FileHash -LiteralPath $innoInstaller -Algorithm SHA256).Hash
    if ($actualInnoHash -ne $innoInstallerSha256) {
        throw "Inno Setup checksum mismatch: expected $innoInstallerSha256, got $actualInnoHash"
    }

    $innoDirectory = Join-Path $workDirectory 'Inno Setup'
    Write-Host "Installing Inno Setup $innoVersion"
    $innoInstallArguments = @(
        '/CURRENTUSER',
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        '/SP-',
        "/DIR=$innoDirectory"
    )
    Invoke-StudioDuoProcess `
        -FilePath $innoInstaller `
        -ArgumentList $innoInstallArguments `
        -Description "Installing Inno Setup $innoVersion" `
        -TimeoutSeconds 120

    $innoCompiler = Join-Path $innoDirectory 'ISCC.exe'
    if (-not (Test-Path -LiteralPath $innoCompiler -PathType Leaf)) {
        throw "Inno Setup compiler was not installed at $innoCompiler"
    }

    $vcRedist = Join-Path $workDirectory 'vc_redist.x64.exe'
    Write-Host 'Downloading Microsoft Visual C++ x64 Runtime'
    Invoke-WebRequest `
        -Uri $vcRedistUri `
        -OutFile $vcRedist `
        -TimeoutSec 120
    $vcSignature = Get-AuthenticodeSignature -LiteralPath $vcRedist
    if (
        $vcSignature.Status -ne 'Valid' -or
        $null -eq $vcSignature.SignerCertificate -or
        $vcSignature.SignerCertificate.Subject -notmatch 'Microsoft Corporation'
    ) {
        throw 'The downloaded Visual C++ Runtime does not have a valid Microsoft signature.'
    }

    $vcFileVersion = (Get-Item -LiteralPath $vcRedist).VersionInfo.FileVersion
    if ($vcFileVersion -notmatch '(?<Version>\d+\.\d+\.\d+\.\d+)') {
        throw "Could not determine the Visual C++ Runtime version from '$vcFileVersion'."
    }
    $vcVersion = $Matches.Version

    $installerEnvironment = @{
        STUDIO_DUO_VERSION = $Version
        STUDIO_DUO_EXECUTABLE = $executablePath
        STUDIO_DUO_LICENSE_DIRECTORY = $licenseDirectory
        STUDIO_DUO_SOURCE_DIRECTORY = $repositoryRootPath
        STUDIO_DUO_INSTALLER_OUTPUT_DIRECTORY = $outputDirectoryPath
        STUDIO_DUO_VC_REDIST = $vcRedist
        STUDIO_DUO_VC_REDIST_VERSION = $vcVersion
    }
    $previousEnvironment = @{}
    foreach ($entry in $installerEnvironment.GetEnumerator()) {
        $previousEnvironment[$entry.Key] =
            [Environment]::GetEnvironmentVariable($entry.Key, 'Process')
        [Environment]::SetEnvironmentVariable(
            $entry.Key,
            $entry.Value,
            'Process'
        )
    }

    try {
        Write-Host 'Compiling Studio Duo installer'
        Invoke-StudioDuoProcess `
            -FilePath $innoCompiler `
            -ArgumentList @($installerScript) `
            -Description 'Compiling Studio Duo installer' `
            -TimeoutSeconds 120
    } finally {
        foreach ($entry in $previousEnvironment.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable(
                $entry.Key,
                $entry.Value,
                'Process'
            )
        }
    }

    $installer = Join-Path $outputDirectoryPath (
        "Studio-Duo-$Version-Windows-x64-Setup.exe"
    )
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
        throw "Inno Setup did not produce the expected installer: $installer"
    }

    Write-Output $installer
} finally {
    Remove-Item -LiteralPath $workDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
