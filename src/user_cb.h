#pragma once
#include "types.h"

// Public API for the user-defined constant buffer. Preferred convention:
// SceneCB = b0, ObjectCB = b1, UserCB = b2.

extern UserCBEntry   g_user_cb_entries[MAX_USER_CB_VARS];
extern int           g_user_cb_count;
extern ID3D11Buffer* g_user_cb_buf;

void user_cb_init();
void user_cb_shutdown();
void user_cb_clear();
void user_cb_update();  // call each frame before render
void user_cb_bind();    // compatibility hook; command binding is per-shader
void user_cb_sync_command_params(Command* c, const Resource* shader);
void user_cb_bind_for_command(Command* c, const Resource* shader, bool bind_vs, bool bind_ps, bool bind_cs);
void user_cb_enforce_unique_names();

bool user_cb_type_supported(ResType type);
bool user_cb_add_var(const char* name, ResType type);
bool user_cb_add_from_resource(ResHandle h);
int  user_cb_find(const char* name);
const UserCBEntry* user_cb_get(const char* name);
bool user_cb_rename(int idx, const char* name);
bool user_cb_set_source(int idx, ResHandle h);
bool user_cb_set_scene_source(int idx, UserCBSourceKind kind, const char* target);
void user_cb_refresh_entry(int idx);
const char* user_cb_source_kind_token(UserCBSourceKind kind);
UserCBSourceKind user_cb_source_kind_from_token(const char* token);
void user_cb_detach_resource(ResHandle h);
void user_cb_rename_command_references(const char* old_name, const char* new_name);
void user_cb_rename_variable_references(const char* old_name, const char* new_name);
void user_cb_delete_variable_references(const char* name);
void user_cb_rename_resource_references(ResHandle h, const char* old_name, const char* new_name);
void user_cb_remove(int idx);
void user_cb_move(int from, int to);
int  user_cb_slot_offset(int idx); // always idx * 16
