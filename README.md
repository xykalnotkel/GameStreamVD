# GameStreamVD — driver virtual untuk remote desktop gaming

Driver Windows + perangkat lunak pendukung untuk membangun aplikasi remote
desktop yang dipakai main game di mesin headless / tanpa monitor.

Komponen yang dibutuhkan remote gaming ada tiga, dan repo ini menyediakan
kerangka ketiganya:

| Kebutuhan | Solusi | Status |
|---|---|---|
| Layar virtual (biar game punya monitor untuk dirender) | Indirect Display Driver, IddCx / UMDF | **Kode lengkap** — `driver/DisplayDriver` |
| Audio game keluar → bisa di-encode | PortCls render endpoint + shared memory | Kerangka — `driver/AudioDriver` (TODO) |
| Mikrofon virtual (suara user masuk ke game) | PortCls capture endpoint + shared memory | Kerangka — `driver/AudioDriver` (TODO) |

---

## Kenapa IddCx, bukan DXGI Desktop Duplication

Untuk mesin headless (server / mini PC tanpa monitor), Windows tidak selalu
punya target render, sehingga `IDXGIOutputDuplication` sering gagal atau
menghasilkan frame kosong. IddCx membuat monitor yang benar-benar terdaftar di
OS, jadi game full-screen eksklusif pun punya tempat untuk dirender. Ini
pendekatan yang sama dipakai solusi komersial semacam Parsec / Moonlight virtual
display.

## Arsitektur

```
+----------------------------------------------------------+
| Game (full screen eksklusif)                             |
+---------------------------+------------------------------+
                            | render
                            v
+----------------------------------------------------------+
| GsDisplay.dll  (UMDF, IddCx)                             |
|  - EDID dibuat runtime  -> resolusi & refresh bebas      |
|  - monitor bisa ditambah/dilepas lewat IOCTL             |
|  - tiap frame -> staging texture -> section bersama      |
+---------------------------+------------------------------+
                            | Global\GsVDD_Frame_<slot>
                            v
+----------------------------------------------------------+
| Layanan encoder Anda (user mode, session user)           |
|  GsDevCtl.exe  = alat bantu / contoh pembaca frame       |
+----------------------------------------------------------+
```

Frame dikirim lewat **section bersama** (bukan file, bukan socket), dengan
sinkronisasi *seqlock*: driver tidak pernah menunggu encoder, jadi frame yang
tidak terkejar dihitung sebagai drop dan game tetap mulus.

Lihat `include/GsVirtual.h` untuk kontrak lengkap (layout header, nama objek,
IOCTL).

---

## Build

### Cara A — GitHub Actions (disarankan, tanpa setup lokal)

Repo ini sudah punya `.github/workflows/build.yml`. Runner `windows-2022`
sudah membawa **WDK 10.1.26100** dan **ekstensi WDK untuk VS 2022**, jadi
`msbuild` langsung jalan tanpa instalasi tambahan.

Push ke `main`, lalu buka tab **Actions** → unduh artefak
`GameStreamVD-Release-x64`.

### Cara B — Lokal (Windows)

1. Visual Studio 2022 + workload **Desktop development with C++**
2. **Windows Driver Kit (WDK)** — dari *Individual components* cari "Windows Driver Kit"
3. Buka `GameStreamVD.sln`, pilih `Release | x64`, Build

Hasil ada di `build\x64\Release\`.

### Uji unit generator EDID (Linux/macOS, tanpa WDK)

```bash
cd tests
g++ -std=c++17 -Wall -Wextra -I .. -I shim -o test_edid test_edid.cpp
./test_edid
```

Ini memverifikasi EDID yang dihasilkan driver: header, checksum, dan decode
mode dari DTD (1080p60, 1080p120, 1440p144, 4K60, 720p165).

---

## Install & uji

> **Penting:** driver yang belum di-sign hanya bisa dimuat dengan *test signing*
> aktif, dan itu menuntut **Secure Boot dimatikan**. Untuk produksi Anda wajib
> ikut [Windows Hardware Compatibility Program](https://learn.microsoft.com/windows-hardware/drivers/install/attestation-signing-a-kernel-mode-driver-package) (attestation signing) — lihat bagian *Signing* di bawah.

```powershell
# 1. Aktifkan test signing (butuh reboot)
bcdedit /set testsigning on

# 2. Install driver
pnputil /add-driver build\x64\Release\GsDisplay.inf /install

# 3. Buat monitor virtual 1080p60
GsDevCtl.exe add --slot 0 --width 1920 --height 1080 --hz 60

# 4. Cek status
GsDevCtl.exe info

# 5. Pastikan frame benar-benar mengalir (fps, umur frame, drop)
GsDevCtl.exe watch --slot 0

# 6. Lepas monitor
GsDevCtl.exe remove --slot 0
```

Kalau `GsDevCtl` gagal membuka driver, periksa:
- driver terinstall (`pnputil /enum-drivers`)
- test signing aktif (`bcdedit` → `testsigning  Yes`)
- dijalankan sebagai Administrator
- `CreateFileMapping` dengan nama `Global\` ditolak → lihat catatan di bawah

### Catatan soal privilege `Global\`

Driver UMDF berjalan sebagai LocalSystem di dalam `WUDFHost.exe`. Kalau
`CreateFileMapping` untuk nama `Global\...` ditolak (`ERROR_ACCESS_DENIED`),
tambahkan privilege ke host lewat INF:

```inf
[GsDisplay_Install]
UmdfHostProcessCommandLine=-AccessPolicy:SeCreateGlobalPrivilege
```

Lalu rebuild dan reinstall driver.

---

## Signing

| Tahap | Cara |
|---|---|
| Pengembangan | `bcdedit /set testsigning on` + self-signed cert |
| Uji internal | Test certificate + `signtool sign /v /s ...` |
| Produksi | **Attestation signing** via Partner Center (wajib untuk Windows 10/11 64-bit) |

Proyek `driver/DisplayDriverPackage/GsDisplayPackage.vcxproj` adalah template
driver package (menjalankan `inf2cat` + `signtool`). Proyek ini **sengaja belum
dimasukkan ke solution** karena `inf2cat` menolak INF tanpa `DriverVer` yang
di-stamp. Aktifkan setelah build driver hijau.

---

## Batasan yang diketahui

- **Driver belum pernah dikompilasi.** WDK tidak tersedia di lingkungan tempat
  kode ini ditulis. CI di GitHub Actions adalah jalur verifikasi; sampai build
  pertama hijau, anggap ada error kompilasi yang harus diperbaiki.
- Driver audio (`driver/AudioDriver`) masih kerangka.
- `RemoveMonitor` memanggil `IddCxMonitorDeparture`; perilaku hot-unplug perlu
  diuji di perangkat nyata.
- Frame saat ini selalu BGRA8 (bukan NV12). Untuk bitrate rendah, tambahkan
  konversi NV12 atau encode langsung dari staging texture.

## Lisensi & atribusi

`driver/DisplayDriver` diturunkan dari
[Microsoft/Windows-driver-samples — video/IndirectDisplay](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay)
(MIT License, Copyright (c) Microsoft Corporation). Modifikasi: generator EDID
runtime, kontrol monitor via IOCTL, ekspor frame ke section bersama.
