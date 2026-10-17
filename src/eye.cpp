// ============================================================================
//  eye.cpp —— 截屏 + OCR（纯 C++ / C++/WinRT）
// ============================================================================
#include "eye.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Globalization.h>

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

// ------------------------------ PNG 保存 (GDI+) ------------------------------

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

static bool SaveHBitmapAsPng(const std::wstring& path, HBITMAP hbmp) {
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &gsi, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        Gdiplus::Bitmap bmp(hbmp, nullptr);
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            CLSID png;
            if (GetEncoderClsid(L"image/png", &png) >= 0) {
                ok = (bmp.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok);
            }
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

static bool        g_eyeReady = false;
static OcrEngineW  g_engine{ nullptr };
static int         g_ocrTimeoutMs = 20000;

void EyeSetTimeout(int ms) { g_ocrTimeoutMs = ms; }
int  EyeGetTimeout() { return g_ocrTimeoutMs; }

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

bool EyeInit() {
    if (g_eyeReady) return g_engine != nullptr;
    g_eyeReady = true;
    try {
        // 优先简体中文；不支持则用用户配置语言
        try {
            auto lang = Windows::Globalization::Language(L"zh-CN");
            if (OcrEngineW::IsLanguageSupported(lang)) {
                g_engine = OcrEngineW::TryCreateFromLanguage(lang);
            }
        } catch (...) {}
        if (!g_engine) g_engine = OcrEngineW::TryCreateFromUserProfileLanguages();
    } catch (hresult_error const&) {
        g_engine = nullptr;
    }
    return g_engine != nullptr;
}

RectI EyeScreenRect() {
    RectI r;
    r.x = 0;
    r.y = 0;
    r.w = GetSystemMetrics(SM_CXSCREEN);
    r.h = GetSystemMetrics(SM_CYSCREEN);
    return r;
}

// 把屏幕区域抓到 32bpp 自上而下的 DIB 里，返回位图与像素指针
static HBITMAP CaptureToDib(HDC screenDC, const RectI& src, int dstW, int dstH, void** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dstW;
    bi.bmiHeader.biHeight = -dstH;   // 负数 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* p = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (!dib) return nullptr;

    HDC mem = CreateCompatibleDC(screenDC);
    HGDIOBJ old = SelectObject(mem, dib);
    SetStretchBltMode(mem, HALFTONE);
    SetBrushOrgEx(mem, 0, 0, nullptr);
    BOOL ok = StretchBlt(mem, 0, 0, dstW, dstH, screenDC, src.x, src.y, src.w, src.h, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);

    if (!ok) { DeleteObject(dib); return nullptr; }
    *bits = p;
    return dib;
}

OcrResult EyeCaptureAndOcr(const RectI& regionIn, int scale) {
    OcrResult out;
    if (!EyeInit()) {
        out.error = L"没有可用的 OCR 引擎（可能没装中文 OCR 语言包）";
        return out;
    }
    if (scale < 1) scale = 1;

    RectI region = regionIn;
    if (region.empty()) region = EyeScreenRect();
    out.region = region;
    out.scale = scale;

    try {
        int tw = region.w * scale;
        int th = region.h * scale;

        // OCR 引擎对单边尺寸有上限（通常 10000），超了会失败
        int maxDim = (int)g_engine.MaxImageDimension();
        while ((tw > maxDim || th > maxDim) && scale > 1) {
            --scale;
            tw = region.w * scale;
            th = region.h * scale;
        }
        out.scale = scale;

        HDC screen = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP dib = CaptureToDib(screen, region, tw, th, &bits);
        if (!dib) {
            ReleaseDC(nullptr, screen);
            out.error = L"截屏失败";
            return out;
        }

        auto buf = Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(
            array_view<uint8_t const>(static_cast<uint8_t*>(bits), size_t(tw) * th * 4));
        auto sb = SoftwareBitmap::CreateCopyFromBuffer(
            buf, BitmapPixelFormat::Bgra8, tw, th, BitmapAlphaMode::Ignore);

        Windows::Media::Ocr::OcrResult res{ nullptr };
        if (!AwaitOp(g_engine.RecognizeAsync(sb), g_ocrTimeoutMs, res)) {
            DeleteObject(dib);
            ReleaseDC(nullptr, screen);
            out.error = L"OCR 超时";
            out.timedOut = true;
            return out;
        }

        for (auto const& line : res.Lines()) {
            OcrLine ol;
            ol.text = line.Text().c_str();
            auto words = line.Words();
            ol.words = (int)words.Size();

            // 行边界框 = 所有词的并集
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
            if (any) {
                // 坐标从放大图换算回屏幕，并加回区域偏移
                ol.box.x = region.x + int(minX / scale);
                ol.box.y = region.y + int(minY / scale);
                ol.box.w = int((maxX - minX) / scale);
                ol.box.h = int((maxY - minY) / scale);
            } else {
                ol.box = RectI{};
            }
            if (!ol.text.empty()) out.lines.push_back(std::move(ol));
        }

        DeleteObject(dib);
        ReleaseDC(nullptr, screen);
        out.ok = true;
    } catch (hresult_error const& e) {
        out.error = e.message().c_str();
    } catch (...) {
        out.error = L"OCR 未知异常";
    }
    return out;
}

// ------------------------------ 屏幕指纹 ------------------------------

// dHash：缩到 9x8 灰度，比较每行相邻像素的亮度大小，得到 64 位。
// 对整体亮度变化不敏感，对内容变化敏感，适合判断"画面变了没有"。
uint64_t EyeScreenHash(const RectI& regionIn) {
    RectI region = regionIn;
    if (region.empty()) region = EyeScreenRect();

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CaptureToDib(screen, region, 9, 8, &bits);
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
    ReleaseDC(nullptr, screen);
    return hash;
}

int HashDistance(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    int n = 0;
    while (x) { x &= (x - 1); ++n; }
    return n;
}

OcrResult EyeOcrPixels(const void* bgra, int w, int h) {
    OcrResult out;
    out.region = RectI{ 0, 0, w, h };
    out.scale = 1;
    if (w <= 0 || h <= 0) { out.error = L"empty bitmap"; return out; }
    if (!EyeInit()) { out.error = L"no OCR engine"; return out; }

    try {
        auto buf = Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(
            array_view<uint8_t const>(static_cast<uint8_t const*>(bgra), size_t(w) * size_t(h) * 4));
        auto sb = SoftwareBitmap::CreateCopyFromBuffer(
            buf, BitmapPixelFormat::Bgra8, w, h, BitmapAlphaMode::Ignore);
        Windows::Media::Ocr::OcrResult res{ nullptr };
        if (!AwaitOp(g_engine.RecognizeAsync(sb), g_ocrTimeoutMs, res)) {
            out.error = L"OCR 超时";
            out.timedOut = true;
            return out;
        }

        for (auto const& line : res.Lines()) {
            OcrLine ol;
            ol.text = line.Text().c_str();
            auto words = line.Words();
            ol.words = (int)words.Size();
            double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
            bool any = false;
            for (auto const& word : words) {
                auto r = word.BoundingRect();
                minX = (std::min)(minX, double(r.X));
                minY = (std::min)(minY, double(r.Y));
                maxX = (std::max)(maxX, double(r.X + r.Width));
                maxY = (std::max)(maxY, double(r.Y + r.Height));
                any = true;
            }
            if (any) {
                ol.box.x = int(minX);
                ol.box.y = int(minY);
                ol.box.w = int(maxX - minX);
                ol.box.h = int(maxY - minY);
            }
            if (!ol.text.empty()) out.lines.push_back(std::move(ol));
        }
        out.ok = true;
    } catch (hresult_error const& e) {
        out.error = e.message().c_str();
    }
    return out;
}
bool EyeCoarseGrid(int gw, int gh, std::vector<uint8_t>& cells, const RectI& regionIn) {
    if (gw <= 0 || gh <= 0) return false;
    RectI region = regionIn;
    if (region.empty()) region = EyeScreenRect();

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CaptureToDib(screen, region, gw, gh, &bits);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        ReleaseDC(nullptr, screen);
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
    ReleaseDC(nullptr, screen);
    return true;
}

bool EyeSavePng(const std::wstring& path, const RectI& regionIn) {
    RectI region = regionIn;
    if (region.empty()) region = EyeScreenRect();
    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP dib = CaptureToDib(screen, region, region.w, region.h, &bits);
    bool ok = false;
    if (dib) {
        ok = SaveHBitmapAsPng(path, dib);
        DeleteObject(dib);
    }
    ReleaseDC(nullptr, screen);
    return ok;
}
