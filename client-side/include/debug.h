#ifndef DEBUG_H
#define DEBUG_H
#include <stdarg.h>

// DEBUG FILE

void open_debug_file(const char *filename);

void close_debug_file(void);

void debug(const char * format, ...);

void sleep_ms(int milliseconds);

#endif