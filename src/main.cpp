// ============================================================================
//  main.cpp —— handeye 命令行入口
//
//  目标：让 AI（任何模型，不绑定厂商）能低成本地"看"屏幕、"动"鼠标键盘、
//        并且把观察结果先经过一层筛子再交出去。
//
//  命令：
//    see   感知：截屏 + OCR + 筛子
//    act   执行脚本
//    click / type / key   单步动作
//    say   语音播报
//    rec / play / stop    录制回放
// ============================================================================
#include "common.h"
#include "eye.h"
#include "sieve.h"
#include "input.h"
#include "mouth.h"
#include "log.h"
#include "logquery.h"
#include "bench.h"

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>

#include <winrt/base.h>

// ------------------------------ 参数解析辅助 ------------------------------

// 这些选项后面跟一个值，解析"位置参数"时要连同值一起跳过
static bool IsValueFlag(const std::wstring& s) {
    static const wchar_t* flags[] = {
        L"--region", L"--scale", L"--max-lines", L"--max-chars", L"--max-paras",
        L"--timeout", L"--find", L"--wait", L"--save", L"--voice", L"--rate",
        L"--volume", L"--wav", L"--loop", L"--max-duration", L"--icon",
    };
    for (auto f : flags) if (s == f) return true;
    return false;
}

struct ArgList {
    std::vector<std::wstring> raw;
    bool Has(const std::wstring& name) const {
        for (const auto& a : raw) if (a == name) return true;
        return false;
    }
    std::wstring Get(const std::wstring& name, const std::wstring& def = L"") const {
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == name && i + 1 < raw.size()) return raw[i + 1];
        }
        return def;
    }
    int GetInt(const std::wstring& name, int def) const {
        std::wstring v = Get(name, L"");
        if (v.empty()) return def;
        return _wtoi(v.c_str());
    }
    // 位置参数：不以 - 开头、也不是某个选项的值
    std::vector<std::wstring> Positional() const {
        std::vector<std::wstring> out;
        for (size_t i = 0; i < raw.size(); ++i) {
            const std::wstring& a = raw[i];
            if (!a.empty() && a[0] == L'-') {
                if (IsValueFlag(a)) ++i;   // 跳过它的值
                continue;
            }
            out.push_back(a);
        }
        return out;
    }
};

static bool ParseRect(const std::wstring& s, RectI& out) {
    auto p = Split(s, L',');
    if (p.size() != 4) return false;
    out.x = _wtoi(p[0].c_str());
    out.y = _wtoi(p[1].c_str());
    out.w = _wtoi(p[2].c_str());
    out.h = _wtoi(p[3].c_str());
    return out.w > 0 && out.h > 0;
}

static std::vector<LineItem> BuildContentParagraphs(const SieveResult& sr, int maxChars);
static int Dispatch(const ArgList& args, const std::wstring& cmd);

// ------------------------------ 屏幕指纹状态 ------------------------------

static std::wstring HashStatePath() {
    std::wstring p = DefaultStatePath();      // ...\handeye\last_frame.txt
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"last_hash.txt";
    return p.substr(0, slash + 1) + L"last_hash.txt";
}

static bool ReadHashState(uint64_t& h) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, HashStatePath().c_str(), L"rb") != 0 || !f) return false;
    char buf[64] = {};
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (got == 0) return false;
    h = _strtoui64(buf, nullptr, 16);
    return true;
}

static void WriteHashState(uint64_t h) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, HashStatePath().c_str(), L"wb") != 0 || !f) return;
    fprintf(f, "%016llx", (unsigned long long)h);
    fclose(f);
}

// ------------------------------ see ------------------------------

