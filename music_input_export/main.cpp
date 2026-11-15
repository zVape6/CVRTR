// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 zVape6 and CVRTR contributors.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "portable-file-dialogs.h"
#include "converter.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <future>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif
#include "window_chrome.h"

namespace fs = std::filesystem;
static char input_path[4096] = "", output_path[4096] = "";
static AudioFormat source_format=AudioFormat::Unknown,target_format=AudioFormat::Mp3;
static bool copy_mode=false, source_changed=false;
static bool busy = false;
static fs::path Path(const std::string& value) { return fs::u8path(value); }
static void SetPath(char* buffer, size_t size, const std::string& value) { std::snprintf(buffer, size, "%s", value.c_str()); }
static void SuggestOutput() {
    if (!input_path[0]) return;
    fs::path in = Path(input_path);
    auto filename=in.stem().u8string()+(copy_mode?"_copy"+in.extension().u8string():
        (source_format==target_format?"_converted":"")+std::string(FormatExtension(target_format)));
    SetPath(output_path, sizeof(output_path), (in.parent_path() / Path(filename)).u8string());
}
static void InputChanged() {source_format=FormatFromPath(input_path);source_changed=true;SuggestOutput();}
static void Drop(GLFWwindow*, int count, const char** paths) {
    if (!busy && count > 0 && paths[0]) { SetPath(input_path, sizeof(input_path), paths[0]); InputChanged(); }
}
struct Result { bool success; std::string text; std::string path; };
static Result Process(std::string input, std::string output, AudioFormat source,AudioFormat target,bool copy,int bitrate, bool replace, std::atomic<float>& progress) {
    fs::path temporary, backup;
    try {
        std::string error;if(!ValidateInputFormat(input,source,error)) throw std::runtime_error(error);
        auto out = Path(output);
        std::string stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temporary = out.parent_path() / Path(out.filename().u8string() + ".cvrtr-" + stamp + ".tmp");
        backup = out.parent_path() / Path(out.filename().u8string() + ".cvrtr-" + stamp + ".bak");
        int code = 0;
        if(!copy) code=ConvertAudio(input.c_str(),temporary.u8string().c_str(),target,bitrate,&progress);
        else fs::copy_file(Path(input),temporary);
        if (code < 0) throw std::runtime_error(AudioError(code));
        bool exists = fs::exists(out);
        if (exists && (!replace || !fs::is_regular_file(out) || fs::equivalent(Path(input), out)))
            throw std::runtime_error("Файл назначения изменился. Выберите другой путь.");
        if (exists) fs::rename(out, backup);
        try { fs::rename(temporary, out); }
        catch (...) { if (exists) fs::rename(backup, out); throw; }
        if (exists) { std::error_code ec; fs::remove(backup, ec); }
        progress = 1;
        return {true, "Готово. Результат сохранён.", output};
    } catch (const std::exception& e) {
        if (!temporary.empty()) { std::error_code ec; fs::remove(temporary, ec); }
        return {false, std::string("Ошибка: ") + e.what(), ""};
    }
}
static std::string Tag(const AudioInfo& info, const char* key, const std::string& fallback = "") {
    for (const auto& tag : info.tags) if (tag.first == key) return tag.second;
    return fallback;
}
static void OpenFolder(const std::string& file) {
    auto directory = fs::absolute(Path(file)).parent_path();
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    if (fork() == 0) { execlp("xdg-open", "xdg-open", directory.c_str(), (char*)nullptr); _exit(127); }
#endif
}
static void Theme() {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(24, 20); s.FramePadding = ImVec2(12, 9); s.ItemSpacing = ImVec2(12, 12);
    s.WindowBorderSize = 0; s.ChildBorderSize = 0; s.FrameRounding = 10; s.PopupRounding = 12;
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0,0,0,0);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(.09f,.14f,.23f,.98f);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0,0,0,0);
    s.Colors[ImGuiCol_Text] = ImVec4(.94f,.94f,.98f,1);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(.55f,.59f,.72f,1);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(.40f,.57f,.75f,.11f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.48f,.67f,.83f,.19f);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(.48f,.67f,.83f,.26f);
    s.Colors[ImGuiCol_Button] = ImVec4(.42f,.65f,.84f,.22f);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(.48f,.72f,.91f,.34f);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(.40f,.66f,.86f,.44f);
    s.Colors[ImGuiCol_Header] = s.Colors[ImGuiCol_Button];
    s.Colors[ImGuiCol_HeaderHovered] = s.Colors[ImGuiCol_ButtonHovered];
    s.Colors[ImGuiCol_HeaderActive] = s.Colors[ImGuiCol_ButtonActive];
    s.Colors[ImGuiCol_Separator] = ImVec4(.68f,.81f,.94f,.12f);
    s.Colors[ImGuiCol_PlotHistogram] = ImVec4(.53f,.80f,.94f,.85f);
}

