/*++

    Edid.h

    Generator EDID 1.3 (128 byte) untuk monitor virtual.

    Kenapa perlu: EDID yang di-hardcode di sampel Microsoft hanya punya mode
    yang sudah ditentukan. Untuk remote gaming Anda biasanya ingin mengatur
    resolusi + refresh rate dari aplikasi (mis. 1080p120, 1440p165), jadi EDID
    harus dibuat saat runtime sesuai permintaan client.

    Timing dihitung pakai pola CVT Reduced Blanking (VESA), cukup untuk monitor
    virtual karena tidak ada panel fisik yang perlu di-tune.

Environment:

    User Mode, UMDF

--*/

#pragma once

#include <windows.h>

// Jangan include <string.h> di sini: pada proyek UMDF, include C++ seperti
// <string.h>/<new> diarahkan ke km\crt milik WDK dan bertabrakan dengan STL
// MSVC (C2011 'std::bad_alloc' redefinition dkk). Semua operasi memori di
// bawah ditulis manual.

namespace Microsoft
{
    namespace GameStream
    {
        inline void GsEdidChecksum(_Inout_updates_bytes_(128) BYTE* pEdid)
        {
            BYTE sum = 0;
            for (int i = 0; i < 127; i++)
            {
                sum = (BYTE)(sum + pEdid[i]);
            }
            pEdid[127] = (BYTE)(0x100 - sum);
        }

