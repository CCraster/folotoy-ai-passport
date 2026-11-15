#include "web_code.h"
#include <string.h>

int web_code_check(web_code_t *code, const char *supplied, int64_t now)
{
    if (now < code->blocked_until) return WEB_CODE_BLOCKED;
    if (strlen(supplied) == 6 && strlen(code->value) == 6 && !strcmp(supplied, code->value)) {
        code->failures = 0;
        return WEB_CODE_OK;
    }
    // 短码仅在机身配置窗口有效，连续五次失败后暂停一分钟。
    if (++code->failures >= 5) {
        code->blocked_until = now + 60000000;
        code->failures = 0;
    }
    return WEB_CODE_WRONG;
}