static bool NavigationButton(const char* label, bool selected) {
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,ImVec2(.5f,.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,10);
    ImGui::PushStyleColor(ImGuiCol_Button,selected?ImVec4(.54f,.76f,.95f,.13f):ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.56f,.78f,.97f,.19f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.56f,.78f,.97f,.24f));
    bool clicked=ImGui::Button(label,ImVec2(-1,40));
    if(selected) {
        ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        float alpha=ImGui::GetStyle().Alpha;
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddRect(a,b,IM_COL32(182,220,250,static_cast<int>(32*alpha)),10);
        float center=(a.x+b.x)*.5f;
        draw->AddLine(ImVec2(center-18,b.y-3),ImVec2(center+18,b.y-3),IM_COL32(174,221,255,static_cast<int>(105*alpha)),1);
    }
    ImGui::PopStyleColor(3); ImGui::PopStyleVar(2);
    return clicked;
}

static bool SidebarToggle(bool open) {
    bool clicked=ImGui::Button("##toggle-sidebar",ImVec2(28,28));
    ImVec2 p=ImGui::GetItemRectMin();
    auto* draw=ImGui::GetWindowDrawList();
    ImU32 color=ImGui::GetColorU32(ImVec4(.85f,.94f,1,.85f));
    draw->AddRect(ImVec2(p.x+6,p.y+7),ImVec2(p.x+22,p.y+21),color,2);
    draw->AddLine(ImVec2(p.x+10,p.y+8),ImVec2(p.x+10,p.y+20),color,1);
    float tip=open?14.f:19.f,tail=open?18.f:15.f;
    draw->AddLine(ImVec2(p.x+tail,p.y+11),ImVec2(p.x+tip,p.y+14),color,1.3f);
    draw->AddLine(ImVec2(p.x+tip,p.y+14),ImVec2(p.x+tail,p.y+17),color,1.3f);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",open?"Скрыть боковую панель":"Показать боковую панель");
    return clicked;
}

