// ============================================================================
//  input.cpp —— 手：输入模拟、脚本、录制回放
//
//  移植自早期 mouse_robot.cpp，并修掉了那里的几个真实缺陷：
//    - TEXT 改走 KEYEVENTF_UNICODE（旧版打中文会静默丢弃）
//    - 坐标先裁剪（旧版越界坐标会换算成负数，鼠标瞬移到左上角）
//    - SendInput 检查返回值
//    - DCLICK 间隔取系统双击速度
//    - FOCUS 等窗口真正成为前台，而不是固定睡 500ms
// ============================================================================
#include "input.h"

#include <cstdio>
#include <cstdarg>
#include <cwctype>
#include <deque>
#include <mutex>
#include <atomic>

// ------------------------------ 全局状态 ------------------------------

static const wchar_t* kStopEventName = L"handeye_stop_event";
static const uint32_t kRecMagic = 0x4B434148;   // 'HACK'
static const uint32_t kRecVersion = 2;

static HANDLE g_stopEvent = nullptr;
static DWORD  g_progStart = 0;
static int    g_virtX = 0, g_virtY = 0, g_virtW = 0, g_virtH = 0;

enum EvType : uint32_t {
    EV_MOVE = 1, EV_LDOWN, EV_LUP, EV_RDOWN, EV_RUP,
    EV_MDOWN, EV_MUP, EV_WHEEL, EV_KEYDOWN, EV_KEYUP,
};

struct Ev {
    uint32_t type;
    int32_t  x, y;
    int32_t  wheel;
    uint32_t vk, scan;
    uint32_t extended;
    uint32_t t;
};

static std::vector<Ev> g_log;
static std::atomic<bool> g_recording{ false };
static DWORD g_recStart = 0;
static HHOOK g_mouseHook = nullptr;
static HHOOK g_keyHook = nullptr;
static std::deque<int> g_keyQueue;
static std::mutex g_keyMutex;
static bool g_keyDown[256] = { false };
static bool g_recordInjected = false;

// ------------------------------ 基础工具 ------------------------------

static int ClampX(int x) {
    if (g_virtW <= 0) return x;
    if (x < g_virtX) return g_virtX;
    if (x > g_virtX + g_virtW - 1) return g_virtX + g_virtW - 1;
    return x;
}
static int ClampY(int y) {
    if (g_virtH <= 0) return y;
    if (y < g_virtY) return g_virtY;
    if (y > g_virtY + g_virtH - 1) return g_virtY + g_virtH - 1;
    return y;
}

bool InputInit() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_virtX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_virtY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_virtW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_virtH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_progStart = GetTickCount();
    if (!g_stopEvent) {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
    }
    return g_stopEvent != nullptr;
}

void InputShutdown() {
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
}

static bool StopRequested() {
    return g_stopEvent && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0;
}

static bool DurationExceeded() {
    if (g_opt.maxDurationSec <= 0) return false;
    return (GetTickCount() - g_progStart) > DWORD(g_opt.maxDurationSec) * 1000u;
}

bool ShouldAbort() { return StopRequested() || DurationExceeded(); }

static void SleepInterruptible(DWORD ms) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        if (ShouldAbort()) break;
        DWORD left = ms - (GetTickCount() - start);
        Sleep(left > 20 ? 20 : left);
    }
}

// ------------------------------ 输入发送 ------------------------------

static void SendMouseAbs(DWORD flags, int x, int y, int wheel = 0) {
    if (g_opt.dryRun) return;
    int cx = ClampX(x), cy = ClampY(y);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.dx = LONG(double(cx - g_virtX) * 65535.0 / (g_virtW > 1 ? g_virtW - 1 : 1));
    in.mi.dy = LONG(double(cy - g_virtY) * 65535.0 / (g_virtH > 1 ? g_virtH - 1 : 1));
    in.mi.mouseData = DWORD(wheel);
    if (SendInput(1, &in, sizeof(INPUT)) != 1)
        Say(L"警告: SendInput 被系统丢弃 (错误码 %lu)", GetLastError());
}

