// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 zVape6 and CVRTR contributors.
#include "converter.h"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>
#include <cstring>
#include <filesystem>
#include <cctype>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/base64.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {
struct FormatSpec {const char* name;const char* extension;const char* muxer;AVCodecID codec;};
const FormatSpec formats[] = {{"WAV",".wav","wav",AV_CODEC_ID_PCM_S16LE},{"MP3",".mp3","mp3",AV_CODEC_ID_MP3},
    {"FLAC",".flac","flac",AV_CODEC_ID_FLAC},{"OGG",".ogg","ogg",AV_CODEC_ID_VORBIS},{"M4A",".m4a","ipod",AV_CODEC_ID_AAC}};
const FormatSpec* Spec(AudioFormat format) {int index=static_cast<int>(format);return index>=0 && index<AudioFormatCount?&formats[index]:nullptr;}
const AVCodec* Encoder(AudioFormat format) {
    auto* spec=Spec(format);if(!spec) return nullptr;
    if(format==AudioFormat::Ogg) return avcodec_find_encoder_by_name("libvorbis");
    return avcodec_find_encoder(spec->codec);
}
AudioFormat DetectFormat(AVFormatContext* context) {
    std::string name=context->iformat->name;
    if(name=="wav") return AudioFormat::Wav;
    if(name=="mp3") return AudioFormat::Mp3;
    if(name=="flac") return AudioFormat::Flac;
    if(name=="ogg") return AudioFormat::Ogg;
    if(name.find("mov")!=std::string::npos || name.find("m4a")!=std::string::npos) return AudioFormat::M4a;
    return AudioFormat::Unknown;
}
std::string Error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

struct Resources {
    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;
    AVCodecContext* decoder = nullptr;
    AVCodecContext* encoder = nullptr;
    SwrContext* resampler = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVFrame* decoded = av_frame_alloc();
    AVPacket* input_packet = av_packet_alloc();
    AVPacket* output_packet = av_packet_alloc();
    ~Resources() {
        av_audio_fifo_free(fifo);
        swr_free(&resampler);
        av_frame_free(&decoded);
        av_packet_free(&input_packet);
        av_packet_free(&output_packet);
        avcodec_free_context(&decoder);
        avcodec_free_context(&encoder);
        avformat_close_input(&input);
        if (output && !(output->oformat->flags & AVFMT_NOFILE)) avio_closep(&output->pb);
        avformat_free_context(output);
    }
};

