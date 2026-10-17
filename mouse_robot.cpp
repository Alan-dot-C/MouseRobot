// ============================================================================
//  mouse_robot.cpp —— 命令行鼠标/键盘机器人
//
//  设计思路: 用全局低层钩子录制鼠标键盘事件 (WH_MOUSE_LL / WH_KEYBOARD_LL),
//           再用 SendInput 回放。在此基础上提供: 命令行驱动 / 录制存档 /
//           文本脚本 / 停止信号 / 自定义鼠标样式。
//
//  编译 (MSVC x64):
//     cl /nologo /utf-8 /EHsc /O2 /MT /Fe:mouse_robot.exe mouse_robot.cpp user32.lib gdi32.lib
//   （需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载）
//
//  用法:
//     mouse_robot record <文件.rec>              录制: ScrollLock 开始/停止, Esc 保存退出
//     mouse_robot play   <文件.rec> [--loop N]   回放 N 遍 (0=无限, 默认 1)
//     mouse_robot script <文件.txt>              执行文本脚本
//     mouse_robot stop                           给正在运行的实例发停止信号
//     mouse_robot testicon                       测试自定义鼠标样式 (2 秒后还原)
//     mouse_robot listen <文件.rec> <秒数>        监听键盘事件
//     mouse_robot dump   <文件.rec>              打印录制内容
//     [--icon <ico路径>] 任何命令可用; 默认找 exe 同目录的 tray.ico
//
//  脚本语法 (每行一条, # 开头为注释):
//     MOVE x y [ms]   鼠标移动到屏幕绝对坐标 (可选的 ms 为平滑时长, 默认 300)
//     CLICKAT x y [ms] 平滑移动到(x,y)并单击 (最稳的点击方式)
//     SLEEP ms        等待毫秒
//     CLICK           左键单击当前位置
//     RCLICK          右键单击
//     DCLICK          左键双击 (间隔取系统双击速度)
//     DOWN / UP       左键按下 / 抬起 (可加键名: DOWN CTRL / UP CTRL)
//     WHEEL n         滚轮 (n 必须为 120 的倍数, 正数向上)
//     KEY <键名>      按一下键 (如 KEY t, KEY ENTER, KEY TAB, KEY F5, KEY LWIN)
//                    也接受十六进制: KEY 0x41
//     TEXT <内容>     打一串字 (支持中文与任意 Unicode 字符)
//     FOCUS <标题片段> 按标题找窗口并设为前台, 并等待它真正成为前台
//     WAITWIN <标题片段> [超时ms]  等窗口出现 (默认 5000ms, 超时返回失败)
//     GETPOS          打印当前鼠标坐标 (机器可读格式)
//     SET <名> <值>   设置脚本变量, 之后用 ${名} 引用
//     MOVEREL dx dy   相对当前位置移动
//
//  全局选项:
//     --icon <ico路径>       自定义鼠标样式 (默认找 exe 同目录的 tray.ico)
//     --no-icon              完全不动系统光标 (最安全的做法)
//     --loop N               回放/脚本循环次数 (0=无限)
//     --json                 结果以 JSON 输出到 stdout
//     --dry-run              只打印将要做的事, 不发送任何输入
//     --max-duration <秒>    总时长上限, 超过自动中止
//     --auto                 录制时启动即开始录, 不用按 ScrollLock
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <clocale>
#include <cwchar>
#include <cwctype>
#include <io.h>
#include <fcntl.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ------------------------------ 全局状态 ------------------------------

static const wchar_t* kStopEventName = L"DSH_MouseRobot_Stop_Event";

// 默认图标不再写死某个绝对路径 (2026-10-17): 先找 exe 同目录的 tray.ico /
// cursor.ico, 都没有就不换光标。想指定就加 --icon <路径>。
static std::wstring ExeDir() {
    wchar_t exe[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring p(exe, n);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
}

static std::wstring DefaultIconPath() {
    std::wstring dir = ExeDir();
    const wchar_t* names[] = { L"tray.ico", L"cursor.ico" };
    for (const wchar_t* nm : names) {
        std::wstring cand = dir + nm;
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;
    }
    return std::wstring();   // 空 = 不换光标
}

// .rec 格式版本 (v1 没有版本字段, v2 起写入并校验)
static const uint32_t kRecMagic  = 0x52544852;   // 'RTHR'
static const uint32_t kRecVersion = 2;

static int  g_screenW = 0;      // 主屏宽度 (逻辑像素)
static int  g_screenH = 0;      // 主屏高度 (逻辑像素)
static int  g_virtX  = 0;       // 虚拟桌面左上角 (多显示器时可能为负)
static int  g_virtY  = 0;
static int  g_virtW  = 0;
static int  g_virtH  = 0;
static HANDLE g_stopEvent = NULL;

// ---- 命令行选项 (全局, 便于脚本执行时读取) ----
struct Options {
    bool json = false;          // 结果以 JSON 输出
    bool dryRun = false;        // 不发送任何真实输入
    bool noIcon = false;        // 不动系统光标
    bool autoStart = false;     // 录制: 立即开始
    int  maxDurationSec = 0;    // 0 = 无限制
};
static Options g_opt;
static DWORD g_progStart = 0;   // 本进程开始执行脚本/回放的时间

// 退出码语义 (供外部程序判断)
enum {
    RC_OK        = 0,
    RC_ARG       = 1,   // 参数错误
    RC_FILE      = 2,   // 文件读写错误
    RC_EXEC      = 3,   // 执行失败 (某一行报错)
    RC_TIMEOUT   = 4,   // 超时/被停止
};

// ------------------------------ 事件结构 ------------------------------

enum EvType : uint32_t {
    EV_MOVE = 1, EV_LDOWN, EV_LUP, EV_RDOWN, EV_RUP,
    EV_MDOWN, EV_MUP, EV_WHEEL, EV_KEYDOWN, EV_KEYUP,
};

struct Ev {
    uint32_t type;
    int32_t  x, y;          // 屏幕坐标
    int32_t  wheel;         // 滚轮量 (120 的倍数)
    uint32_t vk, scan;
    uint32_t extended;
    uint32_t t;             // 距录制开始的毫秒
};

static std::vector<Ev> g_log;
static std::atomic<bool> g_recording{ false };
static DWORD  g_recStart = 0;
static HHOOK  g_mouseHook = NULL;
static HHOOK  g_keyHook = NULL;
static std::deque<int> g_keyQueue;
static std::mutex      g_keyMutex;
static bool g_keyDown[256] = { false };
static bool g_recordInjected = false;

// ------------------------------ 工具函数 ------------------------------

static bool IsControlKey(int vk) { return vk == VK_SCROLL || vk == VK_PAUSE || vk == VK_ESCAPE; }

static void PushEvent(const Ev& e) {
    bool merge = (e.type == EV_MOVE && !g_log.empty() &&
                  g_log.back().type == EV_MOVE && e.t - g_log.back().t < 15);
    if (merge) g_log.back() = e; else g_log.push_back(e);
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

static void Trim(std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    size_t b = s.find_last_not_of(L" \t\r\n");
    if (a == std::wstring::npos) { s.clear(); return; }
    s = s.substr(a, b - a + 1);
}

static bool StopRequested() {
    return g_stopEvent && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0;
}

// 是否已超过 --max-duration 设定的总时长
static bool DurationExceeded() {
    if (g_opt.maxDurationSec <= 0 || g_progStart == 0) return false;
    return (GetTickCount() - g_progStart) > (DWORD)g_opt.maxDurationSec * 1000u;
}

// 统一的中止判断: 手动 stop 或超过总时长都算
static bool ShouldAbort() { return StopRequested() || DurationExceeded(); }

static void SleepInterruptible(DWORD ms) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        if (ShouldAbort()) break;
        DWORD left = ms - (GetTickCount() - start);
        Sleep(left > 20 ? 20 : left);
    }
}

