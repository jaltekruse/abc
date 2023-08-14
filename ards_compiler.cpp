#include "ards_compiler.hpp"

#include <assert.h>

namespace ards
{

constexpr compiler_type_t TYPE_NONE = { 0, true };

constexpr compiler_type_t TYPE_VOID = { 0, false };
constexpr compiler_type_t TYPE_U8   = { 1, false };
constexpr compiler_type_t TYPE_U16  = { 2, false };
constexpr compiler_type_t TYPE_U24  = { 3, false };
constexpr compiler_type_t TYPE_U32  = { 4, false };
constexpr compiler_type_t TYPE_S8   = { 1, true  };
constexpr compiler_type_t TYPE_S16  = { 2, true  };
constexpr compiler_type_t TYPE_S24  = { 3, true  };
constexpr compiler_type_t TYPE_S32  = { 4, true };

static std::unordered_map<std::string, compiler_type_t> const primitive_types
{
    { "void", TYPE_VOID },
    { "u8",   TYPE_U8   },
    { "u16",  TYPE_U16  },
    { "u24",  TYPE_U24  },
    { "u32",  TYPE_U32  },
    { "s8",   TYPE_S8   },
    { "s16",  TYPE_S16  },
    { "s24",  TYPE_S24  },
    { "s32",  TYPE_S32  },
};

static std::unordered_map<sysfunc_t, compiler_func_decl_t> const sysfunc_decls
{
    { SYS_DISPLAY,          { TYPE_VOID, {} } },
    { SYS_DRAW_PIXEL,       { TYPE_VOID, { TYPE_S16, TYPE_S16, TYPE_U8 } } },
    { SYS_DRAW_FILLED_RECT, { TYPE_VOID, { TYPE_S16, TYPE_S16, TYPE_U8, TYPE_U8, TYPE_U8 } } },
    { SYS_SET_FRAME_RATE,   { TYPE_VOID, { TYPE_U8 } } },
    { SYS_NEXT_FRAME,       { TYPE_U8,   {} } },
    { SYS_IDLE,             { TYPE_VOID, {} } },
};

static bool isspace(char c)
{
    switch(c)
    {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
        return true;
    default:
        return false;
    }
}

compiler_type_t compiler_t::resolve_type(ast_node_t const& n)
{
    assert(n.type == AST::TYPE);
    if(n.type == AST::TYPE)
    {
        std::string name(n.data);
        auto it = primitive_types.find(name);
        if(it != primitive_types.end())
        {
            return it->second;
        }
        else
        {
            errs.push_back({
                "Unknown type \"" + name + "\"",
                n.line_info });
        }
    }
    return TYPE_NONE;
}

void compiler_t::compile(std::istream& fi, std::ostream& fo)
{
    assert(sysfunc_decls.size() == SYS_NUM);

    parse(fi);
    if(!errs.empty()) return;

    // trim all token whitespace
    ast.recurse([](ast_node_t& n) {
        size_t size = n.data.size();
        size_t i;
        if(size == 0) return;
        for(i = 0; isspace(n.data[i]); ++i);
        n.data.remove_prefix(i);
        for(i = 0; isspace(n.data[size - i - 1]); ++i);
        n.data.remove_suffix(i);
    });

    // gather all functions and globals and check for duplicates
    assert(ast.type == AST::PROGRAM);
    for(auto const& n : ast.children)
    {
        if(!errs.empty()) return;
        assert(n.type == AST::DECL_STMT || n.type == AST::FUNC_STMT);
        if(n.type == AST::DECL_STMT)
        {
            assert(n.children.size() == 2);
            assert(n.children[1].type == AST::IDENT);
            std::string name(n.children[1].data);
            auto it = globals.find(name);
            if(it != globals.end())
            {
                errs.push_back({
                    "Duplicate global \"" + name + "\"",
                    n.children[1].line_info });
                return;
            }
            auto& g = globals[name];
            g.name = name;
            g.type = resolve_type(n.children[0]);
        }
        else if(n.type == AST::FUNC_STMT)
        {
            assert(n.children.size() == 3);
            assert(n.children[1].type == AST::IDENT);
            assert(n.children[2].type == AST::BLOCK);
            std::string name(n.children[1].data);
            auto it = funcs.find(name);
            if(it != funcs.end())
            {
                errs.push_back({
                    "Duplicate function \"" + name + "\"",
                    n.children[1].line_info });
                return;
            }
            auto& f = funcs[name];
            f.decl.return_type = resolve_type(n.children[0]);
            f.name = name;
        }
    }
}

}
