#include "quarzum.h"

Type *ty_void = &(Type){TY_VOID, 0, 0};
Type *ty_bool = &(Type){TY_BOOL, 1, 1};

Type *ty_char = &(Type){TY_CHAR, 1, 1};

Type *ty_int8 = &(Type){TY_INT8, 1, 1};
Type *ty_int16 = &(Type){TY_INT16, 2, 2};
Type *ty_int32 = &(Type){TY_INT32, 4, 4};
Type *ty_int64 = &(Type){TY_INT64, 8, 8};

Type *ty_uint8 = &(Type){TY_UINT8, 1, 1};
Type *ty_uint16 = &(Type){TY_UINT16, 2, 2};
Type *ty_uint32 = &(Type){TY_UINT32, 4, 4};
Type *ty_uint64 = &(Type){TY_UINT64, 8, 8};

Type *ty_float32 = &(Type){TY_FLOAT32, 4, 4};
Type *ty_float64 = &(Type){TY_FLOAT64, 8, 8};

Type *ty_type_param = &(Type){TY_TYPE_PARAM, 8, 8};


static Type* new_type(TypeKind kind, int size, int align) {
    Type* type = calloc(1, sizeof(Type));
    if(type == NULL) {
        perror("calloc");
        exit(1);
    }
    type->kind = kind;
    type->size = size;
    type->align = align;
    return type;
}

Type* make_type_param_type(const char* name){
    Type* ty = new_type(TY_TYPE_PARAM, 8, 8);
    ty->type_param_name = strdup(name);
    return ty;
}

bool is_any_int(Type* ty){
    return is_int(ty) || is_uint(ty);
}

bool is_int(Type* ty) {
    return ty->kind >= TY_INT8 && ty->kind <= TY_INT64;
}

bool is_uint(Type* ty){
    return ty->kind >= TY_UINT8 && ty->kind <= TY_UINT64;
}

bool is_float(Type* ty){
    return ty->kind >= TY_FLOAT32 && ty->kind <= TY_FLOAT64;
}

bool is_numeric(Type* ty){
    return ty->kind == TY_CHAR || is_any_int(ty) || is_float(ty);
}

// A string is a slice<char> (written char[]). The internal TY_STRING type and
// a TY_ARRAY over char are interchangeable representations of it.
bool is_string_type(Type* ty){
    return ty && (ty->kind == TY_STRING ||
                  (ty->kind == TY_ARRAY && ty->base && ty->base->kind == TY_CHAR));
}

bool is_compatible(Type* t1, Type* t2) {
    if (t1 == t2)
        return true;
    if (!t1 || !t2)
        return false;

    if (t1->origin)
        return is_compatible(t1->origin, t2);

    if (t2->origin)
        return is_compatible(t1, t2->origin);

    if(is_numeric(t1) && is_numeric(t2))
        return true;

    // string (TY_STRING) and char[] (TY_ARRAY of char) are the same type
    if ((t1->kind == TY_STRING && t2->kind == TY_ARRAY) ||
        (t2->kind == TY_STRING && t1->kind == TY_ARRAY)){
        Type* arr = t1->kind == TY_ARRAY ? t1 : t2;
        return arr->base && arr->base->kind == TY_CHAR;
    }

    if (t1->kind != t2->kind)
        return false;

    switch (t1->kind) {
        case TY_CHAR:
        case TY_BOOL:
        case TY_INT8:
        case TY_INT16:
        case TY_INT32:
        case TY_INT64:
        case TY_UINT8:
        case TY_UINT16:
        case TY_UINT32:
        case TY_UINT64:
        case TY_FLOAT32:
        case TY_FLOAT64:
            return true;
        case TY_PTR:
            return is_compatible(t1->base, t2->base);
        case TY_FUNC: {
            if (!is_compatible(t1->function.return_type, t2->function.return_type))
            return false;
            
            if(t1->function.param_count != t2->function.param_count) return false;
            
            for (int i = 0; i < t1->function.param_count; i++){
                Type p1 = t1->function.params[i];
                Type p2 = t2->function.params[i];
                if (!is_compatible(&p1, &p2)) return false;
            }
        
            return true;
        }
        case TY_ARRAY:
            // T[] is a slice: all arrays are slices, so compatibility depends
            // only on the element type (no static arrays or VLAs in the language).
            return is_compatible(t1->base, t2->base);
        case TY_STRING:
            return true;
        case TY_ENUM:
            if (t1->enumeration.def != t2->enumeration.def)
                return false;
            if (t1->enumeration.type_arg_count != t2->enumeration.type_arg_count)
                return false;
            for (int i = 0; i < t1->enumeration.type_arg_count; i++) {
                if (!is_compatible(t1->enumeration.type_args[i], t2->enumeration.type_args[i]))
                    return false;
            }
            return true;
        case TY_STRUCT:
            if(!t1->structure.struct_def || !t2->structure.struct_def)
                return false;
            return strcmp(t1->structure.struct_def->name, t2->structure.struct_def->name) == 0;
    }
    return false;
}

Type* copy_type(Type* ty) {
    Type* ret = calloc(1, sizeof(Type));
    if(ret == NULL){
        perror("calloc");
        exit(1);
    }
    *ret = *ty;
    ret->origin = ty;
    return ret;
}

Type* pointer_to(Type* base) {
    Type* ty = new_type(TY_PTR, 8, 8);
    ty->base = base;
    return ty;
}

Type* func_type(Type* return_type) {
    Type *ty = new_type(TY_FUNC, 8, 8);
    ty->function.return_type = return_type;
    return ty;
}

Type* dynarray_type(Type* base) {
    Type* ty = new_type(TY_ARRAY, 16, 8);
    ty->base = base;
    return ty;
}

Type* string_type(void) {
    Type* ty = new_type(TY_STRING, 16, 8);
    ty->base = ty_char;
    return ty;
}


Type* enum_def_type(EnumDef* def) {
    Type* ty = new_type(TY_ENUM, 8, 8);
    ty->enumeration.def = def;
    ty->enumeration.type_args = NULL;
    ty->enumeration.type_arg_count = 0;
    ty->enumeration.data_size = 0;
    ty->enumeration.variant_tag_offsets = NULL;
    ty->enumeration.variant_data_offsets = NULL;
    ty->enumeration.variant_data_sizes = NULL;
    return ty;
}

