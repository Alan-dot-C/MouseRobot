// ============================================================================
//  logquery.cpp —— 日志查询（对标事件查看器的筛选、统计、导出）
// ============================================================================
#include "logquery.h"
#include "log.h"
#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <set>

// ------------------------------ 参数 ------------------------------

struct QArgs {
    std::vector<std::wstring> raw;

    bool Has(const std::wstring& n) const {
        for (const auto& a : raw) if (a == n) return true;
        return false;
    }
    std::wstring Get(const std::wstring& n, const std::wstring& def = L"") const {
        for (size_t i = 0; i < raw.size(); ++i)
            if (raw[i] == n && i + 1 < raw.size()) return raw[i + 1];
        return def;
    }
    int GetInt(const std::wstring& n, int def) const {
        std::wstring v = Get(n);
        return v.empty() ? def : _wtoi(v.c_str());
    }
};

static bool IsValueFlag(const std::wstring& s) {
    static const wchar_t* f[] = {
        L"--tail", L"--level", L"--id", L"--cat", L"--source", L"--since",
        L"--from", L"--to", L"--grep", L"--export", L"--dir",
    };
    for (auto x : f) if (s == x) return true;
    return false;
}

// 逗号分隔 → 集合
static std::set<std::wstring> SplitSet(const std::wstring& s) {
    std::set<std::wstring> out;
    for (auto& p : Split(s, L',')) {
        std::wstring t = Trim(p);
        if (!t.empty()) out.insert(t);
    }
    return out;
}

// "30m" / "2h" / "1d" / "2026-10-17 18:00" → 时间前缀字符串
static std::wstring SinceToStamp(const std::wstring& since) {
    std::wstring s = Trim(since);
    if (s.empty()) return L"";

    // 相对时间
    if (s.size() >= 2) {
        wchar_t unit = towlower(s.back());
        std::wstring numPart = s.substr(0, s.size() - 1);
        bool allDigit = !numPart.empty();
        for (wchar_t c : numPart) if (!iswdigit(c)) { allDigit = false; break; }
        if (allDigit && (unit == L's' || unit == L'm' || unit == L'h' || unit == L'd')) {
            long long n = _wtoi64(numPart.c_str());
            long long ms = 0;
            if (unit == L's') ms = n * 1000LL;
            else if (unit == L'm') ms = n * 60000LL;
            else if (unit == L'h') ms = n * 3600000LL;
            else ms = n * 86400000LL;
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
            u.QuadPart -= (ULONGLONG)ms * 10000ULL;
            ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
            SYSTEMTIME st;
            FileTimeToSystemTime(&ft, &st);
            SYSTEMTIME local;
            SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
            wchar_t buf[32];
            swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
                       local.wYear, local.wMonth, local.wDay,
                       local.wHour, local.wMinute, local.wSecond);
            return buf;
        }
    }
    // 绝对时间：补全到可比较的前缀
    if (s.size() == 5 && s[2] == L':') {   // "18:00"
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t buf[32];
        swprintf_s(buf, L"%04d-%02d-%02d %ls:00", st.wYear, st.wMonth, st.wDay, s.c_str());
        return buf;
    }
    if (s.size() == 10) return s + L" 00:00:00";   // "2026-10-17"
    return s;
}

// 从 detail 里取会话号：[S=xxxxx P=...]
static std::wstring ExtractSession(const std::wstring& detail) {
    size_t p = detail.find(L"[S=");
    if (p == std::wstring::npos) return L"";
    size_t e = detail.find(L' ', p);
    if (e == std::wstring::npos) e = detail.find(L']', p);
    if (e == std::wstring::npos) return L"";
    std::wstring s = detail.substr(p + 3, e - p - 3);
    return s;
}

// ------------------------------ 输出 ------------------------------

static std::string RecToJson(const LogRecord& r) {
    std::string s = "{\"time\":\"";
    s += JsonEscape(ToUtf8(r.time));
    s += "\",\"level\":";
    s += std::to_string(r.level);
    s += ",\"levelName\":\"";
    s += JsonEscape(ToUtf8(r.levelName));
    s += "\",\"source\":\"";
    s += JsonEscape(ToUtf8(r.source));
    s += "\",\"eventId\":";
    s += std::to_string(r.eventId);
    s += ",\"category\":\"";
    s += JsonEscape(ToUtf8(r.category));
    s += "\",\"desc\":\"";
    s += JsonEscape(ToUtf8(r.desc));
    s += "\",\"detail\":\"";
    s += JsonEscape(ToUtf8(r.detail));
    s += "\"}";
    return s;
}

static std::string CsvEscape(const std::string& s) {
    bool need = s.find(',') != std::string::npos || s.find('"') != std::string::npos ||
                s.find('\n') != std::string::npos;
    if (!need) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else o += c; }
    o += "\"";
    return o;
}

