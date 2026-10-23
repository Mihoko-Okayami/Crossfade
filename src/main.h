#ifndef MAIN_H
#define MAIN_H

#include <stdatomic.h>
#include <stdint.h>

#ifndef VERSION
#define VERSION "dev"
#endif

extern atomic_int running;

uint64_t msec(void);

#endif
