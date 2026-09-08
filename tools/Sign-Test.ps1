<#
.SYNOPSIS
    Menandatangani driver GameStreamVD dengan sertifikat uji coba.

.DESCRIPTION
    Dipanggil oleh GitHub Actions (tools\Sign-Test.ps1 dijalankan dari root repo).
    Bisa juga dijalankan manual di mesin Windows yang punya WDK:

        powershell -ExecutionPolicy Bypass -File tools\Sign-Test.ps1 -Configuration Release

    Yang dilakukan:
      1. Buat sertifikat self-signed untuk CodeSigningCert (atau pakai yang ada).
      2. Tandatangani GsDisplay.dll.
      3. Buat katalog (GsDisplay.cat) dengan inf2cat, lalu tandatangani.
      4. Ekspor sertifikat ke gsvd-test.cer supaya pengguna bisa meng-import-nya.

    Ini SERTIFIKAT UJI COBA. Hasilnya hanya bisa di-load di PC yang:
      * Secure Boot-nya dimatikan, DAN
      * `bcdedit /set testsigning on` sudah dijalankan + reboot, DAN
      * gsvd-test.cer sudah di-import ke store Root dan TrustedPublisher.

    Untuk distribusi ke pengguna umum butuh sertifikat EV + pengesahan
    Microsoft (attestation signing) lewat Windows Hardware Dev Center.
#>

param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$Platform = 'x64',

    [string]$Subject = 'CN=GameStreamVD Test Signing',

    # Versi yang ditulis stampinf ke DriverVer dan UmdfLibraryVersion.
    # UMDF 2.25 adalah yang dipakai toolset WindowsUserModeDriver10.0 di
    # WDK 10.0.26100 (lihat baris "Using UMDF 2.25" pada log build).
    [string]$DriverVersion = '1.0.0.0',
    [string]$UmdfVersion   = '2.25.0'
)

$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$outDir   = Join-Path $root "$Platform\$Configuration"
$dll      = Join-Path $outDir 'GsDisplay.dll'
$inf      = Join-Path $root 'driver\DisplayDriver\GsDisplay.inf'
$cerOut   = Join-Path $outDir 'gsvd-test.cer'

if (-not (Test-Path $dll)) { throw "DLL belum di-build: $dll" }
if (-not (Test-Path $inf)) { throw "INF tidak ditemukan: $inf" }

# ---------------------------------------------------------------- 1. sertifikat
$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $Subject } |
        Select-Object -First 1

