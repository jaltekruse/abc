#include "ards_compiler.hpp"

#include <algorithm>
#include <assert.h>

namespace ards
{

static void implicit_conversion(compiler_type_t& ta, compiler_type_t& tb)
{
    if(ta == tb) return;
    if(ta.is_signed == tb.is_signed)
    {
        if(ta.prim_size < tb.prim_size)
            ta = tb;
    }
    else if(ta.is_signed)
    {
        // a signed, b unsigned
        if(ta.prim_size <= tb.prim_size)
            ta = tb;
        else
            tb = ta;
    }
}

static bool check_prim(
    compiler_type_t const& t, ast_node_t const& a, std::vector<error_t> errs)
{
    if(!t.is_prim())
    {
        errs.push_back({
            "\"" + std::string(a.data) + "\" is not a primitive type",
            a.line_info });
        return false;
    }
    return true;
}

static bool check_prim(ast_node_t const& a, std::vector<error_t> errs)
{
    return check_prim(a.comp_type.without_ref(), a, errs);
}

static void insert_aref(ast_node_t& a, compiler_type_t const& t)
{
    auto ta = std::move(a);
    a = { {}, AST::OP_AREF };
    a.comp_type.type = compiler_type_t::ARRAY_REF;
    a.comp_type.children.push_back(t);
    a.comp_type.prim_size = t.without_ref().is_prog ? 6 : 4;
    a.children.emplace_back(std::move(ta));
};

static void insert_cast(ast_node_t& a, compiler_type_t const& t)
{
    auto ta = std::move(a);
    a = { {}, AST::OP_CAST };
    a.comp_type = t;
    a.children.push_back({});
    a.children.back().comp_type = t;
    a.children.emplace_back(std::move(ta));
};

compiler_type_t prim_type_for_dec(uint32_t x, bool is_signed)
{
    size_t prim_size = 1;

    if(is_signed)
    {
        if(x < (1 << 7)) prim_size = 1;
        else if(x < (1 << 15)) prim_size = 2;
        else if(x < (1 << 23)) prim_size = 3;
        else prim_size = 4;
    }
    else
    {
        if(x < (1 << 8)) prim_size = 1;
        else if(x < (1 << 16)) prim_size = 2;
        else if(x < (1 << 24)) prim_size = 3;
        else prim_size = 4;
    }

    compiler_type_t t{};
    t.prim_size = prim_size;
    t.type = compiler_type_t::PRIM;
    t.is_signed = is_signed;
    return t;
}

compiler_type_t prim_type_for_hex(uint32_t x, bool is_signed)
{
    size_t prim_size = 1;

    if(x < (1 << 7)) prim_size = 1;
    else if(x < (1 << 8)) prim_size = 1, is_signed = false;
    else if(x < (1 << 15)) prim_size = 2;
    else if(x < (1 << 16)) prim_size = 2, is_signed = false;
    else if(x < (1 << 23)) prim_size = 3;
    else if(x < (1 << 24)) prim_size = 3, is_signed = false;
    else if(x < (1ll << 31)) prim_size = 4;
    else prim_size = 4, is_signed = false;

    compiler_type_t t{};
    t.prim_size = prim_size;
    t.type = compiler_type_t::PRIM;
    t.is_signed = is_signed;
    return t;
}

void compiler_t::type_annotate_recurse(ast_node_t& a, compiler_frame_t const& frame)
{
    if(!errs.empty()) return;
    switch(a.type)
    {
    case AST::SPRITES:
        a.comp_type = TYPE_SPRITES;
        break;
    case AST::FONT:
        a.comp_type = TYPE_FONT;
        break;
    case AST::COMPOUND_LITERAL:
        for(auto& child : a.children)
            type_annotate_recurse(child, frame);
        break;
    case AST::TYPE_ARRAY:
        type_annotate_recurse(a.children[0], frame);
        break;
    case AST::OP_CAST:
        assert(a.children.size() == 2);
        type_annotate_recurse(a.children[1], frame);
        a.comp_type = a.children[0].comp_type;
        break;
    case AST::OP_UNARY:
    {
        assert(a.children.size() == 2);
        type_annotate_recurse(a.children[1], frame);
        auto op = a.children[0].data;
        if(!a.children[0].comp_type.without_ref().is_prim())
        {
            errs.push_back({
                "\"" + std::string(a.children[0].data) + "\" is not a primitive type",
                a.children[0].line_info });
            break;
        }
        if(op == "!")
            a.comp_type = TYPE_BOOL;
        else if(op == "-" || op == "~")
            a.comp_type = a.children[1].comp_type.without_ref();
        else
        {
            assert(false);
        }
        break;
    }
    case AST::OP_LOGICAL_AND:
    case AST::OP_LOGICAL_OR:
        assert(a.children.size() == 2);
        for(auto& child : a.children)
            type_annotate_recurse(child, frame);
        a.comp_type = TYPE_BOOL;
        break;
    case AST::OP_ASSIGN:
    {
        assert(a.children.size() == 2);
        for(auto& child : a.children)
            type_annotate_recurse(child, frame);
        a.comp_type = a.children[0].comp_type.without_ref();
        break;
    }
    case AST::OP_SHIFT:
        assert(a.children.size() == 2);
        for(auto& child : a.children)
            type_annotate_recurse(child, frame);
        if(!check_prim(a.children[0], errs)) break;
        if(!check_prim(a.children[1], errs)) break;
        a.comp_type = a.children[0].comp_type.without_ref();
        break;
    case AST::OP_BITWISE_AND:
    case AST::OP_BITWISE_OR:
    case AST::OP_BITWISE_XOR:
    case AST::OP_EQUALITY:
    case AST::OP_RELATIONAL:
    case AST::OP_ADDITIVE:
    case AST::OP_MULTIPLICATIVE:
    {
        // C-style implicit conversion rules
        assert(a.children.size() == 2);
        for(auto& child : a.children)
            type_annotate_recurse(child, frame);
        auto t0 = a.children[0].comp_type.without_ref();
        auto t1 = a.children[1].comp_type.without_ref();

        if((a.type == AST::OP_BITWISE_AND ||
            a.type == AST::OP_BITWISE_OR ||
            a.type == AST::OP_BITWISE_XOR) &&
            (t0.is_bool || t1.is_bool))
        {
            errs.push_back({
                "Bitwise and, or, and xor may not operate on boolean types.",
                a.line_info });
            return;
        }

        bool ref0 = a.children[0].comp_type.is_ref();
        bool ref1 = a.children[1].comp_type.is_ref();
        bool is_divmod = (
            a.type == AST::OP_MULTIPLICATIVE && (
                a.data == "/" || a.data == "%"));

        if((a.type == AST::OP_EQUALITY || a.type == AST::OP_RELATIONAL) &&
            (t0.is_label_ref() && t1.is_label_ref() && t0.type == t1.type))
        {
            a.comp_type = TYPE_BOOL;
            break;
        }

        if(!check_prim(t0, a.children[0], errs)) break;
        if(!check_prim(t1, a.children[1], errs)) break;

        t0.is_bool = false;
        t1.is_bool = false;

        if(a.type == AST::OP_ADDITIVE)
        {
            t0.prim_size = t1.prim_size = std::min<size_t>(4,
                std::max(t0.prim_size, t1.prim_size) + 1);
            t0.is_signed = t1.is_signed = (t0.is_signed || t1.is_signed);
        }
        else if(a.type == AST::OP_MULTIPLICATIVE && a.data == "*")
        {
            t0.prim_size = t1.prim_size = std::min<size_t>(4,
                t0.prim_size + t1.prim_size);
            t0.is_signed = t1.is_signed = (t0.is_signed || t1.is_signed);
        }
        else if(is_divmod && t0.is_signed != t1.is_signed)
        {
            t0.prim_size = t1.prim_size = std::min<size_t>(4,
                std::max(t0.prim_size, t1.prim_size) + 1);
        }
        if(t0 != t1 || ref0 || ref1)
        {
            implicit_conversion(t0, t1);
            implicit_conversion(t1, t0);
        }

        if(is_divmod)
        {
            if(t0.prim_size < 2 && t1.prim_size < 2)
                t0.prim_size = t1.prim_size = 2;
            else if(t0.prim_size < 4 && t1.prim_size < 4)
                t0.prim_size = t1.prim_size = 4;
        }

        if(ref0 || t0 != a.children[0].comp_type)
            insert_cast(a.children[0], t0);
        if(ref1 || t1 != a.children[1].comp_type)
            insert_cast(a.children[1], t1);

        if(a.type == AST::OP_EQUALITY || a.type == AST::OP_RELATIONAL)
            a.comp_type = TYPE_BOOL;
        else
            a.comp_type = a.children[0].comp_type.without_ref();
        break;
    }
    case AST::INT_CONST:
        // already done during parsing
        break;
    case AST::IDENT:
    {
        std::string name(a.data);
        for(auto it = frame.scopes.rbegin(); it != frame.scopes.rend(); ++it)
        {
            auto jt = it->locals.find(name);
            if(jt != it->locals.end())
            {
                a.comp_type = jt->second.var.type;
                return;
            }
        }
        auto it = globals.find(name);
        if(it != globals.end())
        {
            a.comp_type = it->second.var.type;
            return;
        }
        errs.push_back({ "Undefined variable \"" + name + "\"", a.line_info });
        break;
    }
    case AST::FUNC_CALL:
    {
        assert(a.children.size() == 2);
        for(auto& c : a.children[1].children)
            type_annotate_recurse(c, frame);
        if(a.children[0].data == "len")
        {
            transform_array_len(a);
            break;
        }
        auto f = resolve_func(a.children[0]);
        bool is_format = (f.name == "$format");
        size_t used_args = a.children[1].children.size();
        size_t func_args = f.decl.arg_types.size();
        if(!is_format && used_args > func_args)
        {
            errs.push_back({
                "Too many arguments to function \"" + f.name + "\"",
                a.line_info });
            return;
        }
        if(used_args < func_args)
        {
            errs.push_back({
                "Too few arguments to function \"" + f.name + "\"",
                a.line_info });
            return;
        }
        if(f.name.empty()) break;

        auto* arg_types = &f.decl.arg_types;
        std::vector<compiler_type_t> format_types;
        if(is_format)
        {
            std::string format_str;
            resolve_format_call(a.children[1], format_types, format_str);
            arg_types = &format_types;
        }

        for(size_t i = 0; i < used_args; ++i)
        {
            auto& c = a.children[1].children[i];
            auto const& t = (*arg_types)[i];
            if(is_format && i == 1 && c.type != AST::STRING_LITERAL)
            {
                errs.push_back({
                    "Format string for $format must be a string literal",
                    c.line_info });
                return;
            }
            if(t.is_array_ref())
            {
                if(c.comp_type.is_array_ref())
                {
                    if(t != c.comp_type)
                    {
                        errs.push_back({
                            "Incompatible array element types for UARs",
                            c.line_info });
                        return;
                    }
                    continue;
                }
                if(t.children[0].is_prog != c.comp_type.without_ref().is_prog)
                {
                    errs.push_back({
                        "Mismatched prog for UAR conversion",
                        c.line_info });
                    return;
                }
                insert_aref(c, t);
            }
            else if(c.comp_type != t && c.type != AST::COMPOUND_LITERAL)
                insert_cast(c, t);
        }
        a.comp_type = f.decl.return_type;
        break;
    }
    case AST::ARRAY_INDEX:
    {
        assert(a.children.size() == 2);
        type_annotate_recurse(a.children[0], frame);
        type_annotate_recurse(a.children[1], frame);
        auto t0 = a.children[0].comp_type.without_ref();
        if(!t0.is_array() && !t0.is_array_ref())
        {
            errs.push_back({
                "\"" + std::string(a.children[0].data) +
                "\" is not an array", a.line_info });
            break;
        }
        a.comp_type.type = compiler_type_t::REF;
        a.comp_type.prim_size = 2;
        a.comp_type.children.push_back(t0.children[0]);
        break;
    }
    case AST::STRUCT_MEMBER:
    {
        assert(a.children.size() == 2);
        type_annotate_recurse(a.children[0], frame);
        if(!a.children[0].comp_type.without_ref().is_struct())
        {
            errs.push_back({
                "\"" + std::string(a.children[0].data) +
                "\" is not a struct", a.children[0].line_info
            });
            break;
        }
        std::string name = std::string(a.children[1].data);
        auto* t0 = resolve_member(a.children[0], name);
        if(!t0) break;
        a.comp_type.type = compiler_type_t::REF;
        a.comp_type.prim_size = 2;
        a.comp_type.children.push_back(t0->without_ref());
        break;
    }
    case AST::STRING_LITERAL:
    {
        a.comp_type = strlit_type(strlit_data(a).size());
        break;
    }
    default:
        break;
    }
}

void compiler_t::type_reduce_recurse(ast_node_t& a, size_t size)
{
    if(!errs.empty()) return;
    auto min_size = std::min(a.comp_type.prim_size, size);
    switch(a.type)
    {
    case AST::ARRAY_INDEX:
        type_reduce_recurse(a.children[1], std::min<size_t>(2, min_size));
        break;
    case AST::OP_CAST:
        if(a.children[0].comp_type.is_bool)
            break;
        min_size = std::min(min_size, a.children[0].comp_type.prim_size);
        a.comp_type.prim_size = min_size;
        a.children[0].comp_type.prim_size = min_size;
        type_reduce_recurse(a.children[1], min_size);
        break;
    case AST::OP_BITWISE_AND:
    case AST::OP_BITWISE_OR:
    case AST::OP_BITWISE_XOR:
    case AST::OP_ADDITIVE:
    case AST::OP_MULTIPLICATIVE:
        if(a.type == AST::OP_MULTIPLICATIVE && a.data != "*")
            break;
        a.comp_type.prim_size = min_size;
        type_reduce_recurse(a.children[0], min_size);
        type_reduce_recurse(a.children[1], min_size);
        break;
    case AST::OP_ASSIGN:
        a.comp_type.prim_size = min_size;
        type_reduce_recurse(a.children[1], a.children[0].comp_type.without_ref().prim_size);
        break;
    case AST::FUNC_CALL:
    {
        auto func = resolve_func(a.children[0]);
        for(size_t i = 0; i < func.decl.arg_types.size(); ++i)
        {
            auto const& type = func.decl.arg_types[i];
            auto& expr = a.children[1].children[i];
            if(!type.is_prim())
                continue;
            type_reduce_recurse(expr, std::min(size, type.prim_size));
        }
        break;
    }
    case AST::IDENT:
    case AST::INT_CONST:
        if(a.comp_type.type == compiler_type_t::PRIM && min_size != a.comp_type.prim_size)
            insert_cast(a, a.comp_type.sized_to(min_size));
        break;
    default:
        break;
    }
}

void compiler_t::type_annotate(ast_node_t& a, compiler_frame_t const& frame, size_t size)
{
    type_annotate_recurse(a, frame);
    transform_constexprs(a, frame);
    type_reduce_recurse(a, size);
}

}
