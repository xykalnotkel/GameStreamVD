@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ============================================================================
REM  Pemasang driver display virtual GameStreamVD
REM
REM  Taruh berkas ini SEFOLDER dengan hasil artefak CI:
REM      GsDisplay.dll   GsDisplay.inf   GsDisplay.cat
REM      gsvd-test.cer   GsDevCtl.exe    install.bat
REM
REM  Kode keluar:  0 sukses | 1 gagal | 2 butuh reboot | 3 bukan admin
REM ============================================================================

set "DIR=%~dp0"

REM ---------------------------------------------------------------- 0. admin?
net session >nul 2>&1
if errorlevel 1 (
    echo.
    echo  [X] Klik kanan install.bat -^> "Run as administrator".
    echo      Instalasi driver tidak bisa jalan tanpa hak Administrator.
    echo.
    exit /b 3
)

echo.
echo  ================================================================
echo   GameStreamVD - driver display virtual
echo  ================================================================
echo.

REM ---------------------------------------------------------------- 1. arsitektur
if /I not "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
    echo  [X] Arsitektur %PROCESSOR_ARCHITECTURE% tidak didukung. Driver ini x64 saja.
    exit /b 1
)

REM ---------------------------------------------------------------- 2. berkas
set "MISSING="
for %%F in (GsDisplay.inf GsDisplay.dll gsvd-test.cer) do (
    if not exist "%DIR%%%F" set "MISSING=!MISSING! %%F"
)
if defined MISSING (
    echo  [X] Berkas ini tidak ada di folder ini:!MISSING!
    echo      Ekstrak artefak GameStreamVD-Release-x64 ke folder ini dulu.
    exit /b 1
)

REM ---------------------------------------------------------------- 3. test signing
echo  [1/6] Memeriksa test signing...
bcdedit /enum "{current}" | findstr /C:"testsigning" | findstr /I "Yes" >nul
if errorlevel 1 (
    echo        Test signing belum aktif. Mengaktifkan sekarang...
    bcdedit /set testsigning on >nul 2>&1
    if errorlevel 1 (
        echo  [X] Gagal mengaktifkan test signing.
        echo      Kemungkinan besar Secure Boot masih AKTIF.
        echo      Matikan Secure Boot di BIOS/UEFI, lalu jalankan ulang skrip ini.
        exit /b 1
    )
    set "NEED_REBOOT=1"
    echo        Aktif, tapi baru berlaku setelah REBOOT.
) else (
    echo        Sudah aktif.
)

REM ---------------------------------------------------------------- 4. sertifikat
echo  [2/6] Memasang sertifikat uji coba...
certutil -addstore -f "Root" "%DIR%gsvd-test.cer" >nul 2>&1
if errorlevel 1 ( echo  [X] Gagal memasang ke store Root. & exit /b 1 )
certutil -addstore -f "TrustedPublisher" "%DIR%gsvd-test.cer" >nul 2>&1
if errorlevel 1 ( echo  [X] Gagal memasang ke store TrustedPublisher. & exit /b 1 )
echo        OK.

REM ---------------------------------------------------------------- 5. driver store
echo  [3/6] Mendaftarkan driver ke driver store...
pnputil /add-driver "%DIR%GsDisplay.inf" /install
set "RC=%errorlevel%"
if "%RC%"=="0"   goto :installed
if "%RC%"=="3010" ( set "NEED_REBOOT=1" & goto :installed )
if "%RC%"=="259"  goto :installed
echo        pnputil mengembalikan kode %RC%. Mencoba tanpa /install...
pnputil /add-driver "%DIR%GsDisplay.inf"
if errorlevel 1 (
    echo  [X] Gagal mendaftarkan driver.
    echo      Jalankan tools\Diagnose.ps1 untuk lihat penyebabnya.
    exit /b 1
)
:installed
echo        OK.

REM ---------------------------------------------------------------- 6. reboot?
if defined NEED_REBOOT (
    echo.
    echo  [!] REBOOT DULU, lalu jalankan install.bat sekali lagi.
    echo      Test signing baru berlaku setelah mesin dinyalakan ulang,
    echo      dan device belum bisa dibuat sebelum driver benar-benar termuat.
    echo.
    exit /b 2
)

REM ---------------------------------------------------------------- 7. buat monitor
echo  [4/6] Membuat monitor virtual 1920x1080 @ 60 Hz...
if not exist "%DIR%GsDevCtl.exe" (
    echo        GsDevCtl.exe tidak ada - lewati. Buat manual: GsDevCtl.exe add
    goto :verify
)
"%DIR%GsDevCtl.exe" add --slot 0 --width 1920 --height 1080 --hz 60
if errorlevel 1 (
    echo        GsDevCtl gagal. Jalankan tools\Diagnose.ps1 untuk detailnya.
)

REM ---------------------------------------------------------------- 8. verifikasi
:verify
echo  [5/6] Memeriksa apakah Windows melihat monitor baru...
timeout /t 3 /nobreak >nul
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$m = Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBasicDisplayParams; ^
   if ($m) { Write-Host ('        Windows melihat ' + $m.Count + ' monitor.') } ^
   else    { Write-Host '        Belum ada monitor terdeteksi.' }"

echo  [6/6] Selesai.
echo.
echo  Perintah berguna:
echo      GsDevCtl.exe info              status semua slot monitor
echo      GsDevCtl.exe watch             pantau fps / frame yang mengalir
echo      GsDevCtl.exe remove --slot 0   lepas monitor
echo      powershell -ExecutionPolicy Bypass -File tools\Diagnose.ps1
echo.
exit /b 0
