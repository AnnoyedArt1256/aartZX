// Dear ImGui: standalone example application for SDL2 + OpenGL
// (SDL is a cross-platform general purpose library for handling windows, inputs, OpenGL/Vulkan/Metal graphics context creation, etc.)

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#define GL_GLEXT_PROTOTYPES
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_memory_editor.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "portable-file-dialogs.h"
#include <stdio.h>
#include <stdbool.h>
#include <iostream>
#include <fstream>
#include <SDL.h>

#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL_opengles2.h>
// TODO: add glext include here
#else
#include <SDL_opengl.h>
#endif

// This example can also compile and run with Emscripten! See 'Makefile.emscripten' for details.
#ifdef __EMSCRIPTEN__
#include "../libs/emscripten/emscripten_mainloop_stub.h"
#endif

#define rightClickable if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::SetKeyboardFocusHere(-1);

SDL_Renderer* renderer;

bool emu_window_focused;

#include "config.h"

// from zx.c
extern "C" {
    extern void init_zx(int argc, char *argv[], bool init_files);
    extern void main_zx();
    extern void deinit_zx(bool deinit_file);
    extern bool has_ULA_quit();
    // for mouse controls and such
    void EMU_IMGUI_process_event(SDL_Event *event) {
        ImGui_ImplSDL2_ProcessEvent(event);
    }
    extern uint32_t *pixels;
    SDL_Renderer* get_SDL_renderer() {
        return renderer;
    }
    bool EMU_IMGUI_is_emu_focused() {
        return emu_window_focused;
    }
    extern bool audio_paused;
    extern bool paused;
    extern void reset_audio_buffer_and_unpause();
    extern float audio_volume;
    extern void load_file(const char *file);
    extern uint8_t mem[131072];
    extern uint32_t *mem_highlight;
    extern uint8_t *zx_rom;
    extern uint32_t *zx_rom_highlight;
    extern uint8_t get_ram_bank();
    extern uint8_t get_scr_rom_bank(); // the banks are combined in a bit-field
    extern uint8_t readZ80(uint16_t addr);
    extern uint8_t readZ80_no_highlight(uint16_t addr);
    extern void writeZ80(uint16_t addr, uint8_t val);
    extern void set_YM(bool is_ym, int chip);
    extern void AY_set_pan(int pan_type); // for stereo/mono
    extern void AY_set_clock(bool clock_mode); // for TSFM vs 128k AY clock speeds
    extern uint32_t cur_mempos_rewind;
    extern bool rewind_pressed;
    extern bool actually_rewind;
    struct regs_Struct {
        uint8_t a,b,c,d,e,f,h,l;
        uint8_t as,bs,cs,ds,es,fs,hs,ls;
        uint16_t ix,iy;
        uint8_t iff1,iff2,i,r;
        int16_t has_int;
        uint8_t im;
        uint16_t pc,sp;
        SHARED_BOOL halt;
        int cycles;
    };
    
    struct flags_Struct {
        SHARED_BOOL s,z,x,h,y,p,n,c;
    };

    extern struct regs_Struct regs;
    extern struct flags_Struct flags;

    extern uint16_t read_AF();

    extern int get_vcount();
    extern int get_hcount();
    extern uint8_t *event_viewer;
    extern uint32_t get_contended_cycles();
}

bool LoadTextureFromMemory(GLuint* out_texture) {
    // Load from file
    int image_width = 320;
    int image_height = 256;

    // Create a OpenGL texture identifier
    GLuint image_texture;
    glGenTextures(1, &image_texture);
    glBindTexture(GL_TEXTURE_2D, image_texture);

    // Setup filtering parameters for display
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Upload pixels into texture
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_width, image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    *out_texture = image_texture;

    return true;
}

void save_settings();
void load_settings();

