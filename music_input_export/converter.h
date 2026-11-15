// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <utility>

enum class AudioFormat { Unknown = -1, Wav, Mp3, Flac, Ogg, M4a };
constexpr int AudioFormatCount = 5;
const char* FormatName(AudioFormat format);
const char* FormatExtension(AudioFormat format);
AudioFormat FormatFromPath(const std::string& path);
bool CanEncode(AudioFormat format);
bool ValidateInputFormat(const std::string& path, AudioFormat expected, std::string& error);
int ConvertAudio(const char* input, const char* output, AudioFormat target, int bitrate, std::atomic<float>* progress);

struct AudioInfo {
    int sample_rate = 0;
    int channels = 0;
    double duration = 0;
    std::string codec;
    AudioFormat format = AudioFormat::Unknown;
    std::vector<std::pair<std::string, std::string>> tags;
    std::vector<unsigned char> cover_rgba;
    int cover_width = 0;
    int cover_height = 0;
};

bool ReadAudioInfo(const std::string& path, AudioInfo& info, std::string& error);
std::string AudioError(int code);
int wav_to_mp3(const char* input, const char* output, int bitrate, std::atomic<float>* progress);
int mp3_to_wav(const char* input, const char* output, std::atomic<float>* progress);
