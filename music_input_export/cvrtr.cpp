#include <iostream>
#include <sndfile.h>
#include <filesystem>
#include <string>
#include <fstream>
#include <future>
#include <clocale>
#include <fcntl.h>
#undef max
extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace fs = std::filesystem;

//Throws a string error
static std::string ErrorOut() {
    return "Error!";
}

//Function to get files full path
std::string GetFileDirectory() {
    // Ask user to enter the directory where the file is located
    std::string dir;
    std::getline(std::cin, dir);

    // Check if the directory exists
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::cout << "Invalid directory!" << std::endl;
        return ErrorOut();
    }

    // List all files in the directory
    std::cout << "Files in directory: " << std::endl;
    for (const auto& entry : fs::directory_iterator(dir)) {
        // Print the filename directly as a string
        std::cout << entry.path().filename().string() << std::endl;
    }

    // Ask user to choose a file
    std::string filename;
    std::cout << "Enter the filename (including extension) you want to open: ";
    std::getline(std::cin, filename);

    // Full path to the file
    fs::path filePath = fs::path(dir) / filename;

    // Check if the file exists
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
        std::cout << "Invalid file!" << std::endl;
        return ErrorOut();
    }
    return filePath.string();
}

//Converts PathToFile(.wav) into Outputfile(.mp3)
static int wav_to_mp3(char* inputfile, char* outputfile) {

    char* input_filename = inputfile;
    char* output_filename = outputfile;

    AVFormatContext* input_format_ctx = nullptr;
    AVFormatContext* output_format_ctx = nullptr;
    AVCodecContext* input_codec_ctx = nullptr;
    AVCodecContext* output_codec_ctx = nullptr;
    SwrContext* resampler = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    int ret = 0;

    int audio_stream_index = -1;
    const AVCodec* input_codec = nullptr;
    AVStream* output_stream = nullptr;
    const AVCodec* output_codec = nullptr;

    // Open input file.
    if ((ret = avformat_open_input(&input_format_ctx, input_filename, nullptr, nullptr)) < 0) {
        std::cerr << "Could not open input file\n";
        return 0;
    }

    if ((ret = avformat_find_stream_info(input_format_ctx, nullptr)) < 0) {
        std::cerr << "Failed to find stream info\n";
        goto cleanup;
    }

    audio_stream_index = av_find_best_stream(input_format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_index < 0) {
        std::cerr << "Could not find audio stream\n";
        goto cleanup;
    }

    input_codec = avcodec_find_decoder(input_format_ctx->streams[audio_stream_index]->codecpar->codec_id);
    input_codec_ctx = avcodec_alloc_context3(input_codec);
    avcodec_parameters_to_context(input_codec_ctx, input_format_ctx->streams[audio_stream_index]->codecpar);

    if ((ret = avcodec_open2(input_codec_ctx, input_codec, nullptr)) < 0) {
        std::cerr << "Failed to open input codec\n";
        goto cleanup;
    }

    avformat_alloc_output_context2(&output_format_ctx, nullptr, nullptr, output_filename);
    if (!output_format_ctx) {
        std::cerr << "Could not create output context\n";
        goto cleanup;
    }

    output_stream = avformat_new_stream(output_format_ctx, nullptr);
    if (!output_stream) {
        std::cerr << "Failed to create output stream\n";
        goto cleanup;
    }

    output_codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
    output_codec_ctx = avcodec_alloc_context3(output_codec);

    output_codec_ctx->bit_rate = 256000;
    output_codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    output_codec_ctx->sample_rate = input_codec_ctx->sample_rate;
    av_channel_layout_copy(&output_codec_ctx->ch_layout, &input_codec_ctx->ch_layout);
    av_channel_layout_default(&output_codec_ctx->ch_layout, 2);

    if (output_format_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        output_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(output_codec_ctx, output_codec, nullptr)) < 0) {
        std::cerr << "Failed to open output codec\n";
        goto cleanup;
    }

    avcodec_parameters_from_context(output_stream->codecpar, output_codec_ctx);

    if (!(output_format_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&output_format_ctx->pb, output_filename, AVIO_FLAG_WRITE)) < 0) {
            std::cerr << "Could not open output file\n";
            goto cleanup;
        }
    }

    if ((ret = avformat_write_header(output_format_ctx, nullptr)) < 0) {
        std::cerr << "Failed to write header\n";
        goto cleanup;
    }

    swr_alloc_set_opts2(&resampler,
        &output_codec_ctx->ch_layout,
        output_codec_ctx->sample_fmt,
        output_codec_ctx->sample_rate,
        &input_codec_ctx->ch_layout,
        input_codec_ctx->sample_fmt,
        input_codec_ctx->sample_rate,
        0, nullptr);
    swr_init(resampler);

    {
        AVAudioFifo* fifo = av_audio_fifo_alloc(output_codec_ctx->sample_fmt, output_codec_ctx->ch_layout.nb_channels, 1);
        int64_t current_pts = 0;

        while (av_read_frame(input_format_ctx, packet) >= 0) {
            if (packet->stream_index == audio_stream_index) {
                ret = avcodec_send_packet(input_codec_ctx, packet);
                while (ret >= 0) {
                    ret = avcodec_receive_frame(input_codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    else if (ret < 0) {
                        std::cerr << "Error during decoding\n";
                        goto cleanup;
                    }

                    int out_samples = swr_get_out_samples(resampler, frame->nb_samples);
                    AVFrame* resampled_frame = av_frame_alloc();
                    resampled_frame->sample_rate = output_codec_ctx->sample_rate;
                    resampled_frame->format = output_codec_ctx->sample_fmt;
                    av_channel_layout_copy(&resampled_frame->ch_layout, &output_codec_ctx->ch_layout);
                    resampled_frame->nb_samples = out_samples;
                    av_frame_get_buffer(resampled_frame, 0);

                    int converted = swr_convert(resampler, resampled_frame->data, out_samples,
                        (const uint8_t**)frame->data, frame->nb_samples);

                    av_audio_fifo_write(fifo, (void**)resampled_frame->data, converted);
                    av_frame_free(&resampled_frame);

                    while (av_audio_fifo_size(fifo) >= output_codec_ctx->frame_size) {
                        AVFrame* encode_frame = av_frame_alloc();
                        encode_frame->sample_rate = output_codec_ctx->sample_rate;
                        encode_frame->format = output_codec_ctx->sample_fmt;
                        av_channel_layout_copy(&encode_frame->ch_layout, &output_codec_ctx->ch_layout);
                        encode_frame->nb_samples = output_codec_ctx->frame_size;
                        av_frame_get_buffer(encode_frame, 0);

                        av_audio_fifo_read(fifo, (void**)encode_frame->data, output_codec_ctx->frame_size);
                        encode_frame->pts = current_pts;
                        current_pts += encode_frame->nb_samples;

                        avcodec_send_frame(output_codec_ctx, encode_frame);
                        while (avcodec_receive_packet(output_codec_ctx, packet) >= 0) {
                            av_interleaved_write_frame(output_format_ctx, packet);
                            av_packet_unref(packet);
                        }
                        av_frame_free(&encode_frame);
                    }
                }
            }
            av_packet_unref(packet);
        }

        while (av_audio_fifo_size(fifo) > 0) {
            int left_samples = av_audio_fifo_size(fifo) < output_codec_ctx->frame_size ? av_audio_fifo_size(fifo) : output_codec_ctx->frame_size;
            AVFrame* encode_frame = av_frame_alloc();
            encode_frame->sample_rate = output_codec_ctx->sample_rate;
            encode_frame->format = output_codec_ctx->sample_fmt;
            av_channel_layout_copy(&encode_frame->ch_layout, &output_codec_ctx->ch_layout);
            encode_frame->nb_samples = left_samples;
            av_frame_get_buffer(encode_frame, 0);

            av_audio_fifo_read(fifo, (void**)encode_frame->data, left_samples);
            encode_frame->pts = current_pts;
            current_pts += encode_frame->nb_samples;

            avcodec_send_frame(output_codec_ctx, encode_frame);
            while (avcodec_receive_packet(output_codec_ctx, packet) >= 0) {
                av_interleaved_write_frame(output_format_ctx, packet);
                av_packet_unref(packet);
            }
            av_frame_free(&encode_frame);
        }

        avcodec_send_frame(output_codec_ctx, nullptr);
        while (avcodec_receive_packet(output_codec_ctx, packet) >= 0) {
            av_interleaved_write_frame(output_format_ctx, packet);
            av_packet_unref(packet);
        }

        av_audio_fifo_free(fifo);
    }

    av_write_trailer(output_format_ctx);


cleanup:
    avformat_close_input(&input_format_ctx);
    if (output_format_ctx && !(output_format_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_format_ctx->pb);
    avformat_free_context(output_format_ctx);
    avcodec_free_context(&input_codec_ctx);
    avcodec_free_context(&output_codec_ctx);
    swr_free(&resampler);
    av_frame_free(&frame);
    av_packet_free(&packet);

    std::cout << "Done!\n";
    return ret < 0 ? 1 : 0;
}

// Converts mp3 to wav
static int mp3_to_wav(char* inputfile, char* outputfile) {
    char* input_filename = inputfile;
    char* output_filename = outputfile;

    AVFormatContext* input_format_ctx = nullptr;
    AVFormatContext* output_format_ctx = nullptr;
    AVCodecContext* input_codec_ctx = nullptr;
    AVCodecContext* output_codec_ctx = nullptr;
    SwrContext* resampler = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    int ret = 0;

    int audio_stream_index = -1;
    const AVCodec* input_codec = nullptr;
    AVStream* output_stream = nullptr;
    const AVCodec* output_codec = nullptr;

    if ((ret = avformat_open_input(&input_format_ctx, input_filename, nullptr, nullptr)) < 0) {
        std::cerr << "Could not open input file\n";
        return 0;
    }

    if ((ret = avformat_find_stream_info(input_format_ctx, nullptr)) < 0) {
        std::cerr << "Failed to find stream info\n";
        goto cleanup;
    }

    audio_stream_index = av_find_best_stream(input_format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_index < 0) {
        std::cerr << "Could not find audio stream\n";
        goto cleanup;
    }

    input_codec = avcodec_find_decoder(input_format_ctx->streams[audio_stream_index]->codecpar->codec_id);
    input_codec_ctx = avcodec_alloc_context3(input_codec);
    avcodec_parameters_to_context(input_codec_ctx, input_format_ctx->streams[audio_stream_index]->codecpar);

    if ((ret = avcodec_open2(input_codec_ctx, input_codec, nullptr)) < 0) {
        std::cerr << "Failed to open input codec\n";
        goto cleanup;
    }

    avformat_alloc_output_context2(&output_format_ctx, nullptr, nullptr, output_filename);
    if (!output_format_ctx) {
        std::cerr << "Could not create output context\n";
        goto cleanup;
    }

    output_stream = avformat_new_stream(output_format_ctx, nullptr);
    if (!output_stream) {
        std::cerr << "Failed to create output stream\n";
        goto cleanup;
    }

    output_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    output_codec_ctx = avcodec_alloc_context3(output_codec);

    output_codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    output_codec_ctx->sample_rate = input_codec_ctx->sample_rate;
    av_channel_layout_copy(&output_codec_ctx->ch_layout, &input_codec_ctx->ch_layout);
    
    if (output_format_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        output_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(output_codec_ctx, output_codec, nullptr)) < 0) {
        std::cerr << "Failed to open output codec\n";
        goto cleanup;
    }

    avcodec_parameters_from_context(output_stream->codecpar, output_codec_ctx);

    if (!(output_format_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&output_format_ctx->pb, output_filename, AVIO_FLAG_WRITE)) < 0) {
            std::cerr << "Could not open output file\n";
            goto cleanup;
        }
    }

    if ((ret = avformat_write_header(output_format_ctx, nullptr)) < 0) {
        std::cerr << "Failed to write header\n";
        goto cleanup;
    }

    swr_alloc_set_opts2(&resampler,
        &output_codec_ctx->ch_layout,
        output_codec_ctx->sample_fmt,
        output_codec_ctx->sample_rate,
        &input_codec_ctx->ch_layout,
        input_codec_ctx->sample_fmt,
        input_codec_ctx->sample_rate,
        0, nullptr);
    swr_init(resampler);

    {
        while (av_read_frame(input_format_ctx, packet) >= 0) {
            if (packet->stream_index == audio_stream_index) {
                ret = avcodec_send_packet(input_codec_ctx, packet);
                while (ret >= 0) {
                    ret = avcodec_receive_frame(input_codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    else if (ret < 0) {
                        std::cerr << "Error during decoding\n";
                        goto cleanup;
                    }

                    int out_samples = swr_get_out_samples(resampler, frame->nb_samples);
                    AVFrame* resampled_frame = av_frame_alloc();
                    resampled_frame->sample_rate = output_codec_ctx->sample_rate;
                    resampled_frame->format = output_codec_ctx->sample_fmt;
                    av_channel_layout_copy(&resampled_frame->ch_layout, &output_codec_ctx->ch_layout);
                    resampled_frame->nb_samples = out_samples;
                    av_frame_get_buffer(resampled_frame, 0);

                    swr_convert(resampler, resampled_frame->data, out_samples,
                        (const uint8_t**)frame->data, frame->nb_samples);
                    
                    resampled_frame->pts = frame->pts;

                    avcodec_send_frame(output_codec_ctx, resampled_frame);
                    while (avcodec_receive_packet(output_codec_ctx, packet) >= 0) {
                        av_interleaved_write_frame(output_format_ctx, packet);
                        av_packet_unref(packet);
                    }
                    av_frame_free(&resampled_frame);
                }
            }
            av_packet_unref(packet);
        }

        avcodec_send_frame(output_codec_ctx, nullptr);
        while (avcodec_receive_packet(output_codec_ctx, packet) >= 0) {
            av_interleaved_write_frame(output_format_ctx, packet);
            av_packet_unref(packet);
        }
    }

    av_write_trailer(output_format_ctx);

cleanup:
    avformat_close_input(&input_format_ctx);
    if (output_format_ctx && !(output_format_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_format_ctx->pb);
    avformat_free_context(output_format_ctx);
    avcodec_free_context(&input_codec_ctx);
    avcodec_free_context(&output_codec_ctx);
    swr_free(&resampler);
    av_frame_free(&frame);
    av_packet_free(&packet);

    std::cout << "Conversion to WAV complete!\n";
    return ret < 0 ? 1 : 0;
}

// Creates a copy of your chosen file with "_copy" at the end
static std::string CopyAudioFile(const std::string& input_file, const std::string& output_file) {
    AVFormatContext* input_format_context = nullptr;
    AVFormatContext* output_format_context = nullptr;
    AVPacket* packet = av_packet_alloc();
    int ret;

    if (avformat_open_input(&input_format_context, input_file.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Could not open input file.\n";
        return ErrorOut();
    }

    if (avformat_find_stream_info(input_format_context, nullptr) < 0) {
        std::cerr << "Could not find stream information.\n";
        avformat_close_input(&input_format_context);
        return ErrorOut();
    }

    int audio_stream_index = av_find_best_stream(input_format_context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_index < 0) {
        std::cerr << "Could not find audio stream.\n";
        avformat_close_input(&input_format_context);
        return ErrorOut();
    }
    AVStream* in_stream = input_format_context->streams[audio_stream_index];

    avformat_alloc_output_context2(&output_format_context, nullptr, nullptr, output_file.c_str());
    if (!output_format_context) {
        std::cerr << "Could not create output context.\n";
        avformat_close_input(&input_format_context);
        return ErrorOut();
    }

    AVStream* out_stream = avformat_new_stream(output_format_context, nullptr);
    if (!out_stream) {
        std::cerr << "Failed allocating output stream.\n";
        avformat_free_context(output_format_context);
        avformat_close_input(&input_format_context);
        return ErrorOut();
    }

    ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    if (ret < 0) {
        std::cerr << "Failed to copy codec parameters.\n";
        goto end;
    }
    out_stream->codecpar->codec_tag = 0;

    if (!(output_format_context->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output_format_context->pb, output_file.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::cerr << "Could not open output file.\n";
            goto end;
        }
    }

    ret = avformat_write_header(output_format_context, nullptr);
    if (ret < 0) {
        std::cerr << "Error occurred when opening output file.\n";
        goto end;
    }

    while (av_read_frame(input_format_context, packet) >= 0) {
        if (packet->stream_index == audio_stream_index) {
            av_packet_rescale_ts(packet, in_stream->time_base, out_stream->time_base);
            packet->stream_index = out_stream->index;
            
            ret = av_interleaved_write_frame(output_format_context, packet);
            if (ret < 0) {
                std::cerr << "Error muxing packet\n";
                break;
            }
        }
        av_packet_unref(packet);
    }
    av_write_trailer(output_format_context);

end:
    if (output_format_context && !(output_format_context->oformat->flags & AVFMT_NOFILE))
    avio_closep(&output_format_context->pb);
    avformat_free_context(output_format_context);
    avformat_close_input(&input_format_context);
    av_packet_free(&packet);

    std::cout << "Copy complete!\n";
    return "Done!";
}

//Welcome text function
void CVRTRtext() {
    std::string line;
    std::ifstream read("cvrtr.txt");
    for (int i = 0; i < 5; i++) {
        std::getline(read, line);
        std::cout << line << std::endl;
    }
}

//Prints info about file or throwing error
void PrintInfoAboutFile(fs::path filePath) {
    // Open the file using libsndfile
    SF_INFO sf_info;
    SNDFILE* snd_file;

    // Initialize the SF_INFO struct
    memset(&sf_info, 0, sizeof(sf_info));

    // Open the audio file for reading
    snd_file = sf_open(filePath.string().c_str(), SFM_READ, &sf_info);  // Convert path to c_str() here
    if (snd_file == nullptr) {
        std::cout << "Error opening file: " << sf_strerror(nullptr) << std::endl;
        return;
    }

    // Print file information
    std::cout << "Sample Rate: " << sf_info.samplerate << std::endl;
    std::cout << "Channels: " << sf_info.channels << std::endl;
    std::cout << "Frames: " << sf_info.frames << std::endl;
    std::cout << "Format: " << sf_info.format << std::endl;

    // Close the file
    sf_close(snd_file);
}

int main() {

    std::string InputFilePath = "";
    std::string OutputFilePath = "";


    CVRTRtext();


    std::cout << "Enter your input file directory(.wav or .mp3): ";
    InputFilePath = GetFileDirectory();
    fs::path PathToFilePath = InputFilePath;

    while (true) {
        std::cout << "What do you want to do with file?\n";
        std::cout << "1. Get info about file\n" <<
            "2. Convert .wav file into .mp3\n" <<
            "3. Copy your music file (.mp3 and .wav) and saves it to the same direction\n" <<
            "4. If you want to change input file(.wav or .mp3)\n" <<
            "5. Converts .mp3 file into .wav\n" << 
            "Press 0 if you want to quit\n";
        int ans = 0;
        std::cin >> ans;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (ans == 1) {
            PrintInfoAboutFile(InputFilePath);
        }
        else if (ans == 2) {
            fs::path AutoOutPath = InputFilePath;
            AutoOutPath.replace_extension(".mp3");
            OutputFilePath = AutoOutPath.string();
            wav_to_mp3(InputFilePath.data(), OutputFilePath.data());
        }
        else if (ans == 3) {
            fs::path AutoOutPath = InputFilePath;
            std::string original_stem = AutoOutPath.stem().string();
            std::string ExtensionOfTheOriginalFile = AutoOutPath.extension().string();

            AutoOutPath.replace_filename(original_stem + "_copy" + ExtensionOfTheOriginalFile);
            OutputFilePath = AutoOutPath.string();

            CopyAudioFile(InputFilePath, OutputFilePath);
        }
        else if (ans == 4) {
            InputFilePath = "";
            std::cout << "Enter your input file directory(.wav or .mp3): ";
            InputFilePath = GetFileDirectory();
        }
        else if (ans == 5){
            fs::path AutoOutPath = InputFilePath;
            AutoOutPath.replace_extension(".wav");
            OutputFilePath = AutoOutPath.string();
            mp3_to_wav(InputFilePath.data(), OutputFilePath.data());
        }
        else if (ans == 0) {
            break;
        }
    }
    return 0;
}