void MoveTo(int x, int y, DWORD msSmooth) {
    int tx = ClampX(x), ty = ClampY(y);
    if (g_opt.dryRun) { Say(L"[dry-run] MOVE %d %d", tx, ty); return; }
    POINT cur;
    GetCursorPos(&cur);
    if (msSmooth == 0 || (cur.x == tx && cur.y == ty)) {
        SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, tx, ty);
        return;
    }
    int steps = int(msSmooth / 20);
    if (steps < 2) steps = 2;
    if (steps > 120) steps = 120;
    DWORD per = msSmooth / DWORD(steps);
    for (int i = 1; i <= steps; ++i) {
        double t = double(i) / steps;
        double e = 1.0 - (1.0 - t) * (1.0 - t);   // ease-out
        int nx = cur.x + int((tx - cur.x) * e);
        int ny = cur.y + int((ty - cur.y) * e);
        SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny);
        if (ShouldAbort()) return;
        Sleep(per);
    }
    Sleep(60);
}

void ClickNow(bool right) {
    if (g_opt.dryRun) { Say(L"[dry-run] CLICK"); return; }
    POINT p;
    GetCursorPos(&p);
    DWORD down = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    DWORD up = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | down, p.x, p.y);
    SleepInterruptible(60);
    SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | up, p.x, p.y);
    Sleep(80);
}

void DoubleClickNow() {
    DWORD dci = GetDoubleClickTime();
    DWORD gap = dci / 4;
    if (gap < 30) gap = 30;
    if (gap > 200) gap = 200;
    ClickNow(false);
    SleepInterruptible(gap);
    ClickNow(false);
}

void ClickAt(int x, int y, bool right) {
    MoveTo(x, y, 300);
    SleepInterruptible(120);
    ClickNow(right);
}

void WheelAt(int x, int y, int delta) {
    if (delta % 120 != 0) {
        Say(L"警告: WHEEL 的值应为 120 的倍数，收到 %d", delta);
    }
    SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_WHEEL, x, y, delta);
}

void LeftButton(bool down) {
    if (g_opt.dryRun) { Say(L"[dry-run] LEFT %ls", down ? L"DOWN" : L"UP"); return; }
    POINT p;
    GetCursorPos(&p);
    SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                 (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP), p.x, p.y);
}

void HoldKey(int vk, bool down) {
    if (g_opt.dryRun) { Say(L"[dry-run] KEY %d %ls", vk, down ? L"DOWN" : L"UP"); return; }
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = WORD(vk);
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (SendInput(1, &in, sizeof(INPUT)) != 1)
        Say(L"警告: 按键输入被系统丢弃 (错误码 %lu)", GetLastError());
}

