# handeye — User Manual

**handeye** gives an AI agent a pair of eyes, a hand, and a mouth on Windows:
it can look at the screen, click and type, and speak. It is a single
dependency-free executable. It does not call any language model — it only
exposes a stable text/JSON contract and meaningful exit codes, so any agent
that can run a command can drive it.

- Binary: `handeye.exe` (statically linked, no runtime to install)
- Source: `src/` (C++20, C++/WinRT, GDI, no third-party libraries)
- Machine-readable capability list: `handeye schema`

This manual is written for the person wiring handeye into an agent. Every
command below was run on Windows 11 with a 1440x900 display; timings are from
that machine.

---

## 1. What it is, and what it is not

It is a **tool**, not an agent. It has no planning, no memory and no model.
It answers three questions and performs three kinds of action:

| Question | Command |
|---|---|
| What is on screen? | `see`, `ui`, `read-text`, `see --changed` |
| Where is that text? | `see --find`, `see --wait` |
| Did it work? | `--expect` / `--expect-gone`, exit codes |

| Action | Command |
|---|---|
| Click something | `click-text` (by text), `click` (by coordinate) |
| Type something | `type`, `key` |
| Say something | `say` |

It is **not** a screen reader, a recorder of your data, or a remote-control
backdoor. It writes logs, it never uploads anything, and its only network
code is the build system.

---

## 2. Requirements and build

- Windows 10 1809+ or Windows 11 (uses `Windows.Media.Ocr` and UI Automation)
- An OCR language pack: Chinese Simplified (`zh-Hans-CN`) and/or English
- To build: Visual Studio with the "Desktop development with C++" workload

```bat
build.bat
```

`build.bat` compiles every `src\*.cpp` with `/std:c++20 /O2 /MT /MP` and links
`windowsapp.lib gdi32.lib user32.lib ole32.lib`. The result is one
self-contained `handeye.exe` (about 730 KB).

If `cl.exe` is not on `PATH`, run the script from the *x64 Native Tools
Command Prompt for VS*, or call `vcvars64.bat` first.

---

## 3. Quick start

```bat
handeye see
handeye click-text "Save"
handeye type "hello"
handeye say "done"
```

A typical agent step looks like this:

```bat
handeye see --changed                       :: cheap: did anything move? (~60 ms)
handeye see --fast                          :: OCR, fastest setting (~240 ms)
handeye click-text "OK" --expect-gone "OK"  :: act, then verify
```

---

## 4. The two eyes

handeye looks at the screen through two independent channels.

**UI Automation (UIA).** Standard Windows controls expose a real tree of
elements with names, roles and bounding rectangles. A button literally reports
its name as `Save`; no character recognition is involved, so it cannot misread
the word. This path costs ~60 ms and works even for windows that are not in
focus (for controls that support `InvokePattern`).

**OCR.** When there is no control tree — games, canvases, Electron apps that
expose only a document node, custom-drawn interfaces — handeye falls back to
pixels. `Windows.Media.Ocr` is used with two engines:

- `zh-Hans-CN` at 1x for Chinese lines
- `en-GB` at 2–3x for Latin and numeric lines

Lines are matched by bounding box and merged: a line that is mostly CJK keeps
the Chinese reading, a line that is mostly Latin takes the English reading.
This matters: the Chinese model reads `World` as `Wor1d` and `05:22` as `0 & 22`,
while the English model reads both perfectly.

`click-text` tries UIA first and only then OCR. The output says which path was
used (`via=ui` or `via=ocr`), and the same flag exists to force a path.

> Practical consequence: on standard dialogs and toolbars, clicking by text is
> reliable and fast. On custom-drawn UIs it degrades to OCR accuracy.

---

## 5. Commands

Global options accepted by every command:

| Option | Meaning |
|---|---|
| `--json` | Emit exactly one JSON object on stdout |
| `--dry-run` | Describe what would happen, change nothing |
| `--max-duration N` | Abort after N seconds (covers waiting and verification) |
| `--announce` | Speak the action before performing it |
| `--instant` | Jump the cursor instead of animating (faster) |
| `--log-dir`, `--log-level`, `--log-text`, `--no-log`, `--log-echo` | Logging control |

### 5.1 `see` — look at the screen

```
handeye see [options]
```

