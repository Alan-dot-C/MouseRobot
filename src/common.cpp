// ============================================================================
//  common.cpp —— 公共工具实现
// ============================================================================
#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <clocale>
#include <cwctype>
#include <io.h>
#include <fcntl.h>

Options g_opt;

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

void InitConsoleUtf8() {
    // SetConsoleOutputCP 只管控制台窗口；stdout 被重定向到管道时 CRT 仍按 ANSI 转换。
    // 把 locale 与流模式都设成 UTF-8，wprintf 就直接吐 UTF-8 字节。
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, ".UTF-8");
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

void ReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool StartsWith(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::wstring::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::wstring ToUpper(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& c : o) c = towupper(c);
    return o;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    return o;
}

bool IsCjk(wchar_t c) {
    // CJK 统一表意文字、扩展 A、CJK 标点、全角形式、日文假名、韩文
    return (c >= 0x4E00 && c <= 0x9FFF) ||
           (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0x3000 && c <= 0x303F) ||
           (c >= 0xFF00 && c <= 0xFFEF) ||
           (c >= 0x3040 && c <= 0x30FF) ||
           (c >= 0xAC00 && c <= 0xD7AF);
}

int EditDistance(const std::wstring& a, const std::wstring& b) {
    const size_t n = a.size(), m = b.size();
    if (n == 0) return (int)m;
    if (m == 0) return (int)n;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = (std::min)({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        prev.swap(cur);
    }
    return prev[m];
}

// ------------------------------ 输出 ------------------------------

// --json 模式下，Say() 的内容不再直接打印（会破坏单行 JSON），
// 而是攒在这里，最后由 JsonDoc::Emit 合并成 "warnings":[...]。
static std::vector<std::wstring> g_warnings;
static bool g_jsonEmitted = false;

bool JsonWasEmitted() { return g_jsonEmitted; }

void Say(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[4096];
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_opt.json) {
        g_warnings.push_back(buf);
    } else {
        wprintf(L"%ls\n", buf);
    }
}

std::vector<std::wstring> TakePendingMessages() {
    std::vector<std::wstring> t;
    t.swap(g_warnings);
    return t;
}

void ResetPendingMessages() { g_warnings.clear(); }

std::wstring JsonStr(const std::wstring& s) {
    return FromUtf8(JsonEscape(ToUtf8(s)));
}

JsonDoc& JsonDoc::Raw(const std::wstring& key, const std::wstring& rawJson) {
    fields_.emplace_back(key, rawJson);
    return *this;
}

JsonDoc& JsonDoc::Str(const std::wstring& key, const std::wstring& value) {
    return Raw(key, L"\"" + JsonStr(value) + L"\"");
}

JsonDoc& JsonDoc::Int(const std::wstring& key, long long value) {
    return Raw(key, std::to_wstring(value));
}

JsonDoc& JsonDoc::Bool(const std::wstring& key, bool value) {
    return Raw(key, value ? L"true" : L"false");
}

JsonDoc& JsonDoc::Dbl(const std::wstring& key, double value, int decimals) {
    wchar_t buf[64] = {};
    swprintf_s(buf, L"%.*f", decimals, value);
    return Raw(key, buf);
}

std::wstring JsonDoc::Emit() {
    g_jsonEmitted = true;
    std::wstring s = L"{";
    bool first = true;
    for (const auto& f : fields_) {
        if (!first) s += L",";
        first = false;
        s += L"\"" + JsonStr(f.first) + L"\":" + f.second;
    }

    auto warns = TakePendingMessages();
    if (!warns.empty()) {
        if (!first) s += L",";
        s += L"\"warnings\":[";
        for (size_t i = 0; i < warns.size(); ++i) {
            if (i) s += L",";
            s += L"\"" + JsonStr(warns[i]) + L"\"";
        }
        s += L"]";
    }

    s += L"}";
    return s;
}

// ------------------------------ 窗口 / 光标 ------------------------------

std::wstring GetForegroundTitle() {
    HWND h = GetForegroundWindow();
    if (!h) return L"";
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return L"";
    std::wstring buf(size_t(len) + 1, L'\0');
    int got = GetWindowTextW(h, buf.data(), len + 1);
    buf.resize(got > 0 ? size_t(got) : 0);
    return buf;
}

POINT GetCursorPosNow() {
    POINT p{ 0, 0 };
    GetCursorPos(&p);
    return p;
}
