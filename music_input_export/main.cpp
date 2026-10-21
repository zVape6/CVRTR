#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "portable-file-dialogs.h"

#include <GLFW/glfw3.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

std::string CopyAudioFile(const std::string& input_file, const std::string& output_file);
int wav_to_mp3(char* inputfile, char* outputfile);
int mp3_to_wav(char* inputfile, char* outputfile);

static char g_input_path[512] = "";
static char g_output_path[512] = "";
static int g_current_mode = 0;

void UpdateOutputPath() {
    if (std::strlen(g_input_path) == 0) return;

    fs::path in_p(g_input_path);
    fs::path out_p;

    if (g_current_mode == 0) {
        out_p = in_p.parent_path() / (in_p.stem().string() + ".mp3");
    } else if (g_current_mode == 1) {
        out_p = in_p.parent_path() / (in_p.stem().string() + ".wav");
    } else {
        out_p = in_p.parent_path() / (in_p.stem().string() + "_copy" + in_p.extension().string());
    }

    std::snprintf(g_output_path, sizeof(g_output_path), "%s", out_p.string().c_str());
}

void DropCallback(GLFWwindow* /*window*/, int count, const char** paths) {
    if (count > 0 && paths[0]) {
        std::snprintf(g_input_path, sizeof(g_input_path), "%s", paths[0]);
        UpdateOutputPath();
    }
}

int main() {
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(680, 440, "CVRTR - Audio Converter", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, DropCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

#ifdef _WIN32
    const char* font_path = "C:\\Windows\\Fonts\\segoeui.ttf";
#else
    const char* font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    if (fs::exists(font_path)) {
        io.Fonts->AddFontFromFileTTF(font_path, 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    }
    ImFont* logo_font = io.Fonts->AddFontDefault(); // Встроенный моноширинный шрифт

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    const char* logo =
        "   ____  __   ______ _____ ____  \n"
        "  / ___\\|  | / /  _ \\_   _|  _ \\ \n"
        " | |    |  |/ /| |_) || | | |_) |\n"
        " | |___ |    / |  _ < | | |  _ < \n"
        "  \\____||___/  |_| \\_\\|_| |_| \\_\\";

    const char* modes[] = {
        "Конвертировать WAV -> MP3",
        "Конвертировать MP3 -> WAV",
        "Копировать аудиофайл (без перекодирования)"
    };

    std::atomic<bool> is_working{false};
    std::string status_text = "Готов к работе. Выберите файл или перетащите его в окно.";
    bool last_op_success = true;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse;

        ImGui::Begin("CVRTR Workspace", nullptr, window_flags);

        // Центрированный логотип в 4 строчки
        ImGui::PushFont(logo_font);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(logo).x) * 0.5f);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "%s", logo);
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginDisabled(is_working.load());

        ImGui::Text("Режим работы:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##mode", &g_current_mode, modes, IM_ARRAYSIZE(modes))) {
            UpdateOutputPath();
        }

        ImGui::Spacing();

        ImGui::Text("Исходный файл:");
        ImGui::SetNextItemWidth(io.DisplaySize.x - 140.0f);
        if (ImGui::InputText("##input", g_input_path, sizeof(g_input_path))) {
            UpdateOutputPath();
        }
        ImGui::SameLine();
        if (ImGui::Button("Обзор...##in", ImVec2(110.0f, 0.0f))) {
            std::vector<std::string> filters;
            if (g_current_mode == 0) {
                filters = { "WAV files (.wav)", "*.wav", "All files", "*" };
            } else if (g_current_mode == 1) {
                filters = { "MP3 files (.mp3)", "*.mp3", "All files", "*" };
            } else {
                filters = { "Audio files (.wav, .mp3)", "*.wav *.mp3", "All files", "*" };
            }

            auto selection = pfd::open_file("Выберите аудиофайл", ".", filters).result();
            if (!selection.empty()) {
                std::snprintf(g_input_path, sizeof(g_input_path), "%s", selection[0].c_str());
                UpdateOutputPath();
            }
        }

        ImGui::Spacing();

        ImGui::Text("Куда сохранить результат:");
        ImGui::SetNextItemWidth(io.DisplaySize.x - 140.0f);
        ImGui::InputText("##output", g_output_path, sizeof(g_output_path));
        ImGui::SameLine();
        if (ImGui::Button("Обзор...##out", ImVec2(110.0f, 0.0f))) {
            std::string default_out = std::strlen(g_output_path) > 0 ? g_output_path : ".";
            auto dest = pfd::save_file("Куда сохранить файл", default_out, { "Audio files", "*.mp3 *.wav", "All files", "*" }).result();
            if (!dest.empty()) {
                std::snprintf(g_output_path, sizeof(g_output_path), "%s", dest.c_str());
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Выполнить", ImVec2(180.0f, 40.0f))) {
            std::string in_str(g_input_path);
            std::string out_str(g_output_path);

            if (in_str.empty() || !fs::exists(in_str)) {
                status_text = "Ошибка: исходный файл не найден!";
                last_op_success = false;
            } else if (out_str.empty()) {
                status_text = "Ошибка: укажите путь для сохранения файла!";
                last_op_success = false;
            } else {
                is_working = true;
                status_text = "Обработка файла, подождите...";
                int mode_copy = g_current_mode;

                std::thread([in_str, out_str, mode_copy, &is_working, &status_text, &last_op_success]() {
                    std::vector<char> in_buf(in_str.begin(), in_str.end());
                    in_buf.push_back('\0');
                    std::vector<char> out_buf(out_str.begin(), out_str.end());
                    out_buf.push_back('\0');

                    if (mode_copy == 0) {
                        int res = wav_to_mp3(in_buf.data(), out_buf.data());
                        last_op_success = (res == 0);
                        status_text = last_op_success
                            ? "Готово! WAV успешно сконвертирован в MP3: " + out_str
                            : "Ошибка при конвертации WAV -> MP3 (код " + std::to_string(res) + ").";
                    } else if (mode_copy == 1) {
                        int res = mp3_to_wav(in_buf.data(), out_buf.data());
                        last_op_success = (res == 0);
                        status_text = last_op_success
                            ? "Готово! MP3 успешно сконвертирован в WAV: " + out_str
                            : "Ошибка при конвертации MP3 -> WAV (код " + std::to_string(res) + ").";
                    } else {
                        std::string res = CopyAudioFile(in_str, out_str);
                        last_op_success = !res.empty() && fs::exists(out_str);
                        status_text = last_op_success
                            ? "Готово! Файл скопирован: " + out_str
                            : "Ошибка при копировании файла.";
                    }

                    is_working = false;
                }).detach();
            }
        }

        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (is_working.load()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Статус: %s", status_text.c_str());
        } else if (last_op_success) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Статус: %s", status_text.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Статус: %s", status_text.c_str());
        }

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}