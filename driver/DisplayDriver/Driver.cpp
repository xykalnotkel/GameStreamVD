/*++

    GsDisplay / Driver.cpp

    GameStreamVD - Indirect Display Driver (IddCx, UMDF) untuk remote desktop
    gaming.

    Yang berbeda dari IddSampleDriver bawaan Microsoft:
      * EDID dibuat saat runtime (lihat Edid.h) sehingga resolusi dan refresh
        rate bisa diatur dari aplikasi, bukan hardcode.
      * Monitor bisa ditambah/dilepas lewat IOCTL (IOCTL_GSVD_ADD_MONITOR dkk.)
        melalui sebuah device interface.
      * Setiap frame yang di-acquire dari swap-chain disalin ke section bersama
        (lihat FrameSink.h) supaya encoder user-mode bisa mengambilnya tanpa
        lewat file atau jaringan.

    Dokumentasi IddCx:
    https://learn.microsoft.com/windows-hardware/drivers/display/indirect-display-driver-model

Environment:

    User Mode, UMDF

--*/

#include "Driver.h"
#include "Driver.tmh"

using std::shared_ptr;
using std::make_shared;
using std::unique_ptr;
using namespace Microsoft::GameStream;
using namespace Microsoft::WRL;

#pragma region MonitorTable

// Monitor virtual dibuat saat runtime lewat IOCTL_GSVD_ADD_MONITOR, jadi tidak
// ada tabel EDID statis lagi. Yang tersisa: mode cadangan untuk monitor tanpa
// EDID, dan daftar target mode (kemampuan pemrosesan frame device).

static const struct GsMonitorMode s_GsDefaultModes[] =
{
    { 1920, 1080,  60 },
    { 1600,  900,  60 },
    { 1280,  720,  60 },
    { 1024,  768,  60 },
};

// IddCx mengiris target mode dengan mode dari EDID, jadi apa pun yang ada di
// sini aman. Refresh rate tinggi wajib masuk daftar ini supaya bisa dipilih
// untuk gaming.
static const struct GsMonitorMode s_GsTargetModes[] =
{
    { 3840, 2160,  60 },
    { 2560, 1440, 165 },
    { 2560, 1440, 144 },
    { 2560, 1440, 120 },
    { 2560, 1440,  60 },
    { 1920, 1200,  60 },
    { 1920, 1080, 240 },
    { 1920, 1080, 165 },
    { 1920, 1080, 144 },
    { 1920, 1080, 120 },
    { 1920, 1080,  90 },
    { 1920, 1080,  60 },
    { 1600,  900,  60 },
    { 1280,  720, 165 },
    { 1280,  720, 120 },
    { 1280,  720,  60 },
    { 1024,  768,  75 },
    { 1024,  768,  60 },
};

#pragma endregion

#pragma region helpers

// memset/memcpy/memcmp dari <string.h> sengaja tidak dipakai di driver UMDF
// ini: include C++ semacam itu diarahkan WDK ke km\crt dan bertabrakan dengan
// STL MSVC (C2011 'std::bad_alloc' redefinition dkk).
static inline void GsZeroMemory(_Out_writes_bytes_all_(Size) void* p, size_t Size)
{
    unsigned char* b = (unsigned char*)p;
    for (size_t i = 0; i < Size; i++)
    {
        b[i] = 0;
    }
}


static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // Lihat https://learn.microsoft.com/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64)VSync) * ((UINT64)Width) * ((UINT64)Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD GsDisplayDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY GsDisplayDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED GsDisplayAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES GsDisplayAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION GsDisplayParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES GsDisplayMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES GsDisplayMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN GsDisplayMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN GsDisplayMonitorUnassignSwapChain;

EVT_WDF_IO_QUEUE_EVT_IO_DEVICE_CONTROL GsDisplayIoDeviceControl;

// Satu host UMDF hanya memuat satu instance driver, jadi pointer global ini
// aman dan memudahkan callback yang tidak menerima context (mis. parse EDID).
static IndirectDeviceContext* g_pDeviceContext = nullptr;

// GUID device interface yang dipakai aplikasi untuk membuka driver ini.
// Sengaja diurai dari string lewat IIDFromString, bukan DEFINE_GUID + macro,
// supaya tidak bergantung urutan include guiddef.h / initguid.h antar SDK.
#define GSVD_DISPLAY_INTERFACE_STRING L"{a4f2f254-ed6f-498e-b497-2f36a2a893c6}"

