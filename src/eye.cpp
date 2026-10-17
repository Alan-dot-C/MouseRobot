// ============================================================================
//  eye.cpp —— 截屏 + OCR（纯 C++ / C++/WinRT）
//
//  两路识别：
//    中文行  -> zh-Hans-CN @1x（放大反而会丢词）
//    拉丁/数字 -> en-GB @2~3x（中文模型会把 "World" 认成 "Wor1d"）
//  合并逐行进行：按行框对齐，CJK 占比高的保留中文结果，否则用英文结果。
// ============================================================================
#include "eye.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Globalization.h>

#include <algorithm>
#include <cmath>
#include <vector>

// gdiplus.h 需要 IStream / PROPID 等 COM 定义，而 WIN32_LEAN_AND_MEAN
// 把它们排除了，所以必须先手动引入 objidl.h
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace winrt;
using namespace Windows::Graphics::Imaging;

// 注意: 不能写 using namespace Windows::Media::Ocr;
// 它里面的 OcrLine / OcrResult 会和本项目 eye.h 里的同名结构冲突。
using OcrEngineW = Windows::Media::Ocr::OcrEngine;
using OcrResultW = Windows::Media::Ocr::OcrResult;

// ------------------------------ GDI+ ------------------------------

static ULONG_PTR g_gdiToken = 0;

static void EnsureGdiPlus() {
    if (g_gdiToken) return;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdiToken, &gsi, nullptr);
}

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, format) == 0) { *pClsid = codecs[i].Clsid; return (int)i; }
    }
    return -1;
}

// ------------------------------ 引擎 ------------------------------
static bool        g_eyeReady = false;
static OcrEngineW  g_zh{ nullptr };
static OcrEngineW  g_en{ nullptr };
static int         g_ocrTimeoutMs = 20000;

void EyeSetTimeout(int ms) { g_ocrTimeoutMs = ms; }
int  EyeGetTimeout() { return g_ocrTimeoutMs; }

static OcrEngineW MakeEngine(const wchar_t* tag) {
    try {
        auto lang = Windows::Globalization::Language(tag);
        if (OcrEngineW::IsLanguageSupported(lang)) return OcrEngineW::TryCreateFromLanguage(lang);
    } catch (...) {}
    return nullptr;
}

bool EyeInit() {
    if (g_eyeReady) return g_zh != nullptr || g_en != nullptr;
    g_eyeReady = true;
    try {
        g_zh = MakeEngine(L"zh-Hans-CN");
        if (!g_zh) g_zh = MakeEngine(L"zh-CN");
        if (!g_zh) g_zh = OcrEngineW::TryCreateFromUserProfileLanguages();

        g_en = MakeEngine(L"en-GB");
        if (!g_en) g_en = MakeEngine(L"en-US");
        if (!g_en) g_en = MakeEngine(L"en");
    } catch (hresult_error const&) {
    }
    return g_zh != nullptr || g_en != nullptr;
}

// WinRT 的 .get() 会无限阻塞。轮询 Status，超时就 Cancel 并失败返回。
template <typename T>
static bool AwaitOp(winrt::Windows::Foundation::IAsyncOperation<T> const& op, int timeoutMs, T& out) {
    using winrt::Windows::Foundation::AsyncStatus;
    DWORD start = GetTickCount();
    for (;;) {
        AsyncStatus st = op.Status();
        if (st == AsyncStatus::Completed) {
            out = op.GetResults();
            return true;
        }
        if (st != AsyncStatus::Started) return false;   // Canceled / Error
        if (timeoutMs > 0 && (int)(GetTickCount() - start) > timeoutMs) {
            try { op.Cancel(); } catch (...) {}
            return false;
        }
        Sleep(5);
    }
}

RectI EyeScreenRect() {
    RectI r;
    r.x = 0;
    r.y = 0;
    r.w = GetSystemMetrics(SM_CXSCREEN);
    r.h = GetSystemMetrics(SM_CYSCREEN);
    return r;
}

// ------------------------------ 图像 ------------------------------