void PressKey(int vk, bool extended) {
    if (g_opt.dryRun) { Say(L"[dry-run] PRESS %d", vk); return; }
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = WORD(vk);
    if (extended) in[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    if (SendInput(2, in, sizeof(INPUT)) != 2)
        Say(L"警告: 按键输入被系统丢弃 (错误码 %lu)", GetLastError());
}

// Unicode 输入：不受键盘布局限制，中文也能打
static void TypeUnicode(wchar_t ch) {
    if (g_opt.dryRun) return;
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = 0;
    in[0].ki.wScan = ch;
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    if (SendInput(2, in, sizeof(INPUT)) != 2)
        Say(L"警告: 字符输入被系统丢弃 (错误码 %lu)", GetLastError());
}

bool TypeText(const std::wstring& text, int charDelayMs) {
    if (g_opt.dryRun) { Say(L"[dry-run] TEXT %ls", text.c_str()); return true; }
    for (wchar_t c : text) {
        TypeUnicode(c);
        if (charDelayMs > 0) Sleep(charDelayMs);
        if (ShouldAbort()) return false;
    }
    return true;
}

// ------------------------------ 键名解析 ------------------------------

struct KeyName { const wchar_t* name; int vk; bool ext; };

static const KeyName kKeys[] = {
    { L"ENTER", VK_RETURN, false }, { L"RETURN", VK_RETURN, false },
    { L"TAB", VK_TAB, false }, { L"ESC", VK_ESCAPE, false }, { L"ESCAPE", VK_ESCAPE, false },
    { L"SPACE", VK_SPACE, false }, { L"BACKSPACE", VK_BACK, false }, { L"BACK", VK_BACK, false },
    { L"DELETE", VK_DELETE, true }, { L"DEL", VK_DELETE, true },
    { L"INSERT", VK_INSERT, true }, { L"INS", VK_INSERT, true },
    { L"HOME", VK_HOME, true }, { L"END", VK_END, true },
    { L"PGUP", VK_PRIOR, true }, { L"PGDN", VK_NEXT, true },
    { L"UP", VK_UP, true }, { L"DOWN", VK_DOWN, true },
    { L"LEFT", VK_LEFT, true }, { L"RIGHT", VK_RIGHT, true },
    { L"LWIN", VK_LWIN, true }, { L"RWIN", VK_RWIN, true }, { L"WIN", VK_LWIN, true },
    { L"APPS", VK_APPS, true },
    { L"CTRL", VK_CONTROL, false }, { L"CONTROL", VK_CONTROL, false },
    { L"SHIFT", VK_SHIFT, false }, { L"ALT", VK_MENU, false },
    { L"CAPS", VK_CAPITAL, false }, { L"PRTSC", VK_SNAPSHOT, true },
};

bool ResolveKeyName(const std::wstring& raw, int& vk, bool& ext) {
    std::wstring name = Trim(raw);
    for (wchar_t& c : name) c = towupper(c);
    if (name.empty()) return false;

    // 0x41 形式的十六进制
    if (name.size() >= 3 && name[0] == L'0' && name[1] == L'X') {
        wchar_t* endp = nullptr;
        long v = wcstol(name.c_str() + 2, &endp, 16);
        if (endp && *endp == L'\0' && v > 0 && v < 256) { vk = int(v); ext = false; return true; }
    }
    // F1..F24
    if (name.size() >= 2 && name.size() <= 3 && name[0] == L'F' && iswdigit(name[1])) {
        int n = _wtoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) { vk = VK_F1 + n - 1; ext = false; return true; }
    }
    for (const auto& k : kKeys) {
        if (_wcsicmp(k.name, name.c_str()) == 0) { vk = k.vk; ext = k.ext; return true; }
    }
    if (name.size() == 1) {
        SHORT sc = VkKeyScanW(name[0]);
        if (sc != -1) { vk = sc & 0xFF; ext = false; return true; }
    }
    return false;
}

bool PressKeyCombo(const std::wstring& combo) {
    int mods = 0;
    std::vector<int> keys;
    std::vector<bool> exts;
    for (auto& part : Split(combo, L'+')) {
        std::wstring t = ToUpper(Trim(part));
        if (t.empty()) continue;
        if (t == L"CTRL" || t == L"CONTROL") mods |= 1;
        else if (t == L"WIN" || t == L"LWIN") mods |= 2;
        else if (t == L"ALT") mods |= 4;
        else if (t == L"SHIFT") mods |= 8;
        else {
            int vk = 0;
            bool ext = false;
            if (!ResolveKeyName(t, vk, ext)) return false;
            keys.push_back(vk);
            exts.push_back(ext);
        }
    }
    if (keys.empty()) return false;
    if (g_opt.dryRun) { Say(L"[dry-run] COMBO %ls", combo.c_str()); return true; }

    std::vector<INPUT> in;
    auto push = [&in](WORD vk, DWORD flags, bool ext) {
        INPUT x{};
        x.type = INPUT_KEYBOARD;
        x.ki.wVk = vk;
        x.ki.dwFlags = flags;
        if (ext) x.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        in.push_back(x);
    };
    if (mods & 1) push(VK_CONTROL, 0, false);
    if (mods & 2) push(VK_LWIN, 0, false);
    if (mods & 4) push(VK_MENU, 0, false);
    if (mods & 8) push(VK_SHIFT, 0, false);
    for (size_t i = 0; i < keys.size(); ++i) {
        push(WORD(keys[i]), 0, exts[i]);
        push(WORD(keys[i]), KEYEVENTF_KEYUP, exts[i]);
    }
    if (mods & 8) push(VK_SHIFT, KEYEVENTF_KEYUP, false);
    if (mods & 4) push(VK_MENU, KEYEVENTF_KEYUP, false);
    if (mods & 2) push(VK_LWIN, KEYEVENTF_KEYUP, false);
    if (mods & 1) push(VK_CONTROL, KEYEVENTF_KEYUP, false);
    if (SendInput(UINT(in.size()), in.data(), sizeof(INPUT)) != in.size())
        Say(L"警告: 组合键输入被系统丢弃 (错误码 %lu)", GetLastError());
    return true;
}

