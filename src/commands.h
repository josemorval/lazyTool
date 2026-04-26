#pragma once
#include "types.h"

extern Command g_commands[MAX_COMMANDS];
extern int     g_command_count;
extern bool    g_profiler_enabled;

void      cmd_init();
void      cmd_shutdown();
CmdHandle cmd_alloc(const char* name, CmdType type);
void      cmd_free(CmdHandle h);
Command*  cmd_get(CmdHandle h);
CmdHandle cmd_find_by_name(const char* name);
CmdHandle cmd_move(CmdHandle moving, CmdHandle target, bool after_target);

void      cmd_execute_all();

const char* cmd_type_str(CmdType t);
void        cmd_make_unique_name(const char* base, char* out, int out_sz);
float       cmd_profile_ms(CmdHandle h);
float       cmd_profile_frame_ms();
bool        cmd_profile_ready();
void        cmd_profile_begin_frame_capture();
void        cmd_profile_end_frame_capture();
float       cmd_profile_total_frame_ms();
bool        cmd_profile_total_ready();
