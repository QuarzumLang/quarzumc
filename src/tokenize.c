#include "quarzum.h"

void print_tokens(const TokenList* tokens){
    printf("Tokens:\n--------\n");
    for(uint64_t i = 0; i < tokens->length; i++){
        Token token = tokens->tokens[i];
        printf("Token(%d) at %s(line %ld, column %ld)\n", token.kind, token.file, token.line, token.column);
    }
}

Token get_token(TokenList* list, uint64_t index){
    assert(index < list->length);
    return list->tokens[index];
}

void add_token(TokenList* list, Token tk){
    if(list->length == list->size){
        Token* new_buffer = realloc(list->tokens, list->size * 2 * sizeof(Token));
        if(new_buffer == NULL){
            perror("realloc");
            exit(1);
        }
        list->tokens = new_buffer;
        list->size *= 2;        
    }

    list->tokens[list->length] = tk;
    list->length++;
}

TokenList* init_tokenlist(uint64_t size){
    TokenList* tl = malloc(sizeof(TokenList));
    if(tl == NULL){
        perror("malloc");
        exit(1);
    }
    tl->tokens = malloc(sizeof(Token) * size);
    if(tl->tokens == NULL){
        perror("malloc");
        free(tl);
        exit(1);
    }
    tl->size = size;
    tl->length = 0;
    return tl;
}

TokenList* tokenize(const char* filename){

    FILE* f = strncmp(filename, "-", 1) == 0?
                stdin :
                fopen(filename, "r");
    if(!f){
        perror("fopen");
        exit(1);
    }

    File* file = malloc(sizeof(File));
    if(file == NULL){
        perror("malloc");
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);


    file->name = malloc(sizeof(char) * (strlen(filename) + 1));
    strcpy(file->name, filename);
    
    file->content = malloc(fsize + 1);
    if(file->content == NULL){
        perror("malloc");
        exit(1);
    }
    file->size = fsize;


    fread(file->content, fsize, 1, f);
    fclose(f);
    file->content[fsize] = '\0';


    TokenList* result = tokenize_file(file);

    // FREE THE FILE?
    free(file);
    return result;
}

static bool is_keyword(char* lexeme) {
    static HashMap map;

    // keyword map
    if (map.capacity == 0) {
        static char* kw[] = {
            "exit", "return", "function", "var", "const",
            "char", "bool", "string", "void",
            "int8", "int16", "int32", "int64", "int",
            "uint8", "uint16", "uint32", "uint64", "uint",
            "float32", "float64", "float",
            "enum", "struct",
            "if", "else", "for", "foreach", "in", "while", "do",
            "switch", "match", "import", "case", "default", "break", "continue",
            "true", "false", "or", "and", "xor", "not", "sizeof", "as",
            "alloc", "free", "pass",
            "class", "trait", "implements", "this", "new", "null",
            "public", "private", "protected", "never",
            "pure", "mutable", "unsafe"
        };

        for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
        hashmap_put(&map, kw[i], (void *)1);
    }

    return hashmap_get(&map, lexeme);
}

static Token create_token(char* filename, uint64_t line, uint64_t column, TokenType type){
    Token t;
    t.file = filename; // strcpy?
    t.line = line;
    t.column = column;
    t.kind = type;
    t.value = NULL;
    t.string_value = NULL;
    t.auto_semi = false;
    return t;
}

