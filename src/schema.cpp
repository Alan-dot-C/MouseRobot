// ============================================================================
//  schema.cpp —— 命令与选项表，以及由它生成的帮助 / 能力清单
// ============================================================================
#include "schema.h"
#include "common.h"

#include <cstdio>

// ------------------------------ 选项表 ------------------------------

static const OptSpec kGlobalOpts[] = {
    { L"--json",         false, L"输出单个 JSON 对象" },
    { L"--dry-run",      false, L"只说不做" },
    { L"--max-duration", true,  L"秒；超过就中止（含等待与验证）" },
    { L"--announce",     false, L"动作前先语音播报" },
    { L"--instant",      false, L"鼠标直接跳到位，不做缓动（机器用，快很多）" },
    { L"--log-dir",      true,  L"日志目录" },
    { L"--log-level",    true,  L"日志级别：严重/错误/警告/信息/详细" },
    { L"--log-text",     true,  L"文本落盘策略：full/mask/off" },
    { L"--no-log",       false, L"本次不写日志" },
    { L"--log-echo",     false, L"同时打到 stderr" },
    { nullptr, false, nullptr }
};

static const OptSpec kSeeOpts[] = {
    { L"--region",       true,  L"只识别这一块区域 x,y,w,h" },
    { L"--scale",        true,  L"送 OCR 前的放大倍数：auto(默认) / 1 / 2 / 3" },
    { L"--lang",         true,  L"识别引擎：auto(默认，按行选) / zh / en" },
    { L"--pad",          true,  L"裁切时四周留白像素（默认 8）" },
    { L"--fast",         false, L"只跑中文 1x，最快；轮询用这个" },
    { L"--best",         false, L"双引擎合并 + 可疑行二次识别，最准" },
    { L"--all",          false, L"不做显著性裁剪，输出全部" },
    { L"--max-lines",    true,  L"整体最多保留多少行（默认 40）" },
    { L"--max-chars",    true,  L"单行最长字符数（默认 120）" },
    { L"--max-paras",    true,  L"正文最多输出几段（默认 10）" },
    { L"--raw",          false, L"不做筛子，吐原始 OCR 行" },
    { L"--find",         true,  L"模糊查找，返回坐标与匹配度" },
    { L"--wait",         true,  L"轮询直到文字出现" },
    { L"--timeout",      true,  L"等待超时毫秒（默认 5000）" },
    { L"--changed",      false, L"只回答屏幕变了没有（图像指纹）" },
    { L"--threshold",    true,  L"--changed 的格子差阈值（默认 12）" },
    { L"--no-delta",     false, L"不与上一次比对" },
    { L"--show-delta",   false, L"显示新增/消失的行" },
    { L"--state-key",    true,  L"变化检测状态的键（多会话隔离用）" },
    { L"--save",         true,  L"同时保存截图" },
    { L"--ui",           false, L"叠加 UI Automation 控件树" },
    { L"--ui-depth",     true,  L"UI 树最大深度（默认 8）" },
    { L"--ui-max",       true,  L"UI 树最多节点数（默认 800）" },
    { L"--ui-timeout",   true,  L"UI 查询预算毫秒（默认 300）" },
    { L"--ocr-timeout",  true,  L"单次 OCR 超时毫秒（默认 20000）" },
    { nullptr, false, nullptr }
};

static const OptSpec kClickTextOpts[] = {
    { L"--index",        true,  L"第几个命中（默认 0）" },
    { L"--timeout",      true,  L"找不到就轮询，最多这么多毫秒" },
    { L"--right",        false, L"右键点击" },
    { L"--via",          true,  L"命中来源：auto(默认) / ui / ocr" },
    { L"--scale",        true,  L"OCR 放大倍数（auto 默认）" },
    { L"--yes",          false, L"放行含危险词的点击" },
    { L"--expect",       true,  L"点完确认这段文字出现" },
    { L"--expect-gone",  true,  L"点完确认这段文字消失" },
    { L"--expect-timeout", true, L"验证超时毫秒（默认 3000）" },
    { nullptr, false, nullptr }
};

static const OptSpec kClickOpts[] = {
    { L"--right", false, L"右键点击" },
    { nullptr, false, nullptr }
};

static const OptSpec kReadTextOpts[] = {
    { L"--window",  true, L"先切到这个窗口再读" },
    { L"--method",  true, L"auto(默认，UI Automation) / copy(全选复制)" },
    { L"--timeout", true, L"等剪贴板变化的毫秒（默认 2000）" },
    { nullptr, false, nullptr }
};

static const OptSpec kUiOpts[] = {
    { L"--depth", true, L"最大深度（默认 8）" },
    { L"--max",   true, L"最多节点数（默认 800）" },
    { L"--text",  true, L"只列出名字包含这段文字的节点" },
    { nullptr, false, nullptr }
};

