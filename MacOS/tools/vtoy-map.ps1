<#
  Generate the VtoyMac fallback map (NVRAM vtoy-map) for a contiguous fixed-VHD vdisk.
  Prints the value as Base64, ready for OpenCore config.plist:
    NVRAM > Add > 6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21 > vtoy-map (Data)
  Usage: powershell -File vtoy-map.ps1 D:\path\macOS.vhd.vtoy
  Re-run whenever the file is moved/restored to a different place.
#>
param([Parameter(Mandatory)][string]$Path)
$ErrorActionPreference = 'Stop'
$item = Get-Item -LiteralPath $Path
$part = Get-Partition -DriveLetter $item.PSDrive.Name
$disk = Get-Disk -Number $part.DiskNumber
$vol = Get-Volume -DriveLetter $item.PSDrive.Name
$cluster = [int64]$vol.AllocationUnitSize
if ($disk.PartitionStyle -ne 'MBR') { throw 'Only MBR (Ventoy default) disks are supported by this script' }

$ext = @(fsutil file queryextents $Path | Where-Object { $_ -match 'LCN:' })
# @(...) keeps a single run as a one-element array (PowerShell would otherwise unroll it into its 3 numbers)
$runs = @(foreach ($l in $ext) { if ($l -match 'VCN:\s*0x([0-9a-f]+)\s+Clusters:\s*0x([0-9a-f]+)\s+LCN:\s*0x([0-9a-f]+)') { ,@([Convert]::ToInt64($matches[1],16),[Convert]::ToInt64($matches[2],16),[Convert]::ToInt64($matches[3],16)) } })
if ($runs.Count -eq 0 -or $runs[0].Count -ne 3) { throw 'could not parse fsutil extents' }
for ($i = 1; $i -lt $runs.Count; $i++) { if ($runs[$i][2] -ne $runs[$i-1][2] + $runs[$i-1][1]) { throw "file is fragmented ($($runs.Count) extents)" } }
$startLba = ($part.Offset + $runs[0][2] * $cluster) / 512

# read VHD footer + GPT header from the file itself
$fs = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
$buf = New-Object byte[] 512
$fs.Seek(-512, 'End') | Out-Null; $fs.Read($buf, 0, 512) | Out-Null
$isVhd = [Text.Encoding]::ASCII.GetString($buf, 0, 8) -eq 'conectix'
$dataBytes = if ($isVhd) { $fs.Length - 512 } else { $fs.Length }
$fs.Seek(512, 'Begin') | Out-Null; $fs.Read($buf, 0, 512) | Out-Null
$fs.Close()
if ([Text.Encoding]::ASCII.GetString($buf, 0, 8) -ne 'EFI PART') { throw 'no GPT inside the image yet (install macOS first)' }
$imgGuid = $buf[56..71]

$ms = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $ms
$w.Write([Text.Encoding]::ASCII.GetBytes('VTMP')); $w.Write([uint32]1)
$w.Write([BitConverter]::GetBytes([uint32]$disk.Signature))
$w.Write((New-Object byte[] 16))
$w.Write([uint64]$disk.Size); $w.Write([uint64]$startLba); $w.Write([uint64]$dataBytes)
$w.Write([byte[]]$imgGuid)
$w.Flush()
[PSCustomObject]@{
    Disk = "$($disk.Number) $($disk.FriendlyName) sig=0x{0:X8}" -f $disk.Signature
    StartLBA = $startLba
    DataBytes = $dataBytes
    Base64 = [Convert]::ToBase64String($ms.ToArray())
} | Format-List