static int CmdSee(const ArgList& args) {
    RectI region{};
    std::wstring regionStr = args.Get(L"--region");
    if (!regionStr.empty() && !ParseRect(regionStr, region)) {
        wprintf(L"ERR --region 需要 \"x,y,w,h\" 格式\n");
        return 1;
    }

    // --changed 走图像指纹，不需要 OCR，又快又稳。
    // 早先用 OCR 文本比对，OCR 逐帧有小抖动，会误报 +4/-4。
    if (args.Has(L"--changed")) {
        uint64_t now = EyeScreenHash(region);
        uint64_t prev = 0;
        bool havePrev = ReadHashState(prev);
        int dist = havePrev ? HashDistance(prev, now) : 64;
        const int threshold = args.GetInt(L"--threshold", 3);
        bool changed = !havePrev || dist > threshold;
        WriteHashState(now);
        if (g_opt.json) {
            wprintf(L"{\"changed\":%ls,\"distance\":%d,\"firstFrame\":%ls}\n",
                    changed ? L"true" : L"false", dist, havePrev ? L"false" : L"true");
        } else {
            wprintf(L"%ls\tdistance=%d\n", changed ? L"CHANGED" : L"UNCHANGED", dist);
        }
        return 0;
    }

    if (!EyeInit()) {
        if (g_opt.json) {
            wprintf(L"{\"ok\":false,\"error\":\"no OCR engine\"}\n");
        } else {
            wprintf(L"ERR 没有可用的 OCR 引擎（可能没装中文 OCR 语言包）\n");
        }
        LogE(L"感知", 3006, L"异常", L"OCR 引擎不可用", L"reason=no-engine");
        return 3;
    }

    int scale = args.GetInt(L"--scale", 2);
    if (scale < 1) scale = 1;

    // --wait：轮询直到文字出现
    std::wstring waitText = args.Get(L"--wait");
    int timeoutMs = args.GetInt(L"--timeout", 5000);

    if (!waitText.empty()) {
        DWORD start = GetTickCount();
        for (;;) {
            OcrResult r = EyeCaptureAndOcr(region, scale);
            if (!r.ok) {
                wprintf(L"ERR %ls\n", r.error.c_str());
                return 3;
            }
            auto hits = SieveFind(r.lines, waitText, 1);
            if (!hits.empty()) {
                const auto& h = hits[0];
                if (g_opt.json) {
                    wprintf(L"{\"ok\":true,\"found\":true,\"text\":\"%ls\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"score\":%.2f,\"elapsedMs\":%lu}\n",
                            h.text.c_str(), h.box.x, h.box.y, h.box.w, h.box.h, h.score,
                            (unsigned long)(GetTickCount() - start));
                } else {
                    wprintf(L"FOUND\t%ls\t%d\t%d\t%d\t%d\t%.2f\t%lums\n",
                            h.text.c_str(), h.box.x, h.box.y, h.box.w, h.box.h, h.score,
                            (unsigned long)(GetTickCount() - start));
                }
                LogI(L"感知", 3011, L"感知", L"等待文字出现", L"query=%ls ms=%lu", waitText.c_str(), (unsigned long)(GetTickCount() - start));
                return 0;
            }
            if ((int)(GetTickCount() - start) > timeoutMs) {
                if (g_opt.json)
                    wprintf(L"{\"ok\":false,\"found\":false,\"error\":\"timeout\",\"elapsedMs\":%lu}\n",
                            (unsigned long)(GetTickCount() - start));
                else
                    wprintf(L"NOTFOUND\t超时 %dms\n", timeoutMs);
                LogW(L"感知", 3010, L"感知", L"等待文字超时", L"query=%ls timeout=%d", waitText.c_str(), timeoutMs);
                return 1;
            }
            Sleep(200);
        }
    }

    // 保存截图（可选）
    std::wstring savePath = args.Get(L"--save");
    if (!savePath.empty()) {
        if (!EyeSavePng(savePath, region)) {
            wprintf(L"WARN 截图保存失败: %ls\n", savePath.c_str());
        }
    }

    DWORD tCapture = GetTickCount();
    OcrResult r = EyeCaptureAndOcr(region, scale);
    DWORD msCapture = GetTickCount() - tCapture;
    if (!r.ok) {
        LogE(L"感知", 3008, L"异常", L"截屏失败", L"err=%ls ms=%lu", r.error.c_str(), (unsigned long)msCapture);
        if (g_opt.json) {
            std::string esc = JsonEscape(ToUtf8(r.error));
            wprintf(L"{\"ok\":false,\"error\":\"%hs\"}\n", esc.c_str());
        } else {
            wprintf(L"ERR %ls\n", r.error.c_str());
        }
        return 3;
    }
    LogV(L"感知", 3000, L"感知", L"截屏开始", L"x=%d y=%d w=%d h=%d scale=%d", region.x, region.y, region.w, region.h, r.scale);
    LogV(L"感知", 3002, L"感知", L"OCR 完成", L"raw=%zu ms=%lu", r.lines.size(), (unsigned long)msCapture);

    // --raw：不做筛子，直接吐原始行
    if (args.Has(L"--raw")) {
        if (g_opt.json) {
            std::wstring s = L"{\"ok\":true,\"raw\":true,\"count\":";
            s += std::to_wstring(r.lines.size());
            s += L",\"lines\":[";
            for (size_t i = 0; i < r.lines.size(); ++i) {
                if (i) s += L",";
                s += L"{\"text\":\"";
                s += FromUtf8(JsonEscape(ToUtf8(r.lines[i].text)));
                s += L"\",\"x\":" + std::to_wstring(r.lines[i].box.x);
                s += L",\"y\":" + std::to_wstring(r.lines[i].box.y);
                s += L",\"w\":" + std::to_wstring(r.lines[i].box.w);
                s += L",\"h\":" + std::to_wstring(r.lines[i].box.h) + L"}";
            }
            s += L"]}\n";
            wprintf(L"%ls", s.c_str());
        } else {
            wprintf(L"INFO raw=%zu scale=%d\n", r.lines.size(), r.scale);
            for (const auto& l : r.lines) {
                wprintf(L"%ls\t%d\t%d\t%d\t%d\n", l.text.c_str(), l.box.x, l.box.y, l.box.w, l.box.h);
            }
        }
        return 0;
    }

    // --find：只看匹配
    std::wstring findText = args.Get(L"--find");
    if (!findText.empty()) {
        auto hits = SieveFind(r.lines, findText, 8);
        if (g_opt.json) {
            std::wstring s = L"{\"ok\":true,\"count\":" + std::to_wstring(hits.size()) + L",\"hits\":[";
            for (size_t i = 0; i < hits.size(); ++i) {
                if (i) s += L",";
                s += L"{\"text\":\"" + FromUtf8(JsonEscape(ToUtf8(hits[i].text))) + L"\"";
                s += L",\"x\":" + std::to_wstring(hits[i].box.x);
                s += L",\"y\":" + std::to_wstring(hits[i].box.y);
                s += L",\"w\":" + std::to_wstring(hits[i].box.w);
                s += L",\"h\":" + std::to_wstring(hits[i].box.h);
                s += L",\"score\":" + std::to_wstring(hits[i].score) + L"}";
            }
            s += L"]}\n";
            wprintf(L"%ls", s.c_str());
        } else {
            if (hits.empty()) {
                wprintf(L"NOTFOUND\t%ls\n", findText.c_str());
                LogV(L"感知", 3005, L"感知", L"查找未命中", L"query=%ls", findText.c_str());
                return 1;
            }
            LogI(L"感知", 3004, L"感知", L"查找命中", L"query=%ls hits=%zu best=%.2f", findText.c_str(), hits.size(), hits[0].score);
            for (const auto& h : hits) {
                wprintf(L"HIT\t%ls\t%d\t%d\t%d\t%d\t%.2f\n",
                        h.text.c_str(), h.box.x, h.box.y, h.box.w, h.box.h, h.score);
            }
        }
        return hits.empty() ? 1 : 0;
    }

    // 正常：走筛子
    SieveOptions so;
    so.keepAll = args.Has(L"--all");
    so.maxLines = args.GetInt(L"--max-lines", 40);
    so.maxLineChars = args.GetInt(L"--max-chars", 120);
    so.doDelta = !args.Has(L"--no-delta");
    so.dropUnchanged = args.Has(L"--changed-only");

    SieveResult sr = SieveRun(r.lines, so);
    LogV(L"感知", 3003, L"感知", L"筛子完成", L"blocks=%zu raw=%zu", sr.blocks.size(), r.lines.size());

    POINT cur = GetCursorPosNow();
    std::wstring focus = GetForegroundTitle();
    RectI scr = EyeScreenRect();

    if (g_opt.json) {
        std::wstring s = L"{\"ok\":true";
        s += L",\"screen\":{\"w\":" + std::to_wstring(scr.w) + L",\"h\":" + std::to_wstring(scr.h) + L"}";
        s += L",\"cursor\":{\"x\":" + std::to_wstring(cur.x) + L",\"y\":" + std::to_wstring(cur.y) + L"}";
        s += L",\"focus\":\"" + FromUtf8(JsonEscape(ToUtf8(focus))) + L"\"";
        s += L",\"rawLines\":" + std::to_wstring(r.lines.size());
        s += L",\"delta\":{\"changed\":" + std::wstring(sr.changed ? L"true" : L"false");
        s += L",\"added\":" + std::to_wstring(sr.newLines);
        s += L",\"removed\":" + std::to_wstring(sr.goneLines) + L"}";
        s += L",\"blocks\":[";
        bool firstBlock = true;
        for (const auto& b : sr.blocks) {
            if (!firstBlock) s += L",";
            firstBlock = false;
            s += L"{\"kind\":\"" + b.kind + L"\"";
            s += L",\"box\":{\"x\":" + std::to_wstring(b.box.x) + L",\"y\":" + std::to_wstring(b.box.y);
            s += L",\"w\":" + std::to_wstring(b.box.w) + L",\"h\":" + std::to_wstring(b.box.h) + L"}";
            s += L",\"items\":[";
            for (size_t i = 0; i < b.items.size(); ++i) {
                if (i) s += L",";
                s += L"{\"text\":\"" + FromUtf8(JsonEscape(ToUtf8(b.items[i].text))) + L"\"";
                s += L",\"x\":" + std::to_wstring(b.items[i].box.x);
                s += L",\"y\":" + std::to_wstring(b.items[i].box.y);
                s += L",\"w\":" + std::to_wstring(b.items[i].box.w);
                s += L",\"h\":" + std::to_wstring(b.items[i].box.h) + L"}";
            }
            s += L"]}";
        }
        s += L"]}\n";
        wprintf(L"%ls", s.c_str());
        return 0;
    }

    // 文本输出：紧凑、可解析
    // 策略：正文按阅读顺序拼成段落（正文不需要逐行坐标），
    //       只有可能被点击的元素（标题/按钮/对话框/侧栏/工具栏）才逐行给坐标。
    std::vector<LineItem> contentParas = BuildContentParagraphs(sr, 140);
    int maxParas = args.GetInt(L"--max-paras", 10);
    if (!args.Has(L"--all") && (int)contentParas.size() > maxParas)
        contentParas.resize(maxParas);

    wprintf(L"INFO screen=%dx%d cursor=%d,%d scale=%d raw=%zu blocks=%zu paras=%zu delta=+%d/-%d\n",
            scr.w, scr.h, cur.x, cur.y, r.scale, r.lines.size(), sr.blocks.size(),
            contentParas.size(), sr.newLines, sr.goneLines);
    if (!focus.empty()) wprintf(L"[focus] %ls\n", focus.c_str());

    for (const auto& b : sr.blocks) {
        if (b.kind == L"content") continue;   // 正文统一在下面按段落输出
        for (const auto& it : b.items) {
            wprintf(L"[%ls] %ls @%d,%d %dx%d\n",
                    b.kind.c_str(), it.text.c_str(),
                    it.box.x, it.box.y, it.box.w, it.box.h);
        }
    }
    for (const auto& p : contentParas) {
        wprintf(L"[content] %ls @%d,%d %dx%d\n",
                p.text.c_str(), p.box.x, p.box.y, p.box.w, p.box.h);
    }

    if (args.Has(L"--show-delta") && sr.changed) {
        for (const auto& a : sr.added) wprintf(L"  + %ls\n", a.c_str());
        for (const auto& d : sr.removed) wprintf(L"  - %ls\n", d.c_str());
    }
    return 0;
}