// ------------------------------ 窗口 ------------------------------

struct FindCtx { std::wstring needle; HWND best; int score; };

static BOOL CALLBACK FindProc(HWND h, LPARAM lp) {
    FindCtx* ctx = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t buf[512] = {};
    GetWindowTextW(h, buf, 512);
    if (!buf[0]) return TRUE;
    if (!wcsstr(buf, ctx->needle.c_str())) return TRUE;

    int score = 0;
    if (!IsIconic(h)) score += 2;
    if (_wcsicmp(buf, ctx->needle.c_str()) == 0) score += 4;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) score -= 1;
    if (score > ctx->score) { ctx->score = score; ctx->best = h; }
    return TRUE;
}

HWND FindWindowByTitle(const std::wstring& needle) {
    FindCtx ctx{ needle, nullptr, -1000 };
    EnumWindows(FindProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

bool FocusWindow(const std::wstring& needle, DWORD waitMs) {
    HWND h = nullptr;
    DWORD start = GetTickCount();
    do {
        h = FindWindowByTitle(needle);
        if (h) break;
        Sleep(100);
    } while (GetTickCount() - start < 1500);

    if (!h) return false;
    ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);

    // 等它真正成为前台，而不是睡固定时间
    while (GetTickCount() - start < waitMs) {
        if (GetForegroundWindow() == h) return true;
        if (ShouldAbort()) break;
        Sleep(20);
    }
    return GetForegroundWindow() == h;
}

bool WaitForWindow(const std::wstring& needle, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    for (;;) {
        if (FindWindowByTitle(needle)) return true;
        if (GetTickCount() - start > timeoutMs) return false;
        if (ShouldAbort()) return false;
        Sleep(100);
    }
}

// ------------------------------ 脚本 ------------------------------

struct KeyVal { std::wstring key, val; };
static std::vector<KeyVal> g_vars;

static std::wstring ExpandVars(const std::wstring& in) {
    std::wstring out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'$' && i + 1 < in.size() && in[i + 1] == L'{') {
            size_t close = in.find(L'}', i + 2);
            if (close != std::wstring::npos) {
                std::wstring name = in.substr(i + 2, close - i - 2);
                std::wstring val;
                for (auto& kv : g_vars) if (kv.key == name) { val = kv.val; break; }
                out += val;
                i = close;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

static int ParseInts(const std::wstring& s, int* out, int maxN) {
    int n = 0;
    const wchar_t* p = s.c_str();
    while (n < maxN) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        wchar_t* endp = nullptr;
        long v = wcstol(p, &endp, 10);
        if (endp == p) break;
        out[n++] = int(v);
        p = endp;
    }
    return n;
}

static bool ReadTextFile(const std::wstring& path, std::wstring& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return true; }
    std::vector<char> buf(size_t(sz) + 1, 0);
    size_t got = fread(buf.data(), 1, size_t(sz), f);
    fclose(f);
    if (got != size_t(sz)) return false;
    size_t off = 0;
    if (size_t(sz) >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) off = 3;
    out = FromUtf8(std::string(buf.data() + off, size_t(sz) - off));
    if (!out.empty() && out[0] == 0xFEFF) out.erase(0, 1);
    return true;
}

static bool RunScriptLine(const std::wstring& rawLine, int lineNo) {
    std::wstring ln = ExpandVars(rawLine);
    size_t sp = ln.find_first_of(L" \t");
    std::wstring cmd = ToUpper(ln.substr(0, sp));
    std::wstring rest = (sp == std::wstring::npos) ? L"" : ln.substr(sp + 1);
    rest = Trim(rest);

    if (cmd == L"MOVE") {
        int a[3]{ 0, 0, 300 };
        int n = ParseInts(rest, a, 3);
        if (n < 2) { Say(L"[第 %d 行] MOVE 参数错误", lineNo); return false; }
        MoveTo(a[0], a[1], n >= 3 ? DWORD(a[2] < 0 ? 0 : a[2]) : 300);
        return true;
    }
    if (cmd == L"CLICKAT") {
        int a[2]{ 0, 0 };
        if (ParseInts(rest, a, 2) < 2) { Say(L"[第 %d 行] CLICKAT 参数错误", lineNo); return false; }
        MoveTo(a[0], a[1], 350);
        SleepInterruptible(150);
        ClickNow(false);
        return true;
    }
    if (cmd == L"MOVEREL") {
        int a[2]{ 0, 0 };
        if (ParseInts(rest, a, 2) < 2) { Say(L"[第 %d 行] MOVEREL 参数错误", lineNo); return false; }
        POINT p;
        GetCursorPos(&p);
        MoveTo(p.x + a[0], p.y + a[1], 300);
        return true;
    }
    if (cmd == L"SLEEP") {
        int ms = _wtoi(rest.c_str());
        if (ms > 0) SleepInterruptible(DWORD(ms));
        return true;
    }
    if (cmd == L"CLICK") { ClickNow(false); return true; }
    if (cmd == L"RCLICK") { ClickNow(true); return true; }
    if (cmd == L"DCLICK") { DoubleClickNow(); return true; }
    if (cmd == L"DOWN" || cmd == L"UP") {
        bool down = (cmd == L"DOWN");
        if (rest.empty()) { LeftButton(down); return true; }
        int vk = 0;
        bool ext = false;
        if (ResolveKeyName(rest, vk, ext)) { HoldKey(vk, down); return true; }
        Say(L"[第 %d 行] %ls 未知键名: %ls", lineNo, cmd.c_str(), rest.c_str());
        return false;
    }
    if (cmd == L"WHEEL") {
        int n = _wtoi(rest.c_str());
        if (n == 0 || n % 120 != 0) {
            Say(L"[第 %d 行] WHEEL 必须是 120 的倍数，收到 %d", lineNo, n);
            return false;
        }
        POINT p;
        GetCursorPos(&p);
        WheelAt(p.x, p.y, n);
        return true;
    }
    if (cmd == L"KEY") {
        if (rest.find(L'+') != std::wstring::npos) {
            if (!PressKeyCombo(rest)) { Say(L"[第 %d 行] 无法识别的组合键: %ls", lineNo, rest.c_str()); return false; }
            return true;
        }
        int vk = 0;
        bool ext = false;
        if (ResolveKeyName(rest, vk, ext)) { PressKey(vk, ext); return true; }
        if (rest.size() == 1) { TypeText(rest); return true; }
        Say(L"[第 %d 行] 未知键名: %ls", lineNo, rest.c_str());
        return false;
    }
    if (cmd == L"TEXT") { TypeText(rest); return true; }
    if (cmd == L"FOCUS") {
        if (!FocusWindow(rest)) { Say(L"[第 %d 行] FOCUS 找不到窗口: %ls", lineNo, rest.c_str()); return false; }
        return true;
    }
    if (cmd == L"WAITWIN") {
        std::wstring title = rest;
        DWORD timeout = 5000;
        size_t sp2 = rest.find_first_of(L" \t");
        if (sp2 != std::wstring::npos) {
            title = rest.substr(0, sp2);
            int a[1]{ 5000 };
            ParseInts(rest.substr(sp2), a, 1);
            timeout = DWORD(a[0] < 0 ? 0 : a[0]);
        }
        if (!WaitForWindow(title, timeout)) {
            Say(L"[第 %d 行] WAITWIN 超时 (%lums): %ls", lineNo, (unsigned long)timeout, title.c_str());
            return false;
        }
        return true;
    }
    if (cmd == L"GETPOS") {
        POINT p;
        GetCursorPos(&p);
        Say(L"GETPOS %d %d", p.x, p.y);
        return true;
    }
    if (cmd == L"SET") {
        size_t sp3 = rest.find_first_of(L" \t");
        if (sp3 == std::wstring::npos) { Say(L"[第 %d 行] SET 需要变量名和值", lineNo); return false; }
        std::wstring name = rest.substr(0, sp3);
        std::wstring val = Trim(rest.substr(sp3 + 1));
        bool found = false;
        for (auto& kv : g_vars) if (kv.key == name) { kv.val = val; found = true; break; }
        if (!found) g_vars.push_back(KeyVal{ name, val });
        return true;
    }
    Say(L"[第 %d 行] 未知命令: %ls", lineNo, cmd.c_str());
    return false;
}

int RunScriptFile(const std::wstring& path) {
    std::wstring text;
    if (!ReadTextFile(path, text)) {
        Say(L"读取脚本失败: %ls", path.c_str());
        return 2;
    }
    Say(L"执行脚本: %ls", path.c_str());
    if (g_stopEvent) ResetEvent(g_stopEvent);
    g_vars.clear();
    g_progStart = GetTickCount();

    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = Trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        ++lineNo;
        if (line.empty() || line[0] == L'#') continue;
        if (ShouldAbort()) {
            Say(L"中止于第 %d 行", lineNo);
            return 4;
        }
        if (!RunScriptLine(line, lineNo)) {
            Say(L"脚本在第 %d 行失败, 已中止", lineNo);
            return 3;
        }
    }
    Say(L"脚本执行完毕。");
    return 0;
}

