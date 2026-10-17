// ============================================================================
//  log.cpp —— 系统日志实现
//
//  要点：
//    - 行格式与安卓端一致：时间 | 级别 | 来源 | 事件ID | 类别 | 描述 | 详细
//    - 详细信息里带 [S=会话 P=进程] 前缀，便于把一次 AI 任务的多次调用串起来
//    - 同步写盘 + 每次 flush，崩溃前最后一行不丢
//    - 命名互斥体保证多进程同时写不会撕裂
//    - 活动文件超限就改名带时间戳（照抄 CBS.log 的做法），旧的按数量裁剪
//    - 崩溃时把内存里的"飞行记录器"（最近 20 条）一并落盘
// ============================================================================
#include "log.h"

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <algorithm>
#include <deque>
#include <mutex>
#include <share.h>

// ------------------------------ 全局状态 ------------------------------

LogConfig g_log;

static std::wstring g_session;          // 5 位十六进制
static DWORD        g_pid = 0;
static DWORD        g_startTick = 0;
static std::wstring g_logPath;          // 活动文件完整路径
static HANDLE       g_mutex = nullptr;
static FILE*        g_file = nullptr;   // 以追加方式打开
static std::wstring g_lockName = L"Global\\handeye_log_mutex";

// 飞行记录器：最近 N 条已格式化行，崩溃时落盘
static const size_t kFlightRecords = 20;
static std::deque<std::wstring> g_flight;
static std::mutex g_flightMutex;
static volatile LONG g_inCrash = 0;

// ------------------------------ 小工具 ------------------------------

static std::wstring LogDirFromEnv() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
    return base + L"\\handeye";
}

std::wstring LogDefaultDir() { return LogDirFromEnv() + L"\\logs"; }
std::wstring LogFilePath() { return g_logPath; }

const std::wstring& LogSessionId() { return g_session; }
DWORD LogStartTick() { return g_startTick; }

const wchar_t* LogLevelName(int level) {
    switch (level) {
    case LOG_CRITICAL: return L"严重";
    case LOG_ERROR:    return L"错误";
    case LOG_WARNING:  return L"警告";
    case LOG_INFO:     return L"信息";
    case LOG_VERBOSE:  return L"详细";
    default:           return L"信息";
    }
}

int LogLevelFromName(const std::wstring& name) {
    std::wstring t = Trim(name);
    if (t == L"严重" || t == L"critical") return LOG_CRITICAL;
    if (t == L"错误" || t == L"error")    return LOG_ERROR;
    if (t == L"警告" || t == L"warning")  return LOG_WARNING;
    if (t == L"信息" || t == L"info")     return LOG_INFO;
    if (t == L"详细" || t == L"verbose")  return LOG_VERBOSE;
    return _wtoi(t.c_str());
}

