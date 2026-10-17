// ============================================================================
//  uia.cpp —— UI Automation 的薄封装
// ============================================================================
#include "uia.h"
#include "winenum.h"

#include <algorithm>
#include <objbase.h>

using winrt::com_ptr;

static com_ptr<IUIAutomation> g_uia;

bool UiAvailable() { return g_uia != nullptr; }

bool UiInit() {
    if (g_uia) return true;
    try {
        com_ptr<IUIAutomation> uia;
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()));
        if (FAILED(hr) || !uia) return false;

        // 卡死的应用会把 UIA 调用拖到默认的几秒甚至几十秒。
        // 设成 400ms 就放弃，让调用方回落到 OCR。
        com_ptr<IUIAutomation2> u2;
        if (SUCCEEDED(uia->QueryInterface(IID_PPV_ARGS(u2.put())))) {
            u2->put_ConnectionTimeout(400);
            u2->put_TransactionTimeout(400);
        }
        g_uia = uia;
        return true;
    } catch (...) {
        return false;
    }
}

// ------------------------------ 属性读取 ------------------------------

static std::wstring TakeBstr(BSTR b) {
    std::wstring s = b ? b : L"";
    if (b) SysFreeString(b);
    return s;
}

static std::wstring RoleName(CONTROLTYPEID id) {
    switch (id) {
    case UIA_ButtonControlTypeId:    return L"button";
    case UIA_CheckBoxControlTypeId:  return L"checkbox";
    case UIA_ComboBoxControlTypeId:  return L"combobox";
    case UIA_EditControlTypeId:      return L"edit";
    case UIA_HyperlinkControlTypeId: return L"link";
    case UIA_ImageControlTypeId:     return L"image";
    case UIA_ListItemControlTypeId:  return L"listitem";
    case UIA_ListControlTypeId:      return L"list";
    case UIA_MenuControlTypeId:      return L"menu";
    case UIA_MenuBarControlTypeId:   return L"menubar";
    case UIA_MenuItemControlTypeId:  return L"menuitem";
    case UIA_RadioButtonControlTypeId: return L"radio";
    case UIA_ScrollBarControlTypeId: return L"scrollbar";
    case UIA_SliderControlTypeId:    return L"slider";
    case UIA_StatusBarControlTypeId: return L"statusbar";
    case UIA_TabControlTypeId:       return L"tab";
    case UIA_TabItemControlTypeId:   return L"tabitem";
    case UIA_TextControlTypeId:      return L"text";
    case UIA_ToolBarControlTypeId:   return L"toolbar";
    case UIA_TreeControlTypeId:      return L"tree";
    case UIA_TreeItemControlTypeId:  return L"treeitem";
    case UIA_WindowControlTypeId:    return L"window";
    case UIA_PaneControlTypeId:      return L"pane";
    case UIA_GroupControlTypeId:     return L"group";
    case UIA_DocumentControlTypeId:  return L"document";
    case UIA_TableControlTypeId:     return L"table";
    case UIA_TitleBarControlTypeId:  return L"titlebar";
    case UIA_DataItemControlTypeId:  return L"dataitem";
    case UIA_SplitButtonControlTypeId: return L"splitbutton";
    default:                         return L"custom";
    }
}

// 一次读取一个元素。返回 false 表示这个元素不值得收（没名字、没值、不可见）。
static bool ReadElement(IUIAutomationElement* el, UiNode& n) {
    if (!el) return false;

    BSTR name = nullptr;
    el->get_CurrentName(&name);
    n.name = TakeBstr(name);

    CONTROLTYPEID type = 0;
    el->get_CurrentControlType(&type);
    n.role = RoleName(type);

    RECT r{};
    el->get_CurrentBoundingRectangle(&r);
    n.rect = RectI{ (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top) };

    BOOL b = FALSE;
    el->get_CurrentIsEnabled(&b);
    n.enabled = b != FALSE;
    el->get_CurrentIsOffscreen(&b);
    n.offscreen = b != FALSE;

    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_IsInvokePatternAvailablePropertyId, &v)))
        n.invokable = (v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE);
    VariantClear(&v);

    // 编辑框的值往往比名字有用
    if (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId) {
        com_ptr<IUIAutomationValuePattern> vp;
        BSTR val = nullptr;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(vp.put()))) && vp) {
            if (SUCCEEDED(vp->get_CurrentValue(&val))) n.value = TakeBstr(val);
        }
    }

    if (n.name.empty() && n.value.empty()) return false;
    if (n.rect.w <= 0 || n.rect.h <= 0) return false;
    return true;
}

static void Walk(IUIAutomationElement* el, int depth, int maxDepth,
                 int maxNodes, DWORD deadline, IUIAutomationTreeWalker* walker,
                 std::vector<UiNode>& out) {
    if (!el || depth > maxDepth) return;
    if ((int)out.size() >= maxNodes) return;
    if ((int)(GetTickCount() - deadline) > 0) return;   // deadline 是绝对时刻

    UiNode n;
    n.el.copy_from(el);
    if (ReadElement(el, n)) out.push_back(std::move(n));
    if ((int)out.size() >= maxNodes) return;

    com_ptr<IUIAutomationElement> child;
    if (FAILED(walker->GetFirstChildElement(el, child.put())) || !child) return;

    while (child && (int)out.size() < maxNodes && (int)(GetTickCount() - deadline) <= 0) {
        Walk(child.get(), depth + 1, maxDepth, maxNodes, deadline, walker, out);
        com_ptr<IUIAutomationElement> next;
        if (FAILED(walker->GetNextSiblingElement(child.get(), next.put())) || !next) break;
        child = next;
    }
}

