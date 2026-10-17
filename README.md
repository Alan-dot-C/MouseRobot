# handeye

给 AI 用的**手 / 嘴 / 眼**：让模型能看屏幕、能点能打字、能说话。一个 exe，零第三方依赖，不绑定任何模型厂商。

> 前身是 `mouse_robot`（只看不做记录的输入工具）。现在把"眼"（截屏 + OCR + 筛子）和"嘴"（语音）合进同一个进程，并修掉了旧版的若干真实缺陷。

## 为什么要有"筛子"

一次全屏 OCR 通常能吐出 60～80 行文字，每行还带坐标。直接丢给模型有三个问题：

1. **贵** —— 大部分是噪声，白烧 token
2. **吵** —— 模型要在一堆边栏、图标、状态栏里找它真正需要的那个按钮
3. **碎** —— OCR 会把中文切成「对 话 轨 迹」，把一行拆成几段

所以 `handeye see` 不直接输出 OCR 结果，而是先过一层筛子：

| 阶段 | 做什么 |
|---|---|
| 清洗 | 把被拆开的中文字重新拼回去；丢掉纯符号和 OCR 碎片 |
| 归并 | 碎行合并成视觉行，再按间距聚成区块 |
| 判型 | 按几何位置识别标题栏 / 工具栏 / 侧栏 / 正文 / 状态栏 / 对话框 / 按钮 |
| 变化 | 和上一次比对，只报告新增和消失的内容 |

实测同一屏：**原始 68 行 → 筛后 12 行**，而且结构清楚、可以直接据此操作。

另外还有一条更省的路：`see --changed` 用的是**图像指纹**（9×8 灰度 dHash），完全不做 OCR，只回答"屏幕变了没有"。

## 模型无关

工具本身不调用任何大模型。它只提供两样东西：

- 稳定的**文本 / JSON 输出契约**
- 语义化的**退出码**

任何能执行命令的模型或脚本都能驱动它。运行 `handeye schema` 会打印一份机器可读的能力清单，模型可以据此自学用法，不需要人去写说明书。

## 命令

```
handeye see [选项]                 看屏幕（截屏 + OCR + 筛子）
handeye click-text <文字>          找到文字并点击 —— 先 UI Automation，再 OCR
handeye read-text [选项]           读窗口里的文字（UIA / 剪贴板，比 OCR 准）
handeye ui [选项]                  列出当前窗口的 UI 控件树（调试用）
handeye click <x> <y>              点击坐标
handeye move <x> <y>               移动鼠标
handeye type <文字>                输入文字（支持中文）
handeye key <键名>                 按键，或组合键 CTRL+S
handeye focus <标题片段>           把窗口切到前台
handeye act <脚本.txt>             执行脚本
handeye say <文字>                 语音播报
handeye voices                     列出可用语音
handeye rec <文件.rec> [--auto]    录制
handeye play <文件.rec>            回放
handeye dump <文件.rec>            查看录制内容
handeye stop                       停止正在运行的动作
handeye schema                     输出能力清单（给模型看）
```

### 两条路：控件树和像素

`click-text` 不是一上来就 OCR。它先查 Windows 的 UI Automation 控件树：
标准按钮/菜单/输入框都有名字和边界，**不会认错字**；只有查不到时（游戏、画布、
自绘界面）才回落到 OCR。输出里的 `via=ui` / `via=ocr` 会告诉你这次是靠什么点中的。

```
handeye click-text "确定"              # 先 UI，再 OCR
handeye click-text "确定" --via ocr    # 强制走 OCR
```

同理，要读大段文字用 `read-text`：UIA 的 TextPattern 是 100% 准确的，
比 OCR 便宜也比 OCR 准。`--method copy` 会退到「全选 + 复制 + 读剪贴板」，
代价是会动剪贴板（工具会尽量恢复原来的内容）。

### see 的选项

| 选项 | 说明 |
|---|---|
| `--region x,y,w,h` | 只识别这一块区域，快很多（会自动补留白） |
| `--scale auto\|1\|2\|3` | 送 OCR 前的放大倍数，默认 auto（按文字类型选） |
| `--lang auto\|zh\|en` | 识别引擎，默认 auto（中文行用 zh，拉丁/数字行用 en） |
| `--pad N` | 裁切时四周留白像素，默认 8 |
| `--fast` | 只跑中文 1x，最快；轮询用 |
| `--best` | 双引擎合并 + 可疑行二次识别，最准 |
| `--find <文字>` | 模糊查找，返回坐标和匹配度 |
| `--wait <文字>` | 轮询直到出现，配合 `--timeout`（默认 5000ms） |
| `--changed` | 只回答屏幕变了没有（走图像指纹，不做 OCR） |
| `--state-key K` | delta 状态文件按 K 隔离（多会话/多区域） |
| `--raw` | 不做筛子，吐原始 OCR 行（排障用） |
| `--all` | 不做裁剪，输出全部 |
| `--max-paras N` | 正文最多输出几段，默认 10 |
| `--no-delta` | 不与上一次比对 |
| `--show-delta` | 显示新增/消失的行 |
| `--save 文件.png` | 同时保存截图 |
| `--ui` | 叠加 UI Automation 控件树（见下） |
| `--ocr-timeout ms` | 单次识别超时，默认 20000 |