Type* struct_type(StructDef* def) {
  Type* ty = new_type(TY_STRUCT, def->total_size, 1);
  ty->structure.struct_def = def;
  ty->structure.members = NULL;
  ty->structure.is_flexible = false;
  ty->structure.is_packed = false;
  // Carry the generic application metadata (template + type args) so that
  // inference can match `List<T>` against `List<int64>`.
  ClassDef* cdef = class_table_lookup(def->name);
  if(cdef && cdef->template_def){
    ty->structure.generic_template = cdef->template_def;
    ty->structure.type_args = cdef->type_args;
    ty->structure.type_arg_count = cdef->type_arg_count;
  }
  return ty;
}

// Global enum definitions table
static HashMap enum_table;

void enum_table_init(void) {
    memset(&enum_table, 0, sizeof(HashMap));
}

EnumDef* enum_table_lookup(const char* name) {
    return (EnumDef*)hashmap_get(&enum_table, (char*)name);
}

void enum_table_add(EnumDef* def) {
    hashmap_put(&enum_table, def->name, def);
}

// Global struct definitions table
static HashMap struct_table;

void struct_table_init(void) {
    memset(&struct_table, 0, sizeof(HashMap));
}

StructDef* struct_table_lookup(const char* name) {
    return (StructDef*)hashmap_get(&struct_table, (char*)name);
}

void struct_table_add(StructDef* def) {
    hashmap_put(&struct_table, def->name, def);
}

// Global class definitions table
static HashMap class_table;

void class_table_init(void) {
    memset(&class_table, 0, sizeof(HashMap));
}

ClassDef* class_table_lookup(const char* name) {
    return (ClassDef*)hashmap_get(&class_table, (char*)name);
}

void class_table_add(ClassDef* def) {
    hashmap_put(&class_table, def->name, def);
}

// Global trait definitions table
static HashMap trait_table;

void trait_table_init(void) {
    memset(&trait_table, 0, sizeof(HashMap));
}

TraitDef* trait_table_lookup(const char* name) {
    return (TraitDef*)hashmap_get(&trait_table, (char*)name);
}

void trait_table_add(TraitDef* def) {
    hashmap_put(&trait_table, def->name, def);
}

// Function overload table: mangled_name -> ND_FUNCDEF
static HashMap function_table;

void function_table_init(void) {
    memset(&function_table, 0, sizeof(HashMap));
}

Node* function_table_lookup(const char* mangled_name) {
    return (Node*)hashmap_get(&function_table, (char*)mangled_name);
}

void function_table_add(const char* mangled_name, Node* funcdef) {
    hashmap_put(&function_table, (char*)mangled_name, funcdef);
}



const char* type_kind_to_name(Type* type){
    if(!type) return "unknown";
    switch(type->kind){
        case TY_INT64: return "int64";
        case TY_INT32: return "int32";
        case TY_INT16: return "int16";
        case TY_INT8: return "int8";
        case TY_UINT64: return "uint64";
        case TY_UINT32: return "uint32";
        case TY_UINT16: return "uint16";
        case TY_UINT8: return "uint8";
        case TY_BOOL: return "bool";
        case TY_FLOAT64: return "float64";
        case TY_FLOAT32: return "float32";
        case TY_CHAR: return "char";
        case TY_STRING: return "array";  // string is compatible with uint8[]
        case TY_PTR: return "ptr";
        case TY_FUNC: return "func";
        case TY_ARRAY: return "array";
        case TY_ENUM: return type->enumeration.def->name;
        case TY_STRUCT: return type->structure.struct_def->name;
        default: return "unknown";
    }
}

char* make_func_mangled_name(const char* name, NodeList* params){
    size_t len = strlen(name);
    char* result = malloc(len + 1);
    strcpy(result, name);
    if(params){
        for(uint64_t i = 0; i < params->length; i++){
            Type* ptype = params->nodes[i]->param.type;
            const char* tname = type_kind_to_name(ptype);
            len += 1 + strlen(tname);
            result = realloc(result, len + 1);
            strcat(result, "_");
            strcat(result, tname);
        }
    }
    return result;
}

