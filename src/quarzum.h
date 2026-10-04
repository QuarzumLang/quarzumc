#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

// hashmap.c
typedef struct {
    char *key;
    void* val;
} HashEntry;

typedef struct {
    HashEntry* buckets;
    int capacity;
    int used;
} HashMap;

void* hashmap_get(HashMap* map, char* key);
void hashmap_put(HashMap* map, char* key, void* val);
void hashmap_delete(HashMap* map, char* key);

#define unreachable() \
  fprintf(stderr, "internal error at %s:%d", __FILE__, __LINE__)


// tokenize.c
typedef struct Type Type;

typedef enum {
    TT_IDENT,
    TT_KEYWORD,
    TT_PUNCT,
    TT_STR,
    TT_NUM,
    TT_EOF
} TokenType;

typedef struct {
    char* name;
    char* content;
    uint64_t size;
} File;

typedef struct {
    TokenType kind;
    union {
        long long int int_value;
        long double float_value;
        char* string_value;
    };
    char* value;
    const char* file;
    uint64_t line;
    uint64_t column;

    /*
        True only for the synthetic ';' tokens the lexer inserts before a
        newline to terminate a statement. User-written ';' have this false.
        Both are accepted by the parser as statement terminators.
    */
    bool auto_semi;

    Type* type;
} Token;

typedef struct {
    Token* tokens;
    uint64_t size;
    uint64_t length;
} TokenList;

Token get_token(TokenList* list, uint64_t index);
void add_token(TokenList* list, Token tk);
TokenList* init_tokenlist(uint64_t size);
void print_tokens(const TokenList* tokens);
TokenList* tokenize(const char* filename);
TokenList* tokenize_file(File* file);

// parse.c

typedef struct Node Node;
typedef struct NodeList NodeList;
typedef struct StructDef StructDef;
typedef struct ClassDef ClassDef;
typedef struct TraitDef TraitDef;

typedef enum {
    ND_FUNCDEF,
    ND_IF,
    ND_EXIT,
    ND_VARDECL,
    ND_BINARY_EXPR,
    ND_UNARY_EXPR,
    ND_BOOLLIT,
    ND_INTLIT,
    ND_FLOATLIT,
    ND_STRLIT,
    ND_VARREF,
    ND_RETURN,
    ND_ASSIGN,
    ND_WHILE,
    ND_DO_WHILE,
    ND_FOR,
    ND_INDEX,
    ND_FUNCCALL,
    ND_PARAM,
    ND_STRING_EQ,
    ND_STRING_NE,
    ND_FOREACH,
    ND_MEMBER,
    ND_ENUMDEF,
    ND_STRUCTDEF,
    ND_ENUMCONS,
    ND_MATCH,
    ND_MATCH_CASE,
    ND_SWITCH,
    ND_SWITCH_CASE,
    ND_BREAK,
    ND_CONTINUE,
    ND_CAST,
    ND_ALLOC,
    ND_FREE,
    ND_DEREF_ASSIGN,
    ND_TERNARY,
    ND_EXPR_STMT,
    ND_STRUCTCONS,
    ND_CLASSDEF,
    ND_TRAITDEF,
    ND_THIS,
    ND_NULL,
    ND_NEW,
    ND_METHODCALL,
    ND_MEMBER_ASSIGN,
    ND_PASS,
    ND_ENUM_PATTERN_CMP
} NodeType;

typedef enum {
    OP_OR,
    OP_AND,
    OP_XOR,
    OP_NOT,
    OP_BITOR,
    OP_BITXOR,
    OP_BITAND,
    OP_BITNOT,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_ADDR,
    OP_DEREF,
    OP_NEG,
} OpType;

// Enum definition structures
typedef struct EnumVariant {
    char* name;
    char** param_type_names;
    int param_count;
} EnumVariant;

typedef struct EnumDef {
    char* name;
    const char* file;
    char** generic_params;
    int generic_param_count;
    EnumVariant* variants;
    int variant_count;
} EnumDef;