// ------------------------------ 录制 ------------------------------

static void PushEvent(const Ev& e) {
    bool merge = (e.type == EV_MOVE && !g_log.empty() &&
                  g_log.back().type == EV_MOVE && e.t - g_log.back().t < 15);
    if (merge) g_log.back() = e;
    else g_log.push_back(e);
}

static void PushKey(int vk) {
    std::lock_guard<std::mutex> lk(g_keyMutex);
    g_keyQueue.push_back(vk);
}

static bool PopKey(int& vk) {
    std::lock_guard<std::mutex> lk(g_keyMutex);
    if (g_keyQueue.empty()) return false;
    vk = g_keyQueue.front();
    g_keyQueue.pop_front();
    return true;
}

static bool IsControlKey(int vk) {
    return vk == VK_SCROLL || vk == VK_PAUSE || vk == VK_ESCAPE;
}

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0 || !g_recording) return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (ms->flags & LLMHF_INJECTED) return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    Ev e{};
    e.x = ms->pt.x;
    e.y = ms->pt.y;
    e.t = GetTickCount() - g_recStart;
    switch (wParam) {
    case WM_MOUSEMOVE:   e.type = EV_MOVE; break;
    case WM_LBUTTONDOWN: e.type = EV_LDOWN; break;
    case WM_LBUTTONUP:   e.type = EV_LUP; break;
    case WM_RBUTTONDOWN: e.type = EV_RDOWN; break;
    case WM_RBUTTONUP:   e.type = EV_RUP; break;
    case WM_MBUTTONDOWN: e.type = EV_MDOWN; break;
    case WM_MBUTTONUP:   e.type = EV_MUP; break;
    case WM_MOUSEWHEEL:
        e.type = EV_WHEEL;
        e.wheel = int(GET_WHEEL_DELTA_WPARAM(ms->mouseData));
        break;
    default:
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }
    PushEvent(e);
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK KeyProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
    auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    int vk = int(ks->vkCode);
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

    if (IsControlKey(vk)) {
        if (down) {
            if (!g_keyDown[vk & 0xFF]) { g_keyDown[vk & 0xFF] = true; PushKey(vk); }
            if (vk == VK_SCROLL || vk == VK_PAUSE) return 1;
        } else {
            g_keyDown[vk & 0xFF] = false;
            if (vk == VK_SCROLL || vk == VK_PAUSE) return 1;
        }
        return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
    }
    if (g_recording && (down || up) && (g_recordInjected || !(ks->flags & LLKHF_INJECTED))) {
        Ev e{};
        e.type = down ? EV_KEYDOWN : EV_KEYUP;
        e.vk = ks->vkCode;
        e.scan = ks->scanCode;
        e.extended = (ks->flags & LLKHF_EXTENDED) ? 1 : 0;
        e.t = GetTickCount() - g_recStart;
        PushEvent(e);
    }
    return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
}

