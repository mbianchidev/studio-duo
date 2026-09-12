function Invoke-StudioDuoProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [string[]] $ArgumentList = @(),

        [Parameter(Mandatory = $true)]
        [string] $Description,

        [ValidateRange(1, 900)]
        [int] $TimeoutSeconds = 120
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    foreach ($argument in $ArgumentList) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        Write-Host "$Description (timeout: $TimeoutSeconds seconds)"
        if (-not $process.Start()) {
            throw "$Description could not be started."
        }
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill($true)
                $process.WaitForExit()
            } catch {
                Write-Warning "Could not terminate timed-out process $($process.Id): $_"
            }
            throw "$Description timed out after $TimeoutSeconds seconds."
        }
        if ($process.ExitCode -ne 0) {
            throw "$Description failed with exit code $($process.ExitCode)."
        }
    } finally {
        $process.Dispose()
    }
}
