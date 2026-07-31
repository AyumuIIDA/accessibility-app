param(
    [Parameter(Mandatory = $true)]
    [string]$Path
)

$resolvedPath = (Resolve-Path -LiteralPath $Path).Path
$rows = @(Import-Csv -LiteralPath $resolvedPath)

if ($rows.Count -eq 0) {
    throw "The CSV does not contain any rows."
}
if (-not ($rows[0].PSObject.Properties.Name -contains 'gate_rebase_offset_deg')) {
    throw "The CSV predates the rebase drift columns. Record a new log."
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Ratio
    )
    if ($Values.Count -eq 0) {
        return [double]::NaN
    }
    $ordered = $Values | Sort-Object
    $index = [Math]::Min(
        $ordered.Count - 1,
        [Math]::Max(0, [Math]::Ceiling($Ratio * $ordered.Count) - 1))
    return [double]$ordered[$index]
}

$stateNames = @{
    '0' = 'Uninitialized'
    '1' = 'Tracking'
    '2' = 'Holding'
    '3' = 'Reacquiring'
}
$rejectionNames = @{
    '0' = 'None'
    '1' = 'Invalid'
    '2' = 'AngularJump'
    '3' = 'AngularSpeed'
}

$firstTimestamp = [double]$rows[0].timestamp_us
$lastTimestamp = [double]$rows[-1].timestamp_us
$durationSeconds = ($lastTimestamp - $firstTimestamp) / 1e6