static bool GsGetDisplayInterfaceGuid(GUID* pGuid)
{
    return SUCCEEDED(IIDFromString(GSVD_DISPLAY_INTERFACE_STRING, pGuid));
}

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

// Macro ini membuat method untuk mengakses context wrapper sebagai context WDF
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT  pDriverObject,
    PUNICODE_STRING pRegistryPath
)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config,
        GsDisplayDeviceAdd
    );

    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return Status;
}

_Use_decl_annotations_
NTSTATUS GsDisplayDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    // Register for power callbacks - hanya power-on yang dibutuhkan
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = GsDisplayDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    // Aplikasi mengirim IOCTL_GSVD_ADD_MONITOR dkk. ke device ini, jadi callback
    // IoDeviceControl wajib diisi: IddCx mengarahkan request itu ke queue internal.
    IddConfig.EvtIddCxDeviceIoControl = GsDisplayIoDeviceControl;

    IddConfig.EvtIddCxAdapterInitFinished = GsDisplayAdapterInitFinished;

    IddConfig.EvtIddCxParseMonitorDescription = GsDisplayParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = GsDisplayMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = GsDisplayMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = GsDisplayAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = GsDisplayMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = GsDisplayMonitorUnassignSwapChain;

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Bersihkan context otomatis saat WDF object akan dihapus
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Device interface supaya aplikasi bisa menemukan driver tanpa menebak nama
    // device (CM_Get_Device_Interface_List + CreateFile).
    GUID interfaceGuid;
    if (!GsGetDisplayInterfaceGuid(&interfaceGuid))
    {
        return STATUS_UNSUCCESSFUL;
    }

    Status = WdfDeviceCreateDeviceInterface(Device, &interfaceGuid, nullptr);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Buat object context device dan pasang ke WDF device object
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);
    g_pDeviceContext = pContext->pContext;

    return Status;
}

#pragma region IoDeviceControl

_Use_decl_annotations_
VOID GsDisplayIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    auto* pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(device);
    auto* pContext = (pWrapper != nullptr) ? pWrapper->pContext : nullptr;
    if (pContext == nullptr)
    {
        WdfRequestCompleteWithInformation(Request, STATUS_INVALID_DEVICE_STATE, 0);
        return;
    }

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t bytesReturned = 0;

    switch (IoControlCode)
    {
    case IOCTL_GSVD_QUERY_INFO:
    {
        GSVD_QUERY_INFO_OUT* pOut = nullptr;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*pOut), (PVOID*)&pOut, nullptr);
        if (NT_SUCCESS(status))
        {
            pContext->FillQueryInfo(pOut);
            bytesReturned = sizeof(*pOut);
        }
        break;
    }

    case IOCTL_GSVD_ADD_MONITOR:
    {
        GSVD_ADD_MONITOR_IN* pIn = nullptr;
        GSVD_ADD_MONITOR_OUT* pOut = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*pIn), (PVOID*)&pIn, nullptr);
        if (NT_SUCCESS(status))
        {
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*pOut), (PVOID*)&pOut, nullptr);
        }
        if (NT_SUCCESS(status))
        {
            status = pContext->AddMonitor(pIn, pOut);
            if (NT_SUCCESS(status))
            {
                bytesReturned = sizeof(*pOut);
            }
        }
        break;
    }

    case IOCTL_GSVD_REMOVE_MONITOR:
    {
        GSVD_REMOVE_MONITOR_IN* pIn = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*pIn), (PVOID*)&pIn, nullptr);
        if (NT_SUCCESS(status))
        {
            status = pContext->RemoveMonitor(pIn->Slot);
        }
        break;
    }

    case IOCTL_GSVD_GET_FRAME_INFO:
    {
        GSVD_REMOVE_MONITOR_IN* pIn = nullptr;
        GSVD_FRAME_INFO_OUT* pOut = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*pIn), (PVOID*)&pIn, nullptr);
        if (NT_SUCCESS(status))
        {
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*pOut), (PVOID*)&pOut, nullptr);
        }
        if (NT_SUCCESS(status))
        {
            status = pContext->GetFrameInfo(pIn->Slot, pOut);
            if (NT_SUCCESS(status))
            {
                bytesReturned = sizeof(*pOut);
            }
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

#pragma endregion

_Use_decl_annotations_
NTSTATUS GsDisplayDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();

    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice()
{
    AdapterLuid = LUID{};
}

HRESULT Direct3DDevice::Init()
{
    // DXGI factory bisa di-cache, tapi kalau ada render adapter baru di sistem,
    // factory baru harus dibuat. Kalau mau caching, cek DxgiFactory->IsCurrent().
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    // Cari render adapter yang diminta
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    // Buat D3D device di render adapter. Dukungan BGRA wajib untuk WHQL.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if (FAILED(hr))
    {
        // Render GPU bisa saja hilang (mis. GPU yang bisa dilepas) atau sistem
        // sedang dalam keadaan transien.
        return hr;
    }

    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device,
                                       HANDLE NewFrameEvent, UINT Slot, UINT Width, UINT Height, UINT RefreshHz)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent),
      m_Slot(Slot), m_Width(Width), m_Height(Height), m_RefreshHz(RefreshHz), m_Sink(nullptr)
{
    // Section bersama dibuat SEBELUM thread jalan, supaya client yang menunggu
    // event tidak pernah melihat event tanpa section.
    m_Sink.reset(new FrameSink());
    LUID luid = Device ? Device->AdapterLuid : LUID{};
    if (!m_Sink->Attach(Slot, Width, Height, RefreshHz, luid))
    {
        m_Sink.reset();
    }

    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    // Langsung buat dan jalankan thread pemroses swap-chain
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Beri sinyal thread untuk berhenti
    SetEvent(m_hTerminateEvent.Get());

    if (m_hThread.Get())
    {
        // Tunggu thread selesai
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }

    // Thread sudah berhenti, aman melepas section bersama.
    m_Sink.reset();
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // Tingkatkan prioritas thread supaya frame tidak tertinggal saat CPU sibuk
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    RunCore();
}