static void MakeDirTree(const std::wstring& path) {
    if (path.empty()) return;
    std::wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == L'\\' || path[i] == L'/') {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

static std::wstring NowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

static std::wstring NowFileStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ------------------------------ 配置 ------------------------------

static void LoadConfigFile() {
    std::wstring path = LogDirFromEnv() + L"\\config.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string bytes;
    if (sz > 0) {
        bytes.resize(size_t(sz));
        size_t got = fread(&bytes[0], 1, size_t(sz), f);
        bytes.resize(got);
    }
    fclose(f);

    for (auto& rawLine : Split(FromUtf8(bytes), L'\n')) {
        std::wstring line = Trim(rawLine);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring val = Trim(line.substr(eq + 1));
        if (key == L"dir")        g_log.dir = val;
        else if (key == L"level") g_log.level = LogLevelFromName(val);
        else if (key == L"text") {
            if (val == L"mask") g_log.textMode = LOG_TEXT_MASK;
            else if (val == L"off") g_log.textMode = LOG_TEXT_OFF;
            else g_log.textMode = LOG_TEXT_FULL;
        }
        else if (key == L"maxBytes") g_log.maxFileBytes = _wtoi(val.c_str());
        else if (key == L"keep")     g_log.keepFiles = _wtoi(val.c_str());
        else if (key == L"enabled")  g_log.enabled = !(val == L"0" || val == L"off" || val == L"false");
    }
}

static void LoadConfigEnv() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = 0;

    n = GetEnvironmentVariableW(L"HANDEYE_LOG_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) g_log.dir = std::wstring(buf, n);

    n = GetEnvironmentVariableW(L"HANDEYE_LOG_LEVEL", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) g_log.level = LogLevelFromName(std::wstring(buf, n));

    n = GetEnvironmentVariableW(L"HANDEYE_LOG_TEXT", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring v = ToUpper(std::wstring(buf, n));
        if (v == L"MASK") g_log.textMode = LOG_TEXT_MASK;
        else if (v == L"OFF") g_log.textMode = LOG_TEXT_OFF;
        else g_log.textMode = LOG_TEXT_FULL;
    }

    n = GetEnvironmentVariableW(L"HANDEYE_LOG", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring v = ToUpper(std::wstring(buf, n));
        g_log.enabled = !(v == L"0" || v == L"OFF" || v == L"FALSE" || v == L"NO");
    }

    n = GetEnvironmentVariableW(L"HANDEYE_LOG_ECHO", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring v = ToUpper(std::wstring(buf, n));
        g_log.echoToStderr = (v == L"1" || v == L"ON" || v == L"TRUE");
    }
}

// ------------------------------ 旋转 ------------------------------

// 在持有互斥体的情况下调用。把活动文件改名成带时间戳的名字，并裁剪旧文件。
static void RotateLocked() {
    if (!g_file) return;
    std::wstring dir = g_log.dir;
    std::wstring base = dir + L"\\handeye";
    std::wstring rolled = base + L"_" + NowFileStamp() + L".log";

    fclose(g_file);
    g_file = nullptr;

    // 目标重名就加后缀，绝不覆盖已有日志
    std::wstring target = rolled;
    for (int i = 1; i < 100 && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        wchar_t suffix[16];
        swprintf_s(suffix, L"_%d", i);
        target = base + L"_" + NowFileStamp() + suffix + L".log";
    }
    MoveFileW(g_logPath.c_str(), target.c_str());

    // 裁剪：按文件名（时间戳）排序，超出的删最旧
    if (g_log.keepFiles > 0) {
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd;
        std::wstring pattern = base + L"_*.log";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    files.push_back(dir + L"\\" + fd.cFileName);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(files.begin(), files.end());
        while ((int)files.size() > g_log.keepFiles) {
            DeleteFileW(files.front().c_str());
            files.erase(files.begin());
        }
    }

    g_file = _wfsopen(g_logPath.c_str(), L"ab", _SH_DENYNO);
    if (!g_file) {
        // 追加模式打不开就退化成覆盖写，至少别把日志丢掉
        g_file = _wfsopen(g_logPath.c_str(), L"wb", _SH_DENYNO);
    }

    // 记录一次滚动事件（此时 g_file 已重新打开，可以安全写）
    if (g_file) {
        wchar_t detail[512];
        swprintf_s(detail, L"[S=%ls P=%lu] from=%ls to=%ls",
                   g_session.empty() ? L"----" : g_session.c_str(), (unsigned long)g_pid,
                   g_logPath.c_str(), target.c_str());
        // 直接拼一行写进去，避免递归调用 LogWrite 再次触发旋转判断
        std::wstring line = NowStamp();
        line += L" | 信息 | 配置 | 7002 | 数据 | 日志滚动 | ";
        line += detail;
        line += L"\r\n";
        std::string utf8 = ToUtf8(line);
        fwrite(utf8.data(), 1, utf8.size(), g_file);
        fflush(g_file);
    }
}

static void EnsureFileOpen() {
    if (g_file) return;
    MakeDirTree(g_log.dir);
    g_file = _wfsopen(g_logPath.c_str(), L"ab", _SH_DENYNO);
}

// ------------------------------ 写 ------------------------------

static void FlightPush(const std::wstring& line) {
    std::lock_guard<std::mutex> lk(g_flightMutex);
    g_flight.push_back(line);
    while (g_flight.size() > kFlightRecords) g_flight.pop_front();
}

// 不加锁直接写一行（崩溃路径用，避免死锁）
static void WriteRawNoLock(const std::wstring& line) {
    if (!g_log.enabled) return;
    MakeDirTree(g_log.dir);
    FILE* f = _wfsopen(g_logPath.c_str(), L"ab", _SH_DENYNO);
    if (!f) return;
    std::string utf8 = ToUtf8(line);
    fwrite(utf8.data(), 1, utf8.size(), f);
    fflush(f);
    fclose(f);
}

void LogWrite(int level, const wchar_t* source, int eventId,
              const wchar_t* category, const wchar_t* desc,
              const wchar_t* detailFmt, ...) {
    if (!g_log.enabled) return;
    if (level > g_log.level && level > LOG_WARNING) return;   // 严重/错误/警告永不过滤

    // 组装详细信息
    std::wstring detail;
    if (detailFmt && detailFmt[0]) {
        va_list ap;
        va_start(ap, detailFmt);
        wchar_t buf[2048];
        _vsnwprintf_s(buf, _TRUNCATE, detailFmt, ap);
        va_end(ap);
        detail = buf;
    }

    wchar_t prefix[64];
    swprintf_s(prefix, L"[S=%ls P=%lu]", g_session.empty() ? L"----" : g_session.c_str(),
               (unsigned long)g_pid);
    std::wstring fullDetail = prefix;
    if (!detail.empty()) { fullDetail += L" "; fullDetail += detail; }

    std::wstring line = NowStamp();
    line += L" | ";
    line += LogLevelName(level);
    line += L" | ";
    line += (source && source[0]) ? source : L"系统";
    line += L" | ";
    line += std::to_wstring(eventId);
    line += L" | ";
    line += (category && category[0]) ? category : L"一般";
    line += L" | ";
    line += (desc && desc[0]) ? desc : L"";
    line += L" | ";
    line += fullDetail;
    line += L"\r\n";

    FlightPush(line);

    // 会话互斥：多进程同时写时不撕裂
    bool locked = false;
    if (g_mutex) {
        DWORD w = WaitForSingleObject(g_mutex, 2000);
        locked = (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
    }

    EnsureFileOpen();
    if (g_file) {
        if (g_log.maxFileBytes > 0) {
            long pos = ftell(g_file);
            if (pos >= g_log.maxFileBytes) RotateLocked();
        }
        if (g_file) {
            std::string utf8 = ToUtf8(line);
            fwrite(utf8.data(), 1, utf8.size(), g_file);
            fflush(g_file);
        }
    }

    if (locked) ReleaseMutex(g_mutex);

    if (g_log.echoToStderr) {
        fwprintf(stderr, L"%ls", line.c_str());
    }
}

void LogC(const wchar_t* s, int id, const wchar_t* c, const wchar_t* d, const wchar_t* f, ...) {
    va_list ap; va_start(ap, f);
    wchar_t buf[2048] = {};
    if (f) _vsnwprintf_s(buf, _TRUNCATE, f, ap);
    va_end(ap);
    LogWrite(LOG_CRITICAL, s, id, c, d, f ? L"%ls" : nullptr, buf);
}
void LogE(const wchar_t* s, int id, const wchar_t* c, const wchar_t* d, const wchar_t* f, ...) {
    va_list ap; va_start(ap, f);
    wchar_t buf[2048] = {};
    if (f) _vsnwprintf_s(buf, _TRUNCATE, f, ap);
    va_end(ap);
    LogWrite(LOG_ERROR, s, id, c, d, f ? L"%ls" : nullptr, buf);
}
void LogW(const wchar_t* s, int id, const wchar_t* c, const wchar_t* d, const wchar_t* f, ...) {
    va_list ap; va_start(ap, f);
    wchar_t buf[2048] = {};
    if (f) _vsnwprintf_s(buf, _TRUNCATE, f, ap);
    va_end(ap);
    LogWrite(LOG_WARNING, s, id, c, d, f ? L"%ls" : nullptr, buf);
}
void LogI(const wchar_t* s, int id, const wchar_t* c, const wchar_t* d, const wchar_t* f, ...) {
    va_list ap; va_start(ap, f);
    wchar_t buf[2048] = {};
    if (f) _vsnwprintf_s(buf, _TRUNCATE, f, ap);
    va_end(ap);
    LogWrite(LOG_INFO, s, id, c, d, f ? L"%ls" : nullptr, buf);
}
void LogV(const wchar_t* s, int id, const wchar_t* c, const wchar_t* d, const wchar_t* f, ...) {
    va_list ap; va_start(ap, f);
    wchar_t buf[2048] = {};
    if (f) _vsnwprintf_s(buf, _TRUNCATE, f, ap);
    va_end(ap);
    LogWrite(LOG_VERBOSE, s, id, c, d, f ? L"%ls" : nullptr, buf);
}

// ------------------------------ 隐私 ------------------------------

std::wstring LogRedactText(const std::wstring& text) {
    switch (g_log.textMode) {
    case LOG_TEXT_OFF:
        return L"text=<hidden>";
    case LOG_TEXT_MASK: {
        if (text.empty()) return L"text=\"\" len=0";
        std::wstring head = text.substr(0, 1);
        std::wstring tail = text.size() > 2 ? text.substr(text.size() - 1) : L"";
        wchar_t buf[256];
        swprintf_s(buf, L"text=\"%ls***%ls\" len=%zu", head.c_str(), tail.c_str(), text.size());
        return buf;
    }
    default: {
        // 原文，防止换行把日志行拆坏
        std::wstring t = text;
        ReplaceAll(t, L"\r", L"\\r");
        ReplaceAll(t, L"\n", L"\\n");
        if (t.size() > 500) t = t.substr(0, 500) + L"…";
        std::wstring out = L"text=\"";
        out += t;
        out += L"\"";
        return out;
    }
    }
}

// ------------------------------ 崩溃 ------------------------------

static void DumpFlightOnCrash(const wchar_t* reason, unsigned long code, const wchar_t* extra) {
    if (InterlockedExchange(&g_inCrash, 1) != 0) return;   // 防止崩溃里再崩溃

    std::wstring line = NowStamp();
    line += L" | 严重 | 崩溃 | 2000 | 异常 | ";
    line += reason;
    wchar_t detail[512];
    swprintf_s(detail, L"[S=%ls P=%lu] code=0x%08lX %ls",
               g_session.empty() ? L"----" : g_session.c_str(), (unsigned long)g_pid,
               code, extra ? extra : L"");
    line += L" | ";
    line += detail;
    line += L"\r\n";
    WriteRawNoLock(line);

    std::deque<std::wstring> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_flightMutex);
        snapshot = g_flight;
    }
    while (!snapshot.empty()) {
        WriteRawNoLock(snapshot.front());
        snapshot.pop_front();
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    unsigned long code = 0;
    std::wstring extra;
    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        extra = L"addr=0x" + std::to_wstring((unsigned long long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    }
    DumpFlightOnCrash(L"未捕获的 SEH 异常", code, extra.c_str());
    return EXCEPTION_EXECUTE_HANDLER;
}

static void TerminateHandler() {
    DumpFlightOnCrash(L"未捕获的 C++ 异常 (std::terminate)", 0, L"");
    abort();
}

static void InvalidParamHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    DumpFlightOnCrash(L"无效参数 (CRT)", 0, L"");
}

void LogInstallCrashHandlers() {
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(TerminateHandler);
    _set_invalid_parameter_handler(InvalidParamHandler);
}

// ------------------------------ 初始化 ------------------------------

void LogInit() {
    g_pid = GetCurrentProcessId();
    g_startTick = GetTickCount();

    // 会话号：进程号 + 启动时刻低 12 位，取十六进制五位
    wchar_t sid[16];
    swprintf_s(sid, L"%05x", ((g_pid * 2654435761u) ^ g_startTick) & 0xFFFFF);
    g_session = sid;

    LoadConfigFile();
    LoadConfigEnv();

    if (g_log.dir.empty()) g_log.dir = LogDefaultDir();
    g_logPath = g_log.dir + L"\\handeye.log";

    if (!g_log.enabled) return;

    MakeDirTree(g_log.dir);
    g_mutex = CreateMutexW(nullptr, FALSE, g_lockName.c_str());
    EnsureFileOpen();

    LogInstallCrashHandlers();
}

void LogShutdown() {
    if (g_file) { fflush(g_file); fclose(g_file); g_file = nullptr; }
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = nullptr; }
}