// 把坐标限制在虚拟桌面范围内, 避免越界坐标经 65535 换算后变成负数 (鼠标瞬移到左上角)
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

// JSON 字符串转义 (只处理必要的控制字符与引号反斜杠)
static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'"':  o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n";  break;
        case L'\r': o += L"\\r";  break;
        case L'\t': o += L"\\t";  break;
        default:
            if (c < 0x20) {
                wchar_t b[8];
                swprintf_s(b, L"\\u%04x", (unsigned)c);
                o += b;
            } else o += c;
        }
    }
    return o;
}

// 统一的输出: 普通模式走 wprintf (给人看), --json 模式收集到缓冲最后一次性输出
static std::wstring g_jsonMsgs;
static void Say(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[2048];
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_opt.json) {
        if (!g_jsonMsgs.empty()) g_jsonMsgs += L"\\n";
        g_jsonMsgs += JsonEscape(buf);
    } else {
        wprintf(L"%ls\n", buf);
    }
}

// ------------------------------ 录制存档 ------------------------------
//
//  文件头 (全部 32 位小端):
//     magic    'RTHR' (0x52544852)
//     version  1 = 旧格式(无版本号), 2 = 当前
//     screenW / screenH
//     count
//  之后紧跟 count 个 Ev。
//
//  读取时必须核对 fread 返回值: 旧版本不检查, 文件被截断时会把后面
//  的元素填成 0, 表现为"回放少了事件但看起来正常"。

static bool SaveLog(const wchar_t* path) {
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    uint32_t hdr[6];
    hdr[0] = kRecMagic;
    hdr[1] = kRecVersion;
    hdr[2] = (uint32_t)g_screenW;
    hdr[3] = (uint32_t)g_screenH;
    hdr[4] = (uint32_t)g_log.size();
    bool ok = (fwrite(hdr, sizeof(uint32_t), 5, f) == 5);
    if (ok && !g_log.empty())
        ok = (fwrite(g_log.data(), sizeof(Ev), g_log.size(), f) == g_log.size());
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool LoadLog(const wchar_t* path) {
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
        Say(L"读取失败: 无法打开 %ls", path);
        return false;
    }
    uint32_t magic = 0, a = 0, b = 0, c = 0, d = 0;
    size_t got = fread(&magic, sizeof(uint32_t), 1, f);
    if (got != 1 || magic != kRecMagic) {
        fclose(f);
        Say(L"读取失败: 不是有效的 .rec 文件 (魔数不匹配)");
        return false;
    }

    // 第二个字段: 新格式是版本号; 旧格式这里是屏幕宽度。
    // 屏幕宽度不可能等于 2, 所以能安全区分。
    if (fread(&a, sizeof(uint32_t), 1, f) != 1) { fclose(f); Say(L"读取失败: 文件头不完整"); return false; }
    uint32_t version, sw, sh, cnt;
    if (a == kRecVersion) {
        version = a;
        if (fread(&sw, sizeof(uint32_t), 1, f) != 1 ||
            fread(&sh, sizeof(uint32_t), 1, f) != 1 ||
            fread(&cnt, sizeof(uint32_t), 1, f) != 1) {
            fclose(f); Say(L"读取失败: 文件头不完整"); return false;
        }
    } else {
        // 旧格式 (v1): magic, w, h, count, 紧跟事件
        version = 1;
        sw = a;
        if (fread(&sh, sizeof(uint32_t), 1, f) != 1 ||
            fread(&cnt, sizeof(uint32_t), 1, f) != 1) {
            fclose(f); Say(L"读取失败: 文件头不完整"); return false;
        }
    }
    (void)b; (void)c; (void)d;

    if (cnt > 10000000) { fclose(f); Say(L"读取失败: 事件数异常 (%u)", cnt); return false; }
    if (cnt > 0) {
        g_log.resize(cnt);
        size_t n = fread(g_log.data(), sizeof(Ev), cnt, f);
        if (n != cnt) {
            fclose(f);
            g_log.clear();
            Say(L"读取失败: 文件被截断 (声明 %u 个事件, 实际只有 %zu 个)", cnt, n);
            return false;
        }
    } else {
        g_log.clear();
    }
    fclose(f);

    // 分辨率变了要提醒: 坐标是绝对坐标, 换了分辨率就不准了
    if (version >= 2 && sw > 0 && sh > 0 && ((int)sw != g_screenW || (int)sh != g_screenH)) {
        Say(L"注意: 录制时分辨率 %ux%u, 当前 %dx%d, 坐标可能不准确",
            sw, sh, g_screenW, g_screenH);
    }
    return true;
}

// ------------------------------ 钩子 (录制) ------------------------------

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0 || !g_recording) return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
    if (ms->flags & LLMHF_INJECTED) return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    Ev e = {};
    e.x = ms->pt.x; e.y = ms->pt.y; e.t = GetTickCount() - g_recStart;
    switch (wParam) {
    case WM_MOUSEMOVE:   e.type = EV_MOVE; break;
    case WM_LBUTTONDOWN: e.type = EV_LDOWN; break;
    case WM_LBUTTONUP:   e.type = EV_LUP; break;
    case WM_RBUTTONDOWN: e.type = EV_RDOWN; break;
    case WM_RBUTTONUP:   e.type = EV_RUP; break;
    case WM_MBUTTONDOWN: e.type = EV_MDOWN; break;
    case WM_MBUTTONUP:   e.type = EV_MUP; break;
    case WM_MOUSEWHEEL:  e.type = EV_WHEEL; e.wheel = (int)GET_WHEEL_DELTA_WPARAM(ms->mouseData); break;
    default: return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }
    PushEvent(e);
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK KeyProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
    KBDLLHOOKSTRUCT* ks = (KBDLLHOOKSTRUCT*)lParam;
    int  vk = (int)ks->vkCode;
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (IsControlKey(vk)) {
        if (down) {
            if (!g_keyDown[vk & 0xFF]) { g_keyDown[vk & 0xFF] = true; PushKey(vk); }
            if (vk == VK_SCROLL || vk == VK_PAUSE) return 1;   // 吞掉控制键
        } else {
            g_keyDown[vk & 0xFF] = false;
            if (vk == VK_SCROLL || vk == VK_PAUSE) return 1;
        }
        return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
    }
    if (g_recording && (down || up) && (g_recordInjected || !(ks->flags & LLKHF_INJECTED))) {
        Ev e = {};
        e.type = down ? EV_KEYDOWN : EV_KEYUP;
        e.vk = ks->vkCode;
        e.scan = ks->scanCode;
        e.extended = (ks->flags & LLKHF_EXTENDED) ? 1 : 0;
        e.t = GetTickCount() - g_recStart;
        PushEvent(e);
    }
    return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
}