// RIFF INFO cannot represent cover art or arbitrary ID3 fields. Store an ID3v2.4
// chunk in WAV as well; FFmpeg's WAV demuxer reads it on the next conversion.
void Synchsafe(std::vector<uint8_t>& bytes, size_t n) {
    for (int shift : {21, 14, 7, 0}) bytes.push_back(static_cast<uint8_t>((n >> shift) & 127));
}
void TagFrame(std::vector<uint8_t>& tag, const char* id, const std::vector<uint8_t>& value) {
    tag.insert(tag.end(), id, id + 4); Synchsafe(tag, value.size());
    tag.insert(tag.end(), {0, 0}); tag.insert(tag.end(), value.begin(), value.end());
}
int WriteWaveTags(AVFormatContext* input, AVFormatContext* output) {
    std::vector<uint8_t> body;
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(output->metadata, "", entry, AV_DICT_IGNORE_SUFFIX))) {
        const char* id = nullptr;
        const std::pair<const char*, const char*> names[] = {{"title", "TIT2"}, {"artist", "TPE1"}, {"album", "TALB"},
            {"album_artist", "TPE2"}, {"track", "TRCK"}, {"disc", "TPOS"}, {"date", "TDRC"}, {"genre", "TCON"},
            {"composer", "TCOM"}, {"copyright", "TCOP"}, {"publisher", "TPUB"}};
        for (const auto& name : names) if (!std::strcmp(entry->key, name.first)) { id = name.second; break; }
        std::vector<uint8_t> value{3}; // UTF-8
        if (!id) { value.insert(value.end(), entry->key, entry->key + std::strlen(entry->key)); value.push_back(0); }
        value.insert(value.end(), entry->value, entry->value + std::strlen(entry->value));
        TagFrame(body, id ? id : "TXXX", value);
    }
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        AVStream* pic = input->streams[i];
        if (!(pic->disposition & AV_DISPOSITION_ATTACHED_PIC) || !pic->attached_pic.size) continue;
        const char* mime = pic->codecpar->codec_id == AV_CODEC_ID_PNG ? "image/png" : "image/jpeg";
        std::vector<uint8_t> value{3};
        value.insert(value.end(), mime, mime + std::strlen(mime)); value.insert(value.end(), {0, 3});
        auto* title = av_dict_get(pic->metadata, "title", nullptr, 0);
        if (title) value.insert(value.end(), title->value, title->value + std::strlen(title->value));
        value.push_back(0); value.insert(value.end(), pic->attached_pic.data, pic->attached_pic.data + pic->attached_pic.size);
        TagFrame(body, "APIC", value);
    }
    if (body.empty()) return 0;
    if (body.size() > 0x0fffffff) return AVERROR(EINVAL);
    std::vector<uint8_t> tag{'I','D','3',4,0,0}; Synchsafe(tag, body.size());
    tag.insert(tag.end(), body.begin(), body.end());
    AVIOContext* pb = output->pb;
    int64_t end = avio_tell(pb);
    if (end < 0 || end + static_cast<int64_t>(tag.size()) + 8 > UINT32_MAX) return AVERROR(EINVAL);
    avio_write(pb, reinterpret_cast<const uint8_t*>("id3 "), 4);
    avio_wl32(pb, static_cast<unsigned>(tag.size()));
    avio_write(pb, tag.data(), static_cast<int>(tag.size()));
    if (tag.size() & 1) avio_w8(pb, 0);
    end = avio_tell(pb);
    if (avio_seek(pb, 4, SEEK_SET) < 0) return AVERROR(EIO);
    avio_wl32(pb, static_cast<uint32_t>(end - 8));
    if (avio_seek(pb, end, SEEK_SET) < 0) return AVERROR(EIO);
    return pb->error < 0 ? pb->error : 0;
}

static int WriteOggCover(AVFormatContext* input,AVDictionary** metadata) {
    for(unsigned i=0;i<input->nb_streams;++i) {
        AVStream* pic=input->streams[i];
        if(!(pic->disposition & AV_DISPOSITION_ATTACHED_PIC) || pic->attached_pic.size<=0) continue;
        const std::string mime=pic->codecpar->codec_id==AV_CODEC_ID_PNG?"image/png":"image/jpeg";
        auto* title=av_dict_get(pic->metadata,"title",nullptr,0);
        std::string description=title?title->value:"";
        std::vector<uint8_t> block;
        auto number=[&](uint32_t value) {for(int shift:{24,16,8,0}) block.push_back((value>>shift)&255);};
        number(3);number(static_cast<uint32_t>(mime.size()));block.insert(block.end(),mime.begin(),mime.end());
        number(static_cast<uint32_t>(description.size()));block.insert(block.end(),description.begin(),description.end());
        number(pic->codecpar->width);number(pic->codecpar->height);number(24);number(0);
        number(pic->attached_pic.size);block.insert(block.end(),pic->attached_pic.data,pic->attached_pic.data+pic->attached_pic.size);
        std::vector<char> encoded(AV_BASE64_SIZE(block.size()));
        if(!av_base64_encode(encoded.data(),static_cast<int>(encoded.size()),block.data(),static_cast<int>(block.size()))) return AVERROR(EINVAL);
        return av_dict_set(metadata,"METADATA_BLOCK_PICTURE",encoded.data(),0);
    }
    return 0;
}