void SwapChainProcessor::RunCore()
{
    // Ambil DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        return;
    }

    // Loop acquire / release buffer
    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        // Minta buffer berikutnya dari producer
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        // AcquireBuffer langsung mengembalikan STATUS_PENDING kalau belum ada buffer
        if (hr == E_PENDING)
        {
            HANDLE WaitHandles[] =
            {
                m_hAvailableBufferEvent,
                m_hTerminateEvent.Get()
            };
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
            {
                continue;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1)
            {
                break;
            }
            else
            {
                hr = HRESULT_FROM_WIN32(WaitResult);
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

            // Bagian paling sensitif latensi di driver ini: salin surface ke
            // staging lalu ke section bersama. Jangan taruh blocking call di sini.
            if (m_Sink != nullptr)
            {
                m_Sink->Present(AcquiredBuffer.Get(), m_Device->DeviceContext.Get());
            }

            AcquiredBuffer.Reset();

            // Beri tahu OS bahwa pemrosesan awal frame selesai, sehingga OS bisa
            // mulai menyiapkan frame berikutnya
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }

            // Opsional: laporkan statistik frame setelah encode/kirim selesai
            // IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
        }
        else
        {
            // Swap-chain kemungkinan di-abandon (mis. DXGI_ERROR_ACCESS_LOST)
            break;
        }
    }
}

#pragma endregion

#pragma region DeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
    m_WdfDevice(WdfDevice), m_Adapter(nullptr), m_Monitors{}, m_MonitorObjects{}
{
}

IndirectDeviceContext::~IndirectDeviceContext()
{
    for (UINT i = 0; i < GSVD_MAX_MONITORS; i++)
    {
        if (m_MonitorObjects[i] != nullptr)
        {
            RemoveMonitor(i);
        }
    }

    g_pDeviceContext = nullptr;
}