static bool FormatDropdown(const char* id,AudioFormat& value,bool output) {
    ImVec2 p=ImGui::GetCursorScreenPos();auto* draw=ImGui::GetWindowDrawList();
    ImU32 color=ImGui::GetColorU32(ImVec4(.75f,.86f,.96f,.8f));
    ImGui::SetNextItemWidth(67);bool changed=false;
    if(ImGui::BeginCombo(id,FormatName(value),ImGuiComboFlags_NoArrowButton)) {
        for(int i=0;i<AudioFormatCount;++i) {
            auto candidate=static_cast<AudioFormat>(i);bool available=!output || CanEncode(candidate);
            ImGui::BeginDisabled(!available);
            if(ImGui::Selectable(FormatName(candidate),value==candidate)) {value=candidate;changed=true;}
            ImGui::EndDisabled();
            if(!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Кодировщик отсутствует в этой сборке FFmpeg");
        }
        ImGui::EndCombo();
    }
    draw->AddLine(ImVec2(p.x+54,p.y+15),ImVec2(p.x+58,p.y+19),color,1);
    draw->AddLine(ImVec2(p.x+58,p.y+19),ImVec2(p.x+62,p.y+15),color,1);
    return changed;
}

#ifdef _WIN32
static void WindowControls(GLFWwindow* window,float width) {
    auto* draw=ImGui::GetWindowDrawList();
    ImGui::SetCursorPos(ImVec2(width-150,7));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(4,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,10);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(1,1,1,.025f));
    for(int i=0;i<3;++i) {
        if(i) ImGui::SameLine();
        ImGui::PushID(i);
        if(i==2) ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.85f,.26f,.34f,.55f));
        bool clicked=ImGui::Button("##window",ImVec2(40,24));
        ImVec2 p=ImGui::GetItemRectMin();
        p.y-=6;
        ImU32 color=IM_COL32(219,237,253,210);
        if(i==0) draw->AddLine(ImVec2(p.x+14,p.y+19),ImVec2(p.x+26,p.y+19),color,1.3f);
        if(i==1) {
            if(glfwGetWindowAttrib(window,GLFW_MAXIMIZED)) {
                draw->AddRect(ImVec2(p.x+16,p.y+11),ImVec2(p.x+26,p.y+21),color,1);
                draw->AddRectFilled(ImVec2(p.x+13,p.y+14),ImVec2(p.x+23,p.y+24),IM_COL32(41,59,84,255),1);
                draw->AddRect(ImVec2(p.x+13,p.y+14),ImVec2(p.x+23,p.y+24),color,1);
            } else draw->AddRect(ImVec2(p.x+14,p.y+12),ImVec2(p.x+25,p.y+23),color,1);
        }
        if(i==2) {draw->AddLine(ImVec2(p.x+14,p.y+12),ImVec2(p.x+26,p.y+24),color,1.3f);draw->AddLine(ImVec2(p.x+26,p.y+12),ImVec2(p.x+14,p.y+24),color,1.3f);}
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",i==0?"Свернуть":i==1?(glfwGetWindowAttrib(window,GLFW_MAXIMIZED)?"Восстановить":"Развернуть"):"Закрыть");
        if(i==2) ImGui::PopStyleColor();
        if(clicked) {
            if(i==0) glfwIconifyWindow(window);
            else if(i==1) {if(glfwGetWindowAttrib(window,GLFW_MAXIMIZED)) glfwRestoreWindow(window);else glfwMaximizeWindow(window);}
            else glfwSetWindowShouldClose(window,GLFW_TRUE);
        }
        ImGui::PopID();
    }
    ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
}
#endif

static bool FolderButton() {
    const ImVec2 size(242,38);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.52f,.73f,.90f,.075f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.55f,.78f,.96f,.17f));
    bool clicked=ImGui::Button("##result-folder",size);
    ImVec2 p=ImGui::GetItemRectMin();
    auto* draw=ImGui::GetWindowDrawList();
    ImU32 color=IM_COL32(200,229,249,225);
    draw->AddRect(p,ImVec2(p.x+size.x,p.y+size.y),IM_COL32(184,219,247,25),10);
    ImVec2 folder[]={{p.x+14,p.y+13},{p.x+20,p.y+13},{p.x+23,p.y+16},{p.x+31,p.y+16},{p.x+31,p.y+26},{p.x+14,p.y+26}};
    draw->AddPolyline(folder,6,color,ImDrawFlags_Closed,1.2f);
    draw->AddText(ImVec2(p.x+43,p.y+(size.y-ImGui::GetFontSize())*.5f),color,"Папка результата");
    draw->AddLine(ImVec2(p.x+216,p.y+22),ImVec2(p.x+224,p.y+14),color,1.2f);
    draw->AddLine(ImVec2(p.x+218,p.y+14),ImVec2(p.x+224,p.y+14),color,1.2f);
    draw->AddLine(ImVec2(p.x+224,p.y+14),ImVec2(p.x+224,p.y+20),color,1.2f);
    ImGui::PopStyleColor(2);
    return clicked;
}

