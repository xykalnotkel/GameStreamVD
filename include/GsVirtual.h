/*++

    GsVirtual.h

    Kontrak bersama antara driver GameStreamVD (display + audio) dan
    layanan user-mode (GsCapture / GsDevCtl). File ini di-include oleh
    kedua sisi, jadi JANGAN taruh apa pun yang hanya valid di kernel
    atau hanya valid di user mode.

    Ringkasan cara kerja:

      Display driver (UMDF/IddCx)
        -> menyalin tiap frame yang di-acquire dari swap-chain ke sebuah
           section bersama (CreateFileMapping) bernama Global\GsVDD_Frame_<slot>
        -> menaikkan GS_FRAME_HEADER.FrameSeq lalu SetEvent pada
           Global\GsVDD_FrameReady_<slot>

      Audio driver (KMDF/PortCls)
        -> render : PCM game  -> section  Global\GsVA_Render   (driver = produser)
        -> capture: PCM mikrofon <- section Global\GsVA_Capture (driver = konsumen)

--*/

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
// CTL_CODE / FILE_DEVICE_UNKNOWN / METHOD_BUFFERED / FILE_ANY_ACCESS tinggal di
// devioctl.h pada SDK modern, dan TIDAK otomatis ikut lewat windows.h.
#include <devioctl.h>
#endif

/* ========================================================================== */
/* 1. GUID                                                                     */
/* ========================================================================== */

/* Device interface display driver. Client memakai ini untuk membuka device
   (CM_Get_Device_Interface_List + CreateFile) lalu mengirim IOCTL. */
/* {A4F2F254-ED6F-498E-B497-2F36A2A893C6} */
#define GSVD_DISPLAY_INTERFACE_GUID \
    { 0xa4f2f254, 0xed6f, 0x498e, { 0xb4, 0x97, 0x2f, 0x36, 0xa2, 0xa8, 0x93, 0xc6 } }

/* Device interface audio driver (untuk query status / statistik). */
/* {59298C67-5ED3-48BD-8F3B-0C0ACAE0CC6C} */
#define GSVD_AUDIO_INTERFACE_GUID \
    { 0x59298c67, 0x5ed3, 0x48bd, { 0x8f, 0x3b, 0x0c, 0x0a, 0xca, 0xe0, 0xcc, 0x6c } }

/* Catatan: GUID di atas sengaja TIDAK didefinisikan lewat DEFINE_GUID di header
   ini. Driver men-deklarasikannya lewat DEFINE_GUID di satu file .cpp, dan
   aplikasi memakai macro GUID_DEVINTERFACE_GSVD_DISPLAY untuk
   CM_Get_Device_Interface_List. Dengan begitu tidak ada risiko definisi ganda. */

/* ========================================================================== */
/* 2. Nama objek kernel bersama                                                */
/* ========================================================================== */

/* Prefiks "Global\" WAJIB: driver display berjalan sebagai LocalSystem di
   session 0 (host WUDFHost.exe), sedangkan encoder Anda kemungkinan jalan di
   session user. Tanpa prefiks Global\ tiap session punya namespace objek
   sendiri dan kedua sisi tidak akan pernah bertemu. */
#define GSVD_NS_PREFIX          L"Global\\"

#define GSVD_FRAME_SECTION_FMT  L"Global\\GsVDD_Frame_%u"       /* %u = slot monitor */
#define GSVD_FRAME_EVENT_FMT    L"Global\\GsVDD_FrameReady_%u"  /* auto-reset, di-set driver */
#define GSVD_MONITOR_EVENT      L"Global\\GsVDD_MonitorChanged" /* manual-reset, pulsa singkat */

#define GSVD_RENDER_SECTION     L"Global\\GsVA_Render"
#define GSVD_RENDER_EVENT       L"Global\\GsVA_RenderData"      /* driver: ada data baru untuk encoder */
#define GSVD_CAPTURE_SECTION    L"Global\\GsVA_Capture"
#define GSVD_CAPTURE_EVENT      L"Global\\GsVA_CaptureData"     /* service: ada PCM mikrofon baru */

/* Nilai override di registry device (HKR) kalau Anda perlu nama lain. */
#define GSVD_REG_SECTION_PREFIX L"SectionPrefix"

/* ========================================================================== */
/* 3. Layout section frame                                                     */
/* ========================================================================== */

#define GSVD_FRAME_MAGIC    0x56444446u   /* 'FDDV' */
#define GSVD_FRAME_VERSION  1

/* Format piksel. Sengaja kecil dan eksplisit supaya tidak bergantung header DXGI. */
#define GSVD_PIXFMT_INVALID 0
#define GSVD_PIXFMT_BGRA8   1   /* 32 bpp, sama dengan DXGI_FORMAT_B8G8R8A8_UNORM */
#define GSVD_PIXFMT_NV12    2   /* direncanakan; driver sekarang selalu BGRA8 */