// ------------------------------ 发送 (回放) ------------------------------

static void SendMouseAbs(DWORD flags, int x, int y, int wheel = 0) {
    if (g_opt.dryRun) return;
    // 虚拟桌面坐标 -> 0..65535 归一化。必须先裁剪, 否则越界坐标会换算成
    // 负值, 鼠标会瞬移到屏幕左上角, 而脚本表面上看不出任何错误。
    int cx = ClampX(x), cy = ClampY(y);
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.dx = (LONG)((double)(cx - g_virtX) * 65535.0 / (g_virtW > 1 ? g_virtW - 1 : 1));
    in.mi.dy = (LONG)((double)(cy - g_virtY) * 65535.0 / (g_virtH > 1 ? g_virtH - 1 : 1));
    in.mi.mouseData = (DWORD)wheel;
    if (SendInput(1, &in, sizeof(INPUT)) != 1)
        Say(L"警告: SendInput 被系统丢弃 (错误码 %lu, 目标窗口可能权限更高)", GetLastError());
}

static void SendKeyEv(const Ev& e, bool up) {
    if (g_opt.dryRun) return;
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)e.vk;
    in.ki.wScan = (WORD)(e.scan ? e.scan : MapVirtualKeyW((UINT)e.vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (e.extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (SendInput(1, &in, sizeof(INPUT)) != 1)
        Say(L"警告: 按键输入被系统丢弃 (错误码 %lu)", GetLastError());
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
    }
}

static void PressKeyEx(int vk, bool ext) {
    if (g_opt.dryRun) return;
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk;
    if (ext) in[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    in[1] = in[0]; in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    if (SendInput(2, in, sizeof(INPUT)) != 2)
        Say(L"警告: 按键输入被系统丢弃 (错误码 %lu)", GetLastError());
}

// 按住 / 抬起单个键或鼠标键 (用于 Ctrl 拖拽、Shift 多选之类的组合操作)
static void HoldKey(int vk, bool down) {
    if (g_opt.dryRun) return;
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (SendInput(1, &in, sizeof(INPUT)) != 1)
        Say(L"警告: 按键输入被系统丢弃 (错误码 %lu)", GetLastError());
}

// 用一个 Unicode 码元打字 (中文/符号走这里, 不受键盘布局限制)
static void TypeUnicode(wchar_t ch) {
    if (g_opt.dryRun) return;
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = 0;
    in[0].ki.wScan = ch;
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    if (SendInput(2, in, sizeof(INPUT)) != 2)
        Say(L"警告: 字符输入被系统丢弃 (错误码 %lu)", GetLastError());
}

// 旧实现: 只能打当前键盘布局里能直接按出的字符, 中文一律静默丢弃。
// 保留给"需要走真实按键"的极少数场景, 正常 TEXT 已改用 TypeUnicode。
static bool TypeCharLegacy(wchar_t ch) {
    SHORT s = VkKeyScanW(ch);
    if (s == -1) return false;
    int vk = s & 0xFF;
    // 0x100=Shift, 0x200=Ctrl, 0x400=Alt。旧代码只判断了 Shift。
    int mods = (s >> 8) & 0xFF;
    INPUT in[6] = {}; int n = 0;
    auto addMod = [&](int vkMod) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = (WORD)vkMod; n++; };
    if (mods & 0x02) addMod(VK_CONTROL);
    if (mods & 0x04) addMod(VK_MENU);
    if (mods & 0x01) addMod(VK_SHIFT);
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = (WORD)vk; n++;
    in[n] = in[n - 1]; in[n].ki.dwFlags |= KEYEVENTF_KEYUP; n++;
    if (mods & 0x01) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (mods & 0x04) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_MENU; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (mods & 0x02) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (g_opt.dryRun) return true;
    return SendInput((UINT)n, in, sizeof(INPUT)) == (UINT)n;
}

// 平滑移动, 带缓出 (ease-out) 让轨迹更像人手; ms<=0 时直接跳过去
static void MoveSmooth(int x, int y, DWORD ms) {
    int tx = ClampX(x), ty = ClampY(y);
    if (g_opt.dryRun) { Say(L"[dry-run] MOVE %d %d (%lums)", tx, ty, ms); return; }
    POINT cur; GetCursorPos(&cur);
    if (ms == 0 || (cur.x == tx && cur.y == ty)) {
        SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, tx, ty);
        return;
    }
    int steps = (int)(ms / 20);
    if (steps < 2) steps = 2;
    if (steps > 120) steps = 120;          // 上限, 避免长距离移动拖太久
    DWORD per = ms / (DWORD)steps;
    for (int i = 1; i <= steps; i++) {
        double t = (double)i / steps;
        double e = 1.0 - (1.0 - t) * (1.0 - t);   // ease-out 二次曲线
        int nx = cur.x + (int)((tx - cur.x) * e);
        int ny = cur.y + (int)((ty - cur.y) * e);
        SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny);
        if (ShouldAbort()) return;
        Sleep(per);
    }
    Sleep(80);
}

static void ClickCurrentEx(bool right, DWORD holdMs = 60) {
    // 按住停留后再抬起: 很多控件只认"有停留的完整点击"
    if (g_opt.dryRun) { Say(L"[dry-run] %lsCLICK hold=%lums", right ? L"R" : L"", holdMs); return; }
    POINT p; GetCursorPos(&p);
    DWORD down = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    DWORD up   = right ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;
    SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | down, p.x, p.y);
    SleepInterruptible(holdMs);
    SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | up, p.x, p.y);
    Sleep(90);
}

static void ClickCurrent(bool right) { ClickCurrentEx(right, 60); }

static void LeftAt(bool down) {
    if (g_opt.dryRun) { Say(L"[dry-run] LEFT %ls", down ? L"DOWN" : L"UP"); return; }
    POINT p; GetCursorPos(&p);
    SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP), p.x, p.y);
}

// ------------------------------ 自定义鼠标样式 ------------------------------

// 运行期间把系统光标换成指定图标(居中热区), 结束时自动还原。
struct CursorSwapper {
    HCURSOR hNew = NULL;
    HCURSOR hOld = NULL;
    bool ok = false;