static const OptSpec kSayOpts[] = {
    { L"--voice",     true,  L"语音名（见 voices）" },
    { L"--rate",      true,  L"语速 -10..10" },
    { L"--volume",    true,  L"音量 0..100" },
    { L"--wav",       true,  L"导出为 wav 而不是播放" },
    { L"--text-file", true,  L"从文件读要播报的文字" },
    { L"--async",     false, L"不等待播完" },
    { nullptr, false, nullptr }
};

static const OptSpec kRecOpts[] = {
    { L"--auto", false, L"启动即开始录" },
    { nullptr, false, nullptr }
};

static const OptSpec kPlayOpts[] = {
    { L"--loop", true, L"循环次数，0 = 无限" },
    { nullptr, false, nullptr }
};

static const OptSpec kLogOpts[] = {
    { L"--tail",    true,  L"最近 N 条" },
    { L"--level",   true,  L"级别过滤，逗号分隔" },
    { L"--id",      true,  L"事件 ID 过滤，逗号分隔" },
    { L"--cat",     true,  L"任务类别过滤" },
    { L"--source",  true,  L"来源过滤" },
    { L"--since",   true,  L"最近一段时间，如 30m / 2h" },
    { L"--from",    true,  L"起始时间" },
    { L"--to",      true,  L"结束时间" },
    { L"--grep",    true,  L"按文本过滤" },
    { L"--last",    false, L"只看上一次运行" },
    { L"--stats",   false, L"按级别/来源/ID 统计" },
    { L"--follow",  false, L"实时跟踪" },
    { L"--export",  true,  L"导出 CSV" },
    { L"--path",    false, L"显示日志路径" },
    { L"--clear",   false, L"清空日志（需要 --yes）" },
    { L"--yes",     false, L"确认清空" },
    { nullptr, false, nullptr }
};

static const OptSpec kDoctorOpts[] = {
    { L"--since", true, L"时间窗口，如 30m / 2h / 1d" },
    { nullptr, false, nullptr }
};

static const OptSpec kBenchOpts[] = {
    { L"--screen",    true, L"对一张固定截图做回归测量" },
    { L"--truth",     true, L"真值 JSON（区域 + 文字）" },
    { L"--threshold", true, L"CER 阈值，超过返回非 0" },
    { nullptr, false, nullptr }
};

// ------------------------------ 命令表 ------------------------------

static const CmdSpec kCmds[] = {
    { L"see",        L"see [选项]",                L"看屏幕：截屏 + OCR + 筛子", kSeeOpts },
    { L"click-text", L"click-text <文字> [选项]",  L"找到文字并点击（先 UI，再 OCR）", kClickTextOpts },
    { L"click",      L"click <x> <y> [选项]",      L"点击坐标", kClickOpts },
    { L"move",       L"move <x> <y>",              L"移动鼠标", nullptr },
    { L"type",       L"type <文字>",               L"输入文字（支持中文与任意 Unicode）", nullptr },
    { L"key",        L"key <键名或组合键>",        L"按键，如 ENTER / CTRL+S / 0x41", nullptr },
    { L"focus",      L"focus <标题片段>",          L"把窗口切到前台", nullptr },
    { L"read-text",  L"read-text [选项]",          L"读窗口里的文字（UIA / 剪贴板，比 OCR 准）", kReadTextOpts },
    { L"ui",         L"ui [选项]",                 L"列出当前窗口的 UI 控件树（调试用）", kUiOpts },
    { L"act",        L"act <脚本.txt>",            L"执行脚本（脚本里也能按文字点击）", nullptr },
    { L"say",        L"say <文字> [选项]",         L"语音播报", kSayOpts },
    { L"voices",     L"voices",                    L"列出可用语音", nullptr },
    { L"rec",        L"rec <文件.rec> [选项]",     L"录制", kRecOpts },
    { L"play",       L"play <文件.rec> [选项]",    L"回放", kPlayOpts },
    { L"dump",       L"dump <文件.rec>",           L"打印录制内容", nullptr },
    { L"stop",       L"stop",                      L"停止正在运行的动作、等待和验证", nullptr },
    { L"log",        L"log [选项]",                L"查系统日志", kLogOpts },
    { L"doctor",     L"doctor [选项]",             L"把日志翻译成原因和建议", kDoctorOpts },
    { L"bench",      L"bench [选项]",              L"OCR 参数测量与回归", kBenchOpts },
    { L"gui",        L"gui",                       L"打开图形界面", nullptr },
    { L"schema",     L"schema",                    L"输出这份能力清单", nullptr },
    { L"help",       L"help",                      L"输出帮助", nullptr },
};

const std::vector<CmdSpec>& CommandSpecs() {
    static const std::vector<CmdSpec> v(std::begin(kCmds), std::end(kCmds));
    return v;
}

bool OptTakesValue(const std::wstring& name) {
    for (const auto& o : kGlobalOpts) {
        if (o.name && name == o.name) return o.takesValue;
    }
    for (const auto& c : CommandSpecs()) {
        if (!c.opts) continue;
        for (const OptSpec* o = c.opts; o->name; ++o) {
            if (name == o->name) return o->takesValue;
        }
    }
    return false;
}

