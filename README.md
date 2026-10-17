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
handeye click-text <文字>          找到文字并点击 —— 眼手联动
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

### see 的选项

| 选项 | 说明 |
|---|---|
| `--region x,y,w,h` | 只识别这一块区域，快很多 |
| `--scale N` | 送 OCR 前的放大倍数，默认 2；小字用 3 |
| `--find <文字>` | 模糊查找，返回坐标和匹配度 |
| `--wait <文字>` | 轮询直到出现，配合 `--timeout`（默认 5000ms） |
| `--changed` | 只回答屏幕变了没有（走图像指纹，不做 OCR） |
| `--raw` | 不做筛子，吐原始 OCR 行（排障用） |
| `--all` | 不做裁剪，输出全部 |
| `--max-paras N` | 正文最多输出几段，默认 10 |
| `--no-delta` | 不与上一次比对 |
| `--show-delta` | 显示新增/消失的行 |
| `--save 文件.png` | 同时保存截图 |

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

`--json` 输出同构的结构化对象。

## 退出码

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 参数错误，或 `--find/--wait/click-text` 没找到 |
| 2 | 文件读写错误 |
| 3 | 执行失败（某一行脚本出错、没有 OCR 引擎等） |
| 4 | 超时或被停止 |

## 典型用法

**轮询等一个操作完成**

```
handeye see --changed --wait-diff ...
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
- 小字识别率低，`--scale 3` 会好一些，但更慢
- 识别依赖系统已安装对应语言的 OCR 语言包（中文需要 `zh-CN` 的 OCR 组件）
- 录制保存的是绝对屏幕坐标，换分辨率后回放位置会偏

## 旧工具（已并入本工具，保留备用）

| 文件 | 说明 |
|---|---|
| `mouse_robot.cpp` | 早期独立的输入工具，功能已被 `handeye` 覆盖 |
| `speak.ps1` | 早期独立的语音脚本，功能已被 `handeye say` 覆盖 |
| `OcrHelper.cs` | 早期独立的 OCR 封装，功能已被 `handeye see` 覆盖 |