struct Img {
    std::vector<uint8_t> px;   // BGRA, 自上而下
    int w = 0, h = 0;
};

static bool CaptureRaw(const RectI& r, Img& out) {
    if (r.w <= 0 || r.h <= 0) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = r.w;
    bi.bmiHeader.biHeight = -r.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { ReleaseDC(nullptr, screen); return false; }

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    BOOL ok = BitBlt(mem, 0, 0, r.w, r.h, screen, r.x, r.y, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);

    if (!ok) { DeleteObject(dib); ReleaseDC(nullptr, screen); return false; }

    out.w = r.w;
    out.h = r.h;
    out.px.assign(static_cast<uint8_t*>(bits),
                  static_cast<uint8_t*>(bits) + size_t(r.w) * size_t(r.h) * 4);
    // CreateDIBSection 不填 alpha，GDI+ 会当成全透明；这里直接刷成不透明
    for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;

    DeleteObject(dib);
    ReleaseDC(nullptr, screen);
    return true;
}

// 取采样中位色当背景色。比"取左上角像素"稳：角上可能正好是图标或文字。
static void MedianColor(const Img& s, uint8_t out[4]) {
    int hist[4][256] = {};
    long long n = 0;
    const size_t total = size_t(s.w) * size_t(s.h);
    for (size_t i = 0; i < total; i += 7) {
        const uint8_t* p = &s.px[i * 4];
        for (int c = 0; c < 4; ++c) hist[c][p[c]]++;
        ++n;
    }
    for (int c = 0; c < 4; ++c) {
        long long half = n / 2, acc = 0;
        int v = 0;
        for (; v < 256; ++v) { acc += hist[c][v]; if (acc >= half) break; }
        out[c] = (uint8_t)(v > 255 ? 255 : v);
    }
    out[3] = 255;
}

static void PadImg(const Img& s, int pad, Img& d) {
    if (pad <= 0) { d = s; return; }
    uint8_t bg[4] = { 0, 0, 0, 255 };
    MedianColor(s, bg);

    d.w = s.w + 2 * pad;
    d.h = s.h + 2 * pad;
    d.px.assign(size_t(d.w) * size_t(d.h) * 4, 255);
    for (size_t i = 0; i < size_t(d.w) * size_t(d.h); ++i) {
        d.px[i * 4 + 0] = bg[0];
        d.px[i * 4 + 1] = bg[1];
        d.px[i * 4 + 2] = bg[2];
        d.px[i * 4 + 3] = 255;
    }
    for (int y = 0; y < s.h; ++y) {
        memcpy(&d.px[(size_t(y + pad) * d.w + pad) * 4],
               &s.px[size_t(y) * s.w * 4],
               size_t(s.w) * 4);
    }
}

static bool CropImg(const Img& s, int x, int y, int w, int h, Img& d) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s.w) w = s.w - x;
    if (y + h > s.h) h = s.h - y;
    if (w <= 0 || h <= 0) return false;
    d.w = w;
    d.h = h;
    d.px.resize(size_t(w) * size_t(h) * 4);
    for (int row = 0; row < h; ++row) {
        memcpy(&d.px[size_t(row) * w * 4], &s.px[(size_t(y + row) * s.w + x) * 4], size_t(w) * 4);
    }
    return true;
}

