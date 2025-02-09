#include <iostream>
#include <sndfile.h>
#include <filesystem>
#include <string>
#include <fstream>
#include <future>
#include <clocale>
#include <json.hpp>
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#undef max
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace fs = std::filesystem;
using json = nlohmann::json;

json loadTranslations(const std::string& filename) {
    setlocale(LC_ALL, "Russian");
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open translations file!");
    }
    return json::parse(file);
}

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
    return dir + '\\' + filename;
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

    // Open input file.
    if ((ret = avformat_open_input(&input_format_ctx, input_filename, nullptr, nullptr)) < 0) {
        std::cerr << "Could not open input file: " << std::endl;
        return 0;
    }

    // Find stream info
    if ((ret = avformat_find_stream_info(input_format_ctx, nullptr)) < 0) {
        std::cerr << "Failed to find stream info: " << std::endl;
        return 0;
    }

    // Find audio stream
    int audio_stream_index = av_find_best_stream(input_format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_index < 0) {
        std::cerr << "Could not find audio stream\n";
        return 0;
    }

    // Initialize decoder codec
    const AVCodec* input_codec = avcodec_find_decoder(input_format_ctx->streams[audio_stream_index]->codecpar->codec_id);
    input_codec_ctx = avcodec_alloc_context3(input_codec);
    avcodec_parameters_to_context(input_codec_ctx, input_format_ctx->streams[audio_stream_index]->codecpar);

    if ((ret = avcodec_open2(input_codec_ctx, input_codec, nullptr)) < 0) {
        std::cerr << "Failed to open input codec: " << std::endl;
        return 0;
    }

    // Create output format context
    avformat_alloc_output_context2(&output_format_ctx, nullptr, nullptr, output_filename);
    if (!output_format_ctx) {
        std::cerr << "Could not create output context\n";
        return 0;
    }

    // Create output stream
    AVStream* output_stream = avformat_new_stream(output_format_ctx, nullptr);
    if (!output_stream) {
        std::cerr << "Failed to create output stream\n";
        return 0;
    }

    // Configure codec for MP3
    const AVCodec* output_codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
    output_codec_ctx = avcodec_alloc_context3(output_codec);

    output_codec_ctx->bit_rate = 256000;
    output_codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    output_codec_ctx->sample_rate = input_codec_ctx->sample_rate;
    av_channel_layout_copy(&output_codec_ctx->ch_layout, &input_codec_ctx->ch_layout);
    av_channel_layout_default(&output_codec_ctx->ch_layout, 2);

    if (output_format_ctx->oformat->flags & AVFMT_GLOBALHEADER)
    output_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(output_codec_ctx, output_codec, nullptr)) < 0) {
        std::cerr << "Failed to open output codec: " << std::endl;
        goto cleanup;
    }

    // Copy codec parameters to stream
    avcodec_parameters_from_context(output_stream->codecpar, output_codec_ctx);

    // Open output file
    if (!(output_format_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&output_format_ctx->pb, output_filename, AVIO_FLAG_WRITE)) < 0) {
            std::cerr << "Could not open output file: " << std::endl;
            goto cleanup;
        }
    }

    // Write file header
    if ((ret = avformat_write_header(output_format_ctx, nullptr)) < 0) {
        std::cerr << "Failed to write header: " << std::endl;
        goto cleanup;
    }

    // Write file header
    AVChannelLayout stereo_layout;
    av_channel_layout_default(&stereo_layout, 2);

    swr_alloc_set_opts2(&resampler,
        &output_codec_ctx->ch_layout,
        output_codec_ctx->sample_fmt,
        output_codec_ctx->sample_rate,
        &input_codec_ctx->ch_layout,
        input_codec_ctx->sample_fmt,
        input_codec_ctx->sample_rate,
        0, nullptr);
    swr_init(resampler);

    // Process frames
    while (av_read_frame(input_format_ctx, packet) >= 0) {
        if (packet->stream_index == audio_stream_index) {
            ret = avcodec_send_packet(input_codec_ctx, packet);
            while (ret >= 0) {
                ret = avcodec_receive_frame(input_codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                else if (ret < 0) {
                    std::cerr << "Error during decoding: " << std::endl;
                    goto cleanup;
                }

                // Resampling
                AVFrame* resampled_frame = av_frame_alloc();
                resampled_frame->sample_rate = output_codec_ctx->sample_rate;
                resampled_frame->format = output_codec_ctx->sample_fmt;
                av_channel_layout_copy(&resampled_frame->ch_layout, &output_codec_ctx->ch_layout);
                resampled_frame->nb_samples = frame->nb_samples;
                av_frame_get_buffer(resampled_frame, 0);

                swr_convert(resampler, resampled_frame->data, resampled_frame->nb_samples,
                    (const uint8_t**)frame->data, frame->nb_samples);

                // Encoding
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

    // Finalize encoding
    avcodec_send_frame(output_codec_ctx, nullptr);
    while (avcodec_receive_packet(output_codec_ctx, packet) >= 0) {
        av_interleaved_write_frame(output_format_ctx, packet);
        av_packet_unref(packet);
    }

    // Write file trailer
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

//Transfer one mp3 into another file
static std::string OneMp3ToOther(const std::string& input_file, const std::string& output_file) {
    avformat_network_init();

    AVFormatContext* input_format_context = nullptr;
    if (avformat_open_input(&input_format_context, input_file.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Could not open input file." << std::endl;
        return ErrorOut();
    }

    if (avformat_find_stream_info(input_format_context, nullptr) < 0) {
        std::cerr << "Could not find stream information." << std::endl;
        return ErrorOut();
    }

    int audio_stream_index = -1;
    for (int i = 0; i < input_format_context->nb_streams; i++) {
        if (input_format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }

    if (audio_stream_index == -1) {
        std::cerr << "Could not find audio stream." << std::endl;
        return ErrorOut();
    }

    AVStream* audio_stream = input_format_context->streams[audio_stream_index];
    const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    if (!audio_codec) {
        std::cerr << "Could not find codec." << std::endl;
        return ErrorOut();
    }

    AVCodecContext* codec_context = avcodec_alloc_context3(audio_codec);
    if (avcodec_parameters_to_context(codec_context, audio_stream->codecpar) < 0) {
        std::cerr << "Could not copy codec parameters." << std::endl;
        return ErrorOut();
    }

    if (avcodec_open2(codec_context, audio_codec, nullptr) < 0) {
        std::cerr << "Could not open codec." << std::endl;
        return ErrorOut();
    }

    // Open input file
    AVFormatContext* output_format_context = nullptr;
    if (avformat_alloc_output_context2(&output_format_context, nullptr, nullptr, output_file.c_str()) < 0) {
        std::cerr << "Could not create output context." << std::endl;
        return ErrorOut();
    }

    AVStream* output_stream = avformat_new_stream(output_format_context, nullptr);
    if (avcodec_parameters_copy(output_stream->codecpar, audio_stream->codecpar) < 0) {
        std::cerr << "Could not copy codec parameters to output file." << std::endl;
        return ErrorOut();
    }

    if (!(output_format_context->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_format_context->pb, output_file.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Could not open output file." << std::endl;
            return ErrorOut();
        }
    }

    if (avformat_write_header(output_format_context, nullptr) < 0) {
        std::cerr << "Could not write header to output file." << std::endl;
        return ErrorOut();
    }

    // Reading and writing audio packets
    AVPacket packet;
    while (av_read_frame(input_format_context, &packet) >= 0) {
        if (packet.stream_index == audio_stream_index) {
            av_write_frame(output_format_context, &packet);
        }
        av_packet_unref(&packet);
    }

    av_write_trailer(output_format_context);

    // clear
    avcodec_free_context(&codec_context);
    avformat_close_input(&input_format_context);
    avformat_free_context(output_format_context);

    std::cout << "Conversion complete!" << std::endl;
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
    std::memset(&sf_info, 0, sizeof(sf_info));

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

    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);


    std::system("chcp 1251");
    setlocale(LC_ALL, "Russian");

    std::string InputFilePath = "";
    std::string OutputFilePath = "";

    try {
        json translations = loadTranslations("translations.json");
        std::string lang = "ru";
        std::cout << translations[lang]["welcome"] << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    CVRTRtext();


    std::cout << "Enter your input file directory(.wav or .mp3): ";
    InputFilePath = GetFileDirectory();
    fs::path PathToFilePath = InputFilePath;

    while (true) {
        std::cout << "What do you want to do with file?\n";
        std::cout << "1. Get info about file\n" <<
            "2. Convert .wav file into .mp3\n" <<
            "3. Transfer .mp3 file into another .mp3\n" <<
            "4. If you want to change input file(.wav or .mp3)\n" <<
            "Press 0 if you want to quit\n";
        int ans = 0;
        std::cin >> ans;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (ans == 1) {
            PrintInfoAboutFile(InputFilePath);
        }
        else if (ans == 2) {
            if (OutputFilePath == "") {
                std::cout << "Enter path to your output file(.mp3): ";
                OutputFilePath = GetFileDirectory();
            }
                wav_to_mp3(InputFilePath.data(), OutputFilePath.data());
        }
        else if (ans == 3) {
            if (OutputFilePath == "") {
                std::cout << "Enter path to your output file(.mp3): ";
                OutputFilePath = GetFileDirectory();
            }
            wav_to_mp3(InputFilePath.data(), OutputFilePath.data());
        }
        else if (ans == 4) {
            InputFilePath == "";
            std::cout << "Enter your input file directory(.wav or .mp3): ";
            std::string InputFilePath = GetFileDirectory();
        }
        else if (ans == 0) {
            break;
        }
    }
    return 0;
}