typedef struct {
    char* name;
    Type* type;
    bool is_const;
    int offset;
} StructMember;

struct StructDef {
    char* name;
    StructMember* members;
    int member_count;
    int total_size;
};

typedef enum {
    ACCESS_PUBLIC,
    ACCESS_PRIVATE,
    ACCESS_PROTECTED
} AccessModifier;

typedef struct {
    char* name;
    Type* type;
    AccessModifier access;
    bool is_const;
    int offset;
} ClassField;

struct ClassDef {
    char* name;
    char** type_params;
    int type_param_count;
    ClassField* fields;
    int field_count;
    int total_size;
    NodeList* methods;
    char** implements;
    int implements_count;
    bool is_generic_template;
    int body_token_start;
    int body_token_end;
    TokenList* body_tokens;
    char* mangled_name;
};

struct TraitDef {
    char* name;
    NodeList* method_sigs;
};

struct Node {
    NodeType type;
    Type* ty;

    union {
        struct {
            NodeList* children;
            void* global_scope;
        } program_node;

        struct {
            char* name;
            char* unmangled_name;
            NodeList* body;
            NodeList* params;
            int stack_size;
            void* scope;
            Type* return_type;
            bool is_extern;
        } funcdef;

        struct {
            char* name;
            Type* type;
        } param;

        struct {
            char* name;
            NodeList* args;
        } funcall;

        struct {
            Node* cond;
            NodeList* then_body;
            NodeList* else_body;
        } if_stmt;

        struct {
            Node* expr;
        } exit_stmt;

        struct {
            char* name;
            Type* type;
            Node* init;
            bool is_const;
            bool is_global;
            int offset;
        } vardecl;

        struct {
            OpType op;
            Node* lhs;
            Node* rhs;
            bool is_string_cmp;
        } binary_expr;

        struct {
            OpType op;
            Node* operand;
        } unary_expr;

        struct {
            bool value;
        } boollit;

        struct {
            long long value;
        } intlit;

        struct {
            long double value;
            int float_kind; // TY_FLOAT32, TY_FLOAT64
        } floatlit;

        struct {
            char* value;
            int length;
        } strlit;

        struct {
            char* name;
            int offset;
            bool is_string;
            bool is_global;
        } varref;

        struct {
            char* name;
            int offset;
            bool is_global;
            Node* value;
            Node* index_base;
            Node* index_expr;
            Type* var_type;
        } assign;

        struct {
            Node* expr;
        } return_stmt;

        struct {
            Node* cond;
            NodeList* body;
        } loop;

        struct {
            Node* init;
            Node* cond;
            Node* update;
            NodeList* body;
        } for_stmt;

        struct {
            Node* base;
            Node* index;
            int elem_size;
            bool is_string;
        } index_expr;

        struct {
            Node* lhs;
            Node* rhs;
        } string_cmp;

        struct {
            int list_offset;
            int id_offset;
            int index_offset;
            int elem_size;
            Node* collection;
            char* id_name;
            NodeList* body;
        } foreach;

        struct {
            Node* base;
            char* field_name;
            int field_offset;
            bool is_struct;
            bool is_class;
        } member;

        struct {
            EnumDef* def;
        } enumdef;

        struct {
            StructDef* def;
        } structdef;

        struct {
            char* enum_name;
            char* variant_name;
            NodeList* args;
            int variant_index;
            Type* resolved_type;
        } enumcons;

        struct {
            Node* scrutinee;
            NodeList* cases;
            Node* default_body;
            bool is_enum;
            Type* result_type;
        } match;

        struct {
            char* variant_name;
            NodeList* bindings;
            Node* label;
            Node* body;
            int variant_index;
        } match_case;

        struct {
            Node* scrutinee;
            NodeList* cases;
            NodeList* default_body;
            bool is_string;
        } switch_stmt;

        struct {
            Node* label;
            NodeList* body;
        } switch_case;

        struct {
            Node* expr;
            Type* target_type;
        } cast;

        struct {
            Type* alloc_type;
            Node* count;
        } alloc;

