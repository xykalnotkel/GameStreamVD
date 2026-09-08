#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>

#include "GsVirtual.h"
#include "Trace.h"
#include "Edid.h"
#include "FrameSink.h"

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace Microsoft
{
    namespace GameStream
    {
        // Mode yang dilaporkan ke OS untuk satu monitor virtual.
        struct GsMonitorMode
        {
            DWORD Width;
            DWORD Height;
            DWORD VSync;
        };

        static constexpr size_t GsMaxModesPerMonitor = 4;

        // Deskripsi satu monitor virtual. EDID-nya dibuat saat runtime oleh
        // GsGenerateEdid supaya resolusi/refresh bisa diatur dari aplikasi.
        struct GsMonitorInfo
        {
            static constexpr size_t szEdidBlock = 128;

            bool InUse;
            UINT Slot;
            GsMonitorMode Modes[GsMaxModesPerMonitor];
            UINT ModeCount;
            UINT PreferredModeIdx;
            BYTE Edid[szEdidBlock];
        };

        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            Direct3DDevice();
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device,
                               HANDLE NewFrameEvent, UINT Slot, UINT Width, UINT Height, UINT RefreshHz);
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;

            UINT m_Slot;
            UINT m_Width;
            UINT m_Height;
            UINT m_RefreshHz;
            std::unique_ptr<FrameSink> m_Sink;
        };

        class IndirectMonitorContext;

        /// <summary>
        /// Provides the implementation of the GameStreamVD indirect display driver.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit(UINT ConnectorIndex);

            // Kontrol monitor dari aplikasi lewat IOCTL.
            NTSTATUS AddMonitor(_In_ const GSVD_ADD_MONITOR_IN* pIn, _Out_ GSVD_ADD_MONITOR_OUT* pOut);
            NTSTATUS RemoveMonitor(_In_ UINT Slot);
            NTSTATUS GetFrameInfo(_In_ UINT Slot, _Out_ GSVD_FRAME_INFO_OUT* pOut);
            void FillQueryInfo(_Out_ GSVD_QUERY_INFO_OUT* pOut);

            const GsMonitorInfo* GetMonitorInfo(_In_ UINT Slot) const;

        private:
            NTSTATUS CreateMonitorAtSlot(_In_ UINT Slot, _In_ UINT Width, _In_ UINT Height,
                                         _In_ UINT RefreshHz, _In_reads_bytes_opt_(EdidSize) const BYTE* Edid,
                                         _In_ UINT EdidSize);

            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
            GsMonitorInfo m_Monitors[GSVD_MAX_MONITORS];

            // Handle monitor disimpan di sini supaya RemoveMonitor bisa memanggil
            // IddCxMonitorDeparture lalu menghapus object-nya.
            IDDCX_MONITOR m_MonitorObjects[GSVD_MAX_MONITORS];
        };

        class IndirectMonitorContext
        {
        public:
            IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, _In_ UINT Slot, _In_ UINT Width,
                                   _In_ UINT Height, _In_ UINT RefreshHz);
            virtual ~IndirectMonitorContext();

            void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain();

            UINT Slot() const { return m_Slot; }
            UINT64 LastFrameSeq() const { return m_LastFrameSeq; }
            void SetLastFrameSeq(UINT64 seq) { m_LastFrameSeq = seq; }

        private:
            IDDCX_MONITOR m_Monitor;
            UINT m_Slot;
            UINT m_Width;
            UINT m_Height;
            UINT m_RefreshHz;
            UINT64 m_LastFrameSeq;
            std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
        };
    }
}