    bool Apply(const wchar_t* path) {
        // --no-icon: 完全不碰系统光标, 杜绝"进程被强杀后光标改不回来"的风险
        if (g_opt.noIcon) return false;
        hNew = LoadCursorFromFileW(path);
        if (!hNew) return false;
        ICONINFO ii = {};
        if (GetIconInfo(hNew, &ii)) {
            BITMAP bm = {};
            int w = 0, h = 0;
            if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) { w = bm.bmWidth; h = bm.bmHeight; }
            else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm)) { w = bm.bmWidth; h = bm.bmHeight / 2; }
            if (w > 0 && h > 0) {
                int target = 48;
                if (w < target || h < target) {
                    // 小图标放大到 48x48, 光标才显眼 (托盘图标通常只有16/32像素)
                    HDC screen = GetDC(NULL);
                    HDC dst = CreateCompatibleDC(screen), src = CreateCompatibleDC(screen);
                    BITMAPINFO bi = {};
                    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bi.bmiHeader.biWidth = target; bi.bmiHeader.biHeight = -target;
                    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                    void* bits = NULL;
                    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
                    HBITMAP mask = CreateBitmap(target, target, 1, 1, NULL);
                    HGDIOBJ od = SelectObject(dst, color), os = SelectObject(src, ii.hbmColor ? ii.hbmColor : ii.hbmMask);
                    SetStretchBltMode(dst, HALFTONE);
                    StretchBlt(dst, 0, 0, target, target, src, 0, 0, w, h, SRCCOPY);
                    SelectObject(dst, od); SelectObject(src, os);
                    HDC mdst = CreateCompatibleDC(screen), msrc = CreateCompatibleDC(screen);
                    HGDIOBJ omd = SelectObject(mdst, mask), oms = SelectObject(msrc, ii.hbmMask);
                    StretchBlt(mdst, 0, 0, target, target, msrc, 0, 0, w, h, SRCCOPY);
                    SelectObject(mdst, omd); SelectObject(msrc, oms);
                    DeleteDC(mdst); DeleteDC(msrc); DeleteDC(src); DeleteDC(dst);
                    ReleaseDC(NULL, screen);
                    ICONINFO ni = {};
                    ni.fIcon = FALSE;
                    ni.xHotspot = target / 2; ni.yHotspot = target / 2;
                    ni.hbmColor = color; ni.hbmMask = mask;
                    HCURSOR made = (HCURSOR)CreateIconIndirect(&ni);
                    DeleteObject(color); DeleteObject(mask);
                    if (made) { DestroyCursor(hNew); hNew = made; }
                } else {
                    ii.xHotspot = w / 2;
                    ii.yHotspot = h / 2;
                    HCURSOR made = (HCURSOR)CreateIconIndirect(&ii);
                    if (made) { DestroyCursor(hNew); hNew = made; }
                }
            }
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        }
        hOld = CopyCursor(LoadCursorW(NULL, IDC_ARROW));
        if (!hOld) { DestroyCursor(hNew); hNew = NULL; return false; }
        if (!SetSystemCursor(hNew, 32512 /* OCR_NORMAL */)) { DestroyCursor(hNew); hNew = NULL; return false; }
        ok = true;
        return true;
    }

    ~CursorSwapper() { Restore(); }

    void Restore() {
        if (ok) {
            if (hOld) SetSystemCursor(hOld, 32512 /* OCR_NORMAL */);
            SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);   // 强制从注册表重新加载光标方案
        }
        ok = false;
    }
};

// ------------------------------ 回放 ------------------------------

static bool PlayOnce() {
    if (g_log.empty()) return true;
    DWORD start = GetTickCount();
    for (const Ev& e : g_log) {
        if (ShouldAbort()) return false;
        for (;;) {
            DWORD el = GetTickCount() - start;
            if (el >= e.t) break;
            DWORD rem = e.t - el;
            if (ShouldAbort()) return false;
            Sleep(rem > 5 ? 5 : rem);
        }
        if (g_opt.dryRun) { Say(L"[dry-run] %u @ %d,%d", e.type, e.x, e.y); continue; }
        RunEv(e);
    }
    return true;
}

// ------------------------------ 脚本 ------------------------------

struct KeyName { const wchar_t* name; int vk; bool ext; };
static const KeyName kKeys[] = {
    { L"ENTER", VK_RETURN, false }, { L"RETURN", VK_RETURN, false }, { L"TAB", VK_TAB, false },
    { L"ESC", VK_ESCAPE, false }, { L"ESCAPE", VK_ESCAPE, false }, { L"SPACE", VK_SPACE, false },
    { L"BACKSPACE", VK_BACK, false }, { L"BACK", VK_BACK, false },
    { L"DELETE", VK_DELETE, false }, { L"DEL", VK_DELETE, false }, { L"INSERT", VK_INSERT, false }, { L"INS", VK_INSERT, false },
    { L"HOME", VK_HOME, false }, { L"END", VK_END, false },
    { L"PGUP", VK_PRIOR, false }, { L"PGDN", VK_NEXT, false },
    { L"UP", VK_UP, true }, { L"DOWN", VK_DOWN, true }, { L"LEFT", VK_LEFT, true }, { L"RIGHT", VK_RIGHT, true },
    { L"LWIN", VK_LWIN, true }, { L"RWIN", VK_RWIN, true }, { L"WIN", VK_LWIN, true }, { L"APPS", VK_APPS, true },
    { L"CTRL", VK_CONTROL, false }, { L"CONTROL", VK_CONTROL, false },
    { L"SHIFT", VK_SHIFT, false }, { L"ALT", VK_MENU, false },
    { L"CAPS", VK_CAPITAL, false }, { L"PRTSC", VK_SNAPSHOT, true },
};

// 解析键名 -> VK。支持: 内置表, F1..F24, 0xNN 十六进制, 单个字符
static bool ResolveKey(const std::wstring& raw, int& vk, bool& ext) {
    std::wstring name = raw;
    Trim(name);
    for (wchar_t& c : name) c = towupper(c);
    if (name.empty()) return false;

    // 十六进制: 0x41
    if (name.size() >= 3 && name[0] == L'0' && (name[1] == L'X') ) {
        wchar_t* endp = NULL;
        long v = wcstol(name.c_str() + 2, &endp, 16);
        if (endp && *endp == L'\0' && v > 0 && v < 256) { vk = (int)v; ext = false; return true; }
    }
    // F1..F24
    if (name.size() >= 2 && name.size() <= 3 && name[0] == L'F' && iswdigit(name[1])) {
        int n = _wtoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) { vk = VK_F1 + n - 1; ext = false; return true; }
    }
    for (const KeyName& k : kKeys) {
        if (_wcsicmp(k.name, name.c_str()) == 0) { vk = k.vk; ext = k.ext; return true; }
    }
    if (name.size() == 1) {
        SHORT sc = VkKeyScanW(name[0]);
        if (sc != -1) { vk = sc & 0xFF; ext = false; return true; }
        // 不是当前布局里的字符: 当作 Unicode 直接发
        vk = 0; ext = false;
        return false;
    }
    return false;
}

