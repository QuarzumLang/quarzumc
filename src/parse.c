#include "quarzum.h"
#include <libgen.h>

static bool equal(Token t, char* value){
    if(t.kind == TT_STR || t.kind == TT_NUM) return false;
    if(t.value == NULL) return false;
    return strcmp(t.value, value) == 0;
}

static char* dir_from_path(const char* path){
    char* copy = strdup(path);
    char* dir = dirname(copy);
    char* result = strdup(dir);
    free(copy);
    return result;
}

static char* normalize_path(const char* path){
    if(strcmp(path, "/") == 0) return strdup(path);
    int absolute = path[0] == '/';
    char* copy = strdup(path);
    char* segments[256];
    int n = 0;
    char* save = NULL;
    char* tok = strtok_r(copy, "/", &save);
    while(tok){
        if(strcmp(tok, ".") == 0){
            // skip
        } else if(strcmp(tok, "..") == 0){
            if(n > 0) n--;
        } else {
            if(n < 256) segments[n++] = tok;
        }
        tok = strtok_r(NULL, "/", &save);
    }
    size_t len = (absolute ? 1 : 0);
    for(int i = 0; i < n; i++){
        len += strlen(segments[i]);
        if(i < n - 1) len += 1;
    }
    char* result = malloc(len + 1);
    char* w = result;
    if(absolute) *w++ = '/';
    for(int i = 0; i < n; i++){
        size_t sl = strlen(segments[i]);
        memcpy(w, segments[i], sl);
        w += sl;
        if(i < n - 1) *w++ = '/';
    }
    *w = '\0';
    free(copy);
    return result;
}

static char* resolve_import_path(ParserState* state, const char* raw_path){
    if(raw_path[0] == '@'){
        char* rel = malloc(strlen(LIB_PATH) + 1 + strlen(raw_path));
        sprintf(rel, "%s/%s", LIB_PATH, raw_path + 1);
        return rel;
    }
    char* result = malloc(strlen(state->current_file_dir) + 1 + strlen(raw_path) + 1);
    sprintf(result, "%s/%s", state->current_file_dir, raw_path);
    char* normalized = normalize_path(result);
    free(result);
    return normalized;
}

static Token peek(ParserState* state){
    if(state->index + 1 >= state->tokens->length){
        Token eof;
        eof.kind = TT_EOF;
        eof.value = NULL;
        return eof;
    }
    return state->tokens->tokens[state->index + 1];
}

static Token expect(ParserState* state, char* value){
    Token t = next(state);
    if(!equal(t, value)){
        fprintf(stderr, "parse error: expected '%s', got '%s' at %s:%lu:%lu\n",
                value, t.value, t.file, t.line, t.column);
        exit(1);
    }
    return t;
}

/*
    A statement terminator. A semicolon terminates the statement: either a
    user-written ';' (auto_semi == false) or a lexer-inserted ';'
    (auto_semi == true) at a newline boundary. A missing terminator (e.g.
    before '}', ')' or EOF) is also fine.
*/
static void expect_stmt_end(ParserState* state){
    if(equal(peek(state), ";")){
        next(state);
    }
}

/*
    A statement-list separator. Any ';' (user-written or lexer-inserted) is
    consumed here.
*/
static void skip_semicolons(ParserState* state){
    while(equal(peek(state), ";")){
        next(state);
    }
}

static Token expect_ident(ParserState* state){
    Token t = next(state);
    if(t.kind != TT_IDENT){
        fprintf(stderr, "parse error: expected identifier, got '%s' at %s:%lu:%lu\n",
                t.value, t.file, t.line, t.column);
        exit(1);
    }
    return t;
}

static Node* make_node(NodeType type){
    Node* node = calloc(1, sizeof(Node));
    if(node == NULL){
        perror("calloc");
        exit(1);
    }
    node->type = type;
    node->ty = NULL;
    return node;
}

static NodeList* make_nodelist(void){
    NodeList* list = malloc(sizeof(NodeList));
    list->size = 32;
    list->length = 0;
    list->nodes = malloc(32 * sizeof(Node*));
    return list;
}

SymbolTable* symbol_table_make(void){
    SymbolTable* table = malloc(sizeof(SymbolTable));
    table->size = 32;
    table->length = 0;
    table->symbols = malloc(32 * sizeof(Symbol));
    return table;
}

void symbol_table_add(SymbolTable* table, Symbol sym){
    if(table->length >= table->size){
        table->size *= 2;
        table->symbols = realloc(table->symbols, table->size * sizeof(Symbol));
    }
    table->symbols[table->length] = sym;
    table->length++;
}

Symbol* symbol_table_lookup(SymbolTable* table, const char* name){
    for(uint64_t i = table->length; i > 0; i--){
        Symbol* sym = &table->symbols[i - 1];
        if(strcmp(sym->name, name) == 0){
            return sym;
        }
    }
    return NULL;
}

static Symbol* lookup_symbol(ParserState* state, const char* name){
    Symbol* sym = symbol_table_lookup(state->scope, name);
    if(sym) return sym;
    if(state->global_scope && state->global_scope != state->scope){
        return symbol_table_lookup(state->global_scope, name);
    }
    return NULL;
}

static Node* parse_expr(ParserState* state);
static Node* parse_bitor_expr(ParserState* state);
static Node* parse_bitxor_expr(ParserState* state);
static Node* parse_bitand_expr(ParserState* state);
static Node* parse_bitnot_expr(ParserState* state);
static Node* parse_cast_expr(ParserState* state);
static Node* parse_comparison_expr(ParserState* state);
static Node* parse_add_expr(ParserState* state);
static Node* parse_mul_expr(ParserState* state);
static Node* parse_stmt(ParserState* state);
static NodeList* parse_block(ParserState* state);
static NodeList* parse_optional_block(ParserState* state);
static Type* infer_type(Node* expr, ParserState* state);
static Type* resolve_funcall_type(Node* node, ParserState* state);
static Type* parse_type_base(ParserState* state);
static Node* parse_exit_stmt(ParserState* state);
static Node* parse_funcdef(ParserState* state);
typedef struct GenericFuncTemplate GenericFuncTemplate;
static Node* parse_generic_func_template(ParserState* state, Token name);
static Type* substitute_template_type(ParserState* state, GenericFuncTemplate* tmpl, Type* tpl, Type** binds);
static Node* instantiate_generic_func(ParserState* state, GenericFuncTemplate* tmpl, Type** type_args);

static void parse_enum_def(ParserState* state){
    Token name = expect_ident(state);

    int generic_count = 0;
    char** generic_names = NULL;
    if(equal(peek(state), "<")){
        next(state);
        while(true){
            Token gp = expect_ident(state);
            generic_count++;
            generic_names = realloc(generic_names, sizeof(char*) * generic_count);
            generic_names[generic_count - 1] = gp.value;
            if(!equal(peek(state), ",")) break;
            next(state);
        }
        expect(state, ">");
    }

    expect(state, "{");

    EnumVariant* variants = NULL;
    int variant_count = 0;

    while(!equal(peek(state), "}")){
        if(variant_count > 0){
            expect(state, ",");
        }
        Token vname = expect_ident(state);
        char** param_types = NULL;
        int param_count = 0;

        if(equal(peek(state), "(")){
            next(state);
            bool closed = false;
            while(!closed){
                if(equal(peek(state), ")")){
                    next(state);
                    break;
                }
                int depth = 0;
                size_t expr_cap = 32;
                size_t expr_len = 0;
                char* expr = malloc(expr_cap);
                expr[0] = '\0';
                while(true){
                    Token pt = next(state);
                    if(pt.kind == TT_EOF){
                        fprintf(stderr, "parse error: unexpected end of file while parsing enum '%s' variant '%s'\n",
                                name.value, vname.value);
                        exit(1);
                    }
                    if(equal(pt, "(") || equal(pt, "<") || equal(pt, "[")){
                        depth++;
                    } else if(equal(pt, ">") || equal(pt, "]")){
                        depth--;
                    }
                    if(depth == 0 && equal(pt, ")")){
                        closed = true;
                        break;
                    }
                    if(depth == 0 && equal(pt, ",")){
                        break;
                    }
                    size_t vlen = strlen(pt.value);
                    if(expr_len + vlen + 1 > expr_cap){
                        while(expr_len + vlen + 1 > expr_cap) expr_cap *= 2;
                        expr = realloc(expr, expr_cap);
                    }
                    memcpy(expr + expr_len, pt.value, vlen);
                    expr_len += vlen;
                    expr[expr_len] = '\0';
                }
                param_count++;
                param_types = realloc(param_types, sizeof(char*) * param_count);
                param_types[param_count - 1] = expr;
            }
        }

        variant_count++;
        variants = realloc(variants, sizeof(EnumVariant) * variant_count);
        variants[variant_count - 1].name = vname.value;
        variants[variant_count - 1].param_type_names = param_types;
        variants[variant_count - 1].param_count = param_count;
    }
    expect(state, "}");

    EnumDef* def = malloc(sizeof(EnumDef));
    def->name = name.value;
    def->file = name.file;
    def->generic_params = generic_names;
    def->generic_param_count = generic_count;
    def->variants = variants;
    def->variant_count = variant_count;
    enum_table_add(def);
}

static Type* parse_type(ParserState* state);

static void parse_struct_def(ParserState* state){
    Token name = expect_ident(state);
    expect(state, "{");

    StructMember* members = NULL;
    int member_count = 0;
    int total_size = 0;

    while(!equal(peek(state), "}")){
        Token kw = next(state);
        bool is_const = false;
        if(equal(kw, "var")){
            is_const = false;
        } else if(equal(kw, "const")){
            is_const = true;
        } else {
            fprintf(stderr, "parse error: expected 'var' or 'const', got '%s' at %s:%lu:%lu\n",
                    kw.value, kw.file, kw.line, kw.column);
            exit(1);
        }

        Token mname = expect_ident(state);
        expect(state, ":");
        Type* mtype = parse_type(state);
        expect_stmt_end(state);

        int align = mtype->align > 0 ? mtype->align : 1;
        total_size = (total_size + align - 1) & ~(align - 1);

        member_count++;
        members = realloc(members, sizeof(StructMember) * member_count);
        members[member_count - 1].name = mname.value;
        members[member_count - 1].type = mtype;
        members[member_count - 1].is_const = is_const;
        members[member_count - 1].offset = total_size;

        total_size += mtype->size;
    }
    expect(state, "}");

    int align = 8;
    total_size = (total_size + align - 1) & ~(align - 1);

    StructDef* def = malloc(sizeof(StructDef));
    def->name = name.value;
    def->members = members;
    def->member_count = member_count;
    def->total_size = total_size;
    struct_table_add(def);
}

static Type* parse_type(ParserState* state);

static Node* parse_match_case(ParserState* state){
    expect(state, "case");
    Token variant = next(state);

    Node* label = NULL;
    if(variant.kind == TT_NUM){
        if(variant.type && is_float(variant.type)){
            Node* flit = make_node(ND_FLOATLIT);
            flit->floatlit.value = variant.float_value;
            flit->floatlit.float_kind = variant.type->kind;
            label = flit;
        } else {
            Node* ilit = make_node(ND_INTLIT);
            ilit->intlit.value = variant.int_value;
            label = ilit;
        }
    } else if(variant.kind == TT_IDENT){
        // The label may be an enum variant (no symbol) or a numeric value
        // referenced by name (a const/global). Resolve it when it is a
        // known symbol so numeric matches can dispatch on its value.
        label = make_node(ND_VARREF);
        label->varref.name = variant.value;
        Symbol* lsym = lookup_symbol(state, variant.value);
        if(lsym){
            label->varref.offset = lsym->offset;
            label->varref.is_global = lsym->is_global;
            label->varref.is_string = (lsym->type && lsym->type->kind == TY_STRING);
            label->ty = lsym->type;
        }
    } else {
        fprintf(stderr, "parse error: expected match case label, got '%s' at %s:%lu:%lu\n",
                variant.value, variant.file, variant.line, variant.column);
        exit(1);
    }

    NodeList* bindings = NULL;
    if(equal(peek(state), "(")){
        next(state);
        if(!equal(peek(state), ")")){
            bindings = make_nodelist();
            while(true){
                Token binding = expect_ident(state);

                Type* binding_type = ty_int64;
                if(equal(peek(state), ":")){
                    next(state);
                    binding_type = parse_type(state);
                }

                int alloc_size = 8;
                if(binding_type && binding_type->size > 8){
                    alloc_size = binding_type->size;
                }
                state->stack_offset -= alloc_size;
                Symbol sym = { .name = binding.value, .type = binding_type, .offset = state->stack_offset, .is_const = false };
                symbol_table_add(state->scope, sym);

                Node* binding_node = make_node(ND_VARREF);
                binding_node->varref.name = binding.value;
                binding_node->varref.offset = state->stack_offset;
                binding_node->ty = binding_type;
                nodelist_add(bindings, binding_node);
                if(!equal(peek(state), ",")) break;
                next(state);
            }
        }
        expect(state, ")");
    }

    expect(state, "=>");
    Node* body = parse_expr(state);

    Node* node = make_node(ND_MATCH_CASE);
    node->match_case.variant_name = (variant.kind == TT_IDENT) ? variant.value : NULL;
    node->match_case.bindings = bindings;
    node->match_case.label = label;
    node->match_case.body = body;
    return node;
}

static Node* parse_match_body(ParserState* state){
    Node* scrutinee = parse_expr(state);
    expect(state, "{");

    NodeList* cases = make_nodelist();
    Node* default_body = NULL;
    while(!equal(peek(state), "}")){
        if(equal(peek(state), "default")){
            next(state);
            expect(state, "=>");
            default_body = parse_expr(state);
        } else {
            nodelist_add(cases, parse_match_case(state));
        }
        if(equal(peek(state), ",")) next(state);
    }
    expect(state, "}");

    Node* node = make_node(ND_MATCH);
    node->match.scrutinee = scrutinee;
    node->match.cases = cases;
    node->match.default_body = default_body;
    node->match.result_type = NULL;
    return node;
}

static Node* parse_match_expr(ParserState* state){
    expect(state, "match");
    return parse_match_body(state);
}

/*
    Resolves the return type of a method call.

    Class methods are mangled as ClassName_methodName; the
    declared return type is recovered by scanning the class
    methods for the matching mangled name.
*/
static Type* method_call_return_type(const char* class_name, const char* method){
    if(!class_name || !method) return NULL;
    ClassDef* cdef = class_table_lookup(class_name);
    if(!cdef) return NULL;
    for(uint64_t mi = 0; mi < cdef->methods->length; mi++){
        Node* m = cdef->methods->nodes[mi];
        char* mangled = m->funcdef.name;
        char* underscore = mangled ? strchr(mangled, '_') : NULL;
        if(underscore && strcmp(underscore + 1, method) == 0){
            return m->funcdef.return_type;
        }
    }
    return NULL;
}