int Convert(const char* input, const char* output, AudioFormat target, int bitrate, std::atomic<float>* progress) {
    auto* spec=Spec(target);if(!spec) return AVERROR(EINVAL);
    Resources r;
    if (!r.decoded || !r.input_packet || !r.output_packet) return AVERROR(ENOMEM);
    int ret = avformat_open_input(&r.input, input, nullptr, nullptr);
    if (ret < 0) return ret;
    if ((ret = avformat_find_stream_info(r.input, nullptr)) < 0) return ret;
    int index = av_find_best_stream(r.input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) return index;
    AVStream* in_stream = r.input->streams[index];
    const AVCodec* decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!decoder || !(r.decoder = avcodec_alloc_context3(decoder))) return AVERROR_DECODER_NOT_FOUND;
    if ((ret = avcodec_parameters_to_context(r.decoder, in_stream->codecpar)) < 0 ||
        (ret = avcodec_open2(r.decoder, decoder, nullptr)) < 0) return ret;
    if (r.decoder->sample_rate <= 0 || r.decoder->ch_layout.nb_channels <= 0) return AVERROR_INVALIDDATA;
    if ((ret = avformat_alloc_output_context2(&r.output, nullptr, spec->muxer, output)) < 0 || !r.output)
        return ret < 0 ? ret : AVERROR_UNKNOWN;
    const AVCodec* encoder = Encoder(target);
    if (!encoder || !(r.encoder = avcodec_alloc_context3(encoder))) return AVERROR_ENCODER_NOT_FOUND;
    AVStream* out_stream = avformat_new_stream(r.output, nullptr);
    if (!out_stream) return AVERROR(ENOMEM);
    r.encoder->sample_rate = r.decoder->sample_rate;
    const int* sample_rates = nullptr;
    const AVSampleFormat* sample_formats = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 12, 100)
    const void* configs = nullptr;
    if ((ret = avcodec_get_supported_config(r.encoder, encoder, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configs, nullptr)) < 0) return ret;
    sample_rates = static_cast<const int*>(configs);
    configs = nullptr;
    if ((ret = avcodec_get_supported_config(r.encoder, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, nullptr)) < 0) return ret;
    sample_formats = static_cast<const AVSampleFormat*>(configs);
#else
    sample_rates = encoder->supported_samplerates;
    sample_formats = encoder->sample_fmts;
