# Integrasi GameStreamVD ke XyDesk

XyDesk sudah punya modul yang tepat untuk ini:

| Berkas di XyDesk | Peran |
|---|---|
| `host/src/virtual_display.rs` | deteksi headless, install driver, buat display virtual |
| `host/src/virtual_mic.rs` | mikrofon virtual |
| `host/src/audio.rs` | audio capture/encode (Opus) |
| `host/src/screen.rs` | ambil frame (DXGI/WGC/GDI) |
| `host/src/nvenc.rs` | encode H.264 via NVENC |

Dokumen ini menjelaskan cara menyambungkan driver GameStreamVD ke situ.

---

## 1. Display: dua cara pakai

### Cara A — XyDesk tidak perlu diubah sama sekali

`virtual_display.rs` sudah mendeteksi driver lewat `pnputil /enum-drivers` dan
`list_displays()`. Setelah GsDisplay terpasang dan satu monitor virtual dibuat,
Windows melihatnya sebagai monitor biasa. `screen.rs` yang sudah pakai
DXGI/WGC langsung dapat frame — tidak hitam lagi saat sesi lock.

Yang perlu ditambah hanya supaya XyDesk mengenali driver kita:

```rust
const DRIVER_HWIDS: &[&str] = &[
    "IddSampleDriver",
    "VirtualDisplayDriver",
    "ROOT\\VirtualDisplayDriver",
    "ROOT\\IddSampleDriver",
    "ROOT\\GsVDisplay",     // <-- GameStreamVD
    "GsVDisplay",           // <-- GameStreamVD
    "GameStream Virtual Display",
];
```

Lalu tambahkan lokasi INF kita ke daftar kandidat di `try_install_driver()`:

```rust
r"C:\Program Files\XyDesk\driver\GsDisplay.inf",
r"./driver/GsDisplay.inf",
```

Kelebihan cara ini: nol risiko, tidak ada kode FFI baru. Kekurangannya: XyDesk
tetap mengambil frame lewat DXGI, jadi ada satu salinan frame ekstra.

### Cara B — baca langsung dari section driver (lebih cepat)

Driver menyalin setiap frame ke section bersama. XyDesk bisa membacanya tanpa
lewat DXGI sama sekali. Ini jalur yang dipakai untuk latency terendah.

```rust
// host/src/gsvdd.rs  (baru)
use windows::Win32::Foundation::*;
use windows::Win32::System::Threading::*;

#[repr(C)]
pub struct GsFrameHeader {
    pub magic: u32,            // 0x56444446
    pub version: u32,          // 1
    pub header_size: u32,
    pub slot: u32,
    pub width: u32,
    pub height: u32,
    pub stride: u32,           // byte per baris, BISA lebih besar dari width*4
    pub pixel_format: u32,     // 1 = BGRA8
    pub pixel_offset: u32,
    pub pixel_size: u32,
    pub section_size: u32,
    pub refresh_hz: u32,
    pub frame_seq: u64,        // ganjil = sedang ditulis, jangan dibaca
    pub qpc_timestamp: u64,
    pub qpc_frequency: u64,
    pub drop_count: u32,
    pub reserved: u32,
    pub adapter_luid_low: u64,
    pub adapter_luid_high: u32,
    pub monitor_connected: u32,
    pub reserved2: [u8; 64],
}

pub const GSVD_FRAME_MAGIC: u32 = 0x56444446;
```

