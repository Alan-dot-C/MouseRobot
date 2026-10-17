// ============================================================================
//  mouth.h —— 嘴：离线语音合成与播放
//
//  用系统自带的语音引擎，不联网、不需要任何第三方组件。
// ============================================================================
#pragma once

#include "common.h"
#include <string>
#include <vector>

struct VoiceInfo {
    std::wstring name;
    std::wstring language;
};

// 列出系统已安装的语音
std::vector<VoiceInfo> MouthListVoices();

// 说话。
//   voice   为空时自动挑中文语音，找不到再退回默认
//   rate    -10..10，0 为正常
//   volume  0..100
//   wavOut  非空时导出到 WAV 文件而不播放
//   async   立即返回，不等说完
// 返回 0 成功；1 失败；2 文本为空
int MouthSpeak(const std::wstring& text,
               const std::wstring& voice = L"",
               int rate = 0,
               int volume = 100,
               const std::wstring& wavOut = L"",
               bool async = false);