extern "C" {
    struct window_bool {
        SHARED_BOOL memviewer;
        SHARED_BOOL imgui_debugger;
        SHARED_BOOL settings;
        SHARED_BOOL read;
        SHARED_BOOL write;
        SHARED_BOOL exec;
        uint32_t read_col;
        uint32_t write_col;
        uint32_t exec_col;
        SHARED_BOOL is_ym1;
        SHARED_BOOL is_ym2;
        uint8_t aypan;
        int AY_seperation;
        SHARED_BOOL cpu_regview;
        SHARED_BOOL contended;
        SHARED_BOOL disasm;
        float screen_scale;
        SHARED_BOOL do_event_viewer;
        SHARED_BOOL do_event_viewer_bitmap;
        SHARED_BOOL do_tsfm;
    };
    struct window_bool visible_windows; // misc.h
}

static bool opt_padding = false; // Is there padding (a blank space) between the window edge and the Dockspace?

void ShowExampleAppDockSpace(bool* p_open)
{
    // Variables to configure the Dockspace example.
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None; // Config flags for the Dockspace

    // In this example, we're embedding the Dockspace into an invisible parent window to make it more configurable.
    // We set ImGuiWindowFlags_NoDocking to make sure the parent isn't dockable into because this is handled by the Dockspace.
    //
    // ImGuiWindowFlags_MenuBar is to show a menu bar with config options. This isn't necessary to the functionality of a
    // Dockspace, but it is here to provide a way to change the configuration flags interactively.
    // You can remove the MenuBar flag if you don't want it in your app, but also remember to remove the code which actually
    // renders the menu bar, found at the end of this function.
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;

    // Is the example in Fullscreen mode?
    {
        // If so, get the main viewport:
        const ImGuiViewport* viewport = ImGui::GetMainViewport();

        // Set the parent window's position, size, and viewport to match that of the main viewport. This is so the parent window
        // completely covers the main viewport, giving it a "full-screen" feel.
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        // Set the parent window's styles to match that of the main viewport:
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f); // No corner rounding on the window
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); // No border around the window

        // Manipulate the window flags to make it inaccessible to the user (no titlebar, resize/move, or navigation)
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    }

    // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
    // and handle the pass-thru hole, so the parent window should not have its own background:
    if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
        window_flags |= ImGuiWindowFlags_NoBackground;

    // If the padding option is disabled, set the parent window's padding size to 0 to effectively hide said padding.
    if (!opt_padding)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
    // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
    // all active windows docked into it will lose their parent and become undocked.
    // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
    // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
    ImGui::Begin("DockSpace Demo", p_open, window_flags);

    // Remove the padding configuration - we pushed it, now we pop it:
    if (!opt_padding)
        ImGui::PopStyleVar();

    // Pop the two style rules set in Fullscreen mode - the corner rounding and the border size.
    ImGui::PopStyleVar(2);

    // Check if Docking is enabled:
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
    {
        // If it is, draw the Dockspace with the DockSpace() function.
        // The GetID() function is to give a unique identifier to the Dockspace - here, it's "MyDockSpace".
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }
    else
    {
        // Docking is DISABLED - Show a warning message
    }

    // This is to show the menu bar that will change the config settings at runtime.
    // If you copied this demo function into your own code and removed ImGuiWindowFlags_MenuBar at the top of the function,
    // you should remove the below if-statement as well.
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("file")) {
            // Disabling fullscreen would allow the window to be moved to the front of other windows,
            // which we can't undo at the moment without finer window depth/z control.
            if (ImGui::MenuItem("open file")) {
                audio_paused = true;
                auto f = pfd::open_file("Choose a file to read", pfd::path::home(),
                            { "Tape Files (.tap)", "*.tap", "Snapshow Files (.sna)", "*.sna" },
                            pfd::opt::none).result();
                if (!f.empty()) {
                    load_file((const char *)f[0].c_str());
                    audio_paused = false;
                    reset_audio_buffer_and_unpause();
                } else {
                    audio_paused = false;
                    reset_audio_buffer_and_unpause();
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("emulation")) {
            // Disabling fullscreen would allow the window to be moved to the front of other windows,
            // which we can't undo at the moment without finer window depth/z control.
            if (ImGui::MenuItem("reset")) {
                deinit_zx(false);
                init_zx(0,NULL,false);
            }
            
            if (ImGui::MenuItem("reset sync (audio)")) reset_audio_buffer_and_unpause();

            if (ImGui::MenuItem("pause","Escape")) {
                paused = !paused; 
                audio_paused = paused;
                if (!paused) {
                    reset_audio_buffer_and_unpause();
                }
            }

            ImGui::EndMenu();
        }

        ImGui::MenuItem("settings", NULL, (bool *)&visible_windows.settings);

        if (ImGui::BeginMenu("windows")) {
            // Disabling fullscreen would allow the window to be moved to the front of other windows,
            // which we can't undo at the moment without finer window depth/z control.
            ImGui::MenuItem("Memory Viewer", NULL, (bool *)&visible_windows.memviewer);
            ImGui::MenuItem("ImGui Debugger", NULL, (bool *)&visible_windows.imgui_debugger);
            ImGui::MenuItem("CPU Register View", NULL, (bool *)&visible_windows.cpu_regview);
            ImGui::MenuItem("Z80 Debugger", NULL, (bool *)&visible_windows.disasm);

            ImGui::EndMenu();
        }


        ImGui::EndMenuBar();
    }

    // End the parent window that contains the Dockspace:
    ImGui::End();
}

