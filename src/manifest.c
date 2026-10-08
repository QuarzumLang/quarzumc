/*
    manifest.c
    Minimal reader for the project manifest (`manifest.toml`).

    Supported TOML subset:
      - `#` line comments (outside double-quoted strings)
      - `key = "value"` entries
      - `[section]` tables
    Sections understood: top level, `[dependencies]`, `[dev-dependencies]`,
    `[build]`.
*/
#include "quarzum.h"

static char* mf_trim(char* s){
    while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char* end = s + strlen(s);
    while(end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    return s;
}

// Cuts a trailing comment: a '#' that is not inside a double-quoted string.
static void mf_strip_comment(char* line){
    int in_string = 0;
    for(char* p = line; *p; p++){
        if(*p == '"'){
            in_string = !in_string;
        } else if(*p == '#' && !in_string){
            *p = '\0';
            return;
        }
    }
}

// Trims and removes one layer of surrounding double quotes.
static char* mf_unquote(char* value){
    value = mf_trim(value);
    size_t n = strlen(value);
    if(n >= 2 && value[0] == '"' && value[n - 1] == '"'){
        value[n - 1] = '\0';
        return value + 1;
    }
    return value;
}

static void mf_add_dependency(ManifestDependency** list, int* count, const char* name, const char* version){
    *count += 1;
    *list = realloc(*list, sizeof(ManifestDependency) * (*count));
    (*list)[*count - 1].name = strdup(name);
    (*list)[*count - 1].version = strdup(version);
}

// Parses a TOML array of strings (`["m", "pthread"]`) into `*out`.
static void mf_parse_string_array(const char* value, char*** out, int* count){
    const char* p = value;
    while(*p && *p != '[') p++;
    if(*p == '[') p++;
    while(*p){
        while(*p == ' ' || *p == '\t' || *p == ',') p++;
        if(*p == ']' || *p == '\0') break;

        const char* start;
        if(*p == '"'){
            p++;
            start = p;
            while(*p && *p != '"') p++;
        } else {
            start = p;
            while(*p && *p != ',' && *p != ']' && *p != ' ' && *p != '\t') p++;
        }
        size_t len = (size_t)(p - start);
        char* item = malloc(len + 1);
        memcpy(item, start, len);
        item[len] = '\0';
        *count += 1;
        *out = realloc(*out, sizeof(char*) * (*count));
        (*out)[*count - 1] = item;

        if(*p == '"') p++;
    }
}

Manifest* manifest_load(const char* path){
    FILE* f = fopen(path, "r");
    if(!f){
        fprintf(stderr, "error: cannot open manifest '%s'\n", path);
        return NULL;
    }

    Manifest* m = calloc(1, sizeof(Manifest));
    char section[128] = "";
    char line[1024];

    while(fgets(line, sizeof(line), f)){
        mf_strip_comment(line);
        char* s = mf_trim(line);
        if(*s == '\0') continue;

        if(*s == '['){
            char* end = strchr(s, ']');
            if(!end){
                fprintf(stderr, "manifest error: malformed section header: '%s'\n", s);
                fclose(f);
                manifest_free(m);
                return NULL;
            }
            *end = '\0';
            strncpy(section, mf_trim(s + 1), sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            continue;
        }

        char* eq = strchr(s, '=');
        if(!eq){
            fprintf(stderr, "manifest error: expected 'key = value', got '%s'\n", s);
            fclose(f);
            manifest_free(m);
            return NULL;
        }
        *eq = '\0';
        char* key = mf_trim(s);
        char* value = mf_unquote(eq + 1);

        if(section[0] == '\0'){
            if(strcmp(key, "name") == 0) m->name = strdup(value);
            else if(strcmp(key, "version") == 0) m->version = strdup(value);
            else if(strcmp(key, "description") == 0) m->description = strdup(value);
            else if(strcmp(key, "license") == 0) m->license = strdup(value);
            // Unknown top-level keys are ignored.
        } else if(strcmp(section, "dependencies") == 0){
            mf_add_dependency(&m->dependencies, &m->dependency_count, key, value);
        } else if(strcmp(section, "dev-dependencies") == 0){
            mf_add_dependency(&m->dev_dependencies, &m->dev_dependency_count, key, value);
        } else if(strcmp(section, "build") == 0){
            if(strcmp(key, "entry") == 0) m->entry = strdup(value);
            else if(strcmp(key, "libc") == 0) m->libc = (strcmp(value, "true") == 0);
            else if(strcmp(key, "link") == 0) mf_parse_string_array(eq + 1, &m->links, &m->link_count);
        }
        // Unknown sections are ignored.
    }
    fclose(f);

    if(!m->name){
        fprintf(stderr, "manifest error: missing 'name'\n");
        manifest_free(m);
        return NULL;
    }
    if(!m->version){
        fprintf(stderr, "manifest error: missing 'version'\n");
        manifest_free(m);
        return NULL;
    }
    if(!m->entry){
        fprintf(stderr, "manifest error: missing '[build] entry'\n");
        manifest_free(m);
        return NULL;
    }
    return m;
}

void manifest_free(Manifest* m){
    if(!m) return;
    free(m->name);
    free(m->version);
    free(m->description);
    free(m->license);
    free(m->entry);
    for(int i = 0; i < m->link_count; i++)
        free(m->links[i]);
    free(m->links);
    for(int i = 0; i < m->dependency_count; i++){
        free(m->dependencies[i].name);
        free(m->dependencies[i].version);
    }
    free(m->dependencies);
    for(int i = 0; i < m->dev_dependency_count; i++){
        free(m->dev_dependencies[i].name);
        free(m->dev_dependencies[i].version);
    }
    free(m->dev_dependencies);
    free(m);
}