static bool SaveLog(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    uint32_t hdr[5] = { kRecMagic, kRecVersion,
                        uint32_t(GetSystemMetrics(SM_CXSCREEN)),
                        uint32_t(GetSystemMetrics(SM_CYSCREEN)),
                        uint32_t(g_log.size()) };
    bool ok = (fwrite(hdr, sizeof(uint32_t), 5, f) == 5);
    if (ok && !g_log.empty())
        ok = (fwrite(g_log.data(), sizeof(Ev), g_log.size(), f) == g_log.size());
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool LoadLog(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        Say(L"无法打开录制文件: %ls", path.c_str());
        return false;
    }
    uint32_t magic = 0, a = 0;
    if (fread(&magic, sizeof(uint32_t), 1, f) != 1 || magic != kRecMagic) {
        fclose(f);
        Say(L"不是有效的录制文件");
        return false;
    }
    if (fread(&a, sizeof(uint32_t), 1, f) != 1) { fclose(f); return false; }

    uint32_t count = 0, sw = 0, sh = 0;
    if (a == 1) {
        // 旧格式：magic, w, h, count
        uint32_t cnt = 0;
        if (fread(&sh, sizeof(uint32_t), 1, f) != 1 || fread(&cnt, sizeof(uint32_t), 1, f) != 1) {
            fclose(f);
            return false;
        }
        sw = a;
        count = cnt;
    } else {
        uint32_t ver = a;
        if (fread(&sw, sizeof(uint32_t), 1, f) != 1 ||
            fread(&sh, sizeof(uint32_t), 1, f) != 1 ||
            fread(&count, sizeof(uint32_t), 1, f) != 1) {
            fclose(f);
            return false;
        }
        (void)ver;
    }
    if (count > 10000000) { fclose(f); Say(L"事件数异常"); return false; }

    g_log.resize(count);
    if (count) {
        size_t got = fread(g_log.data(), sizeof(Ev), count, f);
        if (got != count) {
            fclose(f);
            g_log.clear();
            Say(L"录制文件被截断 (声明 %u，实际 %zu)", count, got);
            return false;
        }
    }
    fclose(f);

    int cw = GetSystemMetrics(SM_CXSCREEN), ch = GetSystemMetrics(SM_CYSCREEN);
    if (sw > 0 && sh > 0 && (int(sw) != cw || int(sh) != ch)) {
        Say(L"注意: 录制时分辨率 %ux%u，当前 %dx%d，坐标可能不准", sw, sh, cw, ch);
    }
    return true;
}

