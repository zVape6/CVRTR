// SPDX-License-Identifier: GPL-3.0-only
#include "converter.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>
#include <cstring>
extern "C" {
#include <libavformat/avformat.h>
}

namespace fs = std::filesystem;
static void write16(std::ofstream& f, uint16_t n) { f.put(n & 255); f.put(n >> 8); }
static void write32(std::ofstream& f, uint32_t n) { write16(f, n & 65535); write16(f, n >> 16); }
static void fixture(const fs::path& path, int channels,int bits=16) {
    const int rate = 44100, samples = rate, bytes = samples * channels * (bits/8);
    std::ofstream f(path, std::ios::binary);
    f.write("RIFF", 4); write32(f, 36 + bytes); f.write("WAVEfmt ", 8); write32(f, 16);
    write16(f, 1); write16(f, channels); write32(f, rate); write32(f, rate * channels * (bits/8));
    write16(f, channels * (bits/8)); write16(f, bits); f.write("data", 4); write32(f, bytes);
    for (int i = 0; i < samples; ++i) for (int c = 0; c < channels; ++c) {
        int32_t sample=static_cast<int16_t>(12000 * std::sin(i * 6.283185307 * 440 / rate));
        if(bits==24) {sample*=256;for(int shift:{0,8,16}) f.put((sample>>shift)&255);}
        else write16(f,static_cast<uint16_t>(sample));
    }
}
// Independent ID3v2.3 fixture (big-endian frame lengths, Latin-1 text).
static std::vector<uint8_t> taggedFixture(const fs::path& path) {
    fixture(path, 2);
    const std::vector<uint8_t> png = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,100,248,15,0,1,5,1,1,39,24,227,102,0,0,0,0,73,69,78,68,174,66,96,130};
    std::vector<uint8_t> body;
    auto frame = [&](const char* id, std::vector<uint8_t> data) {
        body.insert(body.end(), id, id + 4);
        for (int shift : {24,16,8,0}) body.push_back((data.size() >> shift) & 255);
        body.insert(body.end(), {0,0}); body.insert(body.end(), data.begin(), data.end());
    };
    for (const auto& item : {std::make_pair("TIT2", "Test title"), std::make_pair("TPE1", "Test artist"),
            std::make_pair("TALB", "Test album"), std::make_pair("TRCK", "3/12")}) {
        std::vector<uint8_t> data{0}; data.insert(data.end(), item.second, item.second + std::strlen(item.second)); frame(item.first, data);
    }
    std::vector<uint8_t> custom{0};
    const char value[] = "CVRTR_TEST\0custom value";
    custom.insert(custom.end(), value, value + sizeof(value) - 1); frame("TXXX", custom);
    std::vector<uint8_t> picture{0,'i','m','a','g','e','/','p','n','g',0,3,0};
    picture.insert(picture.end(), png.begin(), png.end()); frame("APIC", picture);
    std::vector<uint8_t> tag{'I','D','3',3,0,0};
    for (int shift : {21,14,7,0}) tag.push_back((body.size() >> shift) & 127);
    tag.insert(tag.end(), body.begin(), body.end());
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0, std::ios::end); f.write("id3 ",4);
    uint32_t n = static_cast<uint32_t>(tag.size());
    for (int shift : {0,8,16,24}) f.put((n >> shift) & 255);
    f.write(reinterpret_cast<const char*>(tag.data()), tag.size()); if (tag.size() & 1) f.put(0);
    n = static_cast<uint32_t>(f.tellp()) - 8;
    f.seekp(4); for (int shift : {0,8,16,24}) f.put((n >> shift) & 255);
    return png;
}
int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--verify-file") {
        if (mp3_to_wav(argv[2], argv[3], nullptr) < 0) return 1;
        AudioInfo before, after; std::string error;
        if (!ReadAudioInfo(argv[2], before, error) || !ReadAudioInfo(argv[3], after, error)) return 1;
        for (const auto& tag : before.tags) {
            if (tag.first == "encoder") continue;
            if (std::find(after.tags.begin(), after.tags.end(), tag) == after.tags.end()) {
                std::cerr << "Missing or changed tag: " << tag.first << '\n'; return 1;
            }
        }
        if (before.cover_rgba.empty() || before.cover_rgba != after.cover_rgba ||
            before.cover_width != after.cover_width || before.cover_height != after.cover_height) return 1;
        std::cout << "All source tags and decoded cover pixels preserved.\n";
        return 0;
    }
    auto dir = fs::temp_directory_path() / ("cvrtr-tests-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    bool ok = true;
    auto check = [&](bool condition, const char* message) { if (!condition) { std::cerr << message << '\n'; ok = false; } };
    std::atomic<float> progress{0};
    const auto art = taggedFixture(dir / "tagged.wav");
    auto verifyTags = [&](const fs::path& path,bool custom=true) {
        AVFormatContext* ctx = nullptr;
        int ret = avformat_open_input(&ctx, path.string().c_str(), nullptr, nullptr);
        if (ret >= 0) ret = avformat_find_stream_info(ctx, nullptr);
        check(ret >= 0, "Cannot open tagged result");
        if (ret >= 0) {
            for (const auto& item : {std::make_pair("title", "Test title"), std::make_pair("artist", "Test artist"),
                    std::make_pair("album", "Test album"), std::make_pair("track", "3/12"), std::make_pair("CVRTR_TEST", "custom value")}) {
                if(!custom && std::string(item.first)=="CVRTR_TEST") continue;
                auto* tag = av_dict_get(ctx->metadata, item.first, nullptr, 0);
                int audio=av_find_best_stream(ctx,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
                if(!tag && audio>=0) tag=av_dict_get(ctx->streams[audio]->metadata,item.first,nullptr,0);
                check(tag && !std::strcmp(tag->value, item.second), item.first);
            }
            bool found = false;
            for (unsigned i = 0; i < ctx->nb_streams; ++i) {
                const auto& pic = ctx->streams[i]->attached_pic;
                if (pic.size == static_cast<int>(art.size()) && !std::memcmp(pic.data, art.data(), art.size())) found = true;
            }
            check(found, "Cover art changed or missing");
        }
        avformat_close_input(&ctx);
    };
    verifyTags(dir / "tagged.wav");
    check(wav_to_mp3((dir / "tagged.wav").string().c_str(), (dir / "tagged.mp3").string().c_str(), 192, nullptr) == 0, "Tagged WAV to MP3 failed");
    verifyTags(dir / "tagged.mp3");
    check(mp3_to_wav((dir / "tagged.mp3").string().c_str(), (dir / "tagged-result.wav").string().c_str(), nullptr) == 0, "Tagged MP3 to WAV failed");
    verifyTags(dir / "tagged-result.wav");
    check(wav_to_mp3((dir / "tagged-result.wav").string().c_str(), (dir / "tagged-roundtrip.mp3").string().c_str(), 192, nullptr) == 0, "Tagged roundtrip failed");
    verifyTags(dir / "tagged-roundtrip.mp3");
    for(int source=0;source<AudioFormatCount;++source) {
        auto from=static_cast<AudioFormat>(source);
        check(CanEncode(from),"Required encoder unavailable");
        fs::path input=dir/("matrix-source"+std::string(FormatExtension(from)));
        check(ConvertAudio((dir/"tagged.wav").string().c_str(),input.string().c_str(),from,192,nullptr)==0,"Cannot create format fixture");
        for(int target=0;target<AudioFormatCount;++target) {
            auto to=static_cast<AudioFormat>(target);
            fs::path output=dir/("matrix-"+std::to_string(source)+"-"+std::to_string(target)+FormatExtension(to));
            std::cerr<<FormatName(from)<<" -> "<<FormatName(to)<<'\n';
            check(ConvertAudio(input.string().c_str(),output.string().c_str(),to,192,nullptr)==0,"Matrix conversion failed");
            AudioInfo info;std::string error;
            bool read=ReadAudioInfo(output.string(),info,error);
            check(read && info.format==to,"Wrong output container");
            check(read && info.channels==2 && std::abs(info.duration-1)<.15,"Audio properties changed");
            verifyTags(output,from!=AudioFormat::M4a && to!=AudioFormat::M4a);
        }
    }
    std::string validation_error;
    check(ValidateInputFormat((dir/"tagged.wav").string(),AudioFormat::Wav,validation_error),"Valid source rejected");
    check(!ValidateInputFormat((dir/"tagged.wav").string(),AudioFormat::Mp3,validation_error),"Wrong selection accepted");
    fs::copy_file(dir/"tagged.wav",dir/"disguised.mp3");
    check(!ValidateInputFormat((dir/"disguised.mp3").string(),AudioFormat::Mp3,validation_error),"Disguised file accepted");
    check(FormatFromPath("TRACK.FLAC")==AudioFormat::Flac,"Uppercase extension rejected");
    check(FormatFromPath("track.txt")==AudioFormat::Unknown,"Unsupported extension accepted");
    fixture(dir/"24bit.wav",2,24);
    check(ConvertAudio((dir/"24bit.wav").string().c_str(),(dir/"24bit.flac").string().c_str(),AudioFormat::Flac,0,nullptr)==0,"24-bit FLAC failed");
    AVFormatContext* depth=nullptr;
    bool opened=avformat_open_input(&depth,(dir/"24bit.flac").string().c_str(),nullptr,nullptr)>=0;
    if(opened) opened=avformat_find_stream_info(depth,nullptr)>=0;
    check(opened && depth->streams[0]->codecpar->bits_per_raw_sample==24,"FLAC bit depth was reduced");
    avformat_close_input(&depth);
    for (int channels : {1, 2}) {
        auto wav = dir / (std::to_string(channels) + ".wav"), mp3 = dir / (std::to_string(channels) + ".mp3"), roundtrip = dir / (std::to_string(channels) + "-result.wav");
        fixture(wav, channels);
        int a = wav_to_mp3(wav.string().c_str(), mp3.string().c_str(), 192, &progress);
        check(a == 0, "WAV to MP3 failed");
        int b = a == 0 ? mp3_to_wav(mp3.string().c_str(), roundtrip.string().c_str(), &progress) : -1;
        check(b == 0, "MP3 to WAV failed");
        AudioInfo info; std::string error;
        bool read = b == 0 && ReadAudioInfo(roundtrip.string(), info, error);
        check(read, "Cannot inspect result");
        if (read) { check(info.channels == channels, "Channel count changed"); check(std::abs(info.duration - 1.0) < 0.15, "Unexpected duration"); }
        for(auto format:{AudioFormat::Flac,AudioFormat::Ogg,AudioFormat::M4a}) {
            auto output=dir/("channels-"+std::to_string(channels)+FormatExtension(format));
            check(ConvertAudio(wav.string().c_str(),output.string().c_str(),format,192,nullptr)==0,"Channel fixture conversion failed");
            AudioInfo converted;std::string message;
            bool readable=ReadAudioInfo(output.string(),converted,message);
            check(readable && converted.channels==channels && std::abs(converted.duration-1)<.15,"Channel count or duration changed");
        }
    }
    check(wav_to_mp3((dir / "missing.wav").string().c_str(), (dir / "missing.mp3").string().c_str(), 192, nullptr) < 0, "Missing input was accepted");
    { std::ofstream bad(dir / "bad.wav"); bad << "not audio"; }
    check(wav_to_mp3((dir / "bad.wav").string().c_str(), (dir / "bad.mp3").string().c_str(), 192, nullptr) < 0, "Invalid input was accepted");
    fs::remove_all(dir);
    return ok ? 0 : 1;
}
