using System;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Windows.Graphics.Imaging;
using Windows.Media.Ocr;
using Windows.Storage;
using Windows.Storage.Streams;

// OcrHelper —— Windows.Media.Ocr 的薄封装，供截屏识别脚本调用。
//
// 2026-10-17 重写要点：
//   1. 旧版 catch 后只返回 e.Message，排查时没有任何信息。
//      现在返回 "ERR:类型名: 消息 (内层: ...)"，并能区分"引擎不可用 / 超时 / 解码失败"。
//   2. 旧版用 Task.Run(...).Result 同步阻塞，没有超时，OCR 卡住就永远挂着。
//      现在带超时（默认 20 秒），超时后返回明确的 ERR:timeout。
//   3. 旧版行坐标取"首个词的中心"，长行会偏左。现在取整行边界框的中心，
//      同时输出 x,y,w,h，调用方可以点行中间。
//   4. OCR 引擎缓存为静态对象，不再每次调用重建。
//
// 输出格式（TAB 分隔，每行一条）：
//   LINE 文本 x y w h
//   WORD 文本 x y w h
// 失败时返回："ERR:..."
public static class OcrHelper {
    private static readonly object s_lock = new object();
    private static OcrEngine s_engine;
    private static string s_engineErr;

    /// <summary>获取（并缓存）中文 OCR 引擎。返回 null 表示没有可用引擎。</summary>
    private static OcrEngine GetEngine() {
        lock (s_lock) {
            if (s_engine != null) return s_engine;
            if (s_engineErr != null) return null;
            try {
                var lang = new Windows.Globalization.Language("zh-CN");
                if (OcrEngine.IsLanguageSupported(lang)) {
                    s_engine = OcrEngine.TryCreateFromLanguage(lang);
                }
                if (s_engine == null) s_engine = OcrEngine.TryCreateFromUserProfileLanguages();
                if (s_engine == null) s_engineErr = "没有可用的 OCR 引擎（未安装中文 OCR 语言包？）";
            } catch (Exception ex) {
                s_engineErr = "创建引擎失败: " + Describe(ex);
            }
            return s_engine;
        }
    }

    /// <summary>把异常展开成一行可读文字（类型 + 消息 + 内层）。</summary>
    private static string Describe(Exception ex) {
        if (ex == null) return "(null)";
        var sb = new StringBuilder();
        var e = ex;
        int depth = 0;
        while (e != null && depth < 3) {
            if (depth > 0) sb.Append(" <- ");
            sb.Append(e.GetType().Name).Append(": ").Append(e.Message);
            e = e.InnerException;
            depth++;
        }
        return sb.ToString();
    }

    /// <summary>
    /// 识别图片文件。timeoutMs 默认 20000。失败返回 "ERR:..." 而不是抛异常。
    /// </summary>
    public static string Recognize(string path) {
        return Recognize(path, 20000);
    }

    public static string Recognize(string path, int timeoutMs) {
        if (timeoutMs <= 0) timeoutMs = 20000;
        if (string.IsNullOrWhiteSpace(path)) return "ERR:empty path";

        var engine = GetEngine();
        if (engine == null) return "ERR:no engine - " + s_engineErr;

        try {
            // 注意：这里用 GetAwaiter().GetResult() 而不是 .Result。
            // .Result 会把异常包成 AggregateException，而且在线程池上
            // 存在死锁风险（旧版就是这样写的）。
            return RunWithTimeout(engine, path, timeoutMs).GetAwaiter().GetResult();
        } catch (AggregateException ae) {
            return "ERR:" + Describe(ae.Flatten().InnerExceptions.Count > 0
                ? ae.Flatten().InnerExceptions[0] : ae);
        } catch (Exception ex) {
            return "ERR:" + Describe(ex);
        }
    }

    private static async Task<string> RunWithTimeout(OcrEngine engine, string path, int timeoutMs) {
        var cts = new CancellationTokenSource(timeoutMs);
        Task<string> task = DoRecognize(engine, path);
        Task done = await Task.WhenAny(task, Task.Delay(timeoutMs, cts.Token)).ConfigureAwait(false);
        if (done != task) {
            return "ERR:timeout - OCR 超过 " + timeoutMs + " 毫秒未完成";
        }
        cts.Cancel();
        return await task.ConfigureAwait(false);
    }

    private static async Task<string> DoRecognize(OcrEngine engine, string path) {
        StorageFile file;
        try {
            file = await StorageFile.GetFileFromPathAsync(path);
        } catch (Exception ex) {
            return "ERR:open - " + Describe(ex);
        }

        SoftwareBitmap bmp;
        using (var stream = await file.OpenAsync(FileAccessMode.Read)) {
            try {
                var dec = await BitmapDecoder.CreateAsync(stream);
                bmp = await dec.GetSoftwareBitmapAsync();
            } catch (Exception ex) {
                return "ERR:decode - " + Describe(ex);
            }
        }

        OcrResult res;
        try {
            res = await engine.RecognizeAsync(bmp);
        } catch (Exception ex) {
            return "ERR:recognize - " + Describe(ex);
        }

        var sb = new StringBuilder();
        foreach (var line in res.Lines) {
            // 行边界框 = 所有词边界框的并集，取中心点，长行不会偏左
            double minX = double.MaxValue, minY = double.MaxValue;
            double maxX = double.MinValue, maxY = double.MinValue;
            bool any = false;
            var wordBuf = new StringBuilder();

            if (line.Words != null) {
                foreach (var w in line.Words) {
                    var r = w.BoundingRect;
                    if (r.X < minX) minX = r.X;
                    if (r.Y < minY) minY = r.Y;
                    if (r.X + r.Width > maxX) maxX = r.X + r.Width;
                    if (r.Y + r.Height > maxY) maxY = r.Y + r.Height;
                    any = true;
                    wordBuf.Append("WORD\t").Append(w.Text).Append('\t')
                           .Append((int)(r.X + r.Width / 2)).Append('\t')
                           .Append((int)(r.Y + r.Height / 2)).Append('\t')
                           .Append((int)r.Width).Append('\t')
                           .Append((int)r.Height).Append('\n');
                }
            }

            int cx = 0, cy = 0, cw = 0, ch = 0;
            if (any) {
                cx = (int)((minX + maxX) / 2);
                cy = (int)((minY + maxY) / 2);
                cw = (int)(maxX - minX);
                ch = (int)(maxY - minY);
            }
            // LINE 在前，该行的 WORD 紧随其后
            sb.Append(BuildLine(line.Text, cx, cy, cw, ch));
            sb.Append(wordBuf);
        }
        return sb.ToString();
    }

    private static string BuildLine(string text, int x, int y, int w, int h) {
        var b = new StringBuilder(text.Length + 40);
        b.Append("LINE\t").Append(text).Append('\t')
         .Append(x).Append('\t').Append(y).Append('\t')
         .Append(w).Append('\t').Append(h).Append('\n');
        return b.ToString();
    }
}
