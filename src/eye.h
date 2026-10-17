// ============================================================================
//  eye.h —— 眼睛：截屏 + OCR
//
//  纯 C++ 实现（C++/WinRT 调用系统自带 OCR），不依赖 .NET 或 PowerShell。
// ============================================================================
#pragma once

#include "common.h"
#include <string>
#include <vector>

struct OcrLine {
    std::wstring text;
    RectI        box;      // 已经是真实屏幕坐标
    int          words = 0;
};

struct OcrResult {
    bool                  ok = false;
    std::wstring          error;
    std::vector<OcrLine>  lines;
    RectI                 region;    // 实际识别的屏幕区域
    int                   scale = 1; // 送 OCR 前的放大倍数
};

// 初始化 WinRT（进程内只需调用一次）
bool EyeInit();

// 截取屏幕区域并识别。region 为空表示整屏。
// scale 为送 OCR 前的放大倍数，小字需要更大（默认 2）。
OcrResult EyeCaptureAndOcr(const RectI& region, int scale);

// 只截屏存 PNG（调试用）
bool EyeSavePng(const std::wstring& path, const RectI& region);

// 屏幕指纹：把区域缩到 9x8 灰度算 dHash，返回 64 位。
// 用来判断"屏幕变了没有"，比比对 OCR 文本稳得多。
uint64_t EyeScreenHash(const RectI& region);

// 两个指纹差多少位（0 = 完全一样）
int HashDistance(uint64_t a, uint64_t b);

// 主屏尺寸
RectI EyeScreenRect();
