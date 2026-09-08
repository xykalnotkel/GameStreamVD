<#
.SYNOPSIS
    Diagnosa kenapa monitor virtual GameStreamVD tidak muncul.

.DESCRIPTION
    Jalankan dari PowerShell BIASA (tidak perlu admin):

        powershell -ExecutionPolicy Bypass -File tools\Diagnose.ps1

    Semua temuan ditulis ke layar DAN ke gsvd-diagnosa.txt di folder yang sama.
    Kirim file itu kalau mau dibantu analisa.

    Skrip ini HANYA membaca. Tidak ada yang diubah, tidak ada yang di-install.
#>

$ErrorActionPreference = 'SilentlyContinue'
$logPath = Join-Path $PSScriptRoot 'gsvd-diagnosa.txt'
$script:lines = @()

function Write-Both {
    param([string]$Text, [string]$Color = 'Gray')
    Write-Host $Text -ForegroundColor $Color
    $script:lines += $Text
}

function Section {
    param([string]$Title)
    Write-Both ''
    Write-Both ('=' * 68) 'Cyan'
    Write-Both $Title 'Cyan'
    Write-Both ('=' * 68) 'Cyan'
}

function Ok   { param($m) Write-Both "  [OK]     $m" 'Green' }
function Bad  { param($m) Write-Both "  [MASALAH] $m" 'Red' }
function Warn { param($m) Write-Both "  [PERIKSA] $m" 'Yellow' }
function Info { param($m) Write-Both "  [i]      $m" 'Gray' }

$problems = @()

Write-Both "Diagnosa GameStreamVD - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" 'White'
Write-Both "Komputer: $env:COMPUTERNAME | Windows $([Environment]::OSVersion.Version)"

# ---------------------------------------------------------------- 1. Test signing
Section '1. Test signing & Secure Boot'

$cs = Get-CimInstance Win32_ComputerSystem
if ($cs.CodeIntegrityOptions -band 0x2) {
    Ok 'Test signing AKTIF'
} else {
    Bad 'Test signing MATI. Driver yang ditandatangani sertifikat uji coba TIDAK akan dimuat.'
    $problems += 'test-signing-mati'
    Info 'Perbaiki (PowerShell sebagai Administrator, lalu REBOOT):'
    Info '    bcdedit /set testsigning on'
}

$bcd = (bcdedit /enum '{current}' | Out-String)
if ($bcd -match 'testsigning\s+Yes') { Ok 'bcdedit: testsigning = Yes' }
elseif ($bcd -match 'testsigning') { Warn 'bcdedit menyebut testsigning tapi bukan Yes' }

$sb = Confirm-SecureBootUEFI
if ($sb -eq $true) {
    Bad 'Secure Boot AKTIF. Ini memblokir test signing - harus dimatikan di BIOS/UEFI.'
    $problems += 'secure-boot-aktif'
    Info 'Masuk BIOS/UEFI -> Security/Boot -> Secure Boot = Disabled -> reboot.'
} elseif ($sb -eq $false) {
    Ok 'Secure Boot nonaktif'
} else {
    Warn 'Status Secure Boot tidak bisa dibaca (mungkin bukan UEFI).'
}

# ---------------------------------------------------------------- 2. Driver store
Section '2. Driver di driver store'

$drv = pnputil /enum-drivers | Out-String
$hits = ($drv -split "(?=Published Name)" | Where-Object { $_ -match 'GsDisplay' })
if ($hits) {
    Ok 'GsDisplay.inf ada di driver store:'
    foreach ($h in $hits) {
        foreach ($l in ($h -split "`r?`n")) {
            if ($l.Trim()) { Info "    $($l.Trim())" }
        }
    }
} else {
    Bad 'GsDisplay.inf TIDAK ada di driver store - driver belum pernah terdaftar.'
    $problems += 'tidak-ada-di-store'
    Info 'Daftarkan (PowerShell sebagai Administrator):'
    Info '    pnputil /add-driver GsDisplay.inf /install'
}

# ---------------------------------------------------------------- 3. Device & status
Section '3. Device GsVDisplay dan statusnya'

