/*++

    FrameSink.cpp

Environment:

    User Mode, UMDF

--*/

#include "FrameSink.h"

// <strsafe.h> dan <new> sengaja tidak dipakai: keduanya menarik CRT user-mode
// ke proyek UMDF dan memicu tabrakan header dengan km\crt milik WDK.

using namespace Microsoft::WRL;

namespace Microsoft
{
    namespace GameStream
    {
        FrameSink::FrameSink() :
            m_Slot(0),
            m_View(nullptr),
            m_SectionSize(0),
            m_Section(nullptr),
            m_FrameReadyEvent(nullptr),
            m_MonitorChangedEvent(nullptr),
            m_Width(0),
            m_Height(0),
            m_RefreshHz(0),
            m_AdapterLuid(),
            m_Staging(nullptr),
            m_StagingWidth(0),
            m_StagingHeight(0),
            m_FrameSeq(0),
            m_DropCount(0),
            m_QpcFreq()
        {
            QueryPerformanceFrequency(&m_QpcFreq);
        }

        FrameSink::~FrameSink()
        {
            Detach();
        }

        bool FrameSink::Attach(_In_ UINT Slot, _In_ UINT Width, _In_ UINT Height, _In_ UINT RefreshHz, _In_ LUID AdapterLuid)
        {
            if (m_View != nullptr)
            {
                Detach();
            }

            m_Slot = Slot;
            m_Width = Width;
            m_Height = Height;
            m_RefreshHz = RefreshHz;
            m_AdapterLuid = AdapterLuid;
            m_FrameSeq = 0;
            m_DropCount = 0;

            const UINT bytesPerPixel = 4;
            const UINT stride = Width * bytesPerPixel;

            // Section: header + pixel buffer. Ditambah satu baris supaya
            // CopySubresourceRegion / memcpy tidak pernah menyentuh ujung page.
            m_SectionSize = sizeof(GS_FRAME_HEADER) + ((SIZE_T)stride * Height);

            WCHAR sectionName[64];
            WCHAR eventName[64];
            if (swprintf_s(sectionName, ARRAYSIZE(sectionName), GSVD_FRAME_SECTION_FMT, Slot) < 0 ||
                swprintf_s(eventName, ARRAYSIZE(eventName), GSVD_FRAME_EVENT_FMT, Slot) < 0)
            {
                return false;
            }

            // Catatan: nama "Global\" membuat objek terlihat di seluruh session.
            // Kalau CreateFileMapping gagal dengan ERROR_ACCESS_DENIED, lihat
            // README (bagian "UmdfHostProcessCommandLine / SeCreateGlobalPrivilege").
            m_Section = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                (DWORD)(m_SectionSize >> 32),
                (DWORD)(m_SectionSize & 0xFFFFFFFFu),
                sectionName);

            if (m_Section == nullptr)
            {
                return false;
            }

            m_View = (BYTE*)MapViewOfFile(m_Section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (m_View == nullptr)
            {
                CloseHandle(m_Section);
                m_Section = nullptr;
                m_SectionSize = 0;
                return false;
            }

            m_FrameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, eventName);
            m_MonitorChangedEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, GSVD_MONITOR_EVENT);
            if (m_MonitorChangedEvent == nullptr)
            {
                m_MonitorChangedEvent = CreateEventW(nullptr, TRUE, FALSE, GSVD_MONITOR_EVENT);
            }

            // Bersihkan buffer supaya client tidak pernah membaca sisa proses lain.
            ZeroMemory(m_View, m_SectionSize);
            PublishHeader(Width, Height, stride);