// 把散落的正文行按阅读顺序拼成段落，大幅压掉输出体积。
// 关键：先按 x 分栏，再在栏内按 y 排序拼段。
// 只按 y 排序会把侧栏和正文交错混在一起。
static std::vector<LineItem> BuildContentParagraphs(const SieveResult& sr, int maxChars) {
    std::vector<LineItem> items;
    for (const auto& b : sr.blocks)
        if (b.kind == L"content")
            for (const auto& it : b.items) items.push_back(it);

    if (items.empty()) return {};

    // 1) 按左边界分栏
    std::vector<LineItem> byX = items;
    std::sort(byX.begin(), byX.end(), [](const LineItem& a, const LineItem& b) {
        return a.box.x < b.box.x;
    });
    std::vector<std::vector<LineItem>> columns;
    int colStart = byX[0].box.x;
    columns.push_back({});
    for (const auto& it : byX) {
        if (it.box.x - colStart > 140) {
            columns.push_back({});
            colStart = it.box.x;
        }
        columns.back().push_back(it);
    }

    // 2) 每栏内部按 y 排序后拼段
    std::vector<LineItem> out;
    for (auto& col : columns) {
        std::sort(col.begin(), col.end(), [](const LineItem& a, const LineItem& b) {
            return a.box.y < b.box.y;
        });
        LineItem cur{};
        bool has = false;
        int lastBottom = 0;
        for (const auto& it : col) {
            bool newChunk = !has;
            if (has) {
                if (it.box.y - lastBottom > 42) newChunk = true;
                if ((int)(cur.text.size() + it.text.size()) > maxChars) newChunk = true;
            }
            if (newChunk) {
                if (has) out.push_back(cur);
                cur = it;
                has = true;
            } else {
                cur.text += L" ";
                cur.text += it.text;
                int right = (std::max)(cur.box.right(), it.box.right());
                int bottom = (std::max)(cur.box.bottom(), it.box.bottom());
                cur.box.w = right - cur.box.x;
                cur.box.h = bottom - cur.box.y;
            }
            lastBottom = (std::max)(lastBottom, it.box.bottom());
        }
        if (has) out.push_back(cur);
    }
    return out;
}

