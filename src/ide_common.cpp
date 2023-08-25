#include "ide_common.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#ifndef ABC_VERSION
#define ABC_VERSION "(unknown version)"
#endif

char const* const abc_version = ABC_VERSION;

std::unique_ptr<absim::arduboy_t> arduboy;
float pixel_ratio;
ImGuiID selected_dockid{};
project_t project;

static ImGuiStyle default_style;
ImGuiID dockspace_id;
ImGuiID dockid_project;

extern unsigned char const ProggyVector[198188];

#include "font_icons.hpp"

project_file_t* project_t::get_file(std::string const& filename)
{
    if(auto it = code_files.find(filename); it != code_files.end())
        return &it->second;
    return nullptr;
}

void frame_logic()
{
    if(ImGui::IsKeyPressed(ImGuiKey_F5, false))
        compile_all();
}

void imgui_content()
{
    using namespace ImGui;

    if(BeginMainMenuBar())
    {
        {
            float w = ImGui::CalcTextSize(abc_version, NULL, true).x;
            w += ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - w);
            ImGui::MenuItem(ABC_VERSION "##version", nullptr, nullptr, false);
        }
        EndMainMenuBar();
    }

    ImGuiViewport* viewport = GetMainViewport();
    //SetNextWindowPos(viewport->WorkPos);
    //SetNextWindowSize(viewport->WorkSize);
    //SetNextWindowViewport(viewport->ID);
    //PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    //PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    //PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    //Begin("##dockspace_window", nullptr,
    //    ImGuiWindowFlags_NoTitleBar |
    //    ImGuiWindowFlags_NoCollapse |
    //    ImGuiWindowFlags_NoResize |
    //    ImGuiWindowFlags_NoMove |
    //    ImGuiWindowFlags_NoDocking |
    //    ImGuiWindowFlags_NoBringToFrontOnFocus |
    //    ImGuiWindowFlags_NoNavFocus |
    //    ImGuiWindowFlags_NoBackground);
    //PopStyleVar(3);
    //ImGuiID dockspace_id = ImGui::GetID("dockspace");
    //DockBuilderGetNode(dockspace_id);
    //DockSpace(dockspace_id, {}, ImGuiDockNodeFlags_PassthruCentralNode);
    //End();

    dockspace_id = DockSpaceOverViewport(viewport);
    {
        ImGuiDockNode* node = ImGui::DockBuilderGetCentralNode(dockspace_id);
        ImGuiWindowClass centralAlways = {};
        centralAlways.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe;
        SetNextWindowClass(&centralAlways);
        SetNextWindowDockID(node->ID, ImGuiCond_Always);
        //PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });
        Begin("Display");
        //PopStyleVar();
        Text("Display: " ICON_FA_HEART);
        End();
    }

    static bool firstinit = false;
    if(!firstinit)
    {
        //ImGuiID tid, tid2;
        //DockBuilderRemoveNodeChildNodes(dockspace_id);
        //DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.20f, &dockid_project, &tid);
        //ImGuiDockNode* project = ImGui::DockBuilderGetNode(dockid_project);
        //project->LocalFlags |=
        //    ImGuiDockNodeFlags_NoTabBar |
        //    ImGuiDockNodeFlags_NoDockingOverMe |
        //    ImGuiDockNodeFlags_NoDockingSplitMe;
        //ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
        //root->LocalFlags |= ImGuiDockNodeFlags_NoDockingSplitMe;

        new_project();
        firstinit = true;
    }

    //ShowDemoWindow();

    SetNextWindowDockID(dockid_project, ImGuiCond_Always);
    if(Begin("Project"))
    {
        TextUnformatted("Project Info Here");
    }
    End();

    for(auto& [k, v] : editors)
    {
        v.update();
    }
    for(auto it = editors.begin(); it != editors.end();)
    {
        if(!it->second.open)
            it = editors.erase(it);
        else
            ++it;
    }
}

bool update_pixel_ratio()
{
    float ratio = platform_pixel_ratio();
    bool changed = (ratio != pixel_ratio);
    pixel_ratio = ratio;
    return changed;
}

void define_font()
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    float font_size = DEFAULT_FONT_SIZE * pixel_ratio;
    cfg.FontDataOwnedByAtlas = false;
    cfg.RasterizerMultiply = 1.5f;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    io.Fonts->Clear();
    io.Fonts->AddFontFromMemoryTTF(
        (void*)ProggyVector, sizeof(ProggyVector), font_size, &cfg);
    //cfg.GlyphMinAdvanceX = font_size;
    cfg.MergeMode = true;
    //cfg.GlyphOffset = { 0, -1 };
    static ImWchar const icon_ranges[] =
    {
        0xf004, 0xf35b,
        0
    };
    io.Fonts->AddFontFromMemoryTTF(
        (void*)fa_regular_400, sizeof(fa_regular_400), font_size, &cfg, icon_ranges);
}

void rebuild_fonts()
{
    platform_destroy_fonts_texture();
    define_font();
    platform_create_fonts_texture();
}

void rescale_style()
{
    auto& style = ImGui::GetStyle();
    style = default_style;
    style.ScaleAllSizes(pixel_ratio);
}

void shutdown()
{
#ifndef ARDENS_NO_DEBUGGER
    ImPlot::DestroyContext();
#endif
}

void init()
{
    printf(
        "ABC IDE " ABC_VERSION " by Peter Brown\n");

#ifdef __EMSCRIPTEN__
    EM_ASM(
        FS.mkdir('/offline');
    FS.mount(IDBFS, {}, '/offline');
    FS.syncfs(true, function(err) { ccall('postsyncfs', 'v'); });
    );
#endif

    arduboy = std::make_unique<absim::arduboy_t>();

    //init_settings();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
#if defined(__EMSCRIPTEN__)
    //io.IniFilename = nullptr;
#else
    //ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    //settings_loaded = true;
#endif

    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;

    default_style = style;

    arduboy->fx.erase_all_data();
    arduboy->reset();
    arduboy->fx.min_page = 0xffff;
    arduboy->fx.max_page = 0xffff;
}