// ------------------------------ 主流程 ------------------------------

int CmdLogQuery(const std::vector<std::wstring>& rawArgs) {
    QArgs a; a.raw = rawArgs;

    LogInit();   // 只为拿到目录与配置

    // --- schema：打印事件目录 ---
    if (a.Has(L"schema")) {
        wprintf(L"事件目录（%zu 个）\n", LogEventCatalog().size());
        wprintf(L"%-6ls %-6ls %-6ls %-22ls %ls\n", L"ID", L"来源", L"类别", L"描述", L"字段");
        for (const auto& e : LogEventCatalog()) {
            wprintf(L"%-6d %-6ls %-6ls %-22ls %ls\n",
                    e.id, e.source, e.category, e.desc, e.detailHint);
        }
        return 0;
    }

    // --- path：打印日志位置 ---
    if (a.Has(L"--path")) {
        wprintf(L"directory: %ls\n", g_log.dir.c_str());
        wprintf(L"active   : %ls\n", LogFilePath().c_str());
        wprintf(L"level    : %ls\n", LogLevelName(g_log.level));
        wprintf(L"text     : %ls\n",
                g_log.textMode == LOG_TEXT_FULL ? L"full" :
                g_log.textMode == LOG_TEXT_MASK ? L"mask" : L"off");
        wprintf(L"maxBytes : %d\n", g_log.maxFileBytes);
        wprintf(L"keep     : %d\n", g_log.keepFiles);
        return 0;
    }

    // --- clear：清空日志（必须显式确认）---
    if (a.Has(L"--clear")) {
        if (!a.Has(L"--yes")) {
            wprintf(L"拒绝执行：清空日志需要加 --yes 确认\n");
            return 1;
        }
        int removed = 0;
        WIN32_FIND_DATAW fd;
        std::wstring pattern = g_log.dir + L"\\handeye*.log";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring p = g_log.dir + L"\\" + fd.cFileName;
                if (DeleteFileW(p.c_str())) ++removed;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        wprintf(L"已删除 %d 个日志文件\n", removed);
        return 0;
    }

    // --- 读 ---
    // 先关掉本进程持有的写句柄，避免自己占着自己的文件
    LogCloseFile();
    auto recs = LogReadAll();

    // --- 过滤 ---
    std::set<std::wstring> levels = SplitSet(a.Get(L"--level"));
    std::set<std::wstring> cats = SplitSet(a.Get(L"--cat"));
    std::set<std::wstring> sources = SplitSet(a.Get(L"--source"));
    // --id 支持逗号分隔，和 --level 保持一致
    std::set<int> idFilter;
    for (const auto& part : Split(a.Get(L"--id"), L',')) {
        std::wstring t = Trim(part);
        if (!t.empty()) idFilter.insert(_wtoi(t.c_str()));
    }
    std::wstring grep = a.Get(L"--grep");
    std::wstring since = SinceToStamp(a.Get(L"--since"));
    std::wstring from = SinceToStamp(a.Get(L"--from"));
    std::wstring to = a.Get(L"--to");
    if (!from.empty()) since = from;

    std::vector<LogRecord> hit;
    hit.reserve(recs.size());
    for (const auto& r : recs) {
        if (!levels.empty()) {
            bool ok = false;
            for (const auto& l : levels) {
                if (r.levelName == l || std::to_wstring(r.level) == l) { ok = true; break; }
            }
            if (!ok) continue;
        }
        if (!cats.empty() && !cats.count(r.category)) continue;
        if (!sources.empty() && !sources.count(r.source)) continue;
        if (!idFilter.empty() && !idFilter.count(r.eventId)) continue;
        if (!since.empty() && r.time < since) continue;
        if (!to.empty() && r.time > to) continue;
        if (!grep.empty()) {
            if (r.raw.find(grep) == std::wstring::npos) continue;
        }
        hit.push_back(r);
    }

    // --- --last：只留最后一次会话 ---
    if (a.Has(L"--last") && !hit.empty()) {
        std::wstring lastSession;
        for (auto it = hit.rbegin(); it != hit.rend(); ++it) {
            std::wstring s = ExtractSession(it->detail);
            if (!s.empty()) { lastSession = s; break; }
        }
        if (!lastSession.empty()) {
            std::vector<LogRecord> only;
            for (const auto& r : hit)
                if (ExtractSession(r.detail) == lastSession) only.push_back(r);
            hit.swap(only);
            if (!g_opt.json) wprintf(L"(会话 %ls)\n", lastSession.c_str());
        }
    }

    // --- --stats ---
    if (a.Has(L"--stats")) {
        std::map<int, int> byId;
        std::map<std::wstring, int> byLevel, bySource, byCat;
        for (const auto& r : hit) {
            byId[r.eventId]++;
            byLevel[r.levelName]++;
            bySource[r.source]++;
            byCat[r.category]++;
        }
        wprintf(L"共 %zu 条\n\n按级别:\n", hit.size());
        for (const auto& kv : byLevel) wprintf(L"  %-6ls %d\n", kv.first.c_str(), kv.second);
        wprintf(L"\n按来源:\n");
        for (const auto& kv : bySource) wprintf(L"  %-6ls %d\n", kv.first.c_str(), kv.second);
        wprintf(L"\n按类别:\n");
        for (const auto& kv : byCat) wprintf(L"  %-6ls %d\n", kv.first.c_str(), kv.second);
        wprintf(L"\n按事件 ID（前 25）:\n");
        std::vector<std::pair<int, int>> v(byId.begin(), byId.end());
        std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
        int shown = 0;
        for (auto& p : v) {
            if (shown++ >= 25) break;
            const wchar_t* d = L"";
            for (const auto& e : LogEventCatalog()) if (e.id == p.first) { d = e.desc; break; }
            wprintf(L"  %-6d %-5d %ls\n", p.first, p.second, d);
        }
        return 0;
    }

    // --- --export ---
    std::wstring exportPath = a.Get(L"--export");
    if (!exportPath.empty()) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, exportPath.c_str(), L"wb") != 0 || !f) {
            wprintf(L"无法写入: %ls\n", exportPath.c_str());
            return 2;
        }
        bool csv = exportPath.size() > 4 &&
                   ToUpper(exportPath.substr(exportPath.size() - 4)) == L".CSV";
        std::string out;
        if (csv) {
            // Excel 认 UTF-8 BOM，没有它中文会显示成乱码
            out = "\xEF\xBB\xBF";
            out += "time,level,source,eventId,category,desc,detail\r\n";
            for (const auto& r : hit) {
                out += CsvEscape(ToUtf8(r.time)) + ",";
                out += std::to_string(r.level) + ",";
                out += CsvEscape(ToUtf8(r.source)) + ",";
                out += std::to_string(r.eventId) + ",";
                out += CsvEscape(ToUtf8(r.category)) + ",";
                out += CsvEscape(ToUtf8(r.desc)) + ",";
                out += CsvEscape(ToUtf8(r.detail)) + "\r\n";
            }
        } else {
            for (const auto& r : hit) { out += RecToJson(r); out += "\n"; }
        }
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        wprintf(L"已导出 %zu 条到 %ls\n", hit.size(), exportPath.c_str());
        return 0;
    }

    // --- --follow：跟踪新写入 ---
    if (a.Has(L"--follow")) {
        wprintf(L"(跟踪 %ls，Ctrl+C 退出)\n", LogFilePath().c_str());
        for (const auto& r : hit) wprintf(L"%ls\n", r.raw.c_str());
        fflush(stdout);
        long long offset = 0;
        FILE* f = nullptr;
        if (_wfopen_s(&f, LogFilePath().c_str(), L"rb") == 0 && f) {
            fseek(f, 0, SEEK_END);
            offset = ftell(f);
            fclose(f);
        }
        for (;;) {
            FILE* g = nullptr;
            if (_wfopen_s(&g, LogFilePath().c_str(), L"rb") == 0 && g) {
                fseek(g, 0, SEEK_END);
                long long sz = ftell(g);
                if (sz > offset) {
                    fseek(g, (long)offset, SEEK_SET);
                    std::string buf(size_t(sz - offset), '\0');
                    size_t got = fread(&buf[0], 1, buf.size(), g);
                    buf.resize(got);
                    wprintf(L"%ls", FromUtf8(buf).c_str());
                    fflush(stdout);
                    offset = sz;
                }
                fclose(g);
            }
            Sleep(300);
        }
    }

    // --- 普通输出：默认只看最近 30 条 ---
    int tail = a.GetInt(L"--tail", 30);
    size_t begin = 0;
    if (tail > 0 && hit.size() > size_t(tail)) begin = hit.size() - size_t(tail);

    if (g_opt.json) {
        wprintf(L"{\"count\":%zu,\"events\":[\n", hit.size() - begin);
        for (size_t i = begin; i < hit.size(); ++i) {
            wprintf(L"%ls", FromUtf8(RecToJson(hit[i])).c_str());
            wprintf(L"%ls", (i + 1 < hit.size()) ? L",\n" : L"\n");
        }
        wprintf(L"]}\n");
    } else {
        if (hit.empty()) {
            wprintf(L"(没有匹配的事件)\n");
            return 0;
        }
        wprintf(L"(共 %zu 条，显示最近 %zu 条)\n", hit.size(), hit.size() - begin);
        for (size_t i = begin; i < hit.size(); ++i) wprintf(L"%ls\n", hit[i].raw.c_str());
    }
    return 0;
}
