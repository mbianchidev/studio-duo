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
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $ArgumentList) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        Write-Host "$Description (timeout: $TimeoutSeconds seconds)"
        $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
        if (-not $process.Start()) {
            throw "$Description could not be started."
        }
        $standardOutput = $process.StandardOutput.ReadToEndAsync()
        $standardError = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill($true)
                $process.WaitForExit()
            } catch {
                Write-Warning "Could not terminate timed-out process $($process.Id): $_"
            }
            throw "$Description timed out after $TimeoutSeconds seconds."
        }
        $remainingMilliseconds = [int][Math]::Max(
            0,
            $TimeoutSeconds * 1000 - $elapsed.ElapsedMilliseconds
        )
        if (-not [System.Threading.Tasks.Task]::WaitAll(
            [System.Threading.Tasks.Task[]]@($standardOutput, $standardError),
            $remainingMilliseconds
        )) {
            throw "$Description output collection timed out after $TimeoutSeconds seconds."
        }
        foreach ($outputTask in @($standardOutput, $standardError)) {
            $output = $outputTask.GetAwaiter().GetResult()
            if (-not [string]::IsNullOrWhiteSpace($output)) {
                Write-Host $output.TrimEnd()
            }
        }
        if ($process.ExitCode -ne 0) {
            throw "$Description failed with exit code $($process.ExitCode)."
        }
    } finally {
        $process.Dispose()
    }
}