const char* memory_types[] = {"CPU Memory", "Boot ROM", "Work RAM"};
int selected_memory_type;
static MemoryEditor mem_edit;


// save settings
void save_settings() {
    ImGuiIO& io = ImGui::GetIO();
    std::string filename = "aartZX.cfg";
    std::ofstream f(filename);
    if (!f) return;
    std::string value;
    f << audio_volume << "\n";
    f << io.FontGlobalScale << "\n";
    f << selected_memory_type << "\n";
    for (int i = 0; i < sizeof(mem_edit); i++) {
        f << (int)(((uint8_t*)&mem_edit)[i]) << "\n";
    }
    for (int i = 0; i < sizeof(visible_windows); i++) {
        f << (int)(((uint8_t*)&visible_windows)[i]) << "\n";
    }
}

// load settings
void load_settings() {
    ImGuiIO& io = ImGui::GetIO();
    std::string filename = "aartZX.cfg";
    std::ifstream f(filename);
    if (!f) return;
    std::string value;
    getline(f,value); if (!value.empty()) audio_volume = std::stof(value);
    getline(f,value); if (!value.empty()) io.FontGlobalScale = std::stof(value);
    getline(f,value); if (!value.empty()) selected_memory_type = std::stoi(value)%IM_ARRAYSIZE(memory_types);
    for (int i = 0; i < sizeof(mem_edit); i++) {
        getline(f,value); if (!value.empty()) ((uint8_t*)&mem_edit)[i] = std::stoi(value);
    }
    for (int i = 0; i < sizeof(visible_windows); i++) {
        getline(f,value); if (!value.empty()) ((uint8_t*)&visible_windows)[i] = std::stoi(value);
    }
}

static uint8_t mem_debug_read(const ImU8* mem, size_t off, void* user_data) {
    assert((uint16_t)off == off);
    return readZ80_no_highlight(off);
}

static void mem_debug_write(ImU8* mem, size_t off, ImU8 d, void* user_data) {
    assert((uint16_t)off == off);
    writeZ80(off,d);
}

static uint32_t mem_debug_highlight_cpu(const ImU8* mem, size_t off, void* user_data) {
    assert((uint16_t)off == off);
    switch (off & 0xC000) {
        case 0x0000: // ROM (not contended, obviously)
            return zx_rom_highlight[off|(((get_scr_rom_bank()>>1)&1)<<14)];

        case 0x4000:  // RAM bank 5 (contended)
            return mem_highlight[(off&0x3fff)|(5<<14)];

        case 0x8000: // RAM bank 2 (not contended)
            return mem_highlight[(off&0x3fff)|(2<<14)];

        case 0xC000:  // RAM bank X (*COULD* be contended based off current RAM bank)
            return mem_highlight[(off&0x3fff)|((get_ram_bank()&7)<<14)];
    }
}

static uint32_t mem_debug_highlight_mem(const ImU8* mem, size_t off, void* user_data) {
    return mem_highlight[off&0x1fffff];
}

static uint32_t mem_debug_highlight_zxrom(const ImU8* mem, size_t off, void* user_data) {
    return zx_rom_highlight[off&0x3fff];
}

