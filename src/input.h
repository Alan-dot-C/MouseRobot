// ============================================================================
//  input.h —— 手：鼠标 / 键盘 / 脚本 / 录制回放
// ============================================================================
#pragma once

#include "common.h"
#include <string>
#include <vector>

// 初始化（安装停止事件）
bool InputInit();
void InputShutdown();

// 把进程启动时刻记下来，--max-duration 与 stop 才能覆盖"等屏幕"这类没有鼠标的阶段。
// 在 wmain 里尽早调用一次。
void AbortInit();

// ---- 基础动作 ----
void MoveTo(int x, int y, DWORD msSmooth = 300);void ClickAt(int x, int y, bool right = false);
void ClickNow(bool right = false);
void DoubleClickNow();
void WheelAt(int x, int y, int delta);
void LeftButton(bool down);
void HoldKey(int vk, bool down);
void PressKey(int vk, bool extended);
bool TypeText(const std::wstring& text, int charDelayMs = 15);

// ---- 按键名解析 ----
bool ResolveKeyName(const std::wstring& name, int& vk, bool& extended);
bool PressKeyCombo(const std::wstring& combo);

// ---- 窗口 ----
HWND FindWindowByTitle(const std::wstring& needle);
bool FocusWindow(const std::wstring& needle, DWORD waitMs = 3000);
bool WaitForWindow(const std::wstring& needle, DWORD timeoutMs);

// ---- 脚本 ----
// 返回 0 成功；其他为退出码
int RunScriptFile(const std::wstring& path);

// ---- 录制 / 回放 ----
int DoRecord(const std::wstring& path, bool autoStart);
int DoPlay(const std::wstring& path, int loop);
int DoStop();
int DoDump(const std::wstring& path);

// 是否收到停止信号 / 超时
bool ShouldAbort();