static int from_hex(char c) {
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

static int get_escape_character(char** pp){
    char* p = *pp;
    if ('0' <= *p && *p <= '7') {
        int c = *p++ - '0';
        if ('0' <= *p && *p <= '7') {
            c = (c << 3) + (*p++ - '0');
            if ('0' <= *p && *p <= '7')
                c = (c << 3) + (*p++ - '0');
        }
        *pp = p;
        return c;
    }
    if (*p == 'x') {
        p++;
        if (!isxdigit(*p))
            fprintf(stderr, "Invalid hex escape sequence\n");
        int c = 0;
        for (; isxdigit(*p); p++)
            c = (c << 4) + from_hex(*p);
        *pp = p;
        return c;
    }
    int c;
    switch(*p){
        case 'a': c = '\a'; break;
        case 'b': c = '\b'; break;
        case 't': c = '\t'; break;
        case 'n': c = '\n'; break;
        case 'v': c = '\v'; break;
        case 'f': c = '\f'; break;
        case 'r': c = '\r'; break;
        case 'e': c = 27; break;
        default:  c = *p; break;
    }
    *pp = p + 1;
    return c;
}

/*
    Automatic statement-terminator insertion.

    To let programs omit semicolons, the lexer inserts a synthetic ';' token
    before a newline whenever:

      * the previous token (at bracket depth zero) could end a statement
        (an identifier, a literal, or one of the flow keywords), and
      * the token that follows the newline is not a closing bracket
        (')', ']', '}') -- so multi-line calls/blocks still work.

    This mirrors the common "a newline terminates a statement" rule while
    keeping the rest of the parser unchanged: it still accepts an optional
    ';'. Existing code that keeps its semicolons continues to compile.
*/

static Token g_prev_token;
static bool g_prev_valid = false;
static bool g_saw_newline = false;
static int g_depth = 0;

static bool semi_prev_qualifies(Token t){
    if(t.kind == TT_IDENT) return true;
    if(t.kind == TT_NUM) return true;
    if(t.kind == TT_STR) return true;
    if(t.kind == TT_PUNCT){
        // A closing bracket ends an expression, so a following newline is a
        // statement boundary. skip_semicolons (called at every statement start)
        // consumes the synthetic ';' so 'if(cond) stmt' / 'while(cond) stmt'
        // still parse correctly.
        if(strcmp(t.value, ")") == 0 ||
           strcmp(t.value, "]") == 0 ||
           strcmp(t.value, "}") == 0)
            return true;
    }
    if(t.kind == TT_KEYWORD){
        // Statement-terminating keywords (already ended the expression).
        static const char* term_kw[] = {
            "return", "exit", "break", "continue", "pass",
            "true", "false", "null"
        };
        for(int i = 0; i < 8; i++)
            if(t.value && strcmp(t.value, term_kw[i]) == 0) return true;
        // Type keywords / 'this' can appear as the final token of an expression
        // (e.g. 'x = foo as int64' or 'return this'), so a following newline is
        // a statement boundary.
        static const char* type_kw[] = {
            "char", "bool", "string", "void",
            "int8", "int16", "int32", "int64", "int",
            "uint8", "uint16", "uint32", "uint64", "uint",
            "float32", "float64", "float", "this"
        };
        for(int i = 0; i < 18; i++)
            if(t.value && strcmp(t.value, type_kw[i]) == 0) return true;
    }
    return false;
}

static bool semi_omit_before(Token t){
    if(t.kind != TT_PUNCT) return false;
    return strcmp(t.value, ")") == 0 ||
           strcmp(t.value, "]") == 0 ||
           strcmp(t.value, "}") == 0 ||
           strcmp(t.value, "<") == 0 ||
           strcmp(t.value, ">") == 0;
}

static void emit_token(TokenList* tokens, Token t){
    if(g_saw_newline && g_prev_valid && semi_prev_qualifies(g_prev_token) && !semi_omit_before(t)){
        Token semi = create_token(t.file, t.line, t.column, TT_PUNCT);
        semi.value = ";";
        semi.auto_semi = true;
        add_token(tokens, semi);
    }
    g_saw_newline = false;
    add_token(tokens, t);
    g_prev_token = t;
    g_prev_valid = true;
}

uint64_t unescaped_string_length(const char* s){
    uint64_t len = 0;
    while(*s){
        if(*s == '\\'){
            s++;
            if('0' <= *s && *s <= '7'){
                for(int i = 0; i < 3 && '0' <= *s && *s <= '7'; i++) s++;
            } else if(*s == 'x'){
                s++;
                while(isxdigit((unsigned char)*s)) s++;
            } else {
                s++;
            }
            len++;
            continue;
        }
        s++;
        len++;
    }
    return len;
}

TokenList* tokenize_file(File* f){
    TokenList* tokens = init_tokenlist(64);
    uint64_t line = 1;
    uint64_t column = 1;
    char buffer[256];
    char* p = f->content;

    g_prev_valid = false;
    g_saw_newline = false;
    g_depth = 0;
    g_prev_token = (Token){0};

    while(*p){
        while(isspace(*p)){
            if(*p == '\n'){
                line++;
                column = 1;
                if(g_depth == 0) g_saw_newline = true;
            }
            p++;
        }
        if(!*p) break;
        if(starts_with(p, "//")){
            while(*p && *p != '\n') p++;
            continue;
        }
        if(starts_with(p, "/*")){
            p += 2;
            uint64_t start_line = line;
            while(*p && !starts_with(p, "*/")){
                column++;
                if(*p == '\n'){
                    line++;
                    column = 1;
                }
                p++;
            } 
            if(!*p){
                fprintf(stderr, "Error: Unterminated comment at line %ld\n", start_line);
                exit(1);
            }
            p += 2;
            continue;
        }
        if(isalpha(*p) || *p == '_'){
            char* start = p;
            uint64_t len = 0;
            while(isalnum(*p) || *p == '_') {
                p++;
                column++;
                len++;
            }
            if(len > 255){
                fprintf(stderr, "Error: Identifier large than 255 characters\n");
                exit(1);
            }
            strncpy(buffer, start, len);
            buffer[len] = '\0';
            if(is_keyword(buffer)){
                Token keyword = create_token(f->name, line, column, TT_KEYWORD);
                keyword.value = malloc(sizeof(char) * (len + 1));
                strncpy(keyword.value, buffer, len);
                keyword.value[len] = '\0';
                emit_token(tokens, keyword);
            }
            else {
                Token id = create_token(f->name, line, column, TT_IDENT);
                id.value = malloc(sizeof(char) * (len + 1));
                strncpy(id.value, buffer, len);
                id.value[len] = '\0';
                emit_token(tokens, id);
            }
            continue;
        }
        if(isdigit(*p)){
            char* start = p;
            uint64_t len = 0;

            if(*p == '0' && (p[1] == 'x' || p[1] == 'X')){
                p += 2;
                column += 2;
                while(isxdigit(*p)){
                    len++;
                    p++;
                    column++;
                }
                uint64_t total = len + 2;
                strncpy(buffer, start, total);
                buffer[total] = '\0';

                Token int_lit = create_token(f->name, line, column, TT_NUM);
                int_lit.type = ty_uint32;
                int_lit.int_value = strtoll(buffer, NULL, 16);
                int_lit.value = malloc(sizeof(char) * (total + 1));
                strncpy(int_lit.value, buffer, total + 1);

                emit_token(tokens, int_lit);
                continue;
            }

            while(isdigit(*p)){
                len++;
                p++;
                column++;
            }
            if(*p == '.'){
                p++;
                len++;
                column++;
                while(isdigit(*p)){
                    len++;
                    p++;
                    column++;
                }
                strncpy(buffer, p-len, len);
                buffer[len] = '\0';

                Token float_lit = create_token(f->name, line, column, TT_NUM);
                float_lit.type = ty_float32;
                float_lit.float_value = atof(buffer);
                float_lit.value = malloc(sizeof(char) * (len + 1));
                strncpy(float_lit.value, buffer, len + 1);

                emit_token(tokens, float_lit);
                continue;
            }
            else {
                strncpy(buffer, start, len);
                buffer[len] = '\0';

                Token int_lit = create_token(f->name, line, column, TT_NUM);
                int_lit.type = ty_uint32;
                int_lit.int_value = strtoll(buffer, NULL, 10);
                int_lit.value = malloc(sizeof(char) * (len + 1));
                strncpy(int_lit.value, buffer, len + 1);

                emit_token(tokens, int_lit);
                continue;
            }
            p++;
            continue;
        }
        if(*p == '\''){
            p++;
            int was_escape = 0;
            int char_value = 0;
            if(*p == '\\'){
                p++;
                was_escape = 1;
                char_value = get_escape_character(&p);
            } else {
                char_value = (int)(*p);
                p++;
            }
            column++;
            if(*p != '\''){
                fprintf(stderr, "Error: char literal too long\n");
                exit(1);
            }
            Token t = create_token(f->name, line, column, TT_NUM);
            t.type = ty_char;
            t.int_value = char_value;
            t.value = "";

            emit_token(tokens, t);
            p++;
            continue;
        }
        if(*p == '"'){
            p++;
            char* start = p;
            uint64_t len = 0;
            while(*p && *p != '"'){
                if(*p == '\\'){
                    p++;
                    len++;
                }
                column++;
                len++;
                if(*p == '\n'){
                    line++;
                    column = 1;
                }
                p++;
            }
            if(!*p){
                fprintf(stderr, "Unterminated string literal\n");
                exit(1);
            }
            p++;

            strncpy(buffer, start, len);
            buffer[len] = '\0';
            Token str_lit = create_token(f->name, line, column, TT_STR);
            str_lit.string_value = malloc(sizeof(char) * (len + 1));
            str_lit.type = string_type();
            if(str_lit.string_value == NULL){
                perror("malloc");
                exit(1);
            }
            strncpy(str_lit.string_value, buffer, len + 1);
            str_lit.value = str_lit.string_value;
            emit_token(tokens, str_lit);
            continue;
        }

        static char *symbols[] = {
            ";", "(", ")", "[", "]", "{", "}", ":", ",", ".", "?",
            "=>", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=",
            "&&=", "||=", "^^=", "!!=", "&&", "||", "^^", "!!",
            "=", "<", ">", "+", "-", "*", "/", "%", "&"
        };
        char *start = p;
        uint64_t index = -1;
        for (int i = 0; i < sizeof(symbols) / sizeof(*symbols); i++){
            if (starts_with(p, symbols[i])) {
                index = i;
                break;
            }
        }
        if(index != -1){
            p += strlen(symbols[index]);
            
            Token sym = create_token(f->name, line, column, TT_PUNCT);
            sym.value = symbols[index];
            if(strcmp(symbols[index], "(") == 0 || strcmp(symbols[index], "[") == 0) g_depth++;
            else if(strcmp(symbols[index], ")") == 0 || strcmp(symbols[index], "]") == 0) g_depth--;
            emit_token(tokens, sym);
            continue;
        }

        fprintf(stderr, "Error: Unexpected %c at line %ld column %ld\n", *start, line, column);
        exit(1);

    }
    emit_token(tokens, create_token(f->name, line, column, TT_EOF));


    return tokens;
}