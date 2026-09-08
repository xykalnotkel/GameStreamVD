/*++

    GsDevCtl.cpp

    Alat bantu user-mode untuk driver GameStreamVD Display:
      1. Membuat software device (SwDeviceCreate) supaya driver termuat.
      2. Membuka device interface driver.
      3. Mengirim IOCTL_GSVD_ADD_MONITOR untuk membuat monitor virtual dengan
         resolusi + refresh rate yang diminta.
      4. Opsional: memantau section frame bersama untuk memastikan frame
         benar-benar mengalir.

    Cara pakai:
        GsDevCtl.exe add  --slot 0 --width 1920 --height 1080 --hz 60
        GsDevCtl.exe info
        GsDevCtl.exe watch --slot 0
        GsDevCtl.exe remove --slot 0

    Build: aplikasi Win32 biasa (bukan driver). Lihat GsDevCtl.vcxproj.

--*/

#include <windows.h>
#include <initguid.h>
#include <swdevice.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../../include/GsVirtual.h"

DEFINE_GUID(GUID_DEVINTERFACE_GSVD_DISPLAY, GSVD_DISPLAY_INTERFACE_GUID);

#define GSVD_SW_DEVICE_ID   L"GsVDisplay"
#define GSVD_SW_INSTANCE    L"GsVDisplay"

static void PrintError(const char* what, DWORD err)
{
    printf("[GALAT] %s gagal, kode 0x%08lX (%lu)\n", what, err, err);
}

/* ---------------------------------------------------------------- device -- */

static void WINAPI CreationCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT hrCreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId)
{
    UNREFERENCED_PARAMETER(hSwDevice);
    UNREFERENCED_PARAMETER(pszDeviceInstanceId);

    HANDLE hEvent = (HANDLE)pContext;
    if (hrCreateResult != S_OK)
    {
        printf("[INFO] SwDeviceCreate callback HRESULT 0x%08lX\n", hrCreateResult);
    }
    SetEvent(hEvent);
}

// Membuat software device dan membiarkan handle tetap hidup selama proses jalan.
static HSWDEVICE CreateVirtualDisplayDevice()
{
    HSWDEVICE hSwDevice = nullptr;
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (hEvent == nullptr)
    {
        PrintError("CreateEvent", GetLastError());
        return nullptr;
    }

    SW_DEVICE_CREATE_INFO createInfo = {};
    createInfo.cbSize = sizeof(createInfo);
    createInfo.pszzCompatibleIds = GSVD_SW_DEVICE_ID L"\0\0";
    createInfo.pszInstanceId = GSVD_SW_INSTANCE;
    createInfo.pszzHardwareIds = GSVD_SW_DEVICE_ID L"\0\0";
    createInfo.pszDeviceDescription = L"GameStream Virtual Display";
    createInfo.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                                 SWDeviceCapabilitiesSilentInstall |
                                 SWDeviceCapabilitiesDriverRequired;

    HRESULT hr = SwDeviceCreate(GSVD_SW_DEVICE_ID, L"HTREE\\ROOT\\0", &createInfo,
                                0, nullptr, CreationCallback, hEvent, &hSwDevice);
    if (FAILED(hr))
    {
        // ERROR_OBJECT_ALREADY_EXISTS berarti device sudah ada dari run sebelumnya.
        printf("[INFO] SwDeviceCreate HRESULT 0x%08lX\n", hr);
    }
    else
    {
        WaitForSingleObject(hEvent, 10000);
        printf("[OK] Software device dibuat/dipastikan ada.\n");
    }

    CloseHandle(hEvent);
    return hSwDevice;
}

/* ------------------------------------------------------------- interface -- */

// Mencari path device interface driver.
static bool FindDeviceInterfacePath(WCHAR* path, DWORD pathChars)
{
    ULONG length = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(
        &length, (LPGUID)&GUID_DEVINTERFACE_GSVD_DISPLAY, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || length <= 1)
    {
        printf("[GALAT] Device interface belum ada (CONFIGRET 0x%X). Driver belum termuat?\n", cr);
        return false;
    }

    WCHAR* list = (WCHAR*)malloc(length * sizeof(WCHAR));
    if (list == nullptr)
    {
        return false;
    }

    cr = CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_GSVD_DISPLAY, nullptr,
                                       list, length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS)
    {
        printf("[GALAT] CM_Get_Device_Interface_List CONFIGRET 0x%X\n", cr);
        free(list);
        return false;
    }

    // Daftar berbentuk string ganda yang diakhiri dua NUL; entri pertama saja.
    if (list[0] == L'\0')
    {
        printf("[GALAT] Daftar device interface kosong.\n");
        free(list);
        return false;
    }

    wcsncpy_s(path, pathChars, list, _TRUNCATE);
    free(list);
    return true;
}