static bool is_ident_char(char c){
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static char* type_to_expr_string(Type* t){
    char buf[512];
    switch(t->kind){
        case TY_INT8:   strcpy(buf, "int8"); break;
        case TY_INT16:  strcpy(buf, "int16"); break;
        case TY_INT32:  strcpy(buf, "int32"); break;
        case TY_INT64:  strcpy(buf, "int64"); break;
        case TY_UINT8:  strcpy(buf, "uint8"); break;
        case TY_UINT16: strcpy(buf, "uint16"); break;
        case TY_UINT32: strcpy(buf, "uint32"); break;
        case TY_UINT64: strcpy(buf, "uint64"); break;
        case TY_BOOL:   strcpy(buf, "bool"); break;
        case TY_CHAR:   strcpy(buf, "char"); break;
        case TY_FLOAT32:  strcpy(buf, "float32"); break;
        case TY_FLOAT64:  strcpy(buf, "float64"); break;
        case TY_STRING: strcpy(buf, "string"); break;
        case TY_VOID:   strcpy(buf, "void"); break;
        case TY_STRUCT:
            strcpy(buf, t->structure.struct_def->name);
            break;
        case TY_ENUM: {
            strcpy(buf, t->enumeration.def->name);
            if(t->enumeration.type_arg_count > 0){
                strcat(buf, "<");
                for(int i = 0; i < t->enumeration.type_arg_count; i++){
                    if(i) strcat(buf, ",");
                    char* sub = type_to_expr_string(t->enumeration.type_args[i]);
                    strcat(buf, sub);
                    free(sub);
                }
                strcat(buf, ">");
            }
            break;
        }
        case TY_PTR: {
            strcpy(buf, "ptr<");
            char* sub = type_to_expr_string(t->base);
            strcat(buf, sub);
            free(sub);
            strcat(buf, ">");
            break;
        }
        case TY_ARRAY: {
            char* sub = type_to_expr_string(t->base);
            strcpy(buf, sub);
            free(sub);
            strcat(buf, "[]");
            break;
        }
        default:
            strcpy(buf, "void");
    }
    return strdup(buf);
}

static char* substitute_enum_params(const char* expr, EnumDef* def, Type** type_args, int type_arg_count){
    if(def->generic_param_count == 0 || type_arg_count < def->generic_param_count)
        return strdup(expr);
    char* result = strdup(expr);
    for(int i = 0; i < def->generic_param_count; i++){
        const char* param = def->generic_params[i];
        int plen = strlen(param);
        char* concrete = type_to_expr_string(type_args[i]);
        int clen = strlen(concrete);
        size_t cap = strlen(result) + 1;
        char* out = malloc(cap);
        char* dst = out;
        const char* src = result;
        while(*src){
            if(strncmp(src, param, plen) == 0){
                char before = (src > result) ? src[-1] : ' ';
                char after = src[plen];
                if(!is_ident_char(before) && !is_ident_char(after)){
                    size_t need = (dst - out) + clen + strlen(src + plen) + 1;
                    if(need > cap){
                        cap = need;
                        size_t off = dst - out;
                        out = realloc(out, cap);
                        dst = out + off;
                    }
                    memcpy(dst, concrete, clen);
                    dst += clen;
                    src += plen;
                    continue;
                }
            }
            size_t need = (dst - out) + 2 + strlen(src + 1);
            if(need > cap){
                cap = need;
                size_t off = dst - out;
                out = realloc(out, cap);
                dst = out + off;
            }
            *dst++ = *src++;
        }
        *dst = '\0';
        free(result);
        free(concrete);
        result = out;
    }
    return result;
}

Type* enum_instantiate(ParserState* state, Type* def_type, Type** type_args, int type_arg_count) {
    EnumDef* def = def_type->enumeration.def;
    Type* ty = calloc(1, sizeof(Type));
    *ty = *def_type;
    ty->enumeration.type_args = type_args;
    ty->enumeration.type_arg_count = type_arg_count;

    int data_size = 0;
    int align = 8;

    ty->enumeration.variant_tag_offsets = malloc(sizeof(int) * def->variant_count);
    ty->enumeration.variant_data_offsets = malloc(sizeof(int) * def->variant_count);
    ty->enumeration.variant_data_sizes = malloc(sizeof(int) * def->variant_count);

    for (int v = 0; v < def->variant_count; v++) {
        EnumVariant* var = &def->variants[v];
        int vdata_size = 0;

        for (int p = 0; p < var->param_count; p++) {
            const char* expr = var->param_type_names[p];
            Type* ptype = NULL;
            for(int i = 0; i < def->generic_param_count && i < type_arg_count; i++){
                if(strcmp(expr, def->generic_params[i]) == 0){
                    ptype = type_args[i];
                    break;
                }
            }
            if(!ptype){
                char* substituted = substitute_enum_params(expr, def, type_args, type_arg_count);
                ptype = parse_type_string(substituted, state, def->file);
                free(substituted);
            }
            vdata_size += ptype->size;
        }

        ty->enumeration.variant_data_sizes[v] = vdata_size;
        if (vdata_size > data_size) {
            data_size = vdata_size;
        }
    }

    ty->enumeration.data_size = data_size;
    ty->size = 8 + data_size;
    ty->align = align;
    return ty;
}

static Type *get_common_type(Type *ty1, Type *ty2) {
    if (ty1->base) {
        return pointer_to(ty1->base);
    }

    if (ty1->kind == TY_FUNC)
        return pointer_to(ty1);
    if (ty2->kind == TY_FUNC)
        return pointer_to(ty2);

    if (ty1->kind == TY_FLOAT64 || ty2->kind == TY_FLOAT64)
        return ty_float64;
    if (ty1->kind == TY_FLOAT32 || ty2->kind == TY_FLOAT32)
        return ty_float32;

    if (ty1->size < 4)
        ty1 = ty_int32;
    if (ty2->size < 4)
        ty2 = ty_int32;

    if (ty1->size != ty2->size)
        return (ty1->size < ty2->size) ? ty2 : ty1;
    return ty1;
}

void add_type(Node* node);

static Symbol* check_lookup(SymbolTable* local, SymbolTable* global, const char* name){
    Symbol* sym = symbol_table_lookup(local, name);
    if(sym) return sym;
    if(global && global != local){
        sym = symbol_table_lookup(global, name);
        if(sym) return sym;
    }
    return NULL;
}

static void check_stmts(NodeList* stmts, SymbolTable* local, SymbolTable* global);
static void check_stmt(Node* node, SymbolTable* local, SymbolTable* global);

static bool in_constructor_check = false;
static Type* current_return_type = NULL;
static const char* current_func_name = NULL;

static void check_type_error(const char* msg, Node* node){
    fprintf(stderr, "type error: %s\n", msg);
    exit(1);
}

static char* best_promoted_name;
static int best_promoted_cost;

// Searches function overloads allowing implicit widening of integer args
// (e.g. an int64 literal passed to a uint64 parameter). Picks the match
// requiring the fewest promotions.
static void search_promoted_funcall(NodeList* args, uint64_t idx, char* buf, size_t bufcap, int cost){
    if(idx == args->length){
        if(function_table_lookup(buf) && cost < best_promoted_cost){
            best_promoted_cost = cost;
            free(best_promoted_name);
            best_promoted_name = malloc(strlen(buf) + 1);
            strcpy(best_promoted_name, buf);
        }
        return;
    }
    Type* at = args->nodes[idx]->ty;
    if(!at) return;
    const char* tname = type_kind_to_name(at);
    size_t l = strlen(buf);
    snprintf(buf + l, bufcap - l, "_%s", tname);
    search_promoted_funcall(args, idx + 1, buf, bufcap, cost);
    buf[l] = '\0';
    if(is_any_int(at)){
        Type* promoted[4];
        int pn = 0;
        switch(at->kind){
            case TY_INT8: promoted[pn++] = ty_uint8; promoted[pn++] = ty_uint16; promoted[pn++] = ty_uint32; promoted[pn++] = ty_uint64; break;
            case TY_INT16: promoted[pn++] = ty_uint16; promoted[pn++] = ty_uint32; promoted[pn++] = ty_uint64; break;
            case TY_INT32: promoted[pn++] = ty_uint32; promoted[pn++] = ty_uint64; break;
            case TY_INT64: promoted[pn++] = ty_uint64; break;
            case TY_UINT8: promoted[pn++] = ty_uint16; promoted[pn++] = ty_uint32; promoted[pn++] = ty_uint64; break;
            case TY_UINT16: promoted[pn++] = ty_uint32; promoted[pn++] = ty_uint64; break;
            case TY_UINT32: promoted[pn++] = ty_uint64; break;
            default: break;
        }
        for(int k = 0; k < pn; k++){
            const char* pname = type_kind_to_name(promoted[k]);
            size_t l2 = strlen(buf);
            snprintf(buf + l2, bufcap - l2, "_%s", pname);
            search_promoted_funcall(args, idx + 1, buf, bufcap, cost + 1);
            buf[l2] = '\0';
        }
    }
}

static char* resolve_promoted_funcall(const char* name, NodeList* args){
    size_t bufcap = strlen(name) + 2;
    for(uint64_t i = 0; i < args->length; i++)
        bufcap += 1 + 8;
    char* buf = malloc(bufcap);
    strcpy(buf, name);
    best_promoted_name = NULL;
    best_promoted_cost = 1 << 30;
    search_promoted_funcall(args, 0, buf, bufcap, 0);
    free(buf);
    return best_promoted_name;
}

static void check_expr(Node* node, SymbolTable* local, SymbolTable* global){
    if(!node) return;

    switch(node->type){
        case ND_INTLIT:
            // Character literals already carry ty_char (set at parse time).
            if(!node->ty) node->ty = ty_int64;
            break;
        case ND_FLOATLIT:
            switch(node->floatlit.float_kind){
                case TY_FLOAT32: node->ty = ty_float32; break;
                case TY_FLOAT64: node->ty = ty_float64; break;
                default: node->ty = ty_float32; break;
            }
            break;
        case ND_BOOLLIT:
            node->ty = ty_bool;
            break;
        case ND_STRLIT:
            node->ty = string_type();
            break;
        case ND_VARREF: {
            Symbol* sym = check_lookup(local, global, node->varref.name);
            if(sym){
                node->ty = sym->type;
                if(getenv("QZ_DEBUG_TY") && strcmp(node->varref.name, "k") == 0)
                    fprintf(stderr, "[ty] varref 'k' sym->type=%p kind=%d size=%ld\n",
                            (void*)sym->type, sym->type ? sym->type->kind : -1,
                            sym->type ? (long)sym->type->size : -1);
            }
            break;
        }
        case ND_BINARY_EXPR: {
            check_expr(node->binary_expr.lhs, local, global);
            check_expr(node->binary_expr.rhs, local, global);
            Type* lty = node->binary_expr.lhs->ty;
            Type* rty = node->binary_expr.rhs->ty;

            switch(node->binary_expr.op){
                case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
                    if(lty && rty){
                        if(!is_compatible(lty, rty)){
                            fprintf(stderr, "DEBUG: op=%d lkind=%d rkind=%d op=%s ltype=%d rtype=%d\n",
                                    node->binary_expr.op,
                                    lty ? lty->kind : -1, rty ? rty->kind : -1,
                                    node->binary_expr.op == OP_ADD ? "ADD" :
                                    node->binary_expr.op == OP_SUB ? "SUB" :
                                    node->binary_expr.op == OP_MUL ? "MUL" :
                                    node->binary_expr.op == OP_DIV ? "DIV" : "MOD",
                                    node->binary_expr.lhs->type, node->binary_expr.rhs->type);
                            check_type_error("incompatible types in arithmetic expression", node);
                        }
                        if(is_string_type(lty) && is_string_type(rty)){
                            if(node->binary_expr.op != OP_ADD)
                                check_type_error("string operator only supports +", node);
                            node->ty = lty;
                        } else {
                            if(!is_numeric(lty) && lty->kind != TY_PTR)
                                check_type_error("arithmetic operator requires numeric or pointer operand", node);
                            node->ty = lty ? lty : rty;
                        }
                    } else {
                        node->ty = lty ? lty : rty;
                    }
                    break;
                case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
                    if(lty && rty && !is_compatible(lty, rty))
                        check_type_error("incompatible types in comparison", node);
                    if((node->binary_expr.op == OP_EQ || node->binary_expr.op == OP_NE) &&
                       lty && rty && is_string_type(lty) && is_string_type(rty))
                        node->binary_expr.is_string_cmp = true;
                    node->ty = ty_bool;
                    break;
                case OP_AND: case OP_OR: case OP_XOR:
                    node->ty = ty_bool;
                    break;
                case OP_BITAND: case OP_BITOR: case OP_BITXOR:
                    if(lty && rty && !is_compatible(lty, rty))
                        check_type_error("incompatible types in bitwise expression", node);
                    node->ty = lty ? lty : rty;
                    break;
                default:
                    node->ty = lty;
                    break;
            }
            break;
        }
        case ND_UNARY_EXPR: {
            check_expr(node->unary_expr.operand, local, global);
            Type* oty = node->unary_expr.operand->ty;

            switch(node->unary_expr.op){
                case OP_NOT:
                    if(oty && oty->kind != TY_BOOL)
                        check_type_error("'not' operator requires bool operand", node);
                    node->ty = ty_bool;
                    break;
                case OP_BITNOT:
                    if(oty && !is_any_int(oty))
                        check_type_error("'!!' operator requires integer operand", node);
                    node->ty = oty;
                    break;
                case OP_NEG:
                    if(oty && !is_any_int(oty) && !is_float(oty))
                        check_type_error("'-' operator requires numeric operand", node);
                    node->ty = oty;
                    break;
                case OP_DEREF:
                    if(oty && oty->kind != TY_PTR)
                        check_type_error("dereference requires ptr operand (ref cannot be dereferenced)", node);
                    node->ty = oty ? oty->base : NULL;
                    break;
                case OP_ADDR:
                    node->ty = oty ? pointer_to(oty) : NULL;
                    break;
            }
            break;
        }
        case ND_TERNARY: {
            check_expr(node->ternary_expr.cond, local, global);
            if(node->ternary_expr.cond->ty && node->ternary_expr.cond->ty->kind != TY_BOOL)
                check_type_error("ternary condition must be bool", node);
            check_expr(node->ternary_expr.then_expr, local, global);
            check_expr(node->ternary_expr.else_expr, local, global);
            Type* tty = node->ternary_expr.then_expr->ty;
            Type* ety = node->ternary_expr.else_expr->ty;
            if(tty && ety && !is_compatible(tty, ety))
                check_type_error("ternary branches have incompatible types", node);
            node->ty = tty ? tty : ety;
            break;
        }
        case ND_CAST:
            check_expr(node->cast.expr, local, global);
            resolve_funcref_expected(node->cast.expr, node->cast.target_type);
            node->ty = node->cast.target_type;
            break;
        case ND_ALLOC:
            node->ty = pointer_to(node->alloc.alloc_type);
            if(node->alloc.count)
                check_expr(node->alloc.count, local, global);
            break;
        case ND_INDEX: {
            check_expr(node->index_expr.base, local, global);
            check_expr(node->index_expr.index, local, global);
            Type* bty = node->index_expr.base->ty;
            Type* ity = node->index_expr.index->ty;
            if(ity && !(is_any_int(ity) || ity->kind == TY_CHAR || ity->kind == TY_ENUM)){
                fprintf(stderr, "type error: array index must be an integer\n");
                exit(1);
            }
            if(bty && bty->kind == TY_PTR)
                node->ty = bty->base;
            else if(bty && (bty->kind == TY_ARRAY || bty->kind == TY_STRING))
                node->ty = bty->base;
            else
                node->ty = NULL;
            break;
        }
        case ND_MEMBER: {
            check_expr(node->member.base, local, global);
            Type* bty = node->member.base->ty;
            if(bty && bty->kind == TY_STRUCT && bty->structure.struct_def){
                StructDef* sdef = bty->structure.struct_def;
                bool found = false;
                for(int i = 0; i < sdef->member_count; i++){
                    if(strcmp(sdef->members[i].name, node->member.field_name) == 0){
                        node->ty = sdef->members[i].type;
                        found = true;
                        break;
                    }
                }
                if(!found){
                    fprintf(stderr, "type error: struct '%s' has no member '%s'\n",
                            sdef->name, node->member.field_name);
                    exit(1);
                }
            } else if(bty && bty->kind == TY_PTR &&
                      bty->base && bty->base->kind == TY_STRUCT &&
                      bty->base->structure.struct_def){
                StructDef* sdef = bty->base->structure.struct_def;
                bool found = false;
                for(int i = 0; i < sdef->member_count; i++){
                    if(strcmp(sdef->members[i].name, node->member.field_name) == 0){
                        node->ty = sdef->members[i].type;
                        found = true;
                        break;
                    }
                }
                if(!found){
                    fprintf(stderr, "type error: struct '%s' has no member '%s'\n",
                            sdef->name, node->member.field_name);
                    exit(1);
                }
            } else if(bty && bty->kind == TY_ARRAY){
                if(strcmp(node->member.field_name, "length") == 0) node->ty = ty_uint64;
                if(strcmp(node->member.field_name, "pointer") == 0 ||
                   strcmp(node->member.field_name, "ptr") == 0) node->ty = pointer_to(bty->base);
            } else if(bty && bty->kind == TY_STRING){
                if(strcmp(node->member.field_name, "length") == 0) node->ty = ty_uint64;
                if(strcmp(node->member.field_name, "pointer") == 0 ||
                   strcmp(node->member.field_name, "ptr") == 0) node->ty = pointer_to(ty_char);
            }
            break;
        }
        case ND_FUNCCALL:
            for(uint64_t i = 0; i < node->funcall.args->length; i++)
                check_expr(node->funcall.args->nodes[i], local, global);
            {
                // Resolve function overload: build mangled name from arg types
                size_t len = strlen(node->funcall.name);
                char* mangled = malloc(len + 1);
                strcpy(mangled, node->funcall.name);
                int can_resolve = 1;
                for(uint64_t i = 0; i < node->funcall.args->length; i++){
                    Type* atype = node->funcall.args->nodes[i]->ty;
                    if(!atype) { can_resolve = 0; break; }
                    const char* tname = type_kind_to_name(atype);
                    len += 1 + strlen(tname);
                    mangled = realloc(mangled, len + 1);
                    strcat(mangled, "_");
                    strcat(mangled, tname);
                }
                if(can_resolve){
                    Node* target = function_table_lookup(mangled);
                    if(target){
                        free(node->funcall.name);
                        node->funcall.name = mangled;
                        node->ty = target->funcdef.return_type;
                        mangled = NULL;
                    } else {
                        char* promoted = resolve_promoted_funcall(node->funcall.name, node->funcall.args);
                        if(promoted){
                            Node* ptarget = function_table_lookup(promoted);
                            if(ptarget){
                                free(node->funcall.name);
                                node->funcall.name = promoted;
                                node->ty = ptarget->funcdef.return_type;
                                mangled = NULL;
                                promoted = NULL;
                            }
                            if(promoted) free(promoted);
                        }
                    }
                    if(mangled){
                        // C FFI fallback: a bare-name function registered by
                        // an `extern function` declaration (`strlen`, `malloc`).
                        Node* ctarget = function_table_lookup(node->funcall.name);
                        if(ctarget && ctarget->funcdef.is_c_extern){
                            int param_count = (int)ctarget->funcdef.params->length;
                            int arg_count = (int)node->funcall.args->length;
                            int arity_ok = ctarget->funcdef.is_variadic
                                         ? (arg_count >= param_count)
                                         : (arg_count == param_count);
                            if(!arity_ok){
                                check_type_error("wrong number of arguments in call to extern function", node);
                            }
                            node->funcall.is_c_call = true;
                            node->funcall.c_func = ctarget;
                            node->ty = ctarget->funcdef.return_type;
                            free(mangled);
                            mangled = NULL;
                        }
                    }
                    if(mangled){
                        // Generic function template fallback: instantiate from
                        // the already-resolved argument types (check_expr runs
                        // after parsing, so every arg type is known here).
                        uint64_t nargs = node->funcall.args->length;
                        Type** gargs = malloc(sizeof(Type*) * nargs);
                        int gok = 1;
                        for(uint64_t i = 0; i < nargs; i++){
                            gargs[i] = node->funcall.args->nodes[i]->ty;
                            if(!gargs[i]) gok = 0;
                        }
                        Node* gtarget = NULL;
                        if(gok)
                            gtarget = resolve_generic_funcall_types(quarzum_deferred_state,
                                                                    node->funcall.name,
                                                                    gargs, (int)nargs);
                        free(gargs);
                        if(gtarget){
                            free(node->funcall.name);
                            node->funcall.name = strdup(gtarget->funcdef.name);
                            node->ty = gtarget->funcdef.return_type;
                            mangled = NULL;
                        }
                    }
                }
                if(mangled) free(mangled);
            }
            break;
        case ND_ENUMCONS:
            if(!node->enumcons.resolved_type){
                EnumDef* def = enum_table_lookup(node->enumcons.enum_name);
                if(def){
                    node->enumcons.resolved_type = enum_instantiate(NULL, enum_def_type(def), NULL, 0);
                    for(int i = 0; i < def->variant_count; i++){
                        if(strcmp(def->variants[i].name, node->enumcons.variant_name) == 0){
                            node->enumcons.variant_index = i;
                            break;
                        }
                    }
                }
            }
            node->ty = node->enumcons.resolved_type;
            if(node->enumcons.args){
                for(uint64_t i = 0; i < node->enumcons.args->length; i++)
                    check_expr(node->enumcons.args->nodes[i], local, global);
            }
            break;
        case ND_STRUCTCONS: {
            StructDef* sdef = struct_table_lookup(node->structcons.name);
            if(sdef) node->ty = struct_type(sdef);
            if(node->structcons.args){
                for(uint64_t i = 0; i < node->structcons.args->length; i++)
                    check_expr(node->structcons.args->nodes[i], local, global);
            }
            break;
        }
        case ND_MATCH:
            check_expr(node->match.scrutinee, local, global);
            node->match.is_enum = (node->match.scrutinee->ty &&
                                   node->match.scrutinee->ty->kind == TY_ENUM);
            for(uint64_t i = 0; i < node->match.cases->length; i++){
                Node* c = node->match.cases->nodes[i];
                if(node->match.is_enum){
                    // Enum match: dispatch is by variant, labels are not values.
                    // Resolve each case's variant index against the scrutinee's
                    // enum so codegen can dispatch by the real variant index
                    // regardless of the order the cases appear in.
                    EnumDef* def = node->match.scrutinee->ty->enumeration.def;
                    int vi = -1;
                    if(c->match_case.variant_name){
                        for(int v = 0; v < def->variant_count; v++){
                            if(strcmp(def->variants[v].name, c->match_case.variant_name) == 0){
                                vi = v;
                                break;
                            }
                        }
                    }
                    if(vi == -1){
                        check_type_error("match case is not a variant of the scrutinee enum", c);
                        c->match_case.variant_index = 0;
                    } else {
                        c->match_case.variant_index = vi;
                    }
                } else {
                    // Numeric match: the case label is a value referenced by
                    // name. Resolve its symbol and reject variant bindings.
                    if(c->match_case.bindings){
                        check_type_error("match case bindings require an enum scrutinee", node);
                    }
                    check_expr(c->match_case.label, local, global);
                    if(c->match_case.label->type == ND_VARREF){
                        Symbol* lsym = check_lookup(local, global, c->match_case.label->varref.name);
                        if(lsym){
                            c->match_case.label->varref.is_global = lsym->is_global;
                            c->match_case.label->varref.offset = lsym->offset;
                        }
                    }
                }
                check_stmt(c->match_case.body, local, global);
            }
            if(node->match.default_body){
                check_expr(node->match.default_body, local, global);
            }
            {
                Type* common = NULL;
                for(uint64_t i = 0; i < node->match.cases->length; i++){
                    Type* t = node->match.cases->nodes[i]->match_case.body->ty;
                    if(!t) continue;
                    if(!common){
                        common = t;
                    } else if(!is_compatible(common, t)){
                        common = NULL;
                        break;
                    }
                }
                if(node->match.default_body && node->match.default_body->ty){
                    if(common && !is_compatible(common, node->match.default_body->ty))
                        common = NULL;
                    else if(!common)
                        common = node->match.default_body->ty;
                }
                if(common) node->match.result_type = common;
            }
            node->ty = node->match.result_type;
            break;
        case ND_STRING_EQ: case ND_STRING_NE:
            check_expr(node->string_cmp.lhs, local, global);
            check_expr(node->string_cmp.rhs, local, global);
            node->ty = ty_bool;
            break;
        case ND_ENUM_PATTERN_CMP: {
            // `x == Enum.Variant(a: T, ...)`: dispatch on the scrutinee's
            // enum, resolve the variant index and validate the bindings
            // against the variant parameters.
            check_expr(node->enum_pattern_cmp.lhs, local, global);
            Type* lt = node->enum_pattern_cmp.lhs ? node->enum_pattern_cmp.lhs->ty : NULL;
            if(!lt || lt->kind != TY_ENUM){
                check_type_error("enum pattern comparison requires an enum operand", node);
                break;
            }
            EnumDef* def = lt->enumeration.def;
            int vi = -1;
            if(def){
                for(int v = 0; v < def->variant_count; v++){
                    if(strcmp(def->variants[v].name, node->enum_pattern_cmp.variant_name) == 0){
                        vi = v;
                        break;
                    }
                }
            }
            if(vi == -1){
                check_type_error("pattern is not a variant of the compared enum", node);
                break;
            }
            node->enum_pattern_cmp.variant_index = vi;
            node->enum_pattern_cmp.resolved_type = lt;

            int param_count = def->variants[vi].param_count;
            int binding_count = node->enum_pattern_cmp.bindings
                                    ? (int)node->enum_pattern_cmp.bindings->length : 0;
            if(binding_count > param_count){
                check_type_error("pattern has more bindings than variant parameters", node);
                break;
            }
            node->ty = ty_bool;
            break;
        }
        case ND_NEW:
            if(node->new_expr.args){
                for(uint64_t i = 0; i < node->new_expr.args->length; i++)
                    check_expr(node->new_expr.args->nodes[i], local, global);
            }
            {
                StructDef* sdef = struct_table_lookup(node->new_expr.class_name);
                if(sdef) node->ty = pointer_to(struct_type(sdef));
            }
            break;
        case ND_METHODCALL:
            check_expr(node->methodcall.object, local, global);
            if(node->methodcall.args){
                for(uint64_t i = 0; i < node->methodcall.args->length; i++)
                    check_expr(node->methodcall.args->nodes[i], local, global);
            }
            // Derive the receiver's struct name when it was unknown at parse
            // time (e.g. the result of a generic call).
            if(!node->methodcall.class_name){
                Type* oty = node->methodcall.object->ty;
                if(oty && oty->kind == TY_PTR &&
                   oty->base && oty->base->kind == TY_STRUCT && oty->base->structure.struct_def){
                    node->methodcall.class_name = oty->base->structure.struct_def->name;
                } else if(oty && oty->kind == TY_STRUCT && oty->structure.struct_def){
                    node->methodcall.class_name = oty->structure.struct_def->name;
                }
            }
            if(node->methodcall.class_name){
                // Resolve the overload by building ClassName_method[_type...]
                // from the argument types, with the same integer promotions as
                // free functions.
                char* base = malloc(strlen(node->methodcall.class_name) + 1 +
                                    strlen(node->methodcall.method) + 1);
                sprintf(base, "%s_%s", node->methodcall.class_name, node->methodcall.method);

                Node* target = NULL;
                char* candidate = NULL;
                if(node->methodcall.args){
                    size_t len = strlen(base) + 1;
                    candidate = malloc(len);
                    strcpy(candidate, base);
                    bool can_resolve = true;
                    for(uint64_t i = 0; i < node->methodcall.args->length; i++){
                        Type* at = node->methodcall.args->nodes[i]->ty;
                        if(!at){ can_resolve = false; break; }
                        const char* tn = type_kind_to_name(at);
                        len += 1 + strlen(tn);
                        candidate = realloc(candidate, len);
                        strcat(candidate, "_");
                        strcat(candidate, tn);
                    }
                    if(!can_resolve){
                        free(candidate);
                        candidate = NULL;
                    }
                } else {
                    candidate = strdup(base);
                }
                if(candidate) target = function_table_lookup(candidate);
                if(!target){
                    if(candidate) free(candidate);
                    candidate = resolve_promoted_funcall(base, node->methodcall.args);
                    target = candidate ? function_table_lookup(candidate) : NULL;
                }
                free(base);
                if(target){
                    node->methodcall.mangled_name = candidate;
                    node->ty = target->funcdef.return_type;
                    candidate = NULL;
                }
                if(candidate) free(candidate);

                if(!node->ty){
                    // Fallback: a single method whose name matches (prefix)
                    // resolves even when the argument types do not match by
                    // promotion (e.g. a literal passed to a narrower parameter).
                    ClassDef* cdef = class_table_lookup(node->methodcall.class_name);
                    const char* cname = node->methodcall.class_name;
                    size_t cl = strlen(cname);
                    const char* method = node->methodcall.method;
                    size_t ml = strlen(method);
                    Node* only = NULL;
                    int matches = 0;
                    if(cdef){
                        for(uint64_t i = 0; i < cdef->methods->length; i++){
                            Node* m = cdef->methods->nodes[i];
                            const char* mg = m->funcdef.name;
                            if(mg && strncmp(mg, cname, cl) == 0 && mg[cl] == '_'){
                                const char* rest = mg + cl + 1;
                                if(strncmp(rest, method, ml) == 0 &&
                                   (rest[ml] == '\0' || rest[ml] == '_')){
                                    only = m;
                                    matches++;
                                }
                            }
                        }
                    }
                    if(matches == 1){
                        node->methodcall.mangled_name = only->funcdef.name;
                        node->ty = only->funcdef.return_type;
                    }
                }
            }
            break;
        case ND_THIS:
            break;
        case ND_NULL:
            break;
        case ND_FUNCREF:
            // Type is set at parse time from the referenced funcdef.
            break;
        case ND_INDIRECTCALL:
            check_expr(node->indirect_call.callee, local, global);
            if(node->indirect_call.args){
                for(uint64_t i = 0; i < node->indirect_call.args->length; i++)
                    check_expr(node->indirect_call.args->nodes[i], local, global);
            }
            if(node->indirect_call.callee->ty &&
               node->indirect_call.callee->ty->kind == TY_FUNC){
                node->ty = node->indirect_call.callee->ty->function.return_type;
            }
            break;
        case ND_PASS:
            node->ty = ty_void;
            break;
        default:
            break;
    }
}

static void check_vardecl(Node* node, SymbolTable* local, SymbolTable* global){
    if(node->vardecl.init){
        check_expr(node->vardecl.init, local, global);
        resolve_funcref_expected(node->vardecl.init, node->vardecl.type);
        if(node->vardecl.type && node->vardecl.init->ty){
            if(!is_compatible(node->vardecl.type, node->vardecl.init->ty)){
                fprintf(stderr, "DEBUG: vardecl '%s' type mismatch: type=%p init->ty=%p\n",
                        node->vardecl.name, (void*)node->vardecl.type, (void*)node->vardecl.init->ty);
                check_type_error("initializer type does not match declared type", node);
            }
        }
        if(!node->vardecl.type && node->vardecl.init->ty){
            node->vardecl.type = node->vardecl.init->ty;
            Symbol* sym = check_lookup(local, global, node->vardecl.name);
            if(sym) sym->type = node->vardecl.type;
            if(getenv("QZ_DEBUG_TY") && strcmp(node->vardecl.name, "k") == 0)
                fprintf(stderr, "[ty] vardecl 'k' init->ty kind=%d set sym->type=%p (sym %s)\n",
                        node->vardecl.init->ty->kind, (void*)(sym ? sym->type : NULL),
                        sym ? "found" : "NOT_FOUND");
        } else if(getenv("QZ_DEBUG_TY") && strcmp(node->vardecl.name, "k") == 0){
            fprintf(stderr, "[ty] vardecl 'k' skipped: vardecl.type=%p init->ty=%p (kind %d)\n",
                    (void*)node->vardecl.type, (void*)(node->vardecl.init ? node->vardecl.init->ty : NULL),
                    node->vardecl.init && node->vardecl.init->ty ? node->vardecl.init->ty->kind : -1);
        }
    }

    // Variables declared inside for bodies (and for-loop inits) are
    // removed from the symbol table when the loop scope is closed during
    // parsing, so later varrefs cannot resolve their type. Re-register them
    // with the resolved type; otherwise codegen falls back to 8-byte
    // arguments and corrupts 16-byte struct/string args (e.g. SIGSEGV in
    // Map<string,...>::resize re-inserting keys).
    if(node->vardecl.type){
        Symbol* sym = check_lookup(local, global, node->vardecl.name);
        if(!sym){
            Symbol loop_sym = { .name = node->vardecl.name, .type = node->vardecl.type,
                                .offset = node->vardecl.offset, .is_const = node->vardecl.is_const,
                                .is_global = node->vardecl.is_global };
            symbol_table_add(local, loop_sym);
        }
    }
}

static void check_stmt(Node* node, SymbolTable* local, SymbolTable* global){
    if(!node) return;

    switch(node->type){
        case ND_IF:
            check_expr(node->if_stmt.cond, local, global);
            if(node->if_stmt.cond->ty && node->if_stmt.cond->ty->kind != TY_BOOL)
                check_type_error("if condition must be bool", node);
            check_stmts(node->if_stmt.then_body, local, global);
            if(node->if_stmt.else_body)
                check_stmts(node->if_stmt.else_body, local, global);
            break;
        case ND_WHILE: case ND_DO_WHILE:
            check_expr(node->loop.cond, local, global);
            if(node->loop.cond->ty && node->loop.cond->ty->kind != TY_BOOL)
                check_type_error("loop condition must be bool", node);
            check_stmts(node->loop.body, local, global);
            break;
        case ND_FOR:
            if(node->for_stmt.init) check_stmt(node->for_stmt.init, local, global);
            if(node->for_stmt.cond){
                check_expr(node->for_stmt.cond, local, global);
                if(node->for_stmt.cond->ty && node->for_stmt.cond->ty->kind != TY_BOOL)
                    check_type_error("for condition must be bool", node);
            }
            if(node->for_stmt.update) check_stmt(node->for_stmt.update, local, global);
            check_stmts(node->for_stmt.body, local, global);
            break;
        case ND_VARDECL:
            check_vardecl(node, local, global);
            break;
        case ND_ASSIGN: {
            Symbol* sym = check_lookup(local, global, node->assign.name);
            if(sym && sym->type && node->assign.value){
                check_expr(node->assign.value, local, global);
                resolve_funcref_expected(node->assign.value, sym->type);
                if(node->assign.value->ty && !is_compatible(sym->type, node->assign.value->ty))
                    check_type_error("assignment type mismatch", node);
            } else if(node->assign.value){
                check_expr(node->assign.value, local, global);
            }
            break;
        }
        case ND_RETURN:
            if(node->return_stmt.expr){
                check_expr(node->return_stmt.expr, local, global);
                resolve_funcref_expected(node->return_stmt.expr, current_return_type);
                Type* ety = node->return_stmt.expr->ty;
                if(current_return_type && ety && current_return_type->kind != TY_VOID){
                    bool ret_slice = (current_return_type->kind == TY_STRING ||
                                      current_return_type->kind == TY_ARRAY);
                    bool expr_slice = (ety->kind == TY_STRING || ety->kind == TY_ARRAY);
                    bool ret_ptr = (current_return_type->kind == TY_PTR);
                    bool expr_int = is_any_int(ety) || ety->kind == TY_CHAR;
                    if(!(ret_slice && expr_slice) && !(ret_ptr && expr_int) &&
                       !is_compatible(current_return_type, ety)){
                        fprintf(stderr, "type error: return type does not match function return type (in %s)\n",
                                current_func_name ? current_func_name : "?");
                        exit(1);
                    }
                }
            }
            break;
        case ND_EXIT:
            if(node->exit_stmt.expr)
                check_expr(node->exit_stmt.expr, local, global);
            break;
        case ND_FUNCCALL:
            check_expr(node, local, global);
            break;
        case ND_MEMBER_ASSIGN:
            check_expr(node->member_assign.object, local, global);
            check_expr(node->member_assign.value, local, global);
            if(!in_constructor_check){
                Type* obj_type = node->member_assign.object->ty;
                StructDef* asdef = NULL;
                if(obj_type && obj_type->kind == TY_PTR &&
                   obj_type->base && obj_type->base->kind == TY_STRUCT){
                    asdef = obj_type->base->structure.struct_def;
                } else if(obj_type && obj_type->kind == TY_STRUCT){
                    asdef = obj_type->structure.struct_def;
                }
                if(asdef){
                    for(int i = 0; i < asdef->member_count; i++){
                        if(strcmp(asdef->members[i].name, node->member_assign.field_name) == 0 && asdef->members[i].is_const){
                            check_type_error("cannot assign to const field outside constructor", node);
                        }
                    }
                }
            }
            break;
        case ND_DEREF_ASSIGN:
            check_expr(node->deref_assign.target, local, global);
            check_expr(node->deref_assign.value, local, global);
            if(node->deref_assign.target->ty && node->deref_assign.target->ty->kind != TY_PTR)
                check_type_error("dereference assignment requires a ptr target (ref cannot be dereferenced)", node);
            break;
        case ND_EXPR_STMT:
            check_expr(node->expr_stmt.expr, local, global);
            break;
        case ND_DEFER:
            check_stmt(node->defer_stmt.stmt, local, global);
            break;
        case ND_FREE:
            check_expr(node->free.expr, local, global);
            if(node->free.expr->ty && node->free.expr->ty->kind != TY_PTR)
                check_type_error("free requires a ptr operand", node);
            break;
        case ND_MATCH:
            check_expr(node, local, global);
            break;
        case ND_SWITCH:
            check_expr(node->switch_stmt.scrutinee, local, global);
            for(uint64_t i = 0; i < node->switch_stmt.cases->length; i++){
                if(node->switch_stmt.cases->nodes[i]->switch_case.label)
                    check_expr(node->switch_stmt.cases->nodes[i]->switch_case.label, local, global);
                check_stmts(node->switch_stmt.cases->nodes[i]->switch_case.body, local, global);
            }
            if(node->switch_stmt.default_body)
                check_stmts(node->switch_stmt.default_body, local, global);
            break;
        default:
            check_expr(node, local, global);
            break;
    }
}

static void check_stmts(NodeList* stmts, SymbolTable* local, SymbolTable* global){
    for(uint64_t i = 0; i < stmts->length; i++)
        check_stmt(stmts->nodes[i], local, global);
}

void resolve_types(Node* ast){
    SymbolTable* global = (SymbolTable*)ast->program_node.global_scope;
    for(uint64_t i = 0; i < ast->program_node.children->length; i++){
        Node* child = ast->program_node.children->nodes[i];
        if(child->type == ND_FUNCDEF){
            in_constructor_check = (strstr(child->funcdef.name, "_ctor_") != NULL);
            current_return_type = child->funcdef.return_type;
            current_func_name = child->funcdef.name;
            SymbolTable* local = (SymbolTable*)child->funcdef.scope;
            if(getenv("QZ_DEBUG_TY")){
                fprintf(stderr, "[ty] checking func '%s' local=%p len=%u\n",
                        child->funcdef.name, (void*)local, local ? local->length : 0);
                if(local && strstr(child->funcdef.name, "resize")){
                    for(uint64_t si = 0; si < local->length; si++)
                        fprintf(stderr, "[ty]   sym[%lu]='%s' type=%p kind=%d\n",
                                si, local->symbols[si].name, (void*)local->symbols[si].type,
                                local->symbols[si].type ? local->symbols[si].type->kind : -1);
                }
            }
            if(child->funcdef.body)
                check_stmts(child->funcdef.body, local, global);
        }
        if(child->type == ND_VARDECL){
            check_vardecl(child, global, global);
        }
    }
    in_constructor_check = false;

    // Generic function instances can also be created during this pass (calls
    // whose argument types only become known here). Append and check them.
    append_generic_funcs_to_ast(ast);
    for(uint64_t i = 0; i < ast->program_node.children->length; i++){
        Node* child = ast->program_node.children->nodes[i];
        if(child->type == ND_FUNCDEF && child->funcdef.body){
            current_return_type = child->funcdef.return_type;
            current_func_name = child->funcdef.name;
            in_constructor_check = (strstr(child->funcdef.name, "_ctor_") != NULL);
            SymbolTable* local = (SymbolTable*)child->funcdef.scope;
            check_stmts(child->funcdef.body, local, global);
        }
    }
}