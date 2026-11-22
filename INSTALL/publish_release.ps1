# INSTALL/publish_release.ps1
#
# DeshabOS GitHub Release 发行脚本（本地构建 + gh CLI）。
#
# 用法：
#   powershell -File INSTALL\publish_release.ps1                     # tag=v<yyyy.MM.dd>，镜像=ISO/deshab-release.img
#   powershell -File INSTALL\publish_release.ps1 -Tag v0.1.0         # 指定 tag
#   powershell -File INSTALL\publish_release.ps1 -Clobber            # 删除同名 release 后重建（危险）
#   powershell -File INSTALL\publish_release.ps1 -Token <PAT>        # 未 gh auth login 时直接给 token
#
# 流程：WSL gzip -9 压缩 25GiB 镜像 → 超过 GitHub 2GiB 单资产上限则切成小卷
# → 逐卷 gh release upload（失败自动重试、已上传则跳过，可断点续传）
# → 上传 SHA256SUMS → 取消 draft 正式发布。
#
# 前置：gh 已安装且已登录（或 -Token / GH_TOKEN）；WSL 可用。
#   注意：本机环境变量 SSL_CERT_FILE 可能指向第三方 CA 包，导致 gh 的 TLS 校验
#   失败，脚本启动时会清除这两个变量。
[CmdletBinding()]
param(
    [string]$Tag = ("v{0:yyyy.MM.dd}" -f (Get-Date)),
    [string]$Image = "ISO\deshab-release.img",
    [string]$Repo = "Linehu-Studio/Deshab-OS",
    [string]$Token = "",
    [string[]]$ExtraAssets = @(),
    [switch]$Clobber
)

$ErrorActionPreference = "Stop"
Remove-Item Env:SSL_CERT_FILE -ErrorAction SilentlyContinue
Remove-Item Env:SSL_CERT_DIR -ErrorAction SilentlyContinue
if ($Token) { $env:GH_TOKEN = $Token }
Set-Location (Join-Path $PSScriptRoot "..")

# PS 5.1 会把原生命令的 stderr 当成终止错误，gh 的 "release not found" 之类
# 属于正常分支，统一按 Continue 处理，只看 $LASTEXITCODE。
function Invoke-Gh {
    $ErrorActionPreference = "Continue"
    $out = & gh @args 2>&1
    return @{ Code = $LASTEXITCODE; Out = $out }
}

# --- 前置检查 ---
if (-not (Test-Path $Image)) {
    Write-Error "RELEASE-E01 镜像不存在: $Image（先跑 build.ps1 -Variant release）"
}
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Error "RELEASE-E02 未安装 gh CLI: https://cli.github.com/"
}
if (-not $env:GH_TOKEN) {
    $r = Invoke-Gh auth status
    if ($r.Code -ne 0) {
        Write-Error "RELEASE-E03 gh 未登录：执行 gh auth login，或用 -Token 传入 PAT（需 repo 权限）"
    }
}

$tmpGz = ".build_tmp\deshab-release.img.gz"
$tmpGzDir = Split-Path $tmpGz -Parent
New-Item -ItemType Directory -Force -Path $tmpGzDir | Out-Null

if (Test-Path $tmpGz) {
    Write-Host "[release] 复用已有压缩产物 $tmpGz（删除它可强制重压）"
} else {
    # --- WSL gzip -9 压缩（PS 5.1 无 scriptblock -replace，手工拼 /mnt/x 路径）---
    $winPath = (Resolve-Path $Image).Path
    $drive = $winPath.Substring(0, 1).ToLower()
    $wslImage = "/mnt/$drive" + ($winPath.Substring(2) -replace '\\', '/')
    $wslOutDir = "/mnt/$drive" + ((Resolve-Path $tmpGzDir).Path.Substring(2) -replace '\\', '/')
    $wslOut = "$wslOutDir/deshab-release.img.gz"
    Write-Host "[release] gzip -9 压缩中（25GiB 镜像，需数十分钟）..."
    wsl -e bash -c "gzip -9 -c '$wslImage' > '$wslOut'"
    if ($LASTEXITCODE -ne 0) { Write-Error "RELEASE-E04 WSL gzip 失败" }
}

# --- 分卷：GitHub 单资产 2GiB 上限；上行带宽低，用 200MiB 小卷便于重试 ---
$GITHUB_ASSET_LIMIT = 2147483648
$CHUNK = 209715200       # 200MiB
$gzLen = (Get-Item $tmpGz).Length
$gzMiB = [math]::Round($gzLen / 1MB, 1)
Write-Host "[release] 压缩产物: $tmpGz = $gzMiB MiB"