// A small, heavily blurred texture keeps the background soft at any window size.
// Album colours diffuse through the glass; no large image assets are needed.
static GLuint GlassBackground(const AudioInfo& info) {
    constexpr int size = 96;
    std::vector<float> pixels(size*size*3), temp(pixels.size());
    for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
        float nx=x/95.f, ny=y/95.f;
        float blue=std::exp(-((nx-.18f)*(nx-.18f)+(ny-.18f)*(ny-.18f))*5);
        float teal=std::exp(-((nx-.75f)*(nx-.75f)+(ny-.7f)*(ny-.7f))*6);
        float violet=std::exp(-((nx-.85f)*(nx-.85f)+(ny-.12f)*(ny-.12f))*8);
        float rgb[]={8+22*blue+14*teal+35*violet,17+32*blue+48*teal+18*violet,34+59*blue+54*teal+43*violet};
        for(int c=0;c<3;++c) {
            float album=0;
            if(!info.cover_rgba.empty()) {
                int sx=std::min(info.cover_width-1,x*info.cover_width/size),sy=std::min(info.cover_height-1,y*info.cover_height/size);
                album=info.cover_rgba[(sy*info.cover_width+sx)*4+c]*.42f;
            }
            pixels[(y*size+x)*3+c]=rgb[c]+album;
        }
    }
    for(int pass=0;pass<4;++pass) {
        for(int y=0;y<size;++y) for(int x=0;x<size;++x) for(int c=0;c<3;++c) {
            float value=0; for(int d=-6;d<=6;++d) value+=pixels[(y*size+std::clamp(x+d,0,size-1))*3+c];
            temp[(y*size+x)*3+c]=value/13;
        }
        for(int y=0;y<size;++y) for(int x=0;x<size;++x) for(int c=0;c<3;++c) {
            float value=0; for(int d=-6;d<=6;++d) value+=temp[(std::clamp(y+d,0,size-1)*size+x)*3+c];
            pixels[(y*size+x)*3+c]=value/13;
        }
    }
    std::vector<unsigned char> rgba(size*size*4);
    // Reduce the previous transparency (255 - 230) by 20%: alpha 235.
    for(int i=0;i<size*size;++i) { for(int c=0;c<3;++c) rgba[i*4+c]=static_cast<unsigned char>(std::clamp(pixels[i*3+c],0.f,255.f)); rgba[i*4+3]=235; }
    GLuint texture=0; glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    return texture;
}