Nama objek (prefiks `Global\` wajib — driver jalan di session 0 sebagai
LocalSystem, host XyDesk biasanya di session user):

| Objek | Isi |
|---|---|
| `Global\GsVDD_Frame_<slot>` | section, `slot` = 0..3 |
| `Global\GsVDD_FrameReady_<slot>` | event auto-reset, di-set driver tiap frame |
| `Global\GsVDD_MonitorChanged` | event manual-reset, pulsa saat monitor berubah |

### Membaca frame dengan aman (seqlock)

`FrameSeq` ganjil berarti driver sedang menulis. Jangan pakai mutex — cukup
baca ulang:

```rust
pub fn read_frame(map: *const u8, out: &mut Vec<u8>) -> Option<GsFrameHeader> {
    loop {
        let h = unsafe { *(map as *const GsFrameHeader) };
        if h.magic != GSVD_FRAME_MAGIC || h.frame_seq & 1 == 1 {
            std::hint::spin_loop();
            continue;                       // driver sedang menulis
        }
        let start = h.pixel_offset as usize;
        let end   = start + h.pixel_size as usize;
        out.clear();
        out.extend_from_slice(unsafe {
            std::slice::from_raw_parts(map.add(start), h.pixel_size as usize)
        });
        let _ = end;
        // Baca ulang: kalau sequence berubah saat kita menyalin, ulangi.
        let after = unsafe { (*(map as *const GsFrameHeader)).frame_seq };
        if after == h.frame_seq {
            return Some(h);
        }
    }
}
```

Loop utamanya:

```rust
let ev = open_event(&format!("Global\\GsVDD_FrameReady_{slot}"))?;
loop {
    WaitForSingleObject(ev, 16);            // ~60 fps
    if let Some(h) = read_frame(base, &mut buf) {
        encode_nvenc(&buf, h.width, h.height, h.stride);
    }
}
```

Perhatikan `stride != width * 4`. NVENC mau pitch eksplisit; kirim `h.stride`,
jangan hitung ulang dari `width`.

---

## 2. Kontrol monitor

`GsDevCtl.exe` adalah CLI, jadi bisa dipanggil dari Rust seperti XyDesk sudah
memanggil `pnputil`:

```rust
Command::new(driver_dir.join("GsDevCtl.exe"))
    .args(["add", "--slot", "0", "--width", "1920", "--height", "1080", "--hz", "60"])
    .output()?;
```

| Perintah | Fungsi |
|---|---|
| `GsDevCtl.exe add --slot 0 --width 1920 --height 1080 --hz 60` | buat monitor |
| `GsDevCtl.exe info` | status semua slot (JSON-ish, mudah di-parse) |
| `GsDevCtl.exe watch --slot 0` | fps, umur frame, jumlah drop |
| `GsDevCtl.exe remove --slot 0` | lepas monitor |

Maksimum 4 monitor (`GSVD_MAX_MONITORS`). Resolusi 640x480 sampai 7680x4320,
refresh 24-360 Hz.

Atau lewat IOCTL langsung kalau tidak mau proses eksternal. Device interface:
`{A4F2F254-ED6F-498E-B497-2F36A2A893C6}`

```rust
// CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, fn, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
const IOCTL_GSVD_QUERY_INFO:     u32 = 0x00222400;  // fn 0x900
const IOCTL_GSVD_ADD_MONITOR:    u32 = 0x00222404;  // fn 0x901
const IOCTL_GSVD_REMOVE_MONITOR: u32 = 0x00222408;  // fn 0x902
const IOCTL_GSVD_GET_FRAME_INFO: u32 = 0x0022240C;  // fn 0x903
```

Buka device-nya dengan `CM_Get_Device_Interface_List` + `CreateFile`, bukan
dengan menebak `\\.\...`.

---

## 3. Bundling di installer XyDesk

XyDesk sudah punya pola ini di `packaging/windows/drivers/IddSampleDriver/`.
Buat folder sejajar:

```
packaging/windows/drivers/GsDisplay/
    GsDisplay.dll       <- dari artefak GameStreamVD-Release-x64
    GsDisplay.inf
    GsDisplay.cat
    gsvd-test.cer
    GsDevCtl.exe
    install.bat         <- dari tools/install.bat
```

`install.bat` sudah menangani: cek admin, cek arsitektur, aktifkan test
signing, import sertifikat ke Root + TrustedPublisher, `pnputil /add-driver
/install`, buat monitor 1080p60, verifikasi.

### Masalah tanda tangan (penting untuk rilis)

Artefak CI ditandatangani **sertifikat uji coba**. Artinya di PC pengguna:

1. Secure Boot harus **dimatikan** di BIOS/UEFI
2. `bcdedit /set testsigning on` + reboot
3. `gsvd-test.cer` di-import ke Root dan TrustedPublisher

Dan ada watermark "Test Mode" di pojok layar. Ini oke untuk development dan
uji internal, tapi **tidak layak untuk rilis publik**.

Untuk rilis ke pengguna umum XyDesk, pilihannya:

| Opsi | Biaya | Catatan |
|---|---|---|
| Sertifikat EV + attestation signing | ~USD 300-500/tahun + akun Partner Center | Cara resmi. Tanpa watermark, tidak perlu test signing. |
| WHQL (HLK) | mahal, rumit | Perlu kalau mau logo Windows |
| Tetap pakai driver pihak ketiga (ge9/IddSampleDriver) untuk rilis | gratis | Sudah XyDesk lakukan sekarang; driver kita untuk fitur tambahan |
| HDMI dummy plug | $5-10 | Sudah disebut di `README-VDD.txt` XyDesk |

---

## 4. Audio dan mikrofon

Kontrak section-nya sudah didefinisikan di `include/GsVirtual.h`:

| Objek | Arah | Siapa penulis |
|---|---|---|
| `Global\GsVA_Render` | PCM audio game | driver (produser), XyDesk baca |
| `Global\GsVA_Capture` | PCM mikrofon | XyDesk tulis, driver baca |

Format: 48 kHz, 2 channel, 16 bit, ring 768000 byte (2 detik). `WritePos` dan
`ReadPos` monotonik, di-update dengan `InterlockedExchange64`.

Keuntungan dibanding WASAPI loopback yang dipakai `audio.rs` sekarang: tetap
ada audio walau tidak ada perangkat output fisik — kasus yang sama dengan
masalah display hitam di VM.

Status: **driver audionya belum ditulis**. Rencananya di
`driver/AudioDriver/README.md`.

---

## 5. Memastikan frame benar-benar mengalir

Sebelum menulis kode Rust apa pun, verifikasi dulu dari command line:

```
GsDevCtl.exe add --slot 0 --width 1920 --height 1080 --hz 60
GsDevCtl.exe info
GsDevCtl.exe watch --slot 0
```

`watch` menampilkan fps, nomor sequence, umur frame, dan jumlah drop. Kalau fps
0 padahal monitor sudah muncul di Display Settings, masalahnya di transport
frame — bukan di XyDesk. Kalau `watch` jalan, XyDesk tinggal membaca section
yang sama.

Kalau ada yang aneh, jalankan `tools\Diagnose.ps1` dan kirim
`gsvd-diagnosa.txt`.
