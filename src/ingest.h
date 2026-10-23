#ifndef INGEST_H
#define INGEST_H

#include "config.h"

int setup_streams(struct plan *plan);
void reload_streams(struct plan *plan);
int open_srt(void);
void *ingest(void *argument);
void close_srt(void);

#endif