// 整数倍放大。GDI+ 的 HighQualityBicubic 对 OCR 比 GDI 的 HALFTONE 稳
// （HALFTONE 本来就是给缩图设计的，用它放大是糊的）。
static bool ResizeImg(const Img& s, int factor, Img& d, bool nearest = false) {
    if (factor <= 1) { d = s; return true; }
    EnsureGdiPlus();
    d.w = s.w * factor;
    d.h = s.h * factor;
    d.px.assign(size_t(d.w) * size_t(d.h) * 4, 255);

    Gdiplus::Bitmap src(s.w, s.h, s.w * 4, PixelFormat32bppRGB,
                        const_cast<BYTE*>(s.px.data()));
    Gdiplus::Bitmap dst(d.w, d.h, d.w * 4, PixelFormat32bppRGB, d.px.data());
    if (src.GetLastStatus() != Gdiplus::Ok || dst.GetLastStatus() != Gdiplus::Ok) return false;

    {
        Gdiplus::Graphics g(&dst);
        // 整数倍放大：最近邻保边最狠，实测英文识别 0.000；
        // 双三次偏软，会把 W 糊成 m（Whatever -> matever）。
        g.SetInterpolationMode(nearest ? Gdiplus::InterpolationModeNearestNeighbor
                                       : Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(&src, 0, 0, d.w, d.h);
    }
    return true;
}

// 从文件读一张图（回归测量用）。支持 png/jpg/bmp。
static bool LoadImgFile(const std::wstring& path, Img& out) {
    EnsureGdiPlus();
    Gdiplus::Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;

    const int w = (int)bmp.GetWidth();
    const int h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;

    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData data{};
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &data) != Gdiplus::Ok)
        return false;

    out.w = w;
    out.h = h;
    out.px.resize(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        const auto* src = static_cast<const uint8_t*>(data.Scan0) + ptrdiff_t(y) * data.Stride;
        memcpy(&out.px[size_t(y) * w * 4], src, size_t(w) * 4);
    }
    bmp.UnlockBits(&data);
    return true;
}

// ------------------------------ 文本度量 ------------------------------

static double CjkRatio(const std::wstring& s) {
    int cjk = 0, all = 0;
    for (wchar_t c : s) {
        if (iswspace(c)) continue;
        ++all;
        if (IsCjk(c)) ++cjk;
    }
    return all ? double(cjk) / double(all) : 0.0;
}

static bool InGb2312(wchar_t c) {
    if (c < 128) return true;
    char buf[4] = {};
    int n = WideCharToMultiByte(936, 0, &c, 1, buf, sizeof(buf), nullptr, nullptr);
    return n == 2;
}

// 0..1。低于 0.6 基本可以认为这行有乱码或丢词。
static double TextQuality(const std::wstring& text, const RectI& box) {
    if (text.empty()) return 0.0;
    int nonspace = 0, good = 0;
    for (wchar_t c : text) {
        if (iswspace(c)) continue;
        ++nonspace;
        if (InGb2312(c)) ++good;
    }
    if (nonspace == 0) return 0.0;
    double gb = double(good) / double(nonspace);

    double cjk = CjkRatio(text);
    double charW = (cjk >= 0.5) ? box.h * 0.95 : box.h * 0.55;
    double expect = (charW > 1.0 && box.w > 0) ? double(box.w) / charW : nonspace;
    double dens = (expect > 0) ? (std::min)(1.0, double(nonspace) / expect) : 0.0;

    return 0.6 * gb + 0.4 * dens;
}

// ------------------------------ 一次识别 ------------------------------

