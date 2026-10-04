#include "quarzum.h"

static uint64_t label_count = 0;
static HashMap strlit_table;

static Node* pending_global_init[256];
static int pending_global_init_count = 0;

// Emits the .rodata literal for a string (if not already emitted) and returns
// its label id. The caller is responsible for being inside a .data section.
static uint64_t get_strlit_label(FILE* output_file, const char* str, int length){
    (void)length;
    uint64_t id = (uint64_t)(uintptr_t)hashmap_get(&strlit_table, str);
    if(id == 0){
        id = label_count++;
        hashmap_put(&strlit_table, str, (void*)(uintptr_t)(id + 1));
        fprintf(output_file, "    .section .rodata\n");
        fprintf(output_file, ".Lstrlit_%lu:\n", id);
        fprintf(output_file, "    .asciz \"%s\"\n", str);
        fprintf(output_file, "    .section .data\n");
    } else {
        id = id - 1;
    }
    return id;
}

#define MAX_BREAK_DEPTH 64

typedef struct {
    int stack_size;
    bool is_main;
    uint64_t break_labels[MAX_BREAK_DEPTH];
    uint64_t continue_labels[MAX_BREAK_DEPTH];
    int break_depth;
} CodegenState;

static bool node_is_float(Node* node){
    if(!node || !node->ty) return false;
    return is_float(node->ty);
}

static bool node_is_string(Node* node){
    if(!node || !node->ty) return false;
    return is_string_type(node->ty);
}

static bool node_is_string_like(Node* node){
    if(!node || !node->ty) return false;
    return node->ty->kind == TY_STRING || node->ty->kind == TY_ARRAY;
}

static void gen_int_to_float(Node* node, FILE* output_file, Type* target){
    if(target->kind == TY_FLOAT32){
        fprintf(output_file, "    cvtsi2ss xmm0, rax\n");
        fprintf(output_file, "    cvtss2sd xmm0, xmm0\n");
    } else {
        fprintf(output_file, "    cvtsi2sd xmm0, rax\n");
    }
}

static void gen_float_to_int(Node* node, FILE* output_file, Type* from){
    fprintf(output_file, "    cvttsd2si rax, xmm0\n");
}

static void gen_expr(Node* node, FILE* output_file, CodegenState* state);

static void gen_string_compare(bool is_eq, Node* lhs, Node* rhs,
                               FILE* output_file, CodegenState* state){
    uint64_t id = label_count++;

    gen_expr(lhs, output_file, state);
    fprintf(output_file, "    push rax\n");
    fprintf(output_file, "    push rdx\n");
    state->stack_size += 16;

    gen_expr(rhs, output_file, state);
    fprintf(output_file, "    mov rcx, rax\n");
    fprintf(output_file, "    mov r8, rdx\n");

    fprintf(output_file, "    mov rax, [rsp]\n");
    fprintf(output_file, "    cmp rax, r8\n");
    fprintf(output_file, "    jne .Lstr_neq_%lu\n", id);

    fprintf(output_file, "    mov rax, [rsp+8]\n");
    fprintf(output_file, "    cmp rax, rcx\n");
    fprintf(output_file, "    je .Lstr_eq_%lu\n", id);

    fprintf(output_file, "    test r8, r8\n");
    fprintf(output_file, "    je .Lstr_eq_%lu\n", id);
    fprintf(output_file, "    mov rsi, [rsp+8]\n");
    fprintf(output_file, "    mov rdi, rcx\n");
    fprintf(output_file, ".Lstr_loop_%lu:\n", id);
    fprintf(output_file, "    movzx rax, byte ptr [rsi]\n");
    fprintf(output_file, "    movzx rdx, byte ptr [rdi]\n");
    fprintf(output_file, "    cmp al, dl\n");
    fprintf(output_file, "    jne .Lstr_neq_%lu\n", id);
    fprintf(output_file, "    inc rsi\n");
    fprintf(output_file, "    inc rdi\n");
    fprintf(output_file, "    dec r8\n");
    fprintf(output_file, "    jnz .Lstr_loop_%lu\n", id);

    fprintf(output_file, ".Lstr_eq_%lu:\n", id);
    fprintf(output_file, "    mov rax, %d\n", is_eq ? 1 : 0);
    fprintf(output_file, "    jmp .Lstr_done_%lu\n", id);
    fprintf(output_file, ".Lstr_neq_%lu:\n", id);
    fprintf(output_file, "    mov rax, %d\n", is_eq ? 0 : 1);
    fprintf(output_file, ".Lstr_done_%lu:\n", id);
    fprintf(output_file, "    add rsp, 16\n");
    state->stack_size -= 16;
}