| Option | Default | Meaning |
|---|---|---|
| `--region x,y,w,h` | whole screen | Only look at this rectangle (padding is added automatically) |
| `--scale auto\|1\|2\|3` | `auto` | OCR magnification. `auto` picks per content type |
| `--lang auto\|zh\|en` | `auto` | Engine selection; `auto` merges per line |
| `--pad N` | 8 | Padding around a cropped region, in pixels |
| `--fast` | off | Chinese 1x only; use for polling |
| `--best` | off | Dual engine plus a second look at suspicious lines |
| `--raw` | off | Skip the sieve, print raw OCR lines (debugging) |
| `--find TEXT` | | Fuzzy search; prints coordinates and a score |
| `--wait TEXT` | | Poll until the text appears |
| `--timeout MS` | 5000 | Timeout for `--wait` |
| `--changed` | | Image fingerprint only, no OCR; answers "did anything move?" |
| `--threshold N` | 12 | Per-cell difference threshold for `--changed` |
| `--no-delta` | off | Do not compare with the previous frame |
| `--show-delta` | off | Print added/removed lines |
| `--state-key KEY` | `default` | Isolate delta state between sessions or regions |
| `--save FILE.png` | | Also save the screenshot |
| `--ui` | off | Add the UIA control tree to the output |
| `--ui-depth N` / `--ui-max N` / `--ui-timeout MS` | 8 / 300 / 400 | UIA limits |
| `--ocr-timeout MS` | 20000 | Per-recognition timeout |
| `--all`, `--max-lines N`, `--max-chars N`, `--max-paras N` | | Output size limits |

The sieve compresses raw OCR (often 60–80 lines) into blocks with a semantic
kind: `title`, `toolbar`, `sidebar`, `content`, `status`, `dialog`, `button`.
Body text is joined into paragraphs; only clickable-looking elements keep
per-line coordinates.

### 5.2 `click-text` — find text and click it

```
handeye click-text "Save" [options]
```

| Option | Meaning |
|---|---|
| `--index N` | Which hit to use (default 0) |
| `--timeout MS` | Keep polling this long before giving up |
| `--via auto\|ui\|ocr` | Force a path |
| `--right` | Right click |
| `--yes` | Allow targets that contain a danger word |
| `--expect TEXT` / `--expect-gone TEXT` | Verify after clicking |
| `--expect-timeout MS` | Verification timeout (default 3000) |

Matching is forgiving: exact name, substring, or one edit of distance for
short needles. Standard buttons are activated through `InvokePattern`; anything
else is clicked at the center of its bounding box.

### 5.3 `read-text` — read text out of a window

```
handeye read-text [--window TITLE] [--method auto|copy]
```

`auto` uses UIA `TextPattern` / `ValuePattern` on the focused element, then on
document/text/edit nodes of the focused window. It never touches the clipboard.

`copy` is the fallback for apps with no text pattern: focus the window, press
Ctrl+A, Ctrl+C, wait for the clipboard sequence number to change, read it, then
restore the previous clipboard text. This is exact but it selects everything,
so it is never automatic.

### 5.4 `ui` — dump the control tree

```
handeye ui [--depth 8] [--max 800] [--text FILTER]
```

Debugging aid. Prints `[role] name @x,y wxh invoke` per node.

### 5.5 Basic input

```
handeye click 800 400 [--right]
handeye move 800 400
handeye type "hello"
handeye key ENTER
handeye key CTRL+S
handeye key 0x41
handeye focus "Notepad"
```

`type` sends Unicode through `KEYEVENTF_UNICODE`, so it is independent of the
keyboard layout and can produce Chinese. `key` accepts names (`ENTER`, `TAB`,
`F5`, `PGDN`, ...), combinations (`CTRL+SHIFT+S`), or virtual-key hex (`0x41`).

### 5.6 `act` — run a script

```
handeye act script.txt [--dry-run] [--instant]
```

See section 7 for the language. Any failing line aborts the script and makes
the process exit with 3.

### 5.7 `say` and `voices`

```
handeye say "build finished" [--voice NAME] [--rate -10..10] [--volume 0..100]
handeye say "text" --wav out.wav
handeye voices
```

Uses the system speech synthesizer, preferring a Chinese voice when one is
installed. `--async` returns before playback finishes.