// img 是"内容 + 四周 pad"的图；region 是内容在屏幕上的矩形。
// 像素 (px,py) 对应的屏幕坐标 = region.x + (px - pad) / scale
static bool RecognizeImg(const Img& img, bool english, int scale,
                         const RectI& region, int padPx,
                         std::vector<OcrLine>& out, std::wstring& err, bool& timedOut) {
    OcrEngineW engine = english ? g_en : g_zh;
    if (!engine) engine = g_zh ? g_zh : g_en;
    if (!engine) { err = L"没有可用的 OCR 引擎"; return false; }

    try {
        auto buf = Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(
            array_view<uint8_t const>(img.px.data(), img.px.size()));
        auto sb = SoftwareBitmap::CreateCopyFromBuffer(
            buf, BitmapPixelFormat::Bgra8, img.w, img.h, BitmapAlphaMode::Ignore);

        OcrResultW res{ nullptr };
        if (!AwaitOp(engine.RecognizeAsync(sb), g_ocrTimeoutMs, res)) {
            err = L"OCR 超时";
            timedOut = true;
            return false;
        }

        for (auto const& line : res.Lines()) {
            OcrLine ol;
            ol.text = line.Text().c_str();
            auto words = line.Words();
            ol.words = (int)words.Size();

            double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
            bool any = false;
            for (auto const& w : words) {
                auto r = w.BoundingRect();
                minX = (std::min)(minX, double(r.X));
                minY = (std::min)(minY, double(r.Y));
                maxX = (std::max)(maxX, double(r.X + r.Width));
                maxY = (std::max)(maxY, double(r.Y + r.Height));
                any = true;
            }
            if (!any) continue;

            ol.box.x = region.x + int(std::lround((minX - padPx) / scale));
            ol.box.y = region.y + int(std::lround((minY - padPx) / scale));
            ol.box.w = int(std::lround((maxX - minX) / scale));
            ol.box.h = int(std::lround((maxY - minY) / scale));
            if (ol.box.w <= 0 || ol.box.h <= 0) continue;

            ol.quality = TextQuality(ol.text, ol.box);
            ol.src = english ? L"en" : L"zh";
            if (scale > 1) ol.src += std::to_wstring(scale) + L"x";
            else ol.src += L"1x";

            if (!ol.text.empty()) out.push_back(std::move(ol));
        }
        return true;
    } catch (hresult_error const& e) {
        err = e.message().c_str();
        return false;
    } catch (...) {
        err = L"OCR 未知异常";
        return false;
    }
}

// ------------------------------ 合并 ------------------------------

static const OcrLine* FindMatch(const OcrLine& a, const std::vector<OcrLine>& pool) {
    const OcrLine* best = nullptr;
    double bestD = 1e18;
    for (const auto& b : pool) {
        if (VerticalOverlap(a.box, b.box) < 0.5) continue;
        double d = std::abs(double(a.box.cx()) - double(b.box.cx()));
        double limit = (std::max)(a.box.w, b.box.w) * 0.6 + 8.0;
        if (d <= limit && d < bestD) { bestD = d; best = &b; }
    }
    return best;
}

static bool OverlapsAny(const OcrLine& l, const std::vector<OcrLine>& pool) {
    for (const auto& b : pool) {
        if (VerticalOverlap(l.box, b.box) < 0.5) continue;
        int lx = (std::max)(l.box.x, b.box.x);
        int rx = (std::min)(l.box.right(), b.box.right());
        if (rx - lx > l.box.w / 2) return true;
    }
    return false;
}

// 把英文通道的结果并进中文通道：
//   - 同一行：中文占比高的保留中文（中文模型更准），否则用英文
//   - 英文通道独有的行：只在确实是拉丁文本、且不压在任何中文行上时加入
static void MergeLatinPass(std::vector<OcrLine>& zh, const std::vector<OcrLine>& en) {
    for (auto& a : zh) {
        const OcrLine* b = FindMatch(a, en);
        if (!b) continue;
        if (CjkRatio(a.text) >= 0.5) continue;
        if (CjkRatio(b->text) >= 0.3) continue;
        if (b->text.size() + 2 < a.text.size()) continue;   // 英文那边更短，不采信
        a.text = b->text;
        a.src = b->src;
        a.quality = (std::max)(a.quality, b->quality);
    }

    for (const auto& b : en) {
        if (CjkRatio(b.text) >= 0.2) continue;
        if (b.text.size() < 2) continue;
        if (OverlapsAny(b, zh)) continue;
        zh.push_back(b);
    }
}

// ------------------------------ 二次看 ------------------------------

static bool HasLetterDigitAdjacency(const std::wstring& s) {
    for (size_t i = 1; i < s.size(); ++i) {
        wchar_t a = s[i - 1], b = s[i];
        if ((iswalpha(a) && iswdigit(b)) || (iswdigit(a) && iswalpha(b))) return true;
    }
    return false;
}

static bool HasLoneAsciiInCjk(const std::wstring& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= 128 || !iswalnum(s[i])) continue;
        bool prev = i > 0 && IsCjk(s[i - 1]);
        bool next = i + 1 < s.size() && IsCjk(s[i + 1]);
        if (prev || next) return true;
    }
    return false;
}