static void gen_expr(Node* node, FILE* output_file, CodegenState* state){
    if(!node){
        fprintf(stderr, "codegen error: NULL expression node\n");
        exit(1);
    }
    if(node->type == ND_INTLIT){
        fprintf(output_file, "    mov rax, %lld\n", node->intlit.value);
        return;
    }
    if(node->type == ND_FLOATLIT){
        uint64_t id = label_count++;
        fprintf(output_file, "    .section .rodata\n");
        fprintf(output_file, ".Lfloat_%lu: .double %Lg\n", id, node->floatlit.value);
        fprintf(output_file, "    .section .text\n");
        fprintf(output_file, "    movsd xmm0, [rip + .Lfloat_%lu]\n", id);
        return;
    }
    if(node->type == ND_STRLIT){
        uint64_t id = (uint64_t)(uintptr_t)hashmap_get(&strlit_table, node->strlit.value);
        if(id == 0){
            id = label_count++;
            hashmap_put(&strlit_table, node->strlit.value, (void*)(uintptr_t)(id + 1));
            fprintf(output_file, "    .section .rodata\n");
            fprintf(output_file, ".Lstrlit_%lu:\n", id);
            fprintf(output_file, "    .asciz \"%s\"\n", node->strlit.value);
            fprintf(output_file, "    .section .text\n");
        } else {
            id = id - 1;
        }
        fprintf(output_file, "    lea rax, [rip + .Lstrlit_%lu]\n", id);
        fprintf(output_file, "    mov rdx, %d\n", node->strlit.length);
        return;
    }
    if(node->type == ND_BOOLLIT){
        fprintf(output_file, "    mov rax, %d\n", node->boollit.value ? 1 : 0);
        return;
    }
    if(node->type == ND_THIS){
        fprintf(output_file, "    mov rax, [rbp+16]\n");
        return;
    }
    if(node->type == ND_NULL){
        fprintf(output_file, "    mov rax, 0\n");
        return;
    }
    // `pass` produces no code: it is the void literal / nop.
    if(node->type == ND_PASS){
        return;
    }
    if(node->type == ND_NEW){
        // Allocate memory for the class instance using mmap syscall (like ND_ALLOC)
        StructDef* sdef = struct_table_lookup(node->new_expr.class_name);
        if(!sdef){ fprintf(stderr, "CODE GEN ERROR: struct_table_lookup('%s') failed\n", node->new_expr.class_name); exit(1); }
        int alloc_size = sdef->total_size + 8;

        fprintf(output_file, "    mov rax, %d\n", alloc_size);
        fprintf(output_file, "    add rax, 8\n");
        fprintf(output_file, "    push rax\n");
        fprintf(output_file, "    mov rdi, 0\n");
        fprintf(output_file, "    mov rsi, rax\n");
        fprintf(output_file, "    mov rdx, 3\n");
        fprintf(output_file, "    mov r10, 0x22\n");
        fprintf(output_file, "    mov r8, -1\n");
        fprintf(output_file, "    xor r9, r9\n");
        fprintf(output_file, "    mov rax, 9\n");
        fprintf(output_file, "    syscall\n");
        fprintf(output_file, "    pop rcx\n");
        fprintf(output_file, "    sub rcx, 8\n");
        fprintf(output_file, "    mov [rax], rcx\n");
        fprintf(output_file, "    add rax, 8\n");
        // rax = pointer to allocated memory (this)

        // Save the object pointer
        fprintf(output_file, "    push rax\n");
        state->stack_size += 8;

        // Push constructor args in reverse order
        int total_bytes = 8; // 'this' (re-pushed object pointer)
        if(node->new_expr.args){
            for(int i = (int)node->new_expr.args->length - 1; i >= 0; i--){
                gen_expr(node->new_expr.args->nodes[i], output_file, state);
                if(node_is_float(node->new_expr.args->nodes[i])){
                    fprintf(output_file, "    movq rax, xmm0\n");
                }
                if(node->new_expr.args->nodes[i]->ty &&
                   node->new_expr.args->nodes[i]->ty->kind == TY_STRUCT){
                    int asize = node->new_expr.args->nodes[i]->ty->size;
                    fprintf(output_file, "    sub rsp, %d\n", asize);
                    fprintf(output_file, "    mov rsi, rax\n");
                    fprintf(output_file, "    mov rdi, rsp\n");
                    for(int b = 0; b < asize; b += 8){
                        fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                        fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                    }
                    total_bytes += asize;
                    state->stack_size += asize;
                } else if(node_is_string_like(node->new_expr.args->nodes[i])){
                    fprintf(output_file, "    push rdx\n");
                    fprintf(output_file, "    push rax\n");
                    total_bytes += 16;
                    state->stack_size += 16;
                } else {
                    fprintf(output_file, "    push rax\n");
                    total_bytes += 8;
                    state->stack_size += 8;
                }
            }
        }
        // Push this pointer on top
        fprintf(output_file, "    mov rax, [rsp+%d]\n", total_bytes - 8);
        fprintf(output_file, "    push rax\n");
        state->stack_size += 8;

        fprintf(output_file, "    call %s\n", node->new_expr.mangled_name);

        // Clean up args from stack (this + constructor args)
        fprintf(output_file, "    add rsp, %d\n", total_bytes);
        state->stack_size -= total_bytes;

        // Restore the object pointer
        fprintf(output_file, "    pop rax\n");
        state->stack_size -= 8;
        return;
    }
    if(node->type == ND_METHODCALL){
        // Push args in reverse order (this is pushed last so it's on top = first param)
        int total_bytes = 8; // 'this'
        if(node->methodcall.args){
            for(int i = (int)node->methodcall.args->length - 1; i >= 0; i--){
                if(getenv("QZ_DEBUG_MC") && node->methodcall.args->nodes[i]->type == ND_VARREF){
                    Node* an = node->methodcall.args->nodes[i];
                    fprintf(stderr, "[mc] arg varref '%s' kind=%d size=%ld call=%s.%s idx=%d\n", an->varref.name,
                            an->ty ? an->ty->kind : -1, an->ty ? an->ty->size : -1,
                            node->methodcall.class_name ? node->methodcall.class_name : "?",
                            node->methodcall.method ? node->methodcall.method : "?", i);
                }
                gen_expr(node->methodcall.args->nodes[i], output_file, state);
                if(node_is_float(node->methodcall.args->nodes[i])){
                    fprintf(output_file, "    movq rax, xmm0\n");
                }
                if(node->methodcall.args->nodes[i]->ty &&
                   node->methodcall.args->nodes[i]->ty->kind == TY_STRUCT){
                    int asize = node->methodcall.args->nodes[i]->ty->size;
                    fprintf(output_file, "    sub rsp, %d\n", asize);
                    fprintf(output_file, "    mov rsi, rax\n");
                    fprintf(output_file, "    mov rdi, rsp\n");
                    for(int b = 0; b < asize; b += 8){
                        fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                        fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                    }
                    total_bytes += asize;
                    state->stack_size += asize;
                } else if(node_is_string_like(node->methodcall.args->nodes[i])){
                    fprintf(output_file, "    push rdx\n");
                    fprintf(output_file, "    push rax\n");
                    total_bytes += 16;
                    state->stack_size += 16;
                } else {
                    fprintf(output_file, "    push rax\n");
                    total_bytes += 8;
                    state->stack_size += 8;
                }
            }
        }
        // Push this pointer last (becomes first param at [rbp+16])
        gen_expr(node->methodcall.object, output_file, state);
        fprintf(output_file, "    push rax\n");
        state->stack_size += 8;

        fprintf(output_file, "    call %s_%s\n", node->methodcall.class_name, node->methodcall.method);

        // Clean up args from stack
        fprintf(output_file, "    add rsp, %d\n", total_bytes);
        state->stack_size -= total_bytes;
        return;
    }
    if(node->type == ND_VARREF){
        if(node->varref.is_global){
            if(node->ty && is_float(node->ty)){
                if(node->ty->kind == TY_FLOAT64)
                    fprintf(output_file, "    movsd xmm0, [rip + %s]\n", node->varref.name);
                else {
                    fprintf(output_file, "    movss xmm0, [rip + %s]\n", node->varref.name);
                    fprintf(output_file, "    cvtss2sd xmm0, xmm0\n");
                }
            } else if(node->ty && node->ty->kind == TY_STRING){
                fprintf(output_file, "    lea rax, [rip + %s]\n", node->varref.name);
                fprintf(output_file, "    mov rdx, [rip + %s_len]\n", node->varref.name);
            } else if(node->ty && node->ty->kind == TY_ARRAY){
                fprintf(output_file, "    mov rax, [rip + %s]\n", node->varref.name);
                fprintf(output_file, "    mov rdx, [rip + %s + 8]\n", node->varref.name);
            } else if(node->ty && node->ty->kind == TY_STRUCT){
                fprintf(output_file, "    lea rax, [rip + %s]\n", node->varref.name);
            } else {
                fprintf(output_file, "    mov rax, [rip + %s]\n", node->varref.name);
            }
        } else {
            if(node->ty && is_float(node->ty)){
                if(node->ty->kind == TY_FLOAT64)
                    fprintf(output_file, "    movsd xmm0, [rbp%+d]\n", node->varref.offset);
                else {
                    fprintf(output_file, "    movss xmm0, [rbp%+d]\n", node->varref.offset);
                    fprintf(output_file, "    cvtss2sd xmm0, xmm0\n");
                }
            } else if(node->ty && node->ty->kind == TY_STRUCT){
                fprintf(output_file, "    lea rax, [rbp%+d]\n", node->varref.offset);
            } else if(node->ty && (node->ty->kind == TY_CHAR || node->ty->kind == TY_BOOL)){
                fprintf(output_file, "    movzx eax, byte ptr [rbp%+d]\n", node->varref.offset);
            } else {
                fprintf(output_file, "    mov rax, [rbp%+d]\n", node->varref.offset);
            }
            if(node->ty && (node->ty->kind == TY_STRING || node->ty->kind == TY_ARRAY)){
                fprintf(output_file, "    mov rdx, [rbp%+d]\n", node->varref.offset + 8);
            }
        }
        return;
    }
    if(node->type == ND_INDEX){
        if(node->index_expr.base->type == ND_VARREF){
            if(node->index_expr.base->varref.is_global){
                if(node->index_expr.base->ty && node->index_expr.base->ty->kind == TY_STRING)
                    fprintf(output_file, "    lea rax, [rip + %s]\n", node->index_expr.base->varref.name);
                else
                    fprintf(output_file, "    mov rax, [rip + %s]\n", node->index_expr.base->varref.name);
            } else {
                fprintf(output_file, "    mov rax, [rbp%+d]\n", node->index_expr.base->varref.offset);
            }
        } else {
            gen_expr(node->index_expr.base, output_file, state);
        }
        fprintf(output_file, "    push rax\n");
        state->stack_size += 8;
        gen_expr(node->index_expr.index, output_file, state);
        if(node->index_expr.elem_size > 1){
            fprintf(output_file, "    imul rax, %d\n", node->index_expr.elem_size);
        }
        fprintf(output_file, "    mov rcx, rax\n");
        fprintf(output_file, "    pop rax\n");
        state->stack_size -= 8;
        fprintf(output_file, "    add rax, rcx\n");
        if(node->index_expr.is_string){
            fprintf(output_file, "    mov rdx, [rax+8]\n");
            fprintf(output_file, "    mov rax, [rax]\n");
        } else if(node->index_expr.base && node->index_expr.base->ty &&
                  is_string_type(node->index_expr.base->ty)){
            fprintf(output_file, "    movzx rax, byte ptr [rax]\n");
        } else if(node->index_expr.base && node->index_expr.base->ty &&
                  node->index_expr.base->ty->base &&
                  node->index_expr.base->ty->base->kind == TY_STRUCT){
            fprintf(output_file, "    lea rax, [rax]\n");
        } else {
            fprintf(output_file, "    mov rax, [rax]\n");
        }
        return;
    }
    if(node->type == ND_MEMBER){
        if(node->member.is_class){
            // Class field access through pointer: load pointer, then access field
            if(node->member.base->type == ND_THIS){
                fprintf(output_file, "    mov rax, [rbp+16]\n");
            } else {
                gen_expr(node->member.base, output_file, state);
            }
            if(node->ty && is_float(node->ty)){
                if(node->ty->kind == TY_FLOAT64)
                    fprintf(output_file, "    movsd xmm0, [rax+%d]\n", node->member.field_offset);
                else {
                    fprintf(output_file, "    movss xmm0, [rax+%d]\n", node->member.field_offset);
                    fprintf(output_file, "    cvtss2sd xmm0, xmm0\n");
                }
            } else if(node->ty && node->ty->kind == TY_STRUCT){
                fprintf(output_file, "    lea rax, [rax+%d]\n", node->member.field_offset);
            } else {
                fprintf(output_file, "    lea rcx, [rax+%d]\n", node->member.field_offset);
                if(node->ty && (node->ty->kind == TY_BOOL || node->ty->kind == TY_CHAR))
                    fprintf(output_file, "    movzx eax, byte ptr [rcx]\n");
                else
                    fprintf(output_file, "    mov rax, [rcx]\n");
                if(node->ty && is_string_type(node->ty)){
                    fprintf(output_file, "    mov rdx, [rcx+8]\n");
                }
            }
        } else if(node->member.is_struct){
            // Struct value: base evaluates to a pointer to the struct
            gen_expr(node->member.base, output_file, state);
            if(node->ty && node->ty->kind == TY_STRUCT){
                fprintf(output_file, "    lea rax, [rax+%d]\n", node->member.field_offset);
            } else if(node->ty && is_float(node->ty)){
                if(node->ty->kind == TY_FLOAT64)
                    fprintf(output_file, "    movsd xmm0, [rax+%d]\n", node->member.field_offset);
                else {
                    fprintf(output_file, "    movss xmm0, [rax+%d]\n", node->member.field_offset);
                    fprintf(output_file, "    cvtss2sd xmm0, xmm0\n");
                }
            } else {
                fprintf(output_file, "    lea rcx, [rax+%d]\n", node->member.field_offset);
                if(node->ty && (node->ty->kind == TY_BOOL || node->ty->kind == TY_CHAR))
                    fprintf(output_file, "    movzx eax, byte ptr [rcx]\n");
                else
                    fprintf(output_file, "    mov rax, [rcx]\n");
                if(node->ty && is_string_type(node->ty)){
                    fprintf(output_file, "    mov rdx, [rcx+8]\n");
                }
            }
        } else if(strcmp(node->member.field_name, "pointer") == 0 ||
                  strcmp(node->member.field_name, "ptr") == 0){
            gen_expr(node->member.base, output_file, state);
        } else if(strcmp(node->member.field_name, "length") == 0){
            gen_expr(node->member.base, output_file, state);
            fprintf(output_file, "    mov rax, rdx\n");
        }
        return;
    }
    if(node->type == ND_STRING_EQ || node->type == ND_STRING_NE){
        gen_string_compare(node->type == ND_STRING_EQ,
                           node->string_cmp.lhs, node->string_cmp.rhs,
                           output_file, state);
        return;
    }
    if(node->type == ND_BINARY_EXPR){
        if(node->binary_expr.is_string_cmp &&
           (node->binary_expr.op == OP_EQ || node->binary_expr.op == OP_NE)){
            gen_string_compare(node->binary_expr.op == OP_EQ,
                               node->binary_expr.lhs, node->binary_expr.rhs,
                               output_file, state);
            return;
        }
        bool both_strings = node_is_string(node->binary_expr.lhs) && node_is_string(node->binary_expr.rhs);
        if(both_strings && node->binary_expr.op == OP_ADD){
            uint64_t id = label_count++;

            gen_expr(node->binary_expr.lhs, output_file, state);
            fprintf(output_file, "    mov r8, rax\n");
            fprintf(output_file, "    mov r9, rdx\n");
            fprintf(output_file, "    push r8\n");
            fprintf(output_file, "    push r9\n");
            state->stack_size += 16;

            gen_expr(node->binary_expr.rhs, output_file, state);
            fprintf(output_file, "    mov r10, rax\n");
            fprintf(output_file, "    mov r11, rdx\n");
            fprintf(output_file, "    pop r9\n");
            fprintf(output_file, "    pop r8\n");
            state->stack_size -= 16;

            fprintf(output_file, "    sub rsp, 56\n");
            fprintf(output_file, "    mov [rsp], r8\n");
            fprintf(output_file, "    mov [rsp+8], r9\n");
            fprintf(output_file, "    mov [rsp+16], r10\n");
            fprintf(output_file, "    mov [rsp+24], r11\n");

            fprintf(output_file, "    mov rax, r9\n");
            fprintf(output_file, "    add rax, r11\n");
            fprintf(output_file, "    mov [rsp+32], rax\n");
            fprintf(output_file, "    add rax, 8\n");
            fprintf(output_file, "    mov [rsp+40], rax\n");

            fprintf(output_file, "    mov rdi, 0\n");
            fprintf(output_file, "    mov rsi, rax\n");
            fprintf(output_file, "    mov rdx, 3\n");
            fprintf(output_file, "    mov r10, 0x22\n");
            fprintf(output_file, "    mov r8, -1\n");
            fprintf(output_file, "    xor r9, r9\n");
            fprintf(output_file, "    mov rax, 9\n");
            fprintf(output_file, "    syscall\n");

            fprintf(output_file, "    mov rdi, [rsp+32]\n");
            fprintf(output_file, "    mov [rax], rdi\n");
            fprintf(output_file, "    add rax, 8\n");

            fprintf(output_file, "    mov [rsp+48], rax\n");
            fprintf(output_file, "    mov rsi, [rsp]\n");
            fprintf(output_file, "    mov rcx, [rsp+8]\n");
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, ".Lstrcat_cp1_%lu:\n", id);
            fprintf(output_file, "    test rcx, rcx\n");
            fprintf(output_file, "    je .Lstrcat_cp1d_%lu\n", id);
            fprintf(output_file, "    movzx rdx, byte ptr [rsi]\n");
            fprintf(output_file, "    mov [rdi], dl\n");
            fprintf(output_file, "    inc rsi\n");
            fprintf(output_file, "    inc rdi\n");
            fprintf(output_file, "    dec rcx\n");
            fprintf(output_file, "    jmp .Lstrcat_cp1_%lu\n", id);
            fprintf(output_file, ".Lstrcat_cp1d_%lu:\n", id);

            fprintf(output_file, "    mov rsi, [rsp+16]\n");
            fprintf(output_file, "    mov rcx, [rsp+24]\n");
            fprintf(output_file, ".Lstrcat_cp2_%lu:\n", id);
            fprintf(output_file, "    test rcx, rcx\n");
            fprintf(output_file, "    je .Lstrcat_cp2d_%lu\n", id);
            fprintf(output_file, "    movzx rdx, byte ptr [rsi]\n");
            fprintf(output_file, "    mov [rdi], dl\n");
            fprintf(output_file, "    inc rsi\n");
            fprintf(output_file, "    inc rdi\n");
            fprintf(output_file, "    dec rcx\n");
            fprintf(output_file, "    jmp .Lstrcat_cp2_%lu\n", id);
            fprintf(output_file, ".Lstrcat_cp2d_%lu:\n", id);

            fprintf(output_file, "    mov rax, [rsp+48]\n");
            fprintf(output_file, "    mov rdx, [rsp+32]\n");
            fprintf(output_file, "    add rsp, 56\n");
            return;
        }

        bool both_float = node_is_float(node->binary_expr.lhs) && node_is_float(node->binary_expr.rhs);
        bool any_float = node_is_float(node->binary_expr.lhs) || node_is_float(node->binary_expr.rhs);
        bool is_comparison = (node->binary_expr.op >= OP_EQ && node->binary_expr.op <= OP_GE);

        if(both_float && !is_comparison){
            gen_expr(node->binary_expr.lhs, output_file, state);
            fprintf(output_file, "    movsd xmm1, xmm0\n");
            gen_expr(node->binary_expr.rhs, output_file, state);
            switch(node->binary_expr.op){
                case OP_ADD: fprintf(output_file, "    addsd xmm0, xmm1\n"); break;
                case OP_SUB: fprintf(output_file, "    subsd xmm1, xmm0\n    movsd xmm0, xmm1\n"); break;
                case OP_MUL: fprintf(output_file, "    mulsd xmm0, xmm1\n"); break;
                case OP_DIV: fprintf(output_file, "    divsd xmm1, xmm0\n    movsd xmm0, xmm1\n"); break;
                default: break;
            }
            return;
        }

        if(any_float && !is_comparison){
            if(!node_is_float(node->binary_expr.lhs)){
                gen_expr(node->binary_expr.lhs, output_file, state);
                fprintf(output_file, "    cvtsi2sd xmm1, rax\n");
                gen_expr(node->binary_expr.rhs, output_file, state);
            } else {
                gen_expr(node->binary_expr.lhs, output_file, state);
                fprintf(output_file, "    movsd xmm1, xmm0\n");
                gen_expr(node->binary_expr.rhs, output_file, state);
                fprintf(output_file, "    cvtsi2sd xmm0, rax\n");
            }
            switch(node->binary_expr.op){
                case OP_ADD: fprintf(output_file, "    addsd xmm0, xmm1\n"); break;
                case OP_SUB: fprintf(output_file, "    subsd xmm1, xmm0\n    movsd xmm0, xmm1\n"); break;
                case OP_MUL: fprintf(output_file, "    mulsd xmm0, xmm1\n"); break;
                case OP_DIV: fprintf(output_file, "    divsd xmm1, xmm0\n    movsd xmm0, xmm1\n"); break;
                default: break;
            }
            return;
        }

        if(any_float && is_comparison){
            Type* left_type = node->binary_expr.lhs->ty;
            Type* right_type = node->binary_expr.rhs->ty;

            if(is_float(left_type)){
                gen_expr(node->binary_expr.lhs, output_file, state);
                fprintf(output_file, "    movsd xmm1, xmm0\n");
                gen_expr(node->binary_expr.rhs, output_file, state);
                if(!is_float(right_type)){
                    fprintf(output_file, "    cvtsi2sd xmm0, rax\n");
                }
                fprintf(output_file, "    ucomisd xmm1, xmm0\n");
            } else {
                gen_expr(node->binary_expr.lhs, output_file, state);
                fprintf(output_file, "    push rax\n");
                state->stack_size += 8;
                gen_expr(node->binary_expr.rhs, output_file, state);
                fprintf(output_file, "    movsd xmm1, xmm0\n");
                fprintf(output_file, "    pop rax\n");
                state->stack_size -= 8;
                if(!is_float(left_type)){
                    fprintf(output_file, "    cvtsi2sd xmm0, rax\n");
                }
                fprintf(output_file, "    ucomisd xmm0, xmm1\n");
            }

            switch(node->binary_expr.op){
                case OP_EQ: fprintf(output_file, "    sete al\n    setnp dl\n    and al, dl\n    movzx rax, al\n"); break;
                case OP_NE: fprintf(output_file, "    setne al\n    setp dl\n    or al, dl\n    movzx rax, al\n"); break;
                case OP_LT: fprintf(output_file, "    setb al\n    movzx rax, al\n"); break;
                case OP_LE: fprintf(output_file, "    setbe al\n    movzx rax, al\n"); break;
                case OP_GT: fprintf(output_file, "    seta al\n    movzx rax, al\n"); break;
                case OP_GE: fprintf(output_file, "    setae al\n    movzx rax, al\n"); break;
                default: break;
            }
            return;
        }

        gen_expr(node->binary_expr.lhs, output_file, state);
        fprintf(output_file, "    push rax\n");
        state->stack_size += 8;
        gen_expr(node->binary_expr.rhs, output_file, state);
        fprintf(output_file, "    mov rcx, rax\n");
        fprintf(output_file, "    pop rax\n");
        state->stack_size -= 8;
        if(is_comparison &&
           ((node->binary_expr.lhs->ty && node->binary_expr.lhs->ty->kind == TY_ENUM) ||
            (node->binary_expr.rhs->ty && node->binary_expr.rhs->ty->kind == TY_ENUM))){
            fprintf(output_file, "    mov rax, [rax]\n");
            fprintf(output_file, "    mov rcx, [rcx]\n");
        }
        bool is_unsigned = (node->binary_expr.lhs->ty && is_uint(node->binary_expr.lhs->ty)) ||
                           (node->binary_expr.rhs->ty && is_uint(node->binary_expr.rhs->ty));
        switch(node->binary_expr.op){
            case OP_AND: fprintf(output_file, "    and rax, rcx\n"); break;
            case OP_OR:  fprintf(output_file, "    or rax, rcx\n");  break;
            case OP_XOR: fprintf(output_file, "    xor rax, rcx\n"); break;
            case OP_BITAND: fprintf(output_file, "    and rax, rcx\n"); break;
            case OP_BITOR:  fprintf(output_file, "    or rax, rcx\n");  break;
            case OP_BITXOR: fprintf(output_file, "    xor rax, rcx\n"); break;
            case OP_ADD: fprintf(output_file, "    add rax, rcx\n"); break;
            case OP_SUB: fprintf(output_file, "    sub rax, rcx\n"); break;
            case OP_MUL: fprintf(output_file, "    imul rax, rcx\n"); break;
            case OP_DIV:
                if(is_unsigned){
                    fprintf(output_file, "    xor edx, edx\n");
                    fprintf(output_file, "    div rcx\n");
                } else {
                    fprintf(output_file, "    cqo\n");
                    fprintf(output_file, "    idiv rcx\n");
                }
                break;
            case OP_MOD:
                if(is_unsigned){
                    fprintf(output_file, "    xor edx, edx\n");
                    fprintf(output_file, "    div rcx\n");
                } else {
                    fprintf(output_file, "    cqo\n");
                    fprintf(output_file, "    idiv rcx\n");
                }
                fprintf(output_file, "    mov rax, rdx\n");
                break;
            case OP_EQ:
                fprintf(output_file, "    cmp rax, rcx\n");
                fprintf(output_file, "    sete al\n");
                fprintf(output_file, "    movzx rax, al\n");
                break;
            case OP_NE:
                fprintf(output_file, "    cmp rax, rcx\n");
                fprintf(output_file, "    setne al\n");
                fprintf(output_file, "    movzx rax, al\n");
                break;
            case OP_LT:
                fprintf(output_file, "    cmp rax, rcx\n");
                fprintf(output_file, is_unsigned ? "    setb al\n" : "    setl al\n");
                fprintf(output_file, "    movzx rax, al\n");
                break;
            case OP_LE:
                fprintf(output_file, "    cmp rax, rcx\n");
                fprintf(output_file, is_unsigned ? "    setbe al\n" : "    setle al\n");
                fprintf(output_file, "    movzx rax, al\n");
                break;
            case OP_GT:
                fprintf(output_file, "    cmp rax, rcx\n");
                fprintf(output_file, is_unsigned ? "    seta al\n" : "    setg al\n");
                fprintf(output_file, "    movzx rax, al\n");
                break;
            case OP_GE:
                fprintf(output_file, "    cmp rax, rcx\n");
                fprintf(output_file, is_unsigned ? "    setae al\n" : "    setge al\n");
                fprintf(output_file, "    movzx rax, al\n");
                break;
            default: break;
        }
        return;
    }
    if(node->type == ND_TERNARY){
        uint64_t id = label_count++;
        gen_expr(node->ternary_expr.cond, output_file, state);
        fprintf(output_file, "    cmp rax, 0\n");
        fprintf(output_file, "    je .Lternary_else_%lu\n", id);
        gen_expr(node->ternary_expr.then_expr, output_file, state);
        fprintf(output_file, "    jmp .Lternary_end_%lu\n", id);
        fprintf(output_file, ".Lternary_else_%lu:\n", id);
        gen_expr(node->ternary_expr.else_expr, output_file, state);
        fprintf(output_file, ".Lternary_end_%lu:\n", id);
        return;
    }
    if(node->type == ND_UNARY_EXPR){
        if(node->unary_expr.op == OP_ADDR){
            Node* operand = node->unary_expr.operand;
            if(operand->type == ND_VARREF){
                if(operand->varref.is_global)
                    fprintf(output_file, "    lea rax, [rip + %s]\n", operand->varref.name);
                else
                    fprintf(output_file, "    lea rax, [rbp%+d]\n", operand->varref.offset);
                return;
            }
            if(operand->type == ND_MEMBER){
                // Address of a struct/class field: base yields the struct address.
                gen_expr(operand->member.base, output_file, state);
                fprintf(output_file, "    lea rax, [rax+%d]\n", operand->member.field_offset);
                return;
            }
            if(operand->type == ND_INDEX){
                // Address of an element: base address + index*elem_size.
                Node* ibase = operand->index_expr.base;
                if(ibase->type == ND_VARREF){
                    if(ibase->varref.is_global){
                        if(ibase->ty && ibase->ty->kind == TY_STRING)
                            fprintf(output_file, "    lea rax, [rip + %s]\n", ibase->varref.name);
                        else
                            fprintf(output_file, "    mov rax, [rip + %s]\n", ibase->varref.name);
                    } else {
                        fprintf(output_file, "    mov rax, [rbp%+d]\n", ibase->varref.offset);
                    }
                } else {
                    gen_expr(ibase, output_file, state);
                }
                fprintf(output_file, "    push rax\n");
                state->stack_size += 8;
                gen_expr(operand->index_expr.index, output_file, state);
                if(operand->index_expr.elem_size > 1)
                    fprintf(output_file, "    imul rax, %d\n", operand->index_expr.elem_size);
                fprintf(output_file, "    mov rcx, rax\n");
                fprintf(output_file, "    pop rax\n");
                state->stack_size -= 8;
                fprintf(output_file, "    add rax, rcx\n");
                return;
            }
            gen_expr(operand, output_file, state);
            fprintf(output_file, "    lea rax, [rax]\n");
            return;
        }
        gen_expr(node->unary_expr.operand, output_file, state);
        switch(node->unary_expr.op){
            case OP_NOT:
                fprintf(output_file, "    not rax\n");
                fprintf(output_file, "    and rax, 1\n");
                break;
            case OP_BITNOT:
                fprintf(output_file, "    not rax\n");
                break;
            case OP_NEG:
                if(is_float(node->unary_expr.operand->ty)){
                    fprintf(output_file, "    movq rax, xmm0\n");
                    fprintf(output_file, "    btc rax, 63\n");
                    fprintf(output_file, "    movq xmm0, rax\n");
                } else {
                    fprintf(output_file, "    neg rax\n");
                }
                break;
            case OP_ADDR: {
                Node* operand = node->unary_expr.operand;
                fprintf(output_file, "    lea rax, [rbp%+d]\n", operand->varref.offset);
                break;
            }
            case OP_DEREF:
                if(node->ty && is_string_type(node->ty)){
                    fprintf(output_file, "    mov rdx, [rax+8]\n");
                    fprintf(output_file, "    mov rax, [rax]\n");
                } else if(node->ty && (node->ty->kind == TY_CHAR || node->ty->kind == TY_UINT8)){
                    fprintf(output_file, "    movzx rax, byte ptr [rax]\n");
                } else if(node->ty && node->ty->kind == TY_STRUCT){
                    fprintf(output_file, "    lea rax, [rax]\n");
                } else {
                    fprintf(output_file, "    mov rax, [rax]\n");
                }
                break;
            default: break;
        }
        return;
    }
    if(node->type == ND_FUNCCALL){
        if(strcmp(node->funcall.name, "fopen_array_array") == 0 && node->funcall.args->length == 2){
            uint64_t id = label_count++;
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    mov rsi, rax\n");
            fprintf(output_file, "    movzx rax, byte ptr [rax]\n");
            fprintf(output_file, "    cmp al, 'w'\n");
            fprintf(output_file, "    je .Lfopen_w_%lu\n", id);
            fprintf(output_file, "    cmp al, 'a'\n");
            fprintf(output_file, "    je .Lfopen_a_%lu\n", id);
            fprintf(output_file, "    mov rdx, 0\n");
            fprintf(output_file, "    jmp .Lfopen_flags_%lu\n", id);
            fprintf(output_file, ".Lfopen_w_%lu:\n", id);
            fprintf(output_file, "    mov rdx, 0x241\n");
            fprintf(output_file, "    jmp .Lfopen_flags_%lu\n", id);
            fprintf(output_file, ".Lfopen_a_%lu:\n", id);
            fprintf(output_file, "    mov rdx, 0x441\n");
            fprintf(output_file, ".Lfopen_flags_%lu:\n", id);
            fprintf(output_file, "    push rdx\n");
            state->stack_size += 8;
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    pop rsi\n");
            state->stack_size -= 8;
            fprintf(output_file, "    mov rdx, 0x1B6\n");
            fprintf(output_file, "    mov rax, 2\n");
            fprintf(output_file, "    syscall\n");
            fprintf(output_file, "    sub rsp, 32\n");
            state->stack_size += 32;
            fprintf(output_file, "    mov [rsp], rax\n");
            fprintf(output_file, "    mov qword ptr [rsp+8], 0\n");
            fprintf(output_file, "    mov qword ptr [rsp+16], 0\n");
            fprintf(output_file, "    mov qword ptr [rsp+24], 1\n");
            fprintf(output_file, "    mov rax, rsp\n");
            return;
        }
        if(strcmp(node->funcall.name, "fclose_File") == 0 && node->funcall.args->length == 1){
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, [rax]\n");
            fprintf(output_file, "    mov rax, 3\n");
            fprintf(output_file, "    syscall\n");
            fprintf(output_file, "    mov rax, 0\n");
            return;
        }
        if(strcmp(node->funcall.name, "fread_File") == 0 && node->funcall.args->length == 1){
            uint64_t id = label_count++;
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, [rax]\n");
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            fprintf(output_file, "    mov rax, 8\n");
            fprintf(output_file, "    mov rsi, 0\n");
            fprintf(output_file, "    mov rdx, 2\n");
            fprintf(output_file, "    syscall\n");
            fprintf(output_file, "    mov r8, rax\n");
            fprintf(output_file, "    mov rax, 8\n");
            fprintf(output_file, "    xor rsi, rsi\n");
            fprintf(output_file, "    mov rdx, 0\n");
            fprintf(output_file, "    syscall\n");
            fprintf(output_file, "    mov rax, 9\n");
            fprintf(output_file, "    xor rdi, rdi\n");
            fprintf(output_file, "    mov rsi, r8\n");
            fprintf(output_file, "    mov rdx, 3\n");
            fprintf(output_file, "    mov r10, 0x22\n");
            fprintf(output_file, "    push r8\n");
            fprintf(output_file, "    mov r8, -1\n");
            fprintf(output_file, "    xor r9, r9\n");
            fprintf(output_file, "    syscall\n");
            fprintf(output_file, "    pop r8\n");
            fprintf(output_file, "    mov r9, rax\n");
            fprintf(output_file, "    pop rax\n");
            state->stack_size -= 8;
            fprintf(output_file, "    mov rdi, [rax]\n");
            fprintf(output_file, "    mov rsi, r9\n");
            fprintf(output_file, "    mov rdx, r8\n");
            fprintf(output_file, "    mov rax, 0\n");
            fprintf(output_file, "    syscall\n");
            fprintf(output_file, "    mov rax, r9\n");
            fprintf(output_file, "    mov rdx, r8\n");
            return;
        }
        if(strcmp(node->funcall.name, "fwrite_File_array") == 0 && node->funcall.args->length == 2){
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, [rax]\n");
            fprintf(output_file, "    push rdi\n");
            state->stack_size += 8;
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    mov rcx, rax\n");
            fprintf(output_file, "    mov r8, rdx\n");
            fprintf(output_file, "    pop rdi\n");
            state->stack_size -= 8;
            fprintf(output_file, "    mov rsi, rcx\n");
            fprintf(output_file, "    mov rdx, r8\n");
            fprintf(output_file, "    mov rax, 1\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        // ---- Socket syscalls (Linux x86-64) -----------------------------------
        // Backed by hardcoded kernel syscalls (socket=41, bind=49, listen=50,
        // accept=43, connect=42, read=0, write=1, close=3, shutdown=48).
        // All functions return the raw syscall result (>=0 on success, -errno on
        // failure). The matching declarations live in lib/std/network/socket.qz.
        if(strcmp(node->funcall.name, "socketCreate_int64_int64_int64") == 0 && node->funcall.args->length == 3){
            gen_expr(node->funcall.args->nodes[2], output_file, state);
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            fprintf(output_file, "    pop rdi\n");
            fprintf(output_file, "    pop rsi\n");
            fprintf(output_file, "    pop rdx\n");
            state->stack_size -= 24;
            fprintf(output_file, "    mov rax, 41\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "socketBind_int64_array") == 0 && node->funcall.args->length == 2){
            // rdx = address.length (sockaddr size); the fd load below may clobber
            // rdx (struct member access reloads the second half of a 16-byte struct),
            // so the length is preserved across it.
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    push rdx\n");
            fprintf(output_file, "    mov rsi, rax\n");
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    pop rdx\n");
            fprintf(output_file, "    mov rax, 49\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "socketListen_int64_int64") == 0 && node->funcall.args->length == 2){
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            fprintf(output_file, "    pop rdi\n");
            fprintf(output_file, "    pop rsi\n");
            state->stack_size -= 16;
            fprintf(output_file, "    mov rax, 50\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "socketAccept_int64") == 0 && node->funcall.args->length == 1){
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    mov rsi, 0\n");
            fprintf(output_file, "    mov rdx, 0\n");
            fprintf(output_file, "    mov rax, 43\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "socketConnect_int64_array") == 0 && node->funcall.args->length == 2){
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    push rdx\n");
            fprintf(output_file, "    mov rsi, rax\n");
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    pop rdx\n");
            fprintf(output_file, "    mov rax, 42\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "socketRead_int64_array") == 0 && node->funcall.args->length == 2){
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    push rdx\n");
            fprintf(output_file, "    mov rsi, rax\n");
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    pop rdx\n");
            fprintf(output_file, "    mov r10, 0\n");
            fprintf(output_file, "    mov rax, 0\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "socketWrite_int64_array") == 0 && node->funcall.args->length == 2){
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    push rdx\n");
            fprintf(output_file, "    mov rsi, rax\n");
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    pop rdx\n");
            fprintf(output_file, "    mov r10, 0\n");
            fprintf(output_file, "    mov rax, 1\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "socketClose_int64") == 0 && node->funcall.args->length == 1){
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    mov rax, 3\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "socketShutdown_int64_int64") == 0 && node->funcall.args->length == 2){
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            fprintf(output_file, "    pop rdi\n");
            fprintf(output_file, "    pop rsi\n");
            state->stack_size -= 16;
            fprintf(output_file, "    mov rax, 48\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        // ---- Time syscalls (Linux x86-64) --------------------------------------
        // clock_gettime = 228, nanosleep = 35. The matching declarations live in
        // lib/std/time/time.qz.
        if(strcmp(node->funcall.name, "clockGetTime_int64_array") == 0 && node->funcall.args->length == 2){
            // tp must be a buffer of at least 16 bytes (struct timespec)
            gen_expr(node->funcall.args->nodes[1], output_file, state);
            fprintf(output_file, "    mov rsi, rax\n");
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    mov rax, 228\n");
            fprintf(output_file, "    syscall\n");
            return;
        }
        if(strcmp(node->funcall.name, "nanoSleep_array") == 0 && node->funcall.args->length == 1){
            // req must be a buffer of at least 16 bytes (struct timespec)
            gen_expr(node->funcall.args->nodes[0], output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    mov rsi, 0\n");
            fprintf(output_file, "    mov rax, 35\n");
            fprintf(output_file, "    syscall\n");
            return;
        }

        int total_pushed = 0;
        for(int i = node->funcall.args->length - 1; i >= 0; i--){
            gen_expr(node->funcall.args->nodes[i], output_file, state);
            if(node_is_float(node->funcall.args->nodes[i])){
                fprintf(output_file, "    movq rax, xmm0\n");
            }
            if(node->funcall.args->nodes[i]->ty &&
               node->funcall.args->nodes[i]->ty->kind == TY_STRUCT){
                int asize = node->funcall.args->nodes[i]->ty->size;
                fprintf(output_file, "    sub rsp, %d\n", asize);
                fprintf(output_file, "    mov rsi, rax\n");
                fprintf(output_file, "    mov rdi, rsp\n");
                for(int b = 0; b < asize; b += 8){
                    fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                    fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                }
                total_pushed += asize;
                state->stack_size += asize;
            } else if(node_is_string_like(node->funcall.args->nodes[i])){
                fprintf(output_file, "    push rdx\n");
                fprintf(output_file, "    push rax\n");
                total_pushed += 16;
                state->stack_size += 16;
            } else {
                fprintf(output_file, "    push rax\n");
                total_pushed += 8;
                state->stack_size += 8;
            }
        }
        fprintf(output_file, "    call %s\n", node->funcall.name);
        if(total_pushed > 0){
            fprintf(output_file, "    add rsp, %d\n", total_pushed);
            state->stack_size -= total_pushed;
        }
        return;
    }
    if(node->type == ND_ENUMCONS){
        Type* etype = node->enumcons.resolved_type;
        if(!etype){
            fprintf(stderr, "codegen error: enum constructor '%s.%s' has no resolved type at line %s:%lu\n",
                    node->enumcons.enum_name, node->enumcons.variant_name,
                    "", 0UL);
            exit(1);
        }
        EnumDef* def = etype->enumeration.def;
        int vi = node->enumcons.variant_index;
        int total_size = etype->size;
        int data_size = etype->enumeration.data_size;

        fprintf(output_file, "    mov rax, %d\n", total_size);
        fprintf(output_file, "    add rax, 8\n");
        fprintf(output_file, "    push rax\n");
        fprintf(output_file, "    mov rdi, 0\n");
        fprintf(output_file, "    mov rsi, rax\n");
        fprintf(output_file, "    mov rdx, 3\n");
        fprintf(output_file, "    mov r10, 0x22\n");
        fprintf(output_file, "    mov r8, -1\n");
        fprintf(output_file, "    xor r9, r9\n");
        fprintf(output_file, "    mov rax, 9\n");
        fprintf(output_file, "    syscall\n");
        fprintf(output_file, "    pop rcx\n");
        fprintf(output_file, "    sub rcx, 8\n");
        fprintf(output_file, "    mov [rax], rcx\n");
        fprintf(output_file, "    add rax, 8\n");
        fprintf(output_file, "    push r12\n");
        fprintf(output_file, "    mov r12, rax\n");
        fprintf(output_file, "    mov qword ptr [r12], %d\n", vi);

        if(node->enumcons.args){
            EnumVariant* var = &def->variants[vi];
            int arg_offset = 8;
            for(int i = 0; i < node->enumcons.args->length; i++){
                gen_expr(node->enumcons.args->nodes[i], output_file, state);
                if(node->enumcons.args->nodes[i]->ty &&
                   is_float(node->enumcons.args->nodes[i]->ty)){
                    fprintf(output_file, "    movsd [r12+%d], xmm0\n", arg_offset);
                    arg_offset += 8;
                    continue;
                }
                if(node->enumcons.args->nodes[i]->ty &&
                   node->enumcons.args->nodes[i]->ty->kind == TY_STRUCT){
                    // Struct payloads are stored inline in the enum object
                    // (the enum data_size already accounts for the full
                    // struct size). This avoids dangling pointers into
                    // dead stack frames when the payload is read later.
                    int fsize = node->enumcons.args->nodes[i]->ty->size;
                    fprintf(output_file, "    mov rsi, rax\n");
                    fprintf(output_file, "    lea rdi, [r12+%d]\n", arg_offset);
                    for(int b = 0; b < fsize; b += 8){
                        fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                        fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                    }
                    arg_offset += fsize;
                    continue;
                }
                if(node->enumcons.args->nodes[i]->ty &&
                   node->enumcons.args->nodes[i]->ty->size == 1){
                    fprintf(output_file, "    mov byte ptr [r12+%d], al\n", arg_offset);
                } else if(node->enumcons.args->nodes[i]->ty &&
                          node->enumcons.args->nodes[i]->ty->size == 2){
                    fprintf(output_file, "    mov word ptr [r12+%d], ax\n", arg_offset);
                } else if(node->enumcons.args->nodes[i]->ty &&
                          node->enumcons.args->nodes[i]->ty->size == 4){
                    fprintf(output_file, "    mov dword ptr [r12+%d], eax\n", arg_offset);
                } else {
                    fprintf(output_file, "    mov [r12+%d], rax\n", arg_offset);
                }
                if(node->enumcons.args->nodes[i]->ty &&
                   (node->enumcons.args->nodes[i]->ty->kind == TY_STRING ||
                    node->enumcons.args->nodes[i]->ty->kind == TY_ARRAY)){
                    fprintf(output_file, "    mov [r12+%d], rdx\n", arg_offset + 8);
                    arg_offset += 16;
                } else {
                    arg_offset += 8;
                }
            }
        }

        fprintf(output_file, "    mov rax, r12\n");
        fprintf(output_file, "    pop r12\n");
        return;
    }
    if(node->type == ND_STRUCTCONS){
        StructDef* sdef = struct_table_lookup(node->structcons.name);
        Type* sty = struct_type(sdef);

        fprintf(output_file, "    lea rax, [rbp%+d]\n", node->structcons.temp_offset);
        fprintf(output_file, "    push r12\n");
        state->stack_size += 8;
        fprintf(output_file, "    mov r12, rax\n");

        int field_offset = 0;
        for(int i = 0; i < sdef->member_count && i < (int)node->structcons.args->length; i++){
            int align = sdef->members[i].type->align > 0 ? sdef->members[i].type->align : 1;
            field_offset = (field_offset + align - 1) & ~(align - 1);

            gen_expr(node->structcons.args->nodes[i], output_file, state);

            if(sdef->members[i].type->kind == TY_STRUCT){
                int fsize = sdef->members[i].type->size;
                fprintf(output_file, "    mov rsi, rax\n");
                fprintf(output_file, "    lea rdi, [r12+%d]\n", field_offset);
                for(int b = 0; b < fsize; b += 8){
                    fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                    fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                }
            } else if(is_float(sdef->members[i].type)){
                if(sdef->members[i].type->kind == TY_FLOAT32)
                    fprintf(output_file, "    cvtsd2ss xmm0, xmm0\n");
                if(sdef->members[i].type->kind == TY_FLOAT64)
                    fprintf(output_file, "    movsd [r12+%d], xmm0\n", field_offset);
                else
                    fprintf(output_file, "    movss [r12+%d], xmm0\n", field_offset);
            } else if(sdef->members[i].type->size == 1){
                fprintf(output_file, "    mov byte ptr [r12+%d], al\n", field_offset);
            } else if(sdef->members[i].type->size == 2){
                fprintf(output_file, "    mov word ptr [r12+%d], ax\n", field_offset);
            } else if(sdef->members[i].type->size == 4){
                fprintf(output_file, "    mov dword ptr [r12+%d], eax\n", field_offset);
            } else {
                fprintf(output_file, "    mov [r12+%d], rax\n", field_offset);
                if(sdef->members[i].type && is_string_type(sdef->members[i].type)){
                    fprintf(output_file, "    mov [r12+%d], rdx\n", field_offset + 8);
                }
            }

            field_offset += sdef->members[i].type->size;
        }

        fprintf(output_file, "    mov rax, r12\n");
        fprintf(output_file, "    pop r12\n");
        state->stack_size -= 8;
        node->ty = sty;
        return;
    }
    if(node->type == ND_ENUM_PATTERN_CMP){
        // `lhs == Enum.Variant(a: T, ...)`: compare only the tag of the
        // scrutinee against the pattern's variant index and, on a match,
        // unpack the payload into the binding stack slots.
        Type* etype = node->enum_pattern_cmp.resolved_type;
        if(!etype || etype->kind != TY_ENUM){
            fprintf(stderr, "codegen error: enum pattern '%s.%s' has no resolved type\n",
                    node->enum_pattern_cmp.enum_name, node->enum_pattern_cmp.variant_name);
            exit(1);
        }

        gen_expr(node->enum_pattern_cmp.lhs, output_file, state);
        fprintf(output_file, "    mov rcx, [rax]\n");
        fprintf(output_file, "    cmp rcx, %d\n", node->enum_pattern_cmp.variant_index);

        if(node->enum_pattern_cmp.negated){
            // Pure tag test: bound names must not be read when != holds,
            // so no payload is extracted.
            fprintf(output_file, "    setne al\n");
            fprintf(output_file, "    movzx rax, al\n");
            return;
        }

        uint64_t id = label_count++;
        fprintf(output_file, "    jne .Lepat_fail_%lu\n", id);

        NodeList* bindings = node->enum_pattern_cmp.bindings;
        if(bindings){
            fprintf(output_file, "    add rax, 8\n");
            uint64_t payload_off = 0;
            for(uint64_t j = 0; j < bindings->length; j++){
                Node* binding = bindings->nodes[j];
                if(binding->ty && binding->ty->kind == TY_STRUCT){
                    // Struct payloads are stored inline in the enum object;
                    // copy them slot by slot like match bindings do.
                    fprintf(output_file, "    lea rsi, [rax+%lu]\n", payload_off);
                    fprintf(output_file, "    lea rdi, [rbp%+d]\n", binding->varref.offset);
                    for(int b = 0; b < binding->ty->size; b += 8){
                        fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                        fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                    }
                    payload_off += binding->ty->size;
                    continue;
                }
                fprintf(output_file, "    mov rcx, [rax+%lu]\n", payload_off);
                fprintf(output_file, "    mov [rbp%+d], rcx\n", binding->varref.offset);
                if(binding->ty && (binding->ty->kind == TY_STRING || binding->ty->kind == TY_ARRAY)){
                    fprintf(output_file, "    mov rcx, [rax+%lu]\n", payload_off + 8);
                    fprintf(output_file, "    mov [rbp%+d], rcx\n", binding->varref.offset + 8);
                    payload_off += 16;
                } else {
                    payload_off += 8;
                }
            }
        }

        fprintf(output_file, "    mov eax, 1\n");
        fprintf(output_file, "    jmp .Lepat_end_%lu\n", id);
        fprintf(output_file, ".Lepat_fail_%lu:\n", id);
        fprintf(output_file, "    xor eax, eax\n");
        fprintf(output_file, ".Lepat_end_%lu:\n", id);
        return;
    }
    if(node->type == ND_MATCH){
        uint64_t id = label_count++;

        gen_expr(node->match.scrutinee, output_file, state);
        fprintf(output_file, "    push rax\n");
        state->stack_size += 8;

        NodeList* cases = node->match.cases;

        if(node->match.is_enum){
            fprintf(output_file, "    mov rax, [rsp]\n");
            fprintf(output_file, "    mov rax, [rax]\n");
            for(uint64_t i = 0; i < cases->length; i++){
                fprintf(output_file, "    cmp rax, %lu\n", (unsigned long)cases->nodes[i]->match_case.variant_index);
                fprintf(output_file, "    je .Lmatch_case_%lu_%lu\n", id, i);
            }
        } else {
            for(uint64_t i = 0; i < cases->length; i++){
                fprintf(output_file, "    mov rcx, [rsp]\n");
                gen_expr(cases->nodes[i]->match_case.label, output_file, state);
                fprintf(output_file, "    cmp rax, rcx\n");
                fprintf(output_file, "    je .Lmatch_case_%lu_%lu\n", id, i);
            }
        }

        if(node->match.default_body){
            fprintf(output_file, "    jmp .Lmatch_default_%lu\n", id);
        } else {
            fprintf(output_file, "    jmp .Lmatch_end_%lu\n", id);
        }

        for(uint64_t i = 0; i < cases->length; i++){
            Node* c = cases->nodes[i];
            fprintf(output_file, ".Lmatch_case_%lu_%lu:\n", id, i);

            if(c->match_case.bindings){
                fprintf(output_file, "    mov rax, [rsp]\n");
                fprintf(output_file, "    add rax, 8\n");
                uint64_t payload_off = 0;
                for(uint64_t j = 0; j < c->match_case.bindings->length; j++){
                    Node* binding = c->match_case.bindings->nodes[j];
                    if(binding->ty && binding->ty->kind == TY_STRUCT){
                        // Struct payloads are stored inline in the enum
                        // object, so copy directly from the payload area.
                        fprintf(output_file, "    lea rsi, [rax+%lu]\n", payload_off);
                        fprintf(output_file, "    lea rdi, [rbp%+d]\n", binding->varref.offset);
                        for(int b = 0; b < binding->ty->size; b += 8){
                            fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                            fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                        }
                        payload_off += binding->ty->size;
                        continue;
                    }
                    fprintf(output_file, "    mov rcx, [rax+%lu]\n", payload_off);
                    fprintf(output_file, "    mov [rbp%+d], rcx\n", binding->varref.offset);
                    if(binding->ty && (binding->ty->kind == TY_STRING || binding->ty->kind == TY_ARRAY)){
                        fprintf(output_file, "    mov rcx, [rax+%lu]\n", payload_off + 8);
                        fprintf(output_file, "    mov [rbp%+d], rcx\n", binding->varref.offset + 8);
                        payload_off += 16;
                    } else {
                        payload_off += 8;
                    }
                }
            }

            gen_expr(c->match_case.body, output_file, state);
            fprintf(output_file, "    jmp .Lmatch_end_%lu\n", id);
        }

        if(node->match.default_body){
            fprintf(output_file, ".Lmatch_default_%lu:\n", id);
            gen_expr(node->match.default_body, output_file, state);
        }

        fprintf(output_file, ".Lmatch_end_%lu:\n", id);
        fprintf(output_file, "    add rsp, 8\n");
        state->stack_size -= 8;
        return;
    }
    if(node->type == ND_CAST){
        Type* from_type = node->cast.expr->ty;
        Type* to_type = node->cast.target_type;
        gen_expr(node->cast.expr, output_file, state);

        bool from_is_float = from_type && is_float(from_type);
        bool to_is_float = is_float(to_type);

        if(!from_is_float && to_is_float){
            gen_int_to_float(node, output_file, to_type);
        } else if(from_is_float && !to_is_float){
            gen_float_to_int(node, output_file, from_type);
        } else if(from_is_float && to_is_float){
            if(from_type->kind == TY_FLOAT32 && (to_type->kind == TY_FLOAT64)){
                if(node->cast.expr->type != ND_FLOATLIT){
                    fprintf(output_file, "    cvtss2sd xmm0, xmm0\n");
                }
            } else if((from_type->kind == TY_FLOAT64) && to_type->kind == TY_FLOAT32){
                fprintf(output_file, "    cvtsd2ss xmm0, xmm0\n");
            }
        }
        return;
    }
    if(node->type == ND_ALLOC){
        int elem_size = node->alloc.alloc_type->size;
        if(node->alloc.count){
            gen_expr(node->alloc.count, output_file, state);
            fprintf(output_file, "    mov rdi, %d\n", elem_size);
            fprintf(output_file, "    imul rax, rdi\n");
        } else {
            fprintf(output_file, "    mov rax, %d\n", elem_size);
        }
        fprintf(output_file, "    add rax, 8\n");
        fprintf(output_file, "    push rax\n");
        fprintf(output_file, "    mov rdi, 0\n");
        fprintf(output_file, "    mov rsi, rax\n");
        fprintf(output_file, "    mov rdx, 3\n");
        fprintf(output_file, "    mov r10, 0x22\n");
        fprintf(output_file, "    mov r8, -1\n");
        fprintf(output_file, "    xor r9, r9\n");
        fprintf(output_file, "    mov rax, 9\n");
        fprintf(output_file, "    syscall\n");
        fprintf(output_file, "    pop rcx\n");
        fprintf(output_file, "    sub rcx, 8\n");
        fprintf(output_file, "    mov [rax], rcx\n");
        fprintf(output_file, "    add rax, 8\n");
        return;
    }
    fprintf(stderr, "codegen error: unsupported expression type %d\n", node->type);
    exit(1);
}

static void gen_stmt(Node* node, FILE* output_file, CodegenState* state);

static void gen_block(NodeList* stmts, FILE* output_file, CodegenState* state){
    for(uint64_t i = 0; i < stmts->length; i++){
        gen_stmt(stmts->nodes[i], output_file, state);
    }
}

static void gen_switch(Node* node, FILE* output_file, CodegenState* state){
    uint64_t id = label_count++;
    uint64_t end_label = id;

    state->break_labels[state->break_depth++] = end_label;

    gen_expr(node->switch_stmt.scrutinee, output_file, state);

    if(node->switch_stmt.is_string){
        fprintf(output_file, "    mov rcx, rdx\n");
        fprintf(output_file, "    push rcx\n");
        fprintf(output_file, "    push rax\n");
        state->stack_size += 16;
    } else {
        fprintf(output_file, "    push rax\n");
        state->stack_size += 8;
    }

    if(node->switch_stmt.is_string){
        fprintf(output_file, "    .section .rodata\n");
        for(uint64_t i = 0; i < node->switch_stmt.cases->length; i++){
            Node* c = node->switch_stmt.cases->nodes[i];
            fprintf(output_file, ".Lswitch_%lu_str_%lu:\n", id, i);
            fprintf(output_file, "    .asciz \"%s\"\n", c->switch_case.label->strlit.value);
        }
        fprintf(output_file, "    .section .text\n");
    }

    fprintf(output_file, ".Lswitch_%lu_cmp:\n", id);
    for(uint64_t i = 0; i < node->switch_stmt.cases->length; i++){
        Node* c = node->switch_stmt.cases->nodes[i];
        if(node->switch_stmt.is_string){
            int slen = c->switch_case.label->strlit.length;
            fprintf(output_file, "    mov rax, [rsp+8]\n");
            fprintf(output_file, "    cmp rax, %d\n", slen);
            fprintf(output_file, "    jne .Lswitch_%lu_next_%lu\n", id, i);

            fprintf(output_file, "    test rax, rax\n");
            fprintf(output_file, "    je .Lswitch_%lu_case_%lu\n", id, i);

            fprintf(output_file, "    lea rdi, [rip + .Lswitch_%lu_str_%lu]\n", id, i);
            fprintf(output_file, "    mov rsi, [rsp]\n");
            fprintf(output_file, "    mov rcx, rax\n");
            fprintf(output_file, ".Lswitch_%lu_chrcmp_%lu:\n", id, i);
            fprintf(output_file, "    movzx rax, byte ptr [rdi]\n");
            fprintf(output_file, "    movzx rdx, byte ptr [rsi]\n");
            fprintf(output_file, "    cmp al, dl\n");
            fprintf(output_file, "    jne .Lswitch_%lu_next_%lu\n", id, i);
            fprintf(output_file, "    inc rdi\n");
            fprintf(output_file, "    inc rsi\n");
            fprintf(output_file, "    dec rcx\n");
            fprintf(output_file, "    jnz .Lswitch_%lu_chrcmp_%lu\n", id, i);
            fprintf(output_file, "    jmp .Lswitch_%lu_case_%lu\n", id, i);
            fprintf(output_file, ".Lswitch_%lu_next_%lu:\n", id, i);
        } else {
            fprintf(output_file, "    mov rcx, [rsp]\n");
            gen_expr(c->switch_case.label, output_file, state);
            fprintf(output_file, "    cmp rax, rcx\n");
            fprintf(output_file, "    je .Lswitch_%lu_case_%lu\n", id, i);
        }
    }
    if(node->switch_stmt.default_body){
        fprintf(output_file, "    jmp .Lswitch_%lu_default\n", id);
    } else {
        fprintf(output_file, "    jmp .Lbreak_%lu\n", id);
    }

    for(uint64_t i = 0; i < node->switch_stmt.cases->length; i++){
        Node* c = node->switch_stmt.cases->nodes[i];
        fprintf(output_file, ".Lswitch_%lu_case_%lu:\n", id, i);
        gen_block(c->switch_case.body, output_file, state);
    }

    if(node->switch_stmt.default_body){
        fprintf(output_file, ".Lswitch_%lu_default:\n", id);
        gen_block(node->switch_stmt.default_body, output_file, state);
    }

    fprintf(output_file, ".Lbreak_%lu:\n", id);
    if(node->switch_stmt.is_string){
        fprintf(output_file, "    add rsp, 16\n");
        state->stack_size -= 16;
    } else {
        fprintf(output_file, "    add rsp, 8\n");
        state->stack_size -= 8;
    }

    state->break_depth--;
}

static void gen_if(Node* node, FILE* output_file, CodegenState* state){
    uint64_t id = label_count++;

    gen_expr(node->if_stmt.cond, output_file, state);
    fprintf(output_file, "    cmp rax, 0\n");

    if(node->if_stmt.else_body){
        fprintf(output_file, "    je .Lelse_%lu\n", id);
        gen_block(node->if_stmt.then_body, output_file, state);
        fprintf(output_file, "    jmp .Lend_%lu\n", id);
        fprintf(output_file, ".Lelse_%lu:\n", id);
        gen_block(node->if_stmt.else_body, output_file, state);
        fprintf(output_file, ".Lend_%lu:\n", id);
    } else {
        fprintf(output_file, "    je .Lend_%lu\n", id);
        gen_block(node->if_stmt.then_body, output_file, state);
        fprintf(output_file, ".Lend_%lu:\n", id);
    }
}

static void gen_vardecl(Node* node, FILE* output_file, CodegenState* state){
    if(node->vardecl.is_global){
        return;
    }
    int alloc_size = 8;
    if(node->vardecl.type && node->vardecl.type->size > 8){
        alloc_size = node->vardecl.type->size;
    }
    state->stack_size += alloc_size;
    if(node->vardecl.init){
        gen_expr(node->vardecl.init, output_file, state);
        if(node->vardecl.type && is_float(node->vardecl.type)){
            if(!node_is_float(node->vardecl.init)){
                if(node->vardecl.type->kind == TY_FLOAT32)
                    fprintf(output_file, "    cvtsi2ss xmm0, rax\n");
                else
                    fprintf(output_file, "    cvtsi2sd xmm0, rax\n");
            } else {
                if(node->vardecl.type->kind == TY_FLOAT32)
                    fprintf(output_file, "    cvtsd2ss xmm0, xmm0\n");
            }
            if(node->vardecl.type->kind == TY_FLOAT32)
                fprintf(output_file, "    movss [rbp%+d], xmm0\n", node->vardecl.offset);
            else
                fprintf(output_file, "    movsd [rbp%+d], xmm0\n", node->vardecl.offset);
        } else if(node->vardecl.type && node->vardecl.type->kind == TY_CHAR){
            fprintf(output_file, "    mov byte ptr [rbp%+d], al\n", node->vardecl.offset);
        } else if(node->vardecl.type && node->vardecl.type->kind == TY_STRUCT){
            int vsize = node->vardecl.type->size;
            fprintf(output_file, "    mov rsi, rax\n");
            fprintf(output_file, "    lea rdi, [rbp%+d]\n", node->vardecl.offset);
            for(int b = 0; b < vsize; b += 8){
                fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
            }
        } else {
            fprintf(output_file, "    mov [rbp%+d], rax\n", node->vardecl.offset);
            if(node->vardecl.type && (node->vardecl.type->kind == TY_STRING ||
                                      node->vardecl.type->kind == TY_ARRAY)){
                fprintf(output_file, "    mov [rbp%+d], rdx\n", node->vardecl.offset + 8);
            }
        }
    } else {
        if(node->vardecl.type && node->vardecl.type->kind == TY_STRUCT){
            for(int b = 0; b < node->vardecl.type->size; b += 8){
                fprintf(output_file, "    mov qword ptr [rbp%+d], 0\n", node->vardecl.offset + b);
            }
        } else {
            fprintf(output_file, "    mov qword ptr [rbp%+d], 0\n", node->vardecl.offset);
        }
    }
}

static void gen_stmt(Node* node, FILE* output_file, CodegenState* state){
    if(node->type == ND_EXIT){
        gen_expr(node->exit_stmt.expr, output_file, state);
        if(node_is_float(node->exit_stmt.expr)){
            fprintf(output_file, "    cvttsd2si rdi, xmm0\n");
        } else {
            fprintf(output_file, "    mov rdi, rax\n");
        }
        fprintf(output_file, "    mov rax, 60\n");
        fprintf(output_file, "    syscall\n");
        return;
    }
    if(node->type == ND_IF){
        gen_if(node, output_file, state);
        return;
    }
    if(node->type == ND_WHILE){
        uint64_t id = label_count++;
        uint64_t cont_id = label_count++;
        state->break_labels[state->break_depth] = id;
        state->continue_labels[state->break_depth] = cont_id;
        state->break_depth++;
        fprintf(output_file, ".Lwhile_%lu:\n", id);
        fprintf(output_file, ".Lcont_%lu:\n", cont_id);
        gen_expr(node->loop.cond, output_file, state);
        fprintf(output_file, "    cmp rax, 0\n");
        fprintf(output_file, "    je .Lbreak_%lu\n", id);
        gen_block(node->loop.body, output_file, state);
        fprintf(output_file, "    jmp .Lwhile_%lu\n", id);
        fprintf(output_file, ".Lbreak_%lu:\n", id);
        state->break_depth--;
        return;
    }
    if(node->type == ND_DO_WHILE){
        uint64_t id = label_count++;
        uint64_t cont_id = label_count++;
        state->break_labels[state->break_depth] = id;
        state->continue_labels[state->break_depth] = cont_id;
        state->break_depth++;
        fprintf(output_file, ".Ldo_%lu:\n", id);
        gen_block(node->loop.body, output_file, state);
        fprintf(output_file, ".Lcont_%lu:\n", cont_id);
        gen_expr(node->loop.cond, output_file, state);
        fprintf(output_file, "    cmp rax, 0\n");
        fprintf(output_file, "    jne .Ldo_%lu\n", id);
        fprintf(output_file, ".Lbreak_%lu:\n", id);
        state->break_depth--;
        return;
    }
    if(node->type == ND_FOR){
        uint64_t id = label_count++;
        uint64_t cont_id = label_count++;
        state->break_labels[state->break_depth] = id;
        state->continue_labels[state->break_depth] = cont_id;
        state->break_depth++;
        if(node->for_stmt.init){
            gen_stmt(node->for_stmt.init, output_file, state);
        }
        fprintf(output_file, ".Lfor_%lu:\n", id);
        if(node->for_stmt.cond){
            gen_expr(node->for_stmt.cond, output_file, state);
            fprintf(output_file, "    cmp rax, 0\n");
            fprintf(output_file, "    je .Lbreak_%lu\n", id);
        }
        gen_block(node->for_stmt.body, output_file, state);
        fprintf(output_file, ".Lcont_%lu:\n", cont_id);
        if(node->for_stmt.update){
            gen_stmt(node->for_stmt.update, output_file, state);
        }
        fprintf(output_file, "    jmp .Lfor_%lu\n", id);
        fprintf(output_file, ".Lbreak_%lu:\n", id);
        state->break_depth--;
        return;
    }
    if(node->type == ND_VARDECL){
        gen_vardecl(node, output_file, state);
        return;
    }
    if(node->type == ND_RETURN){
        if(node->return_stmt.expr){
            gen_expr(node->return_stmt.expr, output_file, state);
            if(node_is_float(node->return_stmt.expr)){
                fprintf(output_file, "    cvttsd2si rdi, xmm0\n");
            } else {
                fprintf(output_file, "    mov rdi, rax\n");
            }
        }
        fprintf(output_file, "    mov rsp, rbp\n");
        fprintf(output_file, "    pop rbp\n");
        if(state->is_main){
            fprintf(output_file, "    mov rax, 60\n");
            fprintf(output_file, "    syscall\n");
        } else {
            fprintf(output_file, "    ret\n");
        }
        return;
    }
    if(node->type == ND_ASSIGN){
        gen_expr(node->assign.value, output_file, state);
        bool val_is_float = node_is_float(node->assign.value);
        bool target_is_float = node->assign.var_type && is_float(node->assign.var_type);
        bool target_is_32 = node->assign.var_type && node->assign.var_type->kind == TY_FLOAT32;

        if(!val_is_float && target_is_float){
            if(target_is_32)
                fprintf(output_file, "    cvtsi2ss xmm0, rax\n");
            else
                fprintf(output_file, "    cvtsi2sd xmm0, rax\n");
            val_is_float = true;
        }

        if(val_is_float && target_is_32){
            fprintf(output_file, "    cvtsd2ss xmm0, xmm0\n");
        }

        if(node->assign.is_global){
            if(val_is_float){
                bool is_32 = node->assign.var_type && node->assign.var_type->kind == TY_FLOAT32;
                if(is_32)
                    fprintf(output_file, "    movss [rip + %s], xmm0\n", node->assign.name);
                else
                    fprintf(output_file, "    movsd [rip + %s], xmm0\n", node->assign.name);
            } else if(node->assign.var_type && node->assign.var_type->kind == TY_STRING){
                fprintf(output_file, "    mov [rip + %s], rax\n", node->assign.name);
                fprintf(output_file, "    mov [rip + %s_len], rdx\n", node->assign.name);
            } else if(node->assign.var_type && node->assign.var_type->kind == TY_ARRAY){
                fprintf(output_file, "    mov [rip + %s], rax\n", node->assign.name);
                fprintf(output_file, "    mov [rip + %s + 8], rdx\n", node->assign.name);
            } else if(node->assign.var_type && node->assign.var_type->kind == TY_STRUCT){
                int gsize = node->assign.var_type->size;
                fprintf(output_file, "    mov rsi, rax\n");
                fprintf(output_file, "    lea rdi, [rip + %s]\n", node->assign.name);
                for(int b = 0; b < gsize; b += 8){
                    fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                    fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                }
            } else {
                fprintf(output_file, "    mov [rip + %s], rax\n", node->assign.name);
            }
        } else {
            if(val_is_float){
                bool is_32 = node->assign.var_type && node->assign.var_type->kind == TY_FLOAT32;
                if(is_32)
                    fprintf(output_file, "    movss [rbp%+d], xmm0\n", node->assign.offset);
                else
                    fprintf(output_file, "    movsd [rbp%+d], xmm0\n", node->assign.offset);
            } else if(node->assign.var_type && (node->assign.var_type->kind == TY_CHAR || node->assign.var_type->kind == TY_BOOL)){
                fprintf(output_file, "    mov byte ptr [rbp%+d], al\n", node->assign.offset);
            } else if(node->assign.var_type && node->assign.var_type->kind == TY_STRUCT){
                int asize = node->assign.var_type->size;
                fprintf(output_file, "    mov rsi, rax\n");
                fprintf(output_file, "    lea rdi, [rbp%+d]\n", node->assign.offset);
                for(int b = 0; b < asize; b += 8){
                    fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                    fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                }
            } else if(node->assign.var_type && (node->assign.var_type->kind == TY_STRING ||
                                                node->assign.var_type->kind == TY_ARRAY)){
                fprintf(output_file, "    mov [rbp%+d], rax\n", node->assign.offset);
                fprintf(output_file, "    mov [rbp%+d], rdx\n", node->assign.offset + 8);
            } else {
                fprintf(output_file, "    mov [rbp%+d], rax\n", node->assign.offset);
            }
        }
        return;
    }
    if(node->type == ND_MEMBER_ASSIGN){
        gen_expr(node->member_assign.value, output_file, state);
        bool val_is_float = node_is_float(node->member_assign.value);
        bool val_is_struct = node->member_assign.value->ty &&
                             node->member_assign.value->ty->kind == TY_STRUCT;
        int vsize = val_is_struct ? node->member_assign.value->ty->size : 0;
        Node* mobj = node->member_assign.object;
        int field_off = node->member_assign.field_offset;

        if(mobj->type == ND_THIS){
            fprintf(output_file, "    mov rdi, [rbp+16]\n");
        } else if(!node->member_assign.is_struct && !node->member_assign.is_class){
            // Array/string member assign (pointer/length): the value lives inline in
            // the 16-byte {ptr, length} slot, so write to the slot directly instead
            // of dereferencing the data pointer.
            int base_off = mobj->varref.offset;
            int slot_off = strcmp(node->member_assign.field_name, "length") == 0 ? 8 : 0;
            if(val_is_float){
                fprintf(output_file, "    movsd [rbp%+d], xmm0\n", base_off + slot_off);
            } else {
                fprintf(output_file, "    mov [rbp%+d], rax\n", base_off + slot_off);
            }
            return;
        } else if(mobj->type == ND_VARREF){
            if(mobj->varref.is_global){
                if(mobj->ty && (mobj->ty->kind == TY_PTR || mobj->ty->kind == TY_REF)){
                    fprintf(output_file, "    mov rdi, [rip + %s]\n", mobj->varref.name);
                } else {
                    fprintf(output_file, "    lea rdi, [rip + %s]\n", mobj->varref.name);
                }
            } else if(node->member_assign.is_class ||
                      (mobj->ty && (mobj->ty->kind == TY_PTR || mobj->ty->kind == TY_REF))){
                fprintf(output_file, "    mov rdi, [rbp%+d]\n", mobj->varref.offset);
            } else {
                fprintf(output_file, "    lea rdi, [rbp%+d]\n", mobj->varref.offset);
            }
        } else {
            bool val_is_string = node_is_string_like(node->member_assign.value);
            if(val_is_string){
                fprintf(output_file, "    push rdx\n");
                state->stack_size += 8;
            }
            fprintf(output_file, "    push rax\n");
            state->stack_size += 8;
            gen_expr(mobj, output_file, state);
            fprintf(output_file, "    mov rdi, rax\n");
            fprintf(output_file, "    pop rax\n");
            state->stack_size -= 8;
            if(val_is_string){
                fprintf(output_file, "    pop rdx\n");
                state->stack_size -= 8;
            }
        }

        if(val_is_struct){
            fprintf(output_file, "    mov rsi, rax\n");
            fprintf(output_file, "    lea rdi, [rdi+%d]\n", field_off);
            for(int b = 0; b < vsize; b += 8){
                fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
            }
        } else if(val_is_float){
            fprintf(output_file, "    movsd [rdi+%d], xmm0\n", field_off);
        } else if(node_is_string_like(node->member_assign.value)){
            fprintf(output_file, "    mov [rdi+%d], rax\n", field_off);
            fprintf(output_file, "    mov [rdi+%d], rdx\n", field_off + 8);
        } else if(node->member_assign.value->ty && (node->member_assign.value->ty->kind == TY_BOOL ||
                                                    node->member_assign.value->ty->kind == TY_CHAR)){
            fprintf(output_file, "    mov byte ptr [rdi+%d], al\n", field_off);
        } else {
            fprintf(output_file, "    mov [rdi+%d], rax\n", field_off);
        }
        return;
    }
    if(node->type == ND_FUNCCALL){
        gen_expr(node, output_file, state);
        return;
    }
    if(node->type == ND_BREAK){
        if(state->break_depth == 0){
            fprintf(stderr, "codegen error: break outside of switch/loop\n");
            exit(1);
        }
        fprintf(output_file, "    jmp .Lbreak_%lu\n", state->break_labels[state->break_depth - 1]);
        return;
    }
    if(node->type == ND_CONTINUE){
        if(state->break_depth == 0){
            fprintf(stderr, "codegen error: continue outside of loop\n");
            exit(1);
        }
        fprintf(output_file, "    jmp .Lcont_%lu\n", state->continue_labels[state->break_depth - 1]);
        return;
    }
    if(node->type == ND_FREE){
        gen_expr(node->free.expr, output_file, state);
        fprintf(output_file, "    sub rax, 8\n");
        fprintf(output_file, "    mov rdi, rax\n");
        fprintf(output_file, "    mov rsi, [rax]\n");
        fprintf(output_file, "    add rsi, 8\n");
        fprintf(output_file, "    mov rax, 11\n");
        fprintf(output_file, "    syscall\n");
        return;
    }
    if(node->type == ND_SWITCH){
        gen_switch(node, output_file, state);
        return;
    }
    if(node->type == ND_MATCH){
        gen_expr(node, output_file, state);
        return;
    }
    if(node->type == ND_DEREF_ASSIGN){
        gen_expr(node->deref_assign.target, output_file, state);
        fprintf(output_file, "    push rax\n");
        gen_expr(node->deref_assign.value, output_file, state);
        fprintf(output_file, "    pop rcx\n");
        if(node->deref_assign.value->ty &&
           (node->deref_assign.value->ty->kind == TY_CHAR || node->deref_assign.value->ty->kind == TY_UINT8)){
            fprintf(output_file, "    mov byte ptr [rcx], al\n");
        } else if(node->deref_assign.value->ty && is_string_type(node->deref_assign.value->ty)){
            fprintf(output_file, "    mov [rcx], rax\n");
            fprintf(output_file, "    mov [rcx+8], rdx\n");
        } else if(node->deref_assign.value->ty && node->deref_assign.value->ty->kind == TY_STRUCT){
            fprintf(output_file, "    mov rsi, rax\n");
            fprintf(output_file, "    mov rdi, rcx\n");
            for(int b = 0; b < node->deref_assign.value->ty->size; b += 8){
                fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
            }
        } else {
            fprintf(output_file, "    mov [rcx], rax\n");
        }
        return;
    }
    if(node->type == ND_EXPR_STMT){
        gen_expr(node->expr_stmt.expr, output_file, state);
        return;
    }
    fprintf(stderr, "codegen error: unsupported statement type %d\n", node->type);
    exit(1);
}

static void gen_funcdef(Node* node, FILE* output_file){
    if(node->funcdef.is_extern) return;

    bool is_main = node->funcdef.unmangled_name && strcmp(node->funcdef.unmangled_name, "main") == 0;
    bool main_has_args = is_main && node->funcdef.params->length > 0;

    if(is_main){
        fprintf(output_file, ".globl _start\n_start:\n");
    } else {
        fprintf(output_file, ".globl %s\n%s:\n", node->funcdef.name, node->funcdef.name);
    }

    fprintf(output_file, "    push rbp\n");
    fprintf(output_file, "    mov rbp, rsp\n");

    if(main_has_args){
        uint64_t id = label_count++;
        int locals = node->funcdef.stack_size;
        if(locals < 16) locals = 16;

        fprintf(output_file, "    mov rax, [rbp+8]\n");
        fprintf(output_file, "    mov r8, rax\n");
        fprintf(output_file, "    shl rax, 4\n");
        fprintf(output_file, "    add rax, %d\n", locals);
        fprintf(output_file, "    sub rsp, rax\n");

        fprintf(output_file, "    xor rsi, rsi\n");
        fprintf(output_file, "    mov rdi, rsp\n");

        fprintf(output_file, ".Linit_args_%lu:\n", id);
        fprintf(output_file, "    cmp rsi, r8\n");
        fprintf(output_file, "    jge .Linit_args_done_%lu\n", id);

        fprintf(output_file, "    mov rax, [rbp+16+rsi*8]\n");
        fprintf(output_file, "    mov [rdi], rax\n");

        fprintf(output_file, "    xor rcx, rcx\n");
        fprintf(output_file, ".Lstrlen_%lu:\n", id);
        fprintf(output_file, "    movzx rdx, byte ptr [rax+rcx]\n");
        fprintf(output_file, "    test rdx, rdx\n");
        fprintf(output_file, "    je .Lstrlen_done_%lu\n", id);
        fprintf(output_file, "    inc rcx\n");
        fprintf(output_file, "    jmp .Lstrlen_%lu\n", id);
        fprintf(output_file, ".Lstrlen_done_%lu:\n", id);

        fprintf(output_file, "    mov [rdi+8], rcx\n");
        fprintf(output_file, "    add rdi, 16\n");
        fprintf(output_file, "    inc rsi\n");
        fprintf(output_file, "    jmp .Linit_args_%lu\n", id);
        fprintf(output_file, ".Linit_args_done_%lu:\n", id);

        fprintf(output_file, "    mov [rbp+16], rsp\n");
        fprintf(output_file, "    mov [rbp+24], r8\n");
    } else if(node->funcdef.stack_size > 0){
        fprintf(output_file, "    sub rsp, %d\n", node->funcdef.stack_size);
    }

    CodegenState state = { .stack_size = 0, .is_main = is_main, .break_depth = 0 };

    if(is_main){
        for(int i = 0; i < pending_global_init_count; i++){
            Node* g = pending_global_init[i];
            gen_expr(g->vardecl.init, output_file, &state);
            Type* gty = g->vardecl.type;
            if(g->vardecl.init->type == ND_STRUCTCONS && gty && gty->kind == TY_STRUCT){
                int gsize = gty->size;
                fprintf(output_file, "    mov rsi, rax\n");
                fprintf(output_file, "    lea rdi, [rip + %s]\n", g->vardecl.name);
                for(int b = 0; b < gsize; b += 8){
                    fprintf(output_file, "    mov rcx, [rsi+%d]\n", b);
                    fprintf(output_file, "    mov [rdi+%d], rcx\n", b);
                }
            } else if(gty && (gty->kind == TY_STRING || gty->kind == TY_ARRAY)){
                fprintf(output_file, "    mov [rip + %s], rax\n", g->vardecl.name);
                fprintf(output_file, "    mov [rip + %s + 8], rdx\n", g->vardecl.name);
            } else {
                fprintf(output_file, "    mov [rip + %s], rax\n", g->vardecl.name);
            }
        }
    }

    gen_block(node->funcdef.body, output_file, &state);

    NodeList* body = node->funcdef.body;
    bool ends_with_return = body->length > 0 && body->nodes[body->length - 1]->type == ND_RETURN;
    bool ends_with_exit = body->length > 0 && body->nodes[body->length - 1]->type == ND_EXIT;

    if(!is_main && !ends_with_return){
        fprintf(output_file, "    mov rsp, rbp\n");
        fprintf(output_file, "    pop rbp\n");
        fprintf(output_file, "    ret\n");
    } else if(is_main && !ends_with_return && !ends_with_exit){
        fprintf(output_file, "    mov rax, 0\n");
        fprintf(output_file, "    mov rdi, rax\n");
        fprintf(output_file, "    mov rax, 60\n");
        fprintf(output_file, "    syscall\n");
    }
}

void codegen(const Node* ast, FILE* output_file){
    fprintf(output_file, ".intel_syntax noprefix\n");

    bool has_data = false;
    bool has_bss = false;
    for(uint64_t i = 0; i < ast->program_node.children->length; i++){
        Node* child = ast->program_node.children->nodes[i];
        if(child->type == ND_VARDECL && child->vardecl.is_global){
            if(child->vardecl.init){
                if(!has_data){
                    fprintf(output_file, ".section .data\n");
                    has_data = true;
                }
                if(child->vardecl.init->type == ND_STRLIT && child->vardecl.type && is_string_type(child->vardecl.type)){
                    if(child->vardecl.type->kind == TY_STRING){
                        fprintf(output_file, "%s: .asciz \"%s\"\n", child->vardecl.name, child->vardecl.init->strlit.value);
                        fprintf(output_file, "%s_len: .quad %d\n", child->vardecl.name, child->vardecl.init->strlit.length);
                    } else {
                        // char[] (TY_ARRAY): the global is a {ptr, length} slot.
                        uint64_t id = get_strlit_label(output_file, child->vardecl.init->strlit.value, child->vardecl.init->strlit.length);
                        fprintf(output_file, "%s: .quad .Lstrlit_%lu\n", child->vardecl.name, id);
                        fprintf(output_file, "    .quad %d\n", child->vardecl.init->strlit.length);
                    }
                } else if(child->vardecl.init->type == ND_INTLIT){
                    fprintf(output_file, "%s: .quad %lld\n", child->vardecl.name, child->vardecl.init->intlit.value);
                } else if(child->vardecl.init->type == ND_FLOATLIT){
                    if(child->vardecl.type && child->vardecl.type->kind == TY_FLOAT32)
                        fprintf(output_file, "%s: .float %Lg\n", child->vardecl.name, child->vardecl.init->floatlit.value);
                    else
                        fprintf(output_file, "%s: .double %Lg\n", child->vardecl.name, child->vardecl.init->floatlit.value);
                } else if(child->vardecl.init->type == ND_STRUCTCONS){
                    StructDef* sdef = struct_table_lookup(child->vardecl.init->structcons.name);
                    if(sdef){
                        fprintf(output_file, "%s:\n", child->vardecl.name);
                        for(int m = 0; m < sdef->member_count; m++){
                            Node* arg = NULL;
                            if(child->vardecl.init->structcons.args && m < (int)child->vardecl.init->structcons.args->length)
                                arg = child->vardecl.init->structcons.args->nodes[m];
                            if(arg && arg->type == ND_INTLIT){
                                fprintf(output_file, "    .quad %lld\n", arg->intlit.value);
                            } else if(arg && arg->type == ND_BOOLLIT){
                                fprintf(output_file, "    .quad %d\n", arg->boollit.value ? 1 : 0);
                            } else if(arg && arg->type == ND_STRLIT){
                                if(sdef->members[m].type && is_string_type(sdef->members[m].type)){
                                    uint64_t id = get_strlit_label(output_file, arg->strlit.value, arg->strlit.length);
                                    fprintf(output_file, "    .quad .Lstrlit_%lu\n", id);
                                    fprintf(output_file, "    .quad %d\n", arg->strlit.length);
                                } else {
                                    fprintf(output_file, "    .quad 0\n");
                                }
                            } else {
                                fprintf(output_file, "    .quad 0\n");
                            }
                        }
                    } else {
                        fprintf(output_file, "%s: .quad 0\n", child->vardecl.name);
                    }
                } else {
                    // Runtime-initialized global (constructor call, funcall, etc.).
                    // The initializer is emitted at the top of _start so it runs
                    // before any function body. Reserve the full type size: slices
                    // are {ptr, length} (16 bytes) and struct copies write size bytes.
                    if(pending_global_init_count < 256)
                        pending_global_init[pending_global_init_count++] = child;
                    int galloc = 8;
                    if(child->vardecl.type && child->vardecl.type->size > 8)
                        galloc = child->vardecl.type->size;
                    fprintf(output_file, "%s: .space %d\n", child->vardecl.name, galloc);
                }
            } else {
                if(!has_bss){
                    fprintf(output_file, ".section .bss\n");
                    has_bss = true;
                }
                int alloc = 8;
                if(child->vardecl.type && child->vardecl.type->size > 8)
                    alloc = child->vardecl.type->size;
                fprintf(output_file, "%s: .space %d\n", child->vardecl.name, alloc);
            }
        }
    }

    fprintf(output_file, ".section .text\n");
    for(uint64_t i = 0; i < ast->program_node.children->length; i++){
        Node* child = ast->program_node.children->nodes[i];
        if(child->type == ND_FUNCDEF){
            gen_funcdef(child, output_file);
        }
    }
}
