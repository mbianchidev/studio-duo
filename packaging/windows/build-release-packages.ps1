[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $Version,

    [Parameter(Mandatory = $true)]
    [string] $Executable,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory,

    [Parameter(Mandatory = $true)]
    [string] $CertificateBase64,

    [Parameter(Mandatory = $true)]
    [string] $CertificatePassword,

    [string] $RepositoryRoot = (Join-Path $PSScriptRoot '..\..')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string] $Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath(
        (Join-Path (Get-Location).Path $Path)
    )
}

if ([string]::IsNullOrWhiteSpace($CertificateBase64)) {
    throw 'The Authenticode certificate is empty.'
}
if ([string]::IsNullOrWhiteSpace($CertificatePassword)) {
    throw 'The Authenticode certificate password is empty.'
}

$repositoryRootPath = Get-FullPath $RepositoryRoot
$executablePath = Get-FullPath $Executable
$outputDirectoryPath = Get-FullPath $OutputDirectory
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Studio Duo executable is missing: $executablePath"
}
New-Item -ItemType Directory -Path $outputDirectoryPath -Force | Out-Null

$signTool = Get-ChildItem `
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" `
    -File |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if ($null -eq $signTool) {
    throw 'The Windows SDK SignTool executable is unavailable.'
}

$temporaryRoot = if ($env:RUNNER_TEMP) {
    $env:RUNNER_TEMP
} else {
    [System.IO.Path]::GetTempPath()
}
$certificatePath = Join-Path $temporaryRoot "studio-duo-signing-$PID.pfx"
$stagingDirectory = Join-Path $temporaryRoot (
    "Studio-Duo-$Version-Windows-x64-$PID"
)
$installerPath = Join-Path $outputDirectoryPath (
    "Studio-Duo-$Version-Windows-x64-Setup.exe"
)
$zipPath = Join-Path $outputDirectoryPath (
    "Studio-Duo-$Version-Windows-x64.zip"
)

function Invoke-CodeSigning {
    param([Parameter(Mandatory = $true)][string] $Path)

    $global:LASTEXITCODE = 0
    & $signTool.FullName sign `
        /f $certificatePath `
        /p $CertificatePassword `
        /fd SHA256 `
        /td SHA256 `
        /tr 'http://timestamp.digicert.com' `
        /d 'Studio Duo' `
        /du 'https://github.com/mbianchidev/studio-duo' `
        $Path
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode signing failed for $Path with exit code $LASTEXITCODE"
    }

    $global:LASTEXITCODE = 0
    & $signTool.FullName verify /pa /v $Path
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode verification failed for $Path with exit code $LASTEXITCODE"
    }
}

try {
    $certificateBytes = [Convert]::FromBase64String(
        ($CertificateBase64 -replace '\s', '')
    )
    [System.IO.File]::WriteAllBytes($certificatePath, $certificateBytes)

    Invoke-CodeSigning $executablePath
    $installerOutput = & (Join-Path $PSScriptRoot 'build-installer.ps1') `
        -Version $Version `
        -Executable $executablePath `
        -OutputDirectory $outputDirectoryPath `
        -RepositoryRoot $repositoryRootPath
    if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
        throw "Windows installer was not produced at $installerPath. Builder output: $installerOutput"
    }
    Invoke-CodeSigning $installerPath

    New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null
    Copy-Item `
        -LiteralPath $executablePath `
        -Destination (Join-Path $stagingDirectory 'Studio Duo.exe')
    Copy-Item `
        -LiteralPath (Join-Path $repositoryRootPath 'README.md') `
        -Destination $stagingDirectory
    Copy-Item `
        -LiteralPath (Join-Path $repositoryRootPath 'LICENSE') `
        -Destination $stagingDirectory
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive `
        -Path $stagingDirectory `
        -DestinationPath $zipPath `
        -CompressionLevel Optimal

    if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
        throw "Portable Windows ZIP was not produced at $zipPath"
    }

    Write-Output ([pscustomobject]@{
        Installer = $installerPath
        PortableZip = $zipPath
    })
} finally {
    Remove-Item `
        -LiteralPath $certificatePath `
        -Force `
        -ErrorAction SilentlyContinue
    Remove-Item `
        -LiteralPath $stagingDirectory `
        -Recurse `
        -Force `
        -ErrorAction SilentlyContinue
}
