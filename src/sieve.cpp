// ============================================================================
//  sieve.cpp —— 筛子实现
// ============================================================================
#include "sieve.h"
#include "eye.h"

#include <cstdio>
#include <cwctype>
#include <unordered_set>
#include <map>

// ------------------------------ 1. 文本清洗 ------------------------------

// OCR 对中文常按字切词，line.Text() 会是 "对 话 轨 迹"。
// 规则：空格两侧都是 CJK 字符时，去掉这个空格。
static std::wstring JoinCjkSpaces(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L' ' || c == L'\t' || c == L'\u3000') {
            wchar_t prev = out.empty() ? 0 : out.back();
            size_t j = i;
            while (j < in.size() && (in[j] == L' ' || in[j] == L'\t' || in[j] == L'\u3000')) ++j;
            wchar_t next = (j < in.size()) ? in[j] : 0;
            if (IsCjk(prev) && IsCjk(next)) { i = j - 1; continue; }
            if (!out.empty() && out.back() != L' ') out += L' ';
            i = j - 1;
            continue;
        }
        out += c;
    }
    return Trim(out);
}

// 按钮词判断在下面定义（判型时才用），噪声过滤要提前用，先声明
static bool IsButtonWord(const std::wstring& s);

// 判断是不是纯噪声：全是标点、或者单个非中文短字符
static bool IsNoise(const std::wstring& s) {
    if (s.empty()) return true;
    int meaningful = 0;
    int symbols = 0;
    for (wchar_t c : s) {
        if (iswalnum(c) || IsCjk(c)) {
            // CJK 标点算符号，不算有意义内容
            if (IsCjk(c) && !iswalnum(c) && c >= 0x3000 && c <= 0x303F) symbols++;
            else meaningful++;
        } else if (!iswspace(c)) {
            symbols++;
        }
    }
    if (meaningful == 0) return true;
    // 单个字符基本是 OCR 碎片（"囗" "老" 这类），除非它本身是按钮词
    if (s.size() == 1 && !IsButtonWord(s)) return true;
    // 符号比内容还多，多半是乱码
    if (symbols > meaningful * 2) return true;
    return false;
}

// ------------------------------ 2. 归并 ------------------------------

// 把 OCR 的碎行合并成视觉行。
// 只有"几乎贴在一起、竖直方向高度重合"的碎片才合并，避免把同一行上
// 相距很远的两块界面文字拼成一句。
static std::vector<OcrLine> MergeSplitLines(const std::vector<OcrLine>& in) {
    if (in.size() < 2) return in;

    std::vector<OcrLine> sorted = in;
    std::sort(sorted.begin(), sorted.end(), [](const OcrLine& a, const OcrLine& b) {
        if (a.box.y != b.box.y) return a.box.y < b.box.y;
        return a.box.x < b.box.x;
    });

    std::vector<OcrLine> out;
    std::vector<bool> used(sorted.size(), false);
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (used[i]) continue;
        OcrLine cur = sorted[i];
        used[i] = true;
        for (size_t j = i + 1; j < sorted.size(); ++j) {
            if (used[j]) continue;
            const auto& o = sorted[j];
            if (VerticalOverlap(cur.box, o.box) < 0.8) continue;
            int gap = o.box.x - cur.box.right();
            int lineH = (std::max)(cur.box.h, o.box.h);
            if (gap < 0) gap = 0;
            // 只合并紧贴的碎片（小于 0.35 个行高）
            if (gap > lineH * 0.35) continue;
            cur.text = JoinCjkSpaces(cur.text) + L" " + JoinCjkSpaces(o.text);
            int right = (std::max)(cur.box.right(), o.box.right());
            int bottom = (std::max)(cur.box.bottom(), o.box.bottom());
            cur.box.w = right - cur.box.x;
            cur.box.h = bottom - cur.box.y;
            used[j] = true;
        }
        cur.text = JoinCjkSpaces(cur.text);
        out.push_back(std::move(cur));
    }
    return out;
}