通用选项：`--json`、`--dry-run`、`--max-duration <秒>`

## 输出格式

文本模式每行一条，格式为 `[类型] 文字 @x,y wxh`：

```
INFO screen=1440x900 cursor=500,300 scale=2 raw=68 blocks=36 paras=10 delta=+0/-0
[focus] 无标题 - 记事本
[title] 设置 @600,20 240x28
[dialog] 确认删除? @700,300 400x160
[button] 是 @760,420 40x24
[button] 否 @820,420 40x24
[content] 这里是一段正文，多行已经拼在一起了 @20,60 1400x120
```

类型有：`title` `toolbar` `sidebar` `content` `status` `dialog` `button`。

正文（content）会把多行拼成段落，只给整段的框；其余类型逐行给坐标，因为它们更可能被点击。

`--json` 输出同构的结构化对象。所有文本字段都做过转义；
调用过程中产生的警告会合并进 `warnings[]`，不会再被静默丢掉。

## 安全与验证

这三样东西决定你敢不敢把电脑交给它。

### 危险动作会被拦截

点按钮前先看目标文字。命中 `删除` / `卸载` / `格式化` / `清空` / `关机` / `delete` /
`format` / `uninstall` / `shutdown` 之类的词，就停下来：

```
BLOCKED	目标含危险词「删除」
         目标文字：删除测试这一行用来验证护栏
         确认要执行请加 --yes
```

`--dry-run` 时不拦，但会提醒"正式执行会被拦截"。

词表故意做得短。太长了大家会习惯性加 `--yes`，等于没有护栏。

### 动作后可以验证

```
handeye click-text "确定" --expect-gone "确定"
handeye click-text "保存" --expect "已保存"
```

它会在超时内反复看屏幕，条件成立返回 0，不成立返回 **5**。这是把开环变闭环的关键——
调用方不用再自己截一次屏去判断。

### 动作前可以先说出来

```
handeye click-text "确定" --announce
```

会先播报"点击 确定"，然后才动鼠标。人在旁边时能跟得上它在干什么。

### 退出码

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 参数错误，或 `--find/--wait/click-text` 没找到 |
| 2 | 文件读写错误 |
| 3 | 执行失败（某一行脚本出错、没有 OCR 引擎等） |
| 4 | 超时或被停止 |
| 5 | 动作做了，但 `--expect/--expect-gone` 没通过 |

## 典型用法

**轮询等一个操作完成**

```
handeye see --changed              # 屏幕变了没有（约 60ms，不 OCR）
handeye see --wait "完成" --timeout 30000 --fast
```

更常见的写法是循环：先 `see --changed` 判断有没有动静，变了再 `see` 看内容。

**点一个按钮，不管它在哪**

```
handeye click-text "确定"
```

它会自己截屏、OCR、模糊匹配（允许 OCR 认错一两个字）、算中心点、点下去。

**等对话框出现再操作**

```
handeye see --wait "保存" --timeout 10000
handeye click-text "保存"
```

**用脚本串起来**

```
# 打开记事本，写一段中文，另存
FOCUS Notepad
SLEEP 400
TEXT 你好，这是一段测试文字。
KEY CTRL+S
WAITWIN "另存为" 3000
CLICKAT 700 500
TEXT D:\out.txt
KEY ENTER
```

**让 AI 先看清楚再动手**

```
handeye see --dry-run        # 只描述，不动手
```

## 系统日志

工具会把自己的每一次行为写进日志，格式和 Windows 事件日志同一套思路，也和本项目的安卓端
`AppLog` 保持同一行格式。

### 行格式

```
2026-10-17 18:18:13.225 | 信息 | 感知 | 3004 | 感知 | 查找命中 | [S=0d9a0 P=4692] query=DeepSeek hits=2 best=0.79
2026-10-17 18:18:13.381 | 信息 | 动作 | 4002 | 动作 | 输入文字 | [S=5c146 P=22096] text="测试文本" len=4
```

字段固定七段：**时间 | 级别 | 来源 | 事件ID | 任务类别 | 描述 | 详细信息**

- **级别**五档，对齐 Windows 的 1-5：`严重 / 错误 / 警告 / 信息 / 详细`
- **事件 ID** 按来源分段：1xxx 核心、2xxx 崩溃、3xxx 感知、4xxx 动作、5xxx 语音、
  6xxx 脚本、7xxx 配置、8xxx 会话
- **详细信息**是 `key=value` 结构，不是一句话。开头 `[S=会话 P=进程]` 用于把一次任务里的
  多次调用串起来

### 存储

