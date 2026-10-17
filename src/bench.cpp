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
        // 区域拼接用的分隔符和省略号不算错误
        if (c == L'|' || c == L'…') continue;
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

// ---------------------------------------------------------------------------
//  Region crop experiment
//
//  Question: small text on a full screen. Is it better to upscale the whole
//  screen 2x, or to crop just the text area and upscale that 4x?
//
//  Both end up with a different number of pixels per glyph, but they also
//  differ in total image size, which may affect the engine. Measure it.
// ---------------------------------------------------------------------------

static HBITMAP CropDib(HBITMAP src, int sw, int sh, RectI r, int scale) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > sw) r.w = sw - r.x;
    if (r.y + r.h > sh) r.h = sh - r.y;
    if (r.w <= 0 || r.h <= 0) return nullptr;

    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    int dw = r.w * scale, dh = r.h * scale;
    void* bits = nullptr;
    HBITMAP dst = NewDib(screen, dw, dh, &bits);
    if (!dst) { ReleaseDC(nullptr, screen); return nullptr; }

    HDC sdc = CreateCompatibleDC(screen);
    HDC ddc = CreateCompatibleDC(screen);
    HGDIOBJ os = SelectObject(sdc, src);
    HGDIOBJ od = SelectObject(ddc, dst);
    SetStretchBltMode(ddc, HALFTONE);
    SetBrushOrgEx(ddc, 0, 0, nullptr);
    StretchBlt(ddc, 0, 0, dw, dh, sdc, r.x, r.y, r.w, r.h, SRCCOPY);
    SelectObject(ddc, od);
    SelectObject(sdc, os);
    DeleteDC(ddc);
    DeleteDC(sdc);
    ReleaseDC(nullptr, screen);
    return dst;
}

static double OcrDibCer(HBITMAP dib, int w, int h, const std::wstring& ref) {
    BITMAP bm{};
    GetObjectW(dib, sizeof(bm), &bm);
    if (!bm.bmBits) return 1.0;
    OcrResult r = EyeOcrPixels(bm.bmBits, w, h);
    if (!r.ok) return 1.0;
    std::sort(r.lines.begin(), r.lines.end(),
              [](const OcrLine& a, const OcrLine& b) { return a.box.y < b.box.y; });
    std::wstring hyp;
    for (const auto& line : r.lines) {
        if (!hyp.empty()) hyp += L" ";
        hyp += line.text;
    }
    return Cer(ref, hyp);
}

static void RegionExperiment(const std::wstring& ref) {
    const int CW = 1440, CH = 900;
    const int sizes[] = { 10, 12, 14, 16 };
    const int nSize = 4;

    wprintf(L"\n=== region crop vs whole screen ===\n");
    wprintf(L"canvas %dx%d, text drawn at 300,400\n", CW, CH);
    wprintf(L"%-6ls %14ls %14ls %14ls\n",
            L"px", L"full@2x", L"crop@2x", L"crop@4x");

    for (int si = 0; si < nSize; ++si) {
        int px = sizes[si];

        HDC screen = GetDC(nullptr);
        if (!screen) return;
        void* bits = nullptr;
        HBITMAP canvas = NewDib(screen, CW, CH, &bits);
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, canvas);
        RECT full{ 0, 0, CW, CH };
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(mem, &full, bg);
        DeleteObject(bg);

        HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
        HGDIOBJ oldFont = SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(24, 24, 24));
        RECT measure{ 0, 0, 0, 0 };
        DrawTextW(mem, ref.c_str(), (int)ref.size(), &measure,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        RECT tr{ 300, 400, 300 + measure.right, 400 + measure.bottom };
        DrawTextW(mem, ref.c_str(), (int)ref.size(), &tr,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(mem, oldFont);
        DeleteObject(font);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);

        // A: whole canvas at 2x
        HBITMAP fullDib = CropDib(canvas, CW, CH, RectI{ 0, 0, CW, CH }, 2);
        double cerFull = fullDib ? OcrDibCer(fullDib, CW * 2, CH * 2, ref) : 1.0;
        if (fullDib) DeleteObject(fullDib);

        // B and C: crop around the text with padding
        RectI crop{ 300 - 16, 400 - 16, measure.right + 32, measure.bottom + 32 };
        HBITMAP crop2 = CropDib(canvas, CW, CH, crop, 2);
        double cerCrop2 = crop2 ? OcrDibCer(crop2, crop.w * 2, crop.h * 2, ref) : 1.0;
        if (crop2) DeleteObject(crop2);

        HBITMAP crop4 = CropDib(canvas, CW, CH, crop, 4);
        double cerCrop4 = crop4 ? OcrDibCer(crop4, crop.w * 4, crop.h * 4, ref) : 1.0;
        if (crop4) DeleteObject(crop4);

        DeleteObject(canvas);
        wprintf(L"%-6d %14.3f %14.3f %14.3f\n", px, cerFull, cerCrop2, cerCrop4);
    }
}
// ---------------------------------------------------------------------------
//  固定截图回归：handeye bench --screen 图.png --truth 真值.txt [--threshold 0.18]
//
//  真值文件每行一条：x,y,w,h<TAB>参考文字
//  以 # 开头的行和空行忽略。截图不要进 git（那是用户的屏幕内容）。
// ---------------------------------------------------------------------------