/* Header di awal section. Pixel data mulai dari offset PixelOffset. */
typedef struct _GS_FRAME_HEADER
{
    UINT32  Magic;              /* GSVD_FRAME_MAGIC */
    UINT32  Version;            /* GSVD_FRAME_VERSION */
    UINT32  HeaderSize;         /* sizeof(GS_FRAME_HEADER) */
    UINT32  Slot;               /* indeks monitor / connector */

    UINT32  Width;
    UINT32  Height;
    UINT32  Stride;             /* byte per baris pixel buffer */
    UINT32  PixelFormat;        /* GSVD_PIXFMT_* */

    UINT32  PixelOffset;        /* offset pixel data dari awal section */
    UINT32  PixelSize;          /* Stride * Height */
    UINT32  SectionSize;        /* total ukuran section */
    UINT32  RefreshHz;          /* refresh mode yang sedang aktif */

    UINT64  FrameSeq;           /* naik 1 tiap frame yang selesai ditulis */
    UINT64  QpcTimestamp;       /* QueryPerformanceCounter saat frame siap */
    UINT64  QpcFrequency;       /* supaya client bisa hitung umur frame */
    UINT32  DropCount;          /* frame yang dibuang karena client lambat */
    UINT32  Reserved;

    UINT64  AdapterLuidLow;     /* LUID render adapter (buat pilih GPU encode) */
    UINT32  AdapterLuidHigh;
    UINT32  MonitorConnected;   /* 0 = monitor virtual sedang tidak aktif */

    UINT8   Reserved2[64];      /* ruang untuk versi berikutnya */
} GS_FRAME_HEADER, *PGS_FRAME_HEADER;

/* ========================================================================== */
/* 4. Layout section audio (ring buffer)                                       */
/* ========================================================================== */

#define GSVD_AUDIO_MAGIC    0x56444441u   /* 'ADDAV' */
#define GSVD_AUDIO_VERSION  1

#define GSVD_AUDIO_CH       2
#define GSVD_AUDIO_BITS     16
#define GSVD_AUDIO_RATE     48000

/* Ukuran ring default: 2 detik pada 48 kHz / stereo / 16 bit = 768000 byte. */
#define GSVD_AUDIO_RING_BYTES   (GSVD_AUDIO_RATE * GSVD_AUDIO_CH * (GSVD_AUDIO_BITS / 8) * 2)

typedef struct _GS_AUDIO_HEADER
{
    UINT32  Magic;
    UINT32  Version;
    UINT32  HeaderSize;
    UINT32  SampleRate;
    UINT32  Channels;
    UINT32  BitsPerSample;
    UINT32  RingBytes;          /* ukuran ring, kelipatan frame size */
    UINT32  RingOffset;         /* offset ring dari awal section */
    UINT32  SectionSize;

    /* Offset byte monotonik. produser menulis lalu update; konsumen mengejar.
       Dipakai InterlockedExchange64 / ReadNoFence64 supaya aman tanpa lock. */
    UINT64  WritePos;
    UINT64  ReadPos;

    UINT64  FramesTotal;
    UINT64  OverrunCount;       /* konsumen ketinggalan -> data tertimpa */
    UINT64  UnderrunCount;      /* produser tidak dapat data -> silence */

    UINT32  EndpointActive;     /* 1 = stream sedang berjalan */
    UINT32  Reserved;
    UINT8   Reserved2[64];
} GS_AUDIO_HEADER, *PGS_AUDIO_HEADER;

/* ========================================================================== */
/* 5. IOCTL display driver                                                     */
/* ========================================================================== */

/* Pakai FILE_DEVICE_UNKNOWN + METHOD_BUFFERED supaya tidak bentrok dengan
   IOCTL internal IddCx dan tidak butuh privilege khusus. */
#define IOCTL_GSVD_QUERY_INFO       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GSVD_ADD_MONITOR      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GSVD_REMOVE_MONITOR   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GSVD_GET_FRAME_INFO   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define GSVD_MAX_MONITORS   4

typedef struct _GSVD_QUERY_INFO_OUT
{
    UINT32  Version;
    UINT32  MaxMonitors;
    UINT32  ConnectedCount;
    UINT32  ConnectedSlots;                 /* bitmask bit0 = slot 0 */
    WCHAR   SectionPrefix[64];              /* contoh: L"Global\\GsVDD_Frame_" */
} GSVD_QUERY_INFO_OUT, *PGSVD_QUERY_INFO_OUT;

typedef struct _GSVD_ADD_MONITOR_IN
{
    UINT32  Slot;               /* 0 .. GSVD_MAX_MONITORS-1 */
    UINT32  Width;
    UINT32  Height;
    UINT32  RefreshHz;
    UINT32  UseCustomEdid;      /* 0 = pakai EDID bawaan driver */
    UINT32  EdidSize;           /* harus 128 atau 256 kalau UseCustomEdid */
    UINT8   Edid[256];
} GSVD_ADD_MONITOR_IN, *PGSVD_ADD_MONITOR_IN;

typedef struct _GSVD_ADD_MONITOR_OUT
{
    UINT32  Slot;
    UINT32  Width;
    UINT32  Height;
    UINT32  RefreshHz;
    WCHAR   SectionName[64];    /* nama section frame untuk slot ini */
    WCHAR   EventName[64];      /* nama event frame-ready untuk slot ini */
} GSVD_ADD_MONITOR_OUT, *PGSVD_ADD_MONITOR_OUT;

typedef struct _GSVD_REMOVE_MONITOR_IN
{
    UINT32  Slot;
} GSVD_REMOVE_MONITOR_IN, *PGSVD_REMOVE_MONITOR_IN;

typedef struct _GSVD_FRAME_INFO_OUT
{
    UINT32  Slot;
    UINT32  Connected;
    UINT32  Width;
    UINT32  Height;
    UINT32  RefreshHz;
    UINT64  FrameSeq;
    WCHAR   SectionName[64];
    WCHAR   EventName[64];
} GSVD_FRAME_INFO_OUT, *PGSVD_FRAME_INFO_OUT;