Write-Output "Rotation gate log: $resolvedPath"
Write-Output ("Rows {0}, duration {1:F1} s, {2:F1} rows/s, frames {3}..{4}" -f `
    $rows.Count, $durationSeconds, ($rows.Count / [Math]::Max($durationSeconds, 1e-6)),
    $rows[0].frame_id, $rows[-1].frame_id)

if ($rows[0].PSObject.Properties.Name -contains 'active_track_id') {
    $unbound = @($rows | Where-Object { $_.active_track_id -eq '0' })
    $bound = @($rows | Where-Object { $_.active_track_id -ne '0' })
    Write-Output ("Rows with no rotation session bound: {0} of {1}" -f `
        $unbound.Count, $rows.Count)
    if ($bound.Count -eq 0) {
        Write-Output ''
        Write-Output 'No rotation session was ever started in this recording.'
        Write-Output 'Open the 3D viewer, give it keyboard focus, and hold Space.'
        $tracked = @($unbound | Where-Object {
            [double]$_.tracking_quality -gt 0.7
        })
        Write-Output ("Hand tracking was healthy in {0} of {1} rows, so this is" -f `
            $tracked.Count, $unbound.Count)
        Write-Output 'a missing clutch, not a perception failure.'
        exit 0
    }
}

Write-Output ''
Write-Output 'Gate state:'
$rows | Group-Object gate_state | Sort-Object Name | ForEach-Object {
    Write-Output ("  {0,-14} {1}" -f $stateNames[$_.Name], $_.Count)
}
Write-Output 'Gate rejection:'
$rows | Group-Object gate_rejection | Sort-Object Name | ForEach-Object {
    Write-Output ("  {0,-14} {1}" -f $rejectionNames[$_.Name], $_.Count)
}

# --- Verification criteria from doc/3d-hand-model-handoff.md ---
$angularSpeed = @($rows | Where-Object { $_.gate_rejection -eq '3' })
$angularJump = @($rows | Where-Object { $_.gate_rejection -eq '2' })
$overThreshold = @($rows | Where-Object {
    [double]$_.gate_raw_delta_deg -gt 90.0
})
$jumpsUnderThreshold = @($angularJump | Where-Object {
    [double]$_.gate_raw_delta_deg -le 90.0
})

$hasOutlierColumn = $rows[0].PSObject.Properties.Name -contains 'gate_outlier_frames'
if ($hasOutlierColumn) {
    $absorbedRuns = 0
    $absorbedFrames = 0
    $previousOutlier = 0
    foreach ($row in $rows) {
        $current = [int]$row.gate_outlier_frames
        if ($current -gt 0) { $absorbedFrames++ }
        if ($current -eq 1 -and $previousOutlier -eq 0) { $absorbedRuns++ }
        $previousOutlier = $current
    }
    $escalated = @($rows | Where-Object {
        $_.gate_rejection -eq '2' -and [int]$_.gate_outlier_frames -eq 0
    })
    Write-Output ''
    Write-Output 'Transient outliers absorbed without rebasing:'
    Write-Output ("  Outlier runs                      {0}" -f $absorbedRuns)
    Write-Output ("  Frames held                       {0}" -f $absorbedFrames)
    Write-Output ("  Runs escalated to recovery        {0}" -f $escalated.Count)
}

if ($rows[0].PSObject.Properties.Name -contains 'rebind_candidate_handedness') {
    # Perception drops a track slot after its own grace, so a hand returning
    # later is a new track whose handedness is never compared with the lost one.
    # This measures whether handedness would have been a usable identity.
    $rebindFrames = @($rows | Where-Object {
        [double]$_.rebind_candidate_handedness -ge 0 -and
        [double]$_.last_handedness -ge 0
    })
    Write-Output ''
    Write-Output 'Handedness across tracking gaps:'
    Write-Output ("  Frames offering a rebind candidate: {0}" -f $rebindFrames.Count)
    if ($rebindFrames.Count -gt 0) {
        $agree = 0
        $ambiguous = 0
        $deltas = New-Object System.Collections.ArrayList
        foreach ($row in $rebindFrames) {
            $last = [double]$row.last_handedness
            $candidate = [double]$row.rebind_candidate_handedness
            [void]$deltas.Add([Math]::Abs($candidate - $last))
            $lastHigh = $last -ge 0.8
            $lastLow = $last -le 0.2
            $candidateHigh = $candidate -ge 0.8
            $candidateLow = $candidate -le 0.2
            if ((-not $lastHigh -and -not $lastLow) -or
                (-not $candidateHigh -and -not $candidateLow)) {
                $ambiguous++
            }
            elseif (($lastHigh -and $candidateHigh) -or ($lastLow -and $candidateLow)) {
                $agree++
            }
        }
        $values = [double[]]$deltas.ToArray()
        Write-Output ("  Same reliable side:                 {0}" -f $agree)
        Write-Output ("  Opposite reliable side:             {0}" -f `
            ($rebindFrames.Count - $agree - $ambiguous))
        Write-Output ("  Ambiguous (0.2 - 0.8) either side:  {0}" -f $ambiguous)
        Write-Output ("  |candidate - last| median {0:F3}, p90 {1:F3}, max {2:F3}" -f `
            (Get-Percentile $values 0.50),
            (Get-Percentile $values 0.90),
            (Get-Percentile $values 1.0))
    }
}

Write-Output ''
Write-Output 'Verification:'
Write-Output ("  AngularSpeed rejections           {0} (expected 0)" -f $angularSpeed.Count)
Write-Output ("  AngularJump rejections            {0}" -f $angularJump.Count)
Write-Output ("  Raw deltas above 90 deg           {0}" -f $overThreshold.Count)
Write-Output ("  Jumps rejected below 90 deg       {0} (expected 0)" -f $jumpsUnderThreshold.Count)

# --- Reference drift injected by gate recoveries ---
$recoveries = @($rows | Where-Object { $_.gate_reacquired -eq '1' })
$steps = [double[]]@($recoveries | ForEach-Object { [double]$_.gate_rebase_step_deg })
$finalOffset = [double]$rows[-1].gate_rebase_offset_deg
$offsets = [double[]]@($rows | ForEach-Object { [double]$_.gate_rebase_offset_deg })
$stepSum = 0.0
foreach ($step in $steps) { $stepSum += $step }

Write-Output ''
Write-Output 'Reference drift (published output vs raw absolute measurement):'
Write-Output ("  Recoveries                        {0}" -f $recoveries.Count)
Write-Output ("  Injected offset sum               {0:F1} deg" -f $stepSum)
Write-Output ("  Net offset at end of log          {0:F1} deg" -f $finalOffset)
Write-Output ("  Peak net offset                   {0:F1} deg" -f (
    Get-Percentile $offsets 1.0))
if ($durationSeconds -gt 0) {
    Write-Output ("  Injected offset rate              {0:F1} deg/min" -f (
        $stepSum * 60.0 / $durationSeconds))
}
if ($recoveries.Count -gt 0) {
    Write-Output '  Per recovery:'
    foreach ($recovery in $recoveries) {
        Write-Output ("    frame {0,6}  step {1,7:F2} deg  net {2,7:F2} deg" -f `
            $recovery.frame_id,
            [double]$recovery.gate_rebase_step_deg,
            [double]$recovery.gate_rebase_offset_deg)
    }
}