#endif
    if (sample_rates) {
        int best = sample_rates[0];
        for (const int* p = sample_rates; *p; ++p)
            if (std::abs(*p - r.decoder->sample_rate) < std::abs(best - r.decoder->sample_rate)) best = *p;
        r.encoder->sample_rate = best;
    }
    r.encoder->sample_fmt = sample_formats ? sample_formats[0] : AV_SAMPLE_FMT_S16;
    if(target==AudioFormat::Flac && r.decoder->bits_per_raw_sample>16 && sample_formats) {
        for(auto* p=sample_formats;*p!=AV_SAMPLE_FMT_NONE;++p) if(*p==AV_SAMPLE_FMT_S32) {
            r.encoder->sample_fmt=AV_SAMPLE_FMT_S32;
            r.encoder->bits_per_raw_sample=r.decoder->bits_per_raw_sample;
            break;
        }
    }
    if (target==AudioFormat::Mp3 || target==AudioFormat::Ogg || target==AudioFormat::M4a) av_channel_layout_default(&r.encoder->ch_layout, r.decoder->ch_layout.nb_channels == 1 ? 1 : 2);
    else if ((ret = av_channel_layout_copy(&r.encoder->ch_layout, &r.decoder->ch_layout)) < 0) return ret;
    if(target!=AudioFormat::Wav && target!=AudioFormat::Flac) r.encoder->bit_rate = bitrate * 1000;
    r.encoder->time_base = AVRational{1, r.encoder->sample_rate};
    if (r.output->oformat->flags & AVFMT_GLOBALHEADER) r.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((ret = avcodec_open2(r.encoder, encoder, nullptr)) < 0 ||
        (ret = avcodec_parameters_from_context(out_stream->codecpar, r.encoder)) < 0) return ret;
    out_stream->time_base = r.encoder->time_base;
    av_dict_copy(&r.output->metadata, r.input->metadata, 0);
    av_dict_copy(&r.output->metadata, in_stream->metadata, AV_DICT_DONT_OVERWRITE);
    av_dict_copy(&out_stream->metadata, in_stream->metadata, 0);
    if(target==AudioFormat::Ogg && (ret=WriteOggCover(r.input,&r.output->metadata))<0) return ret;
    av_dict_copy(&out_stream->metadata,r.output->metadata,AV_DICT_DONT_OVERWRITE);
    int cover_index = -1;
    if (target!=AudioFormat::Wav && target!=AudioFormat::Ogg) for (unsigned i = 0; i < r.input->nb_streams; ++i) {
        AVStream* pic = r.input->streams[i];
        if (!(pic->disposition & AV_DISPOSITION_ATTACHED_PIC) || !pic->attached_pic.size) continue;
        AVStream* dest = avformat_new_stream(r.output, nullptr);
        if (!dest) return AVERROR(ENOMEM);
        if ((ret = avcodec_parameters_copy(dest->codecpar, pic->codecpar)) < 0) return ret;
        dest->codecpar->codec_tag = 0; dest->disposition = pic->disposition; dest->time_base = pic->time_base;
        av_dict_copy(&dest->metadata, pic->metadata, 0);
        cover_index = static_cast<int>(i); break;
    }
    if (!(r.output->oformat->flags & AVFMT_NOFILE) && (ret = avio_open(&r.output->pb, output, AVIO_FLAG_WRITE)) < 0) return ret;
    if ((ret = avformat_write_header(r.output, nullptr)) < 0) return ret;
    if (cover_index >= 0) {
        AVPacket* pic = av_packet_clone(&r.input->streams[cover_index]->attached_pic);
        if (!pic) return AVERROR(ENOMEM);
        pic->stream_index = 1;
        ret = av_interleaved_write_frame(r.output, pic);
        av_packet_free(&pic);
        if (ret < 0) return ret;
    }
    if ((ret = swr_alloc_set_opts2(&r.resampler, &r.encoder->ch_layout, r.encoder->sample_fmt, r.encoder->sample_rate,
            &r.decoder->ch_layout, r.decoder->sample_fmt, r.decoder->sample_rate, 0, nullptr)) < 0 ||
        (ret = swr_init(r.resampler)) < 0) return ret;
    r.fifo = av_audio_fifo_alloc(r.encoder->sample_fmt, r.encoder->ch_layout.nb_channels, 1);
    if (!r.fifo) return AVERROR(ENOMEM);
    int64_t pts = 0;
    auto encode = [&](bool flush_fifo) -> int {
        const int frame_size = r.encoder->frame_size ? r.encoder->frame_size : 4096;
        while (av_audio_fifo_size(r.fifo) >= frame_size || (flush_fifo && av_audio_fifo_size(r.fifo) > 0)) {
            int count = std::min(frame_size, av_audio_fifo_size(r.fifo));
            AVFrame* frame = av_frame_alloc();
            if (!frame) return AVERROR(ENOMEM);
            frame->nb_samples = count; frame->format = r.encoder->sample_fmt; frame->sample_rate = r.encoder->sample_rate;
            int result = av_channel_layout_copy(&frame->ch_layout, &r.encoder->ch_layout);
            if (result >= 0) result = av_frame_get_buffer(frame, 0);
            if (result >= 0 && av_audio_fifo_read(r.fifo, reinterpret_cast<void**>(frame->extended_data), count) != count) result = AVERROR_UNKNOWN;
            frame->pts = pts; pts += count;
            if (result >= 0) result = avcodec_send_frame(r.encoder, frame);
            av_frame_free(&frame);
            if (result < 0) return result;
            while ((result = avcodec_receive_packet(r.encoder, r.output_packet)) >= 0) {
                av_packet_rescale_ts(r.output_packet, r.encoder->time_base, out_stream->time_base);
                r.output_packet->stream_index = out_stream->index;
                result = av_interleaved_write_frame(r.output, r.output_packet);
                av_packet_unref(r.output_packet);
                if (result < 0) return result;
            }
            if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) return result;
        }
        return 0;
    };
    auto resample = [&](const AVFrame* source) -> int {
        int capacity = swr_get_out_samples(r.resampler, source ? source->nb_samples : 0);
        if (capacity < 0) return capacity;
        if (!capacity) return 0;
        AVFrame* frame = av_frame_alloc();
        if (!frame) return AVERROR(ENOMEM);
        frame->nb_samples = capacity; frame->format = r.encoder->sample_fmt; frame->sample_rate = r.encoder->sample_rate;
        int result = av_channel_layout_copy(&frame->ch_layout, &r.encoder->ch_layout);
        if (result >= 0) result = av_frame_get_buffer(frame, 0);
        if (result >= 0) result = swr_convert(r.resampler, frame->extended_data, capacity,
            source ? const_cast<const uint8_t**>(source->extended_data) : nullptr, source ? source->nb_samples : 0);
        if (result > 0 && av_audio_fifo_write(r.fifo, reinterpret_cast<void**>(frame->extended_data), result) != result) result = AVERROR(ENOMEM);
        av_frame_free(&frame);
        return result < 0 ? result : encode(false);
    };
    auto receive = [&]() -> int {
        int result;
        while ((result = avcodec_receive_frame(r.decoder, r.decoded)) >= 0) {
            result = resample(r.decoded);
            av_frame_unref(r.decoded);
            if (result < 0) return result;
        }
        return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
    };
    while ((ret = av_read_frame(r.input, r.input_packet)) >= 0) {
        if (r.input_packet->stream_index == index) {
            ret = avcodec_send_packet(r.decoder, r.input_packet);
            if (ret >= 0) ret = receive();
        }
        av_packet_unref(r.input_packet);
        if (ret < 0) return ret;
        if (progress && r.input->pb) {
            int64_t size = avio_size(r.input->pb), pos = avio_tell(r.input->pb);
            if (size > 0 && pos >= 0) progress->store(std::min(0.98f, static_cast<float>(pos) / size));
        }
    }
    if (ret != AVERROR_EOF) return ret;
    if ((ret = avcodec_send_packet(r.decoder, nullptr)) < 0 || (ret = receive()) < 0) return ret;
    while (swr_get_delay(r.resampler, r.encoder->sample_rate) > 0) {
        int64_t before = swr_get_delay(r.resampler, r.encoder->sample_rate);
        if ((ret = resample(nullptr)) < 0) return ret;
        if (swr_get_delay(r.resampler, r.encoder->sample_rate) >= before) break;
    }
    if ((ret = encode(true)) < 0 || (ret = avcodec_send_frame(r.encoder, nullptr)) < 0) return ret;
    while ((ret = avcodec_receive_packet(r.encoder, r.output_packet)) >= 0) {
        av_packet_rescale_ts(r.output_packet, r.encoder->time_base, out_stream->time_base);
        r.output_packet->stream_index = out_stream->index;
        ret = av_interleaved_write_frame(r.output, r.output_packet);
        av_packet_unref(r.output_packet);
        if (ret < 0) return ret;
    }
    if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) return ret;
    if ((ret = av_write_trailer(r.output)) < 0) return ret;
    if (target==AudioFormat::Wav && (ret = WriteWaveTags(r.input, r.output)) < 0) return ret;
    if (r.output->pb) avio_flush(r.output->pb);
    if (r.output->pb && r.output->pb->error < 0) return r.output->pb->error;
    if (progress) progress->store(1.0f);
    return 0;
}
}