void LogCloseFile() {
    if (g_file) { fflush(g_file); fclose(g_file); g_file = nullptr; }
}

void LogConfigure(bool hasDir, const std::wstring& dir,
                  bool hasLevel, int level,
                  bool hasText, int textMode,
                  bool hasEnabled, bool enabled) {
    bool dirChanged = false;
    if (hasDir && !dir.empty() && dir != g_log.dir) {
        g_log.dir = dir;
        dirChanged = true;
    }
    if (hasLevel) g_log.level = level;
    if (hasText)  g_log.textMode = static_cast<LogTextMode>(textMode);
    if (hasEnabled) g_log.enabled = enabled;

    if (dirChanged && g_log.enabled) {
        if (g_file) { fclose(g_file); g_file = nullptr; }
        g_logPath = g_log.dir + L"\\handeye.log";
        MakeDirTree(g_log.dir);
        EnsureFileOpen();
    }
}

// ------------------------------ 解析 ------------------------------

// 按 " | " 切分，最多 maxParts 段；最后一段保留剩余内容
static std::vector<std::wstring> SplitPipe(const std::wstring& s, size_t maxParts) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (out.size() + 1 < maxParts) {
        size_t p = s.find(L" | ", start);
        if (p == std::wstring::npos) break;
        out.push_back(s.substr(start, p - start));
        start = p + 3;
    }
    out.push_back(s.substr(start));
    return out;
}

