#include "quarzum.h"

bool starts_with(const char* str, const char* lexeme){
    return strncmp(str, lexeme, strlen(lexeme)) == 0;
}

bool ends_with(char* str, const char* lexeme){
    uint64_t str_len = strlen(str);
    uint64_t lexeme_len = strlen(lexeme);

    if(lexeme_len > str_len) return false;
    // unsafe pointer arithmetic operations
    char* start = str + str_len - lexeme_len; 
    return strncmp(start, lexeme, lexeme_len) == 0;
}