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

    [string] $RepositoryRoot = (Join-Path $PSScriptRoot '..\..'),

    [string] $TimestampUrl = 'http://timestamp.digicert.com',

    [switch] $TrustSigningCertificate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'process-utils.ps1')

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
if (
    $TrustSigningCertificate -and
    ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted')
) {
    throw 'Temporary signing-certificate trust is only allowed on disposable GitHub-hosted runners.'
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

    Write-Host "Signing $Path"
    $signArguments = @(
        '/f', $certificatePath,
        '/p', $CertificatePassword,
        '/fd', 'SHA256',
        '/d', 'Studio Duo',
        '/du', 'https://github.com/mbianchidev/studio-duo'
    )
    if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
        $signArguments += @(
            '/td', 'SHA256',
            '/tr', $TimestampUrl
        )
    }
    $signArguments += $Path

    Invoke-StudioDuoProcess `
        -FilePath $signTool.FullName `
        -ArgumentList (@('sign') + $signArguments) `
        -Description "Authenticode signing $Path" `
        -TimeoutSeconds 120

    Write-Host "Verifying $Path"
    Invoke-StudioDuoProcess `
        -FilePath $signTool.FullName `
        -ArgumentList @('verify', '/pa', '/v', $Path) `
        -Description "Authenticode verification $Path" `
        -TimeoutSeconds 60
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if (
        $signature.Status -ne 'Valid' -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $signingCertificate.Thumbprint
    ) {
        throw "Authenticode signer verification failed for ${Path}: $($signature.StatusMessage)"
    }
}

$signingCertificate = $null
$publicSigningCertificate = $null
$trustStore = $null
$addedCertificateTrust = $false
try {
    $certificateBytes = [Convert]::FromBase64String(
        ($CertificateBase64 -replace '\s', '')
    )
    [System.IO.File]::WriteAllBytes($certificatePath, $certificateBytes)
    $signingCertificate =
        [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
            $certificatePath,
            $CertificatePassword,
            [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet
        )

    if ($TrustSigningCertificate) {
        $publicSigningCertificate =
            [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
                $signingCertificate.RawData
            )
        $trustStore =
            [System.Security.Cryptography.X509Certificates.X509Store]::new(
                'Root',
                [System.Security.Cryptography.X509Certificates.StoreLocation]::CurrentUser
            )
        $trustStore.Open(
            [System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite
        )
        $existingCertificates = $trustStore.Certificates.Find(
            [System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
            $publicSigningCertificate.Thumbprint,
            $false
        )
        if ($existingCertificates.Count -eq 0) {
            Write-Host 'Temporarily trusting the configured public signing certificate'
            $trustStore.Add($publicSigningCertificate)
            $addedCertificateTrust = $true
        }
    }

    Invoke-CodeSigning $executablePath
    Write-Host 'Building Inno Setup installer'
    $installerOutput = & (Join-Path $PSScriptRoot 'build-installer.ps1') `
        -Version $Version `
        -Executable $executablePath `
        -OutputDirectory $outputDirectoryPath `
        -RepositoryRoot $repositoryRootPath
    if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
        throw "Windows installer was not produced at $installerPath. Builder output: $installerOutput"
    }
    Invoke-CodeSigning $installerPath

    Write-Host 'Building portable ZIP'
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
    Copy-Item `
        -LiteralPath (Join-Path (Split-Path -Parent $executablePath) 'licenses') `
        -Destination (Join-Path $stagingDirectory 'licenses') `
        -Recurse
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
    try {
        if ($addedCertificateTrust) {
            Write-Host 'Removing temporary signing-certificate trust'
            $trustStore.Remove($publicSigningCertificate)
        }
    } finally {
        if ($null -ne $trustStore) {
            $trustStore.Dispose()
        }
        if ($null -ne $publicSigningCertificate) {
            $publicSigningCertificate.Dispose()
        }
        if ($null -ne $signingCertificate) {
            $signingCertificate.Dispose()
        }
        try {
            if (Test-Path -LiteralPath $certificatePath) {
                Remove-Item -LiteralPath $certificatePath -Force
            }
        } finally {
            if (Test-Path -LiteralPath $stagingDirectory) {
                Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
            }
        }
    }
}