// fades markers used in the memory debugger
void do_mem_fade() {
    for (int i = 0; i < 32768; i++) {
        if ((zx_rom_highlight[i]&0xFF000000) >= 0x12000000) zx_rom_highlight[i] -= 0x12000000;
        else if ((zx_rom_highlight[i]&0xFF000000) > 0) zx_rom_highlight[i] &= 0xFFFFFF;
    }
    for (int i = 0; i < 131072; i++) {
        if ((mem_highlight[i]&0xFF000000) >= 0x12000000) mem_highlight[i] -= 0x12000000;
        else if ((mem_highlight[i]&0xFF000000) > 0 ) mem_highlight[i] &= 0xFFFFFF;
    }
}

extern void do_disasm(bool *p_open, bool *enable_event, bool *enable_event_bitmap);

// Main code
int main(int argc, char *argv[]) {
    
    selected_memory_type = 0;
    visible_windows.do_tsfm = false;
    visible_windows.memviewer = false;
    visible_windows.imgui_debugger = false;
    visible_windows.settings = true;

    visible_windows.read = true;
    visible_windows.write = true;
    visible_windows.exec = true; 
    
    visible_windows.read_col = 0xA00000;
    visible_windows.write_col = 0x0000A0;
    visible_windows.exec_col = 0x006000;

    visible_windows.is_ym1 = false;
    visible_windows.is_ym2 = false;

    visible_windows.aypan = false;
    visible_windows.AY_seperation = 70; // 70%

    visible_windows.cpu_regview = false;
    visible_windows.contended = true;
    visible_windows.disasm = false;

    visible_windows.screen_scale = 0.94f;
    visible_windows.do_event_viewer = false;
    visible_windows.do_event_viewer_bitmap = false;

    audio_volume = 1;

    init_zx(argc, argv, true);

    // Setup SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100 (WebGL 1.0)
    const char* glsl_version = "#version 100";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
    // GL ES 3.0 + GLSL 300 es (WebGL 2.0)
    const char* glsl_version = "#version 300 es";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    // GL 3.2 Core + GLSL 150
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // From 2.0.18: Enable native IME.
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI| SDL_WINDOW_MAXIMIZED);
    SDL_Window* window = SDL_CreateWindow("aartZX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return -1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (gl_context == nullptr)
    {
        printf("Error: SDL_GL_CreateContext(): %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("IBMPlexSans.ttf", 25.0f); // FOINISS FONT OH MAI GAHHHH

    io.FontGlobalScale = 1.0f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Our state
    ImVec4 clear_color = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);

    load_settings();
    save_settings(); // for updating config files from older versions

    #ifdef AY_EMULATION
        AY_set_pan(visible_windows.aypan);
        #ifdef AY_TURBOSOUND_FM
            AY_set_clock(visible_windows.do_tsfm);
        #endif
    #endif

    GLuint my_image_texture = 0;
    memset(pixels,0xff,sizeof(uint32_t)*320*256);
    LoadTextureFromMemory(&my_image_texture);

    // Main loop
    bool done = false;
    emu_window_focused = false;
#ifdef __EMSCRIPTEN__
    // For an Emscripten build we are disabling file-system access, so let's not attempt to do a fopen() of the imgui.ini file.
    // You may manually call LoadIniSettingsFromMemory() to load settings from your own storage.
    io.IniFilename = nullptr;
    EMSCRIPTEN_MAINLOOP_BEGIN
#else
    // Main loop
    int cur_frame = 0;
    while (!has_ULA_quit()) {
#endif
        main_zx();
        if (!paused) do_mem_fade();

        cur_frame++;
        if (cur_frame == (50*60)) {
            cur_frame = 0;
            save_settings();
        }

        glBindTexture(GL_TEXTURE_2D, my_image_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 256, GL_BGRA, GL_UNSIGNED_BYTE, pixels);

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();


        ShowExampleAppDockSpace((bool*)false);

        if (visible_windows.settings) {
            // Settings
            ImGui::Begin("Settings",(bool *)&visible_windows.settings);
            ImGui::SliderFloat("UI Scaling Factor", &io.FontGlobalScale, 0.5f, 3.0f); rightClickable
            ImGui::SliderFloat("Emulator Window Scaline Factor", &visible_windows.screen_scale, 0.1f, 3.0f); rightClickable
            ImGui::SliderFloat("Master Audio Volume", &audio_volume, 0.0f, 3.0f); rightClickable
            ImGui::Separator();

            #ifdef AY_EMULATION
                if (ImGui::Button(visible_windows.is_ym1?"Chip 1: YM-type":"Chip 1: AY-type")) {
                    visible_windows.is_ym1 ^= 1;
                    set_YM((bool)visible_windows.is_ym1,0);
                }
                #ifdef AY_TURBOSOUND
                    ImGui::SameLine();
                    if (ImGui::Button(visible_windows.is_ym2?"Chip 2: YM-type":"Chip 2: AY-type")) {
                        visible_windows.is_ym2 ^= 1;
                        set_YM((bool)visible_windows.is_ym2,0);
                    }
                #endif
            #endif

            #ifdef AY_EMULATION
                ImGui::Text("AY Stereo Config:");
                ImGui::SameLine();
                const char *pan_str[] = {"Mono", "ABC Stereo", "ACB Stereo"};
                if (ImGui::Button(pan_str[visible_windows.aypan])) {
                    visible_windows.aypan = (visible_windows.aypan+1)%3;
                    AY_set_pan(visible_windows.aypan);
                }

                bool stereo_sep_changed = ImGui::SliderInt("AY Stereo Seperation", &visible_windows.AY_seperation, 0, 100); rightClickable
                if (stereo_sep_changed) {
                    AY_set_pan(visible_windows.aypan);
                }

                #ifdef AY_TURBOSOUND_FM
                    bool tsfm_changed = ImGui::Checkbox("Enable TurboSound FM (2xYM2203)",(bool *)&visible_windows.do_tsfm);
                    if (tsfm_changed) {
                        // TSFM has a seperate clock crystal running at **exactly** 3.5mHz apparently(?)
                        AY_set_clock(visible_windows.do_tsfm);
                    }
                #endif
            #endif

            ImGui::Separator();

            {
                float col[3]; 
                ImVec4 col_vec = ImGui::ColorConvertU32ToFloat4(visible_windows.read_col);
                col[0] = col_vec.x;
                col[1] = col_vec.y;
                col[2] = col_vec.z;
                ImGui::ColorEdit3("Read Label", (float *)&col);
                visible_windows.read_col = ImGui::ColorConvertFloat4ToU32(ImVec4(col[0],col[1],col[2],1.0f));
            }

            {
                float col[3]; 
                ImVec4 col_vec = ImGui::ColorConvertU32ToFloat4(visible_windows.write_col);
                col[0] = col_vec.x;
                col[1] = col_vec.y;
                col[2] = col_vec.z;
                ImGui::ColorEdit3("Write Label", (float *)&col);
                visible_windows.write_col = ImGui::ColorConvertFloat4ToU32(ImVec4(col[0],col[1],col[2],1.0f));
            }

            {
                float col[3]; 
                ImVec4 col_vec = ImGui::ColorConvertU32ToFloat4(visible_windows.exec_col);
                col[0] = col_vec.x;
                col[1] = col_vec.y;
                col[2] = col_vec.z;
                ImGui::ColorEdit3("Execution Label",(float *)&col);
                visible_windows.exec_col = ImGui::ColorConvertFloat4ToU32(ImVec4(col[0],col[1],col[2],1.0f));
            }

            ImGui::Separator();

            ImGui::Checkbox("Enable contended memory",(bool *)&visible_windows.contended);

            ImGui::Separator();
            ImGui::MenuItem("Padding", NULL, &opt_padding);
            ImGui::Separator();

            if (ImGui::MenuItem("Save Settings")) save_settings();
            if (ImGui::MenuItem("Load Settings")) load_settings();
            ImGui::Separator();

            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::Text("Rewind Buffer Info: %d / %d",cur_mempos_rewind,REWIND_MEM);
            ImGui::NewLine();
            ImGui::Text("%d cycles have been used for memory contention",get_contended_cycles());
            ImGui::Text("%.3f%% of CPU time has been used for memory contention",((double)get_contended_cycles())/(315000000.0/88.0/50.0)*100.0);
            ImGui::End();
        }

        if (visible_windows.imgui_debugger) {
            ImGui::ShowMetricsWindow((bool *)&visible_windows.imgui_debugger);
        }

        // Render Emulator framebuffer
        ImGui::Begin("Emulator Window");

        if (paused) {
            ImGui::Text("Paused");
        } if (rewind_pressed && actually_rewind) {
            ImGui::Text("Rewinding...");
        }

        emu_window_focused = ImGui::IsWindowFocused(); // get if window is focused
        double aspect_ratio = ImGui::GetWindowSize().x / ImGui::GetWindowSize().y;

        ImVec2 res;
        double res_rect;
        if (aspect_ratio < 4.0f/3.0f) {
            double x_res = ImGui::GetWindowSize().x * visible_windows.screen_scale;
            res_rect = x_res/4.0*3.0;
            res = ImVec2(x_res, x_res/4.0*3.0);
        } else {
            double y_res = ImGui::GetWindowSize().y * visible_windows.screen_scale;
            res_rect = y_res/3.0*4.0;
            res = ImVec2(y_res/3.0*4.0, y_res);
        }
        ImVec2 cursor_pos = (ImGui::GetWindowSize() - res) * 0.5f;
        ImGui::SetCursorPos(cursor_pos);
        ImGui::Image((void*)(intptr_t)my_image_texture, res);

        if (visible_windows.do_event_viewer) { // draw rectangles if event viewer is enabled
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 window_pos = ImGui::GetWindowPos();
            double x = (double)(((get_hcount()-112)*2.0f+160.0f)*(res.x/320.0f))+cursor_pos.x+window_pos.x;
            double y = (double)((get_vcount()-31.0f)*(res.y/256.0f))+cursor_pos.y+window_pos.y;
            res_rect /= 144.0f;
            draw_list->AddRectFilled(ImVec2(x-res_rect, y-res_rect), ImVec2(x+res_rect, y+res_rect), IM_COL32(0xFF,0x80,0xFF,0xFF));
            draw_list->AddRect(ImVec2(x-res_rect, y-res_rect), ImVec2(x+res_rect, y+res_rect), IM_COL32(0x80,0x40,0x80,0xFF), 0, 0, res_rect/2.0f);\
            // render I/O writes
            static std::string event_type[] = {"ULA Write","ULA Read","AY Write","AY Read","Paging Write","Bitmap Write","Shadow Bitmap Write"};
            int i = 0;
            for (int yp = 0; yp < 311; yp++) {
                for (int xp = 0; xp < 228; xp++) {
                    uint8_t event = event_viewer[i++];
                    if (event != 0) {
                        uint32_t col_outer = 0;
                        uint32_t col_inner = 0;
                        switch (event-1) {
                            case 0: // ULA write
                                col_outer = IM_COL32(0xFF,0x06,0x0D,0xFF);
                                col_inner = IM_COL32(0xFF>>1,0x06>>1,0x0D>>1,0xFF);
                                break;
                            case 1: // ULA read
                                col_outer = IM_COL32(0xFF,0x74,0x0A,0xFF);
                                col_inner = IM_COL32(0xFF>>1,0x74>>1,0x0A>>1,0xFF);
                                break;
                            case 2: // AY write
                                col_outer = IM_COL32(0x9F,0x93,0xC6,0xFF);
                                col_inner = IM_COL32(0x9F>>1,0x93>>1,0xC6>>1,0xFF);
                                break;
                            case 3: // AY read
                                col_outer = IM_COL32(0xF9,0xFE,0xAC,0xFF);
                                col_inner = IM_COL32(0xF9>>1,0xFE>>1,0xAC>>1,0xFF);
                                break;
                            case 4: // paging write
                                col_outer = IM_COL32(0x00,0x6E,0x6E,0xFF);
                                col_inner = IM_COL32(0x00>>1,0x6E>>1,0x6E>>1,0xFF);
                                break;
                            case 5: // bitmap write
                                col_outer = IM_COL32(0xB4,0x7A,0xDA,0xFF);
                                col_inner = IM_COL32(0xB4>>1,0x7A>>1,0xDA>>1,0xFF);
                                break;
                            case 6: // shadow bitmap write
                                col_outer = IM_COL32(0xC9,0x29,0x29,0xFF);
                                col_inner = IM_COL32(0xC9>>1,0x29>>1,0x29>>1,0xFF);
                                break;
                            default:
                                break;
                        }
                        x = (double)(((xp-112)*2.0f+160.0f)*(res.x/320.0f))+cursor_pos.x+window_pos.x;
                        y = (double)((yp-31)*(res.y/256.0f))+cursor_pos.y+window_pos.y;
                        draw_list->AddRectFilled(ImVec2(x-res_rect, y-res_rect), ImVec2(x+res_rect, y+res_rect), col_outer);
                        draw_list->AddRect(ImVec2(x-res_rect, y-res_rect), ImVec2(x+res_rect, y+res_rect), col_inner, 0, 0, res_rect/2.0f);
                        if (ImGui::IsMouseHoveringRect(ImVec2(x-res_rect, y-res_rect), ImVec2(x+res_rect, y+res_rect))) {
                            ImGui::BeginTooltip();
                            ImGui::Text("Type: %s",event_type[event-1].c_str());
                            ImGui::Separator();
                            ImGui::Text("Scanline: %d",yp);
                            ImGui::Text("Cycle: %d",xp);
                            ImGui::EndTooltip();
                        }
                    }
                }
            }
        }

        ImGui::End();
        
        //ImGui::ShowDemoWindow();

        if (visible_windows.memviewer) {
            ImGui::Begin("Memory Viewer",(bool *)&visible_windows.memviewer);

            ImGui::Checkbox("Reads",(bool *)&visible_windows.read);
            ImGui::SameLine();
            ImGui::Checkbox("Writes",(bool *)&visible_windows.write);
            ImGui::SameLine();
            ImGui::Checkbox("Execution",(bool *)&visible_windows.exec);
            ImGui::SameLine();

            ImGui::SetNextItemWidth(ImGui::GetWindowSize().x * 0.3f);

            const char* memory_type_value = memory_types[selected_memory_type];
            if (ImGui::BeginCombo("Memory Type",memory_type_value)) {
                for (int n = 0; n < IM_ARRAYSIZE(memory_types); n++) {
                    const bool is_selected = (selected_memory_type == n);
                    if (ImGui::Selectable(memory_types[n], is_selected))
                        selected_memory_type = n;

                    // Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            uint8_t misc_bank = get_scr_rom_bank();
            switch (selected_memory_type) {
                case 0: // CPU Memory
                default:
                    mem_edit.ReadFn = &mem_debug_read;
                    mem_edit.WriteFn = &mem_debug_write;
                    mem_edit.BgColorFn = &mem_debug_highlight_cpu;
                    mem_edit.DrawContents(NULL,65536);
                    break;

                case 1: // Boot ROM
                    mem_edit.ReadFn = NULL;
                    mem_edit.WriteFn = NULL;
                    mem_edit.BgColorFn = &mem_debug_highlight_zxrom;
                    mem_edit.DrawContents(zx_rom,32768);
                    break;

                case 2: // Work RAM
                    mem_edit.ReadFn = NULL;
                    mem_edit.WriteFn = NULL;
                    mem_edit.BgColorFn = &mem_debug_highlight_mem;
                    mem_edit.DrawContents(mem,131072);
                    break;
            }

            ImGui::End();
        }

        if (visible_windows.cpu_regview) {
            ImGui::Begin("CPU Register Viewer",(bool *)&visible_windows.cpu_regview);
            // why do i have to do this
            uint8_t one = 1;
            uint16_t one16 = 1;

            #define PRINT_REG(text,id,reg)  \
                ImGui::Text(text); \
                ImGui::SameLine(); \
                ImGui::SetNextItemWidth(ImGui::GetWindowSize().x * 0.25f); \
                ImGui::InputScalar(id,ImGuiDataType_U8,&reg,&one,NULL,"%02x",0);

            #define PRINT_REG_NARROW(text,id,reg)  \
                ImGui::Text(text); \
                ImGui::SameLine(); \
                ImGui::SetNextItemWidth(ImGui::GetWindowSize().x * 0.1f); \
                ImGui::InputScalar(id,ImGuiDataType_U8,&reg,NULL,NULL,"%02x",0);

            #define PRINT_REG16(text,id,reg)  \
                ImGui::Text(text); \
                ImGui::SameLine(); \
                ImGui::SetNextItemWidth(ImGui::GetWindowSize().x * 0.25f); \
                ImGui::InputScalar(id,ImGuiDataType_U16,&reg,&one16,NULL,"%04x",0);

            PRINT_REG("A:","##areg",regs.a);
            ImGui::SameLine();
            uint8_t flag = read_AF()&0xff;
            PRINT_REG("F:","##freg",flag);
            ImGui::SameLine();
            PRINT_REG16("SP:","##spreg",regs.sp);

            PRINT_REG("B:","##breg",regs.b);
            ImGui::SameLine();
            PRINT_REG("C:","##creg",regs.c);
            ImGui::SameLine();
            PRINT_REG16("PC:","##pcreg",regs.pc);

            PRINT_REG("D:","##dreg",regs.d);
            ImGui::SameLine();
            PRINT_REG("E:","##ereg",regs.e);
            ImGui::SameLine();
            PRINT_REG16("IX:","##ixreg",regs.ix);

            PRINT_REG("H:","##hreg",regs.h);
            ImGui::SameLine();
            PRINT_REG("L:","##lreg",regs.l);
            ImGui::SameLine();
            PRINT_REG16("IY:","##iyreg",regs.iy);
            
            ImGui::Separator();
            // flags
            PRINT_REG("F:","##freg2",flag);
            ImGui::Checkbox("Carry",(bool *)&flags.c); ImGui::SameLine();
            ImGui::Checkbox("Negative",(bool *)&flags.n); ImGui::SameLine();
            ImGui::Checkbox("Parity",(bool *)&flags.p); ImGui::SameLine();
            ImGui::Checkbox("Half Carry",(bool *)&flags.h);
            ImGui::Checkbox("F5/X",(bool *)&flags.x); ImGui::SameLine();
            ImGui::Checkbox("Zero",(bool *)&flags.z); ImGui::SameLine();
            ImGui::Checkbox("F3/Y",(bool *)&flags.y); ImGui::SameLine();
            ImGui::Checkbox("Sign",(bool *)&flags.s);

            ImGui::Separator();
            // shadow regs
            PRINT_REG_NARROW("A':","##asreg",regs.as); ImGui::SameLine();
            PRINT_REG_NARROW("F':","##fsreg",regs.fs); ImGui::SameLine();
            PRINT_REG_NARROW("B':","##bsreg",regs.bs); ImGui::SameLine();
            PRINT_REG_NARROW("C':","##csreg",regs.cs);

            PRINT_REG_NARROW("D':","##dsreg",regs.ds); ImGui::SameLine();
            PRINT_REG_NARROW("E':","##esreg",regs.es); ImGui::SameLine();
            PRINT_REG_NARROW("H':","##hsreg",regs.hs); ImGui::SameLine();
            PRINT_REG_NARROW("L':","##lsreg",regs.ls);

            ImGui::Separator();
            // shadow regs
            PRINT_REG_NARROW("I:","##ireg",regs.i); ImGui::SameLine();
            PRINT_REG_NARROW("R:","##rreg",regs.r); ImGui::SameLine();
            PRINT_REG_NARROW("IM:","##imreg",regs.im);
            ImGui::Checkbox("HALT",(bool *)&regs.halt); ImGui::SameLine();
            ImGui::Checkbox("IFF1",(bool *)&regs.iff1); ImGui::SameLine();
            ImGui::Checkbox("IFF2",(bool *)&regs.iff2);

            ImGui::End();
        }

        if (visible_windows.disasm) do_disasm((bool *)&visible_windows.disasm,(bool *)&visible_windows.do_event_viewer,(bool *)&visible_windows.do_event_viewer_bitmap);

        ImGui::Render();

        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_MAINLOOP_END;
#endif

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    return 0;
}
