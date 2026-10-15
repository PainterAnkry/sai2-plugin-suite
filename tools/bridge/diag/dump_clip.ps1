# dump_clip.ps1 - inspect what SAI2 actually puts on the clipboard after Ctrl+C:
#   * the CF_DIBV5 header (header size, w/h, compression, alpha mask)
#   * SAI2's own registered private format (GUID) - may carry the layer origin
Add-Type -AssemblyName System.Windows.Forms

$o = [System.Windows.Forms.Clipboard]::GetDataObject()
Write-Output ("formats: " + (($o.GetFormats()) -join ', '))

function Get-Bytes($obj) {
    if ($null -eq $obj) { return $null }
    if ($obj -is [byte[]]) { return $obj }
    if ($obj -is [System.IO.MemoryStream]) { return $obj.ToArray() }
    if ($obj -is [System.IO.Stream]) {
        $ms = New-Object System.IO.MemoryStream
        $obj.CopyTo($ms)
        return $ms.ToArray()
    }
    return $null
}

foreach ($name in @('Format17', 'DeviceIndependentBitmap')) {
    if (-not $o.GetDataPresent($name)) { continue }
    $b = Get-Bytes ($o.GetData($name))
    if (-not $b) { Write-Output ("$name : (not a byte stream)"); continue }
    Write-Output ("--- $name : " + $b.Length + " bytes ---")
    $hdrSize = [BitConverter]::ToUInt32($b, 0)
    $w = [BitConverter]::ToInt32($b, 4)
    $h = [BitConverter]::ToInt32($b, 8)
    $bpp = [BitConverter]::ToUInt16($b, 14)
    $comp = [BitConverter]::ToUInt32($b, 16)
    Write-Output ("  headerSize=$hdrSize  w=$w  h=$h  bitCount=$bpp  compression=$comp")
    if ($hdrSize -ge 108 -and $b.Length -ge 56) {
        $rm = [BitConverter]::ToUInt32($b, 40)
        $gm = [BitConverter]::ToUInt32($b, 44)
        $bm = [BitConverter]::ToUInt32($b, 48)
        $am = [BitConverter]::ToUInt32($b, 52)
        Write-Output ("  masks R={0:X8} G={1:X8} B={2:X8} A={3:X8}" -f $rm, $gm, $bm, $am)
    }
    $stride = [int]((([math]::Abs($w) * $bpp + 31) / 32) * 4)
    $expected = $stride * [math]::Abs($h)
    Write-Output ("  stride=$stride pixels=$expected  => dataStart=" + ($b.Length - $expected))
}

# SAI2 私有格式
foreach ($f in $o.GetFormats()) {
    if ($f -match '^[0-9A-F]{8}-') {
        $b = Get-Bytes ($o.GetData($f))
        if (-not $b) { continue }
        Write-Output ("--- private $f : " + $b.Length + " bytes ---")
        $n = [math]::Min(96, $b.Length)
        $hex = ($b[0..($n-1)] | ForEach-Object { $_.ToString('X2') }) -join ' '
        Write-Output ("  " + $hex)
        $asc = ($b[0..($n-1)] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } }) -join ''
        Write-Output ("  ascii: " + $asc)
        # 逐个 u32 解释前 8 个
        for ($i = 0; $i + 4 -le [math]::Min(32, $b.Length); $i += 4) {
            Write-Output ("  [u32 @{0,2}] = {1}  (0x{1:X8})" -f $i, [BitConverter]::ToUInt32($b, $i))
        }
    }
}
