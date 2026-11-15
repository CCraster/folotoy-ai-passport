#pragma once
#include <stdint.h>

typedef struct {
    char value[7];
    unsigned failures;
    int64_t blocked_until;
} web_code_t;
enum { WEB_CODE_OK, WEB_CODE_WRONG, WEB_CODE_BLOCKED };
int web_code_check(web_code_t *code, const char *supplied, int64_t now);
