#pragma once
#include "types.h"

// Small logging API used by runtime code and displayed inside the editor UI.

#define LOG_MAX_ENTRIES 512
#define LOG_MSG_LEN     256

enum LogLevel { LOG_INFO = 0, LOG_WARN, LOG_ERROR };

struct LogEntry {
    LogLevel level;
    char     time[16];
    char     msg[LOG_MSG_LEN];
};

struct AppLog {
    LogEntry entries[LOG_MAX_ENTRIES];
    int      head;
    int      count;
    bool     scroll_to_bottom;
};

extern AppLog g_log;

#ifdef LAZYTOOL_NO_LOG
inline void log_init() {}
#define log_info(...)  ((void)0)
#define log_warn(...)  ((void)0)
#define log_error(...) ((void)0)
#else
void log_init();
void log_push(LogLevel lvl, const char* fmt, ...);

#define log_info(...)  log_push(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_push(LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_push(LOG_ERROR, __VA_ARGS__)
#endif
