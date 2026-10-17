// ============================================================================
//  sieve.h —— 筛子：把原始 OCR 结果压成 AI 真正需要的那一点信息
//
//  原始 OCR 一次能吐 60~80 行、每行还带坐标，直接喂给模型既贵又吵。
//  筛子做四件事：
//    1. 清洗：中文字被 OCR 拆成 "对 话 轨 迹"，重新拼回 "对话轨迹"；
//             丢掉纯符号噪声、OCR 乱码行
//    2. 归并：把碎行合并成视觉行，再按纵向间距聚成"区块"
//    3. 判型：按几何位置识别标题栏 / 工具栏 / 侧栏 / 正文 / 状态栏 /
//             对话框 / 按钮，给模型一个结构先验
//    4. 变化：和上一次的结果比对，只报告"新增了什么、消失了什么"
//
//  另外提供模糊查找：OCR 认错一两个字也能找到目标。
// ============================================================================
#pragma once

#include "common.h"
#include "eye.h"
#include "winenum.h"
#include <string>
#include <vector>

struct LineItem {
    std::wstring text;
    RectI        box;
};

struct Block {
    RectI                  box;
    std::vector<LineItem>  items;
    std::wstring           kind;    // title / toolbar / sidebar / content / status / dialog / button
    std::wstring           window;  // owning window title, empty when unknown
    int                    winZ = -1;
};

struct SieveOptions {
    bool         keepAll = false;      // 不做显著性裁剪
    int          maxLines = 40;        // 整体最多保留多少行
    int          maxLineChars = 120;   // 单行最长字符数
    bool         doDelta = true;       // 与上一次对比
    std::wstring statePath;            // 显式指定状态文件（测试用）
    std::wstring stateKey;             // 状态文件的键：多会话/多区域互相隔离
};

struct SieveResult {
    std::vector<Block> blocks;
    std::vector<WinInfo> windows;   // real window rectangles, in Z order
    int  newLines = 0;
    int  goneLines = 0;
    bool changed = false;
    std::vector<std::wstring> added;   // 新增行的文字（最多若干条）
    std::vector<std::wstring> removed; // 消失行的文字
};

SieveResult SieveRun(const std::vector<OcrLine>& lines, const SieveOptions& opt);

// 模糊查找：返回按匹配度排序的命中
struct FindHit {
    std::wstring text;
    RectI        box;
    double       score = 0;   // 1.0 = 完全一致
};
std::vector<FindHit> SieveFind(const std::vector<OcrLine>& lines,
                               const std::wstring& needle,
                               int maxHits = 5);

// 默认状态文件路径（%LOCALAPPDATA%\handeye\last_frame.txt）
std::wstring DefaultStatePath();

// 状态目录与按 key 隔离的文件名。
// 以前所有调用共用 last_frame.txt，两个会话同时用会互相把状态写坏。
std::wstring HandeyeStateDir();
std::wstring StateFilePath(const std::wstring& prefix, const std::wstring& key);
