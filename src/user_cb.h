#pragma once
#include "types.h"

// Public API for the user-defined constant buffer. Preferred convention:
// SceneCB = b0, ObjectCB = b1, UserCB = b2.

extern UserCBEntry   g_user_cb_entries[MAX_USER_CB_VARS];
extern int           g_user_cb_count;
extern ID3D11Buffer* g_user_cb_buf;

void user_cb_init();
void user_cb_shutdown();
void user_cb_update();  // call each frame before render
void user_cb_bind();    // compatibility hook; command binding is per-shader
void user_cb_sync_command_params(Command* c, const Resource* shader);
void user_cb_bind_for_command(Command* c, const Resource* shader, bool bind_vs, bool bind_ps, bool bind_cs);

bool user_cb_type_supported(ResType type);
bool user_cb_add_var(const char* name, ResType type);
bool user_cb_add_from_resource(ResHandle h);
bool user_cb_set_source(int idx, ResHandle h);
void user_cb_detach_resource(ResHandle h);
void user_cb_remove(int idx);
void user_cb_move(int from, int to);
int  user_cb_slot_offset(int idx); // always idx * 16