// Optional render capture for inspecting the actual OpenGL UI, independent of OS capture permissions.
static void SaveFrame(const char* file,int w,int h) {
    std::vector<unsigned char> pixels(w*h*3); glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
    std::ofstream out(Path(file),std::ios::binary); out<<"P6\n"<<w<<' '<<h<<"\n255\n";
    for(int y=h-1;y>=0;--y) out.write(reinterpret_cast<const char*>(pixels.data()+y*w*3),w*3);
}
int main(int argc, char** argv) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
#ifdef _WIN32
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
#endif
    GLFWwindow* window = glfwCreateWindow(1120, 820, "CVRTR — Audio Converter", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwSetWindowSizeLimits(window, 1000, 760, GLFW_DONT_CARE, GLFW_DONT_CARE);
#ifdef _WIN32
    chrome::Install(window);
#endif
    glfwMakeContextCurrent(window); glfwSwapInterval(1); glfwSetDropCallback(window, Drop);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
#ifdef _WIN32
    const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
#else
    const char* font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    ImFont* heading = nullptr;
    ImFontGlyphRangesBuilder glyph_builder;
    glyph_builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    static const ImWchar symbols[] = {0x2000,0x206f,0x2190,0x21ff,0};
    glyph_builder.AddRanges(symbols);
    ImVector<ImWchar> glyph_ranges; glyph_builder.BuildRanges(&glyph_ranges);
    if (fs::exists(font)) {
        io.Fonts->AddFontFromFileTTF(font, 18, nullptr, glyph_ranges.Data);
        heading = io.Fonts->AddFontFromFileTTF(font, 28, nullptr, glyph_ranges.Data);
    }
    ImFont* logo_font = io.Fonts->AddFontDefault();
    Theme(); ImGui_ImplGlfw_InitForOpenGL(window, true); ImGui_ImplOpenGL3_Init("#version 130");
    std::string snapshot, initial_result; int frame_number=0;
    bool sidebar_open=true; float sidebar_fraction=1;
    for(int arg=1;arg<argc;++arg) {
        if(std::string(argv[arg])=="--snapshot" && arg+1<argc) snapshot=argv[++arg];
        else if(std::string(argv[arg])=="--result" && arg+1<argc) initial_result=argv[++arg];
        else if(std::string(argv[arg])=="--collapsed") {sidebar_open=false;sidebar_fraction=0;}
        else {SetPath(input_path,sizeof(input_path),argv[arg]);InputChanged();}
    }
    const char* logo = "   ____  __   ______ _____ ____  \n  / ___\\|  | / /  _ \\_   _|  _ \\ \n | |    |  |/ /| |_) || | | |_) |\n | |___ |    / |  _ < | | |  _ < \n  \\____||___/  |_| \\_\\|_| |_| \\_\\";
    const int bitrates[] = {128,192,256,320}; const char* qualities[] = {"128 кбит/с", "192 кбит/с", "256 кбит/с", "320 кбит/с"};
    int quality = 1; bool inspect_result = false, details = false, about_requested=false;
    std::atomic<float> progress{0}; std::future<Result> job;
    Result result{true, "Выберите аудиофайл или перетащите его в окно.", ""};
    if(!initial_result.empty()) { result={true,"Открыт сохранённый результат.",initial_result};inspect_result=true;source_changed=false; }
    AudioInfo info; std::string inspected, info_error;
    std::string validated_path,source_error;AudioFormat validated_format=AudioFormat::Unknown;
    GLuint cover = 0, background = GlassBackground(info);
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if(source_changed) {inspect_result=false;source_changed=false;}
        if(validated_path!=input_path || validated_format!=source_format) {
            validated_path=input_path;validated_format=source_format;source_error.clear();
            if(input_path[0]) ValidateInputFormat(input_path,source_format,source_error);
        }
        if (busy && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            result = job.get(); busy = false; if (result.success) { inspect_result = true; inspected.clear(); }
        }
        std::string preview = inspect_result && !result.path.empty() ? result.path : input_path;
        if (preview != inspected) {
            inspected = preview;
            if (cover) { glDeleteTextures(1, &cover); cover = 0; }
            info = AudioInfo{}; info_error.clear();
            if (!preview.empty() && ReadAudioInfo(preview, info, info_error) && !info.cover_rgba.empty()) {
                glGenTextures(1, &cover); glBindTexture(GL_TEXTURE_2D, cover);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,info.cover_width,info.cover_height,0,GL_RGBA,GL_UNSIGNED_BYTE,info.cover_rgba.data());
            }
            glDeleteTextures(1,&background); background=GlassBackground(info);
        }
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
        ImGui::Begin("CVRTR", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        auto* draw = ImGui::GetWindowDrawList();
        const float target=sidebar_open?1.f:0.f;
        sidebar_fraction+=(target-sidebar_fraction)*(1-std::exp(-12.f*std::min(io.DeltaTime,.1f)));
        if(std::abs(sidebar_fraction-target)<.001f) sidebar_fraction=target;
        const float w=io.DisplaySize.x,h=io.DisplaySize.y,sidebar=190*sidebar_fraction,top=108,footer=108;
        draw->AddImage(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(background)),ImVec2(0,0),ImVec2(w,h));
        draw->AddRectFilledMultiColor(ImVec2(0,0),ImVec2(w,top),IM_COL32(165,204,237,22),IM_COL32(190,220,247,10),IM_COL32(20,40,65,12),IM_COL32(28,49,76,16));
        draw->AddLine(ImVec2(0,top),ImVec2(w,top),IM_COL32(213,234,255,28));
        if(sidebar>0) {
            draw->AddRectFilled(ImVec2(0,top),ImVec2(sidebar,h-footer),IM_COL32(17,32,52,85));
            draw->AddLine(ImVec2(sidebar,top),ImVec2(sidebar,h-footer),IM_COL32(213,234,255,22));
        }
        draw->AddRectFilled(ImVec2(0,h-footer),ImVec2(w,h),IM_COL32(12,24,42,140));
        draw->AddLine(ImVec2(0,h-footer),ImVec2(w,h-footer),IM_COL32(213,234,255,25));
        ImGui::PopStyleVar();
        ImGui::PushFont(logo_font);
        const float logo_scale=1.f;
        ImVec2 logo_size=ImGui::CalcTextSize(logo); logo_size.x*=logo_scale;logo_size.y*=logo_scale;
        const float logo_right=32+logo_size.x;
        const float logo_y=(top-logo_size.y)*.5f;
        ImVec2 logo_pos(32,logo_y);
        draw->AddRectFilled(ImVec2(logo_pos.x-18,logo_y-8),ImVec2(logo_right+18,logo_y+logo_size.y+8),IM_COL32(178,217,247,9),16);
        draw->AddRect(ImVec2(logo_pos.x-18,logo_y-8),ImVec2(logo_right+18,logo_y+logo_size.y+8),IM_COL32(213,238,255,23),16);
        draw->AddText(logo_font,logo_font->FontSize*logo_scale,ImVec2(logo_pos.x,logo_pos.y+1),IM_COL32(149,213,252,32),logo);
        draw->AddText(logo_font,logo_font->FontSize*logo_scale,logo_pos,IM_COL32(218,240,255,205),logo);
        ImGui::PopFont();