static bool ShouldSecondLook(const OcrLine& l) {
    if (l.text.size() < 2) return false;
    if (CjkRatio(l.text) < 0.5) return HasLetterDigitAdjacency(l.text);
    return HasLoneAsciiInCjk(l.text);
}

// 从 1x 底图上把这一行裁出来，补留白、放大 4 倍、按脚本选引擎再认一次。
static bool SecondLook(const Img& base, int padPx, const RectI& region,
                       OcrLine& line) {
    // 屏幕坐标 -> 底图像素
    int bx = line.box.x - region.x + padPx;
    int by = line.box.y - region.y + padPx;
    int extra = (std::max)(8, line.box.h / 2);
    int x = bx - extra, y = by - extra;
    int w = line.box.w + 2 * extra, h = line.box.h + 2 * extra;

    Img crop;
    if (!CropImg(base, x, y, w, h, crop)) return false;

    Img big;
    if (!ResizeImg(crop, 4, big)) return false;

    bool english = CjkRatio(line.text) < 0.5;
    std::vector<OcrLine> got;
    std::wstring err;
    bool timedOut = false;
    // crop 的屏幕原点 = region 原点 + (x - pad)
    RectI cropRegion{ region.x + (x - padPx), region.y + (y - padPx), crop.w, crop.h };
    if (!RecognizeImg(big, english, 4, cropRegion, 0, got, err, timedOut)) return false;
    if (got.empty()) return false;

    // 取面积最大（最像整行）的那条
    const OcrLine* best = &got[0];
    for (const auto& g : got) {
        if (double(g.box.w) * g.box.h > double(best->box.w) * best->box.h) best = &g;
    }
    bool better = best->quality > line.quality + 0.05 ||
                  (best->text.size() > line.text.size() + 1 && best->quality >= line.quality - 0.05);
    if (!better) return false;

    line.text = best->text;
    line.quality = best->quality;
    line.src = best->src;
    return true;
}

// ------------------------------ 主入口 ------------------------------

static RectI ClampRect(const RectI& r, const RectI& bounds) {
    RectI o = r;
    if (o.x < bounds.x) { o.w -= (bounds.x - o.x); o.x = bounds.x; }
    if (o.y < bounds.y) { o.h -= (bounds.y - o.y); o.y = bounds.y; }
    if (o.right() > bounds.right()) o.w = bounds.right() - o.x;
    if (o.bottom() > bounds.bottom()) o.h = bounds.bottom() - o.y;
    if (o.w < 1) o.w = 1;
    if (o.h < 1) o.h = 1;
    return o;
}