// 按纵向间距聚成区块；水平方向完全不相交时也断开（把侧栏和正文分开）
static std::vector<Block> ClusterBlocks(const std::vector<OcrLine>& lines,
                                        const std::vector<int>& winOf,
                                        const std::vector<WinInfo>& wins) {
    struct Tagged {
        OcrLine line;
        int     win;
    };

    std::vector<Tagged> sorted;
    sorted.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        Tagged t;
        t.line = lines[i];
        t.win = (i < winOf.size()) ? winOf[i] : -1;
        sorted.push_back(std::move(t));
    }

    // Group by window before anything else. Windows come in Z order, so the
    // topmost window is emitted first. Lines not inside any window go last.
    std::sort(sorted.begin(), sorted.end(), [](const Tagged& a, const Tagged& b) {
        int za = (a.win < 0) ? 100000 : a.win;
        int zb = (b.win < 0) ? 100000 : b.win;
        if (za != zb) return za < zb;
        return a.line.box.y < b.line.box.y;
    });

    std::vector<int> heights;
    for (const auto& t : sorted) if (t.line.box.h > 0) heights.push_back(t.line.box.h);
    int medH = 16;
    if (!heights.empty()) {
        std::sort(heights.begin(), heights.end());
        medH = heights[heights.size() / 2];
    }

    // Adaptive gap: measure the typical distance between neighbouring lines
    // instead of using a fixed number.
    std::vector<int> gaps;
    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i].win != sorted[i - 1].win) continue;
        const RectI& a = sorted[i - 1].line.box;
        const RectI& b = sorted[i].line.box;
        int l = (std::max)(a.x, b.x);
        int r = (std::min)(a.right(), b.right());
        if (r - l <= 0) continue;
        int g = b.y - a.bottom();
        if (g >= 0) gaps.push_back(g);
    }
    int medGap = 0;
    if (!gaps.empty()) {
        std::sort(gaps.begin(), gaps.end());
        medGap = gaps[gaps.size() / 2];
    }
    int gapLimit = (std::max)(int(medGap * 2.5), (std::max)(medH, 12));

    auto xOverlap = [](const RectI& a, const RectI& b) {
        int l = (std::max)(a.x, b.x);
        int r = (std::min)(a.right(), b.right());
        return r - l;
    };

    std::vector<Block> blocks;
    Block cur;
    bool has = false;
    int  curWin = -2;

    for (const auto& t : sorted) {
        const OcrLine& l = t.line;
        bool startNew = !has || t.win != curWin;
        if (has && !startNew) {
            int gap = l.box.y - cur.box.bottom();
            bool sameColumn = xOverlap(cur.box, l.box) > 0;
            if (gap > gapLimit || !sameColumn) startNew = true;
        }

        if (startNew) {
            if (has) blocks.push_back(std::move(cur));
            cur = Block{};
            cur.box = l.box;
            cur.items.push_back(LineItem{ l.text, l.box });
            curWin = t.win;
            cur.winZ = t.win;
            if (t.win >= 0 && t.win < (int)wins.size()) cur.window = wins[t.win].title;
            has = true;
        } else {
            int right = (std::max)(cur.box.right(), l.box.right());
            int bottom = (std::max)(cur.box.bottom(), l.box.bottom());
            cur.box.w = right - cur.box.x;
            cur.box.h = bottom - cur.box.y;
            cur.items.push_back(LineItem{ l.text, l.box });
        }
    }
    if (has) blocks.push_back(std::move(cur));
    return blocks;
}

// ------------------------------ 3. 判型 ------------------------------

// 按钮词：必须整行等于才算，不能用子串匹配。
// 早期版本用子串匹配，结果正文里出现一个"是"字就被判成对话框。
static const std::vector<std::wstring> kButtonWords = {
    L"确定", L"取消", L"确认", L"关闭", L"应用", L"保存", L"是", L"否",
    L"好", L"知道了", L"删除", L"重试", L"忽略", L"下一步", L"完成",
    L"OK", L"Cancel", L"Yes", L"No", L"Apply", L"Save", L"Close", L"Retry",
    L"Delete", L"Next", L"Finish", L"Done"
};

static bool IsButtonWord(const std::wstring& s) {
    std::wstring t = Trim(s);
    for (wchar_t& c : t) c = towupper(c);
    for (const auto& w : kButtonWords) {
        std::wstring ww = w;
        for (wchar_t& c : ww) c = towupper(c);
        if (t == ww) return true;
    }
    return false;
}