static HANDLE OpenDriver()
{
    WCHAR path[512];
    if (!FindDeviceInterfacePath(path, ARRAYSIZE(path)))
    {
        return INVALID_HANDLE_VALUE;
    }

    wprintf(L"[OK] Device interface: %ls\n", path);

    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        PrintError("CreateFile", GetLastError());
    }
    return h;
}

/* ----------------------------------------------------------------- aksi --- */

static int DoAddMonitor(HANDLE h, UINT slot, UINT width, UINT height, UINT hz)
{
    GSVD_ADD_MONITOR_IN in = {};
    in.Slot = slot;
    in.Width = width;
    in.Height = height;
    in.RefreshHz = hz;
    in.UseCustomEdid = 0;

    GSVD_ADD_MONITOR_OUT out = {};
    DWORD returned = 0;

    BOOL ok = DeviceIoControl(h, IOCTL_GSVD_ADD_MONITOR, &in, sizeof(in),
                              &out, sizeof(out), &returned, nullptr);
    if (!ok)
    {
        PrintError("IOCTL_GSVD_ADD_MONITOR", GetLastError());
        return 1;
    }

    printf("[OK] Monitor slot %u dibuat: %ux%u@%u\n", out.Slot, out.Width, out.Height, out.RefreshHz);
    wprintf(L"     section : %ls\n", out.SectionName);
    wprintf(L"     event   : %ls\n", out.EventName);
    printf("\nSekarang buka Display Settings / pilih mode di game. "
           "Kalau monitor tidak muncul, coba 'remove' lalu 'add' lagi.\n");
    return 0;
}

static int DoInfo(HANDLE h)
{
    GSVD_QUERY_INFO_OUT out = {};
    DWORD returned = 0;

    if (!DeviceIoControl(h, IOCTL_GSVD_QUERY_INFO, nullptr, 0, &out, sizeof(out), &returned, nullptr))
    {
        PrintError("IOCTL_GSVD_QUERY_INFO", GetLastError());
        return 1;
    }

    printf("Version        : %u\n", out.Version);
    printf("MaxMonitors    : %u\n", out.MaxMonitors);
    printf("ConnectedCount : %u\n", out.ConnectedCount);
    printf("ConnectedSlots : 0x%X\n", out.ConnectedSlots);
    wprintf(L"SectionPrefix  : %ls\n", out.SectionPrefix);

    for (UINT slot = 0; slot < out.MaxMonitors; slot++)
    {
        GSVD_REMOVE_MONITOR_IN in = {};
        in.Slot = slot;

        GSVD_FRAME_INFO_OUT fi = {};
        if (DeviceIoControl(h, IOCTL_GSVD_GET_FRAME_INFO, &in, sizeof(in), &fi, sizeof(fi), &returned, nullptr) &&
            fi.Connected)
        {
            printf("  slot %u: %ux%u@%u  FrameSeq=%llu\n",
                   fi.Slot, fi.Width, fi.Height, fi.RefreshHz, fi.FrameSeq);
        }
    }
    return 0;
}

static int DoRemove(HANDLE h, UINT slot)
{
    GSVD_REMOVE_MONITOR_IN in = {};
    in.Slot = slot;
    DWORD returned = 0;

    if (!DeviceIoControl(h, IOCTL_GSVD_REMOVE_MONITOR, &in, sizeof(in), nullptr, 0, &returned, nullptr))
    {
        PrintError("IOCTL_GSVD_REMOVE_MONITOR", GetLastError());
        return 1;
    }

    printf("[OK] Monitor slot %u dilepas.\n", slot);
    return 0;
}