// 组合键: CTRL+L / WIN+H / CTRL+SHIFT+S
static bool PressCombo(const std::wstring& name) {
    int mods = 0;
    std::vector<int> kvs;
    std::vector<bool> exts;
    size_t s = 0;
    while (s <= name.size()) {
        size_t p = name.find(L'+', s);
        if (p == std::wstring::npos) p = name.size();
        std::wstring t = name.substr(s, p - s);
        Trim(t);
        for (wchar_t& c : t) c = towupper(c);
        if (t == L"CTRL" || t == L"CONTROL") mods |= 1;
        else if (t == L"WIN" || t == L"LWIN") mods |= 2;
        else if (t == L"ALT") mods |= 4;
        else if (t == L"SHIFT") mods |= 8;
        else if (!t.empty()) {
            int vk = 0; bool ext = false;
            if (!ResolveKey(t, vk, ext)) { Say(L"[脚本] 组合键里有无法识别的键: %ls", t.c_str()); return false; }
            kvs.push_back(vk); exts.push_back(ext);
        }
        s = p + 1;
    }
    if (kvs.empty()) return false;
    std::vector<INPUT> in;
    auto pushKey = [&in](WORD vk, DWORD flags, bool ext) {
        INPUT x = {};
        x.type = INPUT_KEYBOARD;
        x.ki.wVk = vk;
        x.ki.dwFlags = flags;
        if (ext) x.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        in.push_back(x);
    };
    if (!g_opt.dryRun) {
        if (mods & 1) pushKey(VK_CONTROL, 0, false);
        if (mods & 2) pushKey(VK_LWIN, 0, false);
        if (mods & 4) pushKey(VK_MENU, 0, false);
        if (mods & 8) pushKey(VK_SHIFT, 0, false);
        for (size_t i = 0; i < kvs.size(); i++) {
            pushKey((WORD)kvs[i], 0, exts[i]);
            pushKey((WORD)kvs[i], KEYEVENTF_KEYUP, exts[i]);
        }
        if (mods & 8) pushKey(VK_SHIFT, KEYEVENTF_KEYUP, false);
        if (mods & 4) pushKey(VK_MENU, KEYEVENTF_KEYUP, false);
        if (mods & 2) pushKey(VK_LWIN, KEYEVENTF_KEYUP, false);
        if (mods & 1) pushKey(VK_CONTROL, KEYEVENTF_KEYUP, false);
        if (!in.empty() && SendInput((UINT)in.size(), in.data(), sizeof(INPUT)) != in.size())
            Say(L"警告: 组合键输入被系统丢弃 (错误码 %lu)", GetLastError());
    }
    return true;
}

// 窗口标题匹配优先级:
//   1) 可见+非最小化+标题完全等于目标
//   2) 可见+非最小化+标题包含目标
//   3) 可见+标题包含目标
// 旧实现只做裸的子串匹配且不过滤可见性, 有误切到后台窗口的风险。
struct FindCtx { std::wstring needle; HWND best; int bestScore; };

static BOOL CALLBACK FindByTitleCB(HWND h, LPARAM lp) {
    FindCtx* ctx = (FindCtx*)lp;
    wchar_t buf[512] = {};
    GetWindowTextW(h, buf, 512);
    if (!buf[0]) return TRUE;
    bool vis = IsWindowVisible(h) != 0;
    bool min = IsIconic(h) != 0;
    if (!vis) return TRUE;
    if (!wcsstr(buf, ctx->needle.c_str())) return TRUE;
    int score = 0;
    if (!min) score += 2;
    if (_wcsicmp(buf, ctx->needle.c_str()) == 0) score += 4;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) score -= 1;
    if (score > ctx->bestScore) { ctx->bestScore = score; ctx->best = h; }
    return TRUE;
}

static HWND FindWindowByTitle(const std::wstring& needle) {
    FindCtx ctx; ctx.needle = needle; ctx.best = NULL; ctx.bestScore = -1000;
    EnumWindows(FindByTitleCB, (LPARAM)&ctx);
    return ctx.best;
}

// 等目标窗口成为前台, 最多 timeoutMs。旧实现固定 Sleep(500), 慢机器上会
// 把输入打到动画中途的旧位置上。
static bool WaitForeground(HWND target, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (GetForegroundWindow() == target) return true;
        if (ShouldAbort()) return false;
        Sleep(20);
    }
    return GetForegroundWindow() == target;
}

// SET / 变量: ${NAME}
static std::vector<std::pair<std::wstring, std::wstring>> g_vars;

static std::wstring GetVar(const std::wstring& name, bool& found) {
    for (auto& kv : g_vars) if (kv.first == name) { found = true; return kv.second; }
    found = false;
    return L"";
}

static std::wstring ExpandVars(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'$' && i + 1 < in.size() && in[i + 1] == L'{') {
            size_t close = in.find(L'}', i + 2);
            if (close != std::wstring::npos) {
                std::wstring nm = in.substr(i + 2, close - i - 2);
                bool found = false;
                std::wstring v = GetVar(nm, found);
                if (!found) { Say(L"警告: 未定义的变量 ${%ls}, 按空串处理", nm.c_str()); }
                out += v;
                i = close;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

// 把 "x y [ms]" 解析成整数, 返回成功个数
static int ParseInts(const std::wstring& s, int* out, int maxN) {
    int n = 0;
    const wchar_t* p = s.c_str();
    while (n < maxN) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        wchar_t* endp = NULL;
        long v = wcstol(p, &endp, 10);
        if (endp == p) break;
        out[n++] = (int)v;
        p = endp;
    }
    return n;
}

static bool ReadTextFile(const std::wstring& path, std::wstring& out) {
    FILE* f = NULL;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return true; }
    std::vector<char> buf((size_t)sz + 1);
    size_t got = fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) return false;
    buf[sz] = 0;
    size_t off = 0;
    if (sz >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) off = 3;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)(sz - off), NULL, 0);
    if (wlen <= 0) { out.clear(); return true; }
    out.resize((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, buf.data() + off, (int)(sz - off), &out[0], wlen);
    // 去掉 BOM 产生的零宽字符
    if (!out.empty() && out[0] == 0xFEFF) out.erase(0, 1);
    return true;
}

