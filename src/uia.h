// ============================================================================
//  uia.h —— UI Automation：直接读标准控件的名字、角色、矩形
//
//  为什么要有这条通道：OCR 再准也是"猜像素"。对记事本、资源管理器、设置、
//  Office 这类标准控件，系统本来就有一棵带名字和边界的树，零依赖、毫秒级、
//  不会认错字。OCR 应该只做它的兜底：游戏、画布、自绘界面。
// ============================================================================
#pragma once

#include "common.h"
#include <string>
#include <vector>

// UIAutomationCore.h 用了 COM 的 interface 宏，而 WIN32_LEAN_AND_MEAN
// 把 objbase.h 排除了，所以必须先把它引进来（和 eye.cpp 引 objidl.h 同理）。
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <winrt/base.h>

struct UiNode {
    std::wstring name;
    std::wstring role;      // button / edit / menuitem / text ...
    std::wstring value;     // 编辑框当前值（可能有）
    RectI        rect;
    bool         enabled = true;
    bool         offscreen = false;
    bool         invokable = false;
    winrt::com_ptr<IUIAutomationElement> el;
};

bool UiInit();

// 枚举一棵窗口的控件树。root 为 nullptr 时用当前前台窗口。
// timeoutMs 是总预算：浏览器第一次被查询时要现建无障碍树，可能很慢。
std::vector<UiNode> UiTree(HWND root, int maxDepth, int maxNodes, int timeoutMs);

// 按名字找控件：先当前窗口，找不到再看其它可见顶层窗口。
// 返回按匹配度排序的结果（完全相等 > 包含 > 编辑距离 1）。
std::vector<UiNode> UiFind(const std::wstring& needle, int maxNodes, int timeoutMs);

// 触发控件（优先 InvokePattern，不移动鼠标）
bool UiInvoke(const UiNode& node);

// 取控件里的文本（TextPattern / ValuePattern）
bool UiGetText(const UiNode& node, std::wstring& out);

// 取当前焦点控件里的文本
bool UiFocusedText(std::wstring& out);

// 给 doctor / 调试用：UIA 是否可用
bool UiAvailable();