// ------------------------------ click-text：眼手联动 ------------------------------

// 找到屏幕上的文字并点它。这是"眼"和"手"的接缝：
// AI 不需要知道坐标，只要说要点什么。
static int CmdClickText(const ArgList& args) {
    auto pos = args.Positional();
    if (pos.size() < 2) { wprintf(L"ERR 用法: click-text <文字> [--index N] [--timeout ms]\n"); return 1; }
    std::wstring needle;
    for (size_t i = 1; i < pos.size(); ++i) {
        if (!needle.empty()) needle += L" ";
        needle += pos[i];
    }
    if (!EyeInit()) { wprintf(L"ERR 没有可用的 OCR 引擎\n"); return 3; }

    int scale = args.GetInt(L"--scale", 2);
    int timeout = args.GetInt(L"--timeout", 0);
    int wanted = args.GetInt(L"--index", 0);
    DWORD start = GetTickCount();

    for (;;) {
        OcrResult r = EyeCaptureAndOcr(RectI{}, scale);
        if (!r.ok) { wprintf(L"ERR %ls\n", r.error.c_str()); return 3; }

        auto hits = SieveFind(r.lines, needle, 8);
        if (!hits.empty()) {
            int idx = wanted;
            if (idx < 0 || idx >= int(hits.size())) idx = 0;
            const auto& h = hits[idx];

            if (g_opt.dryRun) {
                if (g_opt.json) {
                    wprintf(L"{\"ok\":true,\"dryRun\":true,\"text\":\"%ls\",\"x\":%d,\"y\":%d,\"score\":%.2f}\n",
                            h.text.c_str(), h.box.cx(), h.box.cy(), h.score);
                } else {
                    wprintf(L"[dry-run] 会点击「%ls」@%d,%d (匹配度 %.2f)\n",
                            h.text.c_str(), h.box.cx(), h.box.cy(), h.score);
                }
                return 0;
            }

            InputInit();
            ClickAt(h.box.cx(), h.box.cy(), args.Has(L"--right"));
            InputShutdown();

            if (g_opt.json) {
                wprintf(L"{\"ok\":true,\"text\":\"%ls\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"score\":%.2f}\n",
                        h.text.c_str(), h.box.cx(), h.box.cy(), h.box.w, h.box.h, h.score);
            } else {
                wprintf(L"CLICKED\t%ls\t%d\t%d\t%.2f\n", h.text.c_str(), h.box.cx(), h.box.cy(), h.score);
            }
            return 0;
        }

        if (timeout <= 0 || int(GetTickCount() - start) > timeout) {
            if (g_opt.json) wprintf(L"{\"ok\":false,\"error\":\"not found\"}\n");
            else wprintf(L"NOTFOUND\t%ls\n", needle.c_str());
            return 1;
        }
        Sleep(300);
    }
}

