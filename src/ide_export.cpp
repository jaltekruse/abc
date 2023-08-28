#include "ide_common.hpp"

#include <fstream>
#include <cassert>

#include <imgui.h>
#include <nfd.hpp>

#include <miniz.h>
#include <miniz_zip.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>

static void export_compiled_fxdata(std::string const& filename)
{
    std::ofstream f(filename.c_str(), std::ios::out | std::ios::binary);
    if(!f) return;
    f.write((char const*)project.binary.data(), project.binary.size());
}

static void export_fxdata()
{
    if(!compile_all())
        return;

    NFD::UniquePath path;
    nfdfilteritem_t filterItem[1] = { { "FX Data", "bin" } };
    auto result = NFD::SaveDialog(path, filterItem, 1, nullptr, "fxdata.bin");
    if(result != NFD_OKAY)
        return;

    export_compiled_fxdata(path.get());
}

static size_t zip_write_data(
    void* data, mz_uint64 file_ofs, const void* pBuf, size_t n)
{
    auto& d = *(std::vector<uint8_t>*)data;
    assert(file_ofs == d.size());
    size_t bytes = file_ofs + n;
    if(bytes > d.size())
        d.resize(bytes);
    memcpy(d.data() + file_ofs, pBuf, n);
    return n;
}

static void export_arduboy()
{
    if(!compile_all())
        return;

    NFD::UniquePath path;
    nfdfilteritem_t filterItem[1] = { { "Arduboy Game", "arduboy" } };
    auto result = NFD::SaveDialog(path, filterItem, 1, nullptr, "game.arduboy");
    if(result != NFD_OKAY)
        return;
    std::string filename = path.get();

    std::string info_json;
    {
        rapidjson::StringBuffer s;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> w(s);
        w.StartObject();

        w.Key("schemaVersion");
        w.String("3");
        w.Key("title");
        w.String(project.info.name.c_str());
        w.Key("description");
        w.String(project.info.desc.c_str());
        w.Key("author");
        w.String(project.info.author.c_str());

        w.Key("binaries");
        w.StartArray();
        w.StartObject();
        w.Key("title");
        w.String(project.info.name.c_str());
        w.Key("filename");
        w.String("abc_interpreter.hex");
        w.Key("flashdata");
        w.String("abc_data.bin");
        w.Key("device");
        w.String("ArduboyFX");
        w.EndObject();
        w.EndArray();

        w.EndObject();
        info_json = s.GetString();
    }

    std::vector<uint8_t> zipdata;
    mz_zip_archive zip{};
    zip.m_pWrite = zip_write_data;
    zip.m_pIO_opaque = &zipdata;

    mz_zip_writer_init(&zip, 0);

    mz_zip_writer_add_mem(
        &zip, "info.json",
        info_json.data(), info_json.size(),
        MZ_DEFAULT_COMPRESSION);

    mz_zip_writer_add_mem(
        &zip, "abc_interpreter.hex",
        VM_HEX_ARDUBOYFX, VM_HEX_ARDUBOYFX_SIZE,
        MZ_DEFAULT_COMPRESSION);

    mz_zip_writer_add_mem(
        &zip, "abc_data.bin",
        project.binary.data(), project.binary.size(),
        MZ_DEFAULT_COMPRESSION);

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);

    std::ofstream f(filename.c_str(), std::ios::out | std::ios::binary);
    if(!f) return;
    f.write((char const*)zipdata.data(), zipdata.size());
}

void export_menu_items()
{
    using namespace ImGui;
    if(MenuItem("Export FX data..."))
        export_fxdata();
    if(MenuItem("Export .arduboy file..."))
        export_arduboy();
}