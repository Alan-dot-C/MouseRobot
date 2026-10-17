# mouse_robot

命令行驱动的 Windows 鼠标 / 键盘自动化工具。用全局低层钩子录制操作，用 `SendInput` 回放，也可以通过文本脚本精确控制。

单文件 C++ 实现，只依赖 `user32.lib` 和 `gdi32.lib`，不需要任何第三方库。

## 特性

- **录制回放**：`ScrollLock` 开始 / 停止，`Esc` 保存退出
- **文本脚本**：一行一条命令，可读、可版本控制、可参数化
- **中文输入**：走 `KEYEVENTF_UNICODE`，不受键盘布局限制，能打中文和任意符号
- **窗口定位**：按标题找窗口、等窗口出现、等它真正成为前台
- **组合操作**：支持按住 Ctrl / Shift 做拖拽、多选
- **机器可读输出**：`--json` 与语义化退出码，方便被别的程序调用
- **安全开关**：`--dry-run` 只打印不执行，`--max-duration` 总时长上限，`--no-icon` 不动系统光标

## 编译

需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载。

```bat
cl /nologo /utf-8 /EHsc /O2 /MT /Fe:mouse_robot.exe mouse_robot.cpp user32.lib gdi32.lib
```

## 用法

```
mouse_robot record <文件.rec> [--auto]       录制
mouse_robot play   <文件.rec> [--loop N]     回放（0 = 无限循环）
mouse_robot script <文件.txt>                执行文本脚本
mouse_robot stop                             让正在运行的实例停下
mouse_robot listen <文件.rec> <秒数>          监听键盘事件
mouse_robot dump   <文件.rec>                打印录制内容
mouse_robot testicon                         测试自定义鼠标样式
```

全局选项：

| 选项 | 说明 |
|---|---|
| `--icon <路径>` | 自定义鼠标样式，默认找 exe 同目录的 `tray.ico` / `cursor.ico` |
| `--no-icon` | 完全不改动系统光标 |
| `--json` | 结果以 JSON 输出到 stdout |
| `--dry-run` | 只打印将要做的事，不发送任何输入 |
| `--max-duration <秒>` | 总时长上限，超时自动中止 |
| `--auto` | 录制时启动即开始，不用按 ScrollLock |
| `--loop <N>` | 回放 / 脚本循环次数 |

## 脚本语法

每行一条命令，`#` 开头为注释。

| 命令 | 说明 |
|---|---|
| `MOVE x y [ms]` | 移动到绝对坐标，`ms` 为平滑时长（默认 300） |
| `CLICKAT x y [ms]` | 移动到目标并单击，最稳的点击方式 |
| `MOVEREL dx dy` | 相对当前位置移动 |
| `SLEEP ms` | 等待 |
| `CLICK` / `RCLICK` / `DCLICK` | 左键 / 右键 / 双击 |
| `DOWN [键]` / `UP [键]` | 按住 / 松开鼠标左键或指定键 |
| `WHEEL n` | 滚轮，`n` 必须是 120 的倍数 |
| `KEY 键名` | 按键，支持 `CTRL+L` 组合键、`0x41` 十六进制、`F1`~`F24` |
| `TEXT 内容` | 输入文本，支持中文 |
| `FOCUS 标题片段` | 把窗口切到前台并等待就绪 |
| `WAITWIN 标题 [超时ms]` | 等窗口出现，超时返回失败 |
| `GETPOS` | 打印当前鼠标坐标 |
| `SET 名 值` | 设置变量，之后用 `${名}` 引用 |

### 示例

```
# 打开记事本，输入一段中文，再全选
FOCUS Notepad
SLEEP 500
TEXT 你好，世界！
KEY ENTER
KEY CTRL+A
```

```
# 等待窗口出现后再操作，避免靠固定等待时间
WAITWIN 计算器 5000
CLICKAT 320 240
SLEEP 200
TEXT 12*34
KEY ENTER
```

```
# 用变量做参数化，同一份脚本适配不同坐标
SET X 500
SET Y 300
CLICKAT ${X} ${Y}
```

## 退出码

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 参数错误 |
| 2 | 文件读写错误 |
| 3 | 执行失败（某一行出错） |
| 4 | 超时或被停止 |

## 录制文件格式

`*.rec` 二进制格式，全部为 32 位小端：

```
magic    'RTHR' (0x52544852)
version  2
screenW  screenH  count
count 个事件：type, x, y, wheel, vk, scan, extended, t
```

读取时会校验事件数量与文件长度是否一致，截断的文件会被明确拒绝。同时兼容旧的无版本号格式（version 1）。

## 附带的两个小工具

仓库里还有两个与 `mouse_robot` 配合使用的独立小工具，都只依赖系统自带组件。

### speak.ps1 —— 离线语音播报

用 Windows 自带的 `System.Speech`，不需要联网。会自动挑一个中文语音，找不到就降级到默认语音。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File speak.ps1 -Text "构建完成"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File speak.ps1 -ListVoices
powershell.exe -NoProfile -ExecutionPolicy Bypass -File speak.ps1 -TextFile 稿子.txt -Rate -2
powershell.exe -NoProfile -ExecutionPolicy Bypass -File speak.ps1 -Text "导出" -WaveOut out.wav
```

参数：`-Text`、`-TextFile`、`-ListVoices`、`-Rate`（-10~10）、`-Volume`（0~100）、`-WaveOut`、`-Async`。

注意 `-Text` 的字符串要加引号，含空格时尤其如此。

### OcrHelper.cs —— 屏幕文字识别

Windows 自带 OCR 引擎的薄封装，编译成 DLL 后可从 PowerShell 调用。返回每行 / 每个词的文字与坐标，可直接喂给 `mouse_robot` 的 `CLICKAT`。

```
csc /target:library /out:OcrHelper.dll ^
  /r:"C:\Program Files (x86)\Windows Kits\10\UnionMetadata\<版本>\Windows.winmd" ^
  /r:"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\System.Runtime.WindowsRuntime.dll" ^
  /r:"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\System.Runtime.dll" ^
  /r:"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\System.Runtime.InteropServices.WindowsRuntime.dll" ^
  OcrHelper.cs
```

```powershell
Add-Type -Path .\OcrHelper.dll
[OcrHelper]::Recognize("screen.png")
```

输出每行一条，TAB 分隔：`LINE  文字  x  y  w  h`，失败时返回 `ERR:...`。

小字识别率很低，先把截图放大 2 倍再送去识别效果会好很多。

## 已知限制

- `SendInput` 无法向权限更高的进程发送输入（UIPI 限制），也无法操作 UAC 提示框、登录界面等安全桌面
- `ASSERT` / `WAITTEXT` 未实现，本工具不包含 OCR 后端
- 录制保存的是绝对屏幕坐标，换分辨率后回放位置会偏
- 光标替换在进程被强制结束时可能无法还原，介意的话用 `--no-icon`