### 5.8 Recording

```
handeye rec demo.rec [--auto]
handeye play demo.rec [--loop N]
handeye dump demo.rec
```

Recording captures global mouse and keyboard events; ScrollLock starts and
stops, Esc saves and exits. Playback uses absolute screen coordinates, so it is
resolution-dependent by design.

### 5.9 Diagnostics

```
handeye log [--tail N] [--level ...] [--id ...] [--cat ...] [--since 30m]
            [--grep TEXT] [--last] [--stats] [--follow] [--export out.csv]
            [--json] [--path] [--clear --yes]
handeye doctor [--since 30m]
handeye bench                                  # synthetic OCR matrix
handeye bench --screen shot.png --truth t.txt  # regression over a real screen
```

`doctor` groups problem events by ID and prints a cause plus advice.

The regression mode reads a truth file, one region per line, TAB separated:

```
90,136,530,26	Home Browse Live Games
978,362,140,230	Billie Jean Beat It Whatever Happens
```

Average character error rate above `--threshold` (default 0.18) exits with 1.

### 5.10 Everything else

```
handeye schema     # capability list as JSON (what an agent should read first)
handeye help       # human help
handeye gui        # small launcher window
handeye stop       # signal a running action, wait or verify to stop
```

---

## 6. Output contracts

### 6.1 Text output

`see` prints a header, then elements grouped by owning window:

```
INFO screen=1440x900 cursor=800,400 scale=1 raw=55 blocks=29 paras=10 delta=+31/-30
[focus] Untitled - Notepad
[ui:button] Save @820,64 60x28 invoke
[win] Untitled - Notepad
[content] first paragraph of body text @20,120 900x60
[windows] 1 个:
  z=0 Untitled - Notepad @0,0 1440x900
```

Element lines are `[kind] text @x,y wxh`. Kinds are `title`, `toolbar`,
`sidebar`, `content`, `status`, `dialog`, `button`, and `ui:<role>` for control
tree entries. One-shot results look like:

```
HIT	Save	820	64	60	28	0.91
FOUND	Save	820	64	60	28	1.00	120ms
CLICKED	Save	837	78	1.00	ui
NOTFOUND	Save
BLOCKED	目标含危险词「删除」
```

Some human-readable strings are Chinese. Machine consumers should use `--json`.

### 6.2 JSON

`--json` emits a single object, always with `"ok"`, and always with `"warnings"`
when the tool produced any (warnings are never silently dropped).

```json
{
  "ok": true,
  "screen": {"w": 1440, "h": 900},
  "cursor": {"x": 800, "y": 400},
  "focus": "Untitled - Notepad",
  "rawLines": 55,
  "delta": {"changed": true, "added": 3, "removed": 1},
  "blocks": [
    {"kind": "button", "window": "Untitled - Notepad",
     "box": {"x": 820, "y": 64, "w": 60, "h": 28},
     "items": [{"text": "Save", "x": 820, "y": 64, "w": 60, "h": 28}]}
  ]
}
```

Other shapes:

- `--find`: `{"ok":true,"count":N,"hits":[{"text","x","y","w","h","score"}]}`
- `--wait` (found): `{"ok":true,"found":true,"text","x","y","w","h","score","elapsedMs"}`
- `--changed`: `{"ok":true,"changed":true,"cells":N,"region":{...},"firstFrame":false}`
- `--raw`: `{"ok":true,"raw":true,"count":N,"lines":[{"text","x","y","w","h"}]}`
- `click-text`: `{"ok":true,"text":"Save","x":837,"y":78,"w":60,"h":28,"score":1.0,"via":"ui"}`
- `click-text --dry-run`: same plus `"dryRun":true`, `"dangerous":false`, no `w`/`h`
- `read-text`: `{"ok":true,"chars":N,"text":"...","via":"uia-focus"}`
- `ui`: `{"ok":true,"count":N,"nodes":[{"role","name","value","x","y","w","h","enabled","invokable"}]}`
- `see --ui` adds a `"ui"` array to the `see` object.
- `schema`: `{"tool","version","purpose","commands","globalOptions","exitCodes","outputFormats","tips"}`