// ------------------------------ 能力自述 ------------------------------

// 输出一份机器可读的能力清单，任何 AI 都能据此学会怎么用这个工具。
static void PrintSchema() {
    wprintf(L"{\n");
    wprintf(L"  \"tool\": \"handeye\",\n");
    wprintf(L"  \"purpose\": \"给 AI 使用的屏幕感知与鼠标键盘控制。不绑定任何模型厂商。\",\n");
    wprintf(L"  \"commands\": [\n");
    wprintf(L"    {\"name\":\"see\",\"desc\":\"看屏幕，结果经过筛子压缩\",\"options\":[\"--region x,y,w,h\",\"--scale N\",\"--all\",\"--max-paras N\",\"--raw\",\"--find 文字\",\"--wait 文字\",\"--changed\",\"--no-delta\",\"--show-delta\",\"--save 文件.png\"]},\n");
    wprintf(L"    {\"name\":\"click-text\",\"desc\":\"找到文字并点击（眼手联动）\",\"args\":[\"文字\"],\"options\":[\"--index N\",\"--timeout ms\",\"--right\"]},\n");
    wprintf(L"    {\"name\":\"click\",\"desc\":\"点击坐标\",\"args\":[\"x\",\"y\"],\"options\":[\"--right\"]},\n");
    wprintf(L"    {\"name\":\"move\",\"desc\":\"移动鼠标\",\"args\":[\"x\",\"y\"]},\n");
    wprintf(L"    {\"name\":\"type\",\"desc\":\"输入文字（支持中文与任意 Unicode）\",\"args\":[\"文字\"]},\n");
    wprintf(L"    {\"name\":\"key\",\"desc\":\"按键或组合键\",\"args\":[\"键名\"],\"examples\":[\"ENTER\",\"CTRL+S\",\"0x41\",\"F5\"]},\n");
    wprintf(L"    {\"name\":\"focus\",\"desc\":\"把窗口切到前台\",\"args\":[\"标题片段\"]},\n");
    wprintf(L"    {\"name\":\"act\",\"desc\":\"执行脚本文件\",\"args\":[\"路径\"]},\n");
    wprintf(L"    {\"name\":\"say\",\"desc\":\"语音播报\",\"args\":[\"文字\"],\"options\":[\"--voice\",\"--rate\",\"--volume\",\"--wav\",\"--async\"]},\n");
    wprintf(L"    {\"name\":\"voices\",\"desc\":\"列出语音\"},\n");
    wprintf(L"    {\"name\":\"rec\",\"desc\":\"录制\",\"args\":[\"文件.rec\"],\"options\":[\"--auto\"]},\n");
    wprintf(L"    {\"name\":\"play\",\"desc\":\"回放\",\"args\":[\"文件.rec\"],\"options\":[\"--loop N\"]},\n");
    wprintf(L"    {\"name\":\"stop\",\"desc\":\"停止正在运行的动作\"},\n");
    wprintf(L"    {\"name\":\"schema\",\"desc\":\"输出这份清单\"}\n");
    wprintf(L"  ],\n");
    wprintf(L"  \"globalOptions\": [\"--json\",\"--dry-run\",\"--max-duration 秒\"],\n");
    wprintf(L"  \"exitCodes\": {\"0\":\"成功\",\"1\":\"参数错误或未找到\",\"2\":\"文件错误\",\"3\":\"执行失败\",\"4\":\"超时或被停止\"},\n");
    wprintf(L"  \"outputFormats\": {\n");
    wprintf(L"    \"text\": \"[类型] 文字 @x,y wxh；类型为 title/toolbar/sidebar/content/status/dialog/button\",\n");
    wprintf(L"    \"json\": \"结构化对象，含 blocks[].items[] 与 delta\"\n");
    wprintf(L"  },\n");
    wprintf(L"  \"tips\": [\n");
    wprintf(L"    \"先用 see --changed 判断屏幕有没有动，代价最低\",\n");
    wprintf(L"    \"要点击某个按钮，直接 click-text 按钮文字，不用自己算坐标\",\n");
    wprintf(L"    \"文字找不到时用 --wait 轮询，不要盲目重复截图\",\n");
    wprintf(L"    \"不确定要做什么时先 --dry-run，看清楚再真执行\",\n");
    wprintf(L"    \"中文小字识别率低，可以把 --scale 调到 3\",\n");
    wprintf(L"    \"选中高亮（蓝底白字）会让 OCR 明显变差，必要时先取消选择\"\n");
    wprintf(L"  ]\n");
    wprintf(L"}\n");
}

