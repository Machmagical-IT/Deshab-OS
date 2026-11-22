[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$IsoPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputWim,

    [Parameter(Mandatory = $true)]
    [string]$BootCapsuleDir,

    [ValidateRange(1, 99)]
    [int]$EditionIndex = 4,

    [string]$DriversDir,
    [string]$GuestAgentDir,
    [string]$WorkDir,
    [string]$ExpandedDir,
    [switch]$ReuseExpanded,
    [switch]$KeepExpanded
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Windows guest preparation requires an elevated PowerShell session (DISM, registry hives, and diskpart).'
    }
}

function Invoke-Native([string]$FilePath, [string[]]$Arguments) {
    Write-Host "[windows] $FilePath $($Arguments -join ' ')"
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $FilePath @Arguments
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($exitCode -ne 0) {
        throw "$FilePath failed with exit code $exitCode"
    }
}

function Get-FreeDriveLetter {
    $used = @{}
    Get-Volume -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.DriveLetter) { $used[[string]$_.DriveLetter] = $true }
    }
    foreach ($letter in @('W','V','U','T','S','R','Q')) {
        if (-not $used.ContainsKey($letter)) { return $letter }
    }
    throw 'No free temporary drive letter is available for the boot capsule.'
}

function Set-OfflineRegistry([string]$WindowsRoot, [bool]$InstallGuestShell) {
    $softwareHive = Join-Path $WindowsRoot 'Windows\System32\Config\SOFTWARE'
    $systemHive = Join-Path $WindowsRoot 'Windows\System32\Config\SYSTEM'
    $softwareLoaded = $false
    $systemLoaded = $false
    try {
        Invoke-Native 'reg.exe' @('load', 'HKLM\DESHAB_SOFTWARE', $softwareHive)
        $softwareLoaded = $true
        Invoke-Native 'reg.exe' @('load', 'HKLM\DESHAB_SYSTEM', $systemHive)
        $systemLoaded = $true

        # Frozen compatibility appliance: keep servicing files intact, but disable
        # automatic update scans. A future maintenance command can reverse policy.
        Invoke-Native 'reg.exe' @('add', 'HKLM\DESHAB_SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate', '/v', 'DisableWindowsUpdateAccess', '/t', 'REG_DWORD', '/d', '1', '/f')
        Invoke-Native 'reg.exe' @('add', 'HKLM\DESHAB_SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU', '/v', 'NoAutoUpdate', '/t', 'REG_DWORD', '/d', '1', '/f')

        # Experimental first milestone: TPM2 and Secure Boot are not emulated yet.
        Invoke-Native 'reg.exe' @('add', 'HKLM\DESHAB_SYSTEM\Setup\LabConfig', '/v', 'BypassTPMCheck', '/t', 'REG_DWORD', '/d', '1', '/f')
        Invoke-Native 'reg.exe' @('add', 'HKLM\DESHAB_SYSTEM\Setup\LabConfig', '/v', 'BypassSecureBootCheck', '/t', 'REG_DWORD', '/d', '1', '/f')
        Invoke-Native 'reg.exe' @('add', 'HKLM\DESHAB_SYSTEM\Setup\LabConfig', '/v', 'BypassRAMCheck', '/t', 'REG_DWORD', '/d', '1', '/f')

        if ($InstallGuestShell) {
            Invoke-Native 'reg.exe' @('add', 'HKLM\DESHAB_SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon', '/v', 'Shell', '/t', 'REG_SZ', '/d', 'C:\Program Files\Deshab\DeshabGuestShell.exe', '/f')
        }
    }
    finally {
        if ($systemLoaded) { & reg.exe unload 'HKLM\DESHAB_SYSTEM' | Out-Null }
        if ($softwareLoaded) { & reg.exe unload 'HKLM\DESHAB_SOFTWARE' | Out-Null }
    }
}

Assert-Administrator

$IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
if (-not $WorkDir) {
    $WorkDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '.build_tmp\windows'
}
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$OutputWim = [IO.Path]::GetFullPath($OutputWim)
$BootCapsuleDir = [IO.Path]::GetFullPath($BootCapsuleDir)
if ($ExpandedDir) {
    $ApplyDir = [IO.Path]::GetFullPath($ExpandedDir)
}
else {
    $ApplyDir = Join-Path $WorkDir 'expanded'
}
$BootVhd = Join-Path $WorkDir 'boot-capsule.vhd'
$DiskpartScript = Join-Path $WorkDir 'boot-capsule.diskpart'
$IsoImage = $null
$BootAttached = $false
$BootLetter = Get-FreeDriveLetter

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
if (-not $ReuseExpanded -and (Test-Path -LiteralPath $ApplyDir)) {
    Remove-Item -LiteralPath $ApplyDir -Recurse -Force
}
if ($ReuseExpanded -and -not (Test-Path -LiteralPath (Join-Path $ApplyDir 'Windows\System32\Config\SYSTEM'))) {
    throw "Cannot reuse incomplete Windows tree: $ApplyDir"
}
if (Test-Path -LiteralPath $BootCapsuleDir) { Remove-Item -LiteralPath $BootCapsuleDir -Recurse -Force }
if (Test-Path -LiteralPath $OutputWim) { Remove-Item -LiteralPath $OutputWim -Force }
if (Test-Path -LiteralPath $BootVhd) {
    Dismount-DiskImage -ImagePath $BootVhd -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $BootVhd -Force
}
New-Item -ItemType Directory -Force -Path $ApplyDir, $BootCapsuleDir, (Split-Path -Parent $OutputWim) | Out-Null