### 6.3 Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Bad arguments, or `--find` / `--wait` / `click-text` found nothing |
| 2 | File read/write error |
| 3 | Execution failed (script line failed, no OCR engine, screenshot failed) |
| 4 | Timed out or stopped by `handeye stop` / `--max-duration` |
| 5 | The action ran but `--expect` / `--expect-gone` did not hold |

---

## 7. Script reference

One instruction per line, `#` starts a comment. Variables are set with
`SET name value` and expanded as `${name}`.

| Instruction | Meaning |
|---|---|
| `MOVE x y [ms]` | Move with an ease-out curve, default 300 ms |
| `CLICKAT x y` | Move and click |
| `MOVEREL dx dy` | Relative move |
| `CLICK` / `RCLICK` / `DCLICK` | Click at the current position |
| `DOWN [KEY]` / `UP [KEY]` | Hold or release a mouse button or key |
| `WHEEL n` | Wheel, `n` must be a multiple of 120 |
| `KEY name` | Press one key or a `CTRL+S` style combination |
| `TEXT text` | Type Unicode text |
| `FOCUS title` | Bring a window to the foreground and wait for it |
| `WAITWIN title [ms]` | Wait for a window to exist |
| `GETPOS` | Print the cursor position |
| `SET name value` | Define a variable |
| `SLEEP ms` | Sleep, interruptible by `stop` |
| `CLICKTEXT text [--index N]` | Same strategy chain as `click-text` |
| `WAITTEXT text [ms]` | Wait until the text appears |
| `WAITGONE text [ms]` | Wait until the text disappears |
| `SCROLL UP\|DOWN n [--to text]` | Scroll, optionally until the text is visible |
| `CLEARINPUT` | Ctrl+A then Delete |

Example:

```
FOCUS "Untitled - Notepad"
TEXT 你好，这是一段测试文字。
KEY CTRL+S
WAITWIN 另存为 3000
TEXT D:\out.txt
KEY ENTER
WAITGONE 另存为 5000
```

Danger words are checked for `CLICKTEXT` but not for `CLICKAT` — a coordinate
carries no meaning, so there is nothing to check. Prefer `CLICKTEXT`.

---

## 8. Recipes for agents

**Wait for something, then act**

```
handeye see --wait "Export complete" --timeout 60000 --fast
handeye click-text "Close" --via ui
```

**Poll without paying for OCR**

```
handeye see --changed --json           # about 60 ms, no OCR
```

**Click a button whose label the OCR mangles**

```
handeye click-text "确定" --via ui     # control tree ignores recognizer errors
```

**Read a long document exactly**

```
handeye read-text --method copy
```

**Fill a search box**

```
handeye focus "MyApp"
handeye click-text "Search" --via ui
handeye type "invoice 2026"
handeye key ENTER
```

**Find an item in a long list**

```
handeye click-text "Settings"
handeye see --find "Advanced" --fast
handeye act scroll_to.txt       # SCROLL DOWN 3 --to Advanced
```

**Verify an action without guessing**

```
handeye click-text "Delete" --yes --expect-gone "Delete" --expect "Deleted"
```

---

## 9. Logging and privacy

