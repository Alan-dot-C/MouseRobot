// ============================================================================
//  mouth.cpp —— 语音合成
// ============================================================================
#include "mouth.h"
#include "log.h"
#include "input.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstdio>
#include <vector>

using namespace winrt;
using namespace Windows::Media::SpeechSynthesis;
using namespace Windows::Media::Playback;
using namespace Windows::Media::Core;
using Windows::Storage::Streams::DataReader;

std::vector<VoiceInfo> MouthListVoices() {
    std::vector<VoiceInfo> out;
    try {
        for (auto const& v : SpeechSynthesizer::AllVoices()) {
            VoiceInfo vi;
            vi.name = v.DisplayName().c_str();
            vi.language = v.Language().c_str();
            out.push_back(std::move(vi));
        }
    } catch (hresult_error const&) {
    }
    return out;
}

static SpeechSynthesizer CreateSynth(const std::wstring& wantVoice) {
    SpeechSynthesizer synth;
    auto voices = SpeechSynthesizer::AllVoices();
    VoiceInformation chosen{ nullptr };

    if (!wantVoice.empty()) {
        for (auto const& v : voices) {
            std::wstring name = v.DisplayName().c_str();
            if (_wcsicmp(name.c_str(), wantVoice.c_str()) == 0) { chosen = v; break; }
        }
        if (!chosen) {
            for (auto const& v : voices) {
                std::wstring name = v.DisplayName().c_str();
                if (name.find(wantVoice) != std::wstring::npos) { chosen = v; break; }
            }
        }
    }
    if (!chosen) {
        // 优先中文语音
        for (auto const& v : voices) {
            std::wstring lang = v.Language().c_str();
            if (lang.rfind(L"zh", 0) == 0) { chosen = v; break; }
        }
    }
    if (chosen) synth.Voice(chosen);
    return synth;
}

// 把合成流整体读成字节
static bool ReadStreamToBytes(SpeechSynthesisStream const& stream, std::vector<uint8_t>& out) {
    try {
        uint64_t size = stream.Size();
        if (size == 0 || size > 256ull * 1024 * 1024) return false;
        DataReader reader(stream.GetInputStreamAt(0));
        reader.LoadAsync(uint32_t(size)).get();
        out.resize(size_t(size));
        reader.ReadBytes(array_view<uint8_t>(out.data(), out.size()));
        return true;
    } catch (hresult_error const&) {
        return false;
    }
}

int MouthSpeak(const std::wstring& text,
               const std::wstring& voice,
               int rate,
               int volume,
               const std::wstring& wavOut,
               bool async) {
    std::wstring body = Trim(text);
    if (body.empty()) {
        Say(L"ERR 文本为空");
        LogW(L"语音", 5003, L"异常", L"语音引擎不可用", L"reason=empty-text");
        return 2;
    }
    LogI(L"语音", 5000, L"语音", L"播报开始", L"chars=%zu voice=%ls rate=%d volume=%d wav=%ls",
         body.size(), voice.empty() ? L"auto" : voice.c_str(), rate, volume, wavOut.c_str());

    try {
        SpeechSynthesizer synth = CreateSynth(voice);

        // 语速: -10..10 映射到 0.5..2.0 倍
        if (rate != 0) {
            double r = 1.0 + double(rate) * 0.1;
            if (r < 0.5) r = 0.5;
            if (r > 2.0) r = 2.0;
            synth.Options().SpeakingRate(r);
        }
        if (volume != 100) {
            double v = double(volume) / 100.0;
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            synth.Options().AudioVolume(v);
        }

        auto stream = synth.SynthesizeTextToStreamAsync(body).get();

        if (!wavOut.empty()) {
            std::vector<uint8_t> bytes;
            if (!ReadStreamToBytes(stream, bytes)) {
                Say(L"ERR 读取合成结果失败");
                return 1;
            }
            FILE* f = nullptr;
            if (_wfopen_s(&f, wavOut.c_str(), L"wb") != 0 || !f) {
                Say(L"ERR 无法写入 %ls", wavOut.c_str());
                return 1;
            }
            size_t written = fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
            if (written != bytes.size()) {
                Say(L"ERR 写入不完整: %ls", wavOut.c_str());
                return 1;
            }
            Say(L"已导出 %ls (%zu 字节)", wavOut.c_str(), bytes.size());
            LogI(L"语音", 5002, L"语音", L"导出 WAV", L"path=%ls bytes=%zu", wavOut.c_str(), bytes.size());
            return 0;
        }

        // 播放
        static MediaPlayer s_keepAlive{ nullptr };   // async 时防止对象被回收
        MediaPlayer player;
        player.Source(MediaSource::CreateFromStream(stream, stream.ContentType()));
        player.Play();

        if (async) {
            s_keepAlive = player;
            Say(L"开始播报（异步）");
            return 0;
        }

        // 等到播完：位置走到总长，或状态回落到 None
        auto session = player.PlaybackSession();
        for (int i = 0; i < 6000; ++i) {   // 最多等 5 分钟
            if (ShouldAbort()) break;
            auto state = session.PlaybackState();
            if (state == MediaPlaybackState::None) break;
            auto pos = session.Position();
            auto dur = session.NaturalDuration();
            if (dur.count() > 0 && pos.count() > 0 && pos >= dur) break;
            Sleep(50);
        }
        player.Pause();
        player.Close();

        Say(L"播报完成 (%zu 字)", body.size());
        LogI(L"语音", 5001, L"语音", L"播报完成", L"chars=%zu", body.size());
        return 0;
    } catch (hresult_error const& e) {
        std::wstring msg = e.message().c_str();
        Say(L"ERR 语音合成失败: %ls", msg.c_str());
        LogE(L"语音", 5004, L"异常", L"语音合成失败", L"err=%ls", msg.c_str());
        return 1;
    } catch (...) {
        Say(L"ERR 语音合成未知异常");
        return 1;
    }
}
