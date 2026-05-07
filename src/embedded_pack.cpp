#include "embedded_pack.h"
#include "types.h"
#include "log.h"
#include "cgltf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Normal executable packaging.
//
// The normal player keeps the full runtime so asset-heavy projects can load
// textures, meshes, and shader files. Exporting copies the player executable,
// appends a small pack at the end of the PE file, and writes a footer that lets
// the runtime find that pack when it starts. The project text is minified before
// it is embedded, but external assets are preserved byte-for-byte.

static const unsigned char k_footer_magic[8] = { 'L','T','P','A','C','K','1','!' };
static const unsigned char k_data_magic[8]   = { 'L','T','P','D','A','T','1','!' };
static const unsigned int  k_pack_version    = 1;
static const char*         k_embedded_project = "p";

#pragma pack(push, 1)
struct LtPackFooter {
    unsigned long long pack_offset;
    unsigned long long pack_size;
    unsigned char magic[8];
};

struct LtPackHeader {
    unsigned char magic[8];
    unsigned int version;
    unsigned int file_count;
    unsigned int project_path_len;
};

struct LtPackEntryHeader {
    unsigned int path_len;
    unsigned long long data_size;
};
#pragma pack(pop)

struct LtPackFile {
    char path[MAX_PATH_LEN];
    const unsigned char* data;
    size_t size;
};

static unsigned char* s_pack_bytes = nullptr;
static size_t s_pack_size = 0;
static LtPackFile* s_pack_files = nullptr;
static int s_pack_file_count = 0;
static char s_pack_project_path[MAX_PATH_LEN] = {};

static void set_err(char* err, int err_sz, const char* msg) {
    if (!err || err_sz <= 0)
        return;
    strncpy(err, msg ? msg : "", err_sz - 1);
    err[err_sz - 1] = '\0';
}

static bool read_u64_file_size(FILE* f, unsigned long long* out_size) {
    if (!f || !out_size)
        return false;
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0)
        return false;
    __int64 end = _ftelli64(f);
    if (end < 0)
        return false;
    if (_fseeki64(f, 0, SEEK_SET) != 0)
        return false;
    *out_size = (unsigned long long)end;
#else
    if (fseek(f, 0, SEEK_END) != 0)
        return false;
    long end = ftell(f);
    if (end < 0)
        return false;
    if (fseek(f, 0, SEEK_SET) != 0)
        return false;
    *out_size = (unsigned long long)end;
#endif
    return true;
}

static void normalize_path(const char* in, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!in)
        return;

    while (in[0] == '.' && (in[1] == '/' || in[1] == '\\'))
        in += 2;

    int oi = 0;
    for (int i = 0; in[i] && oi < out_sz - 1; i++) {
        char c = in[i] == '\\' ? '/' : in[i];
        if (c == '/' && oi > 0 && out[oi - 1] == '/')
            continue;
        out[oi++] = c;
    }
    out[oi] = '\0';
}