#ifdef _WIN32
        WindowControls(window,w);
#endif
        // Keep the reopening control on the workspace edge, outside the header.
        if(!sidebar_open) {
            ImGui::SetCursorPos(ImVec2(sidebar+8,top+6));
            if(SidebarToggle(false)) sidebar_open=true;
        }
        if(sidebar_fraction>0) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,sidebar_fraction);
        ImGui::SetCursorPos(ImVec2(sidebar-190+16,top+24));
        ImGui::BeginChild("Navigation",ImVec2(158,h-top-footer-36),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("КОНВЕРТАЦИЯ");
        if(sidebar_open) {
            ImGui::SameLine(130);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY()+4);
            if(SidebarToggle(true)) sidebar_open=false;
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(busy);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(6,8));
        if(FormatDropdown("##source-format",source_format,false)) {copy_mode=false;SuggestOutput();}
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Исходный формат: определяется по файлу, можно выбрать вручную");
        ImGui::SameLine(0,4);ImGui::TextUnformatted("→");ImGui::SameLine(0,4);
        ImGui::BeginDisabled(copy_mode);
        AudioFormat displayed_target=copy_mode?source_format:target_format;
        if(FormatDropdown("##target-format",displayed_target,true)) {target_format=displayed_target;SuggestOutput();}
        ImGui::EndDisabled();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Выходной формат");
        ImGui::PopStyleVar();
        if(NavigationButton("Копировать файл",copy_mode)) {copy_mode=!copy_mode;SuggestOutput();}
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0,28)); ImGui::TextDisabled("ПРОСМОТР"); ImGui::Spacing();
        if (NavigationButton("Исходный файл",!inspect_result)) inspect_result=false;
        ImGui::BeginDisabled(result.path.empty());
        if (NavigationButton("Результат",inspect_result)) inspect_result=true;
        ImGui::EndDisabled();
        ImGui::EndChild(); ImGui::PopStyleVar();
        }
        const float workspace_left=sidebar+26+16*(1-sidebar_fraction);
        ImGui::SetCursorPos(ImVec2(workspace_left,top+22));
        ImGui::BeginChild("Workspace",ImVec2(w-workspace_left-26,h-top-footer-36));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(12,5));
        ImGui::BeginChild("Album",ImVec2(0,184));
        const float art_size=160;
        if (cover) {
            ImVec2 origin=ImGui::GetCursorScreenPos();
            float scale=std::min(art_size/info.cover_width,art_size/info.cover_height);
            float cw=info.cover_width*scale,ch=info.cover_height*scale;
            ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(cover)),ImVec2(origin.x+(art_size-cw)/2,origin.y+(art_size-ch)/2),ImVec2(origin.x+(art_size+cw)/2,origin.y+(art_size+ch)/2));
            ImGui::Dummy(ImVec2(art_size,art_size));
        } else {
            ImVec2 p=ImGui::GetCursorScreenPos(); auto* d=ImGui::GetWindowDrawList();
            d->AddRectFilled(p,ImVec2(p.x+art_size,p.y+art_size),IM_COL32(41,49,77,255));
            d->AddCircle(ImVec2(p.x+80,p.y+80),48,IM_COL32(112,133,201,180),64,2);
            d->AddCircleFilled(ImVec2(p.x+80,p.y+80),9,IM_COL32(112,133,201,180));
            ImGui::Dummy(ImVec2(art_size,art_size));
        }
        ImGui::SameLine(0,24); ImGui::BeginGroup();
        ImGui::TextDisabled("%s",inspect_result ? "РЕЗУЛЬТАТ" : "ИСХОДНЫЙ ФАЙЛ");
        std::string title=Tag(info,"title",preview.empty()?"Добавьте вашу музыку":Path(preview).stem().u8string());
        if(heading) ImGui::PushFont(heading); ImGui::TextWrapped("%s",title.c_str()); if(heading) ImGui::PopFont();
        ImGui::TextWrapped("%s",Tag(info,"artist",preview.empty()?"Перетащите файл в окно или выберите его ниже.":"Исполнитель не указан").c_str());
        ImGui::TextDisabled("%s",Tag(info,"album","Альбом не указан").c_str());
        if(info.sample_rate) ImGui::Text("%s   /   %.1f кГц   /   %d кан.   /   %d:%02d",info.codec.c_str(),info.sample_rate/1000.f,info.channels,static_cast<int>(info.duration)/60,static_cast<int>(info.duration)%60);
        ImGui::TextDisabled("Тегов: %d   /   Обложка: %s",static_cast<int>(info.tags.size()),cover?"есть":"нет");
        ImGui::EndGroup(); ImGui::EndChild(); ImGui::PopStyleVar();
        ImGui::Separator(); ImGui::Spacing();
        ImGui::BeginDisabled(busy);
        ImGui::TextDisabled("ИСХОДНЫЙ ФАЙЛ");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-132);
        if(ImGui::InputText("##input",input_path,sizeof(input_path))) { InputChanged(); inspect_result=false; }
        ImGui::SameLine();
        if(ImGui::Button("Выбрать...",ImVec2(120,0))) {
            auto selection=pfd::open_file("Выберите аудио",".",{"Аудиофайлы","*.mp3 *.wav *.flac *.ogg *.m4a","Все файлы","*"}).result();
            if(!selection.empty()) { SetPath(input_path,sizeof(input_path),selection[0]); InputChanged(); inspect_result=false; }
        }
        ImGui::TextDisabled("СОХРАНИТЬ КАК");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-132); ImGui::InputText("##output",output_path,sizeof(output_path)); ImGui::SameLine();
        if(ImGui::Button("Обзор...",ImVec2(120,0))) {
            auto destination=pfd::save_file("Сохранить результат",output_path,{"Аудиофайлы","*.mp3 *.wav *.flac *.ogg *.m4a"}).result();
            if(!destination.empty()) SetPath(output_path,sizeof(output_path),destination);
        }
        ImGui::EndDisabled();
        if(ImGui::BeginTable("Settings",3,ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn(); ImGui::TextDisabled("ПРЕОБРАЗОВАНИЕ");
            if(copy_mode) ImGui::TextUnformatted("Копирование");else ImGui::Text("%s → %s",FormatName(source_format),FormatName(target_format));
            ImGui::TableNextColumn(); ImGui::TextDisabled("КАЧЕСТВО");
            if(!copy_mode && (target_format==AudioFormat::Mp3 || target_format==AudioFormat::Ogg || target_format==AudioFormat::M4a)) { ImGui::BeginDisabled(busy); ImGui::SetNextItemWidth(-1); ImGui::Combo("##quality",&quality,qualities,4); ImGui::EndDisabled(); }
            else ImGui::TextUnformatted(copy_mode?"Без перекодирования":target_format==AudioFormat::Flac?"FLAC / без потерь":"PCM / 16 бит");
            ImGui::TableNextColumn(); ImGui::TextDisabled("МЕТАДАННЫЕ"); ImGui::TextUnformatted("Теги и обложка"); ImGui::TextDisabled("Сохраняются в файле");
            ImGui::EndTable();
        }
        ImGui::Spacing(); ImGui::Separator();
        if(ImGui::Selectable(details?"Скрыть метаданные":"Показать все метаданные",details)) details=!details;
        if(details && ImGui::BeginTable("Tags",2,ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Поле",ImGuiTableColumnFlags_WidthFixed,160); ImGui::TableSetupColumn("Значение");
            for(const auto& tag:info.tags) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s",tag.first.empty()?"(без имени)":tag.first.c_str()); ImGui::TableNextColumn(); ImGui::TextWrapped("%s",tag.second.c_str()); }
            ImGui::EndTable();
        }
        if(!info_error.empty()) ImGui::TextWrapped("Не удалось прочитать файл: %s",info_error.c_str());
        if(!source_error.empty()) {ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.57f,.56f,1));ImGui::TextWrapped("%s",source_error.c_str());ImGui::PopStyleColor();}
        if(inspect_result && info.codec=="pcm_s16le") ImGui::TextDisabled("WAV: отображение ID3-тегов в других программах зависит от их поддержки.");
        ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(24,h-footer+18));
        ImGui::BeginChild("Status",ImVec2(w-280,footer-24));
        ImGui::TextColored(result.success?ImVec4(.73f,.80f,.98f,1):ImVec4(1,.57f,.56f,1),"%s",busy?"Обработка аудио...":result.text.c_str());
        if(busy) ImGui::ProgressBar(progress.load(),ImVec2(-1,6),"");
        else if(!result.path.empty() && FolderButton()) OpenFolder(result.path);
        if(!busy) {
            if(!result.path.empty()) ImGui::SameLine();
            if(ImGui::SmallButton("О программе")) about_requested=true;
        }
        ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(w-230,h-footer+28)); ImGui::BeginDisabled(busy);
        if(ImGui::Button("Конвертировать",ImVec2(206,46))) {
            try {
                auto in=Path(input_path),out=Path(output_path); std::error_code ec;
                std::string extension=out.extension().u8string();
                std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                if(!input_path[0] || !fs::is_regular_file(in)) throw std::runtime_error("Выберите исходный аудиофайл.");
                if(!output_path[0]) throw std::runtime_error("Укажите путь для сохранения.");
                if(fs::equivalent(in,out,ec) || fs::weakly_canonical(in)==fs::weakly_canonical(out)) throw std::runtime_error("Исходный и выходной файл совпадают.");
                std::string error;
                if(!ValidateInputFormat(input_path,source_format,error)) throw std::runtime_error(error);
                if(!copy_mode && !CanEncode(target_format)) throw std::runtime_error("Кодировщик отсутствует в этой сборке FFmpeg.");
                if((!copy_mode && extension!=FormatExtension(target_format)) || (copy_mode && FormatFromPath(output_path)!=source_format)) throw std::runtime_error("Расширение результата не соответствует выбранному формату.");
                bool replace=fs::exists(out);
                if(replace && !fs::is_regular_file(out)) throw std::runtime_error("Путь результата не является файлом.");
                if(!replace || pfd::message("Заменить файл?","Файл уже существует. Заменить его?",pfd::choice::yes_no,pfd::icon::warning).result()==pfd::button::yes) {
                    progress=0; job=std::async(std::launch::async,Process,std::string(input_path),std::string(output_path),source_format,target_format,copy_mode,bitrates[quality],replace,std::ref(progress)); busy=true;
                }
            } catch(const std::exception& e) { result.success=false; result.text=e.what(); }
        }
        ImGui::EndDisabled();
        if(about_requested) {ImGui::OpenPopup("О программе CVRTR");about_requested=false;}
        if(ImGui::BeginPopupModal("О программе CVRTR",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("CVRTR %s",CVRTR_VERSION);
            ImGui::TextUnformatted("Copyright (C) 2026 zVape6 and CVRTR contributors.");
            ImGui::Spacing();
            ImGui::TextUnformatted("GNU GPL v3 (GPL-3.0-only)");
            ImGui::TextUnformatted("Вы можете изменять и распространять программу\nна условиях GPL v3. Программа предоставляется без гарантий.");
            ImGui::TextUnformatted("Полный текст лицензии: LICENSE.\nСведения о сторонних компонентах: THIRD_PARTY.md.");
            ImGui::Spacing();if(ImGui::Button("Закрыть",ImVec2(120,0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::End();
        ImGui::Render(); int fw,fh; glfwGetFramebufferSize(window,&fw,&fh); glViewport(0,0,fw,fh);
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if(!snapshot.empty() && ++frame_number==24) { SaveFrame(snapshot.c_str(),fw,fh); glfwSetWindowShouldClose(window,GLFW_TRUE); }
        glfwSwapBuffers(window);
    }
    if(job.valid()) job.wait(); if(cover) glDeleteTextures(1,&cover); glDeleteTextures(1,&background);
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext(); glfwDestroyWindow(window); glfwTerminate();
    return 0;
}
