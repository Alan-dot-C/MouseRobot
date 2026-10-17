// ============================================================================
//  doctor.cpp
//
//  A log nobody reads is just disk usage. This reads the log, groups events by
//  ID, and maps each known ID to a plain-language cause and a suggestion.
//  The point is not the count, it is the "why" and the "what now".
// ============================================================================
#include "doctor.h"
#include "log.h"
#include "common.h"

#include <cstdio>
#include <map>
#include <algorithm>

struct Diagnosis {
    const wchar_t* cause;
    const wchar_t* advice;
};

// Keyed by event ID. Anything not listed still shows up, with a generic line.
static const std::map<int, Diagnosis>& Table() {
    static const std::map<int, Diagnosis> t = {
        { 2000, { L"未捕获异常，进程崩溃过",     L"查看崩溃前最后 20 条事件，定位触发它的那一步" } },
        { 2001, { L"未捕获的 C++ 异常",        L"同上；如果是脚本引起的，缩小到具体那一行" } },
        { 3006, { L"OCR 引擎不可用",           L"检查系统是否安装了中文 OCR 组件" } },
        { 3007, { L"OCR 超时",                L"图片过大或系统繁忙；试 --region 缩小范围" } },
        { 3008, { L"截屏失败",                L"检查显示会话是否可用（锁屏时无法截屏）" } },
        { 3010, { L"等待文字超时",            L"目标没出现：可能界面慢了，或文字被 OCR 认错，调大 --timeout 或用更短的关键词" } },
        { 4006, { L"SendInput 被系统丢弃",    L"目标窗口权限更高（管理员），先用 focus 切到前台，别在后台硬发" } },
        { 4007, { L"等待窗口超时",            L"窗口没起来或标题不符；确认标题片段是否太具体" } },
        { 4010, { L"键名无法识别",            L"换用 0xNN 十六进制虚拟键码" } },
        { 4012, { L"动作验证超时",            L"动作可能没生效：目标没变，或变化发生在预期之外的地方" } },
        { 4013, { L"危险动作被拦截",          L"这是保护，不是故障；确认无误后再加 --yes" } },
        { 4015, { L"动作被中止",              L"收到 stop，或超过 --max-duration；确认是谁发的停止信号" } },
        { 5003, { L"语音引擎不可用",          L"系统缺少语音组件，或用 --voice 指定已安装的语音" } },
        { 5004, { L"语音合成失败",            L"换一个语音试试；文本过长也可能失败" } },
        { 6002, { L"脚本某行失败",            L"看 detail 里的 line 和 cmd，单独跑那一行复现" } },
        { 6008, { L"录制文件损坏",            L"文件被截断；重新录一份" } },
        { 7000, { L"状态文件读失败",          L"通常是首次运行，可忽略" } },
        { 7003, { L"日志写入失败",            L"检查日志目录权限或磁盘空间" } },
        { 8001, { L"会话以非 0 退出码结束",     L"没找到/被拦截/验证失败也会走这里；看同一会话的 3xxx/4xxx 定位原因" } },
    };
    return t;
}

int CmdDoctor(const std::vector<std::wstring>& args) {
    bool json = false;
    int minutes = 30;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--json") json = true;
        else if (args[i] == L"--since" && i + 1 < args.size()) {
            std::wstring v = args[i + 1];
            if (!v.empty()) {
                wchar_t unit = towlower(v.back());
                int n = _wtoi(v.substr(0, v.size() - 1).c_str());
                if (n > 0) {
                    if (unit == L'm') minutes = n;
                    else if (unit == L'h') minutes = n * 60;
                    else if (unit == L'd') minutes = n * 60 * 24;
                }
            }
        }
    }

    LogInit();
    LogCloseFile();
    auto recs = LogReadAll();

    // Time window: keep records newer than cutoff. Timestamps are fixed width,
    // so a string compare is enough.
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)minutes * 60ULL * 10000000ULL;
    FILETIME ft2;
    ft2.dwLowDateTime = u.LowPart;
    ft2.dwHighDateTime = u.HighPart;
    SYSTEMTIME cut;
    FileTimeToSystemTime(&ft2, &cut);
    wchar_t cutoff[32];
    swprintf_s(cutoff, L"%04d-%02d-%02d %02d:%02d:%02d",
               cut.wYear, cut.wMonth, cut.wDay, cut.wHour, cut.wMinute, cut.wSecond);

    std::map<int, int> byId;
    std::map<int, std::wstring> lastDesc;
    int serious = 0, errors = 0, warnings = 0, total = 0;

    for (const auto& r : recs) {
        if (r.time < cutoff) continue;
        ++total;
        if (r.level == LOG_CRITICAL) ++serious;
        else if (r.level == LOG_ERROR) ++errors;
        else if (r.level == LOG_WARNING) ++warnings;
        if (r.level <= LOG_WARNING) {
            byId[r.eventId]++;
            lastDesc[r.eventId] = r.desc;
        }
    }

    if (json) {
        wprintf(L"{\"window\":\"%dmin\",\"total\":%d,\"critical\":%d,\"error\":%d,\"warning\":%d,\"issues\":[",
                minutes, total, serious, errors, warnings);
        bool first = true;
        for (const auto& kv : byId) {
            if (!first) wprintf(L",");
            first = false;
            wprintf(L"{\"eventId\":%d,\"count\":%d}", kv.first, kv.second);
        }
        wprintf(L"]}\n");
        return 0;
    }

    wprintf(L"诊断：最近 %d 分钟，共 %d 条事件\n", minutes, total);
    wprintf(L"  严重 %d  错误 %d  警告 %d\n", serious, errors, warnings);

    if (byId.empty()) {
        wprintf(L"\n没有异常事件。\n");
        return 0;
    }

    // Most frequent first: repetition is the strongest signal.
    std::vector<std::pair<int, int>> v(byId.begin(), byId.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    wprintf(L"\n");
    for (const auto& kv : v) {
        int id = kv.first;
        int n = kv.second;
        auto it = Table().find(id);
        wprintf(L"[%d 次] %d %ls\n", n, id, lastDesc[id].c_str());
        if (it != Table().end()) {
            wprintf(L"        原因：%ls\n", it->second.cause);
            wprintf(L"        建议：%ls\n", it->second.advice);
        } else {
            wprintf(L"        未登记的事件，看 handeye log --id %d\n", id);
        }
    }
    return 0;
}