# --- Output continuity ---
$gapRuns = New-Object System.Collections.ArrayList
$runLength = 0
$runStart = $null
$previous = $null
foreach ($row in $rows) {
    if ($row.filtered_valid -eq '0') {
        if ($runLength -eq 0) { $runStart = $row }
        $runLength++
    }
    elseif ($runLength -gt 0) {
        [void]$gapRuns.Add([pscustomobject]@{
            Frames = $runLength
            Milliseconds = ([double]$previous.timestamp_us - [double]$runStart.timestamp_us) / 1000.0
            FirstFrame = $runStart.frame_id
            LastFrame = $previous.frame_id
        })
        $runLength = 0
    }
    $previous = $row
}
if ($runLength -gt 0) {
    [void]$gapRuns.Add([pscustomobject]@{
        Frames = $runLength
        Milliseconds = ([double]$previous.timestamp_us - [double]$runStart.timestamp_us) / 1000.0
        FirstFrame = $runStart.frame_id
        LastFrame = $previous.frame_id
    })
}

Write-Output ''
Write-Output ("Output gaps (filtered_valid = 0): {0} runs" -f $gapRuns.Count)
$gapRuns | Sort-Object Frames -Descending | Select-Object -First 8 | Format-Table -AutoSize

# --- Stale track binding signature ---
$staleTrack = @($rows | Where-Object {
    $_.six_valid -eq '1' -and $_.basis_valid -eq '0'
})
Write-Output ("Stale-track signature rows (six_valid=1, basis_valid=0): {0}" -f $staleTrack.Count)

# --- Jitter floor over quasi-stationary tracking windows ---
$tracking = @($rows | Where-Object {
    $_.gate_state -eq '1' -and $_.gate_rejection -eq '0' -and $_.basis_valid -eq '1'
})
$window = 5
$stationary = New-Object System.Collections.ArrayList
for ($index = $window; $index -lt ($tracking.Count - $window); $index++) {
    $before = [double]$tracking[$index - $window].basis_angle_deg
    $after = [double]$tracking[$index + $window].basis_angle_deg
    if ([Math]::Abs($after - $before) -lt 1.0) {
        [void]$stationary.Add([double]$tracking[$index].gate_raw_delta_deg)
    }
}
$stationaryValues = [double[]]$stationary.ToArray()

Write-Output ''
Write-Output 'Jitter floor (per-frame delta while the palm is quasi-stationary):'
Write-Output ("  Samples {0}, median {1:F2} deg, p90 {2:F2} deg, max {3:F2} deg" -f `
    $stationaryValues.Count,
    (Get-Percentile $stationaryValues 0.50),
    (Get-Percentile $stationaryValues 0.90),
    (Get-Percentile $stationaryValues 1.0))

$deltas = [double[]]@($tracking | ForEach-Object { [double]$_.gate_raw_delta_deg })
Write-Output ("Raw consecutive delta: median {0:F2} deg, p90 {1:F2} deg, p99 {2:F2} deg, max {3:F2} deg" -f `
    (Get-Percentile $deltas 0.50),
    (Get-Percentile $deltas 0.90),
    (Get-Percentile $deltas 0.99),
    (Get-Percentile $deltas 1.0))