static void Classify(Block& b, const RectI& screen) {
    const int W = screen.w, H = screen.h;
    if (W <= 0 || H <= 0) { b.kind = L"content"; return; }

    // 整块文字
    std::wstring all;
    for (const auto& it : b.items) { all += it.text; all += L" "; }

    // 有多少行"整行就是一个按钮词"
    int buttonItems = 0;
    for (const auto& it : b.items) if (IsButtonWord(it.text)) buttonItems++;

    bool topBand = b.box.y < H * 0.07;
    bool bottomBand = b.box.y > H * 0.90;
    bool leftBand = b.box.x < W * 0.18;
    bool tall = b.box.h > H * 0.25;
    bool wide = b.box.w > W * 0.35;
    bool shortH = b.box.h < H * 0.10;
    bool centeredX = std::abs(b.box.cx() - W / 2) < W * 0.18;
    bool notFullW = b.box.w < W * 0.75;
    bool notFullH = b.box.h < H * 0.55;
    bool fewLines = b.items.size() <= 10;

    if (topBand && wide) { b.kind = L"title"; return; }
    if (bottomBand && wide) { b.kind = L"status"; return; }
    if (leftBand && tall) { b.kind = L"sidebar"; return; }
    // 工具栏只可能在最上面一条；阈值放宽会把正文首行误判成工具栏
    if (shortH && b.box.y < H * 0.10 && wide) { b.kind = L"toolbar"; return; }

    // 对话框：居中、不满屏、行数少，且有至少两个"整行就是按钮词"
    if (centeredX && notFullW && notFullH && fewLines && buttonItems >= 2) {
        b.kind = L"dialog";
        return;
    }

    // 单个按钮：一行、很短、整行就是按钮词
    if (b.items.size() == 1 && b.box.w < W * 0.25 && b.box.h < H * 0.08 &&
        buttonItems == 1) {
        b.kind = L"button";
        return;
    }

    b.kind = L"content";
}

// ------------------------------ 4. 变化检测 ------------------------------

static std::wstring NormalizeForCompare(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (iswspace(c)) continue;
        o += towlower(c);
    }
    return o;
}

std::wstring HandeyeStateDir() {
    std::wstring dir;
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) dir = buf;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH] = {};
        DWORD m = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p(exe, m);
        size_t slash = p.find_last_of(L"\\/");
        dir = (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
    }
    dir += L"\\handeye";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring StateFilePath(const std::wstring& prefix, const std::wstring& key) {
    std::wstring k = key.empty() ? L"default" : key;
    for (wchar_t& c : k) {
        if (!iswalnum(c) && c != L'-' && c != L'_') c = L'_';
    }
    return HandeyeStateDir() + L"\\" + prefix + L"." + k + L".txt";
}

std::wstring DefaultStatePath() {
    return StateFilePath(L"last_frame", L"default");
}

static std::vector<std::wstring> LoadPrevious(const std::wstring& path) {
    std::vector<std::wstring> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return out;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        std::vector<char> buf(size_t(sz) + 1, 0);
        size_t got = fread(buf.data(), 1, size_t(sz), f);
        std::string utf8(buf.data(), got);
        std::wstring w = FromUtf8(utf8);
        for (auto& line : Split(w, L'\n')) {
            std::wstring t = Trim(line);
            if (!t.empty()) out.push_back(t);
        }
    }
    fclose(f);
    return out;
}

static void SaveCurrent(const std::wstring& path, const std::vector<std::wstring>& lines) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    std::wstring all;
    for (const auto& l : lines) { all += NormalizeForCompare(l); all += L'\n'; }
    std::string utf8 = ToUtf8(all);
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
}

// ------------------------------ 主流程 ------------------------------

