# Run with MK8_GPU_SKIP=$Skip for $Timeout seconds; print GPU lines of the log.
param([int]$Skip = 0, [int]$Timeout = 40)
$env:MK8_GPU_SKIP = "$Skip"
& "$PSScriptRoot\run.ps1" -NoBuild -Timeout $Timeout -Tail 1 | Out-Null
Select-String -Path "$PSScriptRoot\..\build\run.log" -Pattern "gpu: (draw skipped|[0-9]+ draws)|fatal|exception|violation|frames presented" | Select-Object -First 12 | ForEach-Object { $_.Line }
