// ============================================================================
//  gui.cpp - a small window so the thing can be used without a terminal
//
//  This is a launcher, not a second implementation. Every button builds a
//  command line, runs handeye.exe, and shows what it printed. That keeps one
//  code path: whatever works on the command line works here.
// ============================================================================
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "gui.h"
#include "common.h"
#include "log.h"

#include <cstdio>
#include <string>
#include <vector>

#define ID_INPUT   1001
#define ID_OUTPUT  1002
#define ID_SEE     1101
#define ID_CLICK   1102
#define ID_DOCTOR  1103
#define ID_LOG     1104
#define ID_SAY     1105

static HWND g_in = nullptr;
static HWND g_out = nullptr;

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

// Run handeye.exe with the given arguments and return its stdout.
static std::wstring RunCmd(const std::wstring& args) {
    std::wstring cmd = L"\"" + ExePath() + L"\" " + args + L" 2>&1";
    FILE* p = _wpopen(cmd.c_str(), L"r");
    if (!p) return L"(无法启动)";

    std::string bytes;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) bytes.append(buf, n);
    _pclose(p);

    std::wstring text = FromUtf8(bytes);
    // Keep the box readable: cap at a sensible size.
    if (text.size() > 60000) text = text.substr(0, 60000) + L"\r\n...(截断)";
    return text;
}

static std::wstring GetInputText() {
    int len = GetWindowTextLengthW(g_in);
    if (len <= 0) return L"";
    std::vector<wchar_t> buf(size_t(len) + 1, 0);
    GetWindowTextW(g_in, buf.data(), len + 1);
    return std::wstring(buf.data());
}

static void Show(const std::wstring& text) {
    SetWindowTextW(g_out, text.c_str());
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(h, GWLP_HINSTANCE));

        CreateWindowW(L"STATIC", L"目标文字（用于按文字点击）:",
                      WS_CHILD | WS_VISIBLE, 16, 12, 300, 18, h, nullptr, inst, nullptr);

        g_in = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               16, 34, 470, 26, h, (HMENU)ID_INPUT, inst, nullptr);

        struct Btn { int id; const wchar_t* text; };
        const Btn btns[] = {
            { ID_SEE,    L"看屏幕" },
            { ID_CLICK,  L"按文字点击" },
            { ID_SAY,    L"说出来" },
            { ID_DOCTOR, L"诊断" },
            { ID_LOG,    L"最近日志" },
        };
        int x = 16;
        for (const auto& b : btns) {
            CreateWindowW(L"BUTTON", b.text,
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                          x, 70, 88, 30, h, (HMENU)(INT_PTR)b.id, inst, nullptr);
            x += 96;
        }

        g_out = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                16, 112, 470, 300, h, (HMENU)ID_OUTPUT, inst, nullptr);

        // A monospace-ish font makes the structured output line up.
        HFONT f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, FIXED_PITCH, L"Consolas");
        if (f) {
            SendMessageW(g_out, WM_SETFONT, (WPARAM)f, TRUE);
            SendMessageW(g_in, WM_SETFONT, (WPARAM)f, TRUE);
        }

        Show(L"handeye 已就绪。\r\n\r\n左侧按钮会把命令结果贴在这里。\r\n"
             L"所有动作都会写进系统日志，用「最近日志」或「诊断」查看。");
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_SEE) {
            Show(L"正在看屏幕...\r\n");
            Show(RunCmd(L"see --no-delta"));
        } else if (id == ID_CLICK) {
            std::wstring target = GetInputText();
            if (target.empty()) { Show(L"请先在上面填写要点击的文字。"); break; }
            Show(L"正在查找并点击...\r\n");
            Show(RunCmd(L"click-text \"" + target + L"\" --expect-timeout 2000"));
        } else if (id == ID_SAY) {
            std::wstring target = GetInputText();
            if (target.empty()) target = L"你好，我是 handeye。";
            Show(RunCmd(L"say \"" + target + L"\""));
        } else if (id == ID_DOCTOR) {
            Show(RunCmd(L"doctor"));
        } else if (id == ID_LOG) {
            Show(RunCmd(L"log --tail 40"));
        }
        return 0;
    }

    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        if (g_out) {
            MoveWindow(g_out, 16, 112, rc.right - 32, rc.bottom - 112 - 16, TRUE);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int RunGui() {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"handeye_gui_class";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;

    RECT rc{ 0, 0, 520, 480 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND h = CreateWindowExW(0, wc.lpszClassName, L"handeye",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, inst, nullptr);
    if (!h) return 1;

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