SieveResult SieveRun(const std::vector<OcrLine>& linesIn, const SieveOptions& opt) {
    SieveResult res;
    const RectI screen = EyeScreenRect();

    // 1) 清洗
    std::vector<OcrLine> cleaned;
    cleaned.reserve(linesIn.size());
    for (const auto& l : linesIn) {
        OcrLine c = l;
        c.text = JoinCjkSpaces(l.text);
        if (IsNoise(c.text)) continue;
        if (c.box.w <= 0 || c.box.h <= 0) continue;
        cleaned.push_back(std::move(c));
    }

    // 2) 归并
    std::vector<OcrLine> merged = MergeSplitLines(cleaned);

    // Attribute each line to the window that actually covers it. This is real
    // information from the OS, not a guess about geometry.
    res.windows = EnumTopWindows();
    std::vector<int> winOf(merged.size(), -1);
    for (size_t i = 0; i < merged.size(); ++i) {
        winOf[i] = WindowAtPoint(res.windows, merged[i].box.cx(), merged[i].box.cy());
    }

    // 3) 聚块 + 判型
    res.blocks = ClusterBlocks(merged, winOf, res.windows);
    for (auto& b : res.blocks) Classify(b, screen);

    // 4) 排序：标题、对话框、工具栏优先，正文在后
    auto rank = [](const std::wstring& k) {
        if (k == L"title")   return 0;
        if (k == L"dialog")  return 1;
        if (k == L"button")  return 2;
        if (k == L"toolbar") return 3;
        if (k == L"sidebar") return 4;
        if (k == L"content") return 5;
        return 6;
    };
    std::stable_sort(res.blocks.begin(), res.blocks.end(), [&](const Block& a, const Block& b) {
        int ra = rank(a.kind), rb = rank(b.kind);
        if (ra != rb) return ra < rb;
        return a.box.y < b.box.y;
    });

    // 5) 变化检测
    //    必须在裁剪之前做。先按 maxLines 截断再比对的话，
    //    "新内容把老内容挤出输出"会被误报成"老文字消失了"。
    if (opt.doDelta) {
        std::wstring path = opt.statePath;
        if (path.empty()) path = StateFilePath(L"last_frame", opt.stateKey);
        std::vector<std::wstring> prev = LoadPrevious(path);

        std::vector<std::wstring> now;
        for (const auto& b : res.blocks)
            for (const auto& it : b.items)
                now.push_back(NormalizeForCompare(it.text));

        std::unordered_set<std::wstring> prevSet(prev.begin(), prev.end());
        std::unordered_set<std::wstring> nowSet(now.begin(), now.end());

        for (const auto& l : now)
            if (!prevSet.count(l)) { res.newLines++; if (res.added.size() < 12) res.added.push_back(l); }
        for (const auto& l : prev)
            if (!nowSet.count(l)) { res.goneLines++; if (res.removed.size() < 12) res.removed.push_back(l); }

        res.changed = (res.newLines > 0 || res.goneLines > 0);
        SaveCurrent(path, now);
    }

    // 6) 裁剪
    if (!opt.keepAll) {
        int kept = 0;
        std::vector<Block> filtered;
        for (auto& b : res.blocks) {
            if (kept >= opt.maxLines) break;
            Block nb = b;
            if (nb.items.size() > size_t(opt.maxLines)) nb.items.resize(opt.maxLines);
            for (auto& it : nb.items) {
                if ((int)it.text.size() > opt.maxLineChars)
                    it.text = it.text.substr(0, opt.maxLineChars) + L"…";
            }
            kept += (int)nb.items.size();
            filtered.push_back(std::move(nb));
        }
        res.blocks.swap(filtered);
    }

    return res;
}

// ------------------------------ 模糊查找 ------------------------------

// 在长句里找与 needle 最像的一段（滑动窗口编辑距离）。
// 直接用整句编辑距离的话，"中文测试第二行" 和 "第二冇" 会被判成完全不同。
static int BestWindowDistance(const std::wstring& hay, const std::wstring& needle) {
    const size_t n = needle.size();
    if (hay.size() <= n) return EditDistance(hay, needle);
    int best = 1 << 30;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        int d = EditDistance(hay.substr(i, n), needle);
        if (d < best) {
            best = d;
            if (best == 0) break;
        }
    }
    return best;
}

std::vector<FindHit> SieveFind(const std::vector<OcrLine>& lines,
                               const std::wstring& needle,
                               int maxHits) {
    std::vector<FindHit> hits;
    std::wstring target = NormalizeForCompare(needle);
    if (target.empty()) return hits;

    // 允许的错字数：按目标长度给，至少允许 1 个
    const int allowed = (std::max)(1, int(target.size() / 4));

    for (const auto& l : lines) {
        std::wstring text = JoinCjkSpaces(l.text);
        std::wstring norm = NormalizeForCompare(text);
        if (norm.empty()) continue;

        double score = 0.0;
        if (norm.find(target) != std::wstring::npos) {
            // 子串命中：完全相等得 1.0，否则按长度比例
            score = (norm == target)
                        ? 1.0
                        : 0.85 + 0.15 * (double(target.size()) / (double)norm.size());
        } else {
            int d = BestWindowDistance(norm, target);
            if (d <= allowed) {
                double sim = 1.0 - double(d) / double(target.size());
                score = 0.9 * sim;
            }
        }
        if (score > 0) hits.push_back(FindHit{ text, l.box, score });
    }

    // 排序：匹配度为主，短文本略微加权。
    // 短标签更可能是按钮/菜单项，长段落里的命中多半是正文里的偶合。
    auto rank = [](const FindHit& h) {
        double lenFactor = 1.0 - (std::min)(1.0, double(h.text.size()) / 40.0);
        return h.score + 0.3 * lenFactor;
    };
    std::sort(hits.begin(), hits.end(), [&](const FindHit& a, const FindHit& b) {
        return rank(a) > rank(b);
    });
    if ((int)hits.size() > maxHits) hits.resize(maxHits);
    return hits;
}