$partsDir = ".build_tmp\parts"
$assets = @()
if ($gzLen -gt $GITHUB_ASSET_LIMIT) {
    $reusableParts = @(Get-ChildItem $partsDir -Filter "deshab-release.img.gz.part*" -ErrorAction SilentlyContinue | Sort-Object Name)
    if ($reusableParts.Count -gt 0 -and (($reusableParts | Measure-Object Length -Sum).Sum -eq $gzLen)) {
        Write-Host "[release] 复用已有 $($reusableParts.Count) 个分卷（删除 $partsDir 可强制重切）"
        $assets = @($reusableParts | ForEach-Object { $_.FullName })
    } else {
        if (Test-Path $partsDir) { Remove-Item $partsDir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $partsDir | Out-Null
        Write-Host "[release] 分卷 $gzMiB MiB -> 200MiB/卷 ..."
        $in = [System.IO.File]::OpenRead((Resolve-Path $tmpGz).Path)
        try {
            $buf = New-Object byte[] (4MB)
            $idx = 1
            while ($in.Position -lt $in.Length) {
                $partPath = Join-Path $partsDir ("deshab-release.img.gz.part{0:d3}" -f $idx)
                $out = [System.IO.File]::Create($partPath)
                try {
                    $written = 0L
                    while ($written -lt $CHUNK -and $in.Position -lt $in.Length) {
                        $want = [int][Math]::Min($buf.Length, [Math]::Min($CHUNK - $written, $in.Length - $in.Position))
                        $n = $in.Read($buf, 0, $want)
                        if ($n -le 0) { break }
                        $out.Write($buf, 0, $n)
                        $written += $n
                    }
                } finally { $out.Close() }
                $assets += $partPath
                $idx++
            }
        } finally { $in.Close() }
        Write-Host "[release] 已生成 $($assets.Count) 个分卷"
    }
} else {
    $assets = @($tmpGz)
}

# --- SHA256 ---
# 镜像哈希以 gz 解压流为准：发布物就是 gz，源镜像可能在压缩后被其他进程改动
# （构建/虚拟机残留写盘），不能拿当前镜像文件的哈希填进 SHA256SUMS。
Write-Host "[release] 计算 SHA256（镜像 + 全部分卷，需要数分钟）..."
$imgShaFile = ".build_tmp\img.sha256"
$gzLenStr = "$gzLen"
$cached = @(Get-Content $imgShaFile -ErrorAction SilentlyContinue)
if ($cached.Count -ge 2 -and $cached[0] -eq $gzLenStr) {
    $imgHash = $cached[1].Trim()
    Write-Host "[release] 复用已缓存的镜像 SHA256"
} else {
    $drive = (Resolve-Path $tmpGz).Path.Substring(0, 1).ToLower()
    $wslGz = "/mnt/$drive" + ((Resolve-Path $tmpGz).Path.Substring(2) -replace '\\', '/')
    $h = (wsl -e bash -c "gzip -dc '$wslGz' | sha256sum" 2>$null | Out-String).Trim().Split(' ')[0]
    if ($h -notmatch '^[0-9a-f]{64}$') { Write-Error "RELEASE-E11 解压哈希计算失败: $h" }
    $imgHash = $h
    @($gzLenStr, $imgHash) | Set-Content -Path $imgShaFile -Encoding ASCII
}
$sumsFile = ".build_tmp\SHA256SUMS"
$lines = @("# DeshabOS $Tag - $(Get-Date -Format yyyy-MM-dd)")
$lines += "# 分卷按文件名顺序 cat 合并即为 deshab-release.img.gz"
foreach ($a in $assets) {
    $h = (Get-FileHash $a -Algorithm SHA256).Hash.ToLower()
    $lines += "$h  $([System.IO.Path]::GetFileName($a))"
    Write-Host "[release]   $([System.IO.Path]::GetFileName($a))  $h"
}
$lines += "# 合并并 gunzip 后的原始 25GiB 镜像"
$lines += "$imgHash  deshab-release.img"
[System.IO.File]::WriteAllLines((Join-Path (Get-Location) $sumsFile), $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "[release] 镜像 SHA256: $imgHash"

# --- 建立 draft release ---
$r = Invoke-Gh release view $Tag --repo $Repo
$releaseExists = ($r.Code -eq 0)
if ($releaseExists) {
    $info = Invoke-Gh release view $Tag --repo $Repo --json isDraft
    $isDraft = $true
    if ($info.Code -eq 0) {
        try { $isDraft = [bool](($info.Out | Out-String | ConvertFrom-Json).isDraft) } catch { $isDraft = $true }
    }
    if (-not $isDraft -and -not $Clobber) {
        Write-Error "RELEASE-E06 release $Tag 已正式发布（-Clobber 可删除重建）"
    }
    if (-not $isDraft) {
        Write-Host "[release] -Clobber: 删除已发布 release $Tag ..."
        Invoke-Gh release delete $Tag --repo $Repo --yes | Out-Null
        $releaseExists = $false
    } else {
        Write-Host "[release] 复用已存在的 draft $Tag（继续上传缺失分卷）"
    }
}
if (-not $releaseExists) {
    $created = $false
    for ($t = 1; $t -le 10; $t++) {
        $r = Invoke-Gh release create $Tag --repo $Repo --title "DeshabOS $Tag" --draft --notes "upload in progress"
        if ($r.Code -eq 0) { $created = $true; break }
        $errText = ($r.Out | Out-String)
        if ($errText -match 'already_exists') { $created = $true; break }
        Write-Host "[release] 创建 draft 失败（第 $t 次）: $($errText.Trim())"
        Start-Sleep -Seconds 15
    }
    if (-not $created) { Write-Error "RELEASE-E07 创建 draft 失败（网络持续超时）" }
}

# 取 release 元信息（id / upload_url）：gh 的 TLS 在部分链路上会握手超时，
# 所以资产本体用 curl 直传（支持重试 + 停滞中断），只有小 JSON 请求走 gh api。
# 注意：draft release 用 /releases/tags/<tag> 会 404，必须从列表里按 tag_name 找。
$json = $null
for ($t = 1; $t -le 10; $t++) {
    $r = Invoke-Gh api "repos/$Repo/releases?per_page=100"
    if ($r.Code -eq 0) {
        $list = @($r.Out | Out-String | ConvertFrom-Json)
        $json = $list | Where-Object { $_.tag_name -eq $Tag } | Select-Object -First 1
        if ($json) { break }
    }
    Write-Host "[release] 读取 release 元信息失败（第 $t 次），重试..."
    Start-Sleep -Seconds 10
}
if (-not $json) { Write-Error "RELEASE-E10 无法读取 release 元信息" }
$releaseId = $json.id
$uploadBase = $json.upload_url -replace '\{\?name,label\}', ''

function Get-RemoteAssets {
    for ($t = 1; $t -le 10; $t++) {
        $r = Invoke-Gh api "repos/$Repo/releases/$releaseId/assets?per_page=100"
        if ($r.Code -eq 0) {
            $map = @{}
            foreach ($ra in ($r.Out | Out-String | ConvertFrom-Json)) { $map[$ra.name] = $ra }
            return $map
        }
        Start-Sleep -Seconds 10
    }
    return @{}
}

# --- 逐卷上传（跳过已存在且大小一致的分卷；curl 重试 + 停滞中断）---
$ErrorActionPreference = "Continue"   # curl 的 stderr 错误码分支自行处理
$MAX_TRIES = 10
$failed = @()
$done = 0
foreach ($a in $assets) {
    $name = [System.IO.Path]::GetFileName($a)
    $size = (Get-Item $a).Length
    $remoteMap = Get-RemoteAssets

    if ($remoteMap.ContainsKey($name) -and $remoteMap[$name].size -eq $size) {
        $done++
        Write-Host "[release] 跳过已上传: $name ($([math]::Round($size/1MB,1))MB) [$done/$($assets.Count)]"
        continue
    }
    # 同名但大小不符（上次传了一半）→ 删掉重传
    if ($remoteMap.ContainsKey($name)) {
        Write-Host "[release] 删除不完整资产: $name"
        Invoke-Gh api -X DELETE "repos/$Repo/releases/assets/$($remoteMap[$name].id)" | Out-Null
    }

    $ok = $false
    for ($t = 1; $t -le $MAX_TRIES; $t++) {
        Write-Host "[release] 上传 $name ($([math]::Round($size/1MB,1))MB) 第 $t/$MAX_TRIES 次 $(Get-Date -Format HH:mm:ss) ..."
        $code = & curl.exe -sS -X POST "$uploadBase`?name=$name" `
            -H "Authorization: token $env:GH_TOKEN" `
            -H "Content-Type: application/octet-stream" `
            --data-binary "@$((Get-Item $a).FullName)" `
            --retry 3 --retry-all-errors --retry-delay 10 `
            --speed-limit 20480 --speed-time 120 --max-time 7200 `
            -o NUL -w "%{http_code}"
        if ($code -eq "201") { $ok = $true; break }
        Write-Host "[release]   失败 http=$code，30s 后重试"
        Start-Sleep -Seconds 30
    }
    if (-not $ok) { $failed += $name; Write-Host "[release]   $name 放弃（重跑脚本可续传）" }
    else { $done++; Write-Host "[release]   $name OK [$done/$($assets.Count)]" }
}

# --- 上传 SHA256SUMS ---
$sumsName = "SHA256SUMS"
$remoteMap = Get-RemoteAssets
if ($remoteMap.ContainsKey($sumsName)) { Invoke-Gh api -X DELETE "repos/$Repo/releases/assets/$($remoteMap[$sumsName].id)" | Out-Null }
$code = & curl.exe -sS -X POST "$uploadBase`?name=$sumsName" `
    -H "Authorization: token $env:GH_TOKEN" `
    -H "Content-Type: application/octet-stream" `
    --data-binary "@$((Get-Item $sumsFile).FullName)" `
    --retry 3 --retry-all-errors --retry-delay 10 --max-time 600 `
    -o NUL -w "%{http_code}"
if ($code -ne "201") { $failed += $sumsName }

# --- 附带资产（安装器 ISO 等，一次性小文件）---
foreach ($ea in $ExtraAssets) {
    if (-not (Test-Path $ea)) {
        Write-Host "[release] 附带资产不存在: $ea"
        $failed += $ea
        continue
    }
    $ename = [System.IO.Path]::GetFileName($ea)
    $esize = (Get-Item $ea).Length
    $remoteMap = Get-RemoteAssets
    if ($remoteMap.ContainsKey($ename)) {
        Write-Host "[release] 删除旧资产: $ename"
        Invoke-Gh api -X DELETE "repos/$Repo/releases/assets/$($remoteMap[$ename].id)" | Out-Null
    }
    $ok = $false
    for ($t = 1; $t -le 6; $t++) {
        Write-Host "[release] 上传附带资产 $ename ($([math]::Round($esize/1MB,1))MB) 第 $t/6 次 ..."
        $code = & curl.exe -sS -X POST "$uploadBase`?name=$ename" `
            -H "Authorization: token $env:GH_TOKEN" `
            -H "Content-Type: application/octet-stream" `
            --data-binary "@$((Get-Item $ea).FullName)" `
            --retry 3 --retry-all-errors --retry-delay 10 `
            --speed-limit 10240 --speed-time 120 --max-time 1800 `
            -o NUL -w "%{http_code}"
        if ($code -eq "201") { $ok = $true; break }
        Write-Host "[release]   失败 http=$code，30s 后重试"
        Start-Sleep -Seconds 30
    }
    if (-not $ok) { $failed += $ename } else { Write-Host "[release]   $ename OK" }
}

if ($failed.Count -gt 0) {
    Write-Host "[release] RELEASE-E08 以下资产未完成，release 保持 draft：$($failed -join ', ')"
    Write-Host "[release] 重跑本脚本会自动续传。"
    exit 2
}

# --- 正式发布 ---
$assetNote = if ($assets.Count -eq 1) {
    "- ``deshab-release.img.gz``：25GiB GPT 磁盘镜像 gzip 压缩（可直接 gunzip）"
} else {
    "- ``deshab-release.img.gz.part001..part$('{0:d3}' -f $assets.Count)``：25GiB GPT 磁盘镜像 gzip 压缩后的分卷（GitHub 单资产 2GiB 上限）"
}
$joinNote = if ($assets.Count -eq 1) {
    "gunzip deshab-release.img.gz"
} else {
    "cat deshab-release.img.gz.part* > deshab-release.img.gz  然后  gunzip deshab-release.img.gz"
}

$installerNote = ""
foreach ($ea in $ExtraAssets) {
    $en = [System.IO.Path]::GetFileName($ea)
    if ($en -like '*.iso') {
        $installerNote = "- ``$en``：装机镜像（Hybrid BIOS+UEFI，联网后自动从本 Release 拉取镜像写入目标硬盘）"
    }
}

$notes = @"
DeshabOS release $Tag

## 资产
$assetNote
$installerNote
- ``SHA256SUMS``：各分卷与解压后镜像的 SHA256

## 镜像
25GiB GPT：p1 ESP FAT32 7GiB / p2 DATA ext4 3GiB / p3 NTFS ~15GiB

## 安装
使用仓库 ``INSTALL/deshab-installer.iso``（Hybrid BIOS+UEFI）启动，
联网后自动从本 Release 拉取并写入目标硬盘；或手动：

    $joinNote
    dd if=deshab-release.img of=/dev/sdX bs=4M conv=fsync     # 或用 Rufus 写入

SHA256（解压后镜像）：``$imgHash``
"@

$notesFile = ".build_tmp\release_notes.md"
$notes | Set-Content -Path $notesFile -Encoding UTF8
$r = Invoke-Gh release edit $Tag --repo $Repo --draft=false --notes-file $notesFile
if ($r.Code -ne 0) { Write-Error "RELEASE-E09 发布失败: $($r.Out -join ' ')" }

Write-Host ""
Write-Host "[release] 完成: https://github.com/$Repo/releases/tag/$Tag"
Write-Host "[release] 镜像 SHA256: $imgHash"