static void SendKeyEv(const Ev& e, bool up) {
    if (g_opt.dryRun) return;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = WORD(e.vk);
    in.ki.wScan = WORD(e.scan ? e.scan : MapVirtualKeyW(e.vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (e.extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof(INPUT));
}

static void RunEv(const Ev& e) {
    const DWORD move = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    switch (e.type) {
    case EV_MOVE:   SendMouseAbs(move, e.x, e.y); break;
    case EV_LDOWN:  SendMouseAbs(move | MOUSEEVENTF_LEFTDOWN, e.x, e.y); break;
    case EV_LUP:    SendMouseAbs(move | MOUSEEVENTF_LEFTUP, e.x, e.y); break;
    case EV_RDOWN:  SendMouseAbs(move | MOUSEEVENTF_RIGHTDOWN, e.x, e.y); break;
    case EV_RUP:    SendMouseAbs(move | MOUSEEVENTF_RIGHTUP, e.x, e.y); break;
    case EV_MDOWN:  SendMouseAbs(move | MOUSEEVENTF_MIDDLEDOWN, e.x, e.y); break;
    case EV_MUP:    SendMouseAbs(move | MOUSEEVENTF_MIDDLEUP, e.x, e.y); break;
    case EV_WHEEL:  SendMouseAbs(move | MOUSEEVENTF_WHEEL, e.x, e.y, e.wheel); break;
    case EV_KEYDOWN: SendKeyEv(e, false); break;
    case EV_KEYUP:   SendKeyEv(e, true); break;
    default: break;
    }
}

int DoRecord(const std::wstring& path, bool autoStart) {
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, GetModuleHandleW(nullptr), 0);
    if (!g_mouseHook || !g_keyHook) {
        Say(L"安装钩子失败 (错误码 %lu)", GetLastError());
        return 3;
    }
    if (autoStart) {
        g_log.clear();
        g_recStart = GetTickCount();
        g_recording = true;
        Say(L"[REC] 已开始，按 ScrollLock 停止，Esc 保存退出");
    } else {
        Say(L"录制：按 ScrollLock 开始/停止，Esc 保存并退出");
    }

    MSG msg;
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        int vk = 0;
        while (PopKey(vk)) {
            if (vk == VK_ESCAPE) { running = false; break; }
            if (vk == VK_SCROLL) {
                if (g_recording) {
                    g_recording = false;
                    Say(L"[REC] 停止，共 %zu 个事件", g_log.size());
                } else {
                    g_log.clear();
                    g_recStart = GetTickCount();
                    g_recording = true;
                    Say(L"[REC] 开始");
                }
            }
        }
        Sleep(10);
    }
    g_recording = false;
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    if (g_keyHook) { UnhookWindowsHookEx(g_keyHook); g_keyHook = nullptr; }

    if (!SaveLog(path)) { Say(L"保存失败: %ls", path.c_str()); return 2; }
    Say(L"已保存 %zu 个事件到 %ls", g_log.size(), path.c_str());
    if (g_log.empty()) {
        Say(L"警告: 录制结果是 0 个事件，文件是空的（是否忘记按 ScrollLock？）");
        return 3;
    }
    return 0;
}

