param(
    [Parameter(Mandatory = $true)]
    [string]$Path
)

$resolvedPath = (Resolve-Path -LiteralPath $Path).Path
$rows = Import-Csv -LiteralPath $resolvedPath |
    Where-Object { $_.six_valid -eq '1' -and $_.basis_valid -eq '1' } |
    ForEach-Object {
        [pscustomobject]@{
            Difference = [double]$_.difference_angle_deg
            DifferenceX = [double]$_.difference_x_deg
            DifferenceY = [double]$_.difference_y_deg
            DifferenceZ = [double]$_.difference_z_deg
            SixAngle = [double]$_.six_angle_deg
            BasisAngle = [double]$_.basis_angle_deg
            FitError = [double]$_.six_fit_error
        }
    }

if (-not $rows -or $rows.Count -eq 0) {
    throw "The CSV does not contain any frames where both estimates are valid."
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Ratio
    )
    $ordered = $Values | Sort-Object
    $index = [Math]::Min(
        $ordered.Count - 1,
        [Math]::Max(0, [Math]::Ceiling($Ratio * $ordered.Count) - 1))
    return [double]$ordered[$index]
}

function Get-Rms {
    param([double[]]$Values)
    return [Math]::Sqrt(
        ($Values | ForEach-Object { $_ * $_ } | Measure-Object -Average).Average)
}

$differences = [double[]]$rows.Difference
$summary = [pscustomobject]@{
    Frames = $rows.Count
    MeanDifferenceDeg = ($differences | Measure-Object -Average).Average
    MedianDifferenceDeg = Get-Percentile $differences 0.50
    P95DifferenceDeg = Get-Percentile $differences 0.95
    MaximumDifferenceDeg = ($differences | Measure-Object -Maximum).Maximum
    RmsDifferenceDeg = Get-Rms $differences
    RmsXDeg = Get-Rms ([double[]]$rows.DifferenceX)
    RmsYDeg = Get-Rms ([double[]]$rows.DifferenceY)
    RmsZDeg = Get-Rms ([double[]]$rows.DifferenceZ)
    MeanSixPointFitError = (
        [double[]]$rows.FitError | Measure-Object -Average).Average
}

$angleBins = @(
    @{ Name = '0-15'; Minimum = 0.0; Maximum = 15.0 }
    @{ Name = '15-30'; Minimum = 15.0; Maximum = 30.0 }
    @{ Name = '30-45'; Minimum = 30.0; Maximum = 45.0 }
    @{ Name = '45-60'; Minimum = 45.0; Maximum = 60.0 }
    @{ Name = '60+'; Minimum = 60.0; Maximum = [double]::PositiveInfinity }
)

$byAngle = foreach ($bin in $angleBins) {
    $selected = @($rows | Where-Object {
        $_.BasisAngle -ge $bin.Minimum -and $_.BasisAngle -lt $bin.Maximum
    })
    if ($selected.Count -eq 0) {
        continue
    }
    $values = [double[]]$selected.Difference
    [pscustomobject]@{
        BasisAngleDeg = $bin.Name
        Frames = $selected.Count
        MeanDifferenceDeg = ($values | Measure-Object -Average).Average
        P95DifferenceDeg = Get-Percentile $values 0.95
        MeanFitError = (
            [double[]]$selected.FitError | Measure-Object -Average).Average
    }
}

Write-Output "Rotation comparison: $resolvedPath"
$summary | Format-List
Write-Output "Difference by palm-basis rotation magnitude:"
$byAngle | Format-Table -AutoSize
