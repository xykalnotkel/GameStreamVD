# AudioDriver — KERANGKA / BELUM LENGKAP

> **Status: belum bisa di-build.** Direktori ini baru berisi rencana implementasi
> dan kontrak data. Driver display (`../DisplayDriver`) yang sudah lengkap.

## Yang dibutuhkan remote gaming

Dua endpoint audio virtual:

1. **Render** — game mengirim audio ke "speaker virtual". Driver menaruh PCM ke
   section bersama, encoder Anda mengambilnya dan mengirim ke klien.
2. **Capture** — mikrofon virtual. Klien mengirim PCM, driver menyajikannya ke
   game sebagai input mikrofon (untuk voice chat in-game).

## Pendekatan yang disarankan

PortCls wave RT miniport (KMDF). Jangan mulai dari nol — pakai sampel resmi
Microsoft sebagai basis, lalu ganti jalur datanya:

```
git clone --depth 1 https://github.com/microsoft/Windows-driver-samples.git
# sumber: audio/simpleaudiosample
```

`simpleaudiosample` sudah punya:
- endpoint render (`eSpeakerDevice`) dan capture (`eMicArrayDevice`)
- stream engine wave RT lengkap dengan position/DMA

Yang perlu diganti hanya dua fungsi di `Source/Main/minwavertstream.cpp`:

| Fungsi | Sekarang | Ganti jadi |
|---|---|---|
| `CMiniportWaveRTStream::WriteBytes()` | mengisi DMA buffer dengan nada sinus (`m_ToneGenerator`) | menyalin DMA buffer **ke** ring `Global\GsVA_Render` |
| `CMiniportWaveRTStream::ReadBytes()` | menyalin DMA buffer ke `CSaveData` (file) | menyalin **dari** ring `Global\GsVA_Capture` ke DMA buffer |

Layout kedua ring sudah didefinisikan di `include/GsVirtual.h`
(`GS_AUDIO_HEADER`, `GSVD_AUDIO_RING_BYTES`, nama section/event), supaya driver
display dan audio memakai kontrak yang sama.

## Membuat section bersama dari KMDF

Driver audio berjalan di kernel, jadi tidak bisa memakai `CreateFileMapping`.
Pakai:

```c
// kernel
ZwCreateSection(&hSection, SECTION_ALL_ACCESS, &objAttr, &maxSize,
                PAGE_READWRITE, SEC_COMMIT, NULL);
```

dengan `objAttr.ObjectName = L"\\BaseNamedObjects\\GsVA_Render"` dan
`objAttr.Attributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE`.

Dari user mode, nama yang sama dibuka dengan `OpenFileMapping` memakai
`"Global\\GsVA_Render"` (`Global\` adalah alias user-mode untuk
`\BaseNamedObjects\`).

## Endpoint GUID

Sudah dicadangkan di `include/GsVirtual.h`:

- Audio device interface : `{59298C67-5ED3-48BD-8F3B-0C0ACAE0CC6C}`
- Render / capture       : nama section `Global\GsVA_Render`, `Global\GsVA_Capture`

## Alternatif yang lebih ringan

Kalau tidak ingin menulis driver kernel, pertimbangkan:

- **APO + Virtual Audio Device Graph Isolation (VAD)** — perangkat audio virtual
  murni user-mode, diperkenalkan di Windows 11. Lebih mudah di-sign dan tidak
  perlu test signing untuk diuji.
- **VB-CABLE / VBAN** — siap pakai, tapi tidak bisa dikontrol dari aplikasi Anda.

Untuk proyek remote desktop komersial, VAD umumnya lebih cepat sampai produksi.