bool LogParseLine(const std::wstring& lineIn, LogRecord& out) {
    std::wstring line = lineIn;
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
    if (line.size() < 25) return false;

    auto parts = SplitPipe(line, 7);
    if (parts.size() < 6) return false;

    out.time = Trim(parts[0]);
    out.levelName = Trim(parts[1]);
    out.source = Trim(parts[2]);
    out.eventId = _wtoi(Trim(parts[3]).c_str());
    out.category = Trim(parts[4]);
    out.desc = Trim(parts[5]);
    out.detail = parts.size() >= 7 ? Trim(parts[6]) : L"";
    out.level = LogLevelFromName(out.levelName);
    out.raw = line;
    return out.eventId > 0 || !out.desc.empty();
}

std::vector<LogRecord> LogReadAll(int maxRecords) {
    std::vector<LogRecord> out;
    std::vector<std::wstring> files;

    WIN32_FIND_DATAW fd;
    std::wstring pattern = g_log.dir + L"\\handeye_*.log";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(g_log.dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());          // 文件名带时间戳，排序即时间序
    files.push_back(g_log.dir + L"\\handeye.log");  // 活动文件最后

    for (const auto& path : files) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); continue; }
        std::string bytes(size_t(sz), '\0');
        size_t got = fread(&bytes[0], 1, size_t(sz), f);
        fclose(f);
        bytes.resize(got);

        std::wstring wide = FromUtf8(bytes);
        for (auto& line : Split(wide, L'\n')) {
            LogRecord rec;
            if (LogParseLine(line, rec)) out.push_back(std::move(rec));
            if ((int)out.size() >= maxRecords) return out;
        }
    }
    return out;
}