static bool ReadAllUtf8(const std::wstring& path, std::wstring& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> buf(size_t(sz > 0 ? sz : 0) + 1, 0);
    size_t got = sz > 0 ? fread(buf.data(), 1, size_t(sz), f) : 0;
    fclose(f);
    std::string s(buf.data(), got);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s = s.substr(3);
    out = FromUtf8(s);
    return true;
}

static std::wstring RegionText(const std::vector<OcrLine>& lines, const RectI& box) {
    std::vector<const OcrLine*> picked;
    for (const auto& l : lines) {
        int cx = l.box.cx(), cy = l.box.cy();
        if (cx >= box.x - 2 && cx <= box.right() + 2 && cy >= box.y - 4 && cy <= box.bottom() + 4)
            picked.push_back(&l);
    }
    std::sort(picked.begin(), picked.end(), [](const OcrLine* a, const OcrLine* b) {
        if (a->box.y != b->box.y) return a->box.y < b->box.y;
        return a->box.x < b->box.x;
    });
    std::wstring s;
    for (size_t i = 0; i < picked.size(); ++i) {
        if (i) s += L" | ";
        s += picked[i]->text;
    }
    return s;
}

static int BenchScreen(const std::wstring& image, const std::wstring& truthPath, double threshold) {
    OcrResult r = EyeRecognizeImageFile(image);
    if (!r.ok) {
        wprintf(L"ERR %ls\n", r.error.c_str());
        return 3;
    }
    wprintf(L"图片: %ls  %dx%d  行数: %zu\n", image.c_str(), r.region.w, r.region.h, r.lines.size());
    wprintf(L"阈值: %.3f\n\n", threshold);

    std::wstring truth;
    if (!ReadAllUtf8(truthPath, truth)) {
        wprintf(L"ERR 读不到真值文件: %ls\n", truthPath.c_str());
        return 2;
    }

    double sum = 0;
    int n = 0, failed = 0;
    for (const auto& raw : Split(truth, L'\n')) {
        std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#') continue;

        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        std::wstring rectStr = Trim(line.substr(0, tab));
        std::wstring ref = Trim(line.substr(tab + 1));
        RectI box;
        auto parts = Split(rectStr, L',');
        if (parts.size() != 4) continue;
        box.x = _wtoi(parts[0].c_str());
        box.y = _wtoi(parts[1].c_str());
        box.w = _wtoi(parts[2].c_str());
        box.h = _wtoi(parts[3].c_str());

        std::wstring got = RegionText(r.lines, box);
        double cer = Cer(ref, got);
        sum += cer;
        ++n;
        if (cer > threshold) ++failed;
        wprintf(L"%-6s CER=%.3f  %s\n", (cer > threshold ? L"over" : L"ok"), cer, got.c_str());
        wprintf(L"       参考: %ls\n", ref.c_str());
    }

    if (n == 0) {
        wprintf(L"ERR 真值文件里没有有效行\n");
        return 2;
    }
    double avg = sum / n;
    wprintf(L"\n平均 CER = %.3f （%d 个区域，其中 %d 个高于阈值 %.3f；以平均值为判定）\n",
            avg, n, failed, threshold);
    return (avg > threshold) ? 1 : 0;
}

int CmdBench(const std::vector<std::wstring>& args) {
    std::wstring screen, truth;
    double threshold = 0.18;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--screen" && i + 1 < args.size()) screen = args[i + 1];
        else if (args[i] == L"--truth" && i + 1 < args.size()) truth = args[i + 1];
        else if (args[i] == L"--threshold" && i + 1 < args.size()) threshold = _wtof(args[i + 1].c_str());
    }
    if (!screen.empty()) {
        if (truth.empty()) {
            wprintf(L"ERR --screen 还需要 --truth 真值文件\n");
            return 1;
        }
        return BenchScreen(screen, truth, threshold);
    }

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

    // Matrix: raw, every font size against every upscale factor.
    // This is the table that answers "does more upscale help small text,
    // and where does it start to hurt?"
    wprintf(L"\n=== raw, error rate by size and upscale ===\n");
    wprintf(L"%-6ls %8ls %8ls %8ls %8ls\n", L"px", L"1x", L"2x", L"3x", L"4x");
    for (int si = 0; si < nSize; ++si) {
        wprintf(L"%-6d", sizes[si]);
        for (int s = 0; s < 4; ++s) {
            double v = (res[si][0][s] + res[si][1][s]) / 2.0;   // average light and dark
            wprintf(L" %8.3f", v);
        }
        wprintf(L"\n");
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

    RegionExperiment(ref);

    LogI(L"感知", 3002, L"感知", L"OCR 完成",
         L"bench=1 best_scale=%d best_mode=%ls avg=%.3f",
         cfgs[bestCfg].scale, cfgs[bestCfg].modeName, bestAvg);
    return 0;
}