// ------------------------------ 帮助 ------------------------------

static void PrintUsage() {
    wprintf(L"handeye —— 给 AI 用的手 / 嘴 / 眼\n");
    wprintf(L"\n");
    wprintf(L"  handeye see [选项]              看屏幕：截屏 + OCR + 筛子\n");
    wprintf(L"  handeye act <脚本.txt>          执行脚本\n");
    wprintf(L"  handeye click <x> <y> [--right] 点一下\n");
    wprintf(L"  handeye type <文字>             输入文字（支持中文）\n");
    wprintf(L"  handeye key <键名或组合键>      按键，如 ENTER / CTRL+S / 0x41\n");
    wprintf(L"  handeye say <文字> [--voice V] [--rate N] [--volume N] [--wav 文件]\n");
    wprintf(L"  handeye voices                  列出可用语音\n");
    wprintf(L"  handeye rec <文件.rec> [--auto] 录制\n");
    wprintf(L"  handeye play <文件.rec> [--loop N] 回放\n");
    wprintf(L"  handeye dump <文件.rec>         打印录制内容\n");
    wprintf(L"  handeye stop                    停止正在运行的动作\n");
    wprintf(L"\n");
    wprintf(L"see 的选项:\n");
    wprintf(L"  --region x,y,w,h   只识别这一块区域\n");
    wprintf(L"  --scale N          送 OCR 前的放大倍数（默认 2，小字用 3）\n");
    wprintf(L"  --all              不做显著性裁剪，输出全部\n");
    wprintf(L"  --max-paras N      正文最多输出几段（默认 10）\n");
    wprintf(L"  --raw              不做筛子，吐原始 OCR 行\n");
    wprintf(L"  --find <文字>      模糊查找，返回坐标与匹配度\n");
    wprintf(L"  --wait <文字>      轮询直到出现（配合 --timeout，默认 5000ms）\n");
    wprintf(L"  --changed          只回答屏幕变了没有\n");
    wprintf(L"  --no-delta         不与上一次比对\n");
    wprintf(L"  --show-delta       显示新增/消失的行\n");
    wprintf(L"  --save <文件.png>  同时保存截图\n");
    wprintf(L"\n");
    wprintf(L"通用选项: --json  --dry-run  --max-duration <秒>\n");
    wprintf(L"退出码: 0 成功 / 1 参数或未找到 / 2 文件 / 3 执行失败 / 4 超时\n");
}