try {
    if (-not $ReuseExpanded) {
        Write-Host "[windows] Mounting official ISO: $IsoPath"
        $IsoImage = Mount-DiskImage -ImagePath $IsoPath -PassThru
        $IsoVolume = $IsoImage | Get-Volume
        if (-not $IsoVolume.DriveLetter) { throw 'Mounted ISO has no drive letter.' }
        $IsoRoot = "$($IsoVolume.DriveLetter):\"
        $InstallImage = Join-Path $IsoRoot 'sources\install.wim'
        if (-not (Test-Path -LiteralPath $InstallImage)) {
            $InstallImage = Join-Path $IsoRoot 'sources\install.esd'
        }
        if (-not (Test-Path -LiteralPath $InstallImage)) {
            throw 'The ISO contains neither sources\install.wim nor sources\install.esd.'
        }

        Invoke-Native 'dism.exe' @('/English', '/Apply-Image', "/ImageFile:$InstallImage", "/Index:$EditionIndex", "/ApplyDir:$ApplyDir", '/CheckIntegrity')

        if ($DriversDir) {
            $DriversDir = (Resolve-Path -LiteralPath $DriversDir).Path
            Invoke-Native 'dism.exe' @('/English', "/Image:$ApplyDir", '/Add-Driver', "/Driver:$DriversDir", '/Recurse')
        }

        $installGuestShell = $false
        if ($GuestAgentDir) {
            $GuestAgentDir = (Resolve-Path -LiteralPath $GuestAgentDir).Path
            $guestDest = Join-Path $ApplyDir 'Program Files\Deshab'
            New-Item -ItemType Directory -Force -Path $guestDest | Out-Null
            Copy-Item -Path (Join-Path $GuestAgentDir '*') -Destination $guestDest -Recurse -Force
            $installGuestShell = Test-Path -LiteralPath (Join-Path $guestDest 'DeshabGuestShell.exe')
        }
        Set-OfflineRegistry $ApplyDir $installGuestShell
    }
    else {
        Write-Host "[windows] Reusing serviced Windows tree: $ApplyDir"
    }

    @"
create vdisk file="$BootVhd" maximum=260 type=expandable
select vdisk file="$BootVhd"
attach vdisk
create partition primary
format fs=fat32 quick label="DWINBOOT"
assign letter=$BootLetter
"@ | Set-Content -LiteralPath $DiskpartScript -Encoding ASCII
    Invoke-Native 'diskpart.exe' @('/s', $DiskpartScript)
    $BootAttached = $true

    Invoke-Native (Join-Path $env:SystemRoot 'System32\bcdboot.exe') @(
        (Join-Path $ApplyDir 'Windows'),
        '/s', "${BootLetter}:",
        '/f', 'UEFI',
        '/l', 'zh-CN',
        '/c',
        '/offline',
        '/nofirmwaresync',
        '/v'
    )
    Copy-Item -LiteralPath "${BootLetter}:\EFI" -Destination (Join-Path $BootCapsuleDir 'EFI') -Recurse -Force

    # Capture the serviced tree so the Linux image packer can apply it directly
    # to the single NTFS partition while preserving WIM metadata and hardlinks.
    Invoke-Native 'dism.exe' @('/English', '/Capture-Image', "/ImageFile:$OutputWim", "/CaptureDir:$ApplyDir", '/Name:Deshab Windows Guest', '/Compress:max', '/CheckIntegrity')

    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $IsoPath).Hash
    $metadata = [ordered]@{
        schema = 1
        source_iso = [IO.Path]::GetFileName($IsoPath)
        source_sha256 = $hash
        edition_index = $EditionIndex
        image_wim = $OutputWim
        boot_capsule = $BootCapsuleDir
        updates_frozen = $true
        persistent_partitions = @('NTFS')
    }
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path (Split-Path -Parent $OutputWim) 'metadata.json') -Encoding UTF8
    Write-Host "[windows] Prepared WIM: $OutputWim"
    Write-Host "[windows] Boot capsule: $BootCapsuleDir"
}
finally {
    if ($BootAttached -or (Test-Path -LiteralPath $BootVhd)) {
        @"
select vdisk file="$BootVhd"
detach vdisk
"@ | Set-Content -LiteralPath $DiskpartScript -Encoding ASCII
        & diskpart.exe /s $DiskpartScript | Out-Null
    }
    if ($IsoImage) {
        Dismount-DiskImage -ImagePath $IsoPath -ErrorAction SilentlyContinue
    }
    if (-not $KeepExpanded -and (Test-Path -LiteralPath $ApplyDir)) {
        Remove-Item -LiteralPath $ApplyDir -Recurse -Force
    }
}