// 执行一行脚本; 返回 false 表示这一行失败 (脚本应随之中止)
static bool RunScriptLine(const std::wstring& rawLn, int lineNo) {
    std::wstring ln = ExpandVars(rawLn);
    size_t sp = ln.find_first_of(L" \t");
    std::wstring cmd = ln.substr(0, sp);
    std::wstring rest = (sp == std::wstring::npos) ? L"" : ln.substr(sp + 1);
    Trim(rest);
    for (wchar_t& c : cmd) c = towupper(c);

    if (cmd == L"MOVE") {
        int a[3] = {0, 0, 300};
        int n = ParseInts(rest, a, 3);
        if (n >= 2) { MoveSmooth(a[0], a[1], n >= 3 ? (DWORD)(a[2] < 0 ? 0 : a[2]) : 300); return true; }
        Say(L"[第 %d 行] MOVE 参数错误: %ls", lineNo, rest.c_str());
        return false;
    }
    if (cmd == L"CLICKAT") {
        int a[3] = {0, 0, 350};
        int n = ParseInts(rest, a, 3);
        if (n >= 2) {
            MoveSmooth(a[0], a[1], n >= 3 ? (DWORD)(a[2] < 0 ? 0 : a[2]) : 350);
            SleepInterruptible(150);
            ClickCurrentEx(false, 70);
            return true;
        }
        Say(L"[第 %d 行] CLICKAT 参数错误: %ls", lineNo, rest.c_str());
        return false;
    }
    if (cmd == L"MOVEREL") {
        int a[2] = {0, 0};
        if (ParseInts(rest, a, 2) == 2) {
            POINT p; GetCursorPos(&p);
            MoveSmooth(p.x + a[0], p.y + a[1], 300);
            return true;
        }
        Say(L"[第 %d 行] MOVEREL 参数错误: %ls", lineNo, rest.c_str());
        return false;
    }
    if (cmd == L"SLEEP") {
        int ms = _wtoi(rest.c_str());
        if (ms > 0) SleepInterruptible((DWORD)ms);
        return true;
    }
    if (cmd == L"CLICK")  { ClickCurrent(false); return true; }
    if (cmd == L"RCLICK") { ClickCurrent(true);  return true; }
    if (cmd == L"DCLICK") {
        // 间隔取系统的双击速度 (旧版本写死 60ms, 用户把双击速度调慢时会失效)
        DWORD dci = GetDoubleClickTime();
        DWORD gap = dci / 4; if (gap < 30) gap = 30; if (gap > 200) gap = 200;
        ClickCurrentEx(false, 60);
        SleepInterruptible(gap);
        ClickCurrentEx(false, 60);
        return true;
    }
    if (cmd == L"DOWN" || cmd == L"UP") {
        bool down = (cmd == L"DOWN");
        if (rest.empty()) { LeftAt(down); return true; }
        int vk = 0; bool ext = false;
        if (ResolveKey(rest, vk, ext)) { HoldKey(vk, down); return true; }
        Say(L"[第 %d 行] %ls 未知键名: %ls", lineNo, cmd.c_str(), rest.c_str());
        return false;
    }
    if (cmd == L"WHEEL") {
        int n = _wtoi(rest.c_str());
        if (n == 0 || (n % WHEEL_DELTA) != 0) {
            Say(L"[第 %d 行] WHEEL 的值必须是 120 的倍数 (下滚用负数), 收到: %d", lineNo, n);
            return false;
        }
        POINT p; GetCursorPos(&p);
        SendMouseAbs(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_WHEEL, p.x, p.y, n);
        return true;
    }
    if (cmd == L"FOCUS") {
        HWND h = FindWindowByTitle(rest);
        if (!h) { Say(L"[第 %d 行] FOCUS 找不到窗口: %ls", lineNo, rest.c_str()); return false; }
        ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        if (!WaitForeground(h, 3000))
            Say(L"注意: %ls 没有成为前台窗口 (可能被系统限制), 继续执行", rest.c_str());
        return true;
    }
    if (cmd == L"WAITWIN") {
        int a[1] = {5000};
        size_t sp2 = rest.find_first_of(L" \t");
        std::wstring title = rest;
        if (sp2 != std::wstring::npos) { title = rest.substr(0, sp2); ParseInts(rest.substr(sp2), a, 1); }
        DWORD start = GetTickCount();
        for (;;) {
            if (FindWindowByTitle(title)) return true;
            if (GetTickCount() - start > (DWORD)a[0]) {
                Say(L"[第 %d 行] WAITWIN 超时 (%dms): %ls", lineNo, a[0], title.c_str());
                return false;
            }
            if (ShouldAbort()) return false;
            Sleep(100);
        }
    }
    if (cmd == L"GETPOS") {
        if (g_opt.dryRun) { POINT p; GetCursorPos(&p); Say(L"[dry-run] GETPOS %d %d", p.x, p.y); return true; }
        POINT p; GetCursorPos(&p);
        // 机器可读: "GETPOS x y", 方便外部脚本直接解析
        Say(L"GETPOS %d %d", p.x, p.y);
        return true;
    }
    if (cmd == L"SET") {
        size_t sp3 = rest.find_first_of(L" \t");
        if (sp3 == std::wstring::npos) { Say(L"[第 %d 行] SET 需要变量名和值", lineNo); return false; }
        std::wstring nm = rest.substr(0, sp3);
        std::wstring val = rest.substr(sp3 + 1);
        Trim(val);
        for (auto& kv : g_vars) if (kv.first == nm) { kv.second = val; return true; }
        g_vars.push_back(std::make_pair(nm, val));
        return true;
    }
    if (cmd == L"KEY") {
        if (rest.find(L'+') != std::wstring::npos) return PressCombo(rest);
        int vk = 0; bool ext = false;
        if (ResolveKey(rest, vk, ext)) { PressKeyEx(vk, ext); return true; }
        // 单个非 ASCII 字符 (中文等): 直接按 Unicode 发
        if (rest.size() == 1) { TypeUnicode(rest[0]); return true; }
        Say(L"[第 %d 行] 未知键名: %ls", lineNo, rest.c_str());
        return false;
    }
    if (cmd == L"TEXT") {
        // 走 Unicode 通道: 中文、全角符号、任意布局都能打 (旧实现会静默丢弃)
        for (wchar_t c : rest) { TypeUnicode(c); Sleep(15); }
        return true;
    }
    if (cmd == L"ASSERT" || cmd == L"WAITTEXT") {
        // 这两个原语需要截图 + OCR 支持, 本工具不带 OCR 后端, 保留名字给出提示
        Say(L"[第 %d 行] %ls 未实现: 本工具不含 OCR, 无法校验屏幕文字", lineNo, cmd.c_str());
        return false;
    }
    Say(L"[第 %d 行] 未知命令: %ls", lineNo, cmd.c_str());
    return false;
}

// ------------------------------ 各模式 ------------------------------

static int DoListen(const std::wstring& file, int seconds) {
    if (seconds <= 0 || seconds > 3600) seconds = 10;
    if (file.empty()) { Say(L"缺少文件参数"); return RC_ARG; }
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, GetModuleHandleW(NULL), 0);
    if (!g_keyHook) {
        Say(L"安装键盘钩子失败, 错误码 %lu", GetLastError());
        return RC_EXEC;
    }
    g_log.clear();
    g_recordInjected = true;
    g_recording = true;
    g_recStart = GetTickCount();
    Say(L"监听键盘 %d 秒 (含注入事件)...", seconds);
    MSG msg;
    DWORD end = GetTickCount() + (DWORD)seconds * 1000;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    g_recording = false;
    g_recordInjected = false;
    UnhookWindowsHookEx(g_keyHook);
    g_keyHook = NULL;
    if (SaveLog(file.c_str())) Say(L"已保存 %zu 个键盘事件到 %ls", g_log.size(), file.c_str());
    else { Say(L"保存失败: %ls", file.c_str()); return RC_FILE; }
    return RC_OK;
}