        // Membuat EDID 128 byte dengan satu Detailed Timing Descriptor yang
        // menyatakan (Width x Height @ RefreshHz).
        inline bool GsGenerateEdid(
            _Out_writes_bytes_(128) BYTE* pEdid,
            _In_ UINT Width,
            _In_ UINT Height,
            _In_ UINT RefreshHz)
        {
            if (pEdid == nullptr || Width < 640 || Width > 7680 || Height < 480 || Height > 4320 ||
                RefreshHz < 24 || RefreshHz > 360)
            {
                return false;
            }

            // --- VESA CVT Reduced Blanking v1 ---
            // Horizontal: blanking total 160 px, sync 32 px, front porch 48 px.
            const UINT hBlank = 160;
            const UINT hTotal = Width + hBlank;
            const UINT hSyncWidth = 32;
            const UINT hFrontPorch = 48;

            // Vertikal: CVT-RB memakai 31 baris blanking untuk 16:9 dan 33 untuk
            // 16:10. Karena field pixel clock EDID hanya presisi 10 kHz, kita
            // coba beberapa nilai blanking lalu pilih yang refresh aktualnya
            // paling dekat dengan yang diminta. Tanpa pencarian ini mode tinggi
            // seperti 720p@165 bisa meleset jadi 166 Hz.
            const UINT vSyncWidth = 6;
            const UINT vBiasStart = (Width * 9 == Height * 16) ? 31 :
                                    (Width * 10 == Height * 16) ? 33 : 35;

            UINT vTotal = 0;
            UINT vFrontPorch = 0;
            UINT pixelClock10kHz = 0;
            UINT64 bestError = ~0ULL;

            for (UINT bias = vBiasStart; bias <= vBiasStart + 24; bias++)
            {
                UINT fp = ((Height + bias) & 1u) ? 4u : 3u;   // buat V_TOTAL genap
                if (bias <= fp + vSyncWidth)
                {
                    continue;
                }

                const UINT candidateTotal = Height + bias;
                const UINT64 wanted = (UINT64)hTotal * candidateTotal * RefreshHz;
                const UINT candidateClock = (UINT)((wanted + 5000ULL) / 10000ULL);
                if (candidateClock == 0 || candidateClock > 0xFFFF)
                {
                    continue;
                }

                // Selisih refresh aktual terhadap target, dalam satuan 0,01 Hz.
                const UINT64 actual100 = ((UINT64)candidateClock * 1000000ULL) /
                                         ((UINT64)hTotal * candidateTotal);
                const UINT64 target100 = (UINT64)RefreshHz * 100ULL;
                const UINT64 error = (actual100 > target100) ? (actual100 - target100)
                                                            : (target100 - actual100);
                // Ganti pilihan hanya kalau selisihnya membaik lebih dari 0,06 Hz.
                // Tanpa ini pencarian bisa "lari" ke blanking besar hanya demi
                // 0,002 Hz, padahal blanking CVT standar lebih aman untuk OS.
                if (error + 6 < bestError)
                {
                    bestError = error;
                    vTotal = candidateTotal;
                    vFrontPorch = fp;
                    pixelClock10kHz = candidateClock;
                }
            }

            // Terima hanya kalau refresh aktual masih dalam 1% dari permintaan.
            if (vTotal == 0 || bestError > (UINT64)RefreshHz)
            {
                return false;
            }

            for (int i = 0; i < 128; i++)
            {
                pEdid[i] = 0;
            }

            // Header
            pEdid[0] = 0x00; pEdid[1] = 0xFF; pEdid[2] = 0xFF; pEdid[3] = 0xFF;
            pEdid[4] = 0xFF; pEdid[5] = 0xFF; pEdid[6] = 0xFF; pEdid[7] = 0x00;

            // Manufacturer ID "GSV" = ((7<<10)|(19<<5)|22)
            const UINT manuf = ((('G' - 'A' + 1) << 10) | (('S' - 'A' + 1) << 5) | ('V' - 'A' + 1));
            pEdid[8] = (BYTE)((manuf >> 8) & 0xFF);
            pEdid[9] = (BYTE)(manuf & 0xFF);

            pEdid[10] = 0x01; pEdid[11] = 0x00;    // product code
            pEdid[12] = 0x01; pEdid[13] = 0x00;
            pEdid[14] = 0x00; pEdid[15] = 0x00;    // serial
            pEdid[16] = 1;                          // week
            pEdid[17] = 34;                         // tahun = 1990 + 34

            pEdid[18] = 1; pEdid[19] = 3;           // EDID 1.3
            pEdid[20] = 0x80;                       // digital, 8 bpc, interface tidak didefinisikan
            pEdid[21] = 52;                         // horizontal size cm
            pEdid[22] = 29;                         // vertical size cm
            pEdid[23] = 78;                         // gamma 2.2
            pEdid[24] = 0x0A;                       // sRGB, preferred timing di DTD1

            // Chromaticity (nilai sRGB standar)
            pEdid[25] = 0xEE; pEdid[26] = 0x95; pEdid[27] = 0xA3; pEdid[28] = 0x54;
            pEdid[29] = 0x4C; pEdid[30] = 0x99; pEdid[31] = 0x26; pEdid[32] = 0x0F;
            pEdid[33] = 0x50; pEdid[34] = 0x54;

            pEdid[35] = 0x00;                       // established timing
            pEdid[36] = 0x01;                       // 720x400@70
            pEdid[37] = 0x01;                       // 1280x1024@60 (aman untuk semua)

            // DTD 1 : mode yang diminta client
            BYTE* dtd = &pEdid[54];
            dtd[0] = (BYTE)(pixelClock10kHz & 0xFF);
            dtd[1] = (BYTE)((pixelClock10kHz >> 8) & 0xFF);
            dtd[2] = (BYTE)(Width & 0xFF);
            dtd[3] = (BYTE)(hBlank & 0xFF);
            dtd[4] = (BYTE)(((Width >> 4) & 0xF0) | ((hBlank >> 8) & 0x0F));
            const UINT vBlank = vTotal - Height;
            dtd[5] = (BYTE)(Height & 0xFF);
            dtd[6] = (BYTE)(vBlank & 0xFF);
            dtd[7] = (BYTE)(((Height >> 4) & 0xF0) | ((vBlank >> 8) & 0x0F));
            dtd[8] = (BYTE)(hFrontPorch & 0xFF);
            dtd[9] = (BYTE)(hSyncWidth & 0xFF);
            dtd[10] = (BYTE)(vFrontPorch & 0xFF);
            dtd[11] = (BYTE)(vSyncWidth & 0xFF);
            dtd[12] = (BYTE)((((hFrontPorch >> 4) & 0xC0) | ((hSyncWidth >> 8) & 0x30) |
                              ((vFrontPorch >> 2) & 0x0C) | ((vSyncWidth >> 4) & 0x03)));
            dtd[13] = 0;
            dtd[14] = (BYTE)(Width & 0xFF);         // horizontal image size
            dtd[15] = (BYTE)(Height & 0xFF);        // vertical image size
            dtd[16] = (BYTE)(((Width >> 4) & 0xF0) | ((Height >> 8) & 0x0F));
            dtd[17] = 0;                            // border
            dtd[18] = 0;
            dtd[19] = 0x1E;                         // non-interlaced, digital separate sync, +H +V

            // DTD 2 : nama monitor
            BYTE* name = &pEdid[72];
            name[3] = 0xFC;
            name[5] = 0x00;
            const char* label = "GameStreamVD";
            for (int i = 0; i < 13; i++)
            {
                name[6 + i] = (BYTE)label[i];
            }
            name[18] = 0x0A;

            // DTD 3 : range limits
            BYTE* range = &pEdid[90];
            range[3] = 0xFD;
            range[5] = 0x00;
            range[6] = 24;                                          // min vfreq
            range[7] = (BYTE)((RefreshHz > 240) ? 240 : RefreshHz); // max vfreq
            range[8] = 15;                                          // min hfreq kHz
            range[9] = (BYTE)((hTotal * RefreshHz / 1000) + 2);     // max hfreq kHz
            range[10] = (BYTE)((pixelClock10kHz / 1000) + 1);       // max pixel clock / 10 MHz
            range[11] = 0x04;                                       // secondary GTF
            range[12] = 0x11;
            range[13] = 0x00;
            range[14] = 0x0A;
            range[15] = 0x20;
            range[16] = 0x20;
            range[17] = 0x20;
            range[18] = 0x20;

            pEdid[126] = 0;                         // tidak ada extension block
            GsEdidChecksum(pEdid);
            return true;
        }
    }
}