        struct {
            Node* expr;
        } free;

        struct {
            Node* target;
            Node* value;
            OpType op;
        } deref_assign;

        struct {
            Node* cond;
            Node* then_expr;
            Node* else_expr;
        } ternary_expr;

        struct {
            Node* expr;
        } expr_stmt;

        struct {
            char* name;
            NodeList* args;
            int temp_offset;
        } structcons;

        struct {
            ClassDef* def;
        } classdef;

        struct {
            TraitDef* def;
        } traitdef;

        struct {} this_expr;

        struct {} null_expr;

        struct {
            char* class_name;
            char* mangled_name;
            NodeList* args;
        } new_expr;

        struct {
            Node* object;
            char* method;
            NodeList* args;
            char* class_name;
        } methodcall;

        struct {
            Node* object;
            char* field_name;
            int field_offset;
            Node* value;
            bool is_class;
            bool is_struct;
        } member_assign;

        /*
            Enum pattern comparison: `x == Enum.Variant(a: T, ...)` (or !=).

            Evaluates to a bool and, on a tag match with `==`, copies the
            variant payload into the binding stack slots so the enclosing
            if/while body can use the bound names directly.
        */
        struct {
            Node* lhs;
            NodeList* bindings;
            char* enum_name;
            char* variant_name;
            int variant_index;
            Type* resolved_type;
            bool negated;
        } enum_pattern_cmp;
    };
};

struct NodeList {
    Node** nodes;
    uint64_t size;
    uint64_t length;
};

typedef struct {
    char* name;
    Type* type;
    int offset;
    bool is_const;
    bool is_global;
} Symbol;

typedef struct {
    Symbol* symbols;
    uint64_t size;
    uint64_t length;
} SymbolTable;

typedef struct ConcreteClassMethodsList {
    NodeList* methods;
    struct ConcreteClassMethodsList* next;
} ConcreteClassMethodsList;

typedef struct {
    TokenList* tokens;
    uint64_t index;
    SymbolTable* scope;
    SymbolTable* global_scope;
    int stack_offset;
    char* current_file_dir;
    ClassDef* current_class;
    bool in_constructor;
    ConcreteClassMethodsList* generic_methods_head;
    TraitDef* current_trait;
    int current_trait_generic_count;
    char** current_trait_generic_names;
    Type* current_return_type;
    int suppress_class_ref;
    int current_func_generic_count;
    char** current_func_generic_names;
} ParserState;

void nodelist_add(NodeList* list, Node* n);
Node* nodelist_get(NodeList* list, uint64_t index);
SymbolTable* symbol_table_make(void);
void symbol_table_add(SymbolTable* table, Symbol sym);
Symbol* symbol_table_lookup(SymbolTable* table, const char* name);

Token next(ParserState* state);
Node* parse(TokenList* tokens);

// codegen.c

void codegen(const Node* ast, FILE* output_file);

// type.c

typedef struct Member Member;


typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,
    TY_INT8,
    TY_INT16,
    TY_INT32,
    TY_INT64,
    TY_UINT8,
    TY_UINT16,
    TY_UINT32,
    TY_UINT64,
    TY_FLOAT32,
    TY_FLOAT64,
    TY_ENUM,
    TY_PTR,
    TY_REF,
    TY_FUNC,
    TY_ARRAY,
    TY_STRUCT,
    TY_STRING,
    TY_TYPE_PARAM,
    TY_NEVER
} TypeKind;

struct Type {
    TypeKind kind;
    int size;
    int align;
    bool is_atomic;
    Type* origin;

    Type* base;
    
    // Declaration?
    Token* name;
    Token* name_pos;

    // For TY_TYPE_PARAM inside generic function templates, the name of the
    // type parameter this leaf stands for (e.g. "T" in Result<T,E>).
    char* type_param_name;

    union {
        struct {
            Member* members; // TBI
            StructDef* struct_def;
            bool is_flexible;
            bool is_packed;
        } structure;

        struct {
            Type* return_type;
            Type* params;
            uint64_t param_count;
        } function;

