#pragma once
#include "types.h"

// Commands are the editable execution units that build a frame: clears,
// draws, compute dispatches, grouping, repetition, and indirect work.

extern Command g_commands[MAX_COMMANDS];
extern int     g_command_count;
extern bool    g_profiler_enabled;

void      cmd_init();
void      cmd_shutdown();
CmdHandle cmd_alloc(const char* name, CmdType type);
void      cmd_free(CmdHandle h);
Command*  cmd_get(CmdHandle h);
CmdHandle cmd_find_by_name(const char* name);
bool      cmd_rename(CmdHandle h, const char* name);
CmdHandle cmd_move(CmdHandle moving, CmdHandle target, bool after_target);

void      cmd_execute_all();
void      cmd_set_reset_execution(bool active);
void      cmd_request_shader_recompute(ResHandle shader_h);
bool      cmd_refresh_draw_bounds(CmdHandle h);
bool      cmd_compute_world_bounds(CmdHandle h, float out_min[3], float out_max[3]);
void      cmd_mark_dirty(CmdHandle h);
void      cmd_mark_all_dirty();
uint64_t  cmd_revision();
uint64_t  cmd_graph_revision();

const char* cmd_type_str(CmdType t);
void        cmd_make_unique_name(const char* base, char* out, int out_sz);
float       cmd_profile_ms(CmdHandle h);
float       cmd_profile_frame_ms();
bool        cmd_profile_ready();
void        cmd_profile_begin_frame_capture();
void        cmd_profile_end_frame_capture();
float       cmd_profile_total_frame_ms();
bool        cmd_profile_total_ready();