int wmain(int argc, wchar_t** argv) {
    InitConsoleUtf8();

    // DPI：用 PerMonitorV2，多显示器下坐标才正确
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // OCR 与语音合成都是 WinRT，必须先初始化 COM 公寓。
    // 用 MTA：OCR 的异步调用在 MTA 上不会像 STA 那样有死锁风险。
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        // 已初始化过就忽略
    }

    ArgList args;
    for (int i = 1; i < argc; ++i) args.raw.push_back(argv[i]);

    // 先起日志，再处理任何东西 —— 参数错误也要留痕
    LogInit();
    {
        bool hasLevel = args.Has(L"--log-level");
        bool hasText = args.Has(L"--log-text");
        bool hasDir = args.Has(L"--log-dir");
        int textMode = g_log.textMode;
        std::wstring tv = ToUpper(args.Get(L"--log-text"));
        if (tv == L"MASK") textMode = LOG_TEXT_MASK;
        else if (tv == L"OFF") textMode = LOG_TEXT_OFF;
        else if (tv == L"FULL") textMode = LOG_TEXT_FULL;
        LogConfigure(hasDir, args.Get(L"--log-dir"),
                     hasLevel, LogLevelFromName(args.Get(L"--log-level")),
                     hasText, textMode,
                     args.Has(L"--no-log"), false);
        if (args.Has(L"--log-echo")) g_log.echoToStderr = true;
    }

    g_opt.json = args.Has(L"--json");
    g_opt.dryRun = args.Has(L"--dry-run");
    g_opt.maxDurationSec = args.GetInt(L"--max-duration", 0);

    if (args.raw.empty()) {
        PrintUsage();
        LogShutdown();
        return 0;
    }

    std::wstring cmd = args.raw[0];
    for (wchar_t& c : cmd) c = towlower(c);

    // 会话开始：把完整命令行记下来，事后能复原"AI 到底调了什么"
    {
        std::wstring argvJoin;
        for (size_t i = 0; i < args.raw.size(); ++i) {
            if (i) argvJoin += L" ";
            argvJoin += args.raw[i];
        }
        if (argvJoin.size() > 400) argvJoin = argvJoin.substr(0, 400) + L"…";
        wchar_t cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        LogI(L"会话", 8000, L"会话", L"会话开始", L"argv=\"%ls\" cwd=%ls",
             argvJoin.c_str(), cwd);
    }

    int rc = Dispatch(args, cmd);

    {
        DWORD ms = GetTickCount() - LogStartTick();
        if (rc == 0)
            LogI(L"会话", 8001, L"会话", L"会话结束", L"rc=0 ms=%lu", (unsigned long)ms);
        else
            LogW(L"会话", 8001, L"会话", L"会话结束", L"rc=%d ms=%lu", rc, (unsigned long)ms);
    }
    LogShutdown();
    return rc;
}