void IndirectDeviceContext::InitAdapter()
{
    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    // Deklarasi kemampuan dasar adapter (wajib)
    AdapterCaps.MaxMonitorsSupported = GSVD_MAX_MONITORS;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // String device untuk telemetri (wajib)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"GameStream Virtual Display";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"GameStreamVD";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"GSVD Display";

    // Versi hardware dan firmware (wajib)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if (NT_SUCCESS(Status))
    {
        m_Adapter = AdapterInitOut.AdapterObject;

        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
}

void IndirectDeviceContext::FinishInit(UINT ConnectorIndex)
{
    // Membuat satu monitor default di slot ini. Aplikasi bisa menambah monitor
    // lain lewat IOCTL_GSVD_ADD_MONITOR.
    CreateMonitorAtSlot(ConnectorIndex, 1920, 1080, 60, nullptr, 0);
}

NTSTATUS IndirectDeviceContext::CreateMonitorAtSlot(
    _In_ UINT Slot,
    _In_ UINT Width,
    _In_ UINT Height,
    _In_ UINT RefreshHz,
    _In_reads_bytes_opt_(EdidSize) const BYTE* Edid,
    _In_ UINT EdidSize)
{
    if (Slot >= GSVD_MAX_MONITORS || m_Adapter == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (m_Monitors[Slot].InUse)
    {
        // Slot sudah dipakai; lepas dulu supaya object monitor tidak bocor.
        RemoveMonitor(Slot);
    }

    GsMonitorInfo& info = m_Monitors[Slot];

    // --- EDID ---
    if (Edid != nullptr && (EdidSize == 128 || EdidSize == 256))
    {
        for (UINT i = 0; i < EdidSize && i < GsMonitorInfo::szEdidBlock; i++)
        {
            info.Edid[i] = Edid[i];
        }
    }
    else if (!GsGenerateEdid(info.Edid, Width, Height, RefreshHz))
    {
        return STATUS_INVALID_PARAMETER;
    }

    // --- daftar mode: mode yang diminta + beberapa refresh rate umum ---
    info.ModeCount = 0;
    info.Modes[0].Width = Width;
    info.Modes[0].Height = Height;
    info.Modes[0].VSync = RefreshHz;
    info.ModeCount = 1;

    static const DWORD extraRates[] = { 60, 120, 144 };
    for (DWORD rate : extraRates)
    {
        if (info.ModeCount >= GsMaxModesPerMonitor)
        {
            break;
        }
        if (rate != RefreshHz)
        {
            info.Modes[info.ModeCount].Width = Width;
            info.Modes[info.ModeCount].Height = Height;
            info.Modes[info.ModeCount].VSync = rate;
            info.ModeCount++;
        }
    }

    info.PreferredModeIdx = 0;
    info.Slot = Slot;

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = Slot;

    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = GsMonitorInfo::szEdidBlock;
    MonitorInfo.MonitorDescription.pData = info.Edid;

    // Container ID stabil per slot supaya OS tidak menganggap monitor ini sebagai
    // perangkat baru setiap kali driver dimuat ulang.
    MonitorInfo.MonitorContainerId.Data1 = 0x6501A0C0;
    MonitorInfo.MonitorContainerId.Data2 = 0x6A5A;
    MonitorInfo.MonitorContainerId.Data3 = 0x4C2A;
    MonitorInfo.MonitorContainerId.Data4[0] = 0x9C;
    MonitorInfo.MonitorContainerId.Data4[1] = 0x71;
    MonitorInfo.MonitorContainerId.Data4[7] = (BYTE)Slot;

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
    pMonitorContextWrapper->pContext = new IndirectMonitorContext(
        MonitorCreateOut.MonitorObject, Slot, Width, Height, RefreshHz);

    IDARG_OUT_MONITORARRIVAL ArrivalOut;
    Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(MonitorCreateOut.MonitorObject);
        return Status;
    }

    m_MonitorObjects[Slot] = MonitorCreateOut.MonitorObject;
    info.InUse = true;
    return STATUS_SUCCESS;
}

void IndirectDeviceContext::FillQueryInfo(_Out_ GSVD_QUERY_INFO_OUT* pOut)
{
    GsZeroMemory(pOut, sizeof(*pOut));
    pOut->Version = GSVD_FRAME_VERSION;
    pOut->MaxMonitors = GSVD_MAX_MONITORS;

    for (UINT i = 0; i < GSVD_MAX_MONITORS; i++)
    {
        if (m_Monitors[i].InUse)
        {
            pOut->ConnectedCount++;
            pOut->ConnectedSlots |= (1u << i);
        }
    }

    // Di C, "\\" menjadi satu backslash saat runtime.
    wcscpy_s(pOut->SectionPrefix, ARRAYSIZE(pOut->SectionPrefix), L"Global\\GsVDD_Frame_");
}

NTSTATUS IndirectDeviceContext::AddMonitor(_In_ const GSVD_ADD_MONITOR_IN* pIn, _Out_ GSVD_ADD_MONITOR_OUT* pOut)
{
    if (pIn == nullptr || pOut == nullptr || pIn->Slot >= GSVD_MAX_MONITORS)
    {
        return STATUS_INVALID_PARAMETER;
    }

    UINT width = (pIn->Width != 0) ? pIn->Width : 1920;
    UINT height = (pIn->Height != 0) ? pIn->Height : 1080;
    UINT hz = (pIn->RefreshHz != 0) ? pIn->RefreshHz : 60;

    const BYTE* edid = nullptr;
    UINT edidSize = 0;
    if (pIn->UseCustomEdid != 0 && (pIn->EdidSize == 128 || pIn->EdidSize == 256))
    {
        edid = pIn->Edid;
        edidSize = pIn->EdidSize;
    }

    NTSTATUS status = CreateMonitorAtSlot(pIn->Slot, width, height, hz, edid, edidSize);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    GsZeroMemory(pOut, sizeof(*pOut));
    pOut->Slot = pIn->Slot;
    pOut->Width = width;
    pOut->Height = height;
    pOut->RefreshHz = hz;
    swprintf_s(pOut->SectionName, ARRAYSIZE(pOut->SectionName), GSVD_FRAME_SECTION_FMT, pIn->Slot);
    swprintf_s(pOut->EventName, ARRAYSIZE(pOut->EventName), GSVD_FRAME_EVENT_FMT, pIn->Slot);
    return STATUS_SUCCESS;
}

NTSTATUS IndirectDeviceContext::RemoveMonitor(_In_ UINT Slot)
{
    if (Slot >= GSVD_MAX_MONITORS || !m_Monitors[Slot].InUse)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    IDDCX_MONITOR monitor = m_MonitorObjects[Slot];
    m_MonitorObjects[Slot] = nullptr;
    m_Monitors[Slot].InUse = false;

    if (monitor == nullptr)
    {
        return STATUS_SUCCESS;
    }

    // Beri tahu OS monitor dicabut, baru hapus object-nya.
    IDARG_OUT_MONITORDEPARTURE Departure = {};
    NTSTATUS status = IddCxMonitorDeparture(monitor, &Departure);
    WdfObjectDelete(monitor);
    return status;
}

NTSTATUS IndirectDeviceContext::GetFrameInfo(_In_ UINT Slot, _Out_ GSVD_FRAME_INFO_OUT* pOut)
{
    if (Slot >= GSVD_MAX_MONITORS || pOut == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    GsZeroMemory(pOut, sizeof(*pOut));
    pOut->Slot = Slot;
    pOut->Connected = m_Monitors[Slot].InUse ? 1 : 0;
    if (pOut->Connected)
    {
        pOut->Width = m_Monitors[Slot].Modes[0].Width;
        pOut->Height = m_Monitors[Slot].Modes[0].Height;
        pOut->RefreshHz = m_Monitors[Slot].Modes[0].VSync;
        swprintf_s(pOut->SectionName, ARRAYSIZE(pOut->SectionName), GSVD_FRAME_SECTION_FMT, Slot);
        swprintf_s(pOut->EventName, ARRAYSIZE(pOut->EventName), GSVD_FRAME_EVENT_FMT, Slot);
    }
    return STATUS_SUCCESS;
}

const GsMonitorInfo* IndirectDeviceContext::GetMonitorInfo(_In_ UINT Slot) const
{
    if (Slot >= GSVD_MAX_MONITORS)
    {
        return nullptr;
    }
    return &m_Monitors[Slot];
}

#pragma endregion

#pragma region MonitorContext

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, _In_ UINT Slot,
                                               _In_ UINT Width, _In_ UINT Height, _In_ UINT RefreshHz) :
    m_Monitor(Monitor), m_Slot(Slot), m_Width(Width), m_Height(Height), m_RefreshHz(RefreshHz),
    m_LastFrameSeq(0)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // Penting: hapus swap-chain kalau inisialisasi D3D gagal, supaya OS tahu
        // harus membuat swap-chain baru dan mencoba lagi.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        m_ProcessingThread.reset(new SwapChainProcessor(
            SwapChain, Device, NewFrameEvent, m_Slot, m_Width, m_Height, m_RefreshHz));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    // Hentikan pemrosesan swap-chain terakhir
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS GsDisplayAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    // Dipanggil saat OS selesai menyiapkan adapter. Sekarang monitor bisa dilaporkan.
    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus) && pDeviceContextWrapper->pContext != nullptr)
    {
        // Hanya slot 0 yang dibuat otomatis. Slot lain dibuat aplikasi lewat
        // IOCTL_GSVD_ADD_MONITOR supaya jumlah monitor sesuai kebutuhan.
        pDeviceContextWrapper->pContext->FinishInit(0);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS GsDisplayAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // Driver ini tidak punya panel fisik, jadi tidak ada yang perlu di-reconfigure.
    // Kalau nanti perlu (mis. ganti bitrate encode saat resolusi berubah), loop
    // pInArgs->pPaths dan cek IDDCX_PATH_FLAGS_ACTIVE di sini.

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS GsDisplayParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    // OS memanggil ini untuk meminta mode dari EDID yang kita laporkan. Kita
    // cocokkan blok EDID-nya dengan tabel monitor runtime, lalu kembalikan
    // daftar mode milik slot tersebut.

    if (pInArgs->MonitorDescription.DataSize != GsMonitorInfo::szEdidBlock)
    {
        return STATUS_INVALID_PARAMETER;
    }

    IndirectDeviceContext* pDeviceContext = g_pDeviceContext;
    if (pDeviceContext == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT slot = 0; slot < GSVD_MAX_MONITORS; slot++)
    {
        const GsMonitorInfo* info = pDeviceContext->GetMonitorInfo(slot);
        if (info == nullptr || !info->InUse)
        {
            continue;
        }

        const BYTE* a = (const BYTE*)pInArgs->MonitorDescription.pData;
        bool same = true;
        for (size_t i = 0; i < GsMonitorInfo::szEdidBlock; i++)
        {
            if (a[i] != info->Edid[i])
            {
                same = false;
                break;
            }
        }

        if (!same)
        {
            continue;
        }

        pOutArgs->MonitorModeBufferOutputCount = info->ModeCount;

        if (pInArgs->MonitorModeBufferInputCount < info->ModeCount)
        {
            // Pemanggil hanya menanyakan jumlah mode.
            return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
        }

        for (UINT i = 0; i < info->ModeCount; i++)
        {
            pInArgs->pMonitorModes[i] = CreateIddCxMonitorMode(
                info->Modes[i].Width,
                info->Modes[i].Height,
                info->Modes[i].VSync,
                IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
        }

        pOutArgs->PreferredMonitorModeIdx = info->PreferredModeIdx;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

_Use_decl_annotations_
NTSTATUS GsDisplayMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // Dipanggil untuk monitor tanpa EDID. Driver ini selalu mengirim EDID, jadi
    // jalur ini hanya cadangan: laporkan mode konservatif yang pasti didukung.
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        pOutArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(s_GsDefaultModes);
    }
    else
    {
        const DWORD available = (DWORD)ARRAYSIZE(s_GsDefaultModes);
        const DWORD count = (pInArgs->DefaultMonitorModeBufferInputCount < available)
                                ? pInArgs->DefaultMonitorModeBufferInputCount : available;

        for (DWORD i = 0; i < count; i++)
        {
            pInArgs->pDefaultMonitorModes[i] = CreateIddCxMonitorMode(
                s_GsDefaultModes[i].Width,
                s_GsDefaultModes[i].Height,
                s_GsDefaultModes[i].VSync,
                IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
        }

        pOutArgs->DefaultMonitorModeBufferOutputCount = count;
        pOutArgs->PreferredMonitorModeIdx = 0;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS GsDisplayMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // Target mode = kemampuan pemrosesan frame device. OS mengiris daftar ini
    // dengan mode dari EDID, jadi mode yang tidak ada di sini tidak akan pernah
    // ditawarkan ke user. Untuk gaming, refresh rate tinggi wajib ada di sini.
    pOutArgs->TargetModeBufferOutputCount = ARRAYSIZE(s_GsTargetModes);

    if (pInArgs->TargetModeBufferInputCount >= ARRAYSIZE(s_GsTargetModes))
    {
        for (DWORD i = 0; i < ARRAYSIZE(s_GsTargetModes); i++)
        {
            pInArgs->pTargetModes[i] = CreateIddCxTargetMode(
                s_GsTargetModes[i].Width,
                s_GsTargetModes[i].Height,
                s_GsTargetModes[i].VSync);
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS GsDisplayMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS GsDisplayMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
