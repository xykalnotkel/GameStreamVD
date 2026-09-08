/*++

    FrameSink.h

    Menyalin frame hasil acquire dari swap-chain IddCx ke sebuah section
    bersama supaya bisa dibaca oleh encoder user-mode tanpa lewat file atau
    jaringan lokal.

    Model sinkronisasi: seqlock. Driver menulis pixel data lalu menaikkan
    FrameSeq dua kali (ganjil = sedang ditulis). Client yang melihat FrameSeq
    ganjil, atau melihat angka berubah di antara dua pembacaan header, harus
    membaca ulang. Jadi tidak perlu lock sama sekali dan driver tidak pernah
    menunggu client (game tetap jalan mulus, frame yang tidak terkejar
    dihitung sebagai drop).

Environment:

    User Mode, UMDF

--*/

#pragma once

#include <windows.h>
#include <d3d11_2.h>
#include <dxgi1_5.h>

/* Hanya ComPtr yang dipakai di sini, jadi tarik wrl/client.h saja.
   <wrl.h> yang lengkap ikut menarik winrt/wrl/wrappers/corewrappers.h, dan
   corewrappers.h memakai STATUS_WAIT_0. Konstanta itu tidak selalu ada saat
   sebuah file .cpp driver hanya meng-include header ini (Driver.cpp selamat
   karena Driver.h meng-include wudfwdm.h lebih dulu). */
#include <wrl/client.h>
#include <memory>

#include "GsVirtual.h"

namespace Microsoft
{
    namespace GameStream
    {
        class FrameSink
        {
        public:
            FrameSink();
            ~FrameSink();

            // Membuat section + event untuk satu slot monitor.
            bool Attach(_In_ UINT Slot, _In_ UINT Width, _In_ UINT Height, _In_ UINT RefreshHz, _In_ LUID AdapterLuid);

            // Menutup handle dan mengosongkan section (client yang sudah map
            // tetap aman; dia cukup cek MonitorConnected).
            void Detach();

            bool IsAttached() const { return m_View != nullptr; }

            // Path cepat: dipanggil dari loop swap-chain. Tidak boleh blocking
            // lebih dari beberapa ratus mikrodetik.
            void Present(_In_ IDXGIResource* AcquiredSurface, _In_ ID3D11DeviceContext* Context);

        private:
            bool EnsureStaging(_In_ ID3D11Device* Device, _In_ UINT Width, _In_ UINT Height);
            void PublishHeader(_In_ UINT Width, _In_ UINT Height, _In_ UINT Stride);

            UINT  m_Slot;
            BYTE* m_View;                 // MapViewOfFile dari m_Section
            SIZE_T m_SectionSize;
            HANDLE m_Section;
            HANDLE m_FrameReadyEvent;
            HANDLE m_MonitorChangedEvent;

            UINT  m_Width;
            UINT  m_Height;
            UINT  m_RefreshHz;
            LUID  m_AdapterLuid;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Staging;
            UINT  m_StagingWidth;
            UINT  m_StagingHeight;

            UINT64 m_FrameSeq;
            UINT32 m_DropCount;
            LARGE_INTEGER m_QpcFreq;
        };
    }
}