        struct {
            EnumDef* def;
            Type** type_args;
            int type_arg_count;
            int data_size;
            int* variant_tag_offsets;
            int* variant_data_offsets;
            int* variant_data_sizes;
        } enumeration;
    };
};

extern Type *ty_type_param;

extern Type *ty_void;
extern Type *ty_bool;

extern Type *ty_char;

extern Type *ty_int8;
extern Type *ty_int16;
extern Type *ty_int32;
extern Type *ty_int64;

extern Type *ty_uint8;
extern Type *ty_uint16;
extern Type *ty_uint32;
extern Type *ty_uint64;

extern Type *ty_float32;
extern Type *ty_float64;

struct Member {
    Member* next;
    Type* type;
    Token* tok; // for error message
    Token* name;
    uint64_t index;
    uint64_t align;
    uint64_t offset;

    bool is_bitfield;
    uint64_t bit_offset;
    uint64_t bit_width; // ???
};

bool is_any_int(Type* ty);
bool is_int(Type* ty);
bool is_uint(Type* ty);
bool is_float(Type* ty);
bool is_numeric(Type* ty);
bool is_string_type(Type* ty);
bool is_compatible(Type* t1, Type* t2);
Type* copy_type(Type *ty);
Type* pointer_to(Type* base);
Type* ref_type(Type* base);
Type* func_type(Type* return_type);
Type* dynarray_type(Type* base);
Type* string_type(void);
Type* enum_def_type(EnumDef* def);
Type* enum_instantiate(ParserState* state, Type* def_type, Type** type_args, int type_arg_count);
Type* struct_type(StructDef* def);
void add_type(Node* node);
void resolve_types(Node* ast);

// parse.c
Type* parse_type_string(const char* str, ParserState* state, const char* filename);
extern ParserState* quarzum_deferred_state;
Type* make_type_param_type(const char* name);
Node* resolve_generic_funcall(ParserState* state, const char* name, NodeList* args);
Node* resolve_generic_funcall_types(ParserState* state, const char* name, Type** arg_types, int arg_count);
Node* instantiate_generic_func_for_types(ParserState* state, const char* name, Type** arg_types, int arg_count);
void append_generic_funcs_to_ast(Node* ast);

// Global enum definitions table
extern HashMap enum_definitions;
void enum_table_init(void);
EnumDef* enum_table_lookup(const char* name);
void enum_table_add(EnumDef* def);

// Global struct definitions table
void struct_table_init(void);
StructDef* struct_table_lookup(const char* name);
void struct_table_add(StructDef* def);

// Global class definitions table
void class_table_init(void);
ClassDef* class_table_lookup(const char* name);
void class_table_add(ClassDef* def);
ClassDef* class_template_instantiate(ParserState* state, ClassDef* template_def, Type** type_args, int type_arg_count);
char* make_mangled_name(const char* class_name, Type** type_args, int type_arg_count);

// Global trait definitions table
void trait_table_init(void);
TraitDef* trait_table_lookup(const char* name);
void trait_table_add(TraitDef* def);

// Function overload table
void function_table_init(void);
Node* function_table_lookup(const char* mangled_name);
void function_table_add(const char* mangled_name, Node* funcdef);

// Type helpers
const char* type_kind_to_name(Type* type);
char* make_func_mangled_name(const char* name, NodeList* params);

// string.c
bool starts_with(const char* str, const char* lexeme);
bool ends_with(char* str, const char* lexeme);

// tokenize.c
uint64_t unescaped_string_length(const char* s);

// main.c
#define VERSION "1.0.0"
#ifndef LIB_PATH
#define LIB_PATH "./lib"
#endif

typedef enum {
    CM_TOK,
    CM_PARSE,
    CM_CHECK,
    CM_CODEGEN,
    CM_BUILD,
    CM_RUN
} CompilationMode;

typedef struct {
    CompilationMode mode;
    bool print_results;
} CompilerOptions;


int parse_arg(CompilerOptions* options, const char* arg);
void print_usage();
bool file_exists(const char* filename);
