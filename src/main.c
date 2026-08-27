#include "quarzum.h"

bool file_exists(const char* filename){
    struct stat st;
    return !stat(filename, &st);
}

void print_usage(){
    printf("Usage: quarzum [file] [--options]\n");
}

int parse_arg(CompilerOptions* options, const char* arg){
    if(strncmp(arg,"--tokenize", 10) == 0){
        options->mode = CM_TOK;
        return 0;
    }
    if(strncmp(arg,"--parse", 7) == 0){
        options->mode = CM_PARSE;
        return 0;
    }
    if(strncmp(arg,"--check", 7) == 0){
        options->mode = CM_CHECK;
        return 0;
    }
    if(strncmp(arg,"--codegen", 9) == 0){
        options->mode = CM_CODEGEN;
        return 0;
    }
    if(strncmp(arg,"--build", 7) == 0){
        options->mode = CM_BUILD;
        return 0;
    }
    if(strncmp(arg,"--run", 5) == 0){
        options->mode = CM_RUN;
        return 0;
    }
    if(strncmp(arg,"--print", 7) == 0){
        options->print_results = true;
        return 0;
    }
   
    fprintf(stderr, "Unknown option '%s'", arg);
    return -1;
}

int main(int argc, char** argv){
    if(argc < 2){
        print_usage();
        return 1;
    }

    if(argc == 2 && strncmp(argv[1], "--version", 9) == 0){
        printf("Quarzum v%s\n", VERSION);
        return 0;
    }

    char* filename = argv[1];
    
    if(!file_exists(filename)){
        fprintf(stderr, "File '%s' does not exist\n", filename);
        return 1;
    }

    if(!ends_with(filename, ".qz")){
        fprintf(stderr, "File '%s' has not a .qz extension\n", filename);
        return 1;
    }

    CompilerOptions options;
    options.mode = CM_RUN;
    options.print_results = false;

    for(uint64_t i = 2; i < argc; i++){
        if(parse_arg(&options, argv[i]) == -1)
            return 1;   
    }

    TokenList* tokens = tokenize(filename);
    if(options.print_results){
        print_tokens(tokens);
    }
    if(options.mode == CM_TOK) return 0;

    Node* ast = parse(tokens);
    if(options.mode == CM_PARSE) return 0;

    resolve_types(ast);
    if(options.mode == CM_CHECK) return 0;

    // check
    FILE* output_file = fopen("out.asm","w");
    codegen(ast, output_file);
    fclose(output_file);

    system("as -o out.o out.asm && ld -o out out.o");

    // asm && ld
    if(options.mode == CM_BUILD) return 0;
    // run
    system("./out");

    free(tokens->tokens);
    free(tokens);

}