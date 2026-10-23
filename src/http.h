#ifndef HTTP_H
#define HTTP_H

#include "server.h"

struct viewer *http_new(void);
int http_input(struct viewer *viewer, uint64_t now);
void http_retry(struct viewer *viewer, int expired);
int http_flush(struct viewer *viewer, uint64_t now);
int http_idle(struct viewer *viewer, uint64_t now);

#endif