std::vector<UiNode> UiTree(HWND root, int maxDepth, int maxNodes, int timeoutMs) {
    std::vector<UiNode> out;
    if (!UiInit()) return out;
    if (maxDepth <= 0) maxDepth = 8;
    if (maxNodes <= 0) maxNodes = 300;
    if (timeoutMs <= 0) timeoutMs = 400;

    HWND hwnd = root ? root : GetForegroundWindow();
    if (!hwnd) return out;

    try {
        com_ptr<IUIAutomationElement> rootEl;
        if (FAILED(g_uia->ElementFromHandle(hwnd, rootEl.put())) || !rootEl) return out;

        com_ptr<IUIAutomationTreeWalker> walker;
        if (FAILED(g_uia->get_ControlViewWalker(walker.put())) || !walker) return out;

        DWORD deadline = GetTickCount() + DWORD(timeoutMs);
        // 根节点自己也算一个（窗口标题）
        UiNode rn;
        rn.el = rootEl;
        if (ReadElement(rootEl.get(), rn)) out.push_back(std::move(rn));

        com_ptr<IUIAutomationElement> child;
        if (SUCCEEDED(walker->GetFirstChildElement(rootEl.get(), child.put())) && child) {
            while (child && (int)out.size() < maxNodes && (int)(GetTickCount() - deadline) <= 0) {
                Walk(child.get(), 1, maxDepth, maxNodes, deadline, walker.get(), out);
                com_ptr<IUIAutomationElement> next;
                if (FAILED(walker->GetNextSiblingElement(child.get(), next.put())) || !next) break;
                child = next;
            }
        }
    } catch (...) {
    }
    return out;
}

// ------------------------------ 查找 ------------------------------

static int EditDistanceSimple(const std::wstring& a, const std::wstring& b) {
    return EditDistance(a, b);
}

static double UiMatchScore(const std::wstring& hay, const std::wstring& needle) {
    if (hay.empty() || needle.empty()) return 0.0;
    if (hay == needle) return 1.0;
    if (hay.find(needle) != std::wstring::npos) {
        // 名字越接近目标越好："确定" 比 "确定并关闭" 更像按钮
        double lenRatio = double(needle.size()) / double(hay.size());
        return 0.85 + 0.1 * lenRatio;
    }
    if (needle.size() <= 6) {
        int d = EditDistanceSimple(hay, needle);
        if (d <= 1) return 0.75;
    }
    return 0.0;
}

std::vector<UiNode> UiFind(const std::wstring& needle, int maxNodes, int timeoutMs) {
    std::vector<UiNode> hits;
    if (!UiInit() || needle.empty()) return hits;

    auto collect = [&](const std::vector<UiNode>& tree) {
        for (const auto& n : tree) {
            double s = UiMatchScore(n.name, needle);
            if (s <= 0) continue;
            UiNode h = n;
            h.name = n.name;
            hits.push_back(std::move(h));
        }
    };

    collect(UiTree(nullptr, 8, maxNodes ? maxNodes : 300, timeoutMs));

    // 当前窗口没找到，再看其它可见顶层窗口（最多 8 个，总预算翻倍）
    if (hits.empty()) {
        auto wins = EnumTopWindows();
        HWND fg = GetForegroundWindow();
        int tried = 0;
        for (const auto& w : wins) {
            if (w.hwnd == fg) continue;
            if (++tried > 8) break;
            collect(UiTree(w.hwnd, 6, 200, timeoutMs / 2));
        }
    }

    std::stable_sort(hits.begin(), hits.end(), [&](const UiNode& a, const UiNode& b) {
        return UiMatchScore(a.name, needle) > UiMatchScore(b.name, needle);
    });
    if (hits.size() > 8) hits.resize(8);
    return hits;
}

// ------------------------------ 动作 ------------------------------

bool UiInvoke(const UiNode& node) {
    if (!node.el) return false;
    try {
        com_ptr<IUIAutomationInvokePattern> inv;
        if (SUCCEEDED(node.el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(inv.put()))) && inv) {
            return SUCCEEDED(inv->Invoke());
        }
    } catch (...) {
    }
    return false;
}

bool UiGetText(const UiNode& node, std::wstring& out) {
    if (!node.el) return false;
    try {
        com_ptr<IUIAutomationTextPattern> tp;
        if (SUCCEEDED(node.el->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(tp.put()))) && tp) {
            com_ptr<IUIAutomationTextRange> range;
            if (SUCCEEDED(tp->get_DocumentRange(range.put())) && range) {
                BSTR text = nullptr;
                if (SUCCEEDED(range->GetText(-1, &text))) {
                    out = TakeBstr(text);
                    if (!out.empty()) return true;
                }
            }
        }
        com_ptr<IUIAutomationValuePattern> vp;
        if (SUCCEEDED(node.el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(vp.put()))) && vp) {
            BSTR val = nullptr;
            if (SUCCEEDED(vp->get_CurrentValue(&val))) {
                out = TakeBstr(val);
                return !out.empty();
            }
        }
    } catch (...) {
    }
    return false;
}

bool UiFocusedText(std::wstring& out) {
    if (!UiInit()) return false;
    try {
        com_ptr<IUIAutomationElement> el;
        if (FAILED(g_uia->GetFocusedElement(el.put())) || !el) return false;
        UiNode n;
        n.el = el;
        return UiGetText(n, out);
    } catch (...) {
    }
    return false;
}