static bool path_is_absolute(const char* path) {
    if (!path || !path[0])
        return false;
    if (path[0] == '/' || path[0] == '\\')
        return true;
    return path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

static void path_dirname(const char* path, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!path)
        return;
    const char* slash1 = strrchr(path, '/');
    const char* slash2 = strrchr(path, '\\');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    if (!slash)
        return;
    int len = (int)(slash - path);
    if (len >= out_sz)
        len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static void path_join(const char* dir, const char* file, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    if (!file)
        file = "";
    if (path_is_absolute(file) || !dir || !dir[0]) {
        strncpy(out, file, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "%s/%s", dir, file);
}

static bool read_disk_file(const char* path, void** out_data, size_t* out_size) {
    if (!path || !out_data || !out_size)
        return false;
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;

    unsigned long long file_size = 0;
    if (!read_u64_file_size(f, &file_size)) {
        fclose(f);
        return false;
    }
    if (file_size > (unsigned long long)SIZE_MAX - 1) {
        fclose(f);
        return false;
    }

    unsigned char* data = (unsigned char*)malloc((size_t)file_size + 1);
    if (!data) {
        fclose(f);
        return false;
    }
    size_t got = fread(data, 1, (size_t)file_size, f);
    fclose(f);
    if (got != (size_t)file_size) {
        free(data);
        return false;
    }
    data[file_size] = 0;
    *out_data = data;
    *out_size = (size_t)file_size;
    return true;
}

static bool disk_file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

// Normal exports are requested from lazyTool.exe, but the player stub has a
// smaller link surface than the editor. Prefer a sibling lazyPlayer.exe when it
// exists and fall back to the requested executable for development builds.
static void choose_normal_export_base_exe(const char* requested, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    strncpy(out, requested ? requested : "", out_sz - 1);
    out[out_sz - 1] = '\0';

    char dir[MAX_PATH_LEN] = {};
    path_dirname(requested, dir, MAX_PATH_LEN);
    char candidate[MAX_PATH_LEN] = {};
    path_join(dir, "lazyPlayer.exe", candidate, MAX_PATH_LEN);
    if (disk_file_exists(candidate)) {
        strncpy(out, candidate, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

static LtPackFile* find_pack_file(const char* path) {
    char norm[MAX_PATH_LEN] = {};
    normalize_path(path, norm, MAX_PATH_LEN);
    for (int i = 0; i < s_pack_file_count; i++) {
        if (_stricmp(s_pack_files[i].path, norm) == 0)
            return &s_pack_files[i];
    }
    return nullptr;
}

bool lt_read_file(const char* path, void** out_data, size_t* out_size) {
    if (!out_data || !out_size)
        return false;
    *out_data = nullptr;
    *out_size = 0;

    if (LtPackFile* packed = find_pack_file(path)) {
        unsigned char* copy = (unsigned char*)malloc(packed->size + 1);
        if (!copy)
            return false;
        memcpy(copy, packed->data, packed->size);
        copy[packed->size] = 0;
        *out_data = copy;
        *out_size = packed->size;
        return true;
    }

    return read_disk_file(path, out_data, out_size);
}

void lt_free_file(void* data) {
    free(data);
}

static bool find_pack_footer(FILE* f, unsigned long long file_size, LtPackFooter* out_footer) {
    if (!f || !out_footer || file_size < sizeof(LtPackFooter))
        return false;
#if defined(_WIN32)
    if (_fseeki64(f, -(__int64)sizeof(LtPackFooter), SEEK_END) != 0)
        return false;
#else
    if (fseek(f, -(long)sizeof(LtPackFooter), SEEK_END) != 0)
        return false;
#endif
    LtPackFooter footer = {};
    if (fread(&footer, 1, sizeof(footer), f) != sizeof(footer))
        return false;
    if (memcmp(footer.magic, k_footer_magic, sizeof(k_footer_magic)) != 0)
        return false;
    if (footer.pack_offset >= file_size || footer.pack_size == 0)
        return false;
    if (footer.pack_offset + footer.pack_size + sizeof(LtPackFooter) != file_size)
        return false;
    *out_footer = footer;
    return true;
}

static void clear_loaded_pack() {
    free(s_pack_files);
    s_pack_files = nullptr;
    s_pack_file_count = 0;
    free(s_pack_bytes);
    s_pack_bytes = nullptr;
    s_pack_size = 0;
    s_pack_project_path[0] = '\0';
}

bool lt_pack_init_from_exe(const char* exe_path) {
    clear_loaded_pack();
    if (!exe_path || !exe_path[0])
        return false;

    FILE* f = fopen(exe_path, "rb");
    if (!f)
        return false;

    unsigned long long file_size = 0;
    LtPackFooter footer = {};
    bool has_pack = read_u64_file_size(f, &file_size) && find_pack_footer(f, file_size, &footer);
    if (!has_pack) {
        fclose(f);
        return false;
    }

    if (footer.pack_size > (unsigned long long)SIZE_MAX) {
        fclose(f);
        return false;
    }

    unsigned char* bytes = (unsigned char*)malloc((size_t)footer.pack_size);
    if (!bytes) {
        fclose(f);
        return false;
    }
#if defined(_WIN32)
    if (_fseeki64(f, (__int64)footer.pack_offset, SEEK_SET) != 0) {
#else
    if (fseek(f, (long)footer.pack_offset, SEEK_SET) != 0) {
#endif
        free(bytes);
        fclose(f);
        return false;
    }
    size_t got = fread(bytes, 1, (size_t)footer.pack_size, f);
    fclose(f);
    if (got != (size_t)footer.pack_size) {
        free(bytes);
        return false;
    }

    if (footer.pack_size < sizeof(LtPackHeader)) {
        free(bytes);
        return false;
    }
    LtPackHeader* hdr = (LtPackHeader*)bytes;
    if (memcmp(hdr->magic, k_data_magic, sizeof(k_data_magic)) != 0 ||
        hdr->version != k_pack_version ||
        hdr->project_path_len >= MAX_PATH_LEN) {
        free(bytes);
        return false;
    }

    size_t pos = sizeof(LtPackHeader);
    if (pos + hdr->project_path_len > (size_t)footer.pack_size) {
        free(bytes);
        return false;
    }
    memcpy(s_pack_project_path, bytes + pos, hdr->project_path_len);
    s_pack_project_path[hdr->project_path_len] = '\0';
    pos += hdr->project_path_len;

    LtPackFile* files = (LtPackFile*)calloc(hdr->file_count ? hdr->file_count : 1, sizeof(LtPackFile));
    if (!files) {
        free(bytes);
        return false;
    }

    for (unsigned int i = 0; i < hdr->file_count; i++) {
        if (pos + sizeof(LtPackEntryHeader) > (size_t)footer.pack_size) {
            free(files);
            free(bytes);
            return false;
        }
        LtPackEntryHeader* eh = (LtPackEntryHeader*)(bytes + pos);
        pos += sizeof(LtPackEntryHeader);
        if (eh->path_len == 0 || eh->path_len >= MAX_PATH_LEN ||
            pos + eh->path_len + eh->data_size > (size_t)footer.pack_size) {
            free(files);
            free(bytes);
            return false;
        }
        char raw_path[MAX_PATH_LEN] = {};
        memcpy(raw_path, bytes + pos, eh->path_len);
        pos += eh->path_len;
        normalize_path(raw_path, files[i].path, MAX_PATH_LEN);
        files[i].data = bytes + pos;
        files[i].size = (size_t)eh->data_size;
        pos += (size_t)eh->data_size;
    }

    s_pack_bytes = bytes;
    s_pack_size = (size_t)footer.pack_size;
    s_pack_files = files;
    s_pack_file_count = (int)hdr->file_count;
    return true;
}

bool lt_pack_is_loaded() {
    return s_pack_bytes != nullptr;
}

const char* lt_pack_project_path() {
    return s_pack_project_path[0] ? s_pack_project_path : nullptr;
}

int lt_pack_file_count() {
    return s_pack_file_count;
}

struct ExportFile {
    char pack_path[MAX_PATH_LEN];
    char source_path[MAX_PATH_LEN];
};

struct ExportList {
    ExportFile files[512];
    int count;
};

static bool export_add_file(ExportList* list, const char* pack_path, const char* source_path) {
    if (!list || !pack_path || !pack_path[0] || !source_path || !source_path[0])
        return false;
    char norm_pack[MAX_PATH_LEN] = {};
    char norm_src[MAX_PATH_LEN] = {};
    normalize_path(pack_path, norm_pack, MAX_PATH_LEN);
    normalize_path(source_path, norm_src, MAX_PATH_LEN);
    if (!norm_pack[0] || !norm_src[0])
        return false;
    for (int i = 0; i < list->count; i++) {
        if (_stricmp(list->files[i].pack_path, norm_pack) == 0)
            return true;
    }
    if (list->count >= (int)(sizeof(list->files) / sizeof(list->files[0])))
        return false;
    strncpy(list->files[list->count].pack_path, norm_pack, MAX_PATH_LEN - 1);
    strncpy(list->files[list->count].source_path, norm_src, MAX_PATH_LEN - 1);
    list->count++;
    return true;
}

static bool has_ext(const char* path, const char* ext) {
    if (!path || !ext)
        return false;
    const char* dot = strrchr(path, '.');
    return dot && _stricmp(dot, ext) == 0;
}

static const char* skip_ws(const char* p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p ? p : "";
}

static void trim_line(char* line) {
    if (!line)
        return;
    char* start = (char*)skip_ws(line);
    if (start != line)
        memmove(line, start, strlen(start) + 1);

    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                       line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
}

static bool append_bytes(unsigned char** out, size_t* out_len, size_t* out_cap,
                         const void* data, size_t data_len) {
    if (!out || !out_len || !out_cap || (!data && data_len))
        return false;
    if (*out_len + data_len + 1 > *out_cap) {
        size_t new_cap = *out_cap ? *out_cap : 256;
        while (*out_len + data_len + 1 > new_cap)
            new_cap *= 2;
        unsigned char* next = (unsigned char*)realloc(*out, new_cap);
        if (!next)
            return false;
        *out = next;
        *out_cap = new_cap;
    }
    if (data_len)
        memcpy(*out + *out_len, data, data_len);
    *out_len += data_len;
    (*out)[*out_len] = 0;
    return true;
}

static bool append_cstr(unsigned char** out, size_t* out_len, size_t* out_cap, const char* s) {
    return append_bytes(out, out_len, out_cap, s, s ? strlen(s) : 0);
}

static bool project_line_is_known_default(const char* line) {
    if (!line || !line[0])
        return true;

    if (strcmp(line, "lazyTool_project 1") == 0 ||
        strcmp(line, "resources") == 0 ||
        strcmp(line, "commands") == 0)
        return true;

    // Exported runtime projects start from the same defaults before parsing.
    if (strcmp(line, "camera_fps 5 5 5 -2.3561945 -0.615479767 1.04700005 0.00100000005 100") == 0)
        return true;
    if (strcmp(line, "dirlight 5 5 5 0 0 0 1 0.949999988 0.899999976 1 1024 1204 0.00999999978 10 8 8 1 5 0.649999976 0.443228424 8 8 0.00999999978 10 0.921136975 8 8 0.00999999978 10 1.69907975 8 8 0.00999999978 10 5 8 8 0.00999999978 10") == 0)
        return true;

    if (strcmp(line, "mrts 0") == 0 ||
        strcmp(line, "mesh_shader - -") == 0 ||
        strcmp(line, "draw_source mesh") == 0 ||
        strcmp(line, "topology triangle_list") == 0 ||
        strcmp(line, "shadow_shader -") == 0 ||
        strcmp(line, "render_state 1 1 1 0 1 0 0") == 0 ||
        strcmp(line, "transform 0 0 0 0 0 0 1 1 1") == 0 ||
        strcmp(line, "clear 1 0.0500000007 0.0500000007 0.0799999982 1 1 1") == 0 ||
        strcmp(line, "vertex_count 3") == 0 ||
        strcmp(line, "instance 1") == 0 ||
        strcmp(line, "threads 1 1 1") == 0 ||
        strcmp(line, "compute_on_reset 0") == 0 ||
        strcmp(line, "dispatch_from -") == 0 ||
        strcmp(line, "indirect_args - 0") == 0 ||
        strcmp(line, "repeat 1 1") == 0 ||
        strcmp(line, "parent -") == 0 ||
        strcmp(line, "textures 0") == 0 ||
        strcmp(line, "srvs 0") == 0 ||
        strcmp(line, "uavs 0") == 0 ||
        strcmp(line, "params 0") == 0)
        return true;

    return false;
}

static void project_minify_command_line(char* line) {
    char tmp[1024] = {};
    strncpy(tmp, line, sizeof(tmp) - 1);
    char* tag = strtok(tmp, " \t\r\n");
    if (!tag || strcmp(tag, "command") != 0)
        return;
    char* kind = strtok(nullptr, " \t\r\n");
    char* name = strtok(nullptr, " \t\r\n");
    char* enabled = strtok(nullptr, " \t\r\n");
    if (kind && name && enabled && strcmp(enabled, "1") == 0)
        snprintf(line, 1024, "command %s %s", kind, name);
}

static bool project_timeline_block_disabled(const char* cursor, const char* end, const char** out_after_block) {
    if (out_after_block)
        *out_after_block = cursor;

    const char* scan = cursor;
    const char* after_end = cursor;
    bool found_end = false;
    bool disabled = false;
    while (scan < end) {
        char line[1024] = {};
        int n = 0;
        while (scan < end && *scan != '\n' && *scan != '\r') {
            if (n < (int)sizeof(line) - 1)
                line[n++] = *scan;
            scan++;
        }
        while (scan < end && (*scan == '\n' || *scan == '\r'))
            scan++;
        after_end = scan;
        line[n] = '\0';
        trim_line(line);

        char tmp[1024] = {};
        strncpy(tmp, line, sizeof(tmp) - 1);
        char* tag = strtok(tmp, " \t\r\n");
        if (!tag)
            continue;
        if (strcmp(tag, "timeline_settings") == 0) {
            strtok(nullptr, " \t\r\n");
            strtok(nullptr, " \t\r\n");
            strtok(nullptr, " \t\r\n");
            strtok(nullptr, " \t\r\n");
            strtok(nullptr, " \t\r\n");
            char* enabled = strtok(nullptr, " \t\r\n");
            if (enabled && atoi(enabled) == 0)
                disabled = true;
        } else if (strcmp(tag, "end_timeline") == 0) {
            found_end = true;
            break;
        }
    }

    if (disabled && found_end && out_after_block)
        *out_after_block = after_end;
    return disabled && found_end;
}

static bool minify_project_text(const void* data, size_t size, void** out_data, size_t* out_size) {
    if (!data || !out_data || !out_size)
        return false;

    unsigned char* out = nullptr;
    size_t out_len = 0;
    size_t out_cap = 0;

    const char* cursor = (const char*)data;
    const char* end = cursor + size;
    while (cursor < end) {
        char line[1024] = {};
        int n = 0;
        while (cursor < end && *cursor != '\n' && *cursor != '\r') {
            if (n < (int)sizeof(line) - 1)
                line[n++] = *cursor;
            cursor++;
        }
        while (cursor < end && (*cursor == '\n' || *cursor == '\r'))
            cursor++;
        line[n] = '\0';
        trim_line(line);
        if (strcmp(line, "timeline") == 0) {
            const char* after_timeline = cursor;
            if (project_timeline_block_disabled(cursor, end, &after_timeline)) {
                cursor = after_timeline;
                continue;
            }
        }
        if (project_line_is_known_default(line))
            continue;
        project_minify_command_line(line);
        if (!append_cstr(&out, &out_len, &out_cap, line) ||
            !append_cstr(&out, &out_len, &out_cap, "\n")) {
            free(out);
            return false;
        }
    }

    if (!out) {
        out = (unsigned char*)malloc(1);
        if (!out)
            return false;
        out[0] = 0;
    }
    *out_data = out;
    *out_size = out_len;
    return true;
}

static bool minify_hlsl_text(const void* data, size_t size, void** out_data, size_t* out_size) {
    if (!data || !out_data || !out_size)
        return false;

    unsigned char* no_comments = (unsigned char*)malloc(size + 1);
    if (!no_comments)
        return false;

    const char* src = (const char*)data;
    size_t wi = 0;
    bool in_string = false;
    char string_ch = 0;
    for (size_t i = 0; i < size; i++) {
        char c = src[i];
        if (in_string) {
            no_comments[wi++] = (unsigned char)c;
            if (c == '\\' && i + 1 < size) {
                no_comments[wi++] = (unsigned char)src[++i];
                continue;
            }
            if (c == string_ch)
                in_string = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            string_ch = c;
            no_comments[wi++] = (unsigned char)c;
            continue;
        }
        if (c == '/' && i + 1 < size && src[i + 1] == '/') {
            i += 2;
            while (i < size && src[i] != '\n' && src[i] != '\r')
                i++;
            no_comments[wi++] = '\n';
            continue;
        }
        if (c == '/' && i + 1 < size && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < size && !(src[i] == '*' && src[i + 1] == '/'))
                i++;
            i++;
            no_comments[wi++] = ' ';
            continue;
        }
        no_comments[wi++] = (unsigned char)c;
    }
    no_comments[wi] = 0;

    unsigned char* out = nullptr;
    size_t out_len = 0;
    size_t out_cap = 0;
    const char* cursor = (const char*)no_comments;
    const char* end = cursor + wi;
    while (cursor < end) {
        char line[2048] = {};
        int n = 0;
        while (cursor < end && *cursor != '\n' && *cursor != '\r') {
            if (n < (int)sizeof(line) - 1)
                line[n++] = *cursor;
            cursor++;
        }
        while (cursor < end && (*cursor == '\n' || *cursor == '\r'))
            cursor++;
        line[n] = '\0';
        trim_line(line);
        if (!line[0])
            continue;

        bool prev_space = false;
        for (char* p = line; *p; p++) {
            char c = *p;
            bool is_space = c == ' ' || c == '\t';
            if (is_space) {
                if (!prev_space) {
                    char sp = ' ';
                    if (!append_bytes(&out, &out_len, &out_cap, &sp, 1)) {
                        free(no_comments);
                        free(out);
                        return false;
                    }
                }
                prev_space = true;
            } else {
                if (!append_bytes(&out, &out_len, &out_cap, &c, 1)) {
                    free(no_comments);
                    free(out);
                    return false;
                }
                prev_space = false;
            }
        }
        char nl = '\n';
        if (!append_bytes(&out, &out_len, &out_cap, &nl, 1)) {
            free(no_comments);
            free(out);
            return false;
        }
    }

    free(no_comments);
    if (!out) {
        out = (unsigned char*)malloc(1);
        if (!out)
            return false;
        out[0] = 0;
    }
    *out_data = out;
    *out_size = out_len;
    return true;
}

static cgltf_result export_cgltf_read(const cgltf_memory_options* memory_options,
                                      const cgltf_file_options*,
                                      const char* path,
                                      cgltf_size* size,
                                      void** data)
{
    (void)memory_options;
    void* bytes = nullptr;
    size_t byte_count = 0;
    if (!lt_read_file(path, &bytes, &byte_count))
        return cgltf_result_file_not_found;
    *data = bytes;
    if (size)
        *size = (cgltf_size)byte_count;
    return cgltf_result_success;
}

static void export_cgltf_release(const cgltf_memory_options*,
                                 const cgltf_file_options*,
                                 void* data,
                                 cgltf_size)
{
    lt_free_file(data);
}

static void collect_gltf_refs(ExportList* list, const char* gltf_path) {
    if (!list || !gltf_path || !has_ext(gltf_path, ".gltf"))
        return;

    cgltf_options opts = {};
    opts.file.read = export_cgltf_read;
    opts.file.release = export_cgltf_release;
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, gltf_path, &data) != cgltf_result_success || !data)
        return;

    char dir[MAX_PATH_LEN] = {};
    path_dirname(gltf_path, dir, MAX_PATH_LEN);

    for (cgltf_size i = 0; i < data->buffers_count; i++) {
        const char* uri = data->buffers[i].uri;
        if (!uri || !uri[0] || strncmp(uri, "data:", 5) == 0 || strstr(uri, "://"))
            continue;
        char decoded[MAX_PATH_LEN] = {};
        strncpy(decoded, uri, MAX_PATH_LEN - 1);
        cgltf_decode_uri(decoded);
        char full[MAX_PATH_LEN] = {};
        path_join(dir, decoded, full, MAX_PATH_LEN);
        export_add_file(list, full, full);
    }

    for (cgltf_size i = 0; i < data->images_count; i++) {
        const char* uri = data->images[i].uri;
        if (!uri || !uri[0] || strncmp(uri, "data:", 5) == 0 || strstr(uri, "://"))
            continue;
        char decoded[MAX_PATH_LEN] = {};
        strncpy(decoded, uri, MAX_PATH_LEN - 1);
        cgltf_decode_uri(decoded);
        char full[MAX_PATH_LEN] = {};
        path_join(dir, decoded, full, MAX_PATH_LEN);
        export_add_file(list, full, full);
    }

    cgltf_free(data);
}

static void collect_shader_includes(ExportList* list, const char* shader_path, int depth) {
    if (!list || !shader_path || depth > 8)
        return;

    void* bytes = nullptr;
    size_t size = 0;
    if (!lt_read_file(shader_path, &bytes, &size))
        return;

    char dir[MAX_PATH_LEN] = {};
    path_dirname(shader_path, dir, MAX_PATH_LEN);

    const char* text = (const char*)bytes;
    const char* p = text;
    while ((p = strstr(p, "#include")) != nullptr) {
        const char* q = p + 8;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != '"' && *q != '<') {
            p = q;
            continue;
        }
        char end_ch = *q == '"' ? '"' : '>';
        q++;
        char inc[MAX_PATH_LEN] = {};
        int ii = 0;
        while (*q && *q != end_ch && ii < MAX_PATH_LEN - 1)
            inc[ii++] = *q++;
        inc[ii] = '\0';
        if (inc[0]) {
            char full[MAX_PATH_LEN] = {};
            path_join(dir, inc, full, MAX_PATH_LEN);
            export_add_file(list, full, full);
            collect_shader_includes(list, full, depth + 1);
        }
        p = q;
    }

    lt_free_file(bytes);
}

static bool collect_project_refs(ExportList* list, const char* project_path, char* err, int err_sz) {
    void* bytes = nullptr;
    size_t size = 0;
    if (!lt_read_file(project_path, &bytes, &size)) {
        set_err(err, err_sz, "Project file not found.");
        return false;
    }

    char* text = (char*)bytes;
    char* line = text;
    while (line && *line) {
        char* next = strpbrk(line, "\r\n");
        if (next) {
            *next++ = '\0';
            while (*next == '\r' || *next == '\n')
                next++;
        }

        char tmp[1024] = {};
        strncpy(tmp, line, sizeof(tmp) - 1);
        char* tag = strtok(tmp, " \t");
        if (tag && strcmp(tag, "resource") == 0) {
            char* kind = strtok(nullptr, " \t");
            char* name = strtok(nullptr, " \t");
            char* path = strtok(nullptr, " \t");
            (void)name;
            bool file_resource =
                kind &&
                (strcmp(kind, "mesh_gltf") == 0 ||
                 strcmp(kind, "texture2d") == 0 ||
                 strcmp(kind, "shader_vsps") == 0 ||
                 strcmp(kind, "shader_cs") == 0);
            if (file_resource && path && strcmp(path, "-") != 0) {
                export_add_file(list, path, path);
                if (strcmp(kind, "shader_vsps") == 0 || strcmp(kind, "shader_cs") == 0)
                    collect_shader_includes(list, path, 0);
                if (strcmp(kind, "mesh_gltf") == 0)
                    collect_gltf_refs(list, path);
            }
        }

        line = next;
    }

    lt_free_file(bytes);
    return true;
}

static bool copy_bytes(FILE* dst, FILE* src, unsigned long long count) {
    unsigned char buffer[64 * 1024];
    while (count > 0) {
        size_t want = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        size_t got = fread(buffer, 1, want, src);
        if (got == 0)
            return false;
        if (fwrite(buffer, 1, got, dst) != got)
            return false;
        count -= got;
    }
    return true;
}

static bool lt_export_normal_exe_internal(const char* base_exe_path,
                                          const char* project_path,
                                          const char* output_exe_path,
                                          char* err,
                                          int err_sz);

bool lt_export_normal_exe(const char* base_exe_path,
                          const char* project_path,
                          const char* output_exe_path,
                          char* err,
                          int err_sz)
{
    return lt_export_normal_exe_internal(base_exe_path, project_path, output_exe_path,
                                         err, err_sz);
}

static bool write_entry(FILE* out, const char* pack_path, const void* data, size_t size) {
    char norm[MAX_PATH_LEN] = {};
    normalize_path(pack_path, norm, MAX_PATH_LEN);
    unsigned int path_len = (unsigned int)strlen(norm);
    LtPackEntryHeader eh = {};
    eh.path_len = path_len;
    eh.data_size = (unsigned long long)size;
    return fwrite(&eh, 1, sizeof(eh), out) == sizeof(eh) &&
           fwrite(norm, 1, path_len, out) == path_len &&
           fwrite(data, 1, size, out) == size;
}

// Build a self-contained normal player executable. The embedded project uses a
// short synthetic path ("p") because every byte saved in the pack also reduces
// the final exe when users run an external compressor afterwards.
static bool lt_export_normal_exe_internal(const char* base_exe_path,
                                          const char* project_path,
                                          const char* output_exe_path,
                                          char* err,
                                          int err_sz)
{
    set_err(err, err_sz, "");
    if (!base_exe_path || !base_exe_path[0] ||
        !project_path || !project_path[0] ||
        !output_exe_path || !output_exe_path[0]) {
        set_err(err, err_sz, "Usage: --export <project.lt> <output.exe>");
        return false;
    }

    ExportList list = {};
    if (!collect_project_refs(&list, project_path, err, err_sz))
        return false;

    void* project_bytes = nullptr;
    size_t project_size = 0;
    if (!lt_read_file(project_path, &project_bytes, &project_size)) {
        set_err(err, err_sz, "Project file not found.");
        return false;
    }

    char actual_base_exe[MAX_PATH_LEN] = {};
    choose_normal_export_base_exe(base_exe_path, actual_base_exe, MAX_PATH_LEN);

    FILE* base = fopen(actual_base_exe, "rb");
    if (!base) {
        lt_free_file(project_bytes);
        set_err(err, err_sz, "Base executable not found.");
        return false;
    }
    unsigned long long base_file_size = 0;
    if (!read_u64_file_size(base, &base_file_size)) {
        fclose(base);
        lt_free_file(project_bytes);
        set_err(err, err_sz, "Could not read base executable size.");
        return false;
    }

    LtPackFooter old_footer = {};
    unsigned long long copy_size = base_file_size;
    if (find_pack_footer(base, base_file_size, &old_footer))
        copy_size = old_footer.pack_offset;

    FILE* out = fopen(output_exe_path, "wb");
    if (!out) {
        fclose(base);
        lt_free_file(project_bytes);
        set_err(err, err_sz, "Could not create output executable.");
        return false;
    }

#if defined(_WIN32)
    _fseeki64(base, 0, SEEK_SET);
#else
    fseek(base, 0, SEEK_SET);
#endif
    if (!copy_bytes(out, base, copy_size)) {
        fclose(out);
        fclose(base);
        lt_free_file(project_bytes);
        set_err(err, err_sz, "Failed while copying base executable.");
        return false;
    }
    fclose(base);

    unsigned long long pack_offset = copy_size;
    LtPackHeader hdr = {};
    memcpy(hdr.magic, k_data_magic, sizeof(k_data_magic));
    hdr.version = k_pack_version;
    hdr.file_count = (unsigned int)(list.count + 1);
    const char* embedded_project_path = k_embedded_project;
    hdr.project_path_len = (unsigned int)strlen(embedded_project_path);
    if (fwrite(&hdr, 1, sizeof(hdr), out) != sizeof(hdr) ||
        fwrite(embedded_project_path, 1, hdr.project_path_len, out) != hdr.project_path_len) {
        fclose(out);
        lt_free_file(project_bytes);
        set_err(err, err_sz, "Failed while writing pack header.");
        return false;
    }

    void* packed_project_bytes = nullptr;
    size_t packed_project_size = 0;
    bool project_packed = minify_project_text(project_bytes, project_size,
                                              &packed_project_bytes,
                                              &packed_project_size);
    if (!project_packed) {
        fclose(out);
        lt_free_file(project_bytes);
        set_err(err, err_sz, "Failed while minifying project.");
        return false;
    }
    lt_free_file(project_bytes);

    if (!write_entry(out, embedded_project_path, packed_project_bytes, packed_project_size)) {
        fclose(out);
        lt_free_file(packed_project_bytes);
        set_err(err, err_sz, "Failed while writing project entry.");
        return false;
    }
    lt_free_file(packed_project_bytes);

    for (int i = 0; i < list.count; i++) {
        void* bytes = nullptr;
        size_t size = 0;
        if (!lt_read_file(list.files[i].source_path, &bytes, &size)) {
            char msg[512] = {};
            snprintf(msg, sizeof(msg), "Referenced file not found: %s", list.files[i].source_path);
            fclose(out);
            set_err(err, err_sz, msg);
            return false;
        }
        void* packed_bytes = bytes;
        size_t packed_size = size;
        bool packed_allocated = false;
        if (has_ext(list.files[i].pack_path, ".hlsl")) {
            void* min_bytes = nullptr;
            size_t min_size = 0;
            if (minify_hlsl_text(bytes, size, &min_bytes, &min_size)) {
                packed_bytes = min_bytes;
                packed_size = min_size;
                packed_allocated = true;
            }
        }
        bool ok = write_entry(out, list.files[i].pack_path, packed_bytes, packed_size);
        if (packed_allocated)
            free(packed_bytes);
        lt_free_file(bytes);
        if (!ok) {
            fclose(out);
            set_err(err, err_sz, "Failed while writing file entry.");
            return false;
        }
    }

    unsigned long long end_pos = 0;
#if defined(_WIN32)
    __int64 pos = _ftelli64(out);
    end_pos = pos >= 0 ? (unsigned long long)pos : 0;
#else
    long pos = ftell(out);
    end_pos = pos >= 0 ? (unsigned long long)pos : 0;
#endif
    LtPackFooter footer = {};
    footer.pack_offset = pack_offset;
    footer.pack_size = end_pos - pack_offset;
    memcpy(footer.magic, k_footer_magic, sizeof(k_footer_magic));
    bool ok = fwrite(&footer, 1, sizeof(footer), out) == sizeof(footer);
    fclose(out);
    if (!ok) {
        set_err(err, err_sz, "Failed while writing pack footer.");
        return false;
    }
    return true;
}