static int Dispatch(const ArgList& args, const std::wstring& cmd) {
    if (cmd == L"help" || cmd == L"--help" || cmd == L"-h") { PrintUsage(); return 0; }
    if (cmd == L"schema") { PrintSchema(); return 0; }
    if (cmd == L"log")   return CmdLogQuery(args.raw);
    if (cmd == L"bench") return CmdBench(args.raw);
    if (cmd == L"see") return CmdSee(args);
    if (cmd == L"click-text" || cmd == L"clicktext") return CmdClickText(args);

    auto pos = args.Positional();   // pos[0] 是命令本身

    // ---------- 手 ----------
    if (cmd == L"act" || cmd == L"script") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少脚本路径\n"); return 1; }
        InputInit();
        int rc = RunScriptFile(pos[1]);
        InputShutdown();
        return rc;
    }
    if (cmd == L"click") {
        if (pos.size() < 3) { wprintf(L"ERR 用法: click <x> <y> [--right]\n"); return 1; }
        InputInit();
        ClickAt(_wtoi(pos[1].c_str()), _wtoi(pos[2].c_str()), args.Has(L"--right"));
        InputShutdown();
        return 0;
    }
    if (cmd == L"move") {
        if (pos.size() < 3) { wprintf(L"ERR 用法: move <x> <y>\n"); return 1; }
        InputInit();
        MoveTo(_wtoi(pos[1].c_str()), _wtoi(pos[2].c_str()));
        InputShutdown();
        return 0;
    }
    if (cmd == L"type") {
        std::wstring text;
        for (size_t i = 1; i < pos.size(); ++i) {
            if (!text.empty()) text += L" ";
            text += pos[i];
        }
        if (text.empty()) { wprintf(L"ERR 缺少要输入的文字\n"); return 1; }
        InputInit();
        bool ok = TypeText(text);
        InputShutdown();
        return ok ? 0 : 4;
    }
    if (cmd == L"key") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少键名\n"); return 1; }
        InputInit();
        bool ok = false;
        if (pos[1].find(L'+') != std::wstring::npos) {
            ok = PressKeyCombo(pos[1]);
        } else {
            int vk = 0;
            bool ext = false;
            if (ResolveKeyName(pos[1], vk, ext)) { PressKey(vk, ext); ok = true; }
            else if (pos[1].size() == 1) { ok = TypeText(pos[1]); }
        }
        InputShutdown();
        if (!ok) { wprintf(L"ERR 无法识别的键: %ls\n", pos[1].c_str()); return 1; }
        return 0;
    }
    if (cmd == L"focus") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少窗口标题片段\n"); return 1; }
        InputInit();
        bool ok = FocusWindow(pos[1]);
        InputShutdown();
        if (!ok) { wprintf(L"ERR 找不到窗口: %ls\n", pos[1].c_str()); return 1; }
        return 0;
    }
    if (cmd == L"rec" || cmd == L"record") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少文件路径\n"); return 1; }
        InputInit();
        int rc = DoRecord(pos[1], args.Has(L"--auto"));
        InputShutdown();
        return rc;
    }
    if (cmd == L"play") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少文件路径\n"); return 1; }
        InputInit();
        int rc = DoPlay(pos[1], args.GetInt(L"--loop", 1));
        InputShutdown();
        return rc;
    }
    if (cmd == L"dump") {
        if (pos.size() < 2) { wprintf(L"ERR 缺少文件路径\n"); return 1; }
        return DoDump(pos[1]);
    }
    if (cmd == L"stop") return DoStop();

    // ---------- 嘴 ----------
    if (cmd == L"voices") {
        auto list = MouthListVoices();
        if (g_opt.json) {
            std::wstring s = L"{\"ok\":true,\"voices\":[";
            for (size_t i = 0; i < list.size(); ++i) {
                if (i) s += L",";
                s += L"{\"name\":\"" + FromUtf8(JsonEscape(ToUtf8(list[i].name))) + L"\"";
                s += L",\"lang\":\"" + FromUtf8(JsonEscape(ToUtf8(list[i].language))) + L"\"}";
            }
            s += L"]}\n";
            wprintf(L"%ls", s.c_str());
        } else {
            for (const auto& v : list)
                wprintf(L"%ls\t%ls\n", v.name.c_str(), v.language.c_str());
        }
        return list.empty() ? 3 : 0;
    }
    if (cmd == L"say" || cmd == L"speak") {
        std::wstring text;
        for (size_t i = 1; i < pos.size(); ++i) {
            if (!text.empty()) text += L" ";
            text += pos[i];
        }
        std::wstring textFile = args.Get(L"--text-file");
        if (!textFile.empty()) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, textFile.c_str(), L"rb") == 0 && f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                std::vector<char> buf(size_t(sz > 0 ? sz : 0) + 1, 0);
                size_t got = sz > 0 ? fread(buf.data(), 1, size_t(sz), f) : 0;
                fclose(f);
                text = FromUtf8(std::string(buf.data(), got));
            } else {
                wprintf(L"ERR 读不到文本文件: %ls\n", textFile.c_str());
                return 2;
            }
        }
        int rc = MouthSpeak(text,
                            args.Get(L"--voice"),
                            args.GetInt(L"--rate", 0),
                            args.GetInt(L"--volume", 100),
                            args.Get(L"--wav"),
                            args.Has(L"--async"));
        return rc;
    }

    wprintf(L"未知命令: %ls\n\n", cmd.c_str());
    PrintUsage();
    return 1;
}
