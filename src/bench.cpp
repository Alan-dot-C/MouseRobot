// ============================================================================
//  bench.cpp —— OCR parameter measurement tool
//
//  Why this exists:
//  Earlier tuning of the perception layer was guesswork. Thresholds were
//  picked by feel, never measured. This tool builds its own test images with
//  a known ground truth, runs OCR over a matrix of parameters, and reports
//  the character error rate.
//
//  Variables under test:
//    font size : 10 12 14 16 20 28 px
//    theme     : light background / dark background
//    upscale   : 1 2 3 4
//    preprocess: raw / contrast / invert / binarize
//
//  Metric: CER = edit distance / reference length, after stripping spaces.
//  Lower is better. 0.00 means a perfect read.
// ============================================================================
#include "bench.h"
#include "eye.h"
#include "log.h"
#include "common.h"

#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>

// ------------------------------ image helpers ------------------------------

static HBITMAP NewDib(HDC screen, int w, int h, void** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(screen, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static HBITMAP RenderText(const std::wstring& text, int px, bool dark, int& outW, int& outH) {
    // Wide enough that the sample never wraps: wrapping makes OCR line order
    // unpredictable, which would wreck the comparison.
    const int W = 1500, H = px * 3 + 40;
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    void* bits = nullptr;
    HBITMAP dib = NewDib(screen, W, H, &bits);
    if (!dib) { ReleaseDC(nullptr, screen); return nullptr; }

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);

    RECT rc{ 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(dark ? RGB(32, 32, 32) : RGB(255, 255, 255));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    // Negative height means "character height", matching how UI font sizes
    // are usually specified. Positive height would include internal leading.
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, dark ? RGB(238, 238, 238) : RGB(24, 24, 24));
    RECT tr{ 20, 20, W - 40, H - 20 };
    DrawTextW(mem, text.c_str(), (int)text.size(), &tr,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(mem, oldFont);
    DeleteObject(font);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    outW = W;
    outH = H;
    return dib;
}

// Scale src into a fresh DIB and return it. Caller must DeleteObject.
static HBITMAP ScaleDib(HBITMAP src, int sw, int sh, int dw, int dh) {
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    void* bits = nullptr;
    HBITMAP dst = NewDib(screen, dw, dh, &bits);
    if (!dst) { ReleaseDC(nullptr, screen); return nullptr; }

    HDC sdc = CreateCompatibleDC(screen);
    HDC ddc = CreateCompatibleDC(screen);
    HGDIOBJ os = SelectObject(sdc, src);
    HGDIOBJ od = SelectObject(ddc, dst);
    SetStretchBltMode(ddc, HALFTONE);
    SetBrushOrgEx(ddc, 0, 0, nullptr);
    StretchBlt(ddc, 0, 0, dw, dh, sdc, 0, 0, sw, sh, SRCCOPY);
    SelectObject(ddc, od);
    SelectObject(sdc, os);
    DeleteDC(ddc);
    DeleteDC(sdc);
    ReleaseDC(nullptr, screen);
    return dst;
}

// Preprocess in place. mode: 0 raw, 1 contrast, 2 invert, 3 binarize
static void Preprocess(uint8_t* px, int w, int h, int mode) {
    if (mode == 0) return;
    const size_t n = size_t(w) * size_t(h);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = px + i * 4;
        int b = p[0], g = p[1], r = p[2];
        int lum = (r * 299 + g * 587 + b * 114) / 1000;

        if (mode == 1) {
            // Contrast stretch around mid grey
            auto stretch = [](int v) {
                int o = int((v - 128) * 1.9) + 128;
                return (uint8_t)(o < 0 ? 0 : (o > 255 ? 255 : o));
            };
            p[0] = stretch(b); p[1] = stretch(g); p[2] = stretch(r);
        } else if (mode == 2) {
            p[0] = (uint8_t)(255 - b);
            p[1] = (uint8_t)(255 - g);
            p[2] = (uint8_t)(255 - r);
        } else if (mode == 3) {
            uint8_t v = (lum > 128) ? 255 : 0;
            p[0] = p[1] = p[2] = v;
        }
        p[3] = 255;
    }
}

// ------------------------------ scoring ------------------------------

static std::wstring StripSpace(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (iswspace(c)) continue;
        // Normalise full-width punctuation to ASCII. Otherwise "．" vs "."
        // counts as an error even though the character was read correctly.
        switch (c) {
        case 0xFF0E: c = L'.'; break;   // ．
        case 0xFF0C: c = L','; break;   // ，
        case 0xFF1A: c = L':'; break;   // ：
        case 0xFF08: c = L'('; break;   // （
        case 0xFF09: c = L')'; break;   // ）
        case 0x3002: c = L'.'; break;   // 。
        case 0x00B7: c = L'.'; break;   // ·
        default: break;
        }
        o += c;
    }
    return o;
}

static double Cer(const std::wstring& ref, const std::wstring& hyp) {
    std::wstring a = StripSpace(ref);
    std::wstring b = StripSpace(hyp);
    if (a.empty()) return 0.0;
    int d = EditDistance(a, b);
    return double(d) / double(a.size());
}

// ------------------------------ main ------------------------------

struct Config {
    int  scale;
    int  mode;
    const wchar_t* modeName;
};