// Membaca section frame bersama dan mencetak statistik. Ini sekaligus jadi
// bukti bahwa jalur driver -> encoder benar-benar jalan.
static int DoWatch(UINT slot)
{
    WCHAR sectionName[64];
    WCHAR eventName[64];
    swprintf_s(sectionName, ARRAYSIZE(sectionName), GSVD_FRAME_SECTION_FMT, slot);
    swprintf_s(eventName, ARRAYSIZE(eventName), GSVD_FRAME_EVENT_FMT, slot);

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName);
    if (hMap == nullptr)
    {
        wprintf(L"[GALAT] OpenFileMapping(%ls) gagal 0x%08lX\n", sectionName, GetLastError());
        printf("        Pastikan monitor slot %u sudah dibuat dan sedang aktif.\n", slot);
        return 1;
    }

    HANDLE hEvent = OpenEventW(SYNCHRONIZE, FALSE, eventName);
    const GS_FRAME_HEADER* hdr =
        (const GS_FRAME_HEADER*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (hdr == nullptr)
    {
        wprintf(L"[GALAT] MapViewOfFile gagal 0x%08lX\n", GetLastError());
        CloseHandle(hMap);
        return 1;
    }

    if (hdr->Magic != GSVD_FRAME_MAGIC)
    {
        printf("[GALAT] Magic tidak cocok (0x%08X). Versi driver/service berbeda?\n", hdr->Magic);
        UnmapViewOfFile(hdr);
        CloseHandle(hMap);
        return 1;
    }

    printf("[OK] Section terbuka: %ux%u stride=%u format=%u\n",
           hdr->Width, hdr->Height, hdr->Stride, hdr->PixelFormat);
    printf("     Tekan Ctrl+C untuk berhenti.\n");

    UINT64 lastSeq = hdr->FrameSeq;
    UINT64 frames = 0;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);

    for (;;)
    {
        if (hEvent != nullptr)
        {
            WaitForSingleObject(hEvent, 1000);
        }
        else
        {
            Sleep(16);
        }

        MemoryBarrier();
        UINT64 seq = hdr->FrameSeq;

        // Seq ganjil = driver sedang menulis, lewati pembacaan ini.
        if ((seq & 1ULL) != 0ULL)
        {
            continue;
        }

        if (seq != lastSeq)
        {
            frames += (seq - lastSeq) / 2;
            lastSeq = seq;
        }

        QueryPerformanceCounter(&t1);
        double elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (elapsed >= 1.0)
        {
            double fps = (double)frames / elapsed;
            double ageMs = 0.0;
            if (hdr->QpcFrequency != 0 && hdr->QpcTimestamp != 0)
            {
                ageMs = (double)((__int64)t1.QuadPart - (__int64)hdr->QpcTimestamp) *
                        1000.0 / (double)hdr->QpcFrequency;
            }
            printf("  %6.1f fps | seq=%llu | umur frame=%.1f ms | drop=%u | connected=%u\n",
                   fps, hdr->FrameSeq, ageMs, hdr->DropCount, hdr->MonitorConnected);
            frames = 0;
            t0 = t1;
        }
    }

    return 0;
}

/* ----------------------------------------------------------------- main --- */

static UINT ArgUint(int argc, char** argv, int start, const char* name, UINT def)
{
    for (int i = start; i < argc - 1; i++)
    {
        if (_stricmp(argv[i], name) == 0)
        {
            return (UINT)strtoul(argv[i + 1], nullptr, 10);
        }
    }
    return def;
}

static void Usage()
{
    printf(
        "GsDevCtl - kontrol monitor virtual GameStreamVD\n"
        "\n"
        "  GsDevCtl add    [--slot N] [--width W] [--height H] [--hz R]\n"
        "  GsDevCtl info\n"
        "  GsDevCtl watch  [--slot N]\n"
        "  GsDevCtl remove [--slot N]\n"
        "\n"
        "Default: slot 0, 1920x1080@60.\n"
        "Jalankan sebagai Administrator.\n");
}

int __cdecl main(int argc, char** argv)
{
    if (argc < 2)
    {
        Usage();
        return 1;
    }

    const char* cmd = argv[1];
    UINT slot = ArgUint(argc, argv, 2, "--slot", 0);

    if (_stricmp(cmd, "watch") == 0)
    {
        return DoWatch(slot);
    }

    // Perintah lain butuh driver sudah termuat.
    HSWDEVICE hSwDevice = CreateVirtualDisplayDevice();
    UNREFERENCED_PARAMETER(hSwDevice);

    // Beri waktu PnP memasang driver setelah device dibuat.
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10 && h == INVALID_HANDLE_VALUE; attempt++)
    {
        h = OpenDriver();
        if (h == INVALID_HANDLE_VALUE)
        {
            Sleep(1000);
        }
    }

    if (h == INVALID_HANDLE_VALUE)
    {
        printf("\n[GALAT] Tidak bisa membuka driver. Periksa:\n"
               "  - driver sudah terinstall (pnputil /add-driver GsDisplay.inf /install)\n"
               "  - test signing aktif (bcdedit /set testsigning on) kalau belum di-sign\n"
               "  - dijalankan sebagai Administrator\n");
        return 1;
    }

    int result = 0;
    if (_stricmp(cmd, "add") == 0)
    {
        UINT width = ArgUint(argc, argv, 2, "--width", 1920);
        UINT height = ArgUint(argc, argv, 2, "--height", 1080);
        UINT hz = ArgUint(argc, argv, 2, "--hz", 60);
        result = DoAddMonitor(h, slot, width, height, hz);
    }
    else if (_stricmp(cmd, "info") == 0)
    {
        result = DoInfo(h);
    }
    else if (_stricmp(cmd, "remove") == 0)
    {
        result = DoRemove(h, slot);
    }
    else
    {
        Usage();
        result = 1;
    }

    CloseHandle(h);
    return result;
}
