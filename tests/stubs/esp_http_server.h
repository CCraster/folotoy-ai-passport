#pragma once
#include <stddef.h>
typedef struct { size_t content_len; } httpd_req_t;
int httpd_req_recv(httpd_req_t *req, char *out, size_t size);
