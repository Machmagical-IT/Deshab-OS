# 用 diskpart 清除U盘 -> 写入镜像
$imgPath = "D:\Code\ZT5\ISO\zratroos.img"
$diskNumber = 1
$out = ''
$diskPath = "\\.\PhysicalDrive$diskNumber"

try {
    # 步骤1: 卸载U盘所有卷
    $out += "[1/3] Dismounting volumes..." + "`n"
    Get-Partition -DiskNumber $diskNumber -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.DriveLetter) {
            $dl = $_.DriveLetter
            $out += "  dismount $dl`:" + "`n"
            try {
                $vol = Get-WmiObject Win32_Volume -Filter "DriveLetter='$dl`:'" -ErrorAction SilentlyContinue
                if ($vol) { $vol.Dismount($true, $true) | Out-Null }
            } catch {}
        }
    }
    Start-Sleep -Seconds 2

    # 步骤2: diskpart clean (清除分区表，让Windows不再挂载)
    $out += "[2/3] diskpart clean..." + "`n"
    $diskpartScript = "select disk $diskNumber`nclean`nexit"
    $diskpartScript | diskpart | Out-Null
    Start-Sleep -Seconds 5

    # 步骤3: 写入镜像 (用 None share 独占)
    $out += "[3/3] Writing image..." + "`n"
    $dstStream = $null
    for ($attempt = 1; $attempt -le 30; $attempt++) {
        try {
            $dstStream = New-Object System.IO.FileStream($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
            $out += "  opened on attempt $attempt (None share)" + "`n"
            break
        } catch {
            # 第3次失败后改用 ReadWrite share
            if ($attempt -ge 3) {
                try {
                    $dstStream = New-Object System.IO.FileStream($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
                    $out += "  opened on attempt $attempt (ReadWrite share)" + "`n"
                    break
                } catch {}
            }
            if ($attempt -le 3 -or $attempt % 5 -eq 0) {
                $out += "  attempt $attempt failed" + "`n"
            }
            Start-Sleep -Seconds 2
        }
    }

    if ($dstStream -eq $null) {
        $out += "ERROR: Cannot open disk" + "`n"
        Set-Content -Path 'D:\Code\ZT5\disk_info.txt' -Value $out
        exit 1
    }

    $srcStream = [System.IO.File]::OpenRead($imgPath)
    $buffer = New-Object byte[] 1048576
    $totalBytes = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        while ($true) {
            $read = $srcStream.Read($buffer, 0, $buffer.Length)
            if ($read -eq 0) { break }
            $dstStream.Write($buffer, 0, $read)
            $totalBytes += $read
        }
        $dstStream.Flush()
    } catch {
        $out += "  WRITE ERROR after $totalBytes bytes: $_" + "`n"
    }
    $dstStream.Close()
    $srcStream.Close()
    $sw.Stop()
    $sizeMB = [math]::Round($totalBytes / 1048576, 2)
    $out += "wrote $sizeMB MB in $($sw.Elapsed.TotalSeconds.ToString('F1'))s" + "`n"
    if ($totalBytes -ge 35000000) {
        $out += "SUCCESS" + "`n"
    } else {
        $out += "INCOMPLETE: only $sizeMB MB written" + "`n"
    }
} catch {
    $out += "ERROR: $_" + "`n"
}

Set-Content -Path 'D:\Code\ZT5\disk_info.txt' -Value $out