static int DoDump(const std::wstring& file) {
    if (!LoadLog(file.c_str())) return RC_FILE;
    Say(L"共 %zu 个事件:", g_log.size());
    for (const Ev& e : g_log) {
        const wchar_t* tn = L"?";
        switch (e.type) {
        case EV_MOVE: tn = L"MOVE"; break;
        case EV_LDOWN: tn = L"LDOWN"; break;
        case EV_LUP: tn = L"LUP"; break;
        case EV_RDOWN: tn = L"RDOWN"; break;
        case EV_RUP: tn = L"RUP"; break;
        case EV_MDOWN: tn = L"MDOWN"; break;
        case EV_MUP: tn = L"MUP"; break;
        case EV_WHEEL: tn = L"WHEEL"; break;
        case EV_KEYDOWN: tn = L"KEYDN"; break;
        case EV_KEYUP: tn = L"KEYUP"; break;
        }
        wchar_t ch = L' ';
        if ((e.vk >= (uint32_t)L'A' && e.vk <= (uint32_t)L'Z') || (e.vk >= (uint32_t)L'0' && e.vk <= (uint32_t)L'9')) ch = (wchar_t)e.vk;
        Say(L"  t=%-6u %-5ls vk=0x%02X %c (x=%d,y=%d)", e.t, tn, e.vk, ch, e.x, e.y);
    }
    return RC_OK;
}

static int DoRecord(const std::wstring& file) {
    if (file.empty()) { Say(L"缺少文件参数"); return RC_ARG; }
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(NULL), 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, GetModuleHandleW(NULL), 0);
    if (!g_mouseHook || !g_keyHook) {
        Say(L"安装钩子失败, 错误码 %lu", GetLastError());
        return RC_EXEC;
    }
    // --auto: 立即开始录, 适合自动化 (旧版必须手动按 ScrollLock, 忘按就是空文件)
    if (g_opt.autoStart) {
        g_log.clear();
        g_recStart = GetTickCount();
        g_recording = true;
        Say(L"[REC] 已自动开始录制 (--auto), 按 ScrollLock 停止, Esc 保存并退出");
    } else {
        Say(L"录制模式: 按 ScrollLock 开始/停止, Esc 保存并退出");
    }
    MSG msg; bool running = true;
    while (running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        int vk = 0;
        while (PopKey(vk)) {
            if (vk == VK_ESCAPE) { running = false; break; }
            if (vk == VK_SCROLL) {
                if (g_recording) { g_recording = false; Say(L"[REC] 停止, 共 %zu 个事件", g_log.size()); }
                else { g_log.clear(); g_recStart = GetTickCount(); g_recording = true; Say(L"[REC] 开始"); }
            }
        }
        Sleep(10);
    }
    g_recording = false;
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = NULL; }
    if (g_keyHook)   { UnhookWindowsHookEx(g_keyHook);   g_keyHook = NULL; }

    int rc = RC_OK;
    if (SaveLog(file.c_str())) {
        Say(L"已保存到 %ls (%zu 个事件)", file.c_str(), g_log.size());
        if (g_log.empty()) {
            // 空录制是静默丢数据的典型来源, 必须明确报出来并返回非零
            Say(L"警告: 录制结果是 0 个事件, 文件是空的。是否忘记按 ScrollLock?");
            rc = RC_EXEC;
        }
    } else {
        Say(L"保存失败: %ls", file.c_str());
        rc = RC_FILE;
    }
    return rc;
}

static int DoPlay(const std::wstring& file, int loop) {
    if (!LoadLog(file.c_str())) return RC_FILE;
    if (g_log.empty()) { Say(L"文件里没有事件, 无需回放"); return RC_EXEC; }
    Say(L"回放 %zu 个事件 (循环 %d 次)...", g_log.size(), loop);
    ResetEvent(g_stopEvent);
    g_progStart = GetTickCount();
    int done = 0;
    for (int i = 0; loop <= 0 || i < loop; i++) {
        if (ShouldAbort()) break;
        if (!PlayOnce()) break;
        done++;
        SleepInterruptible(150);
    }
    Say(L"回放结束 (%d 遍)。", done);
    if (DurationExceeded()) { Say(L"已超过 --max-duration 上限, 自动中止"); return RC_TIMEOUT; }
    if (StopRequested()) return RC_TIMEOUT;
    return RC_OK;
}

static int DoScript(const std::wstring& file) {
    std::wstring text;
    if (!ReadTextFile(file, text)) { Say(L"读取失败: %ls", file.c_str()); return RC_FILE; }
    Say(L"执行脚本: %ls", file.c_str());
    ResetEvent(g_stopEvent);
    g_vars.clear();
    g_progStart = GetTickCount();
    size_t pos = 0; int line = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring ln = text.substr(pos, nl - pos);
        pos = nl + 1;
        line++;
        Trim(ln);
        if (ln.empty() || ln[0] == L'#') continue;
        if (ShouldAbort()) {
            if (DurationExceeded()) Say(L"已超过 --max-duration 上限, 中止于第 %d 行", line);
            else Say(L"收到停止信号, 中止于第 %d 行", line);
            return RC_TIMEOUT;
        }
        if (!RunScriptLine(ln, line)) {
            Say(L"脚本在第 %d 行失败, 已中止", line);
            return RC_EXEC;
        }
        // SLEEP 这类命令被中断时不报错, 所以每行之后再查一次
        if (DurationExceeded()) { Say(L"已超过 --max-duration 上限, 中止于第 %d 行", line); return RC_TIMEOUT; }
        if (StopRequested())   { Say(L"收到停止信号, 中止于第 %d 行", line); return RC_TIMEOUT; }
    }
    Say(L"脚本执行完毕。");
    return RC_OK;
}

static int DoTestIcon(const std::wstring& icon) {
    if (g_opt.noIcon) { Say(L"--no-icon 已启用, 不触碰系统光标"); return RC_OK; }
    CursorSwapper cs;
    if (cs.Apply(icon.c_str())) {
        Say(L"图标加载成功! 鼠标已切换为自定义样式, 2 秒后自动还原。");
        Sleep(2000);
        return RC_OK;
    }
    Say(L"图标加载失败: %ls", icon.c_str());
    return RC_FILE;
}