            return true;
        }

        void FrameSink::Detach()
        {
            if (m_View != nullptr)
            {
                // Beri tahu client bahwa monitor ini tidak aktif lagi sebelum
                // view dilepas, supaya dia berhenti membaca.
                auto* header = reinterpret_cast<GS_FRAME_HEADER*>(m_View);
                header->MonitorConnected = 0;
                MemoryBarrier();

                UnmapViewOfFile(m_View);
                m_View = nullptr;
            }

            if (m_FrameReadyEvent != nullptr)
            {
                SetEvent(m_FrameReadyEvent);
                CloseHandle(m_FrameReadyEvent);
                m_FrameReadyEvent = nullptr;
            }

            if (m_MonitorChangedEvent != nullptr)
            {
                SetEvent(m_MonitorChangedEvent);
                ResetEvent(m_MonitorChangedEvent);
                CloseHandle(m_MonitorChangedEvent);
                m_MonitorChangedEvent = nullptr;
            }

            if (m_Section != nullptr)
            {
                CloseHandle(m_Section);
                m_Section = nullptr;
            }

            m_Staging.Reset();
            m_StagingWidth = 0;
            m_StagingHeight = 0;
            m_SectionSize = 0;
        }

        void FrameSink::PublishHeader(_In_ UINT Width, _In_ UINT Height, _In_ UINT Stride)
        {
            auto* header = reinterpret_cast<GS_FRAME_HEADER*>(m_View);

            header->Magic = GSVD_FRAME_MAGIC;
            header->Version = GSVD_FRAME_VERSION;
            header->HeaderSize = sizeof(GS_FRAME_HEADER);
            header->Slot = m_Slot;
            header->Width = Width;
            header->Height = Height;
            header->Stride = Stride;
            header->PixelFormat = GSVD_PIXFMT_BGRA8;
            header->PixelOffset = sizeof(GS_FRAME_HEADER);
            header->PixelSize = Stride * Height;
            header->SectionSize = (UINT32)m_SectionSize;
            header->RefreshHz = m_RefreshHz;
            header->QpcFrequency = (UINT64)m_QpcFreq.QuadPart;
            header->AdapterLuidLow = (UINT64)m_AdapterLuid.LowPart;
            header->AdapterLuidHigh = (UINT32)m_AdapterLuid.HighPart;
            header->MonitorConnected = 1;
            header->DropCount = 0;

            MemoryBarrier();
        }

        bool FrameSink::EnsureStaging(_In_ ID3D11Device* Device, _In_ UINT Width, _In_ UINT Height)
        {
            if (m_Staging != nullptr && m_StagingWidth == Width && m_StagingHeight == Height)
            {
                return true;
            }

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = Width;
            desc.Height = Height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;

            m_Staging.Reset();
            m_StagingWidth = 0;
            m_StagingHeight = 0;

            HRESULT hr = Device->CreateTexture2D(&desc, nullptr, &m_Staging);
            if (FAILED(hr))
            {
                m_Staging.Reset();
                return false;
            }

            m_StagingWidth = Width;
            m_StagingHeight = Height;
            return true;
        }

        void FrameSink::Present(_In_ IDXGIResource* AcquiredSurface, _In_ ID3D11DeviceContext* Context)
        {
            if (m_View == nullptr || AcquiredSurface == nullptr || Context == nullptr)
            {
                return;
            }

            ComPtr<ID3D11Texture2D> source;
            if (FAILED(AcquiredSurface->QueryInterface(IID_PPV_ARGS(&source))))
            {
                return;
            }

            D3D11_TEXTURE2D_DESC srcDesc = {};
            source->GetDesc(&srcDesc);

            if (srcDesc.Width != m_Width || srcDesc.Height != m_Height)
            {
                // Mode berubah di tengah jalan; tunggu commit mode berikutnya.
                return;
            }

            ComPtr<ID3D11Device> device;
            Context->GetDevice(&device);
            if (device == nullptr || !EnsureStaging(device.Get(), srcDesc.Width, srcDesc.Height))
            {
                return;
            }

            // GPU copy: murah, tidak menyentuh CPU.
            Context->CopyResource(m_Staging.Get(), source.Get());

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(Context->Map(m_Staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                return;
            }

            auto* header = reinterpret_cast<GS_FRAME_HEADER*>(m_View);
            BYTE* dest = m_View + header->PixelOffset;

            // ---- seqlock: mulai tulis (FrameSeq ganjil) ----
            const UINT64 seq = m_FrameSeq + 1;
            header->FrameSeq = seq;
            MemoryBarrier();

            // memcpy dihindari supaya tidak perlu <string.h> (lihat catatan di
            // atas). Copy per baris memakai pointer volatile agar compiler tidak
            // mengoptimalkan ulang urutan tulis terhadap MemoryBarrier.
            const BYTE* src = (const BYTE*)mapped.pData;
            const SIZE_T copyBytes = (mapped.RowPitch < header->Stride) ? mapped.RowPitch : header->Stride;
            for (UINT row = 0; row < header->Height; ++row)
            {
                BYTE* d = dest + (SIZE_T)row * header->Stride;
                const BYTE* s = src + (SIZE_T)row * mapped.RowPitch;
                for (SIZE_T b = 0; b < copyBytes; ++b)
                {
                    d[b] = s[b];
                }
            }

            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);

            // ---- seqlock: selesai tulis (FrameSeq genap) ----
            MemoryBarrier();
            header->QpcTimestamp = (UINT64)qpc.QuadPart;
            header->FrameSeq = seq + 1;
            m_FrameSeq = seq + 1;

            Context->Unmap(m_Staging.Get(), 0);

            if (m_FrameReadyEvent != nullptr)
            {
                SetEvent(m_FrameReadyEvent);
            }
        }
    }
}
