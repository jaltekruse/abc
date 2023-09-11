#include "ards_compiler.hpp"

#include <assert.h>

namespace ards
{

void compiler_t::codegen_expr(
    compiler_func_t& f, compiler_frame_t& frame, ast_node_t const& a, bool ref)
{
    if(!errs.empty()) return;
    switch(a.type)
    {

    case AST::OP_CAST:
    {
        if(ref) goto rvalue_error;
        assert(a.children.size() == 2);
        codegen_expr(f, frame, a.children[1], false);
        codegen_convert(f, frame, a, a.children[0].comp_type, a.children[1].comp_type);
        return;
    }

    case AST::OP_UNARY:
    {
        if(ref) goto rvalue_error;
        assert(a.children.size() == 2);
        auto op = a.children[0].data;
        if(op == "!")
        {
            codegen_expr(f, frame, a.children[1], false);
            codegen_convert(f, frame, a, TYPE_BOOL, a.children[1].comp_type);
            f.instrs.push_back({ I_NOT, a.children[1].line() });
        }
        else if(op == "-")
        {
            auto size = a.children[1].comp_type.prim_size;
            for(size_t i = 0; i < size; ++i)
                f.instrs.push_back({ I_PUSH, a.children[1].line(), 0 });
            frame.size += size;
            codegen_expr(f, frame, a.children[1], false);
            codegen_convert(f, frame, a, a.comp_type, a.children[1].comp_type);
            f.instrs.push_back({ instr_t(I_SUB + size - 1), a.children[1].line() });
            frame.size -= size;
        }
        else if(op == "~")
        {
            auto size = a.children[1].comp_type.prim_size;
            codegen_expr(f, frame, a.children[1], false);
            f.instrs.push_back({ instr_t(I_COMP + size - 1), a.children[1].line() });
        }
        else
        {
            assert(false);
        }
        return;
    }

    case AST::INT_CONST:
    {
        if(ref) goto rvalue_error;
        uint32_t x = (uint32_t)a.value;
        auto size = a.comp_type.prim_size;
        frame.size += size;
        for(size_t i = 0; i < size; ++i, x >>= 8)
            f.instrs.push_back({ I_PUSH, a.line(), (uint8_t)x });
        return;
    }

    case AST::LABEL_REF:
    {
        f.instrs.push_back({ I_PUSHL, a.line(), 0, 0, std::string(a.data) });
        frame.size += 3;
        return;
    }

    case AST::IDENT:
    {
        std::string name(a.data);
        if(auto* local = resolve_local(frame, a))
        {
            assert(!local->var.is_constexpr);
            uint8_t offset = (uint8_t)(frame.size - local->frame_offset);
            uint8_t size = (uint8_t)local->var.type.prim_size;
            if(ref && local->var.type.type != compiler_type_t::REF)
            {
                f.instrs.push_back({ I_REFL, a.line(), offset });
                frame.size += 2;
                return;
            }
            assert(!local->var.type.is_prog);
            f.instrs.push_back({ I_PUSH, a.line(), size });
            f.instrs.push_back({ I_GETLN, a.line(), offset });
            frame.size += (uint8_t)local->var.type.prim_size;
            return;
        }
        if(auto* global = resolve_global(a))
        {
            assert(!global->var.is_constexpr);
            bool prog = global->var.type.is_prog;
            if(global->is_constexpr_ref())
            {
                if(prog)
                    f.instrs.push_back({ I_PUSHL, a.line(), 0, 0, global->constexpr_ref });
                else
                    f.instrs.push_back({ I_PUSHG, a.line(), 0, 0, global->constexpr_ref });
                frame.size += (prog ? 3 : 2);
                return;
            }
            if(ref && prog)
            {
                f.instrs.push_back({ I_PUSHL, a.line(), 0, 0, global->name });
                frame.size += 3;
                return;
            }
            if(ref && !prog)
            {
                f.instrs.push_back({ I_REFG, a.line(), 0, 0, global->name });
                frame.size += 2;
                return;
            }
            assert(global->var.type.prim_size < 256);
            frame.size += (uint8_t)global->var.type.prim_size;
            if(prog)
            {
                f.instrs.push_back({ I_PUSHL, a.line(), 0, 0, global->name });
                f.instrs.push_back({ I_GETPN, a.line(), (uint8_t)global->var.type.prim_size });
            }
            else
            {
                f.instrs.push_back({ I_PUSH, a.line(), (uint8_t)global->var.type.prim_size });
                f.instrs.push_back({ I_GETGN, a.line(), 0, 0, global->name });
            }
            return;
        }
        errs.push_back({ "Undefined variable \"" + name + "\"", a.line_info });
        return;
    }

    case AST::FUNC_CALL:
    {
        if(ref) goto rvalue_error;
        assert(a.children.size() == 2);
        assert(a.children[0].type == AST::IDENT);

        auto func = resolve_func(a.children[0]);

        // TODO: test for reference return type (not allowed)

        // system functions don't need space reserved for return value
        if(!func.is_sys)
        {
            // reserve space for return value
            frame.size += func.decl.return_type.prim_size;
            for(size_t i = 0; i < func.decl.return_type.prim_size; ++i)
                f.instrs.push_back({ I_PUSH, a.line(), 0 });
        }

        assert(a.children[1].type == AST::FUNC_ARGS);
        if(a.children[1].children.size() != func.decl.arg_types.size())
        {
            errs.push_back({
                "Incorrect number of arguments to function \"" + func.name + "\"",
                a.line_info });
            return;
        }
        size_t prev_size = frame.size;
        for(size_t i = 0; i < func.decl.arg_types.size(); ++i)
        {
            auto const& type = func.decl.arg_types[i];
            auto const& expr = a.children[1].children[i];
            // handle reference function arguments
            bool tref = (type.type == compiler_type_t::REF);
            if(tref && type.without_ref() != expr.comp_type.without_ref())
            {
                errs.push_back({
                    "Cannot create reference to expression",
                    expr.line_info });
                return;
            }
            if(expr.type == AST::COMPOUND_LITERAL)
                codegen_expr_compound(f, frame, expr, type);
            else
            {
                codegen_expr(f, frame, expr, tref);
                if(!tref)
                    codegen_convert(f, frame, a, type, expr.comp_type);
            }
        }
        // called function should pop stack
        frame.size = prev_size;

        if(func.is_sys)
            f.instrs.push_back({ I_SYS, a.line(), func.sys });
        else
            f.instrs.push_back({ I_CALL, a.line(), 0, 0, std::string(a.children[0].data) });

        // system functions push return value onto stack
        if(func.is_sys)
            frame.size += func.decl.return_type.prim_size;

        return;
    }

    case AST::OP_ASSIGN:
    {
        assert(a.children.size() == 2);
        assert(a.children[0].comp_type.prim_size != 0);
        if(a.children[0].comp_type.has_child_ref())
        {
            errs.push_back({
                "\"" + std::string(a.children[0].data) + "\" contains references "
                "and thus cannot be reassigned", a.children[0].line_info });
            return;
        }
        auto const& type_noref = a.children[0].comp_type.without_ref();
        if(type_noref.is_prim() || type_noref.is_sprites())
        {
            codegen_expr(f, frame, a.children[1], false);
            codegen_convert(f, frame, a, a.children[0].comp_type, a.children[1].comp_type);
        }
        else if(type_noref.type == compiler_type_t::ARRAY)
        {
            if(a.children[1].type == AST::COMPOUND_LITERAL)
                codegen_expr_compound(f, frame, a.children[1], type_noref);
            else if(type_noref != a.children[1].comp_type.without_ref())
            {
                errs.push_back({
                    "Incompatible types in assignment to \"" +
                    std::string(a.children[0].data) + "\"",
                    a.line_info });
                return;
            }
            else
                codegen_expr(f, frame, a.children[1], false);
        }

        // dup value if not the root op
        switch(a.parent->type)
        {
        case AST::EXPR_STMT:
        case AST::LIST:
            break;
        default:
            frame.size += (uint8_t)a.children[0].comp_type.prim_size;
            f.instrs.push_back({ I_PUSH, a.line(), (uint8_t)a.children[0].comp_type.prim_size });
            f.instrs.push_back({ I_GETLN, a.line(), (uint8_t)a.children[0].comp_type.prim_size });
            break;
        }

        auto lvalue = resolve_lvalue(f, frame, a.children[0]);
        codegen_store_lvalue(f, frame, lvalue);
        return;
    }

    case AST::OP_EQUALITY:
    case AST::OP_RELATIONAL:
    {
        if(ref) goto rvalue_error;
        assert(a.children.size() == 2);
        if(a.children[0].comp_type != a.children[1].comp_type)
        {
            errs.push_back({ "Incompatible types in comparison", a.line_info });
            return;
        }
        assert(a.comp_type == TYPE_BOOL);
        size_t i0 = 0, i1 = 1;
        if(a.data == ">" || a.data == ">=")
            std::swap(i0, i1);
        codegen_expr(f, frame, a.children[i0], false);
        codegen_expr(f, frame, a.children[i1], false);

        auto size = a.children[0].comp_type.prim_size;
        assert(size >= 1 && size <= 4);
        frame.size -= size;       // comparison
        frame.size -= (size - 1); // conversion to bool
        if(a.data == "==" || a.data == "!=")
        {
            f.instrs.push_back({ instr_t(I_SUB + size - 1), a.line() });
            f.instrs.push_back({ instr_t(I_BOOL + size - 1), a.line() });
            if(a.data == "==")
                f.instrs.push_back({ I_NOT, a.line() });
        }
        else if(a.data == "<=" || a.data == ">=")
        {
            instr_t i = (a.children[0].comp_type.is_signed ? I_CSLE : I_CULE);
            f.instrs.push_back({ instr_t(i + size - 1), a.line() });
        }
        else if(a.data == "<" || a.data == ">")
        {
            instr_t i = (a.children[0].comp_type.is_signed ? I_CSLT : I_CULT);
            f.instrs.push_back({ instr_t(i + size - 1), a.line() });
        }
        else
            assert(false);
        return;
    }

    case AST::OP_ADDITIVE:
    {
        if(ref) goto rvalue_error;
        assert(a.data == "+" || a.data == "-");
        assert(a.children.size() == 2);
        codegen_expr(f, frame, a.children[0], false);
        codegen_convert(f, frame, a, a.comp_type, a.children[0].comp_type);
        codegen_expr(f, frame, a.children[1], false);
        codegen_convert(f, frame, a, a.comp_type, a.children[1].comp_type);
        static_assert(I_ADD2 == I_ADD + 1);
        static_assert(I_ADD3 == I_ADD + 2);
        static_assert(I_ADD4 == I_ADD + 3);
        static_assert(I_SUB2 == I_SUB + 1);
        static_assert(I_SUB3 == I_SUB + 2);
        static_assert(I_SUB4 == I_SUB + 3);
        auto size = a.comp_type.prim_size;
        assert(size >= 1 && size <= 4);
        frame.size -= size;
        f.instrs.push_back({ instr_t((a.data == "+" ? I_ADD : I_SUB) + size - 1), a.line() });
        return;
    }

    case AST::OP_MULTIPLICATIVE:
    {
        if(ref) goto rvalue_error;
        assert(a.children.size() == 2);
        //assert(a.children[0].comp_type == a.comp_type);
        //assert(a.children[1].comp_type == a.comp_type);
        codegen_expr(f, frame, a.children[0], false);
        codegen_convert(f, frame, a, a.comp_type, a.children[0].comp_type);
        codegen_expr(f, frame, a.children[1], false);
        codegen_convert(f, frame, a, a.comp_type, a.children[1].comp_type);
        static_assert(I_MUL2 == I_MUL + 1);
        static_assert(I_MUL3 == I_MUL + 2);
        static_assert(I_MUL4 == I_MUL + 3);
        auto size = a.comp_type.prim_size;
        assert(size >= 1 && size <= 4);
        frame.size -= size;
        if(a.data == "*")
            f.instrs.push_back({ instr_t(I_MUL + size - 1), a.line() });
        else if(a.data == "/")
        {
            auto tsize = a.comp_type.prim_size;
            assert(tsize == 2 || tsize == 4);
            if(a.comp_type.is_signed)
                f.instrs.push_back({ tsize == 2 ? I_DIV2 : I_DIV4, a.line() });
            else
                f.instrs.push_back({ tsize == 2 ? I_UDIV2 : I_UDIV4, a.line() });
        }
        else if(a.data == "%")
        {
            auto tsize = a.comp_type.prim_size;
            assert(tsize == 2 || tsize == 4);
            if(a.comp_type.is_signed)
                f.instrs.push_back({ tsize == 2 ? I_MOD2 : I_MOD4, a.line() });
            else
                f.instrs.push_back({ tsize == 2 ? I_UMOD2 : I_UMOD4, a.line() });
        }
        else
            assert(false);
        return;
    }

    case AST::OP_SHIFT:
    {
        codegen_expr(f, frame, a.children[0], false);
        codegen_expr(f, frame, a.children[1], false);
        codegen_convert(f, frame, a, TYPE_U8, a.children[1].comp_type);
        frame.size -= 1;
        auto index = a.comp_type.prim_size - 1;
        if(a.data == "<<")
            f.instrs.push_back({ instr_t(I_LSL + index), a.line() });
        else if(a.data == ">>")
        {
            if(a.comp_type.is_signed)
                f.instrs.push_back({ instr_t(I_ASR + index), a.line() });
            else
                f.instrs.push_back({ instr_t(I_LSR + index), a.line() });
        }
        else
            assert(false);
        return;
    }

    case AST::OP_BITWISE_AND:
    case AST::OP_BITWISE_OR:
    case AST::OP_BITWISE_XOR:
    {
        codegen_expr(f, frame, a.children[0], false);
        codegen_convert(f, frame, a, a.comp_type, a.children[1].comp_type);
        codegen_expr(f, frame, a.children[1], false);
        codegen_convert(f, frame, a, a.comp_type, a.children[1].comp_type);
        auto size = a.comp_type.prim_size;
        frame.size -= size;
        if(a.type == AST::OP_BITWISE_AND)
            f.instrs.push_back({ instr_t(I_AND + size - 1), a.line() });
        else if(a.type == AST::OP_BITWISE_OR)
            f.instrs.push_back({ instr_t(I_OR + size - 1), a.line() });
        else if(a.type == AST::OP_BITWISE_XOR)
            f.instrs.push_back({ instr_t(I_XOR + size - 1), a.line() });
        return;
    }

    case AST::ARRAY_INDEX:
    {
        // TODO: optimize the case where children[0] is an ident and children[1] is
        //       an integer constant, directly adjust offset given to REFL/REFG
        //       instruction instead of below code path
        auto const& t = a.children[0].comp_type.without_ref();
        codegen_expr(f, frame, a.children[0], true);
        codegen_expr(f, frame, a.children[1], false);
        codegen_convert(
            f, frame, a,
            t.is_prog ? TYPE_U24 : TYPE_U16,
            a.children[1].comp_type);
        size_t elem_size = t.children[0].prim_size;
        size_t size = t.prim_size;
        f.instrs.push_back({
            t.is_prog ? I_PIDX : I_AIDX, a.line(),
            (uint16_t)elem_size, (uint16_t)(size / elem_size) });
        frame.size -= t.is_prog ? 3 : 2;
        // if the child type is a reference, dereference it now
        // for example, a[i] where a is T&[N]
        if(t.children[0].is_ref())
            codegen_dereference(f, frame, a, t.children[0]);
        return;
    }

    case AST::OP_LOGICAL_AND:
    case AST::OP_LOGICAL_OR:
    {
        std::string sc_label = new_label(f);
        codegen_expr_logical(f, frame, a, sc_label);
        f.instrs.push_back({ I_NOP, 0, 0, 0, sc_label, true });
        return;
    }

    case AST::SPRITES:
    {
        std::string label = progdata_label();
        add_progdata(label, TYPE_SPRITES, a);
        f.instrs.push_back({ I_PUSHL, a.line(), 0, 0, label });
        frame.size += 3;
        return;
    }

    default:
        assert(false);
        errs.push_back({ "(codegen_expr) Unimplemented AST node", a.line_info });
        return;
    }

rvalue_error:
    errs.push_back({
        "Cannot create reference to expression",
        a.line_info });
}

void compiler_t::codegen_expr_compound(
        compiler_func_t& f, compiler_frame_t& frame,
        ast_node_t const& a, compiler_type_t const& type)
{
    assert(a.type == AST::COMPOUND_LITERAL);
    if(type.type == compiler_type_t::ARRAY)
    {
        const auto& t = type.children[0];
        size_t num_elems = type.prim_size / t.prim_size;
        if(num_elems != a.children.size())
        {
            errs.push_back({
                "Incorrect number of array elements in initializer",
                a.line_info });
            return;
        }
        bool ref = (t.type == compiler_type_t::REF);
        for(auto const& child : a.children)
        {
            if(child.type == AST::COMPOUND_LITERAL)
                codegen_expr_compound(f, frame, child, t);
            else
            {
                codegen_expr(f, frame, child, ref);
                codegen_convert(f, frame, child, t, child.comp_type);
            }
        }
    }
    else assert(false);
}

void compiler_t::codegen_expr_logical(
        compiler_func_t& f, compiler_frame_t& frame,
        ast_node_t const& a, std::string const& sc_label)
{
    if(a.children[0].type == a.type)
        codegen_expr_logical(f, frame, a.children[0], sc_label);
    else
        codegen_expr(f, frame, a.children[0], false);
    codegen_convert(f, frame, a.children[0], TYPE_BOOL, a.children[0].comp_type);
    // TODO: special versions of BZ and BNZ to replace following sequence
    //       of DUP; B[N]Z; POP
    f.instrs.push_back({ I_DUP, a.line() });
    f.instrs.push_back({
        a.type == AST::OP_LOGICAL_AND ? I_BZ : I_BNZ,
        a.line(), 0, 0, sc_label });
    frame.size -= 1;
    f.instrs.push_back({ I_POP });
    if(a.children[1].type == a.type)
        codegen_expr_logical(f, frame, a.children[1], sc_label);
    else
        codegen_expr(f, frame, a.children[1], false);
    codegen_convert(f, frame, a.children[1], TYPE_BOOL, a.children[1].comp_type);
}

}