// ------------------------------ 帮助 ------------------------------

void PrintUsageFromTable() {
    wprintf(L"handeye —— 给 AI 用的手 / 嘴 / 眼\n\n");
    for (const auto& c : CommandSpecs()) {
        wprintf(L"  handeye %-32ls %ls\n", c.usage, c.desc);
    }

    wprintf(L"\n安全与验证（这些决定你敢不敢放手）:\n");
    wprintf(L"  --yes                放行含危险词的点击（删除/卸载/格式化/关机...）\n");
    wprintf(L"  --expect <文字>       动作后确认这段文字出现，否则退出码 5\n");
    wprintf(L"  --expect-gone <文字>  动作后确认这段文字消失，否则退出码 5\n");
    wprintf(L"  --announce           动作前先语音播报要做什么\n");

    wprintf(L"\n通用选项:\n");
    for (const OptSpec* o = kGlobalOpts; o->name; ++o) {
        wprintf(L"  %-20ls %ls\n", o->name, o->help);
    }

    wprintf(L"\nsee 的选项:\n");
    for (const OptSpec* o = kSeeOpts; o->name; ++o) {
        wprintf(L"  %-20ls %ls\n", o->name, o->help);
    }

    wprintf(L"\nclick-text 的选项:\n");
    for (const OptSpec* o = kClickTextOpts; o->name; ++o) {
        wprintf(L"  %-20ls %ls\n", o->name, o->help);
    }

    wprintf(L"\n退出码: 0 成功 / 1 参数或未找到 / 2 文件 / 3 执行失败 / 4 超时或被停止 / 5 验证失败\n");
}

// ------------------------------ 能力清单 ------------------------------

static void PrintJsonStringArray(const OptSpec* opts) {
    wprintf(L"[");
    bool first = true;
    for (const OptSpec* o = opts; o->name; ++o) {
        if (!first) wprintf(L",");
        first = false;
        wprintf(L"\"%ls\"", o->name);
    }
    wprintf(L"]");
}

void PrintSchemaFromTable() {
    wprintf(L"{\n");
    wprintf(L"  \"tool\": \"handeye\",\n");
    wprintf(L"  \"version\": \"1.2.0\",\n");
    wprintf(L"  \"purpose\": \"给 AI 使用的屏幕感知与鼠标键盘控制。不绑定任何模型厂商。\",\n");

    wprintf(L"  \"commands\": [\n");
    const auto& cmds = CommandSpecs();
    for (size_t i = 0; i < cmds.size(); ++i) {
        const auto& c = cmds[i];
        wprintf(L"    {\"name\":\"%ls\",\"usage\":\"%ls\",\"desc\":\"%ls\"",
                c.name, c.usage, c.desc);
        if (c.opts) {
            wprintf(L",\"options\":");
            PrintJsonStringArray(c.opts);
        }
        wprintf(L"}%ls\n", (i + 1 < cmds.size()) ? L"," : L"");
    }
    wprintf(L"  ],\n");

    wprintf(L"  \"globalOptions\": ");
    PrintJsonStringArray(kGlobalOpts);
    wprintf(L",\n");

    wprintf(L"  \"exitCodes\": {\"0\":\"成功\",\"1\":\"参数错误或未找到\",\"2\":\"文件错误\","
            L"\"3\":\"执行失败\",\"4\":\"超时或被停止\",\"5\":\"动作做了但验证没通过\"},\n");

    wprintf(L"  \"outputFormats\": {\n");
    wprintf(L"    \"text\": \"[类型] 文字 @x,y wxh；类型为 title/toolbar/sidebar/content/status/dialog/button/ui\",\n");
    wprintf(L"    \"json\": \"单个对象；含 blocks[].items[]、delta、warnings[]\"\n");
    wprintf(L"  },\n");

    wprintf(L"  \"tips\": [\n");
    wprintf(L"    \"先用 see --changed 判断屏幕有没有动，代价最低\",\n");
    wprintf(L"    \"要点击某个按钮，直接 click-text 按钮文字，不用自己算坐标\",\n");
    wprintf(L"    \"标准 Windows 控件优先走 UI Automation，认不出来才回落到 OCR\",\n");
    wprintf(L"    \"要读大段文字用 read-text，比 OCR 准\",\n");
    wprintf(L"    \"文字找不到时用 --wait 轮询，不要盲目重复截图\",\n");
    wprintf(L"    \"轮询用 --fast，需要看准用 --best\",\n");
    wprintf(L"    \"不确定要做什么时先 --dry-run，看清楚再真执行\",\n");
    wprintf(L"    \"危险动作默认被拦截，确认无误再加 --yes\",\n");
    wprintf(L"    \"动作后加 --expect 或 --expect-gone，让工具自己确认成没成\",\n");
    wprintf(L"    \"要让人跟得上，加 --announce，动作前会先说出来\"\n");
    wprintf(L"  ]\n");
    wprintf(L"}\n");
}
