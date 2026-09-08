// Uji generator EDID: header, checksum, dan decode mode dari DTD 1.
#include "shim/windows.h"
#include "driver/DisplayDriver/Edid.h"
#include <cstdio>
#include <cstring>

using namespace Microsoft::GameStream;

static bool ChecksumOk(const BYTE* e) {
    unsigned s = 0;
    for (int i = 0; i < 128; i++) s += e[i];
    return (s & 0xFF) == 0;
}

static void Decode(const BYTE* e, unsigned& w, unsigned& h, unsigned& hz, unsigned& clk10k) {
    const BYTE* d = e + 54;
    clk10k = d[0] | (d[1] << 8);
    w = d[2] | ((d[4] & 0xF0) << 4);
    unsigned hBlank = d[3] | ((d[4] & 0x0F) << 8);
    h = d[5] | ((d[7] & 0xF0) << 4);
    unsigned vTotal = h + (d[6] | ((d[7] & 0x0F) << 8));
    unsigned hTotal = w + hBlank;
    hz = (unsigned)(((uint64_t)clk10k * 10000ULL) / ((uint64_t)hTotal * vTotal));
    (void)hTotal;
}

int main() {
    struct Case { unsigned w, h, hz; };
    Case cases[] = {{1920,1080,60},{1920,1080,120},{2560,1440,144},{3840,2160,60},{1280,720,165}};
    int fail = 0;

    for (auto& c : cases) {
        BYTE edid[128];
        bool ok = GsGenerateEdid(edid, c.w, c.h, c.hz);
        bool hdr = !memcmp(edid, "\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", 8);
        bool sum = ChecksumOk(edid);
        unsigned w, h, hz, clk;
        Decode(edid, w, h, hz, clk);
        unsigned tol = c.hz / 100 + 1;   // toleransi 1%
        bool mode = (w == c.w && h == c.h && hz + tol >= c.hz && c.hz + tol >= hz);
        bool ext = edid[126] == 0;
        printf("%ux%u@%u : gen=%d header=%d checksum=%d mode=%ux%u@%u clk10k=%u ext=%d -> %s\n",
               c.w, c.h, c.hz, (int)ok, (int)hdr, (int)sum, w, h, hz, clk, (int)ext,
               (ok && hdr && sum && mode && ext) ? "PASS" : "FAIL");
        if (!(ok && hdr && sum && mode && ext)) fail++;
    }

    // Batas harus ditolak
    BYTE e[128];
    if (GsGenerateEdid(e, 100, 100, 60)) { printf("reject tiny: FAIL\n"); fail++; }
    else printf("reject tiny: PASS\n");
    if (GsGenerateEdid(e, 1920, 1080, 5)) { printf("reject 5Hz: FAIL\n"); fail++; }
    else printf("reject 5Hz: PASS\n");
    if (GsGenerateEdid(e, 1920, 1080, 999)) { printf("reject 999Hz: FAIL\n"); fail++; }
    else printf("reject 999Hz: PASS\n");

    printf(fail ? "\nHASIL: %d GAGAL\n" : "\nHASIL: semua PASS\n", fail);
    return fail;
}
