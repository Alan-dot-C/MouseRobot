// ============================================================================
//  eye.h —— 眼睛：截屏 + OCR
//
//  纯 C++/WinRT 调用系统自带 OCR，不依赖 .NET 或 PowerShell。
//
//  识别策略（2026-10-17 重构，依据实测）：
//    - 中文小字在 1x 下最好，放大反而会把相邻的词并掉/丢掉
//    - 英文和数字相反，放大 2~3 倍明显更准
//    - 本机同时装了 zh-Hans-CN 与 en-GB 两个引擎，按行选路能各取所长
//    - 裁切必须留白：没有留白的裁片会让引擎直接返回 0 行
// ============================================================================
#pragma once

#include "common.h"
#include <string>
#include <vector>

struct OcrLine {
    std::wstring text;
    RectI        box;          // 已经是真实屏幕坐标
    int          words = 0;
    double       quality = 1.0;// 0..1，越低越可能是乱码/丢词
    std::wstring src;          // zh1x / en2x / en3x / zh4x ...
};

struct OcrResult {
    bool                  ok = false;
    bool                  timedOut = false;
    std::wstring          error;
    std::vector<OcrLine>  lines;
    RectI                 region;    // 实际识别的屏幕区域
    int                   scale = 1; // 主通道用的放大倍数
};

struct OcrRequest {
    RectI        region;            // 空 = 全屏
    int          scale = 0;         // 0 = auto；>0 表示强制用这个倍数、只跑一路
    std::wstring lang = L"auto";    // auto / zh / en
    int          padPx = 12;        // 裁切留白（只对 --region 有意义）
    bool         merge = true;      // auto 时是否跑第二路（拉丁/数字）
    bool         secondLook = false;// 可疑行是否再认一遍
};

// 初始化 WinRT（进程内只需调用一次）
bool EyeInit();

// 单次识别的最长等待时间（毫秒，<=0 表示不限制）。默认 20000。
// 以前是无限等，OCR 服务卡住会把整个进程挂死。
void EyeSetTimeout(int ms);
int  EyeGetTimeout();

// 识别一块屏幕。这是主入口。
OcrResult EyeRecognize(const OcrRequest& req);

// 对一张图片文件走同一条管线（回归测量用）
OcrResult EyeRecognizeImageFile(const std::wstring& path);

// 兼容旧调用：单路识别，指定放大倍数。
OcrResult EyeCaptureAndOcr(const RectI& region, int scale);

// 直接识别内存里的 BGRA 像素（自上而下，32bpp）——测量工具用。
OcrResult EyeOcrPixels(const void* bgra, int w, int h);

// 只截屏存 PNG（调试用）
bool EyeSavePng(const std::wstring& path, const RectI& region);

// 屏幕指纹：把区域缩到 9x8 灰度算 dHash，返回 64 位。
uint64_t EyeScreenHash(const RectI& region);
int HashDistance(uint64_t a, uint64_t b);

// 粗网格（gw x gh），每格一个平均亮度，用来定位"哪里变了"
bool EyeCoarseGrid(int gw, int gh, std::vector<uint8_t>& cells, const RectI& region = RectI{});

// 主屏尺寸
RectI EyeScreenRect();