static Node* parse_primary(ParserState* state){
    Token t = next(state);

    if(t.kind == TT_NUM){
        if(t.type && is_float(t.type)){
            Node* node = make_node(ND_FLOATLIT);
            node->floatlit.value = t.float_value;
            node->floatlit.float_kind = t.type->kind;
            return node;
        }
        Node* node = make_node(ND_INTLIT);
        node->intlit.value = t.int_value;
        return node;
    }

    if(t.kind == TT_STR){
        Node* node = make_node(ND_STRLIT);
        node->strlit.value = t.string_value;
        node->strlit.length = unescaped_string_length(t.string_value);
        return node;
    }

    if(t.kind == TT_KEYWORD && equal(t, "true")){
        Node* node = make_node(ND_BOOLLIT);
        node->boollit.value = true;
        return node;
    }

    if(t.kind == TT_KEYWORD && equal(t, "false")){
        Node* node = make_node(ND_BOOLLIT);
        node->boollit.value = false;
        return node;
    }

    // `exit` may also appear in expression position (e.g. as a match arm
    // that terminates the program). Codegen already handles ND_EXIT.
    if(t.kind == TT_KEYWORD && equal(t, "exit")){
        return parse_exit_stmt(state);
    }

    if(t.kind == TT_KEYWORD && equal(t, "sizeof")){
        expect(state, "(");
        Type* ty = parse_type(state);
        expect(state, ")");
        Node* node = make_node(ND_INTLIT);
        node->intlit.value = ty->size;
        return node;
    }

    if(t.kind == TT_KEYWORD && equal(t, "new")){
        Token class_name = expect_ident(state);
        char* effective_name = class_name.value;

        // Check for generic instantiation: new List<int64>()
        ClassDef* cdef = class_table_lookup(class_name.value);
        if(cdef && cdef->is_generic_template && cdef->type_param_count > 0 && equal(peek(state), "<")){
            expect(state, "<");
            Type** type_args = malloc(sizeof(Type*) * cdef->type_param_count);
            for(int i = 0; i < cdef->type_param_count; i++){
                type_args[i] = parse_type(state);
                if(i < cdef->type_param_count - 1) expect(state, ",");
            }
            expect(state, ">");
            ClassDef* concrete = class_template_instantiate(state, cdef, type_args, cdef->type_param_count);
            effective_name = concrete->name;
        }

        expect(state, "(");
        NodeList* args = make_nodelist();
        if(!equal(peek(state), ")")){
            while(true){
                nodelist_add(args, parse_expr(state));
                if(!equal(peek(state), ",")) break;
                next(state);
            }
        }
        expect(state, ")");
        Node* node = make_node(ND_NEW);
        node->new_expr.class_name = effective_name;
        node->new_expr.args = args;
        // Build constructor mangled name: ClassName_new_N where N = arg count
        {
            char* mangled = malloc(strlen(effective_name) + 32);
            sprintf(mangled, "%s_new_%lu", effective_name, args->length);
            node->new_expr.mangled_name = mangled;
        }
        StructDef* new_sdef = struct_table_lookup(effective_name);
        if(new_sdef) node->ty = ref_type(struct_type(new_sdef));
        return node;
    }

    if(t.kind == TT_KEYWORD && equal(t, "null")){
        return make_node(ND_NULL);
    }

    // `pass` is a void literal: an empty statement/expression, similar to a
    // nop. It may appear wherever a void expression is expected.
    if(t.kind == TT_KEYWORD && equal(t, "pass")){
        return make_node(ND_PASS);
    }

    if(t.kind == TT_KEYWORD && equal(t, "this")){
        Node* this_node = make_node(ND_THIS);
        if(state->current_class){
            this_node->ty = ref_type(struct_type(struct_table_lookup(state->current_class->name)));
        }

        if(equal(peek(state), ".")){
            next(state);
            Token field = expect_ident(state);

            if(equal(peek(state), "(")){
                // this.method(args)
                next(state);
                NodeList* args = make_nodelist();
                if(!equal(peek(state), ")")){
                    while(true){
                        nodelist_add(args, parse_expr(state));
                        if(!equal(peek(state), ",")) break;
                        next(state);
                    }
                }
                expect(state, ")");
                Node* node = make_node(ND_METHODCALL);
                node->methodcall.object = this_node;
                node->methodcall.method = field.value;
                node->methodcall.args = args;
                node->methodcall.class_name = state->current_class ? state->current_class->name : NULL;
                node->ty = method_call_return_type(node->methodcall.class_name, node->methodcall.method);
                return node;
            }

            // this.field
            Node* base = this_node;
            Node* node = make_node(ND_MEMBER);
            node->member.base = base;
            node->member.field_name = field.value;
            node->member.field_offset = 0;
            node->member.is_struct = false;
            node->member.is_class = false;
            if(state->current_class){
                StructDef* sdef = struct_table_lookup(state->current_class->name);
                for(int i = 0; i < sdef->member_count; i++){
                    if(strcmp(sdef->members[i].name, field.value) == 0){
                        node->member.field_offset = sdef->members[i].offset;
                        node->member.is_class = true;
                        node->ty = sdef->members[i].type;
                        break;
                    }
                }
            }
            return node;
        }
        return this_node;
    }

    if(equal(t, "(")){
        Node* expr = parse_expr(state);
        expect(state, ")");
        return expr;
    }

    if(t.kind == TT_IDENT){
        // Implicit 'this' — method call (before function call fallback)
        if(state->current_class && equal(peek(state), "(")){
            NodeList* methods = state->current_class->methods;
            if(methods){
                Node* matched_method = NULL;
                for(uint64_t mi = 0; mi < methods->length; mi++){
                    Node* method = methods->nodes[mi];
                    char* mangled = method->funcdef.name;
                    char* underscore = strchr(mangled, '_');
                    if(underscore && strcmp(underscore + 1, t.value) == 0){
                        matched_method = method;
                        break;
                    }
                }
                if(matched_method){
                    next(state);
                    NodeList* args = make_nodelist();
                    if(!equal(peek(state), ")")){
                        while(true){
                            nodelist_add(args, parse_expr(state));
                            if(!equal(peek(state), ",")) break;
                            next(state);
                        }
                    }
                    expect(state, ")");
                    if(matched_method->funcdef.params->length == args->length + 1){
                        StructDef* sdef = struct_table_lookup(state->current_class->name);
                        Node* this_node = make_node(ND_THIS);
                        this_node->ty = ref_type(struct_type(sdef));
                        Node* node = make_node(ND_METHODCALL);
                        node->methodcall.object = this_node;
                        node->methodcall.method = t.value;
                        node->methodcall.args = args;
                        node->methodcall.class_name = state->current_class->name;
                        node->ty = method_call_return_type(node->methodcall.class_name, node->methodcall.method);
                        return node;
                    }
                    // Arity mismatch: the call refers to a global function
                    // that happens to share a name with a method.
                    Node* node = make_node(ND_FUNCCALL);
                    node->funcall.name = t.value;
                    node->funcall.args = args;
                    node->ty = resolve_funcall_type(node, state);
                    return node;
                }
            }
        }
        if(equal(peek(state), "(")){
            if(struct_table_lookup(t.value)){
                next(state);
                NodeList* args = make_nodelist();
                if(!equal(peek(state), ")")){
                    while(true){
                        nodelist_add(args, parse_expr(state));
                        if(!equal(peek(state), ",")) break;
                        next(state);
                    }
                }
                expect(state, ")");
                Node* node = make_node(ND_STRUCTCONS);
                node->structcons.name = t.value;
                node->structcons.args = args;
                node->structcons.temp_offset = 0;
                StructDef* csdef = struct_table_lookup(t.value);
                if(csdef){
                    int tsize = struct_type(csdef)->size;
                    int talloc = tsize > 16 ? tsize : 16;
                    state->stack_offset -= talloc;
                    node->structcons.temp_offset = state->stack_offset;
                }
                return node;
            }
            next(state);
            NodeList* args = make_nodelist();
            if(!equal(peek(state), ")")){
                while(true){
                    nodelist_add(args, parse_expr(state));
                    if(!equal(peek(state), ",")) break;
                    next(state);
                }
            }
            expect(state, ")");
            Node* node = make_node(ND_FUNCCALL);
            node->funcall.name = t.value;
            node->funcall.args = args;
            node->ty = resolve_funcall_type(node, state);
            return node;
        }
        if(equal(peek(state), ".")){
            Token dot_name = t;
            next(state);
            Token variant = expect_ident(state);

            if(enum_table_lookup(dot_name.value)){
                NodeList* args = NULL;
                if(equal(peek(state), "(")){
                    next(state);
                    args = make_nodelist();
                    if(!equal(peek(state), ")")){
                        while(true){
                            nodelist_add(args, parse_expr(state));
                            if(!equal(peek(state), ",")) break;
                            next(state);
                        }
                    }
                    expect(state, ")");
                }

                Node* node = make_node(ND_ENUMCONS);
                node->enumcons.enum_name = dot_name.value;
                node->enumcons.variant_name = variant.value;
                node->enumcons.args = args;
                node->enumcons.variant_index = -1;
                node->enumcons.resolved_type = NULL;
                return node;
            }

            Symbol* sym = lookup_symbol(state, dot_name.value);
            Node* implicit_base = NULL;
            Type* base_type = sym ? sym->type : NULL;
            if(!sym){
                // Check implicit-this for class fields
                if(state->current_class){
                    StructDef* sdef = struct_table_lookup(state->current_class->name);
                    if(sdef){
                        for(int i = 0; i < state->current_class->field_count; i++){
                            if(strcmp(state->current_class->fields[i].name, dot_name.value) == 0){
                                Node* this_node = make_node(ND_THIS);
                                this_node->ty = ref_type(struct_type(sdef));
                                implicit_base = make_node(ND_MEMBER);
                                implicit_base->member.base = this_node;
                                implicit_base->member.field_name = dot_name.value;
                                implicit_base->member.field_offset = state->current_class->fields[i].offset;
                                implicit_base->member.is_struct = false;
                                implicit_base->member.is_class = true;
                                implicit_base->ty = state->current_class->fields[i].type;
                                base_type = implicit_base->ty;
                                break;
                            }
                        }
                    }
                }
                if(!implicit_base){
                    fprintf(stderr, "parse error: undefined '%s' at %s:%lu:%lu\n",
                            dot_name.value, dot_name.file, dot_name.line, dot_name.column);
                    exit(1);
                }
            }

            // Check if this is a method call: obj.method(args)
            if(equal(peek(state), "(")){
                next(state);
                NodeList* args = make_nodelist();
                if(!equal(peek(state), ")")){
                    while(true){
                        nodelist_add(args, parse_expr(state));
                        if(!equal(peek(state), ",")) break;
                        next(state);
                    }
                }
                expect(state, ")");

                // Resolve class name from the variable's type
                char* class_name = NULL;
                if(base_type){
                    if((base_type->kind == TY_PTR || base_type->kind == TY_REF) && base_type->base &&
                       base_type->base->kind == TY_STRUCT){
                        StructDef* sdef = base_type->base->structure.struct_def;
                        if(class_table_lookup(sdef->name)){
                            class_name = sdef->name;
                        }
                    } else if(base_type->kind == TY_STRUCT){
                        StructDef* sdef = base_type->structure.struct_def;
                        if(class_table_lookup(sdef->name)){
                            class_name = sdef->name;
                        }
                    }
                }

                Node* obj;
                if(implicit_base){
                    obj = implicit_base;
                } else {
                    obj = make_node(ND_VARREF);
                    obj->varref.name = sym->name;
                    obj->varref.offset = sym->offset;
                    obj->varref.is_global = sym->is_global;
                }

                Node* node = make_node(ND_METHODCALL);
                node->methodcall.object = obj;
                node->methodcall.method = variant.value;
                node->methodcall.args = args;
                node->methodcall.class_name = class_name;
                node->ty = method_call_return_type(node->methodcall.class_name, node->methodcall.method);
                return node;
            }

            // Field access
            Node* base;
            if(implicit_base){
                base = implicit_base;
            } else {
                base = make_node(ND_VARREF);
                base->varref.name = sym->name;
                base->varref.offset = sym->offset;
                base->varref.is_global = sym->is_global;
            }
            Node* node = make_node(ND_MEMBER);
            node->member.base = base;
            node->member.field_name = variant.value;
            node->member.field_offset = 0;
            node->member.is_struct = false;
            node->member.is_class = false;

            // Check if this is a struct field access (pointer to struct)
            if(base_type && (base_type->kind == TY_PTR || base_type->kind == TY_REF) && base_type->base &&
               base_type->base->kind == TY_STRUCT){
                StructDef* sdef = base_type->base->structure.struct_def;
                for(int i = 0; i < sdef->member_count; i++){
                    if(strcmp(sdef->members[i].name, variant.value) == 0){
                        node->member.field_offset = sdef->members[i].offset;
                        node->member.is_struct = (class_table_lookup(sdef->name) == NULL);
                        node->member.is_class = (class_table_lookup(sdef->name) != NULL);
                        node->ty = sdef->members[i].type;
                        break;
                    }
                }
            }
            // Check if this is a direct struct member
            else if(base_type && base_type->kind == TY_STRUCT){
                StructDef* sdef = base_type->structure.struct_def;
                for(int i = 0; i < sdef->member_count; i++){
                    if(strcmp(sdef->members[i].name, variant.value) == 0){
                        node->member.field_offset = sdef->members[i].offset;
                        node->member.is_struct = true;
                        node->ty = sdef->members[i].type;
                        break;
                    }
                }
            }
            return node;
        }
        Symbol* sym = lookup_symbol(state, t.value);
        if(!sym){
            // Implicit 'this' — check if this is a class field
            Node* implicit_base = NULL;
            if(state->current_class){
                StructDef* sdef = struct_table_lookup(state->current_class->name);
                if(sdef){
                    for(int i = 0; i < state->current_class->field_count; i++){
                        if(strcmp(state->current_class->fields[i].name, t.value) == 0){
                            Node* this_node = make_node(ND_THIS);
                            this_node->ty = ref_type(struct_type(sdef));
                            implicit_base = make_node(ND_MEMBER);
                            implicit_base->member.base = this_node;
                            implicit_base->member.field_name = t.value;
                            implicit_base->member.field_offset = state->current_class->fields[i].offset;
                            implicit_base->member.is_struct = false;
                            implicit_base->member.is_class = true;
                            implicit_base->ty = state->current_class->fields[i].type;
                            break;
                        }
                    }
                }
            }
            if(!implicit_base){
                fprintf(stderr, "parse error: undefined variable '%s' at %s:%lu:%lu\n",
                        t.value, t.file, t.line, t.column);
                exit(1);
            }
            if(equal(peek(state), "[")){
                next(state);
                Node* idx = parse_expr(state);
                expect(state, "]");
                int elem_size = 8;
                if(implicit_base->ty && implicit_base->ty->base){
                    elem_size = implicit_base->ty->base->size;
                }
                bool is_string_elem = implicit_base->ty && implicit_base->ty->base &&
                                      (implicit_base->ty->base->kind == TY_STRING ||
                                       implicit_base->ty->base->kind == TY_ARRAY);
                Node* node = make_node(ND_INDEX);
                node->index_expr.base = implicit_base;
                node->index_expr.index = idx;
                node->index_expr.elem_size = elem_size;
                node->index_expr.is_string = is_string_elem;
                node->ty = implicit_base->ty ? implicit_base->ty->base : NULL;
                return node;
            }
            if(equal(peek(state), ".")){
                next(state);
                Token field = expect_ident(state);
                if(equal(peek(state), "(")){
                    next(state);
                    NodeList* args = make_nodelist();
                    if(!equal(peek(state), ")")){
                        while(true){
                            nodelist_add(args, parse_expr(state));
                            if(!equal(peek(state), ",")) break;
                            next(state);
                        }
                    }
                    expect(state, ")");
                    char* class_name = NULL;
                    Type* base_type = implicit_base->ty;
                    if(base_type){
                        if((base_type->kind == TY_PTR || base_type->kind == TY_REF) && base_type->base && base_type->base->kind == TY_STRUCT){
                            StructDef* sdef = base_type->base->structure.struct_def;
                            if(class_table_lookup(sdef->name)) class_name = sdef->name;
                        } else if(base_type->kind == TY_STRUCT){
                            StructDef* sdef = base_type->structure.struct_def;
                            if(class_table_lookup(sdef->name)) class_name = sdef->name;
                        }
                    }
                    Node* node = make_node(ND_METHODCALL);
                    node->methodcall.object = implicit_base;
                    node->methodcall.method = field.value;
                    node->methodcall.args = args;
                    node->methodcall.class_name = class_name;
                    node->ty = method_call_return_type(node->methodcall.class_name, node->methodcall.method);
                    return node;
                }
                Node* node = make_node(ND_MEMBER);
                node->member.base = implicit_base;
                node->member.field_name = field.value;
                node->member.field_offset = 0;
                node->member.is_struct = false;
                node->member.is_class = false;
                if(implicit_base->ty && (implicit_base->ty->kind == TY_PTR || implicit_base->ty->kind == TY_REF) && implicit_base->ty->base &&
                   implicit_base->ty->base->kind == TY_STRUCT){
                    StructDef* sdef = implicit_base->ty->base->structure.struct_def;
                    if(class_table_lookup(sdef->name)){
                        for(int j = 0; j < sdef->member_count; j++){
                            if(strcmp(sdef->members[j].name, field.value) == 0){
                                node->member.field_offset = sdef->members[j].offset;
                                node->member.is_class = true;
                                node->ty = sdef->members[j].type;
                                break;
                            }
                        }
                    }
                } else if(implicit_base->ty && implicit_base->ty->kind == TY_STRUCT){
                    StructDef* sdef = implicit_base->ty->structure.struct_def;
                    for(int j = 0; j < sdef->member_count; j++){
                        if(strcmp(sdef->members[j].name, field.value) == 0){
                            node->member.field_offset = sdef->members[j].offset;
                            node->member.is_struct = true;
                            node->ty = sdef->members[j].type;
                            break;
                        }
                    }
                }
                return node;
            }
            return implicit_base;
        }
        if(equal(peek(state), "[")){
            next(state);
            Node* idx = parse_expr(state);
            expect(state, "]");

            int elem_size = 8;
            if(sym->type && sym->type->base){
                elem_size = sym->type->base->size;
            }

            bool is_string_elem = sym->type && sym->type->base &&
                                  (sym->type->base->kind == TY_STRING ||
                                   sym->type->base->kind == TY_ARRAY);

            Node* base = make_node(ND_VARREF);
            base->varref.name = sym->name;
            base->varref.offset = sym->offset;
            base->varref.is_global = sym->is_global;
            base->ty = sym->type;

            Node* node = make_node(ND_INDEX);
            node->index_expr.base = base;
            node->index_expr.index = idx;
            node->index_expr.elem_size = elem_size;
            node->index_expr.is_string = is_string_elem;
            node->ty = sym->type ? sym->type->base : NULL;
            return node;
        }
        if(equal(peek(state), ".")){
            next(state);
            Token field = expect_ident(state);

            // Check if this is a method call: obj.method(args)
            if(equal(peek(state), "(")){
                next(state);
                NodeList* args = make_nodelist();
                if(!equal(peek(state), ")")){
                    while(true){
                        nodelist_add(args, parse_expr(state));
                        if(!equal(peek(state), ",")) break;
                        next(state);
                    }
                }
                expect(state, ")");

                char* class_name = NULL;
                if(sym->type && (sym->type->kind == TY_PTR || sym->type->kind == TY_REF) && sym->type->base &&
                   sym->type->base->kind == TY_STRUCT){
                    StructDef* sdef = sym->type->base->structure.struct_def;
                    if(class_table_lookup(sdef->name)){
                        class_name = sdef->name;
                    }
                }

                Node* obj = make_node(ND_VARREF);
                obj->varref.name = sym->name;
                obj->varref.offset = sym->offset;
                obj->varref.is_global = sym->is_global;

                Node* node = make_node(ND_METHODCALL);
                node->methodcall.object = obj;
                node->methodcall.method = field.value;
                node->methodcall.args = args;
                node->methodcall.class_name = class_name;
                node->ty = method_call_return_type(node->methodcall.class_name, node->methodcall.method);
                return node;
            }

            // Field access
            Node* base = make_node(ND_VARREF);
            base->varref.name = sym->name;
            base->varref.offset = sym->offset;
            base->varref.is_global = sym->is_global;
            Node* node = make_node(ND_MEMBER);
            node->member.base = base;
            node->member.field_name = field.value;
            node->member.field_offset = 0;
            node->member.is_struct = false;
            node->member.is_class = false;

            // Check if class field access (pointer to class)
            if(sym->type && (sym->type->kind == TY_PTR || sym->type->kind == TY_REF) && sym->type->base &&
               sym->type->base->kind == TY_STRUCT){
                StructDef* sdef = sym->type->base->structure.struct_def;
                if(class_table_lookup(sdef->name)){
                    for(int i = 0; i < sdef->member_count; i++){
                        if(strcmp(sdef->members[i].name, field.value) == 0){
                            node->member.field_offset = sdef->members[i].offset;
                            node->member.is_class = true;
                            break;
                        }
                    }
                }
            }
            // Check if direct struct member
            else if(sym->type && sym->type->kind == TY_STRUCT){
                StructDef* sdef = sym->type->structure.struct_def;
                for(int i = 0; i < sdef->member_count; i++){
                    if(strcmp(sdef->members[i].name, field.value) == 0){
                        node->member.field_offset = sdef->members[i].offset;
                        node->member.is_struct = true;
                        break;
                    }
                }
            }
            return node;
        }
        Node* node = make_node(ND_VARREF);
        node->varref.name = sym->name;
        node->varref.offset = sym->offset;
        node->varref.is_string = (sym->type && sym->type->kind == TY_STRING);
        node->varref.is_global = sym->is_global;
        node->ty = sym->type;
        return node;
    }

    fprintf(stderr, "parse error: expected expression, got '%s' at %s:%lu:%lu\n",
            t.value, t.file, t.line, t.column);
    exit(1);
}

static Node* parse_not_expr(ParserState* state){
    if(equal(peek(state), "match")){
        return parse_match_expr(state);
    }
    if(equal(peek(state), "not")){
        next(state);
        Node* operand = parse_not_expr(state);
        Node* node = make_node(ND_UNARY_EXPR);
        node->unary_expr.op = OP_NOT;
        node->unary_expr.operand = operand;
        return node;
    }
    if(equal(peek(state), "alloc")){
        next(state);
        Type* alloc_type = parse_type_base(state);
        Node* count = NULL;
        if(equal(peek(state), "[")){
            next(state);
            count = parse_expr(state);
            expect(state, "]");
        }
        Node* node = make_node(ND_ALLOC);
        node->alloc.alloc_type = alloc_type;
        node->alloc.count = count;
        return node;
    }
    return parse_bitor_expr(state);
}