static bool PlayOnce() {
    if (g_log.empty()) return true;
    DWORD start = GetTickCount();
    for (const auto& e : g_log) {
        if (ShouldAbort()) return false;
        for (;;) {
            DWORD el = GetTickCount() - start;
            if (el >= e.t) break;
            if (ShouldAbort()) return false;
            DWORD rem = e.t - el;
            Sleep(rem > 5 ? 5 : rem);
        }
        RunEv(e);
    }
    return true;
}

int DoPlay(const std::wstring& path, int loop) {
    if (!LoadLog(path)) return 2;
    if (g_log.empty()) { Say(L"文件里没有事件"); return 3; }
    if (g_stopEvent) ResetEvent(g_stopEvent);
    g_progStart = GetTickCount();
    Say(L"回放 %zu 个事件 (循环 %d 次)", g_log.size(), loop);
    int done = 0;
    for (int i = 0; loop <= 0 || i < loop; ++i) {
        if (ShouldAbort()) break;
        if (!PlayOnce()) break;
        ++done;
        SleepInterruptible(150);
    }
    Say(L"回放结束 (%d 遍)", done);
    return ShouldAbort() ? 4 : 0;
}

int DoStop() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!ev) { Say(L"没有正在运行的动作"); return 0; }
    SetEvent(ev);
    CloseHandle(ev);
    Say(L"已发送停止信号");
    return 0;
}

int DoDump(const std::wstring& path) {
    if (!LoadLog(path)) return 2;
    Say(L"共 %zu 个事件", g_log.size());
    for (const auto& e : g_log) {
        const wchar_t* name = L"?";
        switch (e.type) {
        case EV_MOVE: name = L"MOVE"; break;
        case EV_LDOWN: name = L"LDOWN"; break;
        case EV_LUP: name = L"LUP"; break;
        case EV_RDOWN: name = L"RDOWN"; break;
        case EV_RUP: name = L"RUP"; break;
        case EV_MDOWN: name = L"MDOWN"; break;
        case EV_MUP: name = L"MUP"; break;
        case EV_WHEEL: name = L"WHEEL"; break;
        case EV_KEYDOWN: name = L"KEYDN"; break;
        case EV_KEYUP: name = L"KEYUP"; break;
        default: break;
        }
        Say(L"  t=%-6u %-5ls vk=0x%02X (x=%d,y=%d)", e.t, name, e.vk, e.x, e.y);
    }
    return 0;
}
