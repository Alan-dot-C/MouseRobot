// ============================================================================
//  winenum.cpp
// ============================================================================
#include "winenum.h"

#include <vector>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

struct EnumCtx {
    std::vector<WinInfo>* out;
};

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (!ctx || !ctx->out) return TRUE;

    if (!IsWindowVisible(h)) return TRUE;
    if (IsIconic(h)) return TRUE;

    // Suspended UWP apps leave "cloaked" windows behind. They report visible
    // and have a full-screen rectangle, but the user cannot see them. Without
    // this check, desktop icons get attributed to a phantom Settings window.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
        return TRUE;

    // Tool windows are things like floating palettes and tooltips. They add
    // noise without adding structure.
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t title[512] = {};
    int len = GetWindowTextW(h, title, 511);
    if (len <= 0) return TRUE;   // the desktop and other shell hosts have no title

    RECT r{};
    if (!GetWindowRect(h, &r)) return TRUE;
    int w = r.right - r.left;
    int hh = r.bottom - r.top;
    if (w < 60 || hh < 60) return TRUE;   // ignore slivers

    WinInfo wi;
    wi.hwnd = h;
    wi.rect = RectI{ r.left, r.top, w, hh };
    wi.title = title;
    // Deduplicate by title. The Settings app, for example, has a second
    // shadow window with the same title and almost the same rectangle.
    for (const auto& existing : *ctx->out) {
        if (_wcsicmp(existing.title.c_str(), wi.title.c_str()) == 0) return TRUE;
    }

    wi.z = (int)ctx->out->size();
    ctx->out->push_back(std::move(wi));
    return TRUE;
}

std::vector<WinInfo> EnumTopWindows() {
    std::vector<WinInfo> out;
    EnumCtx ctx{ &out };
    // EnumWindows walks in Z order, topmost first, which is what we want:
    // the first window containing a point is the one the user actually sees.
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

int WindowAtPoint(const std::vector<WinInfo>& wins, int x, int y) {
    for (size_t i = 0; i < wins.size(); ++i) {
        const RectI& r = wins[i].rect;
        if (x >= r.x && x < r.right() && y >= r.y && y < r.bottom()) {
            return (int)i;
        }
    }
    return -1;
}
