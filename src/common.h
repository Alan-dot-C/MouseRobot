// ============================================================================
//  common.h —— 公共类型与工具
//
//  设计约束（2026-10-17）：
//    - 全部对外文本用 UTF-8 输出，避免中文经管道变乱码
//    - 不依赖任何具体 AI 厂商；输出是稳定的文本 / JSON 契约
//    - 不依赖 .NET / PowerShell / 第三方库
// ============================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

// ------------------------------ 基础几何 ------------------------------

struct RectI {
    int x = 0, y = 0, w = 0, h = 0;
    int right()  const { return x + w; }
    int bottom() const { return y + h; }
    int cx()     const { return x + w / 2; }
    int cy()     const { return y + h / 2; }
    bool empty() const { return w <= 0 || h <= 0; }
};

// 两个矩形在垂直方向上的重叠比例（0..1），用于判断是否同一行
inline double VerticalOverlap(const RectI& a, const RectI& b) {
    int top = (std::max)(a.y, b.y);
    int bot = (std::min)(a.bottom(), b.bottom());
    if (bot <= top) return 0.0;
    int overlap = bot - top;
    int base = (std::min)(a.h, b.h);
    return base > 0 ? double(overlap) / base : 0.0;
}

// ------------------------------ 编码 ------------------------------

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

// 把控制台输出统一切到 UTF-8（含重定向到管道的场景）
void InitConsoleUtf8();

// ------------------------------ 字符串 ------------------------------

std::wstring Trim(const std::wstring& s);
void ReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to);
bool StartsWith(const std::wstring& s, const std::wstring& p);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);
std::wstring ToUpper(const std::wstring& s);
std::string JsonEscape(const std::string& s);

// 是否中日韩文字 / 全角标点（用于判断 OCR 拆字后能否无空格拼接）
bool IsCjk(wchar_t c);

// 编辑距离（模糊匹配用）
int EditDistance(const std::wstring& a, const std::wstring& b);

// ------------------------------ 输出 ------------------------------

struct Options {
    bool json = false;
    bool verbose = false;
    bool dryRun = false;
    bool noIcon = false;
    bool autoStart = false;
    int  maxDurationSec = 0;
};

extern Options g_opt;

// 普通模式输出一行；--json 时收集到缓冲，最后统一打印
void Say(const wchar_t* fmt, ...);

// JSON 模式下把收集到的消息取走
std::wstring TakePendingMessages();
void ResetPendingMessages();

// ------------------------------ 窗口 / 光标 ------------------------------

std::wstring GetForegroundTitle();
POINT        GetCursorPosNow();
