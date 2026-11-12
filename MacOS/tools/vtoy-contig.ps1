<#
  Check that a Ventoy vdisk file is stored contiguously (single extent) on its NTFS volume.
  Usage: powershell -File vtoy-contig.ps1 D:\path\macOS.vhd.vtoy
  Exit code 0 = contiguous, 1 = fragmented.
#>
param([Parameter(Mandatory)][string]$Path)
$ext = @(fsutil file queryextents $Path | Where-Object { $_ -match 'LCN:' })
if ($ext.Count -eq 0) { Write-Error "fsutil returned no extents for $Path"; exit 2 }
$prev = $null; $runs = 0
foreach ($line in $ext) {
    if ($line -match 'VCN:\s*0x([0-9a-f]+)\s+Clusters:\s*0x([0-9a-f]+)\s+LCN:\s*0x([0-9a-f]+)') {
        $vcn = [Convert]::ToInt64($matches[1], 16); $cnt = [Convert]::ToInt64($matches[2], 16); $lcn = [Convert]::ToInt64($matches[3], 16)
        if ($null -eq $prev -or $lcn -ne $prev) { $runs++ }
        $prev = $lcn + $cnt
    }
}
if ($runs -eq 1) { Write-Host "OK: $Path is contiguous"; exit 0 }
Write-Host "FRAGMENTED: $Path has $runs physical runs. Copy it to a volume with enough contiguous free space"
Write-Host "(e.g. copy away, defragment free space with 'defrag D: /X', copy back) before booting it with Ventoy."
exit 1