// ------------------------------ 事件目录 ------------------------------

// 事件 ID 分段：来源各占一千，和 Windows 每个来源一段是同一种思路
static const std::vector<LogEventDef> kCatalog = {
    // 1xxx 核心
    { 1000, L"系统", L"启动",  L"程序启动",            L"argv=..." },
    { 1001, L"系统", L"启动",  L"日志系统就绪",         L"dir=... level=..." },
    { 1002, L"系统", L"停止",  L"程序退出",            L"rc=0 ms=..." },
    { 1003, L"系统", L"配置",  L"参数解析失败",         L"arg=..." },
    { 1004, L"系统", L"配置",  L"未知命令",            L"cmd=..." },
    { 1005, L"系统", L"配置",  L"未知选项",            L"opt=..." },

    // 2xxx 崩溃
    { 2000, L"崩溃", L"异常",  L"未捕获的 SEH 异常",    L"code=0x... addr=0x..." },
    { 2001, L"崩溃", L"异常",  L"未捕获的 C++ 异常",     L"" },
    { 2002, L"崩溃", L"异常",  L"无效参数 (CRT)",       L"" },
    { 2003, L"崩溃", L"异常",  L"内存分配失败",         L"bytes=..." },

    // 3xxx 感知（眼）
    { 3000, L"感知", L"感知",  L"截屏开始",            L"x= y= w= h= scale=" },
    { 3001, L"感知", L"感知",  L"截图保存",            L"path= bytes=" },
    { 3002, L"感知", L"感知",  L"OCR 完成",            L"raw= ms=" },
    { 3003, L"感知", L"感知",  L"筛子完成",            L"blocks= paras= ms=" },
    { 3004, L"感知", L"感知",  L"查找命中",            L"query= hits= score=" },
    { 3005, L"感知", L"感知",  L"查找未命中",           L"query= ms=" },
    { 3006, L"感知", L"异常",  L"OCR 引擎不可用",       L"reason=" },
    { 3007, L"感知", L"异常",  L"OCR 超时",            L"ms=" },
    { 3008, L"感知", L"异常",  L"截屏失败",            L"err=" },
    { 3009, L"感知", L"感知",  L"屏幕指纹",            L"hash= distance= changed=" },
    { 3010, L"感知", L"感知",  L"等待文字超时",         L"query= timeout=" },
    { 3011, L"感知", L"感知",  L"等待文字出现",         L"query= ms=" },

    // 4xxx 动作（手）
    { 4000, L"动作", L"动作",  L"移动鼠标",            L"x= y= ms=" },
    { 4001, L"动作", L"动作",  L"点击",                L"x= y= btn=" },
    { 4002, L"动作", L"动作",  L"输入文字",            L"text= len=" },
    { 4003, L"动作", L"动作",  L"按键",                L"key= vk=" },
    { 4004, L"动作", L"动作",  L"按住/释放",            L"key= down=" },
    { 4005, L"动作", L"动作",  L"切换窗口",            L"target= title= ok=" },
    { 4006, L"动作", L"异常",  L"SendInput 被系统丢弃", L"sent= got= err=" },
    { 4007, L"动作", L"感知",  L"等待窗口超时",         L"target= timeout=" },
    { 4008, L"动作", L"动作",  L"滚轮",                L"x= y= delta=" },
    { 4009, L"动作", L"动作",  L"按文字点击",           L"text= x= y= score=" },
    { 4010, L"动作", L"异常",  L"键名无法识别",         L"key=" },

    // 5xxx 语音（嘴）
    { 5000, L"语音", L"语音",  L"播报开始",            L"chars= voice= rate= volume=" },
    { 5001, L"语音", L"语音",  L"播报完成",            L"ms=" },
    { 5002, L"语音", L"语音",  L"导出 WAV",            L"path= bytes=" },
    { 5003, L"语音", L"异常",  L"语音引擎不可用",       L"reason=" },
    { 5004, L"语音", L"异常",  L"语音合成失败",         L"err=" },

    // 6xxx 脚本 / 录制
    { 6000, L"脚本", L"脚本",  L"脚本开始",            L"path= lines=" },
    { 6001, L"脚本", L"脚本",  L"脚本行执行",           L"line= cmd= ms=" },
    { 6002, L"脚本", L"异常",  L"脚本行失败",           L"line= cmd= reason=" },
    { 6003, L"脚本", L"脚本",  L"脚本完成",            L"ok= lines= ms=" },
    { 6004, L"脚本", L"脚本",  L"录制开始",            L"path=" },
    { 6005, L"脚本", L"脚本",  L"录制结束",            L"path= events=" },
    { 6006, L"脚本", L"脚本",  L"回放开始",            L"path= events= loop=" },
    { 6007, L"脚本", L"脚本",  L"回放完成",            L"loops= ms=" },
    { 6008, L"脚本", L"异常",  L"录制文件损坏或截断",    L"path= reason=" },
    { 6009, L"脚本", L"停止",  L"收到停止信号",         L"" },

    // 7xxx 配置 / 状态
    { 7000, L"配置", L"数据",  L"状态文件读取失败",      L"path=" },
    { 7001, L"配置", L"数据",  L"状态文件写入失败",      L"path=" },
    { 7002, L"配置", L"数据",  L"日志滚动",            L"from= to= bytes=" },
    { 7003, L"配置", L"异常",  L"日志写入失败",         L"path= err=" },
    { 7004, L"配置", L"配置",  L"配置来源",            L"source= level= text=" },

    // 8xxx 会话
    { 8000, L"会话", L"会话",  L"会话开始",            L"argv= cwd=" },
    { 8001, L"会话", L"会话",  L"会话结束",            L"rc= ms=" },
};

const std::vector<LogEventDef>& LogEventCatalog() { return kCatalog; }