static Node* parse_bitor_expr(ParserState* state){
    Node* lhs = parse_bitxor_expr(state);
    while(equal(peek(state), "||")){
        next(state);
        Node* rhs = parse_bitxor_expr(state);
        Node* node = make_node(ND_BINARY_EXPR);
        node->binary_expr.op = OP_BITOR;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

static Node* parse_bitxor_expr(ParserState* state){
    Node* lhs = parse_bitand_expr(state);
    while(equal(peek(state), "^^")){
        next(state);
        Node* rhs = parse_bitand_expr(state);
        Node* node = make_node(ND_BINARY_EXPR);
        node->binary_expr.op = OP_BITXOR;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

static Node* parse_bitand_expr(ParserState* state){
    Node* lhs = parse_bitnot_expr(state);
    while(equal(peek(state), "&&")){
        next(state);
        Node* rhs = parse_bitnot_expr(state);
        Node* node = make_node(ND_BINARY_EXPR);
        node->binary_expr.op = OP_BITAND;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

static Node* parse_bitnot_expr(ParserState* state){
    if(equal(peek(state), "!!")){
        next(state);
        Node* operand = parse_bitnot_expr(state);
        Node* node = make_node(ND_UNARY_EXPR);
        node->unary_expr.op = OP_BITNOT;
        node->unary_expr.operand = operand;
        return node;
    }
    return parse_comparison_expr(state);
}

static Node* parse_cast_expr(ParserState* state){
    if(equal(peek(state), "-")){
        next(state);
        Node* operand = parse_cast_expr(state);
        Node* node = make_node(ND_UNARY_EXPR);
        node->unary_expr.op = OP_NEG;
        node->unary_expr.operand = operand;
        return node;
    }
    if(equal(peek(state), "*")){
        next(state);
        Node* operand = parse_cast_expr(state);
        Node* node = make_node(ND_UNARY_EXPR);
        node->unary_expr.op = OP_DEREF;
        node->unary_expr.operand = operand;
        // Derive the pointee type at parse time so that struct-typed locals
        // initialized from a deref (e.g. `var old = *(off as ptr<T>)` in a
        // generic List<T>) reserve the real struct size in the stack frame.
        Type* deref_ty = NULL;
        if(operand->ty){
            deref_ty = operand->ty->base;
        } else if(operand->type == ND_CAST && operand->cast.target_type &&
                  (operand->cast.target_type->kind == TY_PTR ||
                   operand->cast.target_type->kind == TY_REF)){
            deref_ty = operand->cast.target_type->base;
        }
        node->ty = deref_ty;
        return node;
    }
    if(equal(peek(state), "&")){
        next(state);
        Node* operand = parse_cast_expr(state);
        Node* node = make_node(ND_UNARY_EXPR);
        node->unary_expr.op = OP_ADDR;
        node->unary_expr.operand = operand;
        return node;
    }
    Node* lhs = parse_primary(state);
    while(true){
        if(equal(peek(state), ".")){
            next(state);
            Token field = expect_ident(state);
            if(equal(peek(state), "(")){
                next(state);
                NodeList* args = make_nodelist();
                if(!equal(peek(state), ")")){
                    while(true){
                        nodelist_add(args, parse_expr(state));
                        if(!equal(peek(state), ",")) break;
                        next(state);
                    }
                }
                expect(state, ")");
                char* class_name = NULL;
                if(lhs->ty){
                    if((lhs->ty->kind == TY_PTR || lhs->ty->kind == TY_REF) && lhs->ty->base && lhs->ty->base->kind == TY_STRUCT){
                        StructDef* sdef = lhs->ty->base->structure.struct_def;
                        if(class_table_lookup(sdef->name)) class_name = sdef->name;
                    } else if(lhs->ty->kind == TY_STRUCT){
                        StructDef* sdef = lhs->ty->structure.struct_def;
                        if(class_table_lookup(sdef->name)) class_name = sdef->name;
                    }
                }
                Node* node = make_node(ND_METHODCALL);
                node->methodcall.object = lhs;
                node->methodcall.method = field.value;
                node->methodcall.args = args;
                node->methodcall.class_name = class_name;
                node->ty = method_call_return_type(node->methodcall.class_name, node->methodcall.method);
                lhs = node;
                continue;
            }
            Node* node = make_node(ND_MEMBER);
            node->member.base = lhs;
            node->member.field_name = field.value;
            node->member.field_offset = 0;
            node->member.is_struct = false;
            node->member.is_class = false;
            if(lhs->ty && (lhs->ty->kind == TY_PTR || lhs->ty->kind == TY_REF) && lhs->ty->base &&
               lhs->ty->base->kind == TY_STRUCT){
                StructDef* sdef = lhs->ty->base->structure.struct_def;
                if(class_table_lookup(sdef->name)){
                    for(int j = 0; j < sdef->member_count; j++){
                        if(strcmp(sdef->members[j].name, field.value) == 0){
                            node->member.field_offset = sdef->members[j].offset;
                            node->member.is_class = true;
                            node->ty = sdef->members[j].type;
                            break;
                        }
                    }
                }
            } else if(lhs->ty && lhs->ty->kind == TY_STRUCT){
                StructDef* sdef = lhs->ty->structure.struct_def;
                for(int j = 0; j < sdef->member_count; j++){
                    if(strcmp(sdef->members[j].name, field.value) == 0){
                        node->member.field_offset = sdef->members[j].offset;
                        node->member.is_struct = true;
                        node->ty = sdef->members[j].type;
                        break;
                    }
                }
            }
            lhs = node;
            continue;
        }
        if(equal(peek(state), "[")){
            next(state);
            Node* idx = parse_expr(state);
            expect(state, "]");
            int elem_size = 8;
            if(lhs->ty && lhs->ty->base){
                elem_size = lhs->ty->base->size;
            }
            bool is_string_elem = lhs->ty && lhs->ty->base &&
                                  (lhs->ty->base->kind == TY_STRING ||
                                   lhs->ty->base->kind == TY_ARRAY);
            Node* node = make_node(ND_INDEX);
            node->index_expr.base = lhs;
            node->index_expr.index = idx;
            node->index_expr.elem_size = elem_size;
            node->index_expr.is_string = is_string_elem;
            node->ty = lhs->ty ? lhs->ty->base : NULL;
            lhs = node;
            continue;
        }
        break;
    }
    if(equal(peek(state), "as")){
        next(state);
        Type* target_type = parse_type(state);
        Node* node = make_node(ND_CAST);
        node->cast.expr = lhs;
        node->cast.target_type = target_type;
        return node;
    }
    return lhs;
}

static Token peek_at(ParserState* state, uint64_t ahead){
    if(state->index + 1 + ahead >= state->tokens->length){
        Token eof;
        eof.kind = TT_EOF;
        eof.value = NULL;
        return eof;
    }
    return state->tokens->tokens[state->index + 1 + ahead];
}

/*
    Enum pattern in an equality comparison (enum + binding):

        myOptional == Optional.Some(value: string)
        result != Result.Err(code: int64)

    The parenthesized list declares bindings instead of constructor
    arguments, so the variant payload is unpacked into fresh stack slots
    that stay visible while the enclosing if/while body is parsed.

    Detection is done with lookahead only; when the tokens do not form a
    pattern the stream is left untouched so normal expression parsing
    proceeds (e.g. plain constructor calls like `Some(42)`).
*/
static Node* try_parse_enum_pattern(ParserState* state, bool negated){
    Token e0 = peek_at(state, 0);
    if(e0.kind != TT_IDENT || !enum_table_lookup(e0.value)) return NULL;

    Token d1 = peek_at(state, 1);
    if(!(d1.kind == TT_PUNCT && equal(d1, "."))) return NULL;

    Token v2 = peek_at(state, 2);
    if(v2.kind != TT_IDENT) return NULL;

    Token p3 = peek_at(state, 3);
    if(!(p3.kind == TT_PUNCT && equal(p3, "("))) return NULL;

    // Binding-style contents? A constructor argument list can never start
    // with `ident :`, so this is unambiguous.
    Token b4 = peek_at(state, 4);
    Token c5 = peek_at(state, 5);
    if(!(b4.kind == TT_IDENT && c5.kind == TT_PUNCT && equal(c5, ":"))) return NULL;

    // Commit: consume Enum . Variant (
    next(state); next(state); next(state); next(state);

    NodeList* bindings = make_nodelist();
    while(true){
        Token binding = expect_ident(state);
        expect(state, ":");
        Type* binding_type = parse_type(state);

        int alloc_size = 8;
        if(binding_type && binding_type->size > 8){
            alloc_size = binding_type->size;
        }
        state->stack_offset -= alloc_size;
        Symbol sym = { .name = binding.value, .type = binding_type, .offset = state->stack_offset, .is_const = false };
        symbol_table_add(state->scope, sym);

        Node* binding_node = make_node(ND_VARREF);
        binding_node->varref.name = binding.value;
        binding_node->varref.offset = state->stack_offset;
        binding_node->ty = binding_type;
        nodelist_add(bindings, binding_node);

        if(!equal(peek(state), ",")) break;
        next(state);
    }
    expect(state, ")");

    Node* node = make_node(ND_ENUM_PATTERN_CMP);
    node->enum_pattern_cmp.lhs = NULL;
    node->enum_pattern_cmp.bindings = bindings;
    node->enum_pattern_cmp.enum_name = e0.value;
    node->enum_pattern_cmp.variant_name = v2.value;
    node->enum_pattern_cmp.variant_index = -1;
    node->enum_pattern_cmp.resolved_type = NULL;
    node->enum_pattern_cmp.negated = negated;
    return node;
}

static Node* parse_comparison_expr(ParserState* state){
    Node* lhs = parse_add_expr(state);
    while(equal(peek(state), "==") || equal(peek(state), "!=") ||
          equal(peek(state), "<")  || equal(peek(state), "<=") ||
          equal(peek(state), ">")  || equal(peek(state), ">=")){
        Token op = next(state);

        // `Enum.Variant(binding: Type, ...)` on the RHS of == / != binds
        // the variant payload directly inside if/while conditions.
        if(equal(op, "==") || equal(op, "!=")){
            Node* pattern = try_parse_enum_pattern(state, equal(op, "!="));
            if(pattern){
                pattern->enum_pattern_cmp.lhs = lhs;
                lhs = pattern;
                continue;
            }
        }

        Node* rhs = parse_add_expr(state);

        if(equal(op, "==") || equal(op, "!=")){
            Type* lt = infer_type(lhs, state);
            Type* rt = infer_type(rhs, state);
            if(lt && rt && is_string_type(lt) && is_string_type(rt)){
                Node* node = make_node(equal(op, "==") ? ND_STRING_EQ : ND_STRING_NE);
                node->string_cmp.lhs = lhs;
                node->string_cmp.rhs = rhs;
                lhs = node;
                continue;
            }
        }

        Node* node = make_node(ND_BINARY_EXPR);
        if(equal(op, "=="))       node->binary_expr.op = OP_EQ;
        else if(equal(op, "!="))  node->binary_expr.op = OP_NE;
        else if(equal(op, "<"))   node->binary_expr.op = OP_LT;
        else if(equal(op, "<="))  node->binary_expr.op = OP_LE;
        else if(equal(op, ">"))   node->binary_expr.op = OP_GT;
        else                      node->binary_expr.op = OP_GE;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

static Node* parse_add_expr(ParserState* state){
    Node* lhs = parse_mul_expr(state);
    while(equal(peek(state), "+") || equal(peek(state), "-")){
        Token op = next(state);
        Node* rhs = parse_mul_expr(state);
        Node* node = make_node(ND_BINARY_EXPR);
        if(equal(op, "+"))      node->binary_expr.op = OP_ADD;
        else                    node->binary_expr.op = OP_SUB;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

static Node* parse_mul_expr(ParserState* state){
    Node* lhs = parse_cast_expr(state);
    while(equal(peek(state), "*") || equal(peek(state), "/") || equal(peek(state), "%")){
        Token op = next(state);
        Node* rhs = parse_cast_expr(state);
        Node* node = make_node(ND_BINARY_EXPR);
        if(equal(op, "*"))      node->binary_expr.op = OP_MUL;
        else if(equal(op, "/")) node->binary_expr.op = OP_DIV;
        else                    node->binary_expr.op = OP_MOD;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

static Node* parse_and_expr(ParserState* state){
    Node* lhs = parse_not_expr(state);
    while(equal(peek(state), "and")){
        next(state);
        Node* rhs = parse_not_expr(state);
        Node* node = make_node(ND_BINARY_EXPR);
        node->binary_expr.op = OP_AND;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

static Node* parse_xor_expr(ParserState* state){
    Node* lhs = parse_and_expr(state);
    while(equal(peek(state), "xor")){
        next(state);
        Node* rhs = parse_and_expr(state);
        Node* node = make_node(ND_BINARY_EXPR);
        node->binary_expr.op = OP_XOR;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

static Node* parse_expr(ParserState* state){
    Node* lhs = parse_xor_expr(state);
    while(equal(peek(state), "or")){
        next(state);
        Node* rhs = parse_xor_expr(state);
        Node* node = make_node(ND_BINARY_EXPR);
        node->binary_expr.op = OP_OR;
        node->binary_expr.lhs = lhs;
        node->binary_expr.rhs = rhs;
        lhs = node;
    }
    if(equal(peek(state), "?")){
        next(state);
        Node* then_expr = parse_expr(state);
        expect(state, ":");
        Node* else_expr = parse_expr(state);
        Node* node = make_node(ND_TERNARY);
        node->ternary_expr.cond = lhs;
        node->ternary_expr.then_expr = then_expr;
        node->ternary_expr.else_expr = else_expr;
        return node;
    }
    return lhs;
}

static NodeList* parse_block(ParserState* state){
    expect(state, "{");
    NodeList* body = make_nodelist();
    while(true){
        if(equal(peek(state), "}")) break;
        skip_semicolons(state);
        if(equal(peek(state), "}")) break;
        nodelist_add(body, parse_stmt(state));
    }
    expect(state, "}");
    return body;
}

static NodeList* parse_optional_block(ParserState* state){
    if(equal(peek(state), "{"))
        return parse_block(state);
    NodeList* body = make_nodelist();
    nodelist_add(body, parse_stmt(state));
    return body;
}

static Node* parse_if_stmt(ParserState* state){
    expect(state, "(");
    Node* cond = parse_expr(state);
    expect(state, ")");
    NodeList* then_body = parse_optional_block(state);

    NodeList* else_body = NULL;
    if(equal(peek(state), "else")){
        next(state);
        else_body = parse_optional_block(state);
    }

    Node* node = make_node(ND_IF);
    node->if_stmt.cond = cond;
    node->if_stmt.then_body = then_body;
    node->if_stmt.else_body = else_body;
    return node;
}

static Type* parse_type(ParserState* state);

static Type* parse_type_base(ParserState* state){
    Token t = next(state);
    if(t.kind == TT_IDENT){
        // Check for generic class instantiation: List<int64>
        ClassDef* cdef = class_table_lookup(t.value);
        if(cdef && cdef->is_generic_template && cdef->type_param_count > 0 && equal(peek(state), "<")){
            expect(state, "<");
            Type** type_args = malloc(sizeof(Type*) * cdef->type_param_count);
            for(int i = 0; i < cdef->type_param_count; i++){
                type_args[i] = parse_type(state);
                if(i < cdef->type_param_count - 1) expect(state, ",");
            }
            expect(state, ">");
            // Instantiate the generic class
            ClassDef* concrete = class_template_instantiate(state, cdef, type_args, cdef->type_param_count);
            StructDef* sdef = struct_table_lookup(concrete->name);
            // A class name denotes a reference to a heap object (same type as ref<Class>)
            if(sdef) return state->suppress_class_ref ? struct_type(sdef) : ref_type(struct_type(sdef));
            fprintf(stderr, "internal error: concrete class '%s' has no struct\n", concrete->name);
            exit(1);
        }

        // Check if this is a generic type parameter (e.g., T in trait Collection<T>)
        if(state->current_trait && state->current_trait_generic_count > 0){
            for(int i = 0; i < state->current_trait_generic_count; i++){
                if(state->current_trait_generic_names[i] && t.value && strcmp(t.value, state->current_trait_generic_names[i]) == 0){
                    return ty_type_param;
                }
            }
        }

        // Check if this is a generic type parameter of a generic function
        // template (e.g., T in `function unwrap<T, E>(result: Result<T, E>)`).
        if(state->current_func_generic_count > 0){
            for(int i = 0; i < state->current_func_generic_count; i++){
                if(state->current_func_generic_names[i] && t.value && strcmp(t.value, state->current_func_generic_names[i]) == 0){
                    return make_type_param_type(t.value);
                }
            }
        }

        // Handle 'ptr' as substituted type param (e.g., T → ptr from a trait)
        if(strcmp(t.value, "ptr") == 0){
            if(equal(peek(state), "<")){
                expect(state, "<");
                state->suppress_class_ref += 1;
                Type* base = parse_type(state);
                state->suppress_class_ref -= 1;
                expect(state, ">");
                return pointer_to(base);
            }
            return pointer_to(ty_void);
        }

        // Handle 'ref' as substituted type param (e.g., T → ref from a trait)
        if(strcmp(t.value, "ref") == 0){
            expect(state, "<");
            state->suppress_class_ref += 1;
            Type* base = parse_type(state);
            state->suppress_class_ref -= 1;
            expect(state, ">");
            return ref_type(base);
        }

        StructDef* sdef = struct_table_lookup(t.value);
        if(sdef){
            // A class name denotes a reference to a heap object (same type as ref<Class>);
            // a struct name denotes a value passed by value.
            if(class_table_lookup(t.value)){
                return state->suppress_class_ref ? struct_type(sdef) : ref_type(struct_type(sdef));
            }
            return struct_type(sdef);
        }
        TraitDef* trait = trait_table_lookup(t.value);
        if(trait){
            return pointer_to(ty_void);
        }
        EnumDef* def = enum_table_lookup(t.value);
        if(!def){
            fprintf(stderr, "parse error: unknown type '%s' at %s:%lu:%lu\n",
                    t.value, t.file, t.line, t.column);
            exit(1);
        }
        if(def->generic_param_count > 0){
            expect(state, "<");
            Type** type_args = malloc(sizeof(Type*) * def->generic_param_count);
            for(int i = 0; i < def->generic_param_count; i++){
                type_args[i] = parse_type(state);
                if(i < def->generic_param_count - 1) expect(state, ",");
            }
            expect(state, ">");
            Type* base = enum_def_type(def);
            return enum_instantiate(state, base, type_args, def->generic_param_count);
        }
        return enum_def_type(def);
    }
    // Function type: (T1, T2) => ReturnType
    if(equal(t, "(")){
        NodeList* param_types = make_nodelist();
        if(!equal(peek(state), ")")){
            while(true){
                Type* pt = parse_type(state);
                Node* temp = make_node(ND_PARAM);
                temp->param.type = pt;
                nodelist_add(param_types, temp);
                if(!equal(peek(state), ",")) break;
                next(state);
            }
        }
        expect(state, ")");
        expect(state, "=>");
        Type* return_type = parse_type(state);
        Type* func_type = copy_type(&(Type){TY_FUNC, 8, 8});
        func_type->function.return_type = return_type;
        func_type->function.params = malloc(sizeof(Type) * param_types->length);
        func_type->function.param_count = param_types->length;
        for(uint64_t i = 0; i < param_types->length; i++){
            func_type->function.params[i] = *(param_types->nodes[i]->param.type);
        }
        return func_type;
    }

    if(t.kind != TT_KEYWORD){
        goto bad;
    }
    if(equal(t, "void"))   return ty_void;
    if(equal(t, "bool"))   return ty_bool;
    if(equal(t, "char"))   return ty_char;
    if(equal(t, "int"))    return ty_int32;
    if(equal(t, "int8"))   return ty_int8;
    if(equal(t, "int16"))  return ty_int16;
    if(equal(t, "int32"))  return ty_int32;
    if(equal(t, "int64"))  return ty_int64;
    if(equal(t, "uint"))   return ty_uint32;
    if(equal(t, "uint8"))  return ty_uint8;
    if(equal(t, "uint16")) return ty_uint16;
    if(equal(t, "uint32")) return ty_uint32;
    if(equal(t, "uint64")) return ty_uint64;
    if(equal(t, "float"))  return ty_float32;
    if(equal(t, "float32")) return ty_float32;
    if(equal(t, "float64")) return ty_float64;
    if(equal(t, "string"))  return string_type();
    if(equal(t, "ptr")){
        expect(state, "<");
        Type* base = parse_type(state);
        expect(state, ">");
        return pointer_to(base);
    }
    if(equal(t, "ref")){
        expect(state, "<");
        Type* base = parse_type(state);
        expect(state, ">");
        return ref_type(base);
    }
bad:
    fprintf(stderr, "parse error: expected type, got '%s' at %s:%lu:%lu\n",
            t.value, t.file, t.line, t.column);
    exit(1);
}

static Type* parse_type(ParserState* state){
    Type* ty = parse_type_base(state);
    while(equal(peek(state), "[")){
        next(state);
        expect(state, "]");
        ty = dynarray_type(ty);
    }
    return ty;
}

static void resolve_enumcons(ParserState* state, Node* node, Type* expected);

static Node* parse_vardecl(ParserState* state, bool is_const, bool is_global, bool consume_term){
    Token name = expect_ident(state);

    if(lookup_symbol(state, name.value)){
        fprintf(stderr, "parse error: duplicate symbol '%s' at %s:%lu:%lu\n",
                name.value, name.file, name.line, name.column);
        exit(1);
    }

    Type* type = NULL;
    Node* init = NULL;

    if(equal(peek(state), ":")){
        next(state);
        type = parse_type(state);
    }

    if(equal(peek(state), "=")){
        next(state);
        init = parse_expr(state);
        if(init->type == ND_ENUMCONS){
            resolve_enumcons(state, init, type);
        }
        // Infer type from new expression if no explicit type
        if(!type && init->type == ND_NEW){
            StructDef* sdef = struct_table_lookup(init->new_expr.class_name);
            if(sdef) type = ref_type(struct_type(sdef));
        }
        if(!type && init->type == ND_STRUCTCONS){
            StructDef* sdef = struct_table_lookup(init->structcons.name);
            if(sdef) type = struct_type(sdef);
        }
        if(!type && init->type == ND_STRLIT){
            type = string_type();
        }
        if(!type && init->type == ND_MATCH){
            type = infer_type(init, state);
        }
        if(!type && init->type == ND_METHODCALL && init->methodcall.class_name){
            ClassDef* cdef = class_table_lookup(init->methodcall.class_name);
            if(cdef){
                for(uint64_t mi = 0; mi < cdef->methods->length; mi++){
                    Node* method = cdef->methods->nodes[mi];
                    char* mangled = method->funcdef.name;
                    char* underscore = strchr(mangled, '_');
                    if(underscore && strcmp(underscore + 1, init->methodcall.method) == 0){
                        type = method->funcdef.return_type;
                        break;
                    }
                }
            }
        }
        if(!type && init->ty){
            type = init->ty;
        }
    }

    if(consume_term)
        expect_stmt_end(state);

    int alloc_size = 16;
    if(type && type->size > 16){
        alloc_size = type->size;
    }
    if(name.value && strstr(name.value, "importedFile")){
        fprintf(stderr, "DEBUG importedFile: type=%s size=%d alloc=%d prev_off=%d\n",
                type ? type_kind_to_name(type) : "NULL", type ? type->size : -1,
                alloc_size, state->stack_offset);
    }
    if(!type && !is_global){
    //    fprintf(stderr, "DEBUG NULLTYPE local=%s alloc=%d\n", name.value, alloc_size);
    }
    state->stack_offset -= alloc_size;
    Symbol sym = { .name = name.value, .type = type, .offset = state->stack_offset, .is_const = is_const, .is_global = is_global };
    symbol_table_add(state->scope, sym);

    Node* node = make_node(ND_VARDECL);
    node->vardecl.name = name.value;
    node->vardecl.type = type;
    node->vardecl.init = init;
    node->vardecl.is_const = is_const;
    node->vardecl.is_global = is_global;
    node->vardecl.offset = state->stack_offset;
    return node;
}

static Node* parse_exit_stmt(ParserState* state){
    Node* expr = parse_expr(state);
    expect_stmt_end(state);

    Node* node = make_node(ND_EXIT);
    node->exit_stmt.expr = expr;
    return node;
}

static Node* parse_return_stmt(ParserState* state){
    Node* expr = NULL;
    if(!equal(peek(state), ";") && !equal(peek(state), "}")){
        expr = parse_expr(state);
        // Resolve enum constructor types using the current function's return type
        if(expr && expr->type == ND_ENUMCONS && !expr->enumcons.resolved_type && state->current_return_type){
            resolve_enumcons(state, expr, state->current_return_type);
        }
    }
    expect_stmt_end(state);

    Node* node = make_node(ND_RETURN);
    node->return_stmt.expr = expr;
    return node;
}

static Type* infer_type(Node* expr, ParserState* state){
    if(expr->type == ND_BOOLLIT) return ty_bool;
    if(expr->type == ND_INTLIT) return ty_int64;
    if(expr->type == ND_FLOATLIT){
        switch(expr->floatlit.float_kind){
            case TY_FLOAT32: return ty_float32;
            case TY_FLOAT64: return ty_float64;
            default: return ty_float32;
        }
    }
    if(expr->type == ND_VARREF){
        Symbol* sym = lookup_symbol(state, expr->varref.name);
        if(sym) return sym->type;
    }
    if(expr->type == ND_MEMBER){
        Type* base_type = infer_type(expr->member.base, state);
        if(base_type){
            if(base_type->kind == TY_ARRAY){
                if(strcmp(expr->member.field_name, "length") == 0) return ty_uint64;
                if(strcmp(expr->member.field_name, "pointer") == 0 ||
                   strcmp(expr->member.field_name, "ptr") == 0) return pointer_to(base_type->base);
            }
            if(base_type->kind == TY_STRING){
                if(strcmp(expr->member.field_name, "length") == 0) return ty_uint64;
                if(strcmp(expr->member.field_name, "pointer") == 0 ||
                   strcmp(expr->member.field_name, "ptr") == 0) return pointer_to(ty_char);
            }
            StructDef* sdef = NULL;
            if(base_type->kind == TY_STRUCT){
                sdef = base_type->structure.struct_def;
            } else if(base_type->kind == TY_PTR && base_type->base &&
                      base_type->base->kind == TY_STRUCT){
                sdef = base_type->base->structure.struct_def;
            }
            if(sdef){
                for(uint64_t mi = 0; mi < sdef->member_count; mi++){
                    if(strcmp(sdef->members[mi].name, expr->member.field_name) == 0){
                        return sdef->members[mi].type;
                    }
                }
            }
        }
    }
    if(expr->type == ND_METHODCALL && expr->methodcall.class_name){
        ClassDef* cdef = class_table_lookup(expr->methodcall.class_name);
        if(cdef){
            for(uint64_t mi = 0; mi < cdef->methods->length; mi++){
                Node* method = cdef->methods->nodes[mi];
                char* mangled = method->funcdef.name;
                char* underscore = strchr(mangled, '_');
                if(underscore && strcmp(underscore + 1, expr->methodcall.method) == 0){
                    return method->funcdef.return_type;
                }
            }
        }
    }
    if(expr->type == ND_FUNCCALL){
        if(expr->ty) return expr->ty;
        return resolve_funcall_type(expr, state);
    }
    if(expr->type == ND_NEW){
        StructDef* sdef = struct_table_lookup(expr->new_expr.class_name);
        if(sdef) return ref_type(struct_type(sdef));
    }
    if(expr->type == ND_STRUCTCONS){
        StructDef* sdef = struct_table_lookup(expr->structcons.name);
        if(sdef) return struct_type(sdef);
    }
    if(expr->type == ND_MATCH){
        if(expr->match.result_type) return expr->match.result_type;
        if(expr->match.default_body){
            Type* t = infer_type(expr->match.default_body, state);
            if(t) return t;
        }
        if(expr->match.cases && expr->match.cases->length > 0){
            Node* first = expr->match.cases->nodes[0];
            Type* t = infer_type(first->match_case.body, state);
            if(t) return t;
        }
    }
    if(expr->type == ND_THIS){
        if(state->current_class){
            return ref_type(struct_type(struct_table_lookup(state->current_class->name)));
        }
    }
    if(expr->type == ND_NULL){
        return pointer_to(ty_void);
    }
    if(expr->type == ND_STRLIT){
        return string_type();
    }
    if(expr->type == ND_CAST){
        return expr->cast.target_type;
    }
    if(expr->type == ND_STRING_EQ || expr->type == ND_STRING_NE){
        return ty_bool;
    }
    if(expr->type == ND_ENUM_PATTERN_CMP){
        return ty_bool;
    }
    if(expr->type == ND_BINARY_EXPR){
        Type* lt = infer_type(expr->binary_expr.lhs, state);
        Type* rt = infer_type(expr->binary_expr.rhs, state);
        switch(expr->binary_expr.op){
            case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
            case OP_AND: case OP_OR: case OP_XOR:
                return ty_bool;
            default:
                if(lt && is_float(lt)) return lt;
                if(rt && is_float(rt)) return rt;
                return lt ? lt : rt;
        }
    }
    if(expr->type == ND_UNARY_EXPR){
        if(expr->unary_expr.op == OP_DEREF){
            Type* oty = infer_type(expr->unary_expr.operand, state);
            if(oty && oty->kind == TY_PTR) return oty->base;
            return oty;
        }
        if(expr->unary_expr.op == OP_NEG || expr->unary_expr.op == OP_NOT || expr->unary_expr.op == OP_BITNOT){
            return infer_type(expr->unary_expr.operand, state);
        }
    }
    if(expr->type == ND_INDEX){
        return expr->ty;
    }
    if(expr->type == ND_NEW){
        StructDef* sdef = struct_table_lookup(expr->new_expr.class_name);
        if(sdef) return ref_type(struct_type(sdef));
    }
    if(expr->type == ND_METHODCALL){
        if(expr->methodcall.class_name){
            ClassDef* cdef = class_table_lookup(expr->methodcall.class_name);
            if(cdef){
                for(uint64_t i = 0; i < cdef->methods->length; i++){
                    Node* m = cdef->methods->nodes[i];
                    // Match by method name (check suffix of mangled name)
                    char* suffix = strrchr(m->funcdef.name, '_');
                    if(suffix && strcmp(suffix + 1, expr->methodcall.method) == 0){
                        return m->funcdef.return_type;
                    }
                }
            }
        }
    }
    if(expr->type == ND_STRUCTCONS){
        StructDef* sdef = struct_table_lookup(expr->structcons.name);
        if(sdef) return struct_type(sdef);
    }
    if(expr->ty) return expr->ty;
    return NULL;
}

static Type* resolve_funcall_type(Node* node, ParserState* state){
    size_t len = strlen(node->funcall.name);
    char* mangled = malloc(len + 1);
    strcpy(mangled, node->funcall.name);
    int can_resolve = 1;
    for(uint64_t i = 0; i < node->funcall.args->length; i++){
        Type* atype = infer_type(node->funcall.args->nodes[i], state);
        if(!atype){ can_resolve = 0; break; }
        const char* tname = type_kind_to_name(atype);
        len += 1 + strlen(tname);
        mangled = realloc(mangled, len + 1);
        strcat(mangled, "_");
        strcat(mangled, tname);
    }
    Type* ret = NULL;
    if(can_resolve){
        Node* target = function_table_lookup(mangled);
        if(target){
            ret = target->funcdef.return_type;
        } else {
            // Generic function template fallback: infer the concrete type
            // arguments from the argument types and instantiate the function.
            Node* gtarget = resolve_generic_funcall(state, node->funcall.name, node->funcall.args);
            if(getenv("QZ_DEBUG_GEN"))
                fprintf(stderr, "[resolve] generic '%s' can_resolve=%d target=%s\n",
                        node->funcall.name, can_resolve, gtarget ? gtarget->funcdef.name : "NULL");
            if(gtarget){
                ret = gtarget->funcdef.return_type;
                // Rewrite the call target to the concrete instance so codegen
                // emits `call <mangled-instance>` rather than the template name.
                free(node->funcall.name);
                node->funcall.name = strdup(gtarget->funcdef.name);
            }
        }
    }
    /*if(strstr(mangled, "fopen") || strstr(mangled, "resolveImportPath")){
        fprintf(stderr, "DEBUG resolve %s can_resolve=%d found=%s ret=%s\n",
                mangled, can_resolve,
                function_table_lookup(mangled) ? "yes" : "no",
                ret ? type_kind_to_name(ret) : "NULL");
    }*/
    free(mangled);
    return ret;
}

// ---------------------------------------------------------------------------
// Generic function templates
//
// `function f<T, E>(...)` defines a template. The body is stored as raw
// tokens and, on first call with concrete argument types, re-parsed with the
// type parameters substituted (mirroring generic class templates). Each
// concrete instantiation is a separate function registered under a mangled
// name like `f_g_int64_string_Result`.
// ---------------------------------------------------------------------------

struct GenericFuncTemplate {
    char* name;
    char** type_param_names;
    int type_param_count;
    NodeList* params;
    Type* return_type;
    TokenList* tokens;
    int body_start;   // token index of '(' that opens the parameter list
    int body_end;     // token index just past the last body token
    char* file_dir;
};

static HashMap generic_func_templates;
static HashMap generic_func_instances;
static NodeList* generic_func_collector = NULL;
static uint64_t generic_func_appended = 0;

static GenericFuncTemplate* generic_func_template_lookup(const char* name){
    if(!generic_func_templates.buckets) return NULL;
    return (GenericFuncTemplate*)hashmap_get(&generic_func_templates, (char*)name);
}

static char* mangle_func_generic_arg(Type* t){
    char buf[512];
    switch(t->kind){
        case TY_INT8: strcpy(buf, "int8"); break;
        case TY_INT16: strcpy(buf, "int16"); break;
        case TY_INT32: strcpy(buf, "int32"); break;
        case TY_INT64: strcpy(buf, "int64"); break;
        case TY_UINT8: strcpy(buf, "uint8"); break;
        case TY_UINT16: strcpy(buf, "uint16"); break;
        case TY_UINT32: strcpy(buf, "uint32"); break;
        case TY_UINT64: strcpy(buf, "uint64"); break;
        case TY_BOOL: strcpy(buf, "bool"); break;
        case TY_CHAR: strcpy(buf, "char"); break;
        case TY_FLOAT32: strcpy(buf, "float32"); break;
        case TY_FLOAT64: strcpy(buf, "float64"); break;
        case TY_STRING: strcpy(buf, "string"); break;
        case TY_PTR: strcpy(buf, "voidptr"); break;
        case TY_ARRAY: strcpy(buf, "array"); break;
        case TY_STRUCT: strcpy(buf, t->structure.struct_def->name); break;
        case TY_REF:
            if(t->base && t->base->kind == TY_STRUCT) strcpy(buf, t->base->structure.struct_def->name);
            else strcpy(buf, "ref");
            break;
        case TY_ENUM:
            strcpy(buf, t->enumeration.def->name);
            for(int i = 0; i < t->enumeration.type_arg_count; i++){
                char* sub = mangle_func_generic_arg(t->enumeration.type_args[i]);
                strcat(buf, "_");
                strcat(buf, sub);
                free(sub);
            }
            break;
        default: strcpy(buf, "unknown"); break;
    }
    return strdup(buf);
}

static char* make_func_generic_mangled_name(const char* name, Type** type_args, int type_arg_count){
    size_t len = strlen(name) + 8;
    char* result = malloc(len + 1);
    strcpy(result, name);
    strcat(result, "_g");
    for(int i = 0; i < type_arg_count; i++){
        char* sub = mangle_func_generic_arg(type_args[i]);
        len += 1 + strlen(sub);
        result = realloc(result, len + 1);
        strcat(result, "_");
        strcat(result, sub);
        free(sub);
    }
    return result;
}

// Binds concrete types to the template's type parameters by matching a
// template parameter type (which may contain TY_TYPE_PARAM leaves) against
// the caller's concrete type.
static void bind_template_type(GenericFuncTemplate* tmpl, Type* tpl, Type* conc, Type*** binds){
    if(!tpl || !conc) return;
    if(tpl->kind == TY_TYPE_PARAM){
        if(!tpl->type_param_name) return;
        for(int i = 0; i < tmpl->type_param_count; i++){
            if(tmpl->type_param_names[i] && strcmp(tmpl->type_param_names[i], tpl->type_param_name) == 0){
                (*binds)[i] = conc;
                return;
            }
        }
        return;
    }
    if(tpl->kind == TY_ENUM && conc->kind == TY_ENUM){
        if(tpl->enumeration.def != conc->enumeration.def) return;
        for(int i = 0; i < tpl->enumeration.type_arg_count && i < conc->enumeration.type_arg_count; i++){
            bind_template_type(tmpl, tpl->enumeration.type_args[i], conc->enumeration.type_args[i], binds);
        }
        return;
    }
    if((tpl->kind == TY_PTR || tpl->kind == TY_REF) && (conc->kind == TY_PTR || conc->kind == TY_REF)){
        bind_template_type(tmpl, tpl->base, conc->base, binds);
        return;
    }
    if(tpl->kind == TY_ARRAY && conc->kind == TY_ARRAY){
        bind_template_type(tmpl, tpl->base, conc->base, binds);
        return;
    }
}

static Type* substitute_template_type(ParserState* state, GenericFuncTemplate* tmpl, Type* tpl, Type** binds){
    if(!tpl) return NULL;
    switch(tpl->kind){
        case TY_TYPE_PARAM:
            if(tpl->type_param_name){
                for(int i = 0; i < tmpl->type_param_count; i++){
                    if(tmpl->type_param_names[i] && binds[i] &&
                       strcmp(tmpl->type_param_names[i], tpl->type_param_name) == 0){
                        return binds[i];
                    }
                }
            }
            return tpl;
        case TY_ENUM: {
            Type** new_args = malloc(sizeof(Type*) * tpl->enumeration.type_arg_count);
            for(int i = 0; i < tpl->enumeration.type_arg_count; i++){
                new_args[i] = substitute_template_type(state, tmpl, tpl->enumeration.type_args[i], binds);
            }
            Type* base = enum_def_type(tpl->enumeration.def);
            return enum_instantiate(state, base, new_args, tpl->enumeration.type_arg_count);
        }
        case TY_PTR: return pointer_to(substitute_template_type(state, tmpl, tpl->base, binds));
        case TY_REF: return ref_type(substitute_template_type(state, tmpl, tpl->base, binds));
        // T[] is always a slice: no static arrays/VLAs in the language
        case TY_ARRAY: return dynarray_type(substitute_template_type(state, tmpl, tpl->base, binds));
        default: return tpl;
    }
}

// Writes the tokens spelling out a concrete type (used when a bare type
// parameter expands to a class/enum/pointer instead of a primitive).
static void add_type_token(TokenList* out, const char* value, const char* file, uint64_t line, uint64_t col){
    Token tk;
    tk.kind = TT_IDENT;
    if(strcmp(value, "<") == 0 || strcmp(value, ">") == 0 ||
       strcmp(value, ",") == 0 || strcmp(value, "[") == 0 || strcmp(value, "]") == 0){
        tk.kind = TT_PUNCT;
    } else if(strcmp(value, "int8") == 0 || strcmp(value, "int16") == 0 || strcmp(value, "int32") == 0 ||
              strcmp(value, "int64") == 0 || strcmp(value, "uint8") == 0 || strcmp(value, "uint16") == 0 ||
              strcmp(value, "uint32") == 0 || strcmp(value, "uint64") == 0 || strcmp(value, "bool") == 0 ||
              strcmp(value, "char") == 0 || strcmp(value, "float32") == 0 || strcmp(value, "float64") == 0 ||
              strcmp(value, "string") == 0 || strcmp(value, "void") == 0){
        tk.kind = TT_KEYWORD;
    }
    tk.value = strdup(value);
    tk.file = file;
    tk.line = line;
    tk.column = col;
    add_token(out, tk);
}

static void write_type_tokens(Type* t, TokenList* out, const char* file, uint64_t line, uint64_t col){
    if(!t) return;
    const char* name = NULL;
    switch(t->kind){
        case TY_VOID: name = "void"; break;
        case TY_BOOL: name = "bool"; break;
        case TY_CHAR: name = "char"; break;
        case TY_INT8: name = "int8"; break;
        case TY_INT16: name = "int16"; break;
        case TY_INT32: name = "int32"; break;
        case TY_INT64: name = "int64"; break;
        case TY_UINT8: name = "uint8"; break;
        case TY_UINT16: name = "uint16"; break;
        case TY_UINT32: name = "uint32"; break;
        case TY_UINT64: name = "uint64"; break;
        case TY_FLOAT32: name = "float32"; break;
        case TY_FLOAT64: name = "float64"; break;
        case TY_STRING: name = "string"; break;
        case TY_PTR:
            add_type_token(out, "ptr", file, line, col);
            add_type_token(out, "<", file, line, col);
            write_type_tokens(t->base, out, file, line, col);
            add_type_token(out, ">", file, line, col);
            return;
        case TY_ARRAY:
            write_type_tokens(t->base, out, file, line, col);
            add_type_token(out, "[", file, line, col);
            add_type_token(out, "]", file, line, col);
            return;
        case TY_STRUCT: name = t->structure.struct_def->name; break;
        case TY_REF:
            if(t->base && t->base->kind == TY_STRUCT){
                add_type_token(out, "ref", file, line, col);
                add_type_token(out, "<", file, line, col);
                add_type_token(out, t->base->structure.struct_def->name, file, line, col);
                add_type_token(out, ">", file, line, col);
                return;
            }
            name = "ref";
            break;
        case TY_ENUM:
            if(t->enumeration.type_arg_count > 0){
                add_type_token(out, t->enumeration.def->name, file, line, col);
                add_type_token(out, "<", file, line, col);
                for(int i = 0; i < t->enumeration.type_arg_count; i++){
                    if(i) add_type_token(out, ",", file, line, col);
                    write_type_tokens(t->enumeration.type_args[i], out, file, line, col);
                }
                add_type_token(out, ">", file, line, col);
                return;
            }
            name = t->enumeration.def->name;
            break;
        default: name = "unknown"; break;
    }
    add_type_token(out, name, file, line, col);
}

static Type** infer_generic_type_args(GenericFuncTemplate* tmpl, Type** arg_types, int arg_count){
    if(arg_count != (int)tmpl->params->length) return NULL;
    Type** binds = calloc(tmpl->type_param_count, sizeof(Type*));
    for(int i = 0; i < arg_count; i++){
        bind_template_type(tmpl, tmpl->params->nodes[i]->param.type, arg_types[i], &binds);
    }
    for(int i = 0; i < tmpl->type_param_count; i++){
        if(!binds[i]){
            free(binds);
            return NULL;
        }
    }
    return binds;
}

static Node* instantiate_generic_func(ParserState* state, GenericFuncTemplate* tmpl, Type** type_args){
    // Lazy init the instance cache
    if(!generic_func_instances.buckets){
        generic_func_instances.buckets = NULL;
        generic_func_instances.capacity = 0;
        generic_func_instances.used = 0;
    }

    char* mangled = make_func_generic_mangled_name(tmpl->name, type_args, tmpl->type_param_count);
    Node* existing = hashmap_get(&generic_func_instances, mangled);
    if(existing) return existing;

    // Resolve type arg names for the primitive single-token substitutions.
    char** type_arg_names = malloc(sizeof(char*) * tmpl->type_param_count);
    for(int i = 0; i < tmpl->type_param_count; i++){
        Type* t = type_args[i];
        if(t->kind == TY_INT64) type_arg_names[i] = "int64";
        else if(t->kind == TY_INT32) type_arg_names[i] = "int32";
        else if(t->kind == TY_INT16) type_arg_names[i] = "int16";
        else if(t->kind == TY_INT8) type_arg_names[i] = "int8";
        else if(t->kind == TY_UINT64) type_arg_names[i] = "uint64";
        else if(t->kind == TY_UINT32) type_arg_names[i] = "uint32";
        else if(t->kind == TY_UINT16) type_arg_names[i] = "uint16";
        else if(t->kind == TY_UINT8) type_arg_names[i] = "uint8";
        else if(t->kind == TY_BOOL) type_arg_names[i] = "bool";
        else if(t->kind == TY_FLOAT64) type_arg_names[i] = "float64";
        else if(t->kind == TY_FLOAT32) type_arg_names[i] = "float32";
        else if(t->kind == TY_CHAR) type_arg_names[i] = "char";
        else if(t->kind == TY_STRING) type_arg_names[i] = "string";
        else if(t->kind == TY_ENUM) type_arg_names[i] = t->enumeration.def->name;
        else if(t->kind == TY_STRUCT) type_arg_names[i] = t->structure.struct_def->name;
        else if(t->kind == TY_PTR) type_arg_names[i] = "ptr";
        else if(t->kind == TY_REF){
            if(t->base && t->base->kind == TY_STRUCT) type_arg_names[i] = t->base->structure.struct_def->name;
            else type_arg_names[i] = "ref";
        } else if(t->kind == TY_ARRAY) type_arg_names[i] = "array";
        else {
            fprintf(stderr, "error: unsupported type argument for generic function '%s'\n", tmpl->name);
            exit(1);
        }
    }

    // Compute the concrete parameter types (used for the table key and the
    // recursive-instantiation placeholder).
    NodeList* cparams = make_nodelist();
    for(uint64_t i = 0; i < tmpl->params->length; i++){
        Node* src = tmpl->params->nodes[i];
        Type* ct = substitute_template_type(state, tmpl, src->param.type, type_args);
        Node* p = make_node(ND_PARAM);
        p->param.name = src->param.name;
        p->param.type = ct;
        nodelist_add(cparams, p);
    }
    char* table_key = make_func_mangled_name(mangled, cparams);

    // Placeholder funcdef breaks recursion (a body calling the same generic
    // function with the same type arguments would otherwise re-instantiate).
    Node* placeholder = make_node(ND_FUNCDEF);
    placeholder->funcdef.name = table_key;
    placeholder->funcdef.unmangled_name = strdup(mangled);
    placeholder->funcdef.params = cparams;
    placeholder->funcdef.return_type = substitute_template_type(state, tmpl, tmpl->return_type, type_args);
    placeholder->funcdef.body = NULL;
    placeholder->funcdef.is_extern = true;
    hashmap_put(&generic_func_instances, mangled, placeholder);

    // Build the substituted token list: `function <mangled>` + signature/body.
    TokenList* src = tmpl->tokens;
    int span_len = tmpl->body_end - tmpl->body_start;
    Token* ref_tok = span_len > 0 ? &src->tokens[tmpl->body_start] : NULL;
    const char* rfile = ref_tok ? ref_tok->file : (tmpl->file_dir ? tmpl->file_dir : "<generic>");
    uint64_t rline = ref_tok ? ref_tok->line : 0;
    uint64_t rcol = ref_tok ? ref_tok->column : 0;

    TokenList* out = init_tokenlist(2 + span_len);
    Token fname_tk;
    fname_tk.kind = TT_KEYWORD;
    fname_tk.value = strdup("function");
    fname_tk.file = rfile;
    fname_tk.line = rline;
    fname_tk.column = rcol;
    add_token(out, fname_tk);
    Token mname_tk;
    mname_tk.kind = TT_IDENT;
    mname_tk.value = strdup(mangled);
    mname_tk.file = rfile;
    mname_tk.line = rline;
    mname_tk.column = rcol;
    add_token(out, mname_tk);

    for(int i = tmpl->body_start; i < tmpl->body_end; i++){
        Token* st = &src->tokens[i];
        int matched = -1;
        if(st->value){
            for(int p = 0; p < tmpl->type_param_count; p++){
                if(tmpl->type_param_names[p] && strcmp(st->value, tmpl->type_param_names[p]) == 0){
                    matched = p;
                    break;
                }
            }
        }
        if(matched >= 0){
            Type* at = type_args[matched];
            // Classes are reference values; `T` expands to `ref < Name >`.
            if(at->kind == TY_REF && at->base && at->base->kind == TY_STRUCT){
                add_type_token(out, "ref", st->file, st->line, st->column);
                add_type_token(out, "<", st->file, st->line, st->column);
                add_type_token(out, at->base->structure.struct_def->name, st->file, st->line, st->column);
                add_type_token(out, ">", st->file, st->line, st->column);
            } else if(at->kind == TY_ENUM && at->enumeration.type_arg_count > 0){
                write_type_tokens(at, out, st->file, st->line, st->column);
            } else if(at->kind == TY_PTR || at->kind == TY_ARRAY){
                write_type_tokens(at, out, st->file, st->line, st->column);
            } else {
                Token tk = *st;
                tk.value = strdup(type_arg_names[matched]);
                if(strcmp(tk.value, "int64") == 0 || strcmp(tk.value, "int32") == 0 || strcmp(tk.value, "int16") == 0 ||
                   strcmp(tk.value, "int8") == 0 || strcmp(tk.value, "uint64") == 0 || strcmp(tk.value, "uint32") == 0 ||
                   strcmp(tk.value, "uint16") == 0 || strcmp(tk.value, "uint8") == 0 || strcmp(tk.value, "float64") == 0 ||
                   strcmp(tk.value, "float32") == 0 || strcmp(tk.value, "bool") == 0 ||
                   strcmp(tk.value, "char") == 0 || strcmp(tk.value, "void") == 0 || strcmp(tk.value, "string") == 0){
                    tk.kind = TT_KEYWORD;
                } else {
                    tk.kind = TT_IDENT;
                }
                add_token(out, tk);
            }
        } else {
            Token tk = *st;
            tk.value = st->value ? strdup(st->value) : NULL;
            add_token(out, tk);
        }
    }

    // Re-parse the concrete function from the substituted tokens.
    ParserState sub_state;
    sub_state.tokens = out;
    sub_state.index = -1;
    sub_state.scope = symbol_table_make();
    sub_state.global_scope = state->global_scope;
    sub_state.stack_offset = 0;
    sub_state.current_file_dir = tmpl->file_dir ? tmpl->file_dir : state->current_file_dir;
    sub_state.current_class = NULL;
    sub_state.in_constructor = false;
    sub_state.generic_methods_head = NULL;
    sub_state.current_trait = NULL;
    sub_state.current_trait_generic_count = 0;
    sub_state.current_trait_generic_names = NULL;
    sub_state.current_return_type = NULL;
    sub_state.suppress_class_ref = 0;
    sub_state.current_func_generic_count = 0;
    sub_state.current_func_generic_names = NULL;

    Token t0 = next(&sub_state);
    if(t0.kind != TT_KEYWORD || !equal(t0, "function")){
        fprintf(stderr, "internal error: expected 'function' in generic instantiation of '%s'\n", tmpl->name);
        exit(1);
    }
    Node* funcdef = parse_funcdef(&sub_state);
    if(!funcdef){
        fprintf(stderr, "internal error: failed to instantiate generic function '%s'\n", tmpl->name);
        exit(1);
    }

    // Cache the real funcdef, replacing the placeholder.
    hashmap_put(&generic_func_instances, mangled, funcdef);

    // Collect for AST emission.
    if(!generic_func_collector) generic_func_collector = make_nodelist();
    nodelist_add(generic_func_collector, funcdef);

    // Free the temporary token list values.
    for(uint64_t i = 0; i < out->length; i++){
        Token* tk = &out->tokens[i];
        if(tk->kind == TT_IDENT || tk->kind == TT_KEYWORD || tk->kind == TT_NUM || tk->kind == TT_STR){
            if(tk->value) free((void*)tk->value);
        }
    }
    free(out->tokens);
    free(out);

    free(type_arg_names);
    return funcdef;
}

Node* resolve_generic_funcall_types(ParserState* state, const char* name, Type** arg_types, int arg_count){
    GenericFuncTemplate* tmpl = generic_func_template_lookup(name);
    if(getenv("QZ_DEBUG_GEN"))
        fprintf(stderr, "[resolve] types '%s' tmpl=%s nparams=%u nargs=%d\n",
                name, tmpl ? tmpl->name : "NULL", tmpl ? (unsigned)tmpl->params->length : 0, arg_count);
    if(!tmpl) return NULL;
    if(arg_count != (int)tmpl->params->length) return NULL;
    Type** binds = infer_generic_type_args(tmpl, arg_types, arg_count);
    if(getenv("QZ_DEBUG_GEN"))
        fprintf(stderr, "[resolve] binds=%s\n", binds ? "OK" : "NULL");
    if(!binds) return NULL;
    Node* funcdef = instantiate_generic_func(state, tmpl, binds);
    free(binds);
    return funcdef;
}

Node* resolve_generic_funcall(ParserState* state, const char* name, NodeList* args){
    if(!args) return NULL;
    Type** arg_types = malloc(sizeof(Type*) * args->length);
    for(uint64_t i = 0; i < args->length; i++){
        arg_types[i] = infer_type(args->nodes[i], state);
        if(!arg_types[i]) arg_types[i] = args->nodes[i]->ty;
        if(getenv("QZ_DEBUG_GEN"))
            fprintf(stderr, "[resolve] arg %lu type=%s kind=%d\n", i,
                    arg_types[i] ? type_kind_to_name(arg_types[i]) : "NULL",
                    arg_types[i] ? arg_types[i]->kind : -1);
        if(!arg_types[i]){
            free(arg_types);
            return NULL;
        }
    }
    Node* funcdef = resolve_generic_funcall_types(state, name, arg_types, (int)args->length);
    free(arg_types);
    return funcdef;
}

Node* instantiate_generic_func_for_types(ParserState* state, const char* name, Type** arg_types, int arg_count){
    GenericFuncTemplate* tmpl = generic_func_template_lookup(name);
    if(!tmpl) return NULL;
    Type** binds = infer_generic_type_args(tmpl, arg_types, arg_count);
    if(!binds) return NULL;
    Node* funcdef = instantiate_generic_func(state, tmpl, binds);
    free(binds);
    return funcdef;
}

void append_generic_funcs_to_ast(Node* ast){
    if(!generic_func_collector) return;
    while(generic_func_appended < generic_func_collector->length){
        nodelist_add(ast->program_node.children, generic_func_collector->nodes[generic_func_appended]);
        generic_func_appended++;
    }
}

static void resolve_enumcons(ParserState* state, Node* node, Type* expected){
    if(node->type != ND_ENUMCONS) return;
    if(node->enumcons.resolved_type) return;

    EnumDef* def = enum_table_lookup(node->enumcons.enum_name);
    if(!def){
        fprintf(stderr, "parse error: unknown enum '%s'\n", node->enumcons.enum_name);
        exit(1);
    }

    int vi = -1;
    for(int i = 0; i < def->variant_count; i++){
        if(strcmp(def->variants[i].name, node->enumcons.variant_name) == 0){
            vi = i;
            break;
        }
    }
    if(vi == -1){
        fprintf(stderr, "parse error: unknown variant '%s' in enum '%s'\n",
                node->enumcons.variant_name, node->enumcons.enum_name);
        exit(1);
    }

    EnumVariant* var = &def->variants[vi];
    int arg_count = node->enumcons.args ? node->enumcons.args->length : 0;
    if(arg_count != var->param_count){
        fprintf(stderr, "parse error: variant '%s.%s' expects %d arguments, got %d\n",
                node->enumcons.enum_name, node->enumcons.variant_name, var->param_count, arg_count);
        exit(1);
    }

    node->enumcons.variant_index = vi;
    if(expected && expected->kind == TY_ENUM){
        node->enumcons.resolved_type = expected;
    } else if(!expected && def->generic_param_count == 0){
        node->enumcons.resolved_type = enum_instantiate(state, enum_def_type(def), NULL, 0);
    } else {
        fprintf(stderr, "parse error: cannot infer enum type for '%s.%s' (type annotation required for generic enums)\n",
                node->enumcons.enum_name, node->enumcons.variant_name);
        exit(1);
    }
}

static Node* parse_assign_stmt(ParserState* state, Token name, Token op){
    Symbol* sym = lookup_symbol(state, name.value);
    if(!sym){
        // Implicit 'this' — check if this is a class field assignment
        if(state->current_class){
            for(int i = 0; i < state->current_class->field_count; i++){
                if(strcmp(state->current_class->fields[i].name, name.value) == 0){
                    if(state->current_class->fields[i].is_const && !state->in_constructor){
                        fprintf(stderr, "parse error: cannot assign to const field '%s' at %s:%lu:%lu\n",
                                name.value, name.file, name.line, name.column);
                        exit(1);
                    }
                    Node* value = parse_expr(state);
                    expect_stmt_end(state);
                    StructDef* sdef = struct_table_lookup(state->current_class->name);
                    Node* this_node = make_node(ND_THIS);
                    this_node->ty = ref_type(struct_type(sdef));
                    Node* val_expr = NULL;
                    if(equal(op, "=")){
                        val_expr = value;
                    } else {
                        Node* field_read = make_node(ND_MEMBER);
                        field_read->member.base = this_node;
                        field_read->member.field_name = name.value;
                        field_read->member.field_offset = state->current_class->fields[i].offset;
                        field_read->member.is_struct = false;
                        field_read->member.is_class = true;
                        field_read->ty = state->current_class->fields[i].type;
                        Node* bin = make_node(ND_BINARY_EXPR);
                        bin->binary_expr.lhs = field_read;
                        bin->binary_expr.rhs = value;
                        if(equal(op, "+="))            bin->binary_expr.op = OP_ADD;
                        else if(equal(op, "-="))       bin->binary_expr.op = OP_SUB;
                        else if(equal(op, "*="))       bin->binary_expr.op = OP_MUL;
                        else if(equal(op, "/="))       bin->binary_expr.op = OP_DIV;
                        else if(equal(op, "%="))       bin->binary_expr.op = OP_MOD;
                        else if(equal(op, "&&="))      bin->binary_expr.op = OP_BITAND;
                        else if(equal(op, "||="))      bin->binary_expr.op = OP_BITOR;
                        else if(equal(op, "^^="))      bin->binary_expr.op = OP_BITXOR;
                        val_expr = bin;
                    }
                    Node* node = make_node(ND_MEMBER_ASSIGN);
                    node->member_assign.object = this_node;
                    node->member_assign.field_name = name.value;
                    node->member_assign.field_offset = state->current_class->fields[i].offset;
                    node->member_assign.value = val_expr;
                    return node;
                }
            }
        }
        fprintf(stderr, "parse error: undefined variable '%s' at %s:%lu:%lu\n",
                name.value, name.file, name.line, name.column);
        exit(1);
    }
    if(sym->is_const){
        fprintf(stderr, "parse error: cannot assign to const variable '%s' at %s:%lu:%lu\n",
                name.value, name.file, name.line, name.column);
        exit(1);
    }

    Node* node = make_node(ND_ASSIGN);
    node->assign.name = sym->name;
    node->assign.offset = sym->offset;
    node->assign.is_global = sym->is_global;
    node->assign.var_type = sym->type;

    if(equal(op, "!!=")){
        Node* var_node = make_node(ND_VARREF);
        var_node->varref.name = sym->name;
        var_node->varref.offset = sym->offset;
        var_node->varref.is_global = sym->is_global;

        Node* un = make_node(ND_UNARY_EXPR);
        un->unary_expr.op = OP_BITNOT;
        un->unary_expr.operand = var_node;

        node->assign.value = un;
        expect_stmt_end(state);
    } else {
        Node* value = parse_expr(state);
        expect_stmt_end(state);

        if(equal(op, "=")){
            node->assign.value = value;
        } else {
            Node* var_node = make_node(ND_VARREF);
            var_node->varref.name = sym->name;
            var_node->varref.offset = sym->offset;
            var_node->varref.is_global = sym->is_global;

            Node* bin = make_node(ND_BINARY_EXPR);
            bin->binary_expr.lhs = var_node;
            bin->binary_expr.rhs = value;
            if(equal(op, "+="))            bin->binary_expr.op = OP_ADD;
            else if(equal(op, "-="))       bin->binary_expr.op = OP_SUB;
            else if(equal(op, "*="))       bin->binary_expr.op = OP_MUL;
            else if(equal(op, "/="))       bin->binary_expr.op = OP_DIV;
            else if(equal(op, "%="))       bin->binary_expr.op = OP_MOD;
            else if(equal(op, "&&="))      bin->binary_expr.op = OP_BITAND;
            else if(equal(op, "||="))      bin->binary_expr.op = OP_BITOR;
            else if(equal(op, "^^="))      bin->binary_expr.op = OP_BITXOR;

            node->assign.value = bin;
        }
    }

    return node;
}

static Node* parse_while_stmt(ParserState* state){
    expect(state, "(");
    Node* cond = parse_expr(state);
    expect(state, ")");
    NodeList* body = parse_optional_block(state);

    Node* node = make_node(ND_WHILE);
    node->loop.cond = cond;
    node->loop.body = body;
    return node;
}

static Node* parse_do_while_stmt(ParserState* state){
    NodeList* body = parse_optional_block(state);
    expect(state, "while");
    expect(state, "(");
    Node* cond = parse_expr(state);
    expect(state, ")");
    expect_stmt_end(state);

    Node* node = make_node(ND_DO_WHILE);
    node->loop.cond = cond;
    node->loop.body = body;
    return node;
}

static Node* parse_for_stmt(ParserState* state){
    expect(state, "(");

    Node* init = NULL;
    int scope_len = state->scope->length;
    if(!equal(peek(state), ";")){
        if(equal(peek(state), "var") || equal(peek(state), "const")){
            Token kw = next(state);
            init = parse_vardecl(state, equal(kw, "const"), false, false);
            expect(state, ";");
        } else {
            init = parse_expr(state);
            expect(state, ";");
        }
    } else {
        next(state);
    }

    Node* cond = NULL;
    if(!equal(peek(state), ";")){
        cond = parse_expr(state);
    }
    expect(state, ";");

    Node* update = NULL;
    if(!equal(peek(state), ")")){
        Token t = peek(state);
        if(t.kind == TT_IDENT){
            Token t2 = state->tokens->tokens[state->index + 2];
            if(equal(t2, "=") || equal(t2, "+=") || equal(t2, "-=") ||
               equal(t2, "*=") || equal(t2, "/=") || equal(t2, "%=") ||
               equal(t2, "&&=") || equal(t2, "||=") || equal(t2, "^^=") || equal(t2, "!!=")){
                next(state);
                Token op = next(state);
        Symbol* sym = lookup_symbol(state, t.value);
                if(!sym){
                    fprintf(stderr, "parse error: undefined variable '%s' at %s:%lu:%lu\n",
                            t.value, t.file, t.line, t.column);
                    exit(1);
                }
                Node* node = make_node(ND_ASSIGN);
                node->assign.name = sym->name;
                node->assign.offset = sym->offset;
                node->assign.is_global = sym->is_global;
                node->assign.var_type = sym->type;
                if(equal(op, "=")){
                    node->assign.value = parse_expr(state);
                } else if(equal(op, "!!=")){
                    Node* vn = make_node(ND_VARREF);
                    vn->varref.name = sym->name;
                    vn->varref.offset = sym->offset;
                    vn->varref.is_global = sym->is_global;
                    Node* un = make_node(ND_UNARY_EXPR);
                    un->unary_expr.op = OP_BITNOT;
                    un->unary_expr.operand = vn;
                    node->assign.value = un;
                } else {
                    Node* value = parse_expr(state);
                    Node* vn = make_node(ND_VARREF);
                    vn->varref.name = sym->name;
                    vn->varref.offset = sym->offset;
                    vn->varref.is_global = sym->is_global;
                    Node* bin = make_node(ND_BINARY_EXPR);
                    bin->binary_expr.lhs = vn;
                    bin->binary_expr.rhs = value;
                    if(equal(op, "+="))            bin->binary_expr.op = OP_ADD;
                    else if(equal(op, "-="))       bin->binary_expr.op = OP_SUB;
                    else if(equal(op, "*="))       bin->binary_expr.op = OP_MUL;
                    else if(equal(op, "/="))       bin->binary_expr.op = OP_DIV;
                    else if(equal(op, "%="))       bin->binary_expr.op = OP_MOD;
                    else if(equal(op, "&&="))      bin->binary_expr.op = OP_BITAND;
                    else if(equal(op, "||="))      bin->binary_expr.op = OP_BITOR;
                    else if(equal(op, "^^="))      bin->binary_expr.op = OP_BITXOR;
                    node->assign.value = bin;
                }
                update = node;
            } else {
                update = parse_expr(state);
            }
        } else {
            update = parse_expr(state);
        }
    }
    expect(state, ")");

    NodeList* body = parse_optional_block(state);

    state->scope->length = scope_len;

    Node* node = make_node(ND_FOR);
    node->for_stmt.init = init;
    node->for_stmt.cond = cond;
    node->for_stmt.update = update;
    node->for_stmt.body = body;
    return node;
}

static Node* parse_foreach_stmt(ParserState* state){
    expect(state, "(");
    Token id = expect_ident(state);
    expect(state, "in");
    Node* collection = parse_expr(state);
    expect(state, ")");

    int scope_len = state->scope->length;

    state->stack_offset -= 8;
    int index_offset = state->stack_offset;

    int elem_size = 8;
    int list_offset = 0;
    Type* elem_type = NULL;

    if(collection->type == ND_VARREF){
        Symbol* list_sym = lookup_symbol(state, collection->varref.name);
        if(!list_sym){
            fprintf(stderr, "parse error: undefined variable '%s'\n",
                    collection->varref.name);
            exit(1);
        }
        if(!list_sym->type || !list_sym->type->base){
            fprintf(stderr, "parse error: '%s' is not an array\n",
                    collection->varref.name);
            exit(1);
        }
        elem_size = list_sym->type->base->size;
        elem_type = list_sym->type->base;
        if(list_sym->is_global){
            state->stack_offset -= 16;
            list_offset = state->stack_offset;
        } else {
            list_offset = list_sym->offset;
        }
    } else {
        Type* coll_type = infer_type(collection, state);
        if(coll_type && coll_type->base){
            elem_size = coll_type->base->size;
            elem_type = coll_type->base;
        }
        state->stack_offset -= 16;
        list_offset = state->stack_offset;
    }

    int id_alloc = elem_size > 8 ? elem_size : 8;
    state->stack_offset -= id_alloc;
    int id_offset = state->stack_offset;

    Symbol id_sym = { .name = id.value, .type = elem_type, .offset = id_offset, .is_const = false };
    symbol_table_add(state->scope, id_sym);

    NodeList* body = parse_optional_block(state);

    state->scope->length = scope_len;

    Node* node = make_node(ND_FOREACH);
    node->foreach.list_offset = list_offset;
    node->foreach.id_offset = id_offset;
    node->foreach.index_offset = index_offset;
    node->foreach.elem_size = elem_size;
    node->foreach.collection = collection;
    node->foreach.id_name = strdup(id.value);
    node->foreach.body = body;
    return node;
}

static Node* parse_switch_stmt(ParserState* state){
    expect(state, "(");
    Node* scrutinee = parse_expr(state);
    expect(state, ")");
    expect(state, "{");

    NodeList* cases = make_nodelist();
    NodeList* default_body = NULL;

    while(!equal(peek(state), "}")){
        if(equal(peek(state), "case")){
            next(state);
            Node* label = parse_expr(state);
            expect(state, ":");

            NodeList* body = make_nodelist();
            while(true){
                if(equal(peek(state), "case") || equal(peek(state), "default") || equal(peek(state), "}")) break;
                skip_semicolons(state);
                if(equal(peek(state), "case") || equal(peek(state), "default") || equal(peek(state), "}")) break;
                nodelist_add(body, parse_stmt(state));
            }

            Node* c = make_node(ND_SWITCH_CASE);
            c->switch_case.label = label;
            c->switch_case.body = body;
            nodelist_add(cases, c);
        } else if(equal(peek(state), "default")){
            next(state);
            expect(state, ":");
            default_body = make_nodelist();
            while(true){
                if(equal(peek(state), "}")) break;
                skip_semicolons(state);
                if(equal(peek(state), "}")) break;
                nodelist_add(default_body, parse_stmt(state));
            }
        } else {
            fprintf(stderr, "parse error: expected 'case', 'default', or '}' at %s:%lu:%lu\n",
                    peek(state).file, peek(state).line, peek(state).column);
            exit(1);
        }
    }
    expect(state, "}");

    Node* node = make_node(ND_SWITCH);
    node->switch_stmt.scrutinee = scrutinee;
    node->switch_stmt.cases = cases;
    node->switch_stmt.default_body = default_body;
    Type* st = infer_type(scrutinee, state);
    node->switch_stmt.is_string = st && is_string_type(st);
    return node;
}

static Node* parse_break_stmt(ParserState* state){
    expect_stmt_end(state);
    return make_node(ND_BREAK);
}

static Node* parse_continue_stmt(ParserState* state){
    expect_stmt_end(state);
    return make_node(ND_CONTINUE);
}

static Node* parse_stmt(ParserState* state){
    skip_semicolons(state);
    Token t = next(state);
    if(equal(t, "exit")){
        return parse_exit_stmt(state);
    }
    if(equal(t, "if")){
        return parse_if_stmt(state);
    }
    if(equal(t, "while")){
        return parse_while_stmt(state);
    }
    if(equal(t, "do")){
        return parse_do_while_stmt(state);
    }
    if(equal(t, "for")){
        return parse_for_stmt(state);
    }
    if(equal(t, "foreach")){
        return parse_foreach_stmt(state);
    }
    if(equal(t, "var")){
        return parse_vardecl(state, false, false, true);
    }
    if(equal(t, "const")){
        return parse_vardecl(state, true, false, true);
    }
    if(equal(t, "return")){
        return parse_return_stmt(state);
    }
    if(equal(t, "switch")){
        return parse_switch_stmt(state);
    }
    if(equal(t, "break")){
        return parse_break_stmt(state);
    }
    if(equal(t, "continue")){
        return parse_continue_stmt(state);
    }
    if(equal(t, "free")){
        Node* operand = parse_expr(state);
        expect_stmt_end(state);
        Node* node = make_node(ND_FREE);
        node->free.expr = operand;
        return node;
    }
    if(equal(t, "match")){
        Node* m = parse_match_body(state);
        expect_stmt_end(state);
        return m;
    }
    if(equal(t, "*")){
        Node* target = parse_primary(state);
        Token op = peek(state);
        if(equal(op, "=") || equal(op, "+=") || equal(op, "-=") ||
           equal(op, "*=") || equal(op, "/=") || equal(op, "%=")){
            next(state);
            Node* value = parse_expr(state);
            expect_stmt_end(state);
            Node* node = make_node(ND_DEREF_ASSIGN);
            node->deref_assign.target = target;
            node->deref_assign.value = value;
            if(equal(op, "="))       node->deref_assign.op = OP_ADD;
            else if(equal(op, "+=")) node->deref_assign.op = OP_ADD;
            else if(equal(op, "-=")) node->deref_assign.op = OP_SUB;
            else if(equal(op, "*=")) node->deref_assign.op = OP_MUL;
            else if(equal(op, "/=")) node->deref_assign.op = OP_DIV;
            else if(equal(op, "%=")) node->deref_assign.op = OP_MOD;
            return node;
        }
        state->index -= 2;
        Node* expr = parse_expr(state);
        expect_stmt_end(state);
        Node* node = make_node(ND_EXPR_STMT);
        node->expr_stmt.expr = expr;
        return node;
    }
    if(t.kind == TT_IDENT){
        Token op = peek(state);
        if(equal(op, "(")){
            // Implicit 'this' — method call statement
            if(state->current_class){
                int found_method = 0;
                NodeList* methods = state->current_class->methods;
                if(methods){
                    for(uint64_t mi = 0; mi < methods->length; mi++){
                        Node* method = methods->nodes[mi];
                        char* mangled = method->funcdef.name;
                        char* underscore = strchr(mangled, '_');
                        if(underscore && strcmp(underscore + 1, t.value) == 0){
                            found_method = 1;
                            break;
                        }
                    }
                }
                if(found_method){
                    NodeList* args = make_nodelist();
                    next(state);
                    if(!equal(peek(state), ")")){
                        while(true){
                            nodelist_add(args, parse_expr(state));
                            if(!equal(peek(state), ",")) break;
                            next(state);
                        }
                    }
                    expect(state, ")");
                    expect_stmt_end(state);
                    StructDef* sdef = struct_table_lookup(state->current_class->name);
                    Node* this_node = make_node(ND_THIS);
                    this_node->ty = ref_type(struct_type(sdef));
                    Node* node = make_node(ND_METHODCALL);
                    node->methodcall.object = this_node;
                    node->methodcall.method = t.value;
                    node->methodcall.args = args;
                    node->methodcall.class_name = state->current_class->name;
                    node->ty = method_call_return_type(node->methodcall.class_name, node->methodcall.method);
                    Node* stmt = make_node(ND_EXPR_STMT);
                    stmt->expr_stmt.expr = node;
                    return stmt;
                }
            }
            NodeList* args = make_nodelist();
            next(state);
            if(!equal(peek(state), ")")){
                while(true){
                    nodelist_add(args, parse_expr(state));
                    if(!equal(peek(state), ",")) break;
                    next(state);
                }
            }
            expect(state, ")");
            expect_stmt_end(state);
            Node* node = make_node(ND_FUNCCALL);
            node->funcall.name = t.value;
            node->funcall.args = args;
            node->ty = resolve_funcall_type(node, state);
            return node;
        }
        if(equal(op, "=") || equal(op, "+=") || equal(op, "-=") ||
           equal(op, "*=") || equal(op, "/=") || equal(op, "%=") ||
           equal(op, "&&=") || equal(op, "||=") || equal(op, "^^=") || equal(op, "!!=")){
            next(state);
            return parse_assign_stmt(state, t, op);
        }
    }

    state->index--;
    Node* expr = parse_expr(state);

    // Check for member assignment: expr.field = value
    Token op = peek(state);
    if(expr->type == ND_MEMBER && (equal(op, "=") || equal(op, "+=") || equal(op, "-=") ||
       equal(op, "*=") || equal(op, "/=") || equal(op, "%=") ||
       equal(op, "&&=") || equal(op, "||=") || equal(op, "^^=") || equal(op, "!!="))){
        next(state);
        Node* value = parse_expr(state);
        expect_stmt_end(state);

        Node* val_expr = NULL;
        if(equal(op, "=")){
            val_expr = value;
        } else if(equal(op, "!!=")){
            Node* un = make_node(ND_UNARY_EXPR);
            un->unary_expr.op = OP_BITNOT;
            un->unary_expr.operand = expr;
            val_expr = un;
        } else {
            Node* bin = make_node(ND_BINARY_EXPR);
            bin->binary_expr.lhs = expr;
            bin->binary_expr.rhs = value;
            if(equal(op, "+="))       bin->binary_expr.op = OP_ADD;
            else if(equal(op, "-="))  bin->binary_expr.op = OP_SUB;
            else if(equal(op, "*="))  bin->binary_expr.op = OP_MUL;
            else if(equal(op, "/="))  bin->binary_expr.op = OP_DIV;
            else if(equal(op, "%="))  bin->binary_expr.op = OP_MOD;
            else if(equal(op, "&&=")) bin->binary_expr.op = OP_BITAND;
            else if(equal(op, "||=")) bin->binary_expr.op = OP_BITOR;
            else if(equal(op, "^^=")) bin->binary_expr.op = OP_BITXOR;
            val_expr = bin;
        }

        Node* node = make_node(ND_MEMBER_ASSIGN);
        node->member_assign.object = expr->member.base;
        node->member_assign.field_name = expr->member.field_name;
        node->member_assign.field_offset = expr->member.field_offset;
        node->member_assign.value = val_expr;
        node->member_assign.is_class = expr->member.is_class;
        node->member_assign.is_struct = expr->member.is_struct;
        return node;
    }

    expect_stmt_end(state);
    Node* node = make_node(ND_EXPR_STMT);
    node->expr_stmt.expr = expr;
    return node;
}

static AccessModifier parse_access_modifier(ParserState* state){
    if(equal(peek(state), "public")){
        next(state);
        return ACCESS_PUBLIC;
    }
    if(equal(peek(state), "private")){
        next(state);
        return ACCESS_PRIVATE;
    }
    if(equal(peek(state), "protected")){
        next(state);
        return ACCESS_PROTECTED;
    }
    return ACCESS_PUBLIC; // default
}

static void parse_class_body(ParserState* state, ClassDef* class_def);

static char* parse_class_def(ParserState* state){
    Token name = expect_ident(state);

    // Parse type parameters e.g., <T> or <K, V>
    int type_param_count = 0;
    char** type_params = NULL;
    if(equal(peek(state), "<")){
        next(state);
        while(true){
            Token tp = expect_ident(state);
            type_param_count++;
            type_params = realloc(type_params, sizeof(char*) * type_param_count);
            type_params[type_param_count - 1] = tp.value;
            if(!equal(peek(state), ",")) break;
            next(state);
        }
        expect(state, ">");
    }

    // Parse optional implements clause (skip type params as they are just names)
    char** implements = NULL;
    int implements_count = 0;
    if(equal(peek(state), "implements")){
        next(state);
        while(true){
            Token trait_name = expect_ident(state);
            // Skip <T> after trait name (we don't validate trait params)
            if(equal(peek(state), "<")){
                int depth = 1;
                next(state);
                while(depth > 0){
                    Token t = next(state);
                    if(equal(t, "<")) depth++;
                    if(equal(t, ">")) depth--;
                }
            }
            implements_count++;
            implements = realloc(implements, sizeof(char*) * implements_count);
            implements[implements_count - 1] = trait_name.value;
            if(!equal(peek(state), ",")) break;
            next(state);
        }
    }

    // Create ClassDef
    ClassDef* class_def = malloc(sizeof(ClassDef));
    class_def->name = name.value;
    class_def->type_params = type_params;
    class_def->type_param_count = type_param_count;
    class_def->fields = NULL;
    class_def->field_count = 0;
    class_def->total_size = 0;
    class_def->methods = make_nodelist();
    class_def->implements = implements;
    class_def->implements_count = implements_count;

    if(type_param_count > 0){
        // Generic class template: skip body tokens for later instantiation
        class_def->is_generic_template = true;
        class_def->body_token_start = -1;
        class_def->body_token_end = -1;

        // Check if '{' follows (required)
        if(equal(peek(state), "{")){
            class_def->body_token_start = (int)state->index;
            next(state);
            int depth = 1;
            while(depth > 0 && state->index < state->tokens->length){
                Token t = next(state);
                if(equal(t, "{")) depth++;
                if(equal(t, "}")) depth--;
            }
            class_def->body_token_end = (int)state->index; // past the closing }
        }
        class_def->body_tokens = state->tokens;

        class_table_add(class_def);

        // Register forward-declared struct so constructor 'this' type is valid
        StructDef* fwd = malloc(sizeof(StructDef));
        fwd->name = name.value;
        fwd->members = NULL;
        fwd->member_count = 0;
        fwd->total_size = 0;
        struct_table_add(fwd);

        return name.value;
    }

    class_def->is_generic_template = false;
    class_def->body_token_start = -1;
    class_def->body_token_end = -1;

    // Register forward-declared struct so methods can reference 'this' type
    StructDef* fwd = malloc(sizeof(StructDef));
    fwd->name = name.value;
    fwd->members = NULL;
    fwd->member_count = 0;
    fwd->total_size = 0;
    struct_table_add(fwd);

    // Add class to class table early so method bodies can resolve class name
    class_table_add(class_def);

    expect(state, "{");
    parse_class_body(state, class_def);
    expect(state, "}");

    // Align total_size to 8
    int align = 8;
    class_def->total_size = (class_def->total_size + align - 1) & ~(align - 1);

    // Create StructDef from class fields and add to struct_table
    StructMember* members = NULL;
    if(class_def->field_count > 0)
        members = malloc(sizeof(StructMember) * class_def->field_count);
    for(int i = 0; i < class_def->field_count; i++){
        members[i].name = class_def->fields[i].name;
        members[i].type = class_def->fields[i].type;
        members[i].is_const = class_def->fields[i].is_const;
        members[i].offset = class_def->fields[i].offset;
    }
    StructDef* sdef = malloc(sizeof(StructDef));
    sdef->name = name.value;
    sdef->members = members;
    sdef->member_count = class_def->field_count;
    sdef->total_size = class_def->total_size;
    struct_table_add(sdef);

    // Add class to class table
    class_table_add(class_def);

    return name.value;
}

static Node* parse_method_def(ParserState* state, char* class_name, AccessModifier access);

static void parse_class_body(ParserState* state, ClassDef* class_def){
    ClassDef* prev_class = state->current_class;
    state->current_class = class_def;

    while(!equal(peek(state), "}")){
        skip_semicolons(state);
        if(equal(peek(state), "}")) break;
        AccessModifier access = parse_access_modifier(state);

        if(equal(peek(state), "var") || equal(peek(state), "const")){
            bool is_const = equal(peek(state), "const");
            next(state);
            Token fname = expect_ident(state);
            expect(state, ":");
            Type* ftype = parse_type(state);
            expect_stmt_end(state);

            int field_align = ftype->align > 0 ? ftype->align : 1;
            class_def->total_size = (class_def->total_size + field_align - 1) & ~(field_align - 1);

            class_def->field_count++;
            class_def->fields = realloc(class_def->fields, sizeof(ClassField) * class_def->field_count);
            class_def->fields[class_def->field_count - 1].name = fname.value;
            class_def->fields[class_def->field_count - 1].type = ftype;
            class_def->fields[class_def->field_count - 1].access = access;
            class_def->fields[class_def->field_count - 1].is_const = is_const;
            class_def->fields[class_def->field_count - 1].offset = class_def->total_size;

            class_def->total_size += ftype->size;

            // Update forward-declared struct so methods can see field offsets
            StructDef* fwd = struct_table_lookup(class_def->name);
            if(fwd){
                fwd->member_count = class_def->field_count;
                fwd->members = realloc(fwd->members, sizeof(StructMember) * class_def->field_count);
                for(int i = 0; i < class_def->field_count; i++){
                    fwd->members[i].name = class_def->fields[i].name;
                    fwd->members[i].type = class_def->fields[i].type;
                    fwd->members[i].is_const = class_def->fields[i].is_const;
                    fwd->members[i].offset = class_def->fields[i].offset;
                }
                fwd->total_size = class_def->total_size;
            }
        }
        else if(equal(peek(state), "function")){
            Node* method = parse_method_def(state, class_def->name, access);
            nodelist_add(class_def->methods, method);
        }
        else {
            fprintf(stderr, "parse error: expected 'var' or 'function' in class body at %s:%lu:%lu\n",
                    peek(state).file, peek(state).line, peek(state).column);
            exit(1);
        }
    }

    state->current_class = prev_class;
}

static HashMap generic_class_instances;
static HashMap imported_files;
static SymbolTable* shared_global_scope;

char* make_mangled_name(const char* class_name, Type** type_args, int type_arg_count){
    char* result = malloc(strlen(class_name) + 2);
    strcpy(result, class_name);
    for(int i = 0; i < type_arg_count; i++){
        char tn[128];
        if(type_args[i]->kind == TY_INT64) strcpy(tn, "_int64");
        else if(type_args[i]->kind == TY_INT32) strcpy(tn, "_int32");
        else if(type_args[i]->kind == TY_INT16) strcpy(tn, "_int16");
        else if(type_args[i]->kind == TY_INT8) strcpy(tn, "_int8");
        else if(type_args[i]->kind == TY_UINT64) strcpy(tn, "_uint64");
        else if(type_args[i]->kind == TY_UINT32) strcpy(tn, "_uint32");
        else if(type_args[i]->kind == TY_UINT16) strcpy(tn, "_uint16");
        else if(type_args[i]->kind == TY_UINT8) strcpy(tn, "_uint8");
        else if(type_args[i]->kind == TY_BOOL) strcpy(tn, "_bool");
        else if(type_args[i]->kind == TY_FLOAT64) strcpy(tn, "_float64");
        else if(type_args[i]->kind == TY_FLOAT32) strcpy(tn, "_float32");
        else if(type_args[i]->kind == TY_CHAR) strcpy(tn, "_char");
        else if(type_args[i]->kind == TY_STRING) strcpy(tn, "_string");
        else if(type_args[i]->kind == TY_ENUM) snprintf(tn, sizeof(tn), "_%s", type_args[i]->enumeration.def->name);
        else if(type_args[i]->kind == TY_STRUCT) snprintf(tn, sizeof(tn), "_%s", type_args[i]->structure.struct_def->name);
        else if(type_args[i]->kind == TY_PTR) strcpy(tn, "_voidptr");
        else if(type_args[i]->kind == TY_REF && type_args[i]->base && type_args[i]->base->kind == TY_STRUCT)
            snprintf(tn, sizeof(tn), "_%s", type_args[i]->base->structure.struct_def->name);
        else if(type_args[i]->kind == TY_REF) strcpy(tn, "_ref");
        else strcpy(tn, "_unknown");
        result = realloc(result, strlen(result) + strlen(tn) + 1);
        strcat(result, tn);
    }
    return result;
}

ClassDef* class_template_instantiate(ParserState* state, ClassDef* template_def, Type** type_args, int type_arg_count){
    // Lazy init the generic class instance cache
    static int generic_instances_inited = 0;
    if(!generic_instances_inited){
        generic_class_instances.buckets = NULL;
        generic_class_instances.capacity = 0;
        generic_class_instances.used = 0;
        generic_instances_inited = 1;
    }

    // Check if already instantiated
    char* mangled = make_mangled_name(template_def->name, type_args, type_arg_count);
    ClassDef* existing = hashmap_get(&generic_class_instances, mangled);
    if(existing) return existing;

    if(template_def->body_token_start < 0 || template_def->body_token_end < 0){
        fprintf(stderr, "internal error: template '%s' has no body tokens\n", template_def->name);
        exit(1);
    }

    // Resolve type arg names: convert Type* to their string names
    char** type_arg_names = malloc(sizeof(char*) * type_arg_count);
    for(int i = 0; i < type_arg_count; i++){
        if(type_args[i]->kind == TY_INT64) type_arg_names[i] = "int64";
        else if(type_args[i]->kind == TY_INT32) type_arg_names[i] = "int32";
        else if(type_args[i]->kind == TY_INT16) type_arg_names[i] = "int16";
        else if(type_args[i]->kind == TY_INT8) type_arg_names[i] = "int8";
        else if(type_args[i]->kind == TY_UINT64) type_arg_names[i] = "uint64";
        else if(type_args[i]->kind == TY_UINT32) type_arg_names[i] = "uint32";
        else if(type_args[i]->kind == TY_UINT16) type_arg_names[i] = "uint16";
        else if(type_args[i]->kind == TY_UINT8) type_arg_names[i] = "uint8";
        else if(type_args[i]->kind == TY_BOOL) type_arg_names[i] = "bool";
        else if(type_args[i]->kind == TY_FLOAT64) type_arg_names[i] = "float64";
        else if(type_args[i]->kind == TY_FLOAT32) type_arg_names[i] = "float32";
        else if(type_args[i]->kind == TY_CHAR) type_arg_names[i] = "char";
        else if(type_args[i]->kind == TY_STRING) type_arg_names[i] = "string";
        else if(type_args[i]->kind == TY_ENUM) type_arg_names[i] = type_args[i]->enumeration.def->name;
        else if(type_args[i]->kind == TY_STRUCT) type_arg_names[i] = type_args[i]->structure.struct_def->name;
        else if(type_args[i]->kind == TY_PTR){
            type_arg_names[i] = malloc(20);
            snprintf(type_arg_names[i], 20, "ptr");
        }
        else if(type_args[i]->kind == TY_REF){
            // A class name denotes a reference (ref<Class>); substitute the
            // class name itself so templates see `ptr<Name>` instead of the
            // invalid `ptr<ref>`.
            if(type_args[i]->base && type_args[i]->base->kind == TY_STRUCT)
                type_arg_names[i] = strdup(type_args[i]->base->structure.struct_def->name);
            else {
                type_arg_names[i] = malloc(20);
                snprintf(type_arg_names[i], 20, "ref");
            }
        }
        else {
            fprintf(stderr, "error: unsupported type arg for generic class\n");
            exit(1);
        }
    }

    // Get the correct token list (the one from the file where the template was defined)
    TokenList* template_tokens = template_def->body_tokens;
    if(!template_tokens){
        fprintf(stderr, "internal error: template '%s' has no body tokens\n", template_def->name);
        exit(1);
    }

    // Create a new TokenList from the template body tokens with type substitution
    // body_token_start is the index of the token BEFORE '{'
    // body_token_end is the index of the closing '}'
    // Body content (excluding braces) is at indices body_token_start+2 .. body_token_end-1
    int body_start = template_def->body_token_start + 2;
    int body_end = template_def->body_token_end; // exclusive (past the '}')
    int body_len = body_end - body_start;

    // Count tokens to allocate: header + body + closing brace.
    // Each type param occurrence that maps to a class expands to
    // `ref < Name >`; the trailing ti++ is skipped for expanded tokens,
    // so 4 extra slots are needed (writes land at ti..ti+3).
    int total_tokens = 5 + body_len; // "class", "mangled", "{", body..., "}"
    for(int i = body_start; i < body_end; i++){
        Token* src = &template_tokens->tokens[i];
        if(src->value){
            for(int p = 0; p < template_def->type_param_count; p++){
                if(strcmp(src->value, template_def->type_params[p]) == 0 &&
                   type_args[p] && type_args[p]->kind == TY_REF){
                    // `ref < Name >` replaces a single `T` token; the trailing
                    // ti++ below is skipped for expanded tokens, so we need
                    // 4 extra slots (writes land at ti..ti+3).
                    total_tokens += 4;
                    break;
                }
            }
        }
    }
    Token* new_tokens = calloc(total_tokens, sizeof(Token));

    int ti = 0;

    // Find a valid token from the template for file/line references
    Token* ref_tok = &template_tokens->tokens[template_def->body_token_start + 1];

    // Token for "class" keyword
    new_tokens[ti].kind = TT_KEYWORD;
    new_tokens[ti].value = "class";
    new_tokens[ti].file = ref_tok->file;
    new_tokens[ti].line = ref_tok->line;
    ti++;

    // Token for class name (mangled)
    new_tokens[ti].kind = TT_IDENT;
    new_tokens[ti].value = mangled;
    new_tokens[ti].file = ref_tok->file;
    new_tokens[ti].line = ref_tok->line;
    ti++;

    // Opening brace
    new_tokens[ti].kind = TT_PUNCT;
    new_tokens[ti].value = "{";
    new_tokens[ti].file = ref_tok->file;
    new_tokens[ti].line = ref_tok->line;
    ti++;

    // Copy body tokens with substitution (body content between the braces)
    for(int i = body_start; i < body_end; i++){
        Token* src = &template_tokens->tokens[i];
        new_tokens[ti] = *src;
        new_tokens[ti].value = strdup(src->value ? src->value : "");

        // Substitute type param names with concrete types
        int expanded = 0;
        for(int p = 0; p < template_def->type_param_count; p++){
            if(strcmp(new_tokens[ti].value, template_def->type_params[p]) == 0){
                free((void*)new_tokens[ti].value);

                // Classes are reference types; a `T` value is already a reference.
                // Emit `ref < Name >` so e.g. `ptr<T>` becomes a pointer to a
                // reference (correct for generic containers of classes).
                if(type_args[p] && type_args[p]->kind == TY_REF &&
                   type_args[p]->base && type_args[p]->base->kind == TY_STRUCT){
                    expanded = 1;
                    new_tokens[ti].kind = TT_IDENT;
                    new_tokens[ti].value = "ref";
                    new_tokens[ti].file = src->file;
                    new_tokens[ti].line = src->line;
                    ti++;
                    new_tokens[ti].kind = TT_PUNCT;
                    new_tokens[ti].value = "<";
                    new_tokens[ti].file = src->file;
                    new_tokens[ti].line = src->line;
                    ti++;
                    new_tokens[ti].kind = TT_IDENT;
                    new_tokens[ti].value = strdup(type_args[p]->base->structure.struct_def->name);
                    new_tokens[ti].file = src->file;
                    new_tokens[ti].line = src->line;
                    ti++;
                    new_tokens[ti].kind = TT_PUNCT;
                    new_tokens[ti].value = ">";
                    new_tokens[ti].file = src->file;
                    new_tokens[ti].line = src->line;
                    ti++;
                    break;
                }

                new_tokens[ti].value = strdup(type_arg_names[p]);
                // If the substituted name is a type keyword, update the token kind
                const char* v = new_tokens[ti].value;
                if(strcmp(v, "int64") == 0 || strcmp(v, "int32") == 0 || strcmp(v, "int16") == 0 || strcmp(v, "int8") == 0 ||
                   strcmp(v, "uint64") == 0 || strcmp(v, "uint32") == 0 || strcmp(v, "uint16") == 0 || strcmp(v, "uint8") == 0 ||
                   strcmp(v, "float64") == 0 || strcmp(v, "float32") == 0 ||
                   strcmp(v, "bool") == 0 || strcmp(v, "char") == 0 || strcmp(v, "void") == 0 || strcmp(v, "string") == 0){
                    new_tokens[ti].kind = TT_KEYWORD;
                }
                break;
            }
        }
        if(!expanded) ti++;
    }

    // Closing brace
    new_tokens[ti].kind = TT_PUNCT;
    new_tokens[ti].value = "}";
    new_tokens[ti].file = ref_tok->file;
    new_tokens[ti].line = ref_tok->line;
    ti++;

    // Build the TokenList
    TokenList* sub_tokens = malloc(sizeof(TokenList));
    sub_tokens->tokens = new_tokens;
    sub_tokens->length = ti;
    sub_tokens->size = ti;

    // Create a ParserState for the sub-tokens
    ParserState sub_state;
    sub_state.tokens = sub_tokens;
    sub_state.index = -1;
    sub_state.scope = symbol_table_make();
    sub_state.global_scope = state->global_scope;
    sub_state.stack_offset = 0;
    sub_state.current_file_dir = state->current_file_dir;
    sub_state.current_class = NULL;
    sub_state.in_constructor = false;
    sub_state.generic_methods_head = NULL;
    sub_state.current_trait = NULL;
    sub_state.current_trait_generic_count = 0;
    sub_state.current_trait_generic_names = NULL;
    sub_state.current_return_type = NULL;
    sub_state.suppress_class_ref = 0;
    sub_state.current_func_generic_count = 0;
    sub_state.current_func_generic_names = NULL;

    // Parse the concrete class
    Token t = next(&sub_state);
    if(t.kind != TT_KEYWORD || !equal(t, "class")){
        fprintf(stderr, "internal error: expected 'class' in generic instantiation\n");
        exit(1);
    }

    // Cache the instantiation BEFORE parsing to break infinite recursion
    // (methods returning Set<T> would otherwise trigger re-instantiation)
    // Use a placeholder first; will update with real class after parsing
    // Forward-register the name so inner instantiations find it
    {
        ClassDef* existing = hashmap_get(&generic_class_instances, mangled);
        if(!existing){
            ClassDef* placeholder = malloc(sizeof(ClassDef));
            memset(placeholder, 0, sizeof(ClassDef));
            placeholder->name = mangled;
            hashmap_put(&generic_class_instances, mangled, placeholder);
        }
    }

    // Call parse_class_def on the sub-tokens
    char* concrete_name = parse_class_def(&sub_state);

    // Look up the newly created class
    ClassDef* concrete = class_table_lookup(concrete_name);
    if(!concrete){
        fprintf(stderr, "internal error: failed to create concrete class '%s'\n", concrete_name);
        exit(1);
    }

    concrete->mangled_name = mangled;

    // Cache the instantiation
    hashmap_put(&generic_class_instances, mangled, concrete);

    // Add concrete class methods to pending list for AST collection
    ConcreteClassMethodsList* item = malloc(sizeof(ConcreteClassMethodsList));
    item->methods = concrete->methods;
    item->next = state->generic_methods_head;
    state->generic_methods_head = item;

    free(type_arg_names);

    return concrete;
}

static Node* parse_method_def(ParserState* state, char* class_name, AccessModifier access){
    expect(state, "function");
    Token mname = next(state);
    if(mname.kind != TT_IDENT && !(mname.kind == TT_KEYWORD && equal(mname, "new"))){
        fprintf(stderr, "parse error: expected method name, got '%s' at %s:%lu:%lu\n",
                mname.value, mname.file, mname.line, mname.column);
        exit(1);
    }
    expect(state, "(");

    int is_constructor = (strcmp(mname.value, "new") == 0);

    // Create scope for method
    SymbolTable* prev_scope = state->scope;
    int prev_stack_offset = state->stack_offset;
    state->scope = symbol_table_make();
    state->stack_offset = 0;

    // Add 'this' as first parameter
    Node* this_param = make_node(ND_PARAM);
    this_param->param.name = "this";
    this_param->param.type = ref_type(struct_type(struct_table_lookup(class_name)));

    NodeList* params = make_nodelist();
    nodelist_add(params, this_param);

    // Parse remaining parameters ('this' occupies [rbp+16], so params start at [rbp+24])
    int param_offset = 24;
    if(!equal(peek(state), ")")){
        while(true){
            Token pname = expect_ident(state);
            expect(state, ":");
            Type* ptype = parse_type(state);

            state->stack_offset -= 8;
            Symbol sym = { .name = pname.value, .type = ptype, .offset = param_offset, .is_const = false };
            symbol_table_add(state->scope, sym);

            int param_size = 8;
            if(ptype && ptype->kind == TY_STRING) param_size = 16;
            if(ptype && ptype->kind == TY_ARRAY) param_size = 16;
            if(ptype && ptype->kind == TY_STRUCT) param_size = ptype->size;
            param_offset += param_size;

            Node* param = make_node(ND_PARAM);
            param->param.name = pname.value;
            param->param.type = ptype;
            nodelist_add(params, param);

            if(!equal(peek(state), ",")) break;
            next(state);
        }
    }
    expect(state, ")");

    // Build mangled name: ClassName_methodName
    char* full_name = NULL;
    if(is_constructor){
        int param_count = (int)params->length - 1; // exclude 'this'
        full_name = malloc(strlen(class_name) + 1 + strlen(mname.value) + 32);
        sprintf(full_name, "%s_%s_%d", class_name, mname.value, param_count);
    } else {
        full_name = malloc(strlen(class_name) + 1 + strlen(mname.value) + 1);
        sprintf(full_name, "%s_%s", class_name, mname.value);
    }

    // Parse return type
    Type* return_type = NULL;
    if(equal(peek(state), ":")){
        next(state);
        return_type = parse_type(state);
    }

    // Parse body
    state->current_return_type = return_type;
    state->in_constructor = is_constructor;
    NodeList* body;
    if(equal(peek(state), "=>")){
        next(state);
        Node* expr = parse_expr(state);
        expect_stmt_end(state);

        if(!return_type) return_type = infer_type(expr, state);
        if(expr->type == ND_ENUMCONS && !expr->enumcons.resolved_type && return_type){
            resolve_enumcons(state, expr, return_type);
        }

        Node* ret = make_node(ND_RETURN);
        ret->return_stmt.expr = expr;
        body = make_nodelist();
        nodelist_add(body, ret);
    } else if(equal(peek(state), "{")) {
        expect(state, "{");

        body = make_nodelist();
        while(true){
            if(equal(peek(state), "}")) break;
            skip_semicolons(state);
            if(equal(peek(state), "}")) break;
            nodelist_add(body, parse_stmt(state));
        }
        expect(state, "}");
    }
    state->in_constructor = false;
    Node* node = make_node(ND_FUNCDEF);
    node->funcdef.name = full_name;
    node->funcdef.unmangled_name = mname.value;
    node->funcdef.body = body;
    node->funcdef.params = params;
    node->funcdef.stack_size = -state->stack_offset;
    node->funcdef.scope = (void*)state->scope;
    node->funcdef.return_type = return_type;
    node->funcdef.is_extern = (body == NULL);

    state->scope = prev_scope;
    state->stack_offset = prev_stack_offset;
    return node;
}

static void parse_trait_def(ParserState* state){
    Token name = expect_ident(state);

    // Parse generic type parameters if present: trait Collection<T>
    int generic_count = 0;
    char** generic_names = NULL;
    if(equal(peek(state), "<")){
        next(state);
        while(true){
            Token gp = expect_ident(state);
            generic_count++;
            generic_names = realloc(generic_names, sizeof(char*) * generic_count);
            generic_names[generic_count - 1] = gp.value;
            if(!equal(peek(state), ",")) break;
            next(state);
        }
        expect(state, ">");
    }

    // Set current trait context for parse_type_base to resolve type params
    TraitDef* trait_def = malloc(sizeof(TraitDef));
    trait_def->name = name.value;
    trait_def->method_sigs = make_nodelist();
    state->current_trait = trait_def;
    state->current_trait_generic_count = generic_count;
    state->current_trait_generic_names = generic_names;

    expect(state, "{");

    while(!equal(peek(state), "}")){
        expect(state, "function");
        Token mname = expect_ident(state);
        expect(state, "(");

        NodeList* params = make_nodelist();
        if(!equal(peek(state), ")")){
            while(true){
                Token pname = expect_ident(state);
                expect(state, ":");
                Type* ptype = parse_type(state);

                Node* param = make_node(ND_PARAM);
                param->param.name = pname.value;
                param->param.type = ptype;
                nodelist_add(params, param);

                if(!equal(peek(state), ",")) break;
                next(state);
            }
        }
        expect(state, ")");

        Type* return_type = ty_void;
        if(equal(peek(state), ":")){
            next(state);
            return_type = parse_type(state);
        }
        expect_stmt_end(state);

        Node* sig = make_node(ND_FUNCDEF);
        sig->funcdef.name = mname.value;
        sig->funcdef.params = params;
        sig->funcdef.return_type = return_type;
        sig->funcdef.body = NULL;
        sig->funcdef.is_extern = true;
        nodelist_add(trait_def->method_sigs, sig);
    }
    expect(state, "}");

    trait_table_add(trait_def);

    // Clear current trait context
    state->current_trait = NULL;
    state->current_trait_generic_count = 0;
    state->current_trait_generic_names = NULL;
}

static NodeList* parse_func_params(ParserState* state){
    NodeList* params = make_nodelist();
    if(equal(peek(state), ")")) return params;

    while(true){
        Token pname = expect_ident(state);
        expect(state, ":");
        Type* ptype = parse_type(state);

        Node* param = make_node(ND_PARAM);
        param->param.name = pname.value;
        param->param.type = ptype;
        nodelist_add(params, param);

        if(!equal(peek(state), ",")) break;
        next(state);
    }
    return params;
}

static Node* parse_generic_func_template(ParserState* state, Token name){
    next(state); // consume '<'

    int type_param_count = 0;
    char** type_params = NULL;
    while(true){
        Token tp = expect_ident(state);
        type_param_count++;
        type_params = realloc(type_params, sizeof(char*) * type_param_count);
        type_params[type_param_count - 1] = tp.value;
        if(!equal(peek(state), ",")) break;
        next(state);
    }
    expect(state, ">");

    // Activate the type-parameter context so `parse_type` resolves `T`/`E`
    // into TY_TYPE_PARAM leaves during the signature parse below.
    int saved_count = state->current_func_generic_count;
    char** saved_names = state->current_func_generic_names;
    state->current_func_generic_count = type_param_count;
    state->current_func_generic_names = type_params;

    GenericFuncTemplate* tmpl = calloc(1, sizeof(GenericFuncTemplate));
    tmpl->name = name.value;
    tmpl->type_param_names = type_params;
    tmpl->type_param_count = type_param_count;
    tmpl->tokens = state->tokens;
    tmpl->file_dir = state->current_file_dir;
    if(!equal(peek(state), "(")){
        fprintf(stderr, "parse error: expected '(' for generic function '%s' at %s:%lu:%lu\n",
                name.value, name.file, name.line, name.column);
        exit(1);
    }
    // body_start is the index of the '('; instantiation re-parses the span
    // starting at that token (matching parse_funcdef's `expect("(")`).
    tmpl->body_start = (int)state->index + 1;
    next(state); // consume '('

    tmpl->params = parse_func_params(state);
    expect(state, ")");

    if(equal(peek(state), ":")){
        next(state);
        tmpl->return_type = parse_type(state);
    }

    // Capture the body token range without re-interpreting it. A ';' is no
    // longer part of the language, so an external generic template is simply
    // one with no '=>' and no '{' body.
    if(equal(peek(state), "=>")){
        // Expression body: span runs up to the terminating ';', which is the
        // first ';' at paren/bracket/brace depth zero (so `;` inside a match
        // block or nested call is not mistaken for the terminator).
        int end = state->index; // index of '=' of '=>'
        int depth = 0;
        if(end + 1 < (int)state->tokens->length) end++; // skip the '>'
        while(end < (int)state->tokens->length){
            Token t = state->tokens->tokens[end];
            if(equal(t, "(") || equal(t, "[") || equal(t, "{")) depth++;
            if(equal(t, ")") || equal(t, "]") || equal(t, "}")) depth--;
            if(equal(t, ";") && depth == 0){ end++; break; }
            end++;
        }
        tmpl->body_end = end;
        while(state->index < tmpl->body_end - 1) next(state);
    } else if(equal(peek(state), "{")){
        // Block body: span runs up to (and past) the matching '}'.
        // The '{' is at state->index + 1 and is NOT recounted by the scan.
        int depth = 1;
        int end = state->index + 1;
        while(depth > 0 && end + 1 < (int)state->tokens->length){
            end++;
            Token t = state->tokens->tokens[end];
            if(equal(t, "{")) depth++;
            if(equal(t, "}")) depth--;
        }
        tmpl->body_end = end + 1;
        while(state->index < tmpl->body_end - 1) next(state);
    } else {
        // No '=>' and no '{': external generic template declaration.
        tmpl->body_end = (int)state->index + 1;
        while(state->index < tmpl->body_end - 1) next(state);
    }

    state->current_func_generic_count = saved_count;
    state->current_func_generic_names = saved_names;

    // Register the template (no funcdef is emitted for the template itself).
    if(!generic_func_templates.buckets) generic_func_templates = (HashMap){0};
    GenericFuncTemplate* existing = generic_func_template_lookup(tmpl->name);
    if(existing){
        fprintf(stderr, "parse error: duplicate generic function template '%s' at %s:%lu:%lu\n",
                name.value, name.file, name.line, name.column);
        exit(1);
    }
    hashmap_put(&generic_func_templates, strdup(tmpl->name), tmpl);

    return NULL;
}

static Node* parse_funcdef(ParserState* state){
    Token name = expect_ident(state);

    // Generic function template: `function name<T, E>(...)`. The body is
    // stored as raw tokens and re-parsed with concrete type arguments on
    // first call (mirrors how generic class templates work).
    if(equal(peek(state), "<")){
        return parse_generic_func_template(state, name);
    }

    expect(state, "(");

    NodeList* params = parse_func_params(state);
    expect(state, ")");

    Type* return_type = NULL;
    if(equal(peek(state), ":")){
        next(state);
        return_type = parse_type(state);
    }

    // A trailing ';' is no longer part of the language. An external
    // declaration is simply a function with no body (no '{' and no '=>').

    NodeList* body = NULL;

    state->current_return_type = return_type;

    if(equal(peek(state), "=>")){
        next(state);

        state->scope = symbol_table_make();
        state->stack_offset = 0;

        int arrow_param_offset = 16;
        for(uint64_t i = 0; i < params->length; i++){
            Node* p = params->nodes[i];
            state->stack_offset -= 8;
            Symbol sym = { .name = p->param.name, .type = p->param.type, .offset = arrow_param_offset, .is_const = false };
            symbol_table_add(state->scope, sym);
            int param_size = 8;
            if(p->param.type && p->param.type->kind == TY_STRING) param_size = 16;
            if(p->param.type && p->param.type->kind == TY_ARRAY) param_size = 16;
            if(p->param.type && p->param.type->kind == TY_STRUCT) param_size = p->param.type->size;
            arrow_param_offset += param_size;
        }

        Node* expr = parse_expr(state);
        expect_stmt_end(state);

        if(!return_type) return_type = infer_type(expr, state);
        if(expr->type == ND_ENUMCONS && !expr->enumcons.resolved_type && return_type){
            resolve_enumcons(state, expr, return_type);
        }

        Node* ret = make_node(ND_RETURN);
        ret->return_stmt.expr = expr;

        body = make_nodelist();
        nodelist_add(body, ret);
    } else if(equal(peek(state), "{")) {
        expect(state, "{");

        state->scope = symbol_table_make();
        state->stack_offset = 0;

        int param_offset = 16;
        for(uint64_t i = 0; i < params->length; i++){
            Node* p = params->nodes[i];
            state->stack_offset -= 8;
            Symbol sym = { .name = p->param.name, .type = p->param.type, .offset = param_offset, .is_const = false };
            symbol_table_add(state->scope, sym);
            int param_size = 8;
            if(p->param.type && p->param.type->kind == TY_STRING) param_size = 16;
            if(p->param.type && p->param.type->kind == TY_ARRAY) param_size = 16;
            if(p->param.type && p->param.type->kind == TY_STRUCT) param_size = p->param.type->size;
            param_offset += param_size;
        }

        body = make_nodelist();
        while(true){
            if(equal(peek(state), "}")) break;
            skip_semicolons(state);
            if(equal(peek(state), "}")) break;
            nodelist_add(body, parse_stmt(state));
        }
        expect(state, "}");
    }

    // An external declaration may end with an explicit ';'.
    if(body == NULL && equal(peek(state), ";")){
        next(state);
    }

    Node* node = make_node(ND_FUNCDEF);
    node->funcdef.params = params;
    node->funcdef.body = body;
    node->funcdef.stack_size = -state->stack_offset;
    node->funcdef.scope = (void*)state->scope;
    node->funcdef.return_type = return_type;
    node->funcdef.is_extern = (body == NULL);
    node->funcdef.unmangled_name = name.value;

    // Generate mangled name and register in function table
    char* mangled = make_func_mangled_name(name.value, params);
    Node* existing_fn = function_table_lookup(mangled);
    if(existing_fn && existing_fn->funcdef.is_extern){
        free(node);
        free(mangled);
        return existing_fn;
    }
    if(existing_fn){
        fprintf(stderr, "parse error: duplicate function definition '%s' at %s:%lu:%lu\n",
                mangled, name.file, name.line, name.column);
        exit(1);
    }
    node->funcdef.name = mangled;
    function_table_add(mangled, node);
    return node;
}

Token next(ParserState* state){
    if(state->index + 1 >= state->tokens->length){
        Token eof;
        eof.kind = TT_EOF;
        eof.value = NULL;
        return eof;
    }
    state->index++;
    Token result = state->tokens->tokens[state->index];
    return result;
}

void nodelist_add(NodeList* list, Node* n){
    if(list->length >= list->size){
        Node** new_list = realloc(list->nodes, list->size * 2 * sizeof(Node*));
        if(new_list == NULL){
            perror("realloc");
            exit(1);
        }
        list->size *= 2;
        list->nodes = new_list;
    }
    list->nodes[list->length] = n;
    list->length++;
}

Node* nodelist_get(NodeList* list, uint64_t index){
    assert(index < list->length);
    return list->nodes[index];
}

// Root parse context used when lazily resolving deferred types (e.g. enum
// variant fields) from outside the active parse pass.
ParserState* quarzum_deferred_state = NULL;

Type* parse_type_string(const char* str, ParserState* state, const char* filename){
    ParserState* effective = state ? state : quarzum_deferred_state;
    if(!effective){
        fprintf(stderr, "internal error: no parser state available for deferred type resolution\n");
        exit(1);
    }

    File f;
    f.name = (char*)(filename ? filename : "<enum-field>");
    f.size = strlen(str);
    f.content = (char*)str;
    TokenList* sub_tokens = tokenize_file(&f);

    ParserState sub_state;
    sub_state.tokens = sub_tokens;
    sub_state.index = -1;
    sub_state.scope = symbol_table_make();
    sub_state.global_scope = effective->global_scope;
    sub_state.stack_offset = 0;
    sub_state.current_file_dir = effective->current_file_dir;
    sub_state.current_class = NULL;
    sub_state.in_constructor = false;
    sub_state.generic_methods_head = NULL;
    sub_state.current_trait = NULL;
    sub_state.current_trait_generic_count = 0;
    sub_state.current_trait_generic_names = NULL;
    sub_state.current_return_type = NULL;

    Type* ty = parse_type(&sub_state);

    // Splicing generic class methods is done inside the class template
    // instantiation using sub_state.generic_methods_head; merge them back into
    // the effective state so codegen can emit the concrete class methods.
    if(sub_state.generic_methods_head){
        ConcreteClassMethodsList* cur = sub_state.generic_methods_head;
        while(cur){
            ConcreteClassMethodsList* nxt = cur->next;
            cur->next = effective->generic_methods_head;
            effective->generic_methods_head = cur;
            cur = nxt;
        }
    }

    // Free the temporary token list (ident/keyword/str/num values are
    // strdup'd; punct values point to static symbol strings).
    for(uint64_t i = 0; i < sub_tokens->length; i++){
        Token* tk = &sub_tokens->tokens[i];
        if(tk->kind == TT_IDENT || tk->kind == TT_KEYWORD ||
           tk->kind == TT_NUM || tk->kind == TT_STR){
            if(tk->value) free((void*)tk->value);
        }
    }
    free(sub_tokens->tokens);
    free(sub_tokens);

    return ty;
}

Node* parse(TokenList* tokens){
    ParserState state;
    if(!imported_files.buckets)
        imported_files = (HashMap){0};
    if(shared_global_scope == NULL)
        shared_global_scope = symbol_table_make();
    state.tokens = tokens;
    state.index = -1;
    state.scope = shared_global_scope;
    state.global_scope = shared_global_scope;
    state.stack_offset = 0;
    state.current_class = NULL;
    state.in_constructor = false;
    state.generic_methods_head = NULL;
    state.current_trait = NULL;
    state.current_trait_generic_count = 0;
    state.current_trait_generic_names = NULL;
    state.current_return_type = NULL;
    state.suppress_class_ref = 0;
    state.current_func_generic_count = 0;
    state.current_func_generic_names = NULL;

    if(tokens->length > 0 && tokens->tokens[0].file){
        state.current_file_dir = dir_from_path(tokens->tokens[0].file);
    } else {
        state.current_file_dir = strdup(".");
    }

    if(quarzum_deferred_state == NULL){
        quarzum_deferred_state = calloc(1, sizeof(ParserState));
        quarzum_deferred_state->global_scope = state.global_scope;
        quarzum_deferred_state->current_file_dir = strdup(state.current_file_dir);
    }

    Node* ast = malloc(sizeof(Node));
    ast->program_node.children = make_nodelist();
    ast->program_node.global_scope = (void*)state.scope;

    while(true){
        Token t = next(&state);
        if(t.kind == TT_EOF) break;
    if(equal(t, "function")){
        Node* fd = parse_funcdef(&state);
        if(fd) nodelist_add(ast->program_node.children, fd);
        state.scope = state.global_scope;
    }
        else if(equal(t, "enum")){
            parse_enum_def(&state);
        }
        else if(equal(t, "struct")){
            parse_struct_def(&state);
        }
        else if(equal(t, "class")){
            char* class_name = parse_class_def(&state);
            // Add all method function defs to children
            ClassDef* cdef = class_table_lookup(class_name);
            for(uint64_t i = 0; i < cdef->methods->length; i++){
                nodelist_add(ast->program_node.children, cdef->methods->nodes[i]);
            }
            state.scope = state.global_scope;
        }
        else if(equal(t, "trait")){
            parse_trait_def(&state);
        }
        else if(equal(t, "var")){
            nodelist_add(ast->program_node.children, parse_vardecl(&state, false, true, true));
        }
        else if(equal(t, "const")){
            nodelist_add(ast->program_node.children, parse_vardecl(&state, true, true, true));
        }
        else if(equal(t, "import")){
            Token path = next(&state);
            if(path.kind != TT_STR){
                fprintf(stderr, "parse error: expected string after import, got '%s' at %s:%lu:%lu\n",
                        path.value, path.file, path.line, path.column);
                exit(1);
            }

            char* resolved = resolve_import_path(&state, path.string_value);
            if(hashmap_get(&imported_files, resolved)){
                free(resolved);
                continue;
            }
            hashmap_put(&imported_files, resolved, (void*)1);
            TokenList* imported_tokens = tokenize(resolved);
            Node* imported_ast = parse(imported_tokens);
            for(uint64_t i = 0; i < imported_ast->program_node.children->length; i++){
                nodelist_add(ast->program_node.children, imported_ast->program_node.children->nodes[i]);
            }
        }
    }

    // Append methods from concretely instantiated generic classes
    ConcreteClassMethodsList* curr = state.generic_methods_head;
    while(curr){
        for(uint64_t i = 0; i < curr->methods->length; i++){
            nodelist_add(ast->program_node.children, curr->methods->nodes[i]);
        }
        curr = curr->next;
    }

    // Append concretely instantiated generic functions (deduplicated via
    // generic_func_appended so recursive re-parses don't re-add).
    append_generic_funcs_to_ast(ast);

    return ast;
}
