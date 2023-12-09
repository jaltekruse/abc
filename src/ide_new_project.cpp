#include "ide_common.hpp"

#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h>

std::string const main_name = "main.abc";
static std::string const main_prog = R"(int x;
int y;

constexpr sprites MY_SPRITE = sprites{
    8x8
    --XXXX--
    -X....X-
    X......X
    X......X
    X......X
    X......X
    -X....X-
    --XXXX--
};

constexpr sprites DIGITS = sprites{
    3x5
    .X.  .X.  XX.  XX.  X.X  XXX  .X.  XXX  .X.  .X.
    X.X  XX.  ..X  ..X  X.X  X..  X..  ..X  X.X  X.X
    X.X  .X.  .X.  .X.  XXX  XX.  XX.  ..X  .X.  .XX
    X.X  .X.  X..  ..X  ..X  ..X  X.X  .X.  X.X  ..X
    .X.  XXX  XXX  XX.  ..X  XX.  .X.  .X.  .X.  .X.
};

void draw_num(i16 x, i16 y, u8 n)
{
    if(n < 10)
        $draw_sprite(x, y, DIGITS, n);
    else if(n < 100)
    {
        $draw_sprite(x, y, DIGITS, n / 10);
        $draw_sprite(x + 4, y, DIGITS, n % 10);
    }
    else
    {
        if(n < 200)
            $draw_sprite(x, y, DIGITS, 1);
        else
            $draw_sprite(x, y, DIGITS, 2);
        n = n % 100;
        $draw_sprite(x + 4, y, DIGITS, n / 10);
        $draw_sprite(x + 8, y, DIGITS, n % 10);
    }
}

constexpr int MOVE_SPEED = 1;

void setup()
{
    x = 56;
    y = 24;
}

void loop()
{
    while(!$next_frame())
        ;
    if($pressed(RIGHT_BUTTON)) x = x + MOVE_SPEED;
    if($pressed(LEFT_BUTTON )) x = x - MOVE_SPEED;
    if($pressed(DOWN_BUTTON )) y = y + MOVE_SPEED;
    if($pressed(UP_BUTTON   )) y = y - MOVE_SPEED;
    
    $draw_filled_rect(x, y, 16, 16, 1);
    $draw_sprite(x -  8, y +  4, MY_SPRITE, 0);
    $draw_sprite(x +  4, y -  8, MY_SPRITE, 0);
    $draw_sprite(x + 16, y +  4, MY_SPRITE, 0);
    $draw_sprite(x +  4, y + 16, MY_SPRITE, 0);
    draw_num(0, 0, x);
    draw_num(0, 6, y);
    
    $display();
}

void main()
{
    setup();
    while(true)
        loop();
})";

static std::string const info_json = R"({
    "name": "My Game",
    "author": "Unknown",
    "desc": "A totally awesome game!"
})";

void create_default_info_file()
{
    auto f = project.files[INFO_FILENAME] = std::make_shared<project_file_t>();
    f->filename = INFO_FILENAME;
    f->set_content(info_json);
}

void new_project()
{
    project = {};

    create_default_info_file();

    {
        auto f = project.files[main_name] = std::make_shared<project_file_t>();
        f->filename = main_name;
        f->set_content(main_prog);
    }

    {
        auto f = create_project_info_file(INFO_FILENAME);
        f->dirty = true;
        f->save();
    }

    open_files[main_name] = create_code_file(main_name);

    // set up docking
    {
        using namespace ImGui;
        ImGuiID t0, t1;
        DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.20f, &dockid_project, &t0);
        ImGuiDockNode* p = ImGui::DockBuilderGetNode(dockid_project);
        p->LocalFlags |=
            ImGuiDockNodeFlags_NoTabBar |
            ImGuiDockNodeFlags_NoDockingOverMe |
            ImGuiDockNodeFlags_NoDockingSplitMe;
        ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id);
        root->LocalFlags |= ImGuiDockNodeFlags_NoDockingSplitMe;

        DockBuilderSplitNode(t0, ImGuiDir_Left, 0.50f, &t0, &t1);
        DockBuilderDockWindow(open_files[main_name]->window_id().c_str(), t0);
    }
}
