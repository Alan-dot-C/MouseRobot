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
#include "guard.h"
#include "doctor.h"
#include "gui.h"
#include "schema.h"
#include "uia.h"
#include "clip.h"

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>

#include <winrt/base.h>

// ------------------------------ 参数解析辅助 ------------------------------

// 这些选项后面跟一个值，解析"位置参数"时要连同值一起跳过。
// 清单不在这里维护，统一从 schema 表里查，避免四处各写一份。
static bool IsValueFlag(const std::wstring& s) {
    return OptTakesValue(s);
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
static std::wstring SaveFailureShot();

// ------------------------------ 状态文件 ------------------------------
//
// 状态文件按 key 隔离：两个会话同时使用时不会再互相把 delta 基准写坏。
// --region 用它自己的 key，避免把整屏的基准覆盖成一小块。

static std::wstring ResolveStateKey(const ArgList& args, const RectI& region) {
    std::wstring key = args.Get(L"--state-key");
    if (!key.empty()) return key;
    if (!region.empty()) {
        wchar_t buf[96] = {};
        swprintf_s(buf, L"region_%d_%d_%d_%d", region.x, region.y, region.w, region.h);
        return buf;
    }
    return L"default";
}

// ------------------------------ see ------------------------------

// 把命令行开关翻译成一次识别请求。
// 默认 auto：中文走 1x，拉丁/数字行再跑一路英文；--fast 只跑中文 1x。
static OcrRequest BuildOcrRequest(const ArgList& args, const RectI& region) {
    OcrRequest req;
    req.region = region;

    std::wstring sv = ToUpper(args.Get(L"--scale"));
    if (sv == L"1") req.scale = 1;
    else if (sv == L"2") req.scale = 2;
    else if (sv == L"3") req.scale = 3;
    else req.scale = 0;

    if (args.Has(L"--fast")) req.scale = 1;

    req.lang = args.Get(L"--lang", L"auto");
    if (req.lang.empty()) req.lang = L"auto";
    req.padPx = args.GetInt(L"--pad", 8);
    req.merge = !args.Has(L"--fast") && req.scale == 0;
    req.secondLook = args.Has(L"--best");
    return req;
}

static int CmdSee(const ArgList& args) {
    RectI region{};
    std::wstring regionStr = args.Get(L"--region");
    if (!regionStr.empty() && !ParseRect(regionStr, region)) {
        wprintf(L"ERR --region 需要 \"x,y,w,h\" 格式\n");
        return 1;
    }
    const std::wstring stateKey = ResolveStateKey(args, region);

    // --changed 走图像指纹，不需要 OCR，又快又稳。
    // 早先用 OCR 文本比对，OCR 逐帧有小抖动，会误报 +4/-4。
    if (args.Has(L"--changed")) {
        // Coarse grid difference. A single hash bit tells you that something
        // moved; the grid tells you where. 48x30 cells over the whole screen
        // is 1440 bytes of state and localises the change well enough to act.
        const int GW = 48, GH = 30;
        std::vector<uint8_t> now;
        if (!EyeCoarseGrid(GW, GH, now, region)) {
            wprintf(L"ERR 截屏失败\n");
            return 3;
        }

        std::wstring gridPath = StateFilePath(L"last_grid", stateKey);

        std::vector<uint8_t> prev;
        bool havePrev = false;
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, gridPath.c_str(), L"rb") == 0 && f) {
                std::string hex;
                char buf[4096];
                size_t got = fread(buf, 1, sizeof(buf) - 1, f);
                hex.assign(buf, got);
                fclose(f);
                if (hex.size() == size_t(GW) * GH * 2) {
                    prev.resize(size_t(GW) * GH);
                    for (size_t i = 0; i < prev.size(); ++i) {
                        char b2[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
                        prev[i] = (uint8_t)strtoul(b2, nullptr, 16);
                    }
                    havePrev = true;
                }
            }
        }

        RECT scrRect{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (!region.empty()) scrRect = RECT{ region.x, region.y, region.right(), region.bottom() };
        int sw = scrRect.right - scrRect.left, sh = scrRect.bottom - scrRect.top;
        int cw = (sw + GW - 1) / GW, ch = (sh + GH - 1) / GH;

        const int cellThreshold = args.GetInt(L"--threshold", 12);
        int changedCells = 0;
        int minX = GW, minY = GH, maxX = -1, maxY = -1;
        for (int y = 0; y < GH; ++y) {
            for (int x = 0; x < GW; ++x) {
                size_t i = size_t(y) * GW + x;
                int diff = havePrev ? std::abs(int(now[i]) - int(prev[i])) : 255;
                if (diff > cellThreshold) {
                    ++changedCells;
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (x > maxX) maxX = x;
                    if (y > maxY) maxY = y;
                }
            }
        }
        bool changed = !havePrev || changedCells > 0;
        LogV(L"感知", 3009, L"感知", L"屏幕指纹",
             L"changed=%d cells=%d firstFrame=%d", changed ? 1 : 0, changedCells, havePrev ? 0 : 1);

        // Save the new grid for next time.
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, gridPath.c_str(), L"wb") == 0 && f) {
                std::string hex;
                hex.reserve(now.size() * 2);
                const char* digits = "0123456789abcdef";
                for (uint8_t v : now) { hex += digits[v >> 4]; hex += digits[v & 0xF]; }
                fwrite(hex.data(), 1, hex.size(), f);
                fclose(f);
            }
        }

        if (g_opt.json) {
            JsonDoc d;
            d.Bool(L"ok", true).Bool(L"changed", changed).Int(L"cells", changedCells);
            if (changed) {
                wchar_t regionJson[128] = {};
                swprintf_s(regionJson, L"{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                           scrRect.left + minX * cw, scrRect.top + minY * ch,
                           (maxX - minX + 1) * cw, (maxY - minY + 1) * ch);
                d.Raw(L"region", regionJson);
            }
            d.Bool(L"firstFrame", !havePrev);
            wprintf(L"%ls\n", d.Emit().c_str());
        } else {
            if (changed) {
                // Cell size rounds up, so the box can overshoot the captured
                // region. Clamp it, otherwise the reported area is larger than
                // the area that was actually looked at.
                int bx = scrRect.left + minX * cw;
                int by = scrRect.top + minY * ch;
                int bw = (maxX - minX + 1) * cw;
                int bh = (maxY - minY + 1) * ch;
                if (bx + bw > scrRect.right) bw = scrRect.right - bx;
                if (by + bh > scrRect.bottom) bh = scrRect.bottom - by;
                wprintf(L"CHANGED\tx=%d y=%d w=%d h=%d cells=%d\n", bx, by, bw, bh, changedCells);
            } else {
                wprintf(L"UNCHANGED\n");
            }
        }
        return 0;
    }

    if (!EyeInit()) {
        if (g_opt.json) {
            JsonDoc d;
            d.Bool(L"ok", false).Str(L"error", L"no OCR engine");
            wprintf(L"%ls\n", d.Emit().c_str());
        } else {
            wprintf(L"ERR 没有可用的 OCR 引擎（可能没装中文 OCR 语言包）\n");
        }
        LogE(L"感知", 3006, L"异常", L"OCR 引擎不可用", L"reason=no-engine");
        return 3;
    }

    OcrRequest oreq = BuildOcrRequest(args, region);

    // --wait：轮询直到文字出现
    std::wstring waitText = args.Get(L"--wait");
    int timeoutMs = args.GetInt(L"--timeout", 5000);

    if (!waitText.empty()) {
        DWORD start = GetTickCount();
        for (;;) {
            OcrResult r = EyeRecognize(oreq);
            if (!r.ok) {
                wprintf(L"ERR %ls\n", r.error.c_str());
                return 3;
            }
            auto hits = SieveFind(r.lines, waitText, 1);
            if (!hits.empty()) {
                const auto& h = hits[0];
                if (g_opt.json) {
                    JsonDoc d;
                    d.Bool(L"ok", true).Bool(L"found", true);
                    d.Str(L"text", h.text);
                    d.Int(L"x", h.box.x).Int(L"y", h.box.y).Int(L"w", h.box.w).Int(L"h", h.box.h);
                    d.Dbl(L"score", h.score);
                    d.Raw(L"elapsedMs", std::to_wstring(GetTickCount() - start));
                    wprintf(L"%ls\n", d.Emit().c_str());
                } else {
                    wprintf(L"FOUND\t%ls\t%d\t%d\t%d\t%d\t%.2f\t%lums\n",
                            h.text.c_str(), h.box.x, h.box.y, h.box.w, h.box.h, h.score,
                            (unsigned long)(GetTickCount() - start));
                }
                LogI(L"感知", 3011, L"感知", L"等待文字出现", L"%ls ms=%lu", LogRedactText(waitText).c_str(), (unsigned long)(GetTickCount() - start));
                return 0;
            }
            if ((int)(GetTickCount() - start) > timeoutMs) {
                if (g_opt.json)
                    wprintf(L"{\"ok\":false,\"found\":false,\"error\":\"timeout\",\"elapsedMs\":%lu}\n",
                            (unsigned long)(GetTickCount() - start));
                else
                    wprintf(L"NOTFOUND\t超时 %dms\n", timeoutMs);
                LogW(L"感知", 3010, L"感知", L"等待文字超时", L"%ls timeout=%d", LogRedactText(waitText).c_str(), timeoutMs);
                return 1;
            }
            if (ShouldAbort()) {
                LogW(L"动作", 4015, L"停止", L"动作被中止", L"stage=wait %ls",
                     LogRedactText(waitText).c_str());
                if (g_opt.json) {
                    JsonDoc d;
                    d.Bool(L"ok", false).Str(L"error", L"aborted");
                    wprintf(L"%ls\n", d.Emit().c_str());
                } else {
                    wprintf(L"ABORTED\n");
                }
                return 4;
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
    OcrResult r = EyeRecognize(oreq);
    DWORD msCapture = GetTickCount() - tCapture;
    if (!r.ok) {
        LogE(L"感知", r.timedOut ? 3007 : 3008, L"异常",
             r.timedOut ? L"OCR 超时" : L"截屏失败",
             L"err=%ls ms=%lu", r.error.c_str(), (unsigned long)msCapture);
        std::wstring shot = SaveFailureShot();
        if (!shot.empty()) Say(L"现场截图: %ls", shot.c_str());
        if (g_opt.json) {
            JsonDoc d;
            d.Bool(L"ok", false).Str(L"error", r.error);
            wprintf(L"%ls\n", d.Emit().c_str());
        } else {
            wprintf(L"ERR %ls\n", r.error.c_str());
        }
        return 3;
    }
    LogV(L"感知", 3000, L"感知", L"截屏开始", L"x=%d y=%d w=%d h=%d scale=%d", region.x, region.y, region.w, region.h, r.scale);
    LogV(L"感知", 3002, L"感知", L"OCR 完成", L"raw=%zu ms=%lu", r.lines.size(), (unsigned long)msCapture);
    {
        // 分路统计：出问题时第一眼要能看出英文通道有没有跑、跑出多少行
        int zhLines = 0, enLines = 0;
        for (const auto& l : r.lines) {
            if (l.src.rfind(L"en", 0) == 0) ++enLines;
            else ++zhLines;
        }
        LogI(L"感知", 3012, L"感知", L"OCR 分路统计",
             L"zh=%d en=%d raw=%zu ms=%lu", zhLines, enLines, r.lines.size(), (unsigned long)msCapture);
    }

    // --raw：不做筛子，直接吐原始行
    if (args.Has(L"--raw")) {
        if (g_opt.json) {
            std::wstring linesJson = L"[";
            for (size_t i = 0; i < r.lines.size(); ++i) {
                if (i) linesJson += L",";
                linesJson += L"{\"text\":\"" + JsonStr(r.lines[i].text) + L"\"";
                linesJson += L",\"x\":" + std::to_wstring(r.lines[i].box.x);
                linesJson += L",\"y\":" + std::to_wstring(r.lines[i].box.y);
                linesJson += L",\"w\":" + std::to_wstring(r.lines[i].box.w);
                linesJson += L",\"h\":" + std::to_wstring(r.lines[i].box.h) + L"}";
            }
            linesJson += L"]";
            JsonDoc d;
            d.Bool(L"ok", true).Bool(L"raw", true).Int(L"count", (long long)r.lines.size());
            d.Raw(L"lines", linesJson);
            wprintf(L"%ls\n", d.Emit().c_str());
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
            std::wstring hitsJson = L"[";
            for (size_t i = 0; i < hits.size(); ++i) {
                if (i) hitsJson += L",";
                wchar_t score[32] = {};
                swprintf_s(score, L"%.2f", hits[i].score);
                hitsJson += L"{\"text\":\"" + JsonStr(hits[i].text) + L"\"";
                hitsJson += L",\"x\":" + std::to_wstring(hits[i].box.x);
                hitsJson += L",\"y\":" + std::to_wstring(hits[i].box.y);
                hitsJson += L",\"w\":" + std::to_wstring(hits[i].box.w);
                hitsJson += L",\"h\":" + std::to_wstring(hits[i].box.h);
                hitsJson += L",\"score\":" + std::wstring(score) + L"}";
            }
            hitsJson += L"]";
            JsonDoc d;
            d.Bool(L"ok", true).Int(L"count", (long long)hits.size());
            d.Raw(L"hits", hitsJson);
            wprintf(L"%ls\n", d.Emit().c_str());
        } else {
            if (hits.empty()) {
                wprintf(L"NOTFOUND\t%ls\n", findText.c_str());
                LogV(L"感知", 3005, L"感知", L"查找未命中", L"%ls", LogRedactText(findText).c_str());
                return 1;
            }
            LogI(L"感知", 3004, L"感知", L"查找命中", L"%ls hits=%zu best=%.2f", LogRedactText(findText).c_str(), hits.size(), hits[0].score);
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
    so.stateKey = stateKey;

    SieveResult sr = SieveRun(r.lines, so);
    LogV(L"感知", 3003, L"感知", L"筛子完成", L"blocks=%zu raw=%zu", sr.blocks.size(), r.lines.size());

    // --ui：把标准控件的树也带上。OCR 认的是像素，控件树认的是名字，
    // 两者叠起来，模型既看得到自绘界面，也看得到真正的按钮名。
    std::vector<UiNode> uiNodes;
    if (args.Has(L"--ui") && UiInit()) {
        DWORD t0 = GetTickCount();
        uiNodes = UiTree(nullptr,
                         args.GetInt(L"--ui-depth", 8),
                         args.GetInt(L"--ui-max", 300),
                         args.GetInt(L"--ui-timeout", 400));
        LogI(L"感知", 3013, L"感知", L"UI 控件树",
             L"nodes=%zu ms=%lu", uiNodes.size(), (unsigned long)(GetTickCount() - t0));
    }

    POINT cur = GetCursorPosNow();
    std::wstring focus = GetForegroundTitle();
    RectI scr = EyeScreenRect();

    if (g_opt.json) {
        JsonDoc d;
        d.Bool(L"ok", true);
        {
            wchar_t buf[64] = {};
            swprintf_s(buf, L"{\"w\":%d,\"h\":%d}", scr.w, scr.h);
            d.Raw(L"screen", buf);
        }
        {
            wchar_t buf[64] = {};
            swprintf_s(buf, L"{\"x\":%d,\"y\":%d}", cur.x, cur.y);
            d.Raw(L"cursor", buf);
        }
        d.Str(L"focus", focus);
        d.Int(L"rawLines", (long long)r.lines.size());
        {
            wchar_t buf[128] = {};
            swprintf_s(buf, L"{\"changed\":%ls,\"added\":%d,\"removed\":%d}",
                       sr.changed ? L"true" : L"false", sr.newLines, sr.goneLines);
            d.Raw(L"delta", buf);
        }

        std::wstring blocks = L"[";
        bool firstBlock = true;
        for (const auto& b : sr.blocks) {
            if (!firstBlock) blocks += L",";
            firstBlock = false;
            blocks += L"{\"kind\":\"" + JsonStr(b.kind) + L"\"";
            blocks += L",\"window\":\"" + JsonStr(b.window) + L"\"";
            blocks += L",\"box\":{\"x\":" + std::to_wstring(b.box.x) + L",\"y\":" + std::to_wstring(b.box.y);
            blocks += L",\"w\":" + std::to_wstring(b.box.w) + L",\"h\":" + std::to_wstring(b.box.h) + L"}";
            blocks += L",\"items\":[";
            for (size_t i = 0; i < b.items.size(); ++i) {
                if (i) blocks += L",";
                blocks += L"{\"text\":\"" + JsonStr(b.items[i].text) + L"\"";
                blocks += L",\"x\":" + std::to_wstring(b.items[i].box.x);
                blocks += L",\"y\":" + std::to_wstring(b.items[i].box.y);
                blocks += L",\"w\":" + std::to_wstring(b.items[i].box.w);
                blocks += L",\"h\":" + std::to_wstring(b.items[i].box.h) + L"}";
            }
            blocks += L"]}";
        }
        blocks += L"]";
        d.Raw(L"blocks", blocks);

        if (!uiNodes.empty()) {
            std::wstring ui = L"[";
            for (size_t i = 0; i < uiNodes.size(); ++i) {
                if (i) ui += L",";
                ui += L"{\"role\":\"" + JsonStr(uiNodes[i].role) + L"\"";
                ui += L",\"name\":\"" + JsonStr(uiNodes[i].name) + L"\"";
                ui += L",\"value\":\"" + JsonStr(uiNodes[i].value) + L"\"";
                ui += L",\"x\":" + std::to_wstring(uiNodes[i].rect.x);
                ui += L",\"y\":" + std::to_wstring(uiNodes[i].rect.y);
                ui += L",\"w\":" + std::to_wstring(uiNodes[i].rect.w);
                ui += L",\"h\":" + std::to_wstring(uiNodes[i].rect.h);
                ui += std::wstring(L",\"invokable\":") + (uiNodes[i].invokable ? L"true" : L"false");
                ui += L"}";
            }
            ui += L"]";
            d.Raw(L"ui", ui);
        }

        wprintf(L"%ls\n", d.Emit().c_str());
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

    for (const auto& n : uiNodes) {
        wprintf(L"[ui:%ls] %ls @%d,%d %dx%d%ls%ls\n",
                n.role.c_str(), n.name.c_str(), n.rect.x, n.rect.y, n.rect.w, n.rect.h,
                n.invokable ? L" invoke" : L"",
                n.value.empty() ? L"" : (L" value=" + n.value).c_str());
    }

    // Output grouped by owning window. A window header is printed once, then
    // its elements, then its body paragraphs. This is the structure the OS
    // actually has, rather than something inferred from pixel distances.
    {
        // distinct windows in Z order
        std::vector<int> zs;
        for (const auto& b : sr.blocks) {
            if (std::find(zs.begin(), zs.end(), b.winZ) == zs.end()) zs.push_back(b.winZ);
        }
        std::sort(zs.begin(), zs.end(), [](int a, int b) {
            return (a < 0 ? 100000 : a) < (b < 0 ? 100000 : b);
        });

        int paraCount = 0;
        for (int z : zs) {
            std::wstring title;
            for (const auto& b : sr.blocks) {
                if (b.winZ == z && !b.window.empty()) { title = b.window; break; }
            }
            // Skip windows that have no recognised text, otherwise the output
            // is full of empty headers for windows hidden behind others.
            bool hasAny = false;
            for (const auto& b : sr.blocks) if (b.winZ == z && !b.items.empty()) { hasAny = true; break; }
            if (!hasAny) continue;

            if (title.empty()) wprintf(L"[win] (桌面或不属于任何窗口)\n");
            else wprintf(L"[win] %ls\n", title.c_str());

            for (const auto& b : sr.blocks) {
                if (b.winZ != z || b.kind == L"content") continue;
                for (const auto& it : b.items) {
                    wprintf(L"[%ls] %ls @%d,%d %dx%d\n",
                            b.kind.c_str(), it.text.c_str(),
                            it.box.x, it.box.y, it.box.w, it.box.h);
                }
            }

            SieveResult sub;
            for (const auto& b : sr.blocks)
                if (b.winZ == z && b.kind == L"content") sub.blocks.push_back(b);
            std::vector<LineItem> paras = BuildContentParagraphs(sub, 140);
            for (const auto& p : paras) {
                if (maxParas > 0 && paraCount >= maxParas) break;
                wprintf(L"[content] %ls @%d,%d %dx%d\n",
                        p.text.c_str(), p.box.x, p.box.y, p.box.w, p.box.h);
                ++paraCount;
            }
        }
    }
    if (!sr.windows.empty()) {
        wprintf(L"[windows] %zu 个:\n", sr.windows.size());
        for (const auto& w : sr.windows) {
            wprintf(L"  z=%d %ls @%d,%d %dx%d\n",
                    w.z, w.title.c_str(), w.rect.x, w.rect.y, w.rect.w, w.rect.h);
        }
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

// 一次命中的统一表示：不管来自 UI Automation 还是 OCR
struct ClickHit {
    std::wstring text;
    RectI        box;
    double       score = 0;
    std::wstring via = L"ocr";
    bool         invokable = false;
    bool         haveUi = false;
    UiNode       ui;
};

// 找到屏幕上的文字并点它。这是"眼"和"手"的接缝：
// AI 不需要知道坐标，只要说要点什么。
//
// 策略链：先 UI Automation（标准控件有名字和边界，不会认错字），
// 命中不了再回落到 OCR。--via ocr 可以强制走 OCR。
static int CmdClickText(const ArgList& args) {
    auto pos = args.Positional();
    if (pos.size() < 2) { wprintf(L"ERR 用法: click-text <文字> [--index N] [--timeout ms]\n"); return 1; }
    std::wstring needle;
    for (size_t i = 1; i < pos.size(); ++i) {
        if (!needle.empty()) needle += L" ";
        needle += pos[i];
    }
    if (!EyeInit()) { wprintf(L"ERR 没有可用的 OCR 引擎\n"); return 3; }

    const std::wstring via = ToUpper(args.Get(L"--via", L"auto"));
    OcrRequest oreq = BuildOcrRequest(args, RectI{});
    int timeout = args.GetInt(L"--timeout", 0);
    int wanted = args.GetInt(L"--index", 0);
    DWORD start = GetTickCount();

    auto perform = [&](const ClickHit& h) -> int {
        // Danger guard. A tool that can click anything needs a brake.
        std::wstring dangerWord;
        bool dangerous = GuardIsDangerous(h.text, dangerWord);
        bool confirmed = args.Has(L"--yes");
        if (dangerous && !confirmed) {
            if (g_opt.dryRun) {
                wprintf(L"[dry-run] 注意：目标含危险词「%ls」，正式执行会被拦截，需要加 --yes\n",
                        dangerWord.c_str());
            } else {
                wprintf(L"BLOCKED\t目标含危险词「%ls」\n", dangerWord.c_str());
                wprintf(L"         目标文字：%ls\n", h.text.c_str());
                wprintf(L"         确认要执行请加 --yes\n");
                LogW(L"动作", 4013, L"异常", L"危险动作被拦截",
                     L"text=%ls word=%ls x=%d y=%d via=%ls",
                     h.text.c_str(), dangerWord.c_str(), h.box.cx(), h.box.cy(), h.via.c_str());
                return 1;
            }
        }

        // 先记命中，dry-run 也留痕：出问题时"当时选中了什么"必须查得到
        LogI(L"动作", 4014, L"动作", L"按文字点击命中",
             L"%ls via=%ls x=%d y=%d score=%.2f dryRun=%d",
             LogRedactText(h.text).c_str(), h.via.c_str(), h.box.cx(), h.box.cy(), h.score,
             g_opt.dryRun ? 1 : 0);

        if (g_opt.dryRun) {
            std::wstring expectText = args.Get(L"--expect");
            std::wstring expectGone = args.Get(L"--expect-gone");
            if (!expectText.empty() || !expectGone.empty()) {
                wprintf(L"[dry-run] 会验证：出现「%ls」 消失「%ls」\n",
                        expectText.c_str(), expectGone.c_str());
            }
            if (g_opt.json) {
                JsonDoc d;
                d.Bool(L"ok", true).Bool(L"dryRun", true);
                d.Str(L"text", h.text);
                d.Int(L"x", h.box.cx()).Int(L"y", h.box.cy());
                d.Dbl(L"score", h.score);
                d.Bool(L"dangerous", dangerous);
                d.Str(L"via", h.via);
                wprintf(L"%ls\n", d.Emit().c_str());
            } else {
                wprintf(L"[dry-run] 会点击「%ls」@%d,%d (匹配度 %.2f, %ls)\n",
                        h.text.c_str(), h.box.cx(), h.box.cy(), h.score, h.via.c_str());
            }
            return 0;
        }

        GuardAnnounce(std::wstring(L"点击 ") + h.text);

        if (h.invokable && h.haveUi) {
            // 标准按钮优先用 InvokePattern：不用移动鼠标，也不受坐标影响
            if (!UiInvoke(h.ui)) {
                InputInit();
                ClickAt(h.box.cx(), h.box.cy(), args.Has(L"--right"));
                InputShutdown();
            }
        } else {
            InputInit();
            ClickAt(h.box.cx(), h.box.cy(), args.Has(L"--right"));
            InputShutdown();
        }

        if (g_opt.json) {
            JsonDoc d;
            d.Bool(L"ok", true);
            d.Str(L"text", h.text);
            d.Int(L"x", h.box.cx()).Int(L"y", h.box.cy());
            d.Int(L"w", h.box.w).Int(L"h", h.box.h);
            d.Dbl(L"score", h.score);
            d.Str(L"via", h.via);
            wprintf(L"%ls\n", d.Emit().c_str());
        } else {
            wprintf(L"CLICKED\t%ls\t%d\t%d\t%.2f\t%ls\n",
                    h.text.c_str(), h.box.cx(), h.box.cy(), h.score, h.via.c_str());
        }

        // Optional post-action verification. This is what turns an open
        // loop into a closed one: the caller learns whether it worked.
        std::wstring expectText2 = args.Get(L"--expect");
        std::wstring expectGone2 = args.Get(L"--expect-gone");
        if (!expectText2.empty() || !expectGone2.empty()) {
            int vt = args.GetInt(L"--expect-timeout", 3000);
            int rc = GuardVerify(expectText2, expectGone2, vt, 0);
            if (rc == 4) {
                wprintf(L"ABORTED\n");
                return 4;
            }
            if (rc != 0) {
                std::wstring shot = SaveFailureShot();
                wprintf(L"VERIFY-FAILED\texpect=%ls expectGone=%ls timeout=%dms\n",
                        expectText2.c_str(), expectGone2.c_str(), vt);
                if (!shot.empty()) Say(L"现场截图: %ls", shot.c_str());
                return 5;
            }
            wprintf(L"VERIFIED\n");
        }
        return 0;
    };

    // 1) UI Automation：标准控件的名字是准的，优先用它
    if (via != L"OCR" && UiInit()) {
        auto nodes = UiFind(needle, 300, 400);
        if (!nodes.empty()) {
            // 跳过不可见/不可用的，除非只剩它们
            std::vector<UiNode> usable;
            for (auto& n : nodes) if (n.enabled && !n.offscreen) usable.push_back(n);
            if (usable.empty()) usable = nodes;

            int idx = wanted;
            if (idx < 0 || idx >= int(usable.size())) idx = 0;
            const UiNode& n = usable[idx];

            ClickHit h;
            h.text = n.name;
            h.box = n.rect;
            h.score = 1.0;
            h.via = L"ui";
            h.invokable = n.invokable;
            h.haveUi = true;
            h.ui = n;
            if (h.box.w > 0 && h.box.h > 0) return perform(h);
        }
    }

    // 2) OCR
    for (;;) {
        OcrResult r = EyeRecognize(oreq);
        if (!r.ok) { wprintf(L"ERR %ls\n", r.error.c_str()); return 3; }

        auto hits = SieveFind(r.lines, needle, 8);
        if (!hits.empty()) {
            int idx = wanted;
            if (idx < 0 || idx >= int(hits.size())) idx = 0;
            const auto& h = hits[idx];

            ClickHit ch;
            ch.text = h.text;
            ch.box = h.box;
            ch.score = h.score;
            ch.via = L"ocr";
            return perform(ch);
        }

        if (timeout <= 0 || int(GetTickCount() - start) > timeout) {
            LogV(L"感知", 3005, L"感知", L"查找未命中",
                 L"%ls ms=%lu", LogRedactText(needle).c_str(),
                 (unsigned long)(GetTickCount() - start));
            if (g_opt.json) {
                JsonDoc d;
                d.Bool(L"ok", false).Str(L"error", L"not found");
                wprintf(L"%ls\n", d.Emit().c_str());
            } else {
                wprintf(L"NOTFOUND\t%ls\n", needle.c_str());
            }
            return 1;
        }
        Sleep(300);
    }
}

// ------------------------------ ui：控件树调试 ------------------------------

static int CmdUi(const ArgList& args) {
    if (!UiInit()) {
        wprintf(L"ERR UI Automation 不可用（COM 初始化失败）\n");
        return 3;
    }
    int depth = args.GetInt(L"--depth", 8);
    int maxNodes = args.GetInt(L"--max", 800);
    std::wstring filter = args.Get(L"--text");

    auto nodes = UiTree(nullptr, depth, maxNodes, 1500);
    LogV(L"感知", 3013, L"感知", L"UI 控件树", L"nodes=%zu ms=0", nodes.size());
    if (!filter.empty()) {
        std::vector<UiNode> keep;
        for (auto& n : nodes) if (n.name.find(filter) != std::wstring::npos) keep.push_back(n);
        nodes.swap(keep);
    }

    if (g_opt.json) {
        std::wstring arr = L"[";
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (i) arr += L",";
            arr += L"{\"role\":\"" + JsonStr(nodes[i].role) + L"\"";
            arr += L",\"name\":\"" + JsonStr(nodes[i].name) + L"\"";
            arr += L",\"value\":\"" + JsonStr(nodes[i].value) + L"\"";
            arr += L",\"x\":" + std::to_wstring(nodes[i].rect.x);
            arr += L",\"y\":" + std::to_wstring(nodes[i].rect.y);
            arr += L",\"w\":" + std::to_wstring(nodes[i].rect.w);
            arr += L",\"h\":" + std::to_wstring(nodes[i].rect.h);
            arr += std::wstring(L",\"enabled\":") + (nodes[i].enabled ? L"true" : L"false");
            arr += std::wstring(L",\"invokable\":") + (nodes[i].invokable ? L"true" : L"false");
            arr += L"}";
        }
        arr += L"]";
        JsonDoc d;
        d.Bool(L"ok", true).Int(L"count", (long long)nodes.size());
        d.Raw(L"nodes", arr);
        wprintf(L"%ls\n", d.Emit().c_str());
    } else {
        for (const auto& n : nodes) {
            wprintf(L"[%ls] %ls @%d,%d %dx%d%ls%ls\n",
                    n.role.c_str(), n.name.c_str(), n.rect.x, n.rect.y, n.rect.w, n.rect.h,
                    n.invokable ? L" invoke" : L"",
                    n.value.empty() ? L"" : (L" value=" + n.value).c_str());
        }
        if (nodes.empty()) {
            wprintf(L"（当前窗口没有可读的控件）\n");
            return 1;
        }
    }
    return 0;
}

// ------------------------------ read-text ------------------------------

static int CmdReadText(const ArgList& args) {
    std::wstring window = args.Get(L"--window");
    std::wstring method = ToUpper(args.Get(L"--method", L"auto"));
    int timeoutMs = args.GetInt(L"--timeout", 2000);
    if (method.empty()) method = L"AUTO";

    if (!window.empty()) {
        InputInit();
        FocusWindow(window, 800);
        InputShutdown();
    }

    if (!UiInit()) {
        if (method != L"COPY") {
            wprintf(L"ERR UI Automation 不可用；可以试 --method copy\n");
            return 3;
        }
    }

    std::wstring text;
    std::wstring via;

    if (method != L"COPY") {
        if (UiFocusedText(text) && !text.empty()) {
            via = L"uia-focus";
        } else {
            // 前台窗口文档里的文字
            auto nodes = UiTree(nullptr, 8, 400, 800);
            std::wstring buf;
            for (const auto& n : nodes) {
                if (n.role != L"document" && n.role != L"text" && n.role != L"edit") continue;
                std::wstring t;
                if (UiGetText(n, t) && !t.empty()) { buf += t; buf += L"\n"; }
            }
            if (!buf.empty()) { text = buf; via = L"uia-window"; }
        }
    }

    // 只有显式 --method copy 才动剪贴板：它会全选，是有副作用的
    if (text.empty() && method == L"COPY") {
        ClipSnapshot snap = ClipTake();
        DWORD seq0 = GetClipboardSequenceNumber();

        InputInit();
        PressKeyCombo(L"CTRL+A");
        Sleep(80);
        PressKeyCombo(L"CTRL+C");
        InputShutdown();

        DWORD t0 = GetTickCount();
        bool changed = false;
        while ((int)(GetTickCount() - t0) < timeoutMs) {
            if (GetClipboardSequenceNumber() != seq0) { changed = true; break; }
            Sleep(30);
        }
        if (changed) ClipGetText(text);
        ClipRestore(snap);
        if (!text.empty()) via = L"copy";
    }

    if (text.empty()) {
        if (g_opt.json) {
            JsonDoc d;
            d.Bool(L"ok", false).Str(L"error", L"没有读到文字");
            wprintf(L"%ls\n", d.Emit().c_str());
        } else {
            wprintf(L"ERR 没有读到文字（该控件可能不支持文本读取；试 --method copy）\n");
        }
        return 1;
    }

    LogI(L"感知", 3014, L"感知", L"读取文字", L"via=%ls chars=%zu", via.c_str(), text.size());

    if (g_opt.json) {
        JsonDoc d;
        d.Bool(L"ok", true).Int(L"chars", (long long)text.size());
        d.Str(L"text", text).Str(L"via", via);
        wprintf(L"%ls\n", d.Emit().c_str());
    } else {
        wprintf(L"%ls\n", text.c_str());
    }
    return 0;
}

// 日志里的命令行脱敏：--log-text mask/off 时不要把用户输入的文字原样落盘。
// 以前 argv 是无条件写进 8000 事件的，和 README 承诺的脱敏对不上。
static std::wstring RedactArgv(const std::vector<std::wstring>& raw) {
    if (raw.empty()) return L"";
    if (g_log.textMode == LOG_TEXT_FULL) {
        std::wstring s;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (i) s += L" ";
            s += raw[i];
        }
        return s;
    }
    std::wstring s = raw[0];
    int hidden = 0;
    for (size_t i = 1; i < raw.size(); ++i) {
        if (!raw[i].empty() && raw[i][0] == L'-') { s += L" "; s += raw[i]; }
        else ++hidden;
    }
    if (hidden > 0) {
        s += L" ***";
        if (hidden > 1) s += L" x" + std::to_wstring(hidden);
    }
    return s;
}

// 出问题的时候留一张现场图，比事后问"当时屏幕上是什么"有用得多
static std::wstring SaveFailureShot() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[80] = {};
    swprintf_s(name, L"fail_%04d%02d%02d_%02d%02d%02d.png",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring path = HandeyeStateDir() + L"\\" + name;
    return EyeSavePng(path, RectI{}) ? path : L"";
}

// ------------------------------ 能力自述 ------------------------------

// 能力清单和帮助都从 schema.cpp 的同一张表生成。
// 以前 IsValueFlag / PrintUsage / PrintSchema 各写一份，对不上是必然的。
static void PrintSchema() { PrintSchemaFromTable(); }

// ------------------------------ 帮助 ------------------------------

static void PrintUsage() { PrintUsageFromTable(); }

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
    g_opt.announce = args.Has(L"--announce");
    g_opt.instantMove = args.Has(L"--instant");
    EyeSetTimeout(args.GetInt(L"--ocr-timeout", 20000));

    // 从进程启动就开始计时，--max-duration 才能覆盖"等屏幕"这种没有鼠标的阶段
    AbortInit();

    if (args.raw.empty()) {
        PrintUsage();
        LogShutdown();
        return 0;
    }

    std::wstring cmd = args.raw[0];
    for (wchar_t& c : cmd) c = towlower(c);

    // 会话开始：把完整命令行记下来，事后能复原"AI 到底调了什么"
    {
        std::wstring argvJoin = RedactArgv(args.raw);
        if (argvJoin.size() > 400) argvJoin = argvJoin.substr(0, 400) + L"…";
        wchar_t cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        LogI(L"会话", 8000, L"会话", L"会话开始", L"argv=\"%ls\" cwd=%ls",
             argvJoin.c_str(), cwd);
    }

    int rc = Dispatch(args, cmd);

    // 只会 Say() 的命令（act / click / type / say / stop ...）在 --json 模式下
    // 本来什么都不输出，警告也一起丢了。这里补一个收尾对象，把 ok/rc/warnings 交代清楚。
    {
        bool manualJson = (cmd == L"schema" || cmd == L"log" || cmd == L"doctor" ||
                           cmd == L"bench" || cmd == L"gui" || cmd == L"help" ||
                           cmd == L"--help" || cmd == L"-h");
        if (g_opt.json && !manualJson && !JsonWasEmitted()) {
            JsonDoc d;
            d.Bool(L"ok", rc == 0);
            if (rc != 0) d.Int(L"rc", rc);
            wprintf(L"%ls\n", d.Emit().c_str());
        }
    }

    {
        DWORD ms = GetTickCount() - LogStartTick();
        if (rc == 0)
            LogI(L"会话", 8001, L"会话", L"会话结束", L"rc=0 ms=%lu", (unsigned long)ms);
        else
            // A non-zero exit code is a normal outcome (not found, blocked,
            // verification failed), not a warning. Logging it as a warning
            // drowned the doctor output in noise.
            LogI(L"会话", 8001, L"会话", L"会话结束", L"rc=%d ms=%lu", rc, (unsigned long)ms);
    }
    LogShutdown();
    return rc;
}

static int Dispatch(const ArgList& args, const std::wstring& cmd) {
    if (cmd == L"help" || cmd == L"--help" || cmd == L"-h") { PrintUsage(); return 0; }
    if (cmd == L"schema") { PrintSchema(); return 0; }
    if (cmd == L"log")   return CmdLogQuery(args.raw);
    if (cmd == L"bench") return CmdBench(args.raw);
    if (cmd == L"doctor") return CmdDoctor(args.raw);
    if (cmd == L"gui") {
        // Hide the console the moment the window comes up, so double-clicking
        // a shortcut does not leave a black box behind the window.
        FreeConsole();
        return RunGui();
    }
    if (cmd == L"see") return CmdSee(args);
    if (cmd == L"click-text" || cmd == L"clicktext") return CmdClickText(args);
    if (cmd == L"ui") return CmdUi(args);
    if (cmd == L"read-text" || cmd == L"readtext") return CmdReadText(args);

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
            std::wstring listJson = L"[";
            for (size_t i = 0; i < list.size(); ++i) {
                if (i) listJson += L",";
                listJson += L"{\"name\":\"" + JsonStr(list[i].name) + L"\"";
                listJson += L",\"lang\":\"" + JsonStr(list[i].language) + L"\"}";
            }
            listJson += L"]";
            JsonDoc d;
            d.Bool(L"ok", true).Raw(L"voices", listJson);
            wprintf(L"%ls\n", d.Emit().c_str());
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