if (-not $cert) {
    Write-Host "[sign] membuat sertifikat uji coba: $Subject"
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -CertStoreLocation Cert:\CurrentUser\My `
        -HashAlgorithm SHA256 `
        -KeyUsage DigitalSignature `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(10)
} else {
    Write-Host "[sign] memakai sertifikat yang sudah ada: $($cert.Thumbprint)"
}

Export-Certificate -Cert $cert -FilePath $cerOut -Type CERT | Out-Null
Write-Host "[sign] sertifikat diekspor ke $cerOut"

# ---------------------------------------------------------------- 2. cari perkakas
$kitBin = 'C:\Program Files (x86)\Windows Kits\10\bin'

function Find-KitTool {
    param([string]$Name)
    if (-not (Test-Path $kitBin)) { return $null }
    # Folder versi saja (10.0.xxxxx.x); "wdf" dsb. diabaikan.
    $dirs = Get-ChildItem $kitBin -Directory |
            Where-Object { $_.Name -match '^10\.0\.' } |
            Sort-Object { [version]$_.Name } -Descending
    foreach ($d in $dirs) {
        $p = Join-Path $d.FullName "$Platform\$Name"
        if (Test-Path $p) { return $p }
    }
    # Fallback: arsitektur apa pun.
    return Get-ChildItem $kitBin -Recurse -Filter $Name -ErrorAction SilentlyContinue |
           Select-Object -First 1 -ExpandProperty FullName
}

$signtool = Find-KitTool 'signtool.exe'
if (-not $signtool) { throw 'signtool.exe tidak ditemukan - install WDK atau Windows SDK.' }
Write-Host "[sign] signtool: $signtool"

$stampinf = Find-KitTool 'stampinf.exe'
if (-not $stampinf) { throw 'stampinf.exe tidak ditemukan - dibutuhkan untuk mengisi $ARCH$ dan DriverVer.' }
Write-Host "[sign] stampinf: $stampinf"

$inf2cat = Find-KitTool 'inf2cat.exe'
if (-not $inf2cat) { throw 'inf2cat.exe tidak ditemukan - dibutuhkan untuk membuat katalog.' }
Write-Host "[sign] inf2cat : $inf2cat"

# ---------------------------------------------------------------- 3. tandatangani DLL
Write-Host "[sign] menandatangani $dll"
& $signtool sign /v /sha1 $cert.Thumbprint /fd sha256 /t http://timestamp.digicert.com $dll
if ($LASTEXITCODE -ne 0) { throw "signtool gagal untuk DLL (kode $LASTEXITCODE)" }

# ---------------------------------------------------------------- 4. stampinf
# INF sumber masih berupa template: $ARCH$, $UMDFVERSION$ dan DriverVer kosong.
# stampinf yang mengisinya. Tanpa ini inf2cat menolak dengan
# "does not have NTAMD64 decorated model sections".
$stage = Join-Path $outDir 'package'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $dll -Destination $stage -Force

$stamped = Join-Path $stage 'GsDisplay.inf'
Copy-Item $inf -Destination $stamped -Force

$arch = if ($Platform -eq 'x64') { 'amd64' } else { $Platform.ToLowerInvariant() }
Write-Host "[sign] stampinf: mengisi macro INF (arch=$arch, UMDF=$UmdfVersion)"
& $stampinf -f $stamped -a $arch -d '*' -v $DriverVersion -u $UmdfVersion
if ($LASTEXITCODE -ne 0) { throw "stampinf gagal (kode $LASTEXITCODE)" }

Write-Host '[sign] hasil stamping:'
foreach ($k in @('DriverVer', 'UmdfLibraryVersion', 'Standard.NT')) {
    # -SimpleMatch: nama section INF memakai '[' yang bukan regex sah.
    $line = Select-String -Path $stamped -Pattern $k -SimpleMatch | Select-Object -First 1
    if ($line) { Write-Host ("        " + $line.Line.Trim()) }
}

# ---------------------------------------------------------------- 5. katalog
# inf2cat membaca INF lalu menghitung hash setiap berkas yang dirujuknya,
# jadi DLL harus sudah ditandatangani dan sefolder dengan INF.
Write-Host "[sign] membuat katalog di $stage"
& $inf2cat /driver:$stage /os:10_X64 /verbose
if ($LASTEXITCODE -ne 0) { throw "inf2cat gagal (kode $LASTEXITCODE)" }

$cat = Join-Path $stage 'GsDisplay.cat'
if (-not (Test-Path $cat)) { throw "inf2cat tidak menghasilkan $cat" }

& $signtool sign /v /sha1 $cert.Thumbprint /fd sha256 /t http://timestamp.digicert.com $cat
if ($LASTEXITCODE -ne 0) { throw "signtool gagal untuk katalog (kode $LASTEXITCODE)" }

# INF dan katalog hasil stamping adalah yang dipakai pengguna - bukan template.
Copy-Item $stamped -Destination $outDir -Force
Copy-Item $cat     -Destination $outDir -Force

# ---------------------------------------------------------------- 6. verifikasi
# `signtool verify /pa` ikut memvalidasi rantai sertifikat, dan sertifikat uji
# coba belum dipercaya di mesin ini. Jadi sertifikat dipasang dulu ke Root dan
# TrustedPublisher - persis yang dilakukan install.bat di PC pengguna - supaya
# hasil verifikasi di sini mencerminkan keadaan setelah instalasi.
Write-Host ''
Write-Host '[sign] memasang sertifikat ke store Root + TrustedPublisher (untuk verifikasi)'
foreach ($store in @('Root', 'TrustedPublisher')) {
    Import-Certificate -FilePath $cerOut `
        -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}

Write-Host '[sign] verifikasi (signtool verify /pa /all):'
foreach ($f in @($dll, (Join-Path $outDir 'GsDisplay.cat'))) {
    $out = & $signtool verify /pa /all /v $f 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Host $out
        throw "$f tidak lolos signtool verify (kode $LASTEXITCODE)"
    }
    $ts = if ($out -match 'The signature is timestamped') { 'ya' } else { 'tidak' }
    Write-Host ("  {0,-16} Valid, timestamped={1}" -f (Split-Path $f -Leaf), $ts)
}

Write-Host '[sign] melepas sertifikat dari store runner'
foreach ($store in @('Root', 'TrustedPublisher')) {
    Get-ChildItem "Cert:\LocalMachine\$store" |
        Where-Object { $_.Thumbprint -eq $cert.Thumbprint } |
        Remove-Item -Force
}

Write-Host ''
Write-Host '[sign] selesai. Isi paket untuk pengguna:'
foreach ($f in @('GsDisplay.dll', 'GsDisplay.inf', 'GsDisplay.cat', 'gsvd-test.cer')) {
    Write-Host ("        " + $f)
}
Write-Host ''
Write-Host '[sign] Sertifikat ini uji coba. Di PC tujuan harus:'
Write-Host '         1. Secure Boot dimatikan di BIOS/UEFI'
Write-Host '         2. bcdedit /set testsigning on  + reboot'
Write-Host '         3. gsvd-test.cer di-import ke store Root dan TrustedPublisher'
