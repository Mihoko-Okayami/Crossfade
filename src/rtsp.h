#ifndef RTSP_H
#define RTSP_H

#include "server.h"

struct viewer *rtsp_new(void);
int rtsp_input(struct viewer *viewer, uint64_t now);
void rtsp_retry(struct viewer *viewer, int expired);
int rtsp_flush(struct viewer *viewer, uint64_t now);
int rtsp_idle(struct viewer *viewer, uint64_t now);

#endif