$devs = Get-PnpDevice | Where-Object {
    $_.FriendlyName -match 'GameStream|GsDisplay|GsVDisplay' -or
    $_.InstanceId  -match 'GSVDISPLAY|GSVDISPLAY'
}
if (-not $devs) {
    Warn 'Belum ada device GsVDisplay. Wajar kalau "GsDevCtl add" belum pernah jalan,'
    Warn 'atau device-nya gagal dibuat.'
} else {
    foreach ($d in $devs) {
        $msg = "$($d.FriendlyName) | $($d.Status) | $($d.InstanceId)"
        switch ($d.Status) {
            'OK'     { Ok $msg }
            'Error'  { Bad $msg; $problems += "device-error-$($d.InstanceId)" }
            'Unknown'{ Warn $msg }
            default  { Warn $msg }
        }
        $code = (Get-PnpDeviceProperty -InstanceId $d.InstanceId `
                 -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
        if ($code -and $code -ne 0) {
            Bad "    Problem code = $code"
            switch ([int]$code) {
                52 { Info '    Code 52 = Windows tidak bisa memverifikasi tanda tangan driver.'
                     Info '    Penyebab umum: driver tidak ditandatangani, atau sertifikat uji'
                     Info '    coba belum di-import ke store "Root" dan "TrustedPublisher".' }
                39 { Info '    Code 39 = driver korup atau tidak valid.' }
                31 { Info '    Code 31 = device tidak bekerja; driver gagal di-load.' }
                10 { Info '    Code 10 = device tidak bisa start. Cek Event Viewer untuk detail.' }
                28 { Info '    Code 28 = driver belum terpasang untuk device ini.' }
                37 { Info '    Code 37 = Windows tidak bisa menginisialisasi driver untuk device ini.' }
                43 { Info '    Code 43 = device berhenti karena melaporkan masalah.' }
                48 { Info '    Code 48 = driver diblokir oleh kebijakan (mis. Code Integrity).' }
            }
        }
    }
}

# ---------------------------------------------------------------- 4. Monitor
Section '4. Monitor yang terlihat Windows'

$mons = Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBasicDisplayParams
if ($mons) {
    Ok "Windows melihat $($mons.Count) monitor:"
    foreach ($m in $mons) {
        $name = ($m.UserFriendlyName | Where-Object { $_ -ne 0 } |
                 ForEach-Object { [char]$_ }) -join ''
        $mfr  = ($m.ManufacturerName | Where-Object { $_ -ne 0 } |
                 ForEach-Object { [char]$_ }) -join ''
        Info "    - $($m.InstanceName)  mfr=$mfr name=$name active=$($m.Active)"
    }
} else {
    Warn 'Tidak ada monitor terdeteksi sama sekali (headless).'
}

# ---------------------------------------------------------------- 5. WUDFHost
Section '5. Proses WUDFHost (host driver UMDF)'

$wudf = Get-Process WUDFHost
if ($wudf) {
    Ok "WUDFHost berjalan ($($wudf.Count) proses)"
} else {
    Warn 'WUDFHost tidak berjalan. Driver UMDF yang sukses di-load biasanya'
    Warn 'punya proses WUDFHost. Kalau device ada tapi tidak ada WUDFHost,'
    Warn 'berarti DLL-nya gagal dimuat.'
}

# ---------------------------------------------------------------- 6. Event log
Section '6. Kejadian terbaru di Event Viewer (penyebab sebenarnya ada di sini)'

$since = (Get-Date).AddHours(-6)
$found = $false
foreach ($src in @('Microsoft-Windows-DriverFrameworks-UserMode',
                   'Microsoft-Windows-Kernel-PnP',
                   'Microsoft-Windows-CodeIntegrity')) {
    $ev = Get-WinEvent -FilterHashtable @{
        LogName = 'System'; StartTime = $since; ProviderName = $src
    } -MaxEvents 12 -ErrorAction SilentlyContinue
    if ($ev) {
        $found = $true
        Warn "--- $src ---"
        foreach ($e in $ev) {
            $txt = ($e.Message -split "`r?`n" | Select-Object -First 3) -join ' '
            Info ("  {0}  [{1}] {2}" -f $e.TimeCreated.ToString('HH:mm:ss'), $e.LevelDisplayName, $txt)
        }
    }
}
if (-not $found) {
    Info 'Tidak ada kejadian relevan dalam 6 jam terakhir di ketiga log itu.'
}

# ---------------------------------------------------------------- 7. Berkas driver
Section '7. Berkas driver di sekitar skrip ini'

foreach ($f in @('GsDisplay.inf', 'GsDisplay.dll', 'GsDisplay.cat', 'GsDevCtl.exe')) {
    $p = Join-Path $PSScriptRoot $f
    if (Test-Path $p) {
        $sig = Get-AuthenticodeSignature $p
        $len = (Get-Item $p).Length
        switch ($sig.Status) {
            'Valid'        { Ok "$f ($len byte) - tanda tangan VALID oleh $($sig.SignerCertificate.Subject)" }
            'NotSigned'    { Bad "$f ($len byte) - TIDAK DITANDATANGANI. Driver seperti ini ditolak Windows x64."; $problems += "$f-tanpa-tanda-tangan" }
            default        { Bad "$f ($len byte) - status tanda tangan: $($sig.Status)"; $problems += "$f-tanda-tangan-$($sig.Status)" }
        }
    } else {
        Info "$f tidak ada di folder ini"
    }
}

# ---------------------------------------------------------------- kesimpulan
Section 'KESIMPULAN'

if ($problems.Count -eq 0) {
    Ok 'Tidak ada masalah yang terdeteksi dari pemeriksaan di atas.'
    Info 'Kalau monitor tetap tidak muncul, jalankan "GsDevCtl.exe add" lalu'
    Info 'jalankan skrip ini lagi, dan kirim gsvd-diagnosa.txt.'
} else {
    Bad "$($problems.Count) masalah terdeteksi:"
    foreach ($p in $problems) { Info "  - $p" }
}

$script:lines | Set-Content -Path $logPath -Encoding UTF8
Write-Host ''
Write-Host "Laporan tersimpan di: $logPath" -ForegroundColor White