// 识别一张已经抓好的图（截屏和读文件共用这一段）。
static OcrResult RecognizeFromBase(Img base, const RectI& region, int pad,
                                   const OcrRequest& reqIn) {
    OcrResult out;
    out.region = region;

    // 引擎上限（实机是 10000），超了要降倍数
    int scale = reqIn.scale > 0 ? reqIn.scale : 1;
    int maxDim = 10000;
    try { if (g_zh) maxDim = (int)g_zh.MaxImageDimension(); } catch (...) {}
    while (scale > 1 && (base.w * scale > maxDim || base.h * scale > maxDim)) --scale;
    out.scale = scale;

    bool wantZh = (reqIn.lang != L"en");
    bool wantEn = (reqIn.lang == L"en");

    Img pass1 = base;
    if (scale > 1 && !ResizeImg(base, scale, pass1)) {
        out.error = L"放大失败";
        return out;
    }

    std::wstring err;
    bool timedOut = false;
    if (wantZh && !RecognizeImg(pass1, false, scale, region, pad * scale, out.lines, err, timedOut)) {
        out.error = err;
        out.timedOut = timedOut;
        return out;
    }
    if (wantEn && !RecognizeImg(pass1, true, scale, region, pad * scale, out.lines, err, timedOut)) {
        out.error = err;
        out.timedOut = timedOut;
        return out;
    }

    // 第二路：只认拉丁/数字行，且是真的有这类行才跑
    if (reqIn.merge && reqIn.lang == L"auto" && reqIn.scale == 0) {
        int minX = base.w, minY = base.h, maxX = -1, maxY = -1;
        for (const auto& l : out.lines) {
            if (CjkRatio(l.text) >= 0.5) continue;
            if (l.text.size() < 2) continue;
            minX = (std::min)(minX, l.box.x);
            minY = (std::min)(minY, l.box.y);
            maxX = (std::max)(maxX, l.box.right());
            maxY = (std::max)(maxY, l.box.bottom());
        }
        if (maxX > minX && maxY > minY) {
            RectI bbox = ClampRect(RectI{ minX - 12, minY - 12, maxX - minX + 24, maxY - minY + 24 }, region);
            Img crop;
            if (CropImg(base, bbox.x - region.x + pad, bbox.y - region.y + pad,
                        bbox.w, bbox.h, crop)) {
                // 区域大就用 2 倍，小就用 3 倍，控制耗时
                int f = (double(bbox.w) * bbox.h > 0.5 * double(region.w) * region.h) ? 2 : 3;
                Img big;
                if (ResizeImg(crop, f, big, true)) {
                    std::vector<OcrLine> enLines;
                    std::wstring enErr;
                    if (RecognizeImg(big, true, f, bbox, 0, enLines, enErr, timedOut)) {
                        MergeLatinPass(out.lines, enLines);
                    }
                }
            }
        }
    }

    // 可疑行再认一遍（--best）
    if (reqIn.secondLook) {
        for (auto& l : out.lines) {
            if (ShouldSecondLook(l)) SecondLook(base, pad, region, l);
        }
    }

    std::stable_sort(out.lines.begin(), out.lines.end(), [](const OcrLine& a, const OcrLine& b) {
        if (a.box.y != b.box.y) return a.box.y < b.box.y;
        return a.box.x < b.box.x;
    });

    out.ok = true;
    return out;
}

OcrResult EyeRecognize(const OcrRequest& reqIn) {
    OcrResult out;
    if (!EyeInit()) {
        out.error = L"没有可用的 OCR 引擎（可能没装中文 OCR 语言包）";
        return out;
    }

    const RectI screen = EyeScreenRect();
    RectI region = reqIn.region.empty() ? screen : ClampRect(reqIn.region, screen);

    Img base;
    if (!CaptureRaw(region, base)) {
        out.error = L"截屏失败";
        return out;
    }

    // 只有显式裁了一块区域才需要补留白：整屏截图本身边缘就有内容，
    // 再补一圈等于凭空造出背景，反而可能干扰行切分。
    int pad = 0;
    if (reqIn.padPx > 0 && !reqIn.region.empty()) {
        // 注意：不能写 PadImg(base, pad, base) —— 源和目标是同一个对象时，
        // 先改宽高再读像素会越界，补出来的图直接是坏的（表现为 OCR 返回 0 行）。
        Img padded;
        PadImg(base, reqIn.padPx, padded);
        base = std::move(padded);
        pad = reqIn.padPx;
    }

    return RecognizeFromBase(std::move(base), region, pad, reqIn);
}

// 对一张图片文件走同一条管线（回归测量用）。
OcrResult EyeRecognizeImageFile(const std::wstring& path) {
    OcrResult bad;
    if (!EyeInit()) {
        bad.error = L"没有可用的 OCR 引擎";
        return bad;
    }
    Img base;
    if (!LoadImgFile(path, base)) {
        bad.error = L"图片解码失败: " + path;
        return bad;
    }
    OcrRequest req;
    RectI region{ 0, 0, base.w, base.h };
    return RecognizeFromBase(std::move(base), region, 0, req);
}

OcrResult EyeCaptureAndOcr(const RectI& region, int scale) {
    OcrRequest req;
    req.region = region;
    req.scale = scale > 0 ? scale : 1;
    req.lang = L"zh";
    req.merge = false;
    req.secondLook = false;
    return EyeRecognize(req);
}