int CmdBench(const std::vector<std::wstring>& args) {
    (void)args;

    // Mixed Chinese / Latin / digits, similar to real UI text.
    const std::wstring ref = L"确认删除 通知设置 版本 26.2 Update Cancel OK";

    const int sizes[] = { 10, 12, 14, 16, 20, 28 };
    const Config cfgs[] = {
        { 1, 0, L"raw"   }, { 2, 0, L"raw"   }, { 3, 0, L"raw"   }, { 4, 0, L"raw"   },
        { 2, 1, L"contrast" }, { 3, 1, L"contrast" },
        { 2, 2, L"invert" },   { 3, 2, L"invert" },
        { 2, 3, L"binarize" }, { 3, 3, L"binarize" },
    };
    const int nCfg = (int)(sizeof(cfgs) / sizeof(cfgs[0]));
    const int nSize = (int)(sizeof(sizes) / sizeof(sizes[0]));

    wprintf(L"OCR measurement\n");
    wprintf(L"reference: %ls\n", ref.c_str());
    wprintf(L"metric: character error rate, lower is better, 0.00 = perfect\n\n");

    // results[size][theme][cfg]
    std::vector<std::vector<std::vector<double>>> res(
        nSize, std::vector<std::vector<double>>(2, std::vector<double>(nCfg, 1.0)));
    // hypothesis text at (scale 3, contrast) for each size and theme, for inspection
    std::vector<std::vector<std::wstring>> samples(nSize, std::vector<std::wstring>(2));
    std::vector<double> timeSum(nCfg, 0.0);
    std::vector<int> timeN(nCfg, 0);

    for (int si = 0; si < nSize; ++si) {
        for (int theme = 0; theme < 2; ++theme) {
            bool dark = (theme == 1);
            int w = 0, h = 0;
            HBITMAP base = RenderText(ref, sizes[si], dark, w, h);
            if (!base) continue;

            for (int ci = 0; ci < nCfg; ++ci) {
                int dw = w * cfgs[ci].scale;
                int dh = h * cfgs[ci].scale;
                HBITMAP scaled = ScaleDib(base, w, h, dw, dh);
                if (!scaled) { res[si][theme][ci] = 1.0; continue; }

                // get pixel pointer of the scaled DIB
                BITMAP bm{};
                GetObjectW(scaled, sizeof(bm), &bm);
                void* bits = bm.bmBits;
                if (!bits) { DeleteObject(scaled); res[si][theme][ci] = 1.0; continue; }

                Preprocess(static_cast<uint8_t*>(bits), dw, dh, cfgs[ci].mode);

                DWORD t0 = GetTickCount();
                OcrResult r = EyeOcrPixels(bits, dw, dh);
                DWORD elapsed = GetTickCount() - t0;
                timeSum[ci] += elapsed;
                timeN[ci]++;
                // Sort by y first. The engine does not guarantee reading order,
                // and comparing out-of-order lines would inflate the error rate.
                auto lines = r.lines;
                std::sort(lines.begin(), lines.end(),
                          [](const OcrLine& a, const OcrLine& b) { return a.box.y < b.box.y; });
                std::wstring hyp;
                for (const auto& line : lines) {
                    if (!hyp.empty()) hyp += L" ";
                    hyp += line.text;
                }
                if (cfgs[ci].scale == 3 && cfgs[ci].mode == 1) {
                    samples[si][theme] = hyp;   // keep the best config's output for inspection
                }
                res[si][theme][ci] = r.ok ? Cer(ref, hyp) : 1.0;

                DeleteObject(scaled);
            }
            DeleteObject(base);
            wprintf(L"  size %2d px  %ls  done\n", sizes[si], dark ? L"dark " : L"light");
        }
    }

    // Summary: average CER per config
    wprintf(L"\n=== average CER by configuration ===\n");
    wprintf(L"%-6ls %-10ls %8ls %8ls %8ls\n", L"scale", L"preprocess", L"avg", L"worst", L"ms");
    int bestCfg = 0;
    double bestAvg = 9.0;
    for (int ci = 0; ci < nCfg; ++ci) {
        double sum = 0;
        double worst = 0;
        int n = 0;
        for (int si = 0; si < nSize; ++si) {
            for (int t = 0; t < 2; ++t) {
                double v = res[si][t][ci];
                sum += v;
                worst = (std::max)(worst, v);
                ++n;
            }
        }
        double avg = n ? sum / n : 1.0;
        double avgMs = timeN[ci] ? timeSum[ci] / timeN[ci] : 0.0;
        wprintf(L"%-6d %-10ls %8.3f %8.3f %8.0f\n", cfgs[ci].scale, cfgs[ci].modeName, avg, worst, avgMs);
        if (avg < bestAvg) { bestAvg = avg; bestCfg = ci; }
    }

    // Per-size detail for the best config
    wprintf(L"\n=== best config: scale %d, %ls ===\n",
            cfgs[bestCfg].scale, cfgs[bestCfg].modeName);
    wprintf(L"%-8ls %10ls %10ls\n", L"px", L"light", L"dark");
    for (int si = 0; si < nSize; ++si) {
        wprintf(L"%-8d %10.3f %10.3f\n",
                sizes[si], res[si][0][bestCfg], res[si][1][bestCfg]);
    }

    wprintf(L"\n=== what OCR actually returned (scale 3, contrast) ===\n");
    for (int si = 0; si < nSize; ++si) {
        for (int t = 0; t < 2; ++t) {
            wprintf(L"%2dpx %-5ls [%ls]\n", sizes[si], t ? L"dark " : L"light",
                    samples[si][t].c_str());
        }
    }

    LogI(L"感知", 3002, L"感知", L"OCR 完成",
         L"bench=1 best_scale=%d best_mode=%ls avg=%.3f",
         cfgs[bestCfg].scale, cfgs[bestCfg].modeName, bestAvg);
    return 0;
}
