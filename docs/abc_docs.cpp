#include <ards_assembler.hpp>
#include <ards_compiler.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

#include <all_fonts.hpp>
#include <stb_truetype.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <stdio.h>

constexpr uint8_t FONT_HEADER_PER_CHAR = 7;
constexpr uint8_t FONT_HEADER_OFFSET = 1;
constexpr uint16_t FONT_HEADER_CHAR_BYTES =
    FONT_HEADER_PER_CHAR * 256;
constexpr uint16_t FONT_HEADER_BYTES =
    FONT_HEADER_CHAR_BYTES + FONT_HEADER_OFFSET;

static void draw_str(
    std::vector<uint8_t> const& font,
    std::vector<uint8_t>& buf,
    int w, int h,
    int x, int y,
    char const* str)
{
    // gray line
    for(int c = 0; c < w; ++c)
        buf[y * w + c] = 0x40;

    for(uint8_t cc; (cc = (uint8_t)*str) != '\0'; ++str)
    {
        int sw = font[int(cc) * FONT_HEADER_PER_CHAR + 5];
        int sh = font[int(cc) * FONT_HEADER_PER_CHAR + 6];
        int offset = font[int(cc) * FONT_HEADER_PER_CHAR + 3];
        offset += font[int(cc) * FONT_HEADER_PER_CHAR + 4] * 256;
        for(int r = 0; r < sh; ++r)
        {
            int tr = r + y;
            tr += (int8_t)font[int(cc) * FONT_HEADER_PER_CHAR + 2]; // yoff
            int tp = r / 8;
            if(tr < 0) continue;
            if(tr >= h) break;
            for(int c = 0; c < sw; ++c)
            {
                int tc = c + x;
                tc += (int8_t)font[int(cc) * FONT_HEADER_PER_CHAR + 1]; // xoff

                if(tc < 0)
                    continue;
                if(tc >= w)
                    break;
                uint8_t& t = buf[tr * w + tc];
                if((font[FONT_HEADER_BYTES + offset + sw * tp + c] >> (r & 7)) & 1)
                    t = 0xff;
            }
        }
        x += font[int(cc) * FONT_HEADER_PER_CHAR + 0];
    }
}

int abc_docs()
{
    std::map<std::string, ards::sysfunc_t> const sys_names(
        ards::sys_names.begin(), ards::sys_names.end());

    FILE* f;
    
    f = fopen(DOCS_DIR "/system.md", "w");
    if(!f) return 1;

    fprintf(f, "# Predefined Constants\n\n```c\n");
    for(auto const& c : ards::builtin_constexprs)
    {
        fprintf(f, "%s %s;\n", ards::type_name(c.type).c_str(), c.name.c_str());
    }
    fprintf(f, "```\n\n");

    fprintf(f, "# System Calls\n\n```c\n");
    for(auto const& [k, v] : sys_names)
    {
        auto it = ards::sysfunc_decls.find(v);
        if(it == ards::sysfunc_decls.end()) continue;
        auto const& decl = it->second;
        fprintf(f, "%-5s $%s(", ards::type_name(decl.return_type).c_str(), k.c_str());
        for(size_t i = 0; i < decl.arg_types.size(); ++i)
        {
            if(i != 0) fprintf(f, ", ");
            fprintf(f, "%s %s", ards::type_name(decl.arg_types[i]).c_str(), decl.arg_names[i].c_str());
        }
        if(ards::sysfunc_is_format(v))
            fprintf(f, ", ...");
        fprintf(f, ");\n");
    }
    fprintf(f, "```\n\n");

    fclose(f);

    f = fopen(DOCS_DIR "/builtin_fonts.md", "w");
    if(!f) return 1;

    fprintf(f, "# Built-in Font Assets\n");
    fprintf(f, "| Predefined Variable | Ascent | Line Height | Bytes | Preview |\n");
    fprintf(f, "|---|---|---|---|---|\n");

    std::vector<std::tuple<int, int, std::string, std::vector<uint8_t>>> fonts;

    for(auto const& font : ALL_FONTS)
    {
        std::vector<uint8_t> data;
        data.resize(font.size);
        memcpy(data.data(), font.data, data.size());
        fonts.push_back({ (int)font.pixels, data[FONT_HEADER_CHAR_BYTES + 0], font.name, data });
    }

    std::sort(fonts.begin(), fonts.end());

    for(auto const& font : fonts)
    {
        auto const& data = std::get<3>(font);
        fprintf(f, "| `%s` | %d | %d | %d |",
            std::get<2>(font).c_str(),
            std::get<0>(font),
            std::get<1>(font),
            (int)data.size());
        static char const STR_LOWER[] = "the quick brown fox jumps over the lazy dog";
        static char const STR_UPPER[] = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG";
        static char const STR_SYM[] = "0123456789,.:?![/](\\){|}+-=<>@#$%^&*';\"";
        int h = data[FONT_HEADER_CHAR_BYTES + 0] * 14 / 4;
        int w = 2;
        for(char c : STR_UPPER)
            w += data[int(uint8_t(c)) * FONT_HEADER_PER_CHAR + 0];
        if(w > 512)
            w = 512;
        std::vector<uint8_t> buf;
        buf.resize(w * h);
        draw_str(data, buf, w, h, 1, data[FONT_HEADER_CHAR_BYTES + 0] * 1, STR_UPPER);
        draw_str(data, buf, w, h, 1, data[FONT_HEADER_CHAR_BYTES + 0] * 2, STR_LOWER);
        draw_str(data, buf, w, h, 1, data[FONT_HEADER_CHAR_BYTES + 0] * 3, STR_SYM);
        stbi_write_png(
            (std::string(DOCS_DIR) + "/font_images/" + std::get<2>(font) + ".png").c_str(),
            w, h, 1, buf.data(), w);
        fprintf(f, " ![%s](font_images/%s.png) |\n", std::get<2>(font).c_str(), std::get<2>(font).c_str());
    }

    fclose(f);

    return 0;
}
