// ============================================================================
//  guard.cpp
// ============================================================================
#include "guard.h"
#include "eye.h"
#include "sieve.h"
#include "mouth.h"
#include "input.h"
#include "log.h"

#include <cwctype>
#include <vector>

// ------------------------------ danger words ------------------------------
//
// Deliberately short. A long list creates false positives and people learn to
// pass --yes without reading. These are the labels that actually lose data.

static const wchar_t* kDangerWords[] = {
    // Chinese
    L"删除", L"格式化", L"卸载", L"清空", L"重置", L"覆盖", L"丢弃",
    L"放弃", L"移除", L"销毁", L"抹除", L"永久", L"关机", L"注销",
    // English
    L"delete", L"format", L"uninstall", L"erase", L"wipe", L"reset",
    L"remove", L"discard", L"overwrite", L"shutdown", L"destroy", L"purge",
};

bool GuardIsDangerous(const std::wstring& text, std::wstring& hit) {
    std::wstring lower;
    lower.reserve(text.size());
    for (wchar_t c : text) lower += wchar_t(towlower(c));

    for (const wchar_t* w : kDangerWords) {
        std::wstring word = w;
        std::wstring lw;
        for (wchar_t c : word) lw += wchar_t(towlower(c));
        if (lower.find(lw) != std::wstring::npos) {
            hit = word;
            return true;
        }
    }
    return false;
}

// ------------------------------ announce ------------------------------

void GuardAnnounce(const std::wstring& text) {
    if (!g_opt.announce) return;
    // Blocking on purpose: the announcement should finish before the mouse
    // moves, otherwise the words arrive after the fact.
    MouthSpeak(text, L"", 0, 100, L"", false);
}

// ------------------------------ verify ------------------------------

static bool ScreenContains(const std::wstring& needle, int scale) {
    OcrRequest req;
    req.scale = scale > 0 ? scale : 0;   // 0 = auto
    req.merge = true;
    OcrResult r = EyeRecognize(req);
    if (!r.ok) return false;
    auto hits = SieveFind(r.lines, needle, 1);
    return !hits.empty();
}

int GuardVerify(const std::wstring& expectText,
                const std::wstring& expectGone,
                int timeoutMs,
                int scale) {
    if (expectText.empty() && expectGone.empty()) return 0;
    if (timeoutMs <= 0) timeoutMs = 3000;
    if (scale < 0) scale = 0;

    DWORD start = GetTickCount();
    for (;;) {
        bool ok = true;
        if (!expectText.empty()) ok = ok && ScreenContains(expectText, scale);
        if (!expectGone.empty()) ok = ok && !ScreenContains(expectGone, scale);
        if (ok) {
            LogI(L"动作", 4011, L"动作", L"动作验证通过",
                 L"expect=%ls expectGone=%ls ms=%lu",
                 expectText.c_str(), expectGone.c_str(),
                 (unsigned long)(GetTickCount() - start));
            return 0;
        }
        if ((int)(GetTickCount() - start) > timeoutMs) {
            LogW(L"动作", 4012, L"动作", L"动作验证超时",
                 L"expect=%ls expectGone=%ls timeout=%d",
                 expectText.c_str(), expectGone.c_str(), timeoutMs);
            return 5;
        }
        if (ShouldAbort()) {
            LogW(L"动作", 4015, L"停止", L"动作被中止", L"stage=verify %ls",
                 LogRedactText(expectText).c_str());
            return 4;
        }
        Sleep(250);
    }
}
