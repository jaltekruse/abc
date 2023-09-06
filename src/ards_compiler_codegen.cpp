#include "ards_compiler.hpp"

#include <sstream>
#include <assert.h>

namespace ards
{

compiler_local_t const* compiler_t::resolve_local(compiler_frame_t const& frame, ast_node_t const& n)
{
    std::string name(n.data);
    for(auto it = frame.scopes.rbegin(); it != frame.scopes.rend(); ++it)
    {
        auto jt = it->locals.find(name);
        if(jt != it->locals.end())
        {
            size_t offset = frame.size - jt->second.frame_offset;
            if(offset >= 256)
                errs.push_back({ "Stack frame exceeded 256 bytes", n.line_info });
            return &jt->second;
        }
    }
    return nullptr;
}

compiler_global_t const* compiler_t::resolve_global(ast_node_t const& n)
{
    std::string name(n.data);
    auto it = globals.find(name);
    if(it != globals.end())
        return &it->second;
    return nullptr;
}

compiler_lvalue_t compiler_t::resolve_lvalue(
        compiler_func_t const& f, compiler_frame_t const& frame,
        ast_node_t const& n)
{
    assert(n.type == AST::IDENT || n.type == AST::ARRAY_INDEX);
    (void)f;
    uint16_t line = n.line();
    if(n.type == AST::ARRAY_INDEX)
    {
        compiler_lvalue_t t{};
        t.type = n.comp_type;
        t.ref_ast = &n;
        t.line = line;
        return t;
    }
    if(auto* local = resolve_local(frame, n))
        return { local->type, false, uint8_t(frame.size - local->frame_offset), line };
    std::string name(n.data);
    if(auto* global = resolve_global(n))
        return { global->type, true, 0, line, name };
    errs.push_back({ "Undefined variable \"" + name + "\"", n.line_info });
    return {};
}

compiler_lvalue_t compiler_t::return_lvalue(compiler_func_t const& f, compiler_frame_t const& frame)
{
    compiler_lvalue_t t{};
    t.type = f.decl.return_type;
    t.stack_index = uint8_t(frame.size + t.type.prim_size);
    return t;
}

void compiler_t::codegen_return(compiler_func_t& f, compiler_frame_t& frame, ast_node_t const& n)
{
    // store return value
    if(!n.children.empty())
    {
        codegen_expr(f, frame, n.children[0], false);
        codegen_convert(f, frame, n, f.decl.return_type, n.children[0].comp_type);
        auto lvalue = return_lvalue(f, frame);
        codegen_store_lvalue(f, frame, lvalue);
    }
    else if(f.decl.return_type.prim_size != 0)
    {
        errs.push_back({ "Missing return value", n.line_info });
        return;
    }

    // pop remaining func args
    for(size_t i = 0; i < frame.size; ++i)
        f.instrs.push_back({ I_POP, n.line() });

    f.instrs.push_back({ I_RET, n.line() });
}

void compiler_t::codegen_function(compiler_func_t& f)
{
    compiler_frame_t frame{};

    frame.push();

    // add func args to scope here
    assert(f.arg_names.size() == f.decl.arg_types.size());
    auto& scope = frame.scopes.back();
    for(size_t i = 0; i < f.arg_names.size(); ++i)
    {
        auto const& name = f.arg_names[i];
        auto const& type = f.decl.arg_types[i];
        auto& local = scope.locals[name];
        size_t size = type.prim_size;
        local.frame_offset = frame.size;
        local.type = type;
        scope.size += size;
        frame.size += size;
    }

    frame.push();

    codegen(f, frame, f.block);

    // no need to call codegen_return here
    // all function blocks are guaranteed to end with a return statement
    assert(f.block.children.back().type == AST::RETURN_STMT);
}

std::string compiler_t::new_label(compiler_func_t& f)
{
    std::ostringstream ss;
    ss << f.label_count;
    f.label_count += 1;
    std::string label = "$L_" + f.name + "_" + ss.str();
    return label;
}

std::string compiler_t::codegen_label(compiler_func_t& f)
{
    std::string label = new_label(f);
    f.instrs.push_back({ I_NOP, 0, 0, 0, label, true});
    return label;
}

void compiler_t::codegen(compiler_func_t& f, compiler_frame_t& frame, ast_node_t& a)
{
    if(!errs.empty()) return;
    auto prev_size = frame.size;
    switch(a.type)
    {
    case AST::EMPTY_STMT:
        break;
    case AST::IF_STMT:
    {
        assert(a.children.size() == 3);
        type_annotate(a.children[0], frame);

        if(a.children[0].type == AST::INT_CONST && a.children[0].value != 0)
        {
            // only codegen main statement body
            codegen(f, frame, a.children[1]);
            break;
        }

        if(a.children[0].type == AST::INT_CONST && a.children[0].value == 0)
        {
            // only codegen else statement
            codegen(f, frame, a.children[2]);
            break;
        }

        codegen_expr(f, frame, a.children[0], false);
        // TODO: unnecessary for a.children[0].comp_type.prim_size == 1
        codegen_convert(f, frame, a, TYPE_BOOL, a.children[0].comp_type);
        size_t cond_index = f.instrs.size();
        f.instrs.push_back({ I_BZ, a.line() });
        frame.size -= 1;
        codegen(f, frame, a.children[1]);
        size_t jmp_index = f.instrs.size();
        if(a.children[2].type != AST::EMPTY_STMT)
            f.instrs.push_back({ I_JMP, a.line() });
        auto else_label = codegen_label(f);
        f.instrs[cond_index].label = else_label;
        if(a.children[2].type != AST::EMPTY_STMT)
        {
            codegen(f, frame, a.children[2]);
            auto end_label = codegen_label(f);
            f.instrs[jmp_index].label = end_label;
        }
        break;
    }
    case AST::WHILE_STMT:
    {
        assert(a.children.size() == 2);
        type_annotate(a.children[0], frame);
        if(a.children[0].type == AST::INT_CONST && a.children[0].value == 0)
            break;
        bool nocond = (a.children[0].type == AST::INT_CONST && a.children[0].value != 0);
        auto start = codegen_label(f);
        size_t cond_index = size_t(-1);
        if(!nocond)
        {
            codegen_expr(f, frame, a.children[0], false);
            // TODO: unnecessary for a.children[0].comp_type.prim_size == 1
            codegen_convert(f, frame, a, TYPE_BOOL, a.children[0].comp_type);
            cond_index = f.instrs.size();
            f.instrs.push_back({ I_BZ, a.children[0].line() });
            frame.size -= 1;
        }
        codegen(f, frame, a.children[1]);
        f.instrs.push_back({ I_JMP, a.children[0].line(), 0, 0, start });
        if(!nocond)
        {
            auto end = codegen_label(f);
            f.instrs[cond_index].label = end;
        }
        break;
    }
    case AST::BLOCK:
    {
        size_t block_prev_size = frame.size;
        frame.push();
        for(auto& child : a.children)
            codegen(f, frame, child);
        // don't need to pop locals after final return statement
        if(&a != &f.block)
        {
            uint16_t line = f.instrs.empty() ? 0 : f.instrs.back().line;
            for(size_t i = 0; i < frame.scopes.back().size; ++i)
                f.instrs.push_back({ I_POP, line });
        }
        frame.pop();
        (void)block_prev_size;
        assert(!errs.empty() || frame.size == block_prev_size);
        break;
    }
    case AST::RETURN_STMT:
    {
        if(!a.children.empty())
            type_annotate(a.children[0], frame, f.decl.return_type.prim_size);
        codegen_return(f, frame, a);
        break;
    }
    case AST::EXPR_STMT:
    {
        size_t expr_prev_size = frame.size;
        assert(a.children.size() == 1);
        type_annotate(a.children[0], frame);
        codegen_expr(f, frame, a.children[0], false);
        for(size_t i = expr_prev_size; i < frame.size; ++i)
            f.instrs.push_back({ I_POP, a.line() });
        frame.size = expr_prev_size;
        break;
    }
    case AST::DECL_STMT:
    {
        // add local var to frame
        assert(a.children.size() == 2 || a.children.size() == 3);
        assert(a.children[1].type == AST::IDENT);
        if(!check_identifier(a.children[1])) return;
        auto type = resolve_type(a.children[0]);
        if(type.prim_size == 0)
        {
            errs.push_back({
                "Local variable \"" + std::string(a.children[1].data) + "\" has zero size",
                a.line_info });
            return;
        }
        if(type.prim_size >= 256)
        {
            errs.push_back({
                "Local variable \"" + std::string(a.children[1].data) + "\" is too large",
                a.line_info });
            return;
        }
        auto& scope = frame.scopes.back();
        auto& local = scope.locals[std::string(a.children[1].data)];
        local.type = type;
        local.frame_offset = frame.size;
        if(type.is_constexpr)
        {
            type_annotate(a.children[2], frame);
            if(a.children[2].type == AST::INT_CONST)
            {
                local.is_constexpr = true;
                local.value = a.children[2].value;
            }
            else
            {
                errs.push_back({
                    "\"" + std::string(a.children[2].data) +
                    "\" is not a constant expression",
                    a.children[2].line_info });
                return;
            }
        }
        else if(a.children.size() == 3)
        {
            type_annotate(a.children[2], frame);
            // TODO: handle array reference initializers
            assert(type.type != compiler_type_t::ARRAY_REF);
            bool ref = (type.type == compiler_type_t::REF);
            codegen_expr(f, frame, a.children[2], ref);
            if(!errs.empty()) return;
            if(ref)
            {
                if(type.without_ref() != a.children[2].comp_type.without_ref())
                {
                    errs.push_back({
                        "Incorrect type for reference \"" + std::string(a.children[1].data) + "\"",
                        a.line_info });
                    return;
                }
            }
            else
                codegen_convert(f, frame, a, type, a.children[2].comp_type);
        }
        else
        {
            frame.size += type.prim_size;
            for(size_t i = 0; i < type.prim_size; ++i)
                f.instrs.push_back({ I_PUSH, a.line(), 0 });
        }
        if(!type.is_constexpr)
            scope.size += type.prim_size;
        break;
    }
    default:
        assert(false);
        errs.push_back({ "(codegen) Unimplemented AST node", a.line_info });
        return;
    }
    (void)prev_size;
    if(a.type != AST::DECL_STMT && errs.empty())
        assert(frame.size == prev_size);
}

void compiler_t::codegen_store_lvalue(
    compiler_func_t& f, compiler_frame_t& frame, compiler_lvalue_t const& lvalue)
{
    assert(lvalue.type.prim_size < 256);
    if(!errs.empty()) return;
    if(lvalue.ref_ast)
    {
        // assign to reference which needs to be constructed now
        // happens for statements like: x[2] = 42;
        auto size = lvalue.type.children[0].prim_size;
        assert(size < 256);
        codegen_expr(f, frame, *lvalue.ref_ast, false);
        f.instrs.push_back({ I_SETRN, lvalue.line, (uint8_t)size });
        frame.size -= 2;
        frame.size -= size;
    }
    else if(lvalue.type.type == compiler_type_t::REF)
    {
        // assign to reference variable
        auto size = lvalue.type.children[0].prim_size;
        assert(size < 256);
        assert(lvalue.type.prim_size == 2); // ref size is 2
        f.instrs.push_back({ I_PUSH, lvalue.line, 2 });
        f.instrs.push_back({ I_GETLN, lvalue.line, (uint8_t)(lvalue.stack_index) });
        f.instrs.push_back({ I_SETRN, lvalue.line, (uint8_t)size });
        frame.size -= size;
    }
    else if(lvalue.is_global)
    {
        f.instrs.push_back({ I_PUSH, lvalue.line, (uint8_t)lvalue.type.prim_size });
        f.instrs.push_back({ I_SETGN, lvalue.line, 0, 0, lvalue.global_name });
        frame.size -= lvalue.type.prim_size;
    }
    else
    {
        f.instrs.push_back({ I_PUSH, lvalue.line, (uint8_t)lvalue.type.prim_size });
        f.instrs.push_back({ I_SETLN, lvalue.line, (uint8_t)(lvalue.stack_index - lvalue.type.prim_size) });
        frame.size -= lvalue.type.prim_size;
    }
}

void compiler_t::codegen_convert(
    compiler_func_t& f, compiler_frame_t& frame,
    ast_node_t const& n,
    compiler_type_t const& orig_to, compiler_type_t const& orig_from)
{
    if(!errs.empty()) return;

    auto* pfrom = &orig_from;
    assert(orig_from.type != compiler_type_t::ARRAY_REF);
    assert(orig_to.type != compiler_type_t::ARRAY_REF);
    if(orig_from.type == compiler_type_t::ARRAY)
    {
        if(orig_to.type != compiler_type_t::ARRAY)
        {
            errs.push_back({ "Cannot convert array to non-array", n.line_info });
            return;
        }
        if(orig_from.children[0] != orig_to.children[0])
        {
            errs.push_back({ "Cannot implicitly convert array elements", n.line_info });
            return;
        }
    }
    if(pfrom->type == compiler_type_t::REF)
    {
        pfrom = &pfrom->children[0];
        codegen_dereference(f, frame, n, *pfrom);
    }
    auto const& from = *pfrom;
    assert(from.prim_size != 0);

    auto* pto = &orig_to;
    if(pto->type == compiler_type_t::REF)
        pto = &pto->children[0];
    auto const& to = *pto;

    if(to == from) return;
    if(to.is_bool)
    {
        if(from.is_bool) return;
        int num = int(from.prim_size - 1);
        frame.size -= num;
        static_assert(I_BOOL2 == I_BOOL + 1);
        static_assert(I_BOOL3 == I_BOOL + 2);
        static_assert(I_BOOL4 == I_BOOL + 3);
        f.instrs.push_back({ instr_t(I_BOOL + num), n.line() });
        return;
    }
    if(to.prim_size == from.prim_size) return;
    if(to.prim_size < from.prim_size)
    {
        size_t num = from.prim_size - to.prim_size;
        frame.size -= num;
        for(size_t i = 0; i < num; ++i)
            f.instrs.push_back({ I_POP, n.line() });
    }
    if(to.prim_size > from.prim_size)
    {
        size_t num = to.prim_size - from.prim_size;
        frame.size += num;
        compiler_instr_t instr;
        if(from.is_signed)
            instr = { I_SEXT, n.line() };
        else
            instr = { I_PUSH, n.line(), 0 };
        for(size_t i = 0; i < num; ++i)
            f.instrs.push_back(instr);
    }
}

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
            f.instrs.push_back({ I_PUSH, a.line(), (uint8_t)x});
        return;
    }

    case AST::IDENT:
    {
        std::string name(a.data);
        if(auto* local = resolve_local(frame, a))
        {
            assert(!local->is_constexpr);
            uint8_t offset = (uint8_t)(frame.size - local->frame_offset);
            uint8_t size = (uint8_t)local->type.prim_size;
            if(ref && local->type.type != compiler_type_t::REF)
            {
                f.instrs.push_back({ I_REFL, a.line(), offset });
                frame.size += 2;
                return;
            }
            f.instrs.push_back({ I_PUSH, a.line(), size });
            f.instrs.push_back({ I_GETLN, a.line(), offset });
            frame.size += (uint8_t)local->type.prim_size;
            return;
        }
        if(auto* global = resolve_global(a))
        {
            assert(!global->is_constexpr);
            if(ref)
            {
                f.instrs.push_back({ I_REFG, a.line(), 0, 0, global->name});
                frame.size += 2;
                return;
            }
            assert(global->type.prim_size < 256);
            frame.size += (uint8_t)global->type.prim_size;
            f.instrs.push_back({ I_PUSH, a.line(), (uint8_t)global->type.prim_size });
            f.instrs.push_back({ I_GETGN, a.line(), 0, 0, global->name });
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
                    "Cannot create reference to incompatible type: " +
                    std::string(expr.data),
                    expr.line_info });
                return;
            }
            codegen_expr(f, frame, expr, tref);
            if(!tref)
                codegen_convert(f, frame, a, type, expr.comp_type);
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
        if(a.children[0].comp_type.type == compiler_type_t::ARRAY_REF ||
            a.children[0].comp_type.type == compiler_type_t::ARRAY)
        {
            // TODO: nice error message
            assert(false);
        }
        codegen_expr(f, frame, a.children[1], false);
        codegen_convert(f, frame, a, a.children[0].comp_type, a.children[1].comp_type);

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
        if((lvalue.type.type != compiler_type_t::PRIM &&
            lvalue.type.type != compiler_type_t::REF) ||
           (lvalue.type.type == compiler_type_t::REF &&
            lvalue.type.children[0].type != compiler_type_t::PRIM))
        {
            errs.push_back({ "Assignment to non-primitive type", a.line_info });
            return;
        }
        codegen_store_lvalue(f, frame, lvalue);
        return;
    }

    case AST::OP_EQUALITY:
    case AST::OP_RELATIONAL:
    {
        if(ref) goto rvalue_error;
        assert(a.children.size() == 2);
        assert(a.children[0].comp_type == a.children[1].comp_type);
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
        codegen_expr(f, frame, a.children[0], true);
        codegen_expr(f, frame, a.children[1], false);
        codegen_convert(f, frame, a, TYPE_U16, a.children[1].comp_type);
        auto const& t = a.children[0].comp_type.without_ref();
        size_t elem_size = t.children[0].prim_size;
        size_t size = t.prim_size;
        f.instrs.push_back({ I_AIDX, a.line(), (uint16_t)elem_size, (uint16_t)(size / elem_size) });
        frame.size -= 2;
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

    default:
        assert(false); 
        errs.push_back({ "(codegen_expr) Unimplemented AST node", a.line_info });
        return;
    }

rvalue_error:
    errs.push_back({
        "Cannot create reference to lvalue expression: \"" + std::string(a.data) + "\"",
        a.line_info });
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

void compiler_t::codegen_dereference(
    compiler_func_t& f, compiler_frame_t& frame,
    ast_node_t const& n, compiler_type_t const& t)
{
    size_t size = t.prim_size;
    if(t.prim_size >= 256)
    {
        errs.push_back({ "Expression too large (256 bytes or more)", n.line_info });
        return;
    }
    f.instrs.push_back({ I_GETRN, n.line(), (uint8_t)size });
    frame.size -= 2;
    frame.size += size;
}

}