```
%LOCALAPPDATA%\handeye\logs\
    handeye.log                      活动文件，超过 4 MB 就轮转
    handeye_20261017_181909.log      轮转文件，文件名带时间戳
    ...
```

保留最近 30 个文件，更旧的自动删除。轮转本身也会记一条 7002 事件。

### 看日志

```
handeye log                      最近 30 条
handeye log --last               只看上一次运行
handeye log --level 错误,警告     按级别
handeye log --id 4006            按事件 ID
handeye log --cat 动作            按任务类别
handeye log --source 感知         按来源
handeye log --since 30m          最近半小时
handeye log --grep 关键词
handeye log --stats              按级别/来源/ID 统计
handeye log --json               机器可读
handeye log --follow             实时跟踪
handeye log --export out.csv     导出（CSV 带 BOM，Excel 直接打开）
handeye log --path               日志在哪里
handeye log schema               事件目录：每个 ID 是什么意思
handeye log --clear --yes        清空
```

### 配置

配置文件 `%LOCALAPPDATA%\handeye\config.ini`：

```ini
dir=%LOCALAPPDATA%\handeye\logs
level=详细
text=full
maxBytes=4194304
keep=30
enabled=1
```

也可以用环境变量覆盖：`HANDEYE_LOG_DIR`、`HANDEYE_LOG_LEVEL`、`HANDEYE_LOG_TEXT`、
`HANDEYE_LOG`（0 关闭）、`HANDEYE_LOG_ECHO`（同时打到 stderr）。
命令行参数优先级最高：`--log-dir`、`--log-level`、`--log-text`、`--no-log`、`--log-echo`。

### 隐私

默认把输入的文字原样记进日志。如果不希望这样：

- `--log-text mask` 只记长度和首尾各一个字符
- `--log-text off` 完全不记内容
- 屏幕识别结果**从不记录具体文字**，只记行数、块数和耗时
- 截图从不写进日志

### 崩溃时的行为

安装了未捕获异常钩子（SEH / C++ 异常 / CRT 无效参数）。崩溃时会写一条 `2000 严重` 事件，
并把内存里最近 20 条事件一次性落盘——这就是"飞行记录器"，用来回答"退出前最后做了什么"。

写盘在调用线程同步完成，不攒批。代价是每次多几毫秒，换来的是崩溃前最后一行一定在盘上。
## 编译

需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载（C++20 + Windows SDK）。

```bat
build.bat
```

或手动：

```bat
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /Fe:handeye.exe src\*.cpp ^
   windowsapp.lib gdi32.lib user32.lib ole32.lib
```

产物是单个 `handeye.exe`，不需要安装任何运行时。

## 实现要点

- **纯 C++/WinRT**：OCR 用 `Windows.Media.Ocr`，语音用 `Windows.Media.SpeechSynthesis`，截屏用 GDI `StretchBlt`，输入用 `SendInput`。全程不依赖 .NET 或 PowerShell。
- **中文输入走 `KEYEVENTF_UNICODE`**：不受当前键盘布局限制。用 `VkKeyScanW` 的老做法会静默丢弃打不出来的字符。
- **坐标先裁剪到虚拟桌面**：越界坐标经过 65535 归一化会变成负数，鼠标会瞬移到屏幕左上角。
- **DPI 用 PerMonitorV2**：`SetProcessDPIAware` 在多显示器、不同缩放比例下坐标会错。
- **输出统一 UTF-8**：只设 `SetConsoleOutputCP` 是不够的，stdout 被重定向到管道时 CRT 仍按 ANSI 转换，中文会变成 `?`。这里同时设了 locale 和 `_setmode`。
- **前台等待用轮询**：`FOCUS` 之后轮询 `GetForegroundWindow()` 直到目标真的在前台，而不是固定睡一段时间。

## 已知限制

- `SendInput` 无法向权限更高的进程发送输入（UIPI），也进不去 UAC 提示框、登录界面等安全桌面
- **选中高亮会让 OCR 明显变差**：蓝底白字的状态下识别率大幅下降，必要时先取消选择
- **中文小字（12px 左右）是 Windows OCR 模型的下限**：`--best` 已经把中文和英文两路合起来用，
  再往上只能换引擎。实测 16px 以上几乎全对，12px 开始出错，10px 即使放大 3 倍仍明显出错
- 识别依赖系统已安装对应语言的 OCR 语言包（中文需要 `zh-CN`，英文需要 `en-US/en-GB`）
- 录制保存的是绝对屏幕坐标，换分辨率后回放位置会偏
- 默认只看主显示器；`--region` 可以显式读副屏，但判型仍按主屏尺寸算

## 旧工具（已并入本工具，保留备用）

| 文件 | 说明 |
|---|---|
| `mouse_robot.cpp` | 早期独立的输入工具，功能已被 `handeye` 覆盖 |
| `speak.ps1` | 早期独立的语音脚本，功能已被 `handeye say` 覆盖 |
| `OcrHelper.cs` | 早期独立的 OCR 封装，功能已被 `handeye see` 覆盖 |
