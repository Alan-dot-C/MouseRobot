// ============================================================================
//  log.h —— 系统日志（对标 Windows 事件日志）
//
//  设计来源：
//    - Windows 事件日志的六根柱子：级别 / 时间 / 来源 / 事件 ID / 任务类别 /
//      结构化数据。级别固定五档且从小到大表示严重度递减。
//    - Windows 自己的文本日志（CBS.log）：`时间, 级别 组件 消息`，
//      活动文件涨到 2MB 就改名带时间戳，老的压成 .cab。
//    - 本项目安卓端 AppLog / LogFormat 定下的行格式（两边保持同一标准）：
//        时间 | 级别 | 来源 | 事件ID | 任务类别 | 描述 | 详细信息
//
//  三条铁律（继承自安卓端）：
//    1. 日志系统本身绝不能把程序搞崩 —— 所有写操作吞异常
//    2. 写盘在调用线程，不做异步攒批 —— 崩溃前最后一行必须已经在盘上
//    3. 严重/错误/警告 永远不被级别过滤掉
// ============================================================================
#pragma once

#include "common.h"
#include <string>
#include <vector>

// ------------------------------ 级别（对齐 Windows 1-5） ------------------------------

enum LogLevel : int {
    LOG_CRITICAL = 1,   // 严重：进程要挂了，或数据可能已丢
    LOG_ERROR    = 2,   // 错误：某个功能失败，但程序还能继续
    LOG_WARNING  = 3,   // 警告：没失败，但偏离预期
    LOG_INFO     = 4,   // 信息：正常操作与状态变化
    LOG_VERBOSE  = 5,   // 详细：排障细节
};

// 文本落盘策略
enum LogTextMode {
    LOG_TEXT_FULL = 0,  // 原样记录
    LOG_TEXT_MASK = 1,  // 只记长度和首尾
    LOG_TEXT_OFF  = 2,  // 完全不记内容
};

struct LogConfig {
    std::wstring dir;                 // 日志目录
    int          level = LOG_VERBOSE; // 默认全记
    LogTextMode  textMode = LOG_TEXT_FULL;
    int          maxFileBytes = 4 * 1024 * 1024;
    int          keepFiles = 30;
    bool         enabled = true;
    bool         echoToStderr = false;   // 是否同时打到 stderr
};

extern LogConfig g_log;

// ------------------------------ 生命周期 ------------------------------

// 读取配置（环境变量 / config.ini）、创建目录、生成会话号、安装崩溃钩子
void LogInit();
void LogShutdown();

// 命令行覆盖（在 LogInit 之后调用）。只覆盖传入的项。
// hasDir/hasLevel/hasText/hasEnabled 为 false 表示这一项不改。
void LogConfigure(bool hasDir, const std::wstring& dir,
                  bool hasLevel, int level,
                  bool hasText, int textMode,
                  bool hasEnabled, bool enabled);

// 会话标识：每次进程启动生成一个 5 位十六进制
const std::wstring& LogSessionId();
DWORD LogStartTick();

// 关闭活动文件句柄（查询前用）。下一次 LogWrite 会自动重新打开。
void LogCloseFile();

// 崩溃钩子：未捕获异常/SEH 时把飞行记录器落盘
void LogInstallCrashHandlers();

// ------------------------------ 写事件 ------------------------------

// detail 为可选的 key=value 串，会自动加上 [S=xxxx P=nnnn] 前缀
void LogWrite(int level, const wchar_t* source, int eventId,
              const wchar_t* category, const wchar_t* desc,
              const wchar_t* detailFmt = nullptr, ...);

// 便捷入口
void LogC(const wchar_t* src, int id, const wchar_t* cat, const wchar_t* desc, const wchar_t* fmt = nullptr, ...);
void LogE(const wchar_t* src, int id, const wchar_t* cat, const wchar_t* desc, const wchar_t* fmt = nullptr, ...);
void LogW(const wchar_t* src, int id, const wchar_t* cat, const wchar_t* desc, const wchar_t* fmt = nullptr, ...);
void LogI(const wchar_t* src, int id, const wchar_t* cat, const wchar_t* desc, const wchar_t* fmt = nullptr, ...);
void LogV(const wchar_t* src, int id, const wchar_t* cat, const wchar_t* desc, const wchar_t* fmt = nullptr, ...);

// 按隐私策略处理用户文本，返回可直接写进 detail 的片段
std::wstring LogRedactText(const std::wstring& text);

// ------------------------------ 读事件（查询用） ------------------------------

struct LogRecord {
    std::wstring time;       // "2026-10-17 18:30:12.345"
    std::wstring levelName;  // 严重/错误/警告/信息/详细
    std::wstring source;     // 系统/崩溃/感知/动作/语音/脚本/配置/会话
    int          eventId = 0;
    std::wstring category;   // 启动/感知/动作/异常/...
    std::wstring desc;       // 描述
    std::wstring detail;     // 详细信息（含 [S=.. P=..] 前缀）
    int          level = 0;  // 1..5
    std::wstring raw;        // 原始整行
};

// 按时间顺序读出所有日志（活动文件 + 轮转文件）
std::vector<LogRecord> LogReadAll(int maxRecords = 200000);

// 解析单行；失败返回 false
bool LogParseLine(const std::wstring& line, LogRecord& out);

// 级别名 <-> 数值
const wchar_t* LogLevelName(int level);
int LogLevelFromName(const std::wstring& name);

// 日志目录（含配置回退）
std::wstring LogDefaultDir();
std::wstring LogFilePath();

// 事件目录（给 `handeye log schema` 用）
struct LogEventDef {
    int          id;
    const wchar_t* source;
    const wchar_t* category;
    const wchar_t* desc;
    const wchar_t* detailHint;
};
const std::vector<LogEventDef>& LogEventCatalog();
