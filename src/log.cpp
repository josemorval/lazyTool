#include "log.h"
#include <stdarg.h>

// log.cpp stores a fixed-size ring buffer for editor-visible messages. It avoids
// allocations during render/compile paths, and the UI consumes the ring directly.

#ifndef LAZYTOOL_NO_LOG
// The log is a fixed-size ring buffer so the UI can display recent messages
// without allocating during normal frame updates.

AppLog g_log = {};

void log_init() { memset(&g_log, 0, sizeof(g_log)); }

void log_push(LogLevel lvl, const char* fmt, ...) {
    LogEntry& e = g_log.entries[g_log.head % LOG_MAX_ENTRIES];
    e.level = lvl;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    snprintf(e.time, sizeof(e.time), "%02u:%02u:%02u",
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, LOG_MSG_LEN, fmt, ap);
    va_end(ap);
    g_log.head = (g_log.head + 1) % LOG_MAX_ENTRIES;
    if (g_log.count < LOG_MAX_ENTRIES) g_log.count++;
    g_log.scroll_to_bottom = true;
    char dbg[LOG_MSG_LEN + 32];
    snprintf(dbg, sizeof(dbg), "[%s] %s", e.time, e.msg);
    OutputDebugStringA(dbg);
    OutputDebugStringA("\n");
}
#endif