// ------------------------------ 像素直识（测量用） ------------------------------

OcrResult EyeOcrPixels(const void* bgra, int w, int h) {
    OcrResult out;
    out.region = RectI{ 0, 0, w, h };
    out.scale = 1;
    if (w <= 0 || h <= 0) { out.error = L"empty bitmap"; return out; }
    if (!EyeInit()) { out.error = L"no OCR engine"; return out; }

    Img img;
    img.w = w;
    img.h = h;
    img.px.assign(static_cast<const uint8_t*>(bgra),
                  static_cast<const uint8_t*>(bgra) + size_t(w) * size_t(h) * 4);

    std::wstring err;
    bool timedOut = false;
    if (!RecognizeImg(img, false, 1, out.region, 0, out.lines, err, timedOut)) {
        out.error = err;
        out.timedOut = timedOut;
        return out;
    }
    out.ok = true;
    return out;
}

// ------------------------------ 屏幕指纹 ------------------------------

static HBITMAP CaptureScaled(const RectI& region, int dstW, int dstH, void** bits) {
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dstW;
    bi.bmiHeader.biHeight = -dstH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* p = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (!dib) { ReleaseDC(nullptr, screen); return nullptr; }

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    SetStretchBltMode(mem, HALFTONE);
    SetBrushOrgEx(mem, 0, 0, nullptr);
    BOOL ok = StretchBlt(mem, 0, 0, dstW, dstH, screen, region.x, region.y, region.w, region.h, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok) { DeleteObject(dib); return nullptr; }
    *bits = p;
    return dib;
}

uint64_t EyeScreenHash(const RectI& regionIn) {
    RectI region = regionIn;
    if (region.empty()) region = EyeScreenRect();

    void* bits = nullptr;
    HBITMAP dib = CaptureScaled(region, 9, 8, &bits);
    uint64_t hash = 0;
    if (dib && bits) {
        auto* px = static_cast<uint32_t*>(bits);
        int gray[8][9];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 9; ++x) {
                uint32_t c = px[y * 9 + x];
                int b = int(c & 0xFF);
                int g = int((c >> 8) & 0xFF);
                int r = int((c >> 16) & 0xFF);
                gray[y][x] = (r * 299 + g * 587 + b * 114) / 1000;
            }
        }
        int bit = 0;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                if (gray[y][x] > gray[y][x + 1]) hash |= (1ull << bit);
                ++bit;
            }
        }
    }
    if (dib) DeleteObject(dib);
    return hash;
}

int HashDistance(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    int n = 0;
    while (x) { x &= (x - 1); ++n; }
    return n;
}

bool EyeCoarseGrid(int gw, int gh, std::vector<uint8_t>& cells, const RectI& regionIn) {
    if (gw <= 0 || gh <= 0) return false;
    RectI region = regionIn;
    if (region.empty()) region = EyeScreenRect();

    void* bits = nullptr;
    HBITMAP dib = CaptureScaled(region, gw, gh, &bits);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        return false;
    }
    auto* px = static_cast<uint32_t*>(bits);
    cells.resize(size_t(gw) * size_t(gh));
    for (int i = 0; i < gw * gh; ++i) {
        uint32_t c = px[i];
        int b = int(c & 0xFF), g = int((c >> 8) & 0xFF), r = int((c >> 16) & 0xFF);
        cells[size_t(i)] = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
    }
    DeleteObject(dib);
    return true;
}

bool EyeSavePng(const std::wstring& path, const RectI& regionIn) {
    RectI region = regionIn;
    if (region.empty()) region = EyeScreenRect();

    Img img;
    if (!CaptureRaw(region, img)) return false;

    EnsureGdiPlus();
    bool ok = false;
    {
        Gdiplus::Bitmap bmp(img.w, img.h, img.w * 4, PixelFormat32bppRGB, img.px.data());
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            CLSID png;
            if (GetEncoderClsid(L"image/png", &png) >= 0) {
                ok = (bmp.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok);
            }
        }
    }
    return ok;
}
