// ============================================================================
//  winenum.h - top-level window enumeration
//
//  The sieve used to group text purely by geometry, which is guesswork: a
//  sidebar and the main content can end up in the same paragraph just because
//  their line spacing happens to match.
//
//  Windows already knows where its windows are. This gives the sieve that
//  real information.
// ============================================================================
#pragma once

#include "common.h"
#include <string>
#include <vector>

struct WinInfo {
    HWND         hwnd = nullptr;
    RectI        rect;
    std::wstring title;
    int          z = 0;      // 0 = topmost
};

// Visible, titled, non-tool top-level windows, in Z order (topmost first).
std::vector<WinInfo> EnumTopWindows();

// Index into `wins` of the topmost window containing the point, or -1.
int WindowAtPoint(const std::vector<WinInfo>& wins, int x, int y);
