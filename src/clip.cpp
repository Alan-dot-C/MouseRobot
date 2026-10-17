// ============================================================================
//  clip.cpp
// ============================================================================
#include "clip.h"

#include <windows.h>
#include <cstring>

static bool OpenWithRetry() {
    for (int i = 0; i < 12; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(10);
    }
    return false;
}

bool ClipGetText(std::wstring& out) {
    out.clear();
    if (!OpenWithRetry()) return false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        auto* p = static_cast<const wchar_t*>(GlobalLock(h));
        if (p) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return h != nullptr;
}

bool ClipSetText(const std::wstring& text) {
    if (!OpenWithRetry()) return false;
    EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) { CloseClipboard(); return false; }

    void* p = GlobalLock(mem);
    if (!p) { GlobalFree(mem); CloseClipboard(); return false; }
    memcpy(p, text.c_str(), bytes);
    GlobalUnlock(mem);

    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    if (!ok) GlobalFree(mem);
    CloseClipboard();
    return ok;
}

ClipSnapshot ClipTake() {
    ClipSnapshot snap;
    snap.had = ClipGetText(snap.text);
    return snap;
}

void ClipRestore(const ClipSnapshot& snap) {
    if (snap.had) {
        ClipSetText(snap.text);
    } else {
        if (OpenWithRetry()) {
            EmptyClipboard();
            CloseClipboard();
        }
    }
}
