// ============================================================================
//  clip.h —— 剪贴板：读文本、写文本、临时保存/恢复
//
//  用来做 read-text 的兜底：Ctrl+A / Ctrl+C 拿到的文本是 100% 准确的，
//  比 OCR 便宜也比 OCR 准。代价是会动用户的剪贴板，所以调用方要负责恢复。
// ============================================================================
#pragma once

#include <string>

struct ClipSnapshot {
    bool         had = false;
    std::wstring text;
};

bool ClipGetText(std::wstring& out);
bool ClipSetText(const std::wstring& text);

// 记住当前剪贴板里的文本（只处理 CF_UNICODETEXT）
ClipSnapshot ClipTake();
void         ClipRestore(const ClipSnapshot& snap);
