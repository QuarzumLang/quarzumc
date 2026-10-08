#include "quarzum.h"
#include <sys/wait.h>

bool file_exists(const char* filename){
    struct stat st;
    return !stat(filename, &st);
}

// Best-effort location of the x86-64 dynamic loader, used when linking against
// libc. GNU ld's built-in default (/lib/ld64.so.1) is wrong on many distros.
static const char* dynamic_linker_path(void){
    static const char* candidates[] = {
        "/lib64/ld-linux-x86-64.so.2",
        "/lib/ld-linux-x86-64.so.2",
        "/usr/lib64/ld-linux-x86-64.so.2",
        "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
        NULL
    };
    struct stat st;
    for(int i = 0; candidates[i]; i++){
        if(stat(candidates[i], &st) == 0) return candidates[i];
    }
    return candidates[0];
}

void print_usage(){
    printf("Usage:\n");
    printf("  quarzum <file.qz> [--options]   Compile/run a single file\n");
    printf("  quarzum build|run [--options]   Build/run the project described by manifest.toml\n");
    printf("\nOptions: --tokenize --parse --check --codegen --build --run --print\n");
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

    if(strncmp(argv[1], "--version", 9) == 0){
        printf("Quarzum v%s\n", VERSION);
        return 0;
    }

    CompilerOptions options;
    options.mode = CM_RUN;
    options.print_results = false;

    char* filename = NULL;
    int arg_start = 2;

    // Native linking options coming from the manifest (project mode).
    bool use_libc = false;
    char** link_libs = NULL;
    int link_lib_count = 0;

    if(strcmp(argv[1], "build") == 0 || strcmp(argv[1], "run") == 0){
        // Project mode: read manifest.toml and compile its entry point.
        options.mode = (strcmp(argv[1], "build") == 0) ? CM_BUILD : CM_RUN;
        Manifest* manifest = manifest_load("manifest.toml");
        if(!manifest) return 1;
        set_project_root(".");

        int dep_count = manifest->dependency_count + manifest->dev_dependency_count;
        char** dep_names = malloc(sizeof(char*) * (dep_count ? dep_count : 1));
        int k = 0;
        for(int i = 0; i < manifest->dependency_count; i++)
            dep_names[k++] = strdup(manifest->dependencies[i].name);
        for(int i = 0; i < manifest->dev_dependency_count; i++)
            dep_names[k++] = strdup(manifest->dev_dependencies[i].name);
        set_project_dependencies(dep_names, dep_count);

        use_libc = manifest->libc;
        link_lib_count = manifest->link_count;
        if(link_lib_count > 0){
            link_libs = malloc(sizeof(char*) * link_lib_count);
            for(int i = 0; i < link_lib_count; i++)
                link_libs[i] = strdup(manifest->links[i]);
        }

        filename = strdup(manifest->entry);
        manifest_free(manifest);
    } else {
        filename = argv[1];
        if(!file_exists(filename)){
            fprintf(stderr, "File '%s' does not exist\n", filename);
            return 1;
        }
        if(!ends_with(filename, ".qz")){
            fprintf(stderr, "File '%s' has not a .qz extension\n", filename);
            return 1;
        }
    }

    for(uint64_t i = arg_start; i < argc; i++){
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
    codegen_set_use_libc(use_libc);
    FILE* output_file = fopen("out.asm","w");
    if(!output_file){
        fprintf(stderr, "error: cannot write 'out.asm'\n");
        return 1;
    }
    codegen(ast, output_file);
    fclose(output_file);

    char ld_cmd[8192];
    int off = snprintf(ld_cmd, sizeof(ld_cmd), "as -o out.o out.asm && ld -o out out.o");
    if(program_uses_c_externs() && !use_libc && link_lib_count == 0){
        fprintf(stderr, "hint: this program declares extern C functions; enable `[build] libc = true` "
                        "(and any `[build] link` libraries) in manifest.toml so they can be linked.\n");
    }
    if(use_libc){
        off += snprintf(ld_cmd + off, sizeof(ld_cmd) - off,
                        " -lc --dynamic-linker %s", dynamic_linker_path());
    }
    for(int i = 0; i < link_lib_count; i++){
        off += snprintf(ld_cmd + off, sizeof(ld_cmd) - off, " -l%s", link_libs[i]);
    }

    int status = system(ld_cmd);
    if(status != 0){
        fprintf(stderr, "error: assembling/linking failed (are `as` and `ld` installed?)\n");
        return 1;
    }

    // asm && ld
    if(options.mode == CM_BUILD) return 0;

    // run
    status = system("./out");
    free(tokens->tokens);
    free(tokens);
    if(status == -1) return 1;
    if(WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}