bool ReadAudioInfo(const std::string& path, AudioInfo& info, std::string& error) {
    info = AudioInfo{};
    error.clear();
    AVFormatContext* context = nullptr;
    int ret = avformat_open_input(&context, path.c_str(), nullptr, nullptr);
    if (ret >= 0) ret = avformat_find_stream_info(context, nullptr);
    int index = ret >= 0 ? av_find_best_stream(context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : ret;
    if (index < 0) { error = Error(index); avformat_close_input(&context); return false; }
    AVStream* stream = context->streams[index];
    info.sample_rate = stream->codecpar->sample_rate;
    info.channels = stream->codecpar->ch_layout.nb_channels;
    info.duration = context->duration > 0 ? static_cast<double>(context->duration) / AV_TIME_BASE : 0;
    info.codec = avcodec_get_name(stream->codecpar->codec_id);
    info.format = DetectFormat(context);
    const AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(context->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
        info.tags.emplace_back(tag->key, tag->value);
    tag=nullptr;
    while((tag=av_dict_get(stream->metadata,"",tag,AV_DICT_IGNORE_SUFFIX))) {
        bool exists=std::any_of(info.tags.begin(),info.tags.end(),[&](const auto& item){return item.first==tag->key;});
        if(!exists) info.tags.emplace_back(tag->key,tag->value);
    }
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        AVStream* pic = context->streams[i];
        if (!(pic->disposition & AV_DISPOSITION_ATTACHED_PIC) || !pic->attached_pic.size) continue;
        const AVCodec* codec = avcodec_find_decoder(pic->codecpar->codec_id);
        AVCodecContext* decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
        AVFrame* frame = av_frame_alloc();
        if (decoder && frame && avcodec_parameters_to_context(decoder, pic->codecpar) >= 0 &&
            avcodec_open2(decoder, codec, nullptr) >= 0 && avcodec_send_packet(decoder, &pic->attached_pic) >= 0 &&
            avcodec_receive_frame(decoder, frame) >= 0 && frame->width > 0 && frame->height > 0 &&
            frame->width <= 16384 && frame->height <= 16384) {
            const double scale = std::min(1.0, 512.0 / std::max(frame->width, frame->height));
            int w = std::max(1, static_cast<int>(frame->width * scale)), h = std::max(1, static_cast<int>(frame->height * scale));
            SwsContext* scaler = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (scaler) {
                info.cover_rgba.resize(w * h * 4);
                uint8_t* data[] = {info.cover_rgba.data()}; int strides[] = {w * 4};
                if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, data, strides) == h) {
                    info.cover_width = w; info.cover_height = h;
                } else info.cover_rgba.clear();
                sws_freeContext(scaler);
            }
        }
        av_frame_free(&frame); avcodec_free_context(&decoder);
        break;
    }
    avformat_close_input(&context);
    return true;
}
std::string AudioError(int code) { return Error(code); }
int ConvertAudio(const char* input, const char* output, AudioFormat target, int bitrate, std::atomic<float>* progress) {return Convert(input,output,target,bitrate,progress);}
int wav_to_mp3(const char* input, const char* output, int bitrate, std::atomic<float>* progress) { return Convert(input, output, AudioFormat::Mp3, bitrate, progress); }
int mp3_to_wav(const char* input, const char* output, std::atomic<float>* progress) { return Convert(input, output, AudioFormat::Wav, 0, progress); }
const char* FormatName(AudioFormat format) {auto* spec=Spec(format);return spec?spec->name:"Авто";}
const char* FormatExtension(AudioFormat format) {auto* spec=Spec(format);return spec?spec->extension:"";}
bool CanEncode(AudioFormat format) {auto* spec=Spec(format);return spec && Encoder(format)!=nullptr && av_guess_format(spec->muxer,nullptr,nullptr)!=nullptr;}
AudioFormat FormatFromPath(const std::string& path) {
    std::string extension=std::filesystem::u8path(path).extension().u8string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    for(int i=0;i<AudioFormatCount;++i) if(extension==formats[i].extension) return static_cast<AudioFormat>(i);
    return AudioFormat::Unknown;
}
bool ValidateInputFormat(const std::string& path, AudioFormat expected, std::string& error) {
    if(expected==AudioFormat::Unknown) {error="Выберите поддерживаемый исходный формат.";return false;}
    auto extension=FormatFromPath(path);
    if(extension!=expected) {error="Выбранный исходный формат не совпадает с расширением файла.";return false;}
    AVFormatContext* context=nullptr;
    int ret=avformat_open_input(&context,path.c_str(),nullptr,nullptr);
    if(ret>=0) ret=avformat_find_stream_info(context,nullptr);
    bool matches=ret>=0 && DetectFormat(context)==expected && av_find_best_stream(context,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0)>=0;
    avformat_close_input(&context);
    if(!matches) {error=ret<0?"Не удалось прочитать аудиофайл: "+Error(ret):"Содержимое файла не соответствует выбранному формату.";return false;}
    error.clear();return true;
}
