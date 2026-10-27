#ifndef LOG_H
#define LOG_H

enum topic { ON_SERVER, ON_CONFIG, ON_STREAM, ON_VIEWER, WARN = 4, FAIL = 8 };

enum event { UNACCEPTED, FULL, STARVED, OVERSIZED, OVERFLOWED, CHANGED, UNMATCHED, BARRED, STALLED, EVENTS };

void paint(void);
void headline(void);
__attribute__((format(printf, 2, 3)))
void note(int topic, const char *format, ...);
__attribute__((format(printf, 2, 3)))
void tell(enum event event, const char *format, ...);
void settle(void);
void panel(void);

#endif