Every run writes `时间 | 级别 | 来源 | 事件ID | 任务类别 | 描述 | 详细信息`
lines to `%LOCALAPPDATA%\handeye\logs\`. The active file rotates at 4 MB, the
newest 30 files are kept. Per-call errors never crash the logger.

Useful event IDs:

| ID | Meaning |
|---|---|
| 3002 | OCR finished (`raw= ms=`) |
| 3012 | OCR pass statistics (`zh= en= raw= ms=`) — shows whether the English pass ran |
| 3013 | UIA tree (`nodes= ms=`) |
| 3014 | `read-text` (`via= chars=`) |
| 4001 | Click (`x= y= btn=`) |
| 4012 / 4013 | Verification timeout / dangerous action blocked |
| 4014 | Text click hit (`text= via= x= y= score= dryRun=`) |
| 4015 | Action aborted (`stage=wait` / `stage=verify`) |
| 8000 / 8001 | Session start (argv) / session end (rc) |

```bat
handeye doctor --since 30m
handeye log --id 3012,3014,4014 --tail 20
handeye log --grep "BLOCKED"
```

Privacy controls:

- `--log-text mask` keeps only length and first/last character; command-line
  arguments are redacted to `***`
- `--log-text off` records no text at all
- Screen recognition text is not logged wholesale; only the line that a
  `click-text` actually matched is recorded (event 4014), and it obeys
  `--log-text`
- Screenshots are never written to the log; a failure screenshot goes to
  `%LOCALAPPDATA%\handeye\fail_<timestamp>.png`

Nothing is ever sent anywhere.

---

## 10. Safety model

`click-text` refuses to click a target whose matched text contains a danger
word (`删除`, `格式化`, `卸载`, `清空`, `关机`, `delete`, `format`, `uninstall`,
`shutdown`, ...). Add `--yes` to override; the attempt is logged as event 4013.

This is a brake, not a security boundary:

- it only covers text-based clicks, not coordinate clicks or scripts that use
  `CLICKAT`
- it depends on the OC(R)/UIA reading being correct; a misread danger word
  passes

The real guarantees are elsewhere: nothing is deleted by handeye itself,
`--dry-run` never touches the machine, and every action is logged.

`SendInput` cannot reach a window owned by a higher-integrity process (UIPI),
so clicks into elevated windows silently do nothing. If a click appears to
have no effect, check whether the target is running as administrator.

---

## 11. Performance

Measured on a 1440x900 display, i5-14400F:

| Operation | Time |
|---|---|
| `see --changed` | ~60 ms |
| `see --fast` (Chinese 1x) | ~240 ms |
| `see` (default, dual engine) | ~430–700 ms depending on how much Latin text is on screen |
| `see --best` | ~430 ms plus a few small re-reads |
| `click-text` via UIA | ~60–70 ms |
| `click-text` via OCR | ~400–500 ms |

Tuning advice:

- Poll with `--changed`, then look with `--fast`.
- Use `--best` when a single character matters, not in a loop.
- `--region` is the largest single win: it shrinks both capture and OCR.
- `--instant` removes ~400 ms of cursor animation per click.

---

## 12. Limitations and troubleshooting

Known limits:

- **Small Chinese text (10–13 px) is unreliable.** Measured average character
  error rate on a real page: 0.16 overall, but 0.25–0.34 for 12–15 px Chinese
  lines. The Windows Chinese OCR model is the ceiling; parameter tuning cannot
  remove it.
- OCR covers the primary monitor only. `--region` can read a secondary monitor
  explicitly, but layout classification still uses primary-screen dimensions.
- UIA is empty for games, many Qt apps and some custom-drawn interfaces; those
  degrade to OCR.
- Recording/playback uses absolute coordinates.
- `read-text --method copy` overwrites the clipboard and restores only its text
  content.

| Symptom | Likely cause | What to do |
|---|---|---|
| `ERR 没有可用的 OCR 引擎` | No OCR language pack | Install Chinese/English OCR in Windows settings |
| `NOTFOUND` although the text is visible | Text too small, or the region was cropped too tightly | Try `--best`, a wider `--region`, or `--via ui` |
| `BLOCKED` | Danger word in the target | Check the printed target, then `--yes` if correct |
| `VERIFY-FAILED` | The action did not take effect | Look at the printed `fail_*.png`, or lower `--expect` specificity |
| Click does nothing | Target is elevated (UIPI) | Run the target unelevated, or drive it another way |
| `read-text` returns nothing | No text pattern in the app | Use `--method copy` |
| Very slow `see` | A lot of Latin text triggers the second engine | Use `--fast` for polling |
| Delta noise (`+30/-30`) | State file belongs to another screen or session | Use a distinct `--state-key`, or `--no-delta` |

---

## 13. Design notes

- **Zero dependencies** was a hard constraint: one exe, no .NET, no
  PowerShell, no redistributables. UIA and OCR are both inbox Windows APIs.
- **The sieve is the product.** Raw OCR is noisy and expensive to feed to a
  model; grouping lines into typed blocks is what makes the output usable.
- **Stable contracts over clever output.** Text lines, JSON objects and exit
  codes change rarely; internals change freely.
- **Measure, do not guess.** `bench` exists because earlier tuning was
  folklore. The dual-engine design came out of a CER matrix, not intuition.
- **Fail loudly, fail safely.** OCR has a timeout, a hung recognition cannot
  wedge the process, and `--dry-run` is always available.