static void PrintUsage() {
    wprintf(L"mouse_robot —— 命令行鼠标/键盘自动化工具\n");
    wprintf(L"用法:\n");
    wprintf(L"  mouse_robot record <文件.rec> [--auto]      录制: ScrollLock 开始/停止, Esc 保存退出\n");
    wprintf(L"  mouse_robot play <文件.rec> [--loop N]      回放 N 遍 (0=无限, 默认 1)\n");
    wprintf(L"  mouse_robot script <文件.txt>               执行文本脚本\n");
    wprintf(L"  mouse_robot stop                            给正在运行的实例发停止信号\n");
    wprintf(L"  mouse_robot testicon                        测试自定义鼠标样式 (2 秒还原)\n");
    wprintf(L"  mouse_robot listen <文件.rec> <秒数>         监听键盘N秒(含注入事件, 无需窗口)\n");
    wprintf(L"  mouse_robot dump <文件.rec>                 打印录制/监听事件内容\n");
    wprintf(L"全局选项:\n");
    wprintf(L"  --icon <ico路径>   自定义鼠标样式, 默认找 exe 同目录的 tray.ico\n");
    wprintf(L"  --no-icon          完全不改系统光标 (最安全)\n");
    wprintf(L"  --json             结果以 JSON 输出\n");
    wprintf(L"  --dry-run          只打印, 不发送任何输入\n");
    wprintf(L"  --max-duration N   总时长上限(秒), 超时自动中止\n");
    wprintf(L"脚本语法: MOVE x y [ms] | CLICKAT x y [ms] | MOVEREL dx dy | SLEEP ms | CLICK | RCLICK | DCLICK\n");
    wprintf(L"          | DOWN [键] | UP [键] | WHEEL n (120的倍数) | KEY 键名 | KEY 组合键 | TEXT 内容\n");
    wprintf(L"          | FOCUS 标题 | WAITWIN 标题 [超时ms] | GETPOS | SET 名 值 | ${名} 引用 | # 注释\n");
    wprintf(L"退出码: 0=成功 1=参数错误 2=文件错误 3=执行失败 4=超时/被停止\n");
}

// 日志路径改为可执行文件同目录, 不再写死 D:\MouseRobot (换目录就失效)
static void AppendRobotLog(const std::wstring& cmd, const std::wstring& file, int loop, int rc) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring path = exe;
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path = path.substr(0, slash + 1);
    path += L"robot.log";
    FILE* lf = NULL;
    if (_wfopen_s(&lf, path.c_str(), L"a") != 0 || !lf) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(lf, L"%04d-%02d-%02d %02d:%02d:%02d rc=%d cmd=%ls file=%ls loop=%d\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, rc,
             cmd.c_str(), file.c_str(), loop);
    fclose(lf);
}

// ------------------------------ 主入口 ------------------------------

int wmain(int argc, wchar_t** argv) {
    // DPI 感知: 用 PerMonitorV2 而不是 SetProcessDPIAware。
    // 后者在多显示器/不同缩放比例下拿到的坐标会错。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);

    // 输出编码 (2026-10 修复):
    //   SetConsoleOutputCP 只管控制台窗口; 一旦 stdout 被重定向到管道/文件,
    //   CRT 仍按系统 ANSI 代码页 (GBK) 转换宽字符, 中文全变成 '?'。
    //   这里把 locale 和 _setmode 都设为 UTF-8, 让 wprintf 直接吐 UTF-8 字节,
    //   PowerShell / Python 都能正确读到。
    setlocale(LC_ALL, ".UTF-8");
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);
    g_virtX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_virtY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_virtW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_virtH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    std::wstring cmd, file;
    std::wstring icon = DefaultIconPath();
    int loop = 1;
    int extra = -1;
    std::wstring extraStr;
    bool badArg = false;

    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--icon" && i + 1 < argc) icon = argv[++i];
        else if (a == L"--no-icon") g_opt.noIcon = true;
        else if (a == L"--json") g_opt.json = true;
        else if (a == L"--dry-run") g_opt.dryRun = true;
        else if (a == L"--auto") g_opt.autoStart = true;
        else if (a.rfind(L"--loop=", 0) == 0) loop = _wtoi(a.c_str() + 7);
        else if (a == L"--loop" && i + 1 < argc) loop = _wtoi(argv[++i]);
        else if (a == L"--max-duration" && i + 1 < argc) g_opt.maxDurationSec = _wtoi(argv[++i]);
        else if (a.rfind(L"--max-duration=", 0) == 0) g_opt.maxDurationSec = _wtoi(a.c_str() + 15);
        else if (a == L"-h" || a == L"--help" || a == L"/?") { PrintUsage(); return RC_OK; }
        else if (!a.empty() && a[0] == L'-' && a.size() > 1) {
            wprintf(L"未知选项: %ls\n\n", a.c_str());
            badArg = true;
        }
        else if (cmd.empty()) cmd = a;
        else if (file.empty()) file = a;
        else { extraStr = a; }   // 用于 listen 的秒数
    }
    if (badArg) { PrintUsage(); return RC_ARG; }
    if (!extraStr.empty()) {
        wchar_t* endp = NULL;
        long v = wcstol(extraStr.c_str(), &endp, 10);
        if (endp && *endp == L'\0') extra = (int)v;
        else { wprintf(L"无法解析的秒数参数: %ls\n", extraStr.c_str()); return RC_ARG; }
    }

    if (cmd.empty()) { PrintUsage(); return RC_OK; }
    if (cmd == L"stop") {
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
        if (ev) { SetEvent(ev); CloseHandle(ev); Say(L"已发送停止信号。"); }
        else Say(L"没有正在运行的实例。");
        return RC_OK;
    }
    if (cmd == L"testicon") return DoTestIcon(icon);
    if (cmd == L"listen") return DoListen(file, extra > 0 ? extra : 10);
    if (cmd == L"dump") {
        if (file.empty()) { Say(L"缺少文件参数"); return RC_ARG; }
        return DoDump(file);
    }
    if (cmd != L"record" && cmd != L"play" && cmd != L"script") {
        wprintf(L"未知命令: %ls\n\n", cmd.c_str());
        PrintUsage();
        return RC_ARG;
    }
    if (file.empty()) { wprintf(L"缺少文件参数\n"); return RC_ARG; }

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, kStopEventName);
    if (!g_stopEvent) { Say(L"创建停止事件失败 (错误码 %lu)", GetLastError()); return RC_EXEC; }

    // dry-run 不改光标; --no-icon 也不改; 没找到图标文件时静默跳过
    CursorSwapper cs;
    if (g_opt.dryRun || g_opt.noIcon || icon.empty()) { /* 不换光标 */ }
    else if (!cs.Apply(icon.c_str()))
        Say(L"注意: 自定义鼠标图标加载失败, 使用默认样式");

    int rc = RC_OK;
    if (cmd == L"record") rc = DoRecord(file);
    else if (cmd == L"play") rc = DoPlay(file, loop);
    else rc = DoScript(file);

    cs.Restore();          // 先还原光标, 再关事件
    CloseHandle(g_stopEvent);
    g_stopEvent = NULL;

    AppendRobotLog(cmd, file, loop, rc);

    if (g_opt.json) {
        wprintf(L"{\"ok\":%ls,\"cmd\":\"%ls\",\"file\":\"%ls\",\"exitCode\":%d,\"messages\":\"%ls\"}\n",
                rc == 0 ? L"true" : L"false", JsonEscape(cmd).c_str(), JsonEscape(file).c_str(), rc,
                g_jsonMsgs.c_str());
    }
    return rc;
}
