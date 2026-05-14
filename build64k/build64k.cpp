// lazyTool 64k procedural exporter.
//
// Design goal
// -----------
// This exporter is intentionally a *procedural-only* standalone player generator.
// It reads a normal lazyTool .lt project, filters it to the subset that can be
// recreated without shipping asset files, and emits one Win32/D3D11 C source file.
//
// Allowed in the 64k path:
//   - shader_vsps resources; shader source is embedded as text and compiled at startup.
//   - mesh_primitive resources; the runtime creates tiny built-in vertex buffers.
//   - draw_source procedural commands; the shader can synthesize positions from SV_VertexID.
//   - render_texture resources; these become internal D3D11 render targets/SRVs.
//   - scene_color, scene_depth and shadow_map builtins.
//   - command parameters, user constant buffer values, timeline tracks and shadows.
//
// Explicitly not packed:
//   - mesh_gltf / mesh files with a path.
//   - texture2d files with a path.
//   - arbitrary external binary buffers.
//
// The code below favors being easy to inspect and extend over aggressive source
// golfing. The generated .exe can be passed through UPX afterwards, so the C source
// remains didactic and heavily commented where the architecture matters.

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef LT64K_MINIFY_HLSL
#define LT64K_MINIFY_HLSL 0
#endif

static void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static void warnf(const char* fmt, ...) {
    fprintf(stderr, "warning: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static std::string path_slashes(std::string p) {
    for (size_t i = 0; i < p.size(); i++) if (p[i] == '\\') p[i] = '/';
    return p;
}

static std::string read_text_file(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        std::string alt = path_slashes(path);
        if (alt != path) f.open(alt.c_str(), std::ios::binary);
    }
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool file_exists(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        std::string alt = path_slashes(path);
        if (alt != path) f.open(alt.c_str(), std::ios::binary);
    }
    return (bool)f;
}


static long long file_size_bytes(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f) {
        std::string alt = path_slashes(path);
        if (alt != path) f.open(alt.c_str(), std::ios::binary | std::ios::ate);
    }
    if (!f) return -1;
    return (long long)f.tellg();
}

static std::string path_dir(const std::string& p) {
    size_t a = p.find_last_of("/\\");
    if (a == std::string::npos) return ".";
    return p.substr(0, a);
}

static std::string path_join(const std::string& a, const std::string& b) {
    if (b.empty()) return a;
    if (b.size() > 1 && (b[1] == ':' || b[0] == '/' || b[0] == '\\')) return b;
    if (a.empty() || a == ".") return b;
    char last = a[a.size() - 1];
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

static std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

static std::string ref_name(const std::string& r) {
    if (r.empty() || r == "-") return std::string();
    size_t p = r.find('|');
    return p == std::string::npos ? r : r.substr(0, p);
}

static std::string cstr_literal(const std::string& s) {
    std::string o;
    o.reserve(s.size() + s.size() / 8 + 16);
    o += '"';
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') o += "\\\\";
        else if (c == '"') o += "\\\"";
        else if (c == '\n') o += "\\n\"\n\"";
        else if (c == '\r') {}
        else if (c == '\t') o += "\\t";
        else if (c < 32 || c > 126) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\x%02X", c);
            o += b;
        } else o += (char)c;
    }
    o += '"';
    return o;
}

static std::string find_file_for_path(const std::string& p, const std::vector<std::string>& roots) {
    if (p.empty() || p == "-") return std::string();
    if (file_exists(p)) return p;
    for (size_t i = 0; i < roots.size(); i++) {
        std::string c = path_join(roots[i], p);
        if (file_exists(c)) return c;
    }
    return std::string();
}

static std::string expand_hlsl_includes_rec(const std::string& path,
                                            const std::vector<std::string>& roots,
                                            std::set<std::string>& stack,
                                            int depth) {
    if (depth > 16) die("too many nested #include files around %s", path.c_str());
    if (stack.count(path)) die("cyclic #include: %s", path.c_str());
    stack.insert(path);
    std::string src = read_text_file(path);
    if (src.empty()) die("cannot read shader: %s", path.c_str());

    std::string dir = path_dir(path);
    std::istringstream in(src);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.rfind("#include", 0) == 0) {
            size_t q0 = t.find('"');
            size_t q1 = (q0 == std::string::npos) ? std::string::npos : t.find('"', q0 + 1);
            if (q0 != std::string::npos && q1 != std::string::npos && q1 > q0 + 1) {
                std::string inc = t.substr(q0 + 1, q1 - q0 - 1);
                std::vector<std::string> inc_roots = roots;
                inc_roots.insert(inc_roots.begin(), dir);
                std::string inc_path = find_file_for_path(inc, inc_roots);
                if (inc_path.empty()) die("cannot resolve include '%s' from %s", inc.c_str(), path.c_str());
                out << "\n// begin include: " << inc << "\n";
                out << expand_hlsl_includes_rec(inc_path, inc_roots, stack, depth + 1);
                out << "\n// end include: " << inc << "\n";
                continue;
            }
        }
        out << line << "\n";
    }
    stack.erase(path);
    return out.str();
}

static std::string expand_hlsl_includes(const std::string& path, const std::vector<std::string>& roots) {
    std::set<std::string> stack;
    return expand_hlsl_includes_rec(path, roots, stack, 0);
}


static bool hlsl_ident_char(char c) {
    unsigned char u = (unsigned char)c;
    return std::isalnum(u) || c == '_';
}

static std::string strip_hlsl_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool block = false;
    for (size_t i = 0; i < src.size(); i++) {
        char c = src[i];
        char n = (i + 1 < src.size()) ? src[i + 1] : 0;
        if (block) {
            if (c == '*' && n == '/') { block = false; i++; }
            else if (c == '\n') out += '\n';
            continue;
        }
        if (c == '/' && n == '*') { block = true; i++; continue; }
        if (c == '/' && n == '/') {
            while (i < src.size() && src[i] != '\n') i++;
            if (i < src.size()) out += '\n';
            continue;
        }
        out += c;
    }
    return out;
}

static std::string compact_hlsl_line(const std::string& line) {
    std::string t = trim(line);
    if (t.empty()) return std::string();
    if (t[0] == '#') return t;

    static const char* spaced_words[] = {
        "static", "const", "struct", "cbuffer", "return", "if", "else", "for", "while",
        "float", "float2", "float3", "float4", "float4x4", "int", "uint", "bool",
        "Texture2D", "SamplerState", "SamplerComparisonState"
    };

    std::string o;
    o.reserve(t.size());
    bool pending_space = false;
    char prev = 0;
    for (size_t i = 0; i < t.size(); i++) {
        char c = t[i];
        if (std::isspace((unsigned char)c)) { pending_space = true; continue; }
        if (pending_space) {
            bool need = hlsl_ident_char(prev) && hlsl_ident_char(c);
            if (!need && (prev == ']' && hlsl_ident_char(c))) need = true;
            if (!need && (hlsl_ident_char(prev) && c == '[')) need = false;
            if (!need && (prev == ')' && hlsl_ident_char(c))) need = true;
            if (need && !o.empty()) o += ' ';
        }
        o += c;
        prev = c;
        pending_space = false;
    }

    // Add back the few spaces HLSL needs after keywords/types if aggressive punctuation stripping ate them.
    // This pass is deliberately conservative: collapse only duplicate spaces.
    std::string fixed;
    fixed.reserve(o.size());
    for (size_t i = 0; i < o.size(); i++) {
        fixed += o[i];
        if (!hlsl_ident_char(o[i])) continue;
    }
    (void)spaced_words;
    return o;
}

static std::string minify_hlsl_for_64k(const std::string& src) {
    std::string no_comments = strip_hlsl_comments(src);
    std::istringstream in(no_comments);
    std::ostringstream out;
    std::string line;
    int common_guard_depth = 0;
    bool common_guard_seen = false;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t == "#ifndef PROCEDURAL_SPHERES_PBR_POST_COMMON_HLSL") {
            common_guard_depth = 1;
            common_guard_seen = true;
            continue;
        }
        if (common_guard_seen && common_guard_depth == 1 &&
            t == "#define PROCEDURAL_SPHERES_PBR_POST_COMMON_HLSL") {
            continue;
        }
        if (common_guard_depth > 0 && t.size() > 0 && t[0] == '#') {
            if (t.rfind("#if", 0) == 0 && t.rfind("#ifndef PROCEDURAL_SPHERES_PBR_POST_COMMON_HLSL", 0) != 0)
                common_guard_depth++;
            else if (t == "#endif") {
                common_guard_depth--;
                if (common_guard_depth == 0) continue;
            }
        }
        std::string c = compact_hlsl_line(line);
        if (!c.empty()) out << c << "\n";
    }
    return out.str();
}

// Resource categories parsed from the editor .lt file.
// Only a subset reaches the runtime. RK_MESH_FILE and RK_TEXTURE are deliberately
// recognized so we can print useful diagnostics, but they are not packed.
enum ResKind {
    RK_NONE = 0,
    RK_VALUE,
    RK_MESH_PRIMITIVE,
    RK_MESH_FILE,
    RK_SHADER_VSPS,
    RK_SHADER_CS,
    RK_TEXTURE,
    RK_RT,
    RK_RT3D,
    RK_BUFFER,
    RK_GAUSSIAN_SPLAT
};

enum ValType {
    VT_NONE = 0,
    VT_INT, VT_INT2, VT_INT3,
    VT_FLOAT, VT_FLOAT2, VT_FLOAT3, VT_FLOAT4
};

// Command categories in the editor project. The 64k player currently executes
// clears and draw calls. Groups are used only for editor organization, and repeat
// is flattened by the exporter. Compute/dispatch is parsed as an extension point.
enum CmdType {
    CT_NONE = 0,
    CT_CLEAR,
    CT_DRAW,
    CT_DRAW_INSTANCED,
    CT_DISPATCH,
    CT_GROUP,
    CT_REPEAT
};

// Timeline tracks are flattened into compact per-component arrays.
// The exporter avoids storing the editor-side 16-float union for every key.
enum TrackKind {
    TK_NONE = 0,
    TK_USER_VAR,
    TK_COMMAND_TRANSFORM,
    TK_COMMAND_ENABLED,
    TK_CAMERA,
    TK_DIRLIGHT
};

enum SourceKind {
    SRC_NONE = 0,
    SRC_RESOURCE,
    SRC_CMD_POS,
    SRC_CMD_ROT,
    SRC_CMD_SCALE,
    SRC_CAMERA_POS,
    SRC_CAMERA_ROT,
    SRC_DIRLIGHT_POS,
    SRC_DIRLIGHT_TARGET
};

static ValType parse_val_type(const std::string& s) {
    if (s == "int") return VT_INT;
    if (s == "int2") return VT_INT2;
    if (s == "int3") return VT_INT3;
    if (s == "float") return VT_FLOAT;
    if (s == "float2") return VT_FLOAT2;
    if (s == "float3") return VT_FLOAT3;
    if (s == "float4") return VT_FLOAT4;
    return VT_NONE;
}

static int val_components(ValType t) {
    switch (t) {
    case VT_INT: case VT_FLOAT: return 1;
    case VT_INT2: case VT_FLOAT2: return 2;
    case VT_INT3: case VT_FLOAT3: return 3;
    case VT_FLOAT4: return 4;
    default: return 0;
    }
}

static bool val_integral(ValType t) { return t == VT_INT || t == VT_INT2 || t == VT_INT3; }

static int timeline_value_count(TrackKind kind, ValType type) {
    if (kind == TK_COMMAND_TRANSFORM) return 10; // pos3 + quat4 + scale3
    if (kind == TK_COMMAND_ENABLED) return 1;
    if (kind == TK_CAMERA) return 8;
    if (kind == TK_DIRLIGHT) return 10; // light pos/target/color/intensity; shadow setup comes from dirlight base data
    return val_components(type);
}

static CmdType parse_cmd_type(const std::string& s) {
    if (s == "clear") return CT_CLEAR;
    if (s == "draw_mesh") return CT_DRAW;
    if (s == "draw_instanced") return CT_DRAW_INSTANCED;
    if (s == "dispatch") return CT_DISPATCH;
    if (s == "group") return CT_GROUP;
    if (s == "repeat") return CT_REPEAT;
    return CT_NONE;
}

static TrackKind parse_track_kind(const std::string& s) {
    if (s == "user" || s == "user_var") return TK_USER_VAR;
    if (s == "cmd_transform") return TK_COMMAND_TRANSFORM;
    if (s == "cmd_enabled") return TK_COMMAND_ENABLED;
    if (s == "camera") return TK_CAMERA;
    if (s == "dirlight") return TK_DIRLIGHT;
    return TK_NONE;
}

static SourceKind parse_source_kind(const std::string& s) {
    if (s == "resource") return SRC_RESOURCE;
    if (s == "cmd_pos") return SRC_CMD_POS;
    if (s == "cmd_rot") return SRC_CMD_ROT;
    if (s == "cmd_scale") return SRC_CMD_SCALE;
    if (s == "camera_pos") return SRC_CAMERA_POS;
    if (s == "camera_rot") return SRC_CAMERA_ROT;
    if (s == "dirlight_pos") return SRC_DIRLIGHT_POS;
    if (s == "dirlight_target") return SRC_DIRLIGHT_TARGET;
    return SRC_NONE;
}

// Parsed resource definition.
// For procedural export the name is the important stable identifier; external
// paths are used only for shaders, never for mesh/texture payloads.
struct ResourceDef {
    std::string name;
    ResKind kind = RK_NONE;
    ValType value_type = VT_NONE;
    std::string path;
    std::string primitive;
    int ival[8] = {};
    float fval[4] = {};
};

struct SlotRef {
    std::string name;
    int slot = 0;
};

struct UserVarDef {
    std::string name;
    ValType type = VT_NONE;
    SourceKind source_kind = SRC_NONE;
    std::string source_target;
    int ival[4] = {};
    float fval[4] = {};
};

struct ParamDef {
    std::string name;
    ValType type = VT_NONE;
    bool enabled = true;
    SourceKind source_kind = SRC_NONE;
    std::string source_target;
    int ival[4] = {};
    float fval[4] = {};
};

// Parsed render command. This mirrors only the fields needed by the small
// runtime: targets, shader, procedural/primitive geometry, render state, shadows,
// bindings and a compact list of material/user parameters.
struct CommandDef {
    std::string name;
    CmdType type = CT_NONE;
    bool enabled = true;
    std::string mesh;
    std::string shader;
    std::string shadow_shader;
    std::string rt;
    std::string depth;
    std::string parent;
    bool procedural = false;
    int topology = 0; // 0 point, 1 tri
    int mesh_kind = 0; // 0 procedural/no VB, 1 cube, 2 quad, 3 tetrahedron, 4 sphere, 5 fullscreen triangle
    bool color_write = true;
    bool depth_test = true;
    bool depth_write = true;
    bool alpha_blend = false;
    bool cull_back = true;
    bool shadow_cast = false;
    bool shadow_receive = false;
    bool builtin_shadow = false; // true when the generated primitive shadow VS is used
    bool unsupported_bindings = false;
    float pos[3] = {0,0,0};
    float rotq[4] = {0,0,0,1};
    float scale[3] = {1,1,1};
    bool clear_color_enabled = true;
    float clear_color[4] = {0,0,0,1};
    bool clear_depth = true;
    float depth_clear = 1.0f;
    std::string clear_color_source;
    std::string clear_depth_source;
    int vertex_count = 3;
    int instance_count = 1;
    int repeat_count = 1;
    std::vector<SlotRef> textures;
    std::vector<SlotRef> srvs;
    std::vector<SlotRef> uavs;
    std::vector<ParamDef> params;
};

struct TimelineKeyDef {
    int frame = 0;
    int ival[4] = {};
    float fval[16] = {};
};

struct TimelineTrackDef {
    TrackKind kind = TK_NONE;
    std::string target;
    ValType type = VT_NONE;
    bool enabled = true;
    std::vector<TimelineKeyDef> keys;
};

// The in-memory project is intentionally small. It is an intermediate format
// between the verbose editor .lt file and the emitted C arrays.
struct Project {
    float camera[8] = {5,5,5,-2.3561945f,-0.6154797f,1.047f,0.001f,100.0f};
    float dirlight[39] = {5,5,5,0,0,0,1,0.95f,0.9f,1,1024,1024,0.01f,10,8,8,1,5,0.65f,5,8,8,0.01f,10,100,8,8,0.01f,10,100,8,8,0.01f,10,100,8,8,0.01f,10};
    std::vector<ResourceDef> resources;
    std::vector<UserVarDef> user_vars;
    std::vector<CommandDef> commands;
    int timeline_fps = 24;
    int timeline_length = 240;
    bool timeline_loop = false;
    bool timeline_enabled = false;
    bool timeline_interpolate = false;
    std::vector<TimelineTrackDef> tracks;
};

static int find_res(const Project& p, const std::string& name) {
    for (size_t i = 0; i < p.resources.size(); i++) if (p.resources[i].name == name) return (int)i;
    return -1;
}

static int find_cmd(const Project& p, const std::string& name) {
    for (size_t i = 0; i < p.commands.size(); i++) if (p.commands[i].name == name) return (int)i;
    return -1;
}

static int find_user_var(const Project& p, const std::string& name) {
    for (size_t i = 0; i < p.user_vars.size(); i++) if (p.user_vars[i].name == name) return (int)i;
    return -1;
}

static float tokf(const std::vector<std::string>& t, size_t i, float def = 0.0f) {
    return i < t.size() ? (float)std::atof(t[i].c_str()) : def;
}
static int toki(const std::vector<std::string>& t, size_t i, int def = 0) {
    return i < t.size() ? std::atoi(t[i].c_str()) : def;
}

struct Q4 { float x, y, z, w; };

static Q4 qnorm(Q4 q) {
    float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (len <= 1e-8f) return {0,0,0,1};
    float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

static Q4 quat_from_euler_xyz(float x, float y, float z) {
    float cx = std::cos(x), sx = std::sin(x);
    float cy = std::cos(y), sy = std::sin(y);
    float cz = std::cos(z), sz = std::sin(z);
    float m00 = cy * cz;
    float m01 = cy * sz;
    float m02 = -sy;
    float m10 = sx * sy * cz - cx * sz;
    float m11 = sx * sy * sz + cx * cz;
    float m12 = sx * cy;
    float m20 = cx * sy * cz + sx * sz;
    float m21 = cx * sy * sz - sx * cz;
    float m22 = cx * cy;
    Q4 q = {0,0,0,1};
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (m12 - m21) / s; q.y = (m20 - m02) / s; q.z = (m01 - m10) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m12 - m21) / s; q.x = 0.25f * s; q.y = (m01 + m10) / s; q.z = (m20 + m02) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m20 - m02) / s; q.x = (m01 + m10) / s; q.y = 0.25f * s; q.z = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m01 - m10) / s; q.x = (m20 + m02) / s; q.y = (m12 + m21) / s; q.z = 0.25f * s;
    }
    return qnorm(q);
}

static void store_q(float out[4], Q4 q) {
    q = qnorm(q);
    out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
}

// Parse the text .lt file.
//
// This parser is intentionally forgiving: it accepts the editor format, ignores
// fields the procedural player does not need yet, and preserves enough data so
// future renderer paths can be added without changing the .lt syntax again.
static Project parse_lt(const std::string& lt_path) {
    std::string text = read_text_file(lt_path);
    if (text.empty()) die("cannot read lt: %s", lt_path.c_str());
    Project p;
    CommandDef* cur = nullptr;
    TimelineTrackDef* cur_track = nullptr;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string raw = trim(line);
        if (raw.empty() || raw[0] == '#') continue;
        std::vector<std::string> t = split_ws(raw);
        if (t.empty()) continue;
        const std::string& tag = t[0];
        if (tag == "camera_fps") {
            for (int i = 0; i < 8; i++) p.camera[i] = tokf(t, (size_t)i + 1, p.camera[i]);
        } else if (tag == "dirlight") {
            for (int i = 0; i < 39; i++) p.dirlight[i] = tokf(t, (size_t)i + 1, p.dirlight[i]);
        } else if (tag == "resource" && t.size() >= 3) {
            ResourceDef r;
            std::string kind = t[1];
            if (kind == "value" && t.size() >= 4) {
                r.kind = RK_VALUE;
                r.value_type = parse_val_type(t[2]);
                r.name = t[3];
                for (int i = 0; i < 4; i++) r.ival[i] = toki(t, (size_t)4 + i, 0);
                for (int i = 0; i < 4; i++) r.fval[i] = tokf(t, (size_t)8 + i, 0);
            } else if (kind == "mesh_primitive") {
                r.kind = RK_MESH_PRIMITIVE; r.name = t[2]; r.primitive = t.size() > 3 ? t[3] : "";
            } else if (kind == "mesh_gltf") {
                r.kind = RK_MESH_FILE; r.name = t[2]; r.path = t.size() > 3 ? t[3] : "";
            } else if (kind == "shader_vsps") {
                r.kind = RK_SHADER_VSPS; r.name = t[2]; r.path = t.size() > 3 ? t[3] : "";
            } else if (kind == "shader_cs") {
                r.kind = RK_SHADER_CS; r.name = t[2]; r.path = t.size() > 3 ? t[3] : "";
            } else if (kind == "texture2d") {
                r.kind = RK_TEXTURE; r.name = t[2]; r.path = t.size() > 3 ? t[3] : "";
            } else if (kind == "gaussian_splat") {
                r.kind = RK_GAUSSIAN_SPLAT; r.name = t[2]; r.path = t.size() > 3 ? t[3] : "";
            } else if (kind == "render_texture") {
                r.kind = RK_RT; r.name = t[2];
                r.ival[0] = toki(t,3,0); // width, 0 = scene width
                r.ival[1] = toki(t,4,0); // height, 0 = scene height
                r.ival[2] = toki(t,5,28); // DXGI_FORMAT
                r.ival[3] = toki(t,6,1); // RTV
                r.ival[4] = toki(t,7,1); // SRV
                r.ival[5] = toki(t,8,0); // UAV
                r.ival[6] = toki(t,9,0); // DSV
                r.ival[7] = toki(t,10,0); // scene scale divisor
            } else if (kind == "render_texture3d") {
                r.kind = RK_RT3D; r.name = t[2];
                r.ival[0] = toki(t,3,0);
                r.ival[1] = toki(t,4,0);
                r.ival[2] = toki(t,5,1); // depth
                r.ival[3] = toki(t,6,28); // DXGI_FORMAT
                r.ival[4] = toki(t,7,0); // RTV/slice capable
                r.ival[5] = toki(t,8,1); // SRV
                r.ival[6] = toki(t,9,0); // UAV
            } else if (kind == "structured_buffer") {
                r.kind = RK_BUFFER; r.name = t[2];
            }
            if (!r.name.empty()) p.resources.push_back(r);
        } else if (tag == "user_var" && t.size() >= 4) {
            UserVarDef u;
            u.name = t[1]; u.type = parse_val_type(t[2]);
            u.source_target = ref_name(t[3]);
            if (!u.source_target.empty()) u.source_kind = SRC_RESOURCE;
            for (int i = 0; i < 4; i++) u.ival[i] = toki(t, (size_t)4 + i, 0);
            for (int i = 0; i < 4; i++) u.fval[i] = tokf(t, (size_t)8 + i, 0);
            p.user_vars.push_back(u);
        } else if (tag == "user_var_source" && t.size() >= 4) {
            int ui = find_user_var(p, t[1]);
            if (ui >= 0) {
                p.user_vars[(size_t)ui].source_kind = parse_source_kind(t[2]);
                p.user_vars[(size_t)ui].source_target = t[3] == "-" ? std::string() : t[3];
            }
        } else if (tag == "command" && t.size() >= 3) {
            CommandDef c;
            c.type = parse_cmd_type(t[1]);
            c.name = t[2];
            c.enabled = t.size() > 3 ? (std::atoi(t[3].c_str()) != 0) : true;
            p.commands.push_back(c);
            cur = &p.commands.back();
        } else if (tag == "end_command") {
            cur = nullptr;
        } else if (cur) {
            if (tag == "targets" && t.size() >= 2) {
                cur->rt = ref_name(t[1]);
                cur->depth = t.size() >= 3 ? ref_name(t[2]) : std::string();
            } else if (tag == "mesh_shader" && t.size() >= 3) {
                cur->mesh = ref_name(t[1]);
                cur->shader = ref_name(t[2]);
            } else if (tag == "shadow_shader" && t.size() >= 2) {
                cur->shadow_shader = ref_name(t[1]);
            } else if (tag == "draw_source" && t.size() >= 2) {
                cur->procedural = (t[1] == "procedural");
            } else if (tag == "topology" && t.size() >= 2) {
                cur->topology = (t[1] == "point_list") ? 0 : 1;
            } else if (tag == "render_state" && t.size() >= 8) {
                cur->color_write = toki(t,1,1) != 0;
                cur->depth_test = toki(t,2,1) != 0;
                cur->depth_write = toki(t,3,1) != 0;
                cur->alpha_blend = toki(t,4,0) != 0;
                cur->cull_back = toki(t,5,1) != 0;
                cur->shadow_cast = toki(t,6,0) != 0;
                cur->shadow_receive = toki(t,7,0) != 0;
            } else if (tag == "transform" && t.size() >= 10) {
                for (int i = 0; i < 3; i++) cur->pos[i] = tokf(t, (size_t)1 + i);
                store_q(cur->rotq, quat_from_euler_xyz(tokf(t, 4), tokf(t, 5), tokf(t, 6)));
                for (int i = 0; i < 3; i++) cur->scale[i] = tokf(t, (size_t)7 + i, 1.0f);
            } else if (tag == "transformq" && t.size() >= 11) {
                for (int i = 0; i < 3; i++) cur->pos[i] = tokf(t, (size_t)1 + i);
                Q4 q = {tokf(t,4), tokf(t,5), tokf(t,6), tokf(t,7,1.0f)};
                store_q(cur->rotq, q);
                for (int i = 0; i < 3; i++) cur->scale[i] = tokf(t, (size_t)8 + i, 1.0f);
            } else if (tag == "clear" && t.size() >= 8) {
                cur->clear_color_enabled = toki(t,1,1) != 0;
                for (int i = 0; i < 4; i++) cur->clear_color[i] = tokf(t, (size_t)2 + i, i==3?1.0f:0.0f);
                cur->clear_depth = toki(t,6,1) != 0;
                cur->depth_clear = tokf(t,7,1.0f);
            } else if (tag == "clear_sources") {
                cur->clear_color_source = t.size() > 1 ? ref_name(t[1]) : std::string();
                cur->clear_depth_source = t.size() > 2 ? ref_name(t[2]) : std::string();
            } else if (tag == "vertex_count" && t.size() >= 2) {
                cur->vertex_count = toki(t,1,3);
            } else if (tag == "instance" && t.size() >= 2) {
                cur->instance_count = toki(t,1,1);
            } else if (tag == "repeat" && t.size() >= 2) {
                cur->repeat_count = std::max(1, toki(t,1,1));
            } else if (tag == "parent" && t.size() >= 2) {
                cur->parent = (t[1] == "-") ? std::string() : t[1];
            } else if (tag == "textures" && t.size() > 1) {
                int n = std::max(0, toki(t,1,0));
                for (int i = 0; i < n; i++) {
                    size_t b = (size_t)2 + (size_t)i * 2;
                    if (b + 1 < t.size()) cur->textures.push_back({ref_name(t[b]), toki(t,b+1,0)});
                }
            } else if (tag == "srvs" && t.size() > 1) {
                int n = std::max(0, toki(t,1,0));
                for (int i = 0; i < n; i++) {
                    size_t b = (size_t)2 + (size_t)i * 2;
                    if (b + 1 < t.size()) cur->srvs.push_back({ref_name(t[b]), toki(t,b+1,0)});
                }
            } else if (tag == "uavs" && t.size() > 1) {
                int n = std::max(0, toki(t,1,0));
                for (int i = 0; i < n; i++) {
                    size_t b = (size_t)2 + (size_t)i * 2;
                    if (b + 1 < t.size()) cur->uavs.push_back({ref_name(t[b]), toki(t,b+1,0)});
                }
                if (n > 0) {
                    cur->unsupported_bindings = true;
                    warnf("%s: UAV bindings are only exported for future compute/OM paths, not this VS/PS 64k player", cur->name.c_str());
                }
            } else if (tag == "param" && t.size() >= 5) {
                ParamDef pd;
                pd.name = t[1];
                pd.type = parse_val_type(t[2]);
                pd.enabled = toki(t,3,1) != 0;
                pd.source_target = t[4] == "-" ? std::string() : ref_name(t[4]);
                if (!pd.source_target.empty()) pd.source_kind = SRC_RESOURCE;
                for (int i = 0; i < 4; i++) pd.ival[i] = toki(t, (size_t)5 + i, 0);
                for (int i = 0; i < 4; i++) pd.fval[i] = tokf(t, (size_t)9 + i, 0.0f);
                cur->params.push_back(pd);
            } else if (tag == "param_source" && t.size() >= 3) {
                for (size_t i = 0; i < cur->params.size(); i++) {
                    ParamDef& pd = cur->params[i];
                    if (pd.name != t[1]) continue;
                    pd.source_kind = parse_source_kind(t[2]);
                    pd.source_target = (t.size() >= 4 && t[3] != "-") ? t[3] : std::string();
                    break;
                }
            }
        } else if (tag == "timeline_settings") {
            p.timeline_fps = toki(t,1,24);
            p.timeline_length = toki(t,2,240);
            p.timeline_loop = toki(t,5,0) != 0;
            p.timeline_enabled = toki(t,6,0) != 0;
            p.timeline_interpolate = toki(t,7,0) != 0;
        } else if (tag == "timeline_track" && t.size() >= 6) {
            TimelineTrackDef tr;
            tr.kind = parse_track_kind(t[1]);
            tr.target = t[2];
            tr.type = parse_val_type(t[3]);
            tr.enabled = toki(t,5,1) != 0;
            p.tracks.push_back(tr);
            cur_track = &p.tracks.back();
        } else if (tag == "timeline_key" && cur_track && t.size() >= 2) {
            TimelineKeyDef k;
            k.frame = toki(t,1,0);
            int n = timeline_value_count(cur_track->kind, cur_track->type);
            if (val_integral(cur_track->type) || cur_track->kind == TK_COMMAND_ENABLED) {
                for (int i = 0; i < n && i < 4; i++) k.ival[i] = toki(t, (size_t)2 + i, 0);
            } else {
                int value_count = (int)t.size() - 2;
                if (cur_track->kind == TK_COMMAND_TRANSFORM && value_count == 9) {
                    for (int i = 0; i < 3; i++) k.fval[i] = tokf(t, (size_t)2 + i, 0.0f);
                    Q4 q = quat_from_euler_xyz(tokf(t,5), tokf(t,6), tokf(t,7));
                    k.fval[3] = q.x; k.fval[4] = q.y; k.fval[5] = q.z; k.fval[6] = q.w;
                    for (int i = 0; i < 3; i++) k.fval[7 + i] = tokf(t, (size_t)8 + i, 1.0f);
                } else {
                    for (int i = 0; i < n && i < 16; i++) k.fval[i] = tokf(t, (size_t)2 + i, 0.0f);
                    if (cur_track->kind == TK_COMMAND_TRANSFORM) {
                        Q4 q = {k.fval[3], k.fval[4], k.fval[5], k.fval[6]};
                        q = qnorm(q);
                        k.fval[3] = q.x; k.fval[4] = q.y; k.fval[5] = q.z; k.fval[6] = q.w;
                    }
                }
            }
            cur_track->keys.push_back(k);
        } else if (tag == "end_timeline") {
            cur_track = nullptr;
        }
    }

    for (size_t i = 0; i < p.tracks.size(); i++) {
        std::sort(p.tracks[i].keys.begin(), p.tracks[i].keys.end(), [](const TimelineKeyDef& a, const TimelineKeyDef& b){ return a.frame < b.frame; });
    }
    return p;
}

static int shader_resource_index(const Project& p, const std::string& name, const std::vector<int>& shader_res_indices) {
    int ri = find_res(p, name);
    if (ri < 0) return -1;
    for (size_t i = 0; i < shader_res_indices.size(); i++) if (shader_res_indices[i] == ri) return (int)i;
    return -1;
}

static int rt_resource_index(const Project& p, const std::string& name, const std::vector<int>& rt_res_indices) {
    int ri = find_res(p, name);
    if (ri < 0) return -1;
    for (size_t i = 0; i < rt_res_indices.size(); i++) if (rt_res_indices[i] == ri) return (int)i;
    return -1;
}

static bool builtin_scene_color(const std::string& n) { return n == "scene_color" || n == "builtin_scene_color"; }
static bool builtin_scene_depth(const std::string& n) { return n == "scene_depth" || n == "builtin_scene_depth"; }
static bool builtin_shadow_map(const std::string& n) { return n == "shadow_map" || n == "builtin_shadow_map"; }

static int rt_target_code(const Project& p, const std::string& name, const std::vector<int>& rt_res_indices) {
    if (name.empty() || name == "-") return -1;
    if (builtin_scene_color(name)) return -2;
    return rt_resource_index(p, name, rt_res_indices);
}

static int depth_target_code(const std::string& name) {
    return builtin_scene_depth(name) ? -2 : -1;
}

// Texture/SRV source mapping used by draw calls.
// Return values are deliberately tiny runtime codes:
//   >= 0 : index into the generated render-target arrays
//   -2   : builtin scene depth
//   -3   : builtin shadow map
//   -1   : unsupported / unbound
static int texture_source_code(const Project& p, const std::string& name, const std::vector<int>& rt_res_indices) {
    if (name.empty() || name == "-") return -1;
    if (builtin_scene_depth(name)) return -2;
    if (builtin_shadow_map(name)) return -3;
    return rt_resource_index(p, name, rt_res_indices);
}

static bool user_var_has_timeline_track(const Project& p, const std::string& user_name, ValType type) {
    for (size_t i = 0; i < p.tracks.size(); i++) {
        const TimelineTrackDef& tr = p.tracks[i];
        if (tr.kind == TK_USER_VAR && tr.enabled && tr.target == user_name && tr.type == type && !tr.keys.empty())
            return true;
    }
    return false;
}

static int clear_source_user_var_index(const Project& p, const std::string& source, ValType type) {
    if (source.empty() || source == "-") return -1;

    // Legacy compatibility: older editor builds could store a UserCB variable
    // name directly in clear_sources.  In that case the generated player can read
    // the timeline-updated uv[] slot directly.
    int named_user = find_user_var(p, source);
    if (named_user >= 0 && p.user_vars[(size_t)named_user].type == type)
        return named_user;

    // Current editor format stores value resources in clear_sources.  A timeline
    // animates those resources through a UserCB variable linked to that resource,
    // so prefer such a user var when one exists, especially if it has keys.
    int first_linked = -1;
    for (size_t i = 0; i < p.user_vars.size(); i++) {
        const UserVarDef& u = p.user_vars[i];
        if (u.type != type || u.source_kind != SRC_RESOURCE || u.source_target != source)
            continue;
        if (first_linked < 0) first_linked = (int)i;
        if (user_var_has_timeline_track(p, u.name, type))
            return (int)i;
    }
    return first_linked;
}

static void resolve_clear_sources_for_64k(const Project& p, CommandDef& c, int* clear_color_user, int* clear_depth_user) {
    if (clear_color_user) *clear_color_user = clear_source_user_var_index(p, c.clear_color_source, VT_FLOAT4);
    if (clear_depth_user) *clear_depth_user = clear_source_user_var_index(p, c.clear_depth_source, VT_FLOAT);

    // Bake static resource values as fallback.  Runtime uv[] sources below can
    // override this after timeline() has updated the corresponding UserCB slot.
    int color_res = find_res(p, c.clear_color_source);
    if (color_res >= 0) {
        const ResourceDef& r = p.resources[(size_t)color_res];
        if (r.kind == RK_VALUE && r.value_type == VT_FLOAT4)
            for (int i = 0; i < 4; i++) c.clear_color[i] = r.fval[i];
    }

    int depth_res = find_res(p, c.clear_depth_source);
    if (depth_res >= 0) {
        const ResourceDef& r = p.resources[(size_t)depth_res];
        if (r.kind == RK_VALUE && r.value_type == VT_FLOAT)
            c.depth_clear = r.fval[0] < 0.0f ? 0.0f : (r.fval[0] > 1.0f ? 1.0f : r.fval[0]);
    }

    int color_user = clear_color_user ? *clear_color_user : -1;
    if (color_user >= 0 && color_user < (int)p.user_vars.size()) {
        const UserVarDef& u = p.user_vars[(size_t)color_user];
        for (int i = 0; i < 4; i++) c.clear_color[i] = u.fval[i];
    }

    int depth_user = clear_depth_user ? *clear_depth_user : -1;
    if (depth_user >= 0 && depth_user < (int)p.user_vars.size()) {
        const UserVarDef& u = p.user_vars[(size_t)depth_user];
        c.depth_clear = u.fval[0] < 0.0f ? 0.0f : (u.fval[0] > 1.0f ? 1.0f : u.fval[0]);
    }
}

// The primitive IDs below are mirrored by init_prims() in the generated runtime.
// They let normal mesh-style shaders keep using POSITION/NORMAL/TEXCOORD input
// without storing any model files.
static int primitive_mesh_kind(const std::string& prim) {
    if (prim == "cube") return 1;
    if (prim == "quad") return 2;
    if (prim == "tetrahedron") return 3;
    if (prim == "sphere") return 4;
    if (prim == "fullscreen_triangle") return 5;
    return 0;
}

static int primitive_vertex_count(const std::string& prim) {
    if (prim == "fullscreen_triangle") return 3;
    if (prim == "cube") return 36;
    if (prim == "quad") return 6;
    if (prim == "tetrahedron") return 12;
    if (prim == "sphere") return 2880; // 16 rings * 32 segments, expanded triangle list
    return 0;
}

static void emit_float_literal(FILE* f, float v) {
    if (!(v == v)) { fprintf(f, "0.0f"); return; }
    char b[64];
    std::snprintf(b, sizeof(b), "%.9g", v);
    bool has_dot_or_exp = false;
    for (const char* p = b; *p; ++p) {
        if (*p == '.' || *p == 'e' || *p == 'E') { has_dot_or_exp = true; break; }
    }
    if (has_dot_or_exp) fprintf(f, "%sf", b);
    else fprintf(f, "%s.0f", b);
}

static void emit_float_array(FILE* f, const float* v, int n) {
    for (int i = 0; i < n; i++) {
        if (i) fprintf(f, ",");
        emit_float_literal(f, v[i]);
    }
}
static void emit_int_array(FILE* f, const int* v, int n) {
    for (int i = 0; i < n; i++) {
        if (i) fprintf(f, ",");
        fprintf(f, "%d", v[i]);
    }
}


static void append_indent(std::string& out, int indent) {
    for (int i = 0; i < indent; i++) out += "    ";
}

static void trim_trailing_spaces(std::string& out) {
    while (!out.empty() && (out[out.size() - 1] == ' ' || out[out.size() - 1] == '\t')) out.resize(out.size() - 1);
}

static void formatted_newline(std::string& out, int indent) {
    trim_trailing_spaces(out);
    if (!out.empty() && out[out.size() - 1] != '\n') out += '\n';
    if (!out.empty()) append_indent(out, indent);
}

static bool last_is_space_or_nl(const std::string& out) {
    return out.empty() || out[out.size() - 1] == ' ' || out[out.size() - 1] == '\n' || out[out.size() - 1] == '\t';
}


static void replace_all_inplace(std::string& s, const std::string& a, const std::string& b) {
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) {
        s.replace(p, a.size(), b);
        p += b.size();
    }
}

static std::string pretty_c_source(const std::string& in) {
    std::string out;
    out.reserve(in.size() + in.size() / 8);
    int indent = 0;
    int paren = 0;
    bool in_str = false;
    bool in_chr = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_preproc = false;
    bool esc = false;
    bool bol = true;

    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        char n = (i + 1 < in.size()) ? in[i + 1] : 0;

        if (in_line_comment) {
            out += c;
            if (c == '\n') { in_line_comment = false; bol = true; append_indent(out, indent); }
            continue;
        }
        if (in_block_comment) {
            out += c;
            if (c == '*' && n == '/') { out += n; i++; in_block_comment = false; }
            continue;
        }
        if (in_str) {
            out += c;
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            if (c == '\n') { bol = true; append_indent(out, indent); }
            else bol = false;
            continue;
        }
        if (in_chr) {
            out += c;
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '\'') in_chr = false;
            if (c == '\n') { bol = true; append_indent(out, indent); }
            else bol = false;
            continue;
        }
        if (in_preproc) {
            out += c;
            if (c == '\n') { in_preproc = false; bol = true; append_indent(out, indent); }
            continue;
        }

        if (c == '/' && n == '/') { if (bol) append_indent(out, indent); out += c; out += n; i++; in_line_comment = true; bol = false; continue; }
        if (c == '/' && n == '*') { if (bol) append_indent(out, indent); out += c; out += n; i++; in_block_comment = true; bol = false; continue; }
        if (c == '"') { if (bol) append_indent(out, indent); out += c; in_str = true; bol = false; continue; }
        if (c == '\'') { if (bol) append_indent(out, indent); out += c; in_chr = true; bol = false; continue; }
        if (c == '#' && bol) { out += c; in_preproc = true; bol = false; continue; }

        if (c == '\r') continue;
        if (c == '\n') {
            trim_trailing_spaces(out);
            if (!out.empty() && out[out.size() - 1] != '\n') out += '\n';
            bol = true;
            continue;
        }
        if (std::isspace((unsigned char)c)) {
            if (!bol && !last_is_space_or_nl(out)) out += ' ';
            continue;
        }

        if (c == '(') { if (bol) append_indent(out, indent); paren++; out += c; bol = false; continue; }
        if (c == ')') { if (paren > 0) paren--; out += c; bol = false; continue; }

        if (c == '{') {
            if (bol) append_indent(out, indent);
            else if (!last_is_space_or_nl(out)) out += ' ';
            out += '{';
            indent++;
            formatted_newline(out, indent);
            bol = false;
            continue;
        }
        if (c == '}') {
            trim_trailing_spaces(out);
            if (!out.empty() && out[out.size() - 1] != '\n') out += '\n';
            if (indent > 0) indent--;
            append_indent(out, indent);
            out += '}';
            size_t j = i + 1;
            while (j < in.size() && (in[j] == ' ' || in[j] == '\t' || in[j] == '\r' || in[j] == '\n')) j++;
            if (j < in.size() && (in[j] == ';' || in[j] == ',')) {
                /* keep }; and }, compact */
            } else {
                formatted_newline(out, indent);
            }
            bol = false;
            continue;
        }
        if (c == ';' && paren == 0) {
            out += ';';
            formatted_newline(out, indent);
            bol = false;
            continue;
        }
        if (c == ',') {
            out += c;
            if (n && n != '\n' && n != ' ' && n != '\t' && n != '}') out += ' ';
            bol = false;
            continue;
        }

        if (bol) append_indent(out, indent);
        out += c;
        bol = false;
    }
    trim_trailing_spaces(out);
    out += '\n';
    return out;
}

static void pretty_format_c_file(const std::string& path) {
    std::string src = read_text_file(path);
    if (src.empty()) return;
    std::string pretty = pretty_c_source(src);
    const char* named_structs[] = { "M4", "SceneCB", "UserCB", "ObjectCB", "Sh", "Rtd", "Cmd", "Par", "Uv", "Key", "Tr", "Q" };
    for (size_t i = 0; i < sizeof(named_structs) / sizeof(named_structs[0]); i++) {
        std::string a = "}\n";
        a += named_structs[i];
        a += ";";
        std::string b = "} ";
        b += named_structs[i];
        b += ";";
        replace_all_inplace(pretty, a, b);
    }
    replace_all_inplace(pretty, "}\nu;", "} u;");
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out << pretty;
}


// Built-in primitive shadow shader.
//
// Why this exists:
// The editor can render shadows with an explicit shadow shader. Small procedural
// test scenes often do not assign one, because the normal material shader is
// enough for the visible pass. Reusing that material VS for the shadow pass is
// incorrect: it normally writes camera ViewProj clip space, not light
// ShadowViewProj clip space. This tiny fallback VS uses the same primitive input
// layout as the generated cube/quad/sphere buffers and writes the light-space
// position expected by the shadow atlas.
//
// For draw_source procedural commands that synthesize geometry from SV_VertexID,
// the exporter still needs either an explicit shadow_shader or a material VS that
// deliberately switches to ShadowViewProj in the shadow pass. There is no generic
// way to reconstruct arbitrary procedural positions here without calling the
// user's own shader code.
static std::string builtin_primitive_shadow_hlsl() {
    return
        "cbuffer SceneCB:register(b0){"
        "float4x4 ViewProj;float4 TimeVec;float4 LightDir;float4 LightColor;float4 CamPos;"
        "float4x4 ShadowViewProj;float4x4 InvViewProj;float4x4 PrevViewProj;float4x4 PrevInvViewProj;"
        "float4x4 PrevShadowViewProj;float4 CamDir;float4 ShadowCascadeSplits;float4 ShadowParams;"
        "float4 ShadowCascadeRects[4];float4x4 ShadowCascadeViewProj[4];};"
        "cbuffer ObjectCB:register(b1){float4x4 World;};"
        "struct VSIn{float3 pos:POSITION;float3 nor:NORMAL;float2 uv:TEXCOORD0;};"
        "struct VSOut{float4 pos:SV_POSITION;};"
        "VSOut VSMain(VSIn v){VSOut o;float4 w=mul(World,float4(v.pos,1));o.pos=mul(ShadowViewProj,w);return o;}"
        "float4 PSMain(VSOut i):SV_Target{return 0;}";
}

// Emit the standalone player C file.
//
// The exporter performs the main reduction here:
//   1. Embed only shader source text.
//   2. Keep only procedural/primitive draw calls.
//   3. Convert resource references to small integer codes.
//   4. Flatten command parameters and timeline keys into compact arrays.
//   5. Leave D3D11 object creation to the generated runtime.
static void emit_generated_c(const Project& p, const std::string& lt_path, const std::string& out_c) {
    std::string lt_dir = path_dir(lt_path);
    std::vector<std::string> roots;
    roots.push_back(".");
    roots.push_back(lt_dir);
    roots.push_back(path_join(lt_dir, ".."));
    roots.push_back(path_join(lt_dir, "../.."));

    // Resource discovery pass.
    //
    // Shaders are the only path-based assets embedded by this exporter, because
    // source text compresses well and keeps the procedural scene self-contained.
    // Render textures are represented as descriptors. Mesh and texture files are
    // reported but intentionally ignored.
    std::vector<int> shader_res_indices;
    std::vector<std::string> shader_sources;
    std::vector<std::string> shader_names;
    std::vector<size_t> shader_raw_sizes;
    std::vector<size_t> shader_min_sizes;
    std::vector<int> rt_res_indices;
    for (size_t i = 0; i < p.resources.size(); i++) {
        const ResourceDef& r = p.resources[i];
        if (r.kind == RK_SHADER_VSPS) {
            std::string sp = find_file_for_path(r.path, roots);
            if (sp.empty()) die("cannot find shader '%s' for resource '%s'", r.path.c_str(), r.name.c_str());
            std::string raw_src = expand_hlsl_includes(sp, roots);
            // Prefer visual parity with the editor over source-level string squeezing.
            // The generated EXE will still be handled by the normal linker/UPX path,
            // but the HLSL sent to D3DCompile is now the editor-expanded source unless
            // LT64K_MINIFY_HLSL is explicitly enabled while compiling build64k.exe.
            std::string min_src = LT64K_MINIFY_HLSL ? minify_hlsl_for_64k(raw_src) : raw_src;
            shader_res_indices.push_back((int)i);
            shader_names.push_back(r.name);
            shader_raw_sizes.push_back(raw_src.size());
            shader_min_sizes.push_back(min_src.size());
            shader_sources.push_back(min_src);
        } else if (r.kind == RK_RT) {
            rt_res_indices.push_back((int)i);
        } else if (r.kind == RK_RT3D) {
            warnf("resource '%s': render_texture3d is parsed, but the current 64k VS/PS player only instantiates 2D render targets", r.name.c_str());
        } else if (r.kind == RK_TEXTURE || r.kind == RK_MESH_FILE || r.kind == RK_GAUSSIAN_SPLAT) {
            warnf("resource '%s' is not part of the 64k path (%s)", r.name.c_str(), r.path.c_str());
        }
    }
    if (shader_sources.empty()) die("no shader_vsps resources found");

    bool project_uses_shadows = false;
    for (size_t i = 0; i < p.commands.size(); i++) {
        if (p.commands[i].shadow_cast) { project_uses_shadows = true; break; }
    }
    int builtin_shadow_shader_index = -1;
    if (project_uses_shadows) {
        // Appended after user shaders so existing resource indexes remain stable.
        // Commands opt into this shader with CommandDef::builtin_shadow.
        std::string sh = builtin_primitive_shadow_hlsl();
        builtin_shadow_shader_index = (int)shader_sources.size();
        shader_res_indices.push_back(-1);
        shader_names.push_back("__lt_builtin_primitive_shadow");
        shader_raw_sizes.push_back(sh.size());
        shader_min_sizes.push_back(sh.size());
        shader_sources.push_back(sh);
    }

    int project_w = 1280;
    int project_h = 720;
    for (size_t i = 0; i < rt_res_indices.size(); i++) {
        const ResourceDef& r = p.resources[(size_t)rt_res_indices[i]];
        if (r.ival[0] > 0 && r.ival[1] > 0 && r.ival[7] == 1) {
            project_w = r.ival[0];
            project_h = r.ival[1];
            break;
        }
    }
    if (project_w == 1280 && project_h == 720) {
        for (size_t i = 0; i < rt_res_indices.size(); i++) {
            const ResourceDef& r = p.resources[(size_t)rt_res_indices[i]];
            if (r.ival[0] > 0 && r.ival[1] > 0) {
                int div = r.ival[7] <= 0 ? 1 : r.ival[7];
                project_w = r.ival[0] * div;
                project_h = r.ival[1] * div;
                break;
            }
        }
    }

    // Command filtering pass.
    //
    // The editor project may contain many feature types. For the procedural player
    // we keep commands only when every dependency can be regenerated at runtime.
    // This makes failure modes explicit: unsupported commands are skipped with a
    // warning instead of silently creating a broken executable.
    std::vector<CommandDef> out_cmds;
    std::vector<char> emitted(p.commands.size(), 0);

    auto texture_refs_ok = [&](const CommandDef& c) -> bool {
        for (size_t t = 0; t < c.textures.size(); t++) {
            int code = texture_source_code(p, c.textures[t].name, rt_res_indices);
            if (code == -1 && !c.textures[t].name.empty() && c.textures[t].name != "-") {
                warnf("skipping draw '%s': texture '%s' is not an internal render target/builtin 64k source", c.name.c_str(), c.textures[t].name.c_str());
                return false;
            }
        }
        for (size_t t = 0; t < c.srvs.size(); t++) {
            int code = texture_source_code(p, c.srvs[t].name, rt_res_indices);
            if (code == -1 && !c.srvs[t].name.empty() && c.srvs[t].name != "-") {
                warnf("skipping draw '%s': SRV '%s' is not an internal render target/builtin 64k source", c.name.c_str(), c.srvs[t].name.c_str());
                return false;
            }
        }
        return true;
    };

    auto export_one = [&](CommandDef c) -> bool {
        if (c.type == CT_CLEAR) { out_cmds.push_back(c); return true; }
        if (c.type == CT_DISPATCH) { warnf("skipping compute '%s': compute/UAV export is not in this tiny VS/PS player yet", c.name.c_str()); return false; }
        if (!(c.type == CT_DRAW || c.type == CT_DRAW_INSTANCED)) return false;
        if (c.unsupported_bindings) { warnf("skipping draw '%s': UAV/unsupported bindings are not exported by this VS/PS 64k player", c.name.c_str()); return false; }
        if (!texture_refs_ok(c)) return false;
        int sh = shader_resource_index(p, c.shader, shader_res_indices);
        if (sh < 0) { warnf("skipping draw '%s': shader not found/supported", c.name.c_str()); return false; }

        bool ok_proc = c.procedural;
        c.mesh_kind = 0;
        if (!c.mesh.empty()) {
            int mr = find_res(p, c.mesh);
            if (mr >= 0 && p.resources[mr].kind == RK_MESH_PRIMITIVE) {
                const std::string& prim = p.resources[mr].primitive;
                int vc = primitive_vertex_count(prim);
                if (vc > 0) {
                    ok_proc = true;
                    c.vertex_count = vc;
                    c.topology = 1;
                    c.mesh_kind = primitive_mesh_kind(prim);
                }
            } else if (mr >= 0 && p.resources[mr].kind == RK_MESH_FILE) {
                warnf("skipping draw '%s': mesh resource '%s' is external geometry, not a 64k procedural primitive", c.name.c_str(), c.mesh.c_str());
                return false;
            }
        }
        if (!ok_proc) {
            warnf("skipping draw '%s': draw uses no supported 64k procedural source (use draw_source procedural or resource mesh_primitive)", c.name.c_str());
            return false;
        }

        if (c.shadow_cast) {
            int shadow_sh = shader_resource_index(p, c.shadow_shader, shader_res_indices);
            if (shadow_sh < 0 && c.mesh_kind > 0 && builtin_shadow_shader_index >= 0) {
                // Primitive meshes can use the generated fallback safely: their
                // positions come from the known built-in vertex buffers. This fixes
                // test2.lt and similar scenes without needing a separate shadow shader.
                warnf("draw '%s': no supported shadow_shader was set; using generated primitive shadow VS", c.name.c_str());
                c.builtin_shadow = true;
                shadow_sh = builtin_shadow_shader_index;
            } else if (shadow_sh < 0 && sh >= 0) {
                // Last-resort behavior for draw_source procedural. It is only correct
                // if the user's VS itself writes ShadowViewProj-compatible positions
                // during the shadow pass; otherwise the exporter prints this warning.
                warnf("draw '%s': no shadow_shader and no primitive fallback; reusing main shader '%s' for the shadow pass", c.name.c_str(), c.shader.c_str());
                c.shadow_shader = c.shader;
                shadow_sh = sh;
            }
            if (shadow_sh < 0) {
                warnf("draw '%s': shadow_cast is enabled but no supported shadow shader is available; this caster will be disabled", c.name.c_str());
                c.shadow_cast = false;
            }
        }
        out_cmds.push_back(c);
        return true;
    };

    std::function<void(size_t)> emit_idx = [&](size_t idx) {
        if (idx >= p.commands.size() || emitted[idx]) return;
        const CommandDef& c = p.commands[idx];
        emitted[idx] = 1;
        if (c.type == CT_REPEAT) {
            int reps = std::max(1, c.repeat_count);
            for (int r = 0; r < reps; r++) {
                for (size_t j = 0; j < p.commands.size(); j++) {
                    if (p.commands[j].parent == c.name) {
                        CommandDef child = p.commands[j];
                        export_one(child);
                        emitted[j] = 1;
                    }
                }
            }
            return;
        }
        if (!c.parent.empty()) {
            int pi = find_cmd(p, c.parent);
            if (pi >= 0 && p.commands[(size_t)pi].type == CT_REPEAT) return;
        }
        export_one(c);
    };

    for (size_t i = 0; i < p.commands.size(); i++) emit_idx(i);
    if (out_cmds.empty()) warnf("the exported player has no render commands");

    std::map<std::string,int> cmd_out_index;
    for (size_t i = 0; i < out_cmds.size(); i++) cmd_out_index[out_cmds[i].name] = (int)i;

    std::vector<int> cmd_clear_color_user(out_cmds.size(), -1);
    std::vector<int> cmd_clear_depth_user(out_cmds.size(), -1);
    for (size_t i = 0; i < out_cmds.size(); i++) {
        resolve_clear_sources_for_64k(p, out_cmds[i], &cmd_clear_color_user[i], &cmd_clear_depth_user[i]);
        if (!out_cmds[i].clear_color_source.empty() && cmd_clear_color_user[i] < 0) {
            int ri = find_res(p, out_cmds[i].clear_color_source);
            if (ri < 0 || p.resources[(size_t)ri].kind != RK_VALUE || p.resources[(size_t)ri].value_type != VT_FLOAT4)
                warnf("clear '%s': color source '%s' is not a float4 value/UserCB exported to 64k; using baked clear color", out_cmds[i].name.c_str(), out_cmds[i].clear_color_source.c_str());
        }
        if (!out_cmds[i].clear_depth_source.empty() && cmd_clear_depth_user[i] < 0) {
            int ri = find_res(p, out_cmds[i].clear_depth_source);
            if (ri < 0 || p.resources[(size_t)ri].kind != RK_VALUE || p.resources[(size_t)ri].value_type != VT_FLOAT)
                warnf("clear '%s': depth source '%s' is not a float value/UserCB exported to 64k; using baked clear depth", out_cmds[i].name.c_str(), out_cmds[i].clear_depth_source.c_str());
        }
    }

    // Timeline packing pass.
    //
    // A naive export would store TimelineKeyDef for every key, which reserves both
    // int[4] and float[16] even when a track is a single float. Instead, each track
    // records how many components it owns and points into one of two typed streams.
    // This preserves all useful timeline features while avoiding the worst bloat.
    struct FlatTrack { int kind, target, type, integral, frame_start, data_start, count, comp, enabled; };
    std::vector<FlatTrack> flat_tracks;
    std::vector<int> flat_key_frames;
    std::vector<int> flat_key_ints;
    std::vector<float> flat_key_floats;
    for (size_t i = 0; i < p.tracks.size(); i++) {
        const TimelineTrackDef& tr = p.tracks[i];
        if (!tr.enabled || tr.keys.empty()) continue;
        FlatTrack ft = {};
        ft.kind = (int)tr.kind;
        ft.type = (int)tr.type;
        ft.integral = (val_integral(tr.type) || tr.kind == TK_COMMAND_ENABLED) ? 1 : 0;
        ft.frame_start = (int)flat_key_frames.size();
        ft.count = (int)tr.keys.size();
        ft.enabled = tr.enabled ? 1 : 0;
        ft.comp = timeline_value_count(tr.kind, tr.type);
        if (ft.comp <= 0) continue;
        ft.target = -1;
        if (tr.kind == TK_COMMAND_TRANSFORM || tr.kind == TK_COMMAND_ENABLED) {
            std::map<std::string,int>::iterator it = cmd_out_index.find(tr.target);
            if (it == cmd_out_index.end()) continue;
            ft.target = it->second;
        } else if (tr.kind == TK_USER_VAR) {
            ft.target = find_user_var(p, tr.target);
            if (ft.target < 0) continue;
        }
        ft.data_start = ft.integral ? (int)flat_key_ints.size() : (int)flat_key_floats.size();
        for (size_t k = 0; k < tr.keys.size(); k++) {
            const TimelineKeyDef& tk = tr.keys[k];
            flat_key_frames.push_back(tk.frame);
            if (ft.integral) {
                for (int c = 0; c < ft.comp; c++) flat_key_ints.push_back(c < 4 ? tk.ival[c] : 0);
            } else {
                for (int c = 0; c < ft.comp; c++) flat_key_floats.push_back(c < 16 ? tk.fval[c] : 0.0f);
            }
        }
        flat_tracks.push_back(ft);
    }

    // Command parameters are stored sequentially and referenced by each command.
    // The shader reads them through UserCB slots 0..N, matching the editor model.
    struct FlatParam { int type, enabled, source_kind, source_cmd; int iv[4]; float fv[4]; };
    std::vector<FlatParam> flat_params;
    std::vector<int> cmd_param_start(out_cmds.size(), 0);
    std::vector<int> cmd_param_count(out_cmds.size(), 0);
    for (size_t ci = 0; ci < out_cmds.size(); ci++) {
        cmd_param_start[ci] = (int)flat_params.size();
        for (size_t pi = 0; pi < out_cmds[ci].params.size(); pi++) {
            const ParamDef& pp = out_cmds[ci].params[pi];
            if (!pp.enabled) continue;
            FlatParam fp = {};
            fp.type = (int)pp.type;
            fp.enabled = pp.enabled ? 1 : 0;
            fp.source_kind = (int)pp.source_kind;
            fp.source_cmd = -1;
            for (int k = 0; k < 4; k++) { fp.iv[k] = pp.ival[k]; fp.fv[k] = pp.fval[k]; }
            if (pp.source_kind == SRC_RESOURCE && !pp.source_target.empty()) {
                int ri = find_res(p, pp.source_target);
                if (ri >= 0 && p.resources[(size_t)ri].kind == RK_VALUE && p.resources[(size_t)ri].value_type == pp.type) {
                    for (int k = 0; k < 4; k++) { fp.iv[k] = p.resources[(size_t)ri].ival[k]; fp.fv[k] = p.resources[(size_t)ri].fval[k]; }
                    fp.source_kind = SRC_NONE;
                } else {
                    warnf("param '%s' on draw '%s': resource source '%s' is not a matching value resource; using baked value", pp.name.c_str(), out_cmds[ci].name.c_str(), pp.source_target.c_str());
                    fp.source_kind = SRC_NONE;
                }
            } else if (pp.source_kind == SRC_CMD_POS || pp.source_kind == SRC_CMD_ROT || pp.source_kind == SRC_CMD_SCALE) {
                if (pp.source_kind == SRC_CMD_ROT && pp.type != VT_FLOAT4) {
                    warnf("param '%s' on draw '%s': cmd_rot source exports as float4 quaternion in 64k; using baked value", pp.name.c_str(), out_cmds[ci].name.c_str());
                    fp.source_kind = SRC_NONE;
                } else {
                    std::map<std::string,int>::iterator it = cmd_out_index.find(pp.source_target);
                    if (it != cmd_out_index.end()) fp.source_cmd = it->second;
                    else {
                        warnf("param '%s' on draw '%s': source command '%s' is not exported to 64k; using baked value", pp.name.c_str(), out_cmds[ci].name.c_str(), pp.source_target.c_str());
                        fp.source_kind = SRC_NONE;
                    }
                }
            }
            flat_params.push_back(fp);
        }
        cmd_param_count[ci] = (int)flat_params.size() - cmd_param_start[ci];
    }


    FILE* f = std::fopen(out_c.c_str(), "wb");
    if (!f) die("cannot write %s", out_c.c_str());

    fprintf(f, "// generated by lazyTool build64k.cpp from %s\n", lt_path.c_str());
    fprintf(f, "//\n");
    fprintf(f, "// This file is a procedural-only standalone player. It contains no model files,\n");
    fprintf(f, "// no texture files and no editor UI. Geometry is either produced by the shader\n");
    fprintf(f, "// from SV_VertexID or by the tiny built-in primitive vertex buffers below.\n");
    fprintf(f, "// Shadows, render targets, value-backed parameters, scene-source parameters and timeline animation are kept.\n\n");
    fprintf(f, "#define WIN32_LEAN_AND_MEAN\n#define COBJMACROS\n#include <windows.h>\n#include <stddef.h>\n#include <d3d11.h>\n#include <d3dcompiler.h>\n\n");
    fprintf(f, "#define LT_WNDCLS \"lt64k_window\"\n#define LT_PI 3.14159265358979323846f\n");
    int shadow_tex_w = (int)(p.dirlight[10] > 16.0f ? p.dirlight[10] : 16.0f);
    int shadow_tex_h = (int)(p.dirlight[11] > 16.0f ? p.dirlight[11] : 16.0f);
    // The 64k player is now intentionally fullscreen-only.  This removes the
    // ambiguous split between "window size", "internal size" and "project size":
    //
    //   W/H   = real fullscreen monitor/backbuffer size
    //   RW/RH = scene size used for camera projection, scene depth, builtin
    //           scene color, and scene-scaled render textures
    //
    // Therefore RW/RH always equals W/H.  This matches the normal exported player
    // when it is run fullscreen: camera aspect, scene depth, scene-scaled post
    // targets and final backbuffer all describe the same image.  No letterboxing,
    // no stretching, and no hidden low-resolution render path.
    //
    // Only explicitly fixed render textures keep their authored size. In .lt files
    // this is represented by scene_scale_divisor == 0. Render textures with
    // scene_scale_divisor > 0 are derived from RW/RH, just like in the editor.
    fprintf(f, "// -----------------------------------------------------------------------------\n");
    fprintf(f, "// Build-time options. This generated player assumes fullscreen borderless mode.\n");
    fprintf(f, "// W/H and RW/RH are always the fullscreen monitor/backbuffer size. That keeps\n");
    fprintf(f, "// the projection aspect, scene depth, scene-scaled render targets, and final\n");
    fprintf(f, "// output in the same resolution space. Fixed custom RTs still keep the size\n");
    fprintf(f, "// authored in the .lt file when scene_scale_divisor == 0.\n");
    fprintf(f, "//\n");
    fprintf(f, "// Useful overrides through LT64K_CFLAGS:\n");
    fprintf(f, "//   /DLT_VSYNC=1                        Present with VSync for smoother pacing.\n");
    fprintf(f, "//   /DLT_DEBUG_FPS=1                    Compile the tiny GDI FPS overlay.\n");
    fprintf(f, "// -----------------------------------------------------------------------------\n");
    fprintf(f, "#define LT_PROJECT_W %d\n#define LT_PROJECT_H %d\n#define LT_SHADOW_W %d\n#define LT_SHADOW_H %d\n#ifndef LT_VSYNC\n#define LT_VSYNC 0\n#endif\n#ifndef LT_DEBUG_FPS\n#define LT_DEBUG_FPS 0\n#endif\n", project_w, project_h, shadow_tex_w, shadow_tex_h);
    fprintf(f, "// Minimal CRT replacements and tiny math types. /NODEFAULTLIB builds need these.\n");
    fprintf(f, "int _fltused=0; typedef unsigned int u32; typedef struct { float m[16]; } M4;\n");
    fprintf(f, "void* __cdecl memset(void* d,int c,size_t n){ unsigned char* p=(unsigned char*)d; while(n--)*p++=(unsigned char)c; return d; }\n");
    fprintf(f, "void* __cdecl memcpy(void* d,const void* s,size_t n){ unsigned char* p=(unsigned char*)d; const unsigned char* q=(const unsigned char*)s; while(n--)*p++=*q++; return d; }\n");
    fprintf(f, "// Constant buffers shared with the HLSL files. Keep layout in sync with shaders.\n");
    fprintf(f, "typedef struct { float view_proj[16]; float time_vec[4]; float light_dir[4]; float light_color[4]; float cam_pos[4]; float shadow_view_proj[16]; float inv_view_proj[16]; float prev_view_proj[16]; float prev_inv_view_proj[16]; float prev_shadow_view_proj[16]; float cam_dir[4]; float shadow_cascade_splits[4]; float shadow_params[4]; float shadow_cascade_rects[4][4]; float shadow_cascade_view_proj[4][16]; } SceneCB;\n");
    fprintf(f, "typedef union { float f[64][4]; int i[64][4]; } UserCB; typedef struct { float world[16]; } ObjectCB;\n");
    fprintf(f, "// Global D3D handles and runtime sizes. This player is fullscreen-only, so\n");
    fprintf(f, "// W/H are the monitor/backbuffer size and RW/RH intentionally equal W/H.\n");
    fprintf(f, "// Camera projection, scene depth and scene-scaled render textures all use\n");
    fprintf(f, "// this fullscreen size. Only fixed custom RTs keep their authored dimensions.\n");
    fprintf(f, "static ID3D11Device* dev; static ID3D11DeviceContext* ctx; static IDXGISwapChain* swp; static ID3D11RenderTargetView* rtv; static ID3D11Texture2D* depth_tex; static ID3D11DepthStencilView* dsv; static ID3D11ShaderResourceView* depth_srv; static ID3D11Texture2D* shadow_tex; static ID3D11DepthStencilView* shadow_dsv; static ID3D11ShaderResourceView* shadow_srv; static ID3D11SamplerState* smp_lin; static ID3D11SamplerState* smp_cmp; static ID3D11DepthStencilState* ds[4]; static ID3D11RasterizerState* rs[2]; static ID3D11BlendState* bs[4]; static ID3D11Buffer* scene_cb; static ID3D11Buffer* object_cb; static ID3D11Buffer* user_cb; static ID3D11Buffer* prim_vb[6]; static HWND wh; static int W,H,RW,RH,dbg_on=LT_DEBUG_FPS;\n");
    fprintf(f, "typedef struct { const char* src; unsigned int len; ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11InputLayout* il; } Sh;\n");
    fprintf(f, "typedef struct { int w,h,fmt,rtv,srv,uav,dsv,div; } Rtd;\n");

    fprintf(f, "// Embedded shader source. It is compiled at startup so the player does not\n");
    fprintf(f, "// need precompiled bytecode or any external shader files.\n");
    for (size_t i = 0; i < shader_sources.size(); i++) {
        fprintf(f, "static const char sh_src_%u[] =\n%s;\n\n", (unsigned)i, cstr_literal(shader_sources[i]).c_str());
    }
    fprintf(f, "static Sh sh[%u] = {\n", (unsigned)shader_sources.size());
    for (size_t i = 0; i < shader_sources.size(); i++) fprintf(f, " { sh_src_%u, sizeof(sh_src_%u)-1, 0, 0, 0 },\n", (unsigned)i, (unsigned)i);
    fprintf(f, "};\n");

    fprintf(f, "// Render texture descriptors copied from the project. Width/height 0 means\n");
    fprintf(f, "// use the internal render size RW/RH; div is used by half/quarter-resolution passes.\n");
    fprintf(f, "static Rtd rtd[%u] = {\n", (unsigned)(rt_res_indices.empty() ? 1 : rt_res_indices.size()));
    if (rt_res_indices.empty()) fprintf(f, " {1,1,28,0,0,0,0,0},\n");
    for (size_t i = 0; i < rt_res_indices.size(); i++) {
        const ResourceDef& r = p.resources[(size_t)rt_res_indices[i]];
        fprintf(f, " {%d,%d,%d,%d,%d,%d,%d,%d},\n", r.ival[0], r.ival[1], r.ival[2], r.ival[3], r.ival[4], r.ival[5], r.ival[6], r.ival[7]);
    }
    fprintf(f, "};\n");
    fprintf(f, "static ID3D11Texture2D* rt_tex[%u]; static ID3D11RenderTargetView* rt_rtv[%u]; static ID3D11ShaderResourceView* rt_srv[%u];\n", (unsigned)(rt_res_indices.empty() ? 1 : rt_res_indices.size()), (unsigned)(rt_res_indices.empty() ? 1 : rt_res_indices.size()), (unsigned)(rt_res_indices.empty() ? 1 : rt_res_indices.size()));

    fprintf(f, "// Flattened render commands. Resource names are gone here; everything is a\n");
    fprintf(f, "// small integer code so the runtime can be straightforward and deterministic.\n");
    fprintf(f, "typedef struct { int type,enabled,shader,shs,topology,mk,vc,ic,ccen,cden,ccs,dcs,rt,dep,dt,dw,ab,cb,cw,scast,srecv,pst,pc,tc,sc; int tex[8],tsl[8],srv[8],ssl[8]; float cc[4],dc,pos[3],q[4],scl[3]; } Cmd;\n");
    fprintf(f, "static Cmd cmd[%u] = {\n", (unsigned)(out_cmds.empty() ? 1 : out_cmds.size()));
    if (out_cmds.empty()) fprintf(f, " {0,0,-1,-1,1,0,0,0,0,0,-1,-1,-1,-1,0,0,0,0,1,0,0,0,0,0,0,{-1,-1,-1,-1,-1,-1,-1,-1},{0,0,0,0,0,0,0,0},{-1,-1,-1,-1,-1,-1,-1,-1},{0,0,0,0,0,0,0,0},{0.0f,0.0f,0.0f,1.0f},1.0f,{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f,1.0f},{1.0f,1.0f,1.0f}},\n");
    for (size_t i = 0; i < out_cmds.size(); i++) {
        const CommandDef& c = out_cmds[i];
        int shidx = shader_resource_index(p, c.shader, shader_res_indices);
        int shadow_shidx = c.builtin_shadow ? builtin_shadow_shader_index : shader_resource_index(p, c.shadow_shader, shader_res_indices);
        int rtcode = rt_target_code(p, c.rt, rt_res_indices);
        int depcode = depth_target_code(c.depth);
        int tex_src[8]; int tex_slot[8]; int srv_src[8]; int srv_slot[8];
        for (int k = 0; k < 8; k++) { tex_src[k] = -1; tex_slot[k] = 0; srv_src[k] = -1; srv_slot[k] = 0; }
        int texc = (int)std::min<size_t>(8, c.textures.size());
        int srvc = (int)std::min<size_t>(8, c.srvs.size());
        for (int k = 0; k < texc; k++) { tex_src[k] = texture_source_code(p, c.textures[(size_t)k].name, rt_res_indices); tex_slot[k] = c.textures[(size_t)k].slot; }
        for (int k = 0; k < srvc; k++) { srv_src[k] = texture_source_code(p, c.srvs[(size_t)k].name, rt_res_indices); srv_slot[k] = c.srvs[(size_t)k].slot; }
        fprintf(f, " {%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,",
            (c.type == CT_CLEAR ? 1 : 2), c.enabled?1:0, shidx, shadow_shidx, c.topology, c.mesh_kind, c.vertex_count, c.instance_count, c.clear_color_enabled?1:0, c.clear_depth?1:0,
            cmd_clear_color_user[i], cmd_clear_depth_user[i], rtcode, depcode, c.depth_test?1:0, c.depth_write?1:0, c.alpha_blend?1:0, c.cull_back?1:0, c.color_write?1:0, c.shadow_cast?1:0, c.shadow_receive?1:0, cmd_param_start[i], cmd_param_count[i], texc, srvc);
        fprintf(f, "{"); emit_int_array(f, tex_src, 8); fprintf(f, "},{"); emit_int_array(f, tex_slot, 8); fprintf(f, "},{"); emit_int_array(f, srv_src, 8); fprintf(f, "},{"); emit_int_array(f, srv_slot, 8); fprintf(f, "},{");
        emit_float_array(f, c.clear_color, 4); fprintf(f, "},"); emit_float_literal(f, c.depth_clear); fprintf(f, ",{");
        emit_float_array(f, c.pos, 3); fprintf(f, "},{"); emit_float_array(f, c.rotq, 4); fprintf(f, "},{"); emit_float_array(f, c.scale, 3); fprintf(f, "}},\n");
    }
    fprintf(f, "};\n");

    fprintf(f, "typedef struct { int type,enabled,src,src_cmd; int iv[4]; float fv[4]; } Par;\n");
    fprintf(f, "static Par par[%u] = {\n", (unsigned)(flat_params.empty() ? 1 : flat_params.size()));
    if (flat_params.empty()) fprintf(f, " {0,0,0,-1,{0,0,0,0},{0.0f,0.0f,0.0f,0.0f}},\n");
    for (size_t i = 0; i < flat_params.size(); i++) {
        const FlatParam& pp = flat_params[i];
        fprintf(f, " {%d,%d,%d,%d,{", pp.type, pp.enabled, pp.source_kind, pp.source_cmd); emit_int_array(f, pp.iv, 4); fprintf(f, "},{"); emit_float_array(f, pp.fv, 4); fprintf(f, "}},\n");
    }
    fprintf(f, "};\n");

    fprintf(f, "typedef struct { int type,src,src_cmd; int iv[4]; float fv[4]; } Uv;\n");
    fprintf(f, "static Uv uv[%u] = {\n", (unsigned)(p.user_vars.empty() ? 1 : p.user_vars.size()));
    if (p.user_vars.empty()) fprintf(f, " {0,0,-1,{0,0,0,0},{0.0f,0.0f,0.0f,0.0f}},\n");
    for (size_t i = 0; i < p.user_vars.size(); i++) {
        const UserVarDef& u = p.user_vars[i];
        int source_kind = (int)u.source_kind;
        int src_cmd = -1;
        if (u.source_kind == SRC_CMD_POS || u.source_kind == SRC_CMD_ROT || u.source_kind == SRC_CMD_SCALE) {
            if (u.source_kind == SRC_CMD_ROT && u.type != VT_FLOAT4) {
                warnf("user_var '%s': cmd_rot source exports as float4 quaternion in 64k; using baked value", u.name.c_str());
                source_kind = SRC_NONE;
            } else {
                std::map<std::string,int>::iterator it = cmd_out_index.find(u.source_target);
                if (it != cmd_out_index.end()) src_cmd = it->second;
                else {
                    warnf("user_var '%s': source command '%s' is not exported to 64k; using baked value", u.name.c_str(), u.source_target.c_str());
                    source_kind = SRC_NONE;
                }
            }
        }
        fprintf(f, " {%d,%d,%d,{", (int)u.type, source_kind, src_cmd); emit_int_array(f, u.ival, 4); fprintf(f, "},{"); emit_float_array(f, u.fval, 4); fprintf(f, "}},\n");
    }
    fprintf(f, "};\n");

    fprintf(f, "// Timeline data. Frames are separate from values, and values are split into\n");
    fprintf(f, "// int and float streams so a float track does not carry unused int/quaternion data.\n");
    fprintf(f, "typedef struct { int kind,target,type,integral,fs,ds,count,comp,enabled; } Tr;\n");
    fprintf(f, "static int kfr[%u]={", (unsigned)(flat_key_frames.empty() ? 1 : flat_key_frames.size()));
    if (flat_key_frames.empty()) fprintf(f, "0");
    for (size_t i = 0; i < flat_key_frames.size(); i++) { if (i) fprintf(f, ","); fprintf(f, "%d", flat_key_frames[i]); }
    fprintf(f, "};\nstatic int ki[%u]={", (unsigned)(flat_key_ints.empty() ? 1 : flat_key_ints.size()));
    if (flat_key_ints.empty()) fprintf(f, "0");
    for (size_t i = 0; i < flat_key_ints.size(); i++) { if (i) fprintf(f, ","); fprintf(f, "%d", flat_key_ints[i]); }
    fprintf(f, "};\nstatic float kf[%u]={", (unsigned)(flat_key_floats.empty() ? 1 : flat_key_floats.size()));
    if (flat_key_floats.empty()) fprintf(f, "0.0f");
    for (size_t i = 0; i < flat_key_floats.size(); i++) { if (i) fprintf(f, ","); emit_float_literal(f, flat_key_floats[i]); }
    fprintf(f, "};\nstatic Tr tr[%u] = {\n", (unsigned)(flat_tracks.empty() ? 1 : flat_tracks.size()));
    if (flat_tracks.empty()) fprintf(f, " {0,-1,0,0,0,0,0,0,0},\n");
    for (size_t i = 0; i < flat_tracks.size(); i++) {
        const FlatTrack& t = flat_tracks[i];
        fprintf(f, " {%d,%d,%d,%d,%d,%d,%d,%d,%d},\n", t.kind,t.target,t.type,t.integral,t.frame_start,t.data_start,t.count,t.comp,t.enabled);
    }
    fprintf(f, "};\n");
    fprintf(f, "static float cam0[8]={"); emit_float_array(f, p.camera, 8); fprintf(f, "}; static float dl0[39]={"); emit_float_array(f, p.dirlight, 39); fprintf(f, "}; static float cam[8],dl[39];\n");
    fprintf(f, "enum{ CMDN=%u, SHN=%u, RTN=%u, UVN=%u, PN=%u, TRN=%u, KEYN=%u, TL_FPS=%d, TL_LEN=%d, TL_LOOP=%d, TL_ON=%d, TL_LERP=%d };\n",
            (unsigned)out_cmds.size(), (unsigned)shader_sources.size(), (unsigned)rt_res_indices.size(), (unsigned)p.user_vars.size(), (unsigned)flat_params.size(), (unsigned)flat_tracks.size(), (unsigned)flat_key_frames.size(), p.timeline_fps, p.timeline_length, p.timeline_loop?1:0, p.timeline_enabled?1:0, p.timeline_interpolate?1:0);

    // Runtime support. Written as plain C and deliberately compact.
    fputs(R"LT64K(
// -----------------------------------------------------------------------------
// Runtime helpers
// -----------------------------------------------------------------------------
// The generated runtime is kept as plain C. It avoids the CRT where practical,
// but it is not source-golfed: comments document the moving parts so the player
// can be extended later.
static void zmem(void* p, int n){ unsigned char* b=(unsigned char*)p; while(n--)*b++=0; }
static void cpy(void* d,const void* s,int n){ unsigned char* a=(unsigned char*)d; const unsigned char* b=(const unsigned char*)s; while(n--)*a++=*b++; }
static float ab(float x){ return x<0?-x:x; }
static float cl(float x,float a,float b){ return x<a?a:(x>b?b:x); }
static float wrap(float x){ while(x>LT_PI)x-=6.28318530718f; while(x<-LT_PI)x+=6.28318530718f; return x; }
static float sn(float x){ int neg=0; x=wrap(x); if(x<0){neg=1;x=-x;} if(x>1.57079632679f)x=3.14159265359f-x; float x2=x*x; float r=x*(1.0f-x2*(0.1666666667f-x2*(0.00833333333f-x2*(0.000198412698f-x2*(0.00000275573192f-x2*0.0000000250521084f))))); return neg?-r:r; }
static float cs(float x){ return sn(x+1.57079632679f); }
static float lerp(float a,float b,float t){return a+(b-a)*t;} static float lerpa(float a,float b,float t){float d=wrap(b-a);return wrap(a+d*t);}
static float rsq(float x){ union{float f; unsigned int i;} u; float xh=x*0.5f; if(x<=0) return 0; u.f=x; u.i=0x5f3759df-(u.i>>1); u.f=u.f*(1.5f-xh*u.f*u.f); u.f=u.f*(1.5f-xh*u.f*u.f); return u.f; }
static float sqt(float x){ return x<=0?0:x*rsq(x); }
typedef struct { float x,y,z,w; } Q;
static void mid(M4* m){ zmem(m,sizeof(M4)); m->m[0]=m->m[5]=m->m[10]=m->m[15]=1; }
static M4 mmul(M4 a,M4 b){ M4 r; int row,col,k; zmem(&r,sizeof(r)); for(row=0;row<4;row++)for(col=0;col<4;col++)for(k=0;k<4;k++)r.m[row*4+col]+=a.m[row*4+k]*b.m[k*4+col]; return r; }
static void rotxyz(float* rr,M4* out){ float cx=cs(rr[0]),sx=sn(rr[0]),cy=cs(rr[1]),sy=sn(rr[1]),cz=cs(rr[2]),sz=sn(rr[2]); M4 rx,ry,rz; mid(&rx);mid(&ry);mid(&rz); rx.m[5]=cx;rx.m[6]=sx;rx.m[9]=-sx;rx.m[10]=cx; ry.m[0]=cy;ry.m[2]=-sy;ry.m[8]=sy;ry.m[10]=cy; rz.m[0]=cz;rz.m[1]=sz;rz.m[4]=-sz;rz.m[5]=cz; *out=mmul(mmul(rx,ry),rz); }
static Q qn(Q q){ float l=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w; if(l<0.00000001f){q.x=q.y=q.z=0;q.w=1;return q;} float r=rsq(l); q.x*=r;q.y*=r;q.z*=r;q.w*=r; return q; }
static Q qfm(M4* m){ Q q; float m00=m->m[0],m01=m->m[1],m02=m->m[2],m10=m->m[4],m11=m->m[5],m12=m->m[6],m20=m->m[8],m21=m->m[9],m22=m->m[10],tr=m00+m11+m22,s; q.x=q.y=q.z=0;q.w=1; if(tr>0){s=sqt(tr+1.0f)*2.0f; q.w=0.25f*s; q.x=(m12-m21)/s; q.y=(m20-m02)/s; q.z=(m01-m10)/s;} else if(m00>m11&&m00>m22){s=sqt(1.0f+m00-m11-m22)*2.0f; q.w=(m12-m21)/s; q.x=0.25f*s; q.y=(m01+m10)/s; q.z=(m20+m02)/s;} else if(m11>m22){s=sqt(1.0f+m11-m00-m22)*2.0f; q.w=(m20-m02)/s; q.x=(m01+m10)/s; q.y=0.25f*s; q.z=(m12+m21)/s;} else {s=sqt(1.0f+m22-m00-m11)*2.0f; q.w=(m01-m10)/s; q.x=(m20+m02)/s; q.y=(m12+m21)/s; q.z=0.25f*s;} return qn(q); }
static Q qfe(float* r){ M4 m; rotxyz(r,&m); return qfm(&m); }
static Q qfa(float* r){ Q q; q.x=r[0];q.y=r[1];q.z=r[2];q.w=r[3]; return qn(q); }
static Q ql(Q a,Q b,float t){ Q q; float d; a=qn(a); b=qn(b); d=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w; if(d<0){b.x=-b.x;b.y=-b.y;b.z=-b.z;b.w=-b.w;} q.x=lerp(a.x,b.x,t);q.y=lerp(a.y,b.y,t);q.z=lerp(a.z,b.z,t);q.w=lerp(a.w,b.w,t); return qn(q); }
static M4 qmat(Q q){ M4 m; float xx,yy,zz,xy,xz,yz,xw,yw,zw; q=qn(q); xx=q.x*q.x;yy=q.y*q.y;zz=q.z*q.z;xy=q.x*q.y;xz=q.x*q.z;yz=q.y*q.z;xw=q.x*q.w;yw=q.y*q.w;zw=q.z*q.w; mid(&m); m.m[0]=1.0f-2.0f*(yy+zz);m.m[1]=2.0f*(xy+zw);m.m[2]=2.0f*(xz-yw);m.m[4]=2.0f*(xy-zw);m.m[5]=1.0f-2.0f*(xx+zz);m.m[6]=2.0f*(yz+xw);m.m[8]=2.0f*(xz+yw);m.m[9]=2.0f*(yz-xw);m.m[10]=1.0f-2.0f*(xx+yy); return m; }
static void world(Cmd* c,M4* out){ M4 s,t,r; Q q; mid(&s);mid(&t); s.m[0]=c->scl[0];s.m[5]=c->scl[1];s.m[10]=c->scl[2]; t.m[12]=c->pos[0];t.m[13]=c->pos[1];t.m[14]=c->pos[2]; q.x=c->q[0];q.y=c->q[1];q.z=c->q[2];q.w=c->q[3]; r=qmat(q); *out=mmul(mmul(s,r),t); }
static void lookat(M4* r,float* eye,float* at,float* up){ float z[3]={eye[0]-at[0],eye[1]-at[1],eye[2]-at[2]}; float iz=rsq(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]); z[0]*=iz;z[1]*=iz;z[2]*=iz; float x[3]={z[1]*up[2]-z[2]*up[1],z[2]*up[0]-z[0]*up[2],z[0]*up[1]-z[1]*up[0]}; float ix=rsq(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]); if(ix==0){x[0]=1;x[1]=x[2]=0;} else {x[0]*=ix;x[1]*=ix;x[2]*=ix;} float y[3]={x[1]*z[2]-x[2]*z[1],x[2]*z[0]-x[0]*z[2],x[0]*z[1]-x[1]*z[0]}; mid(r); r->m[0]=x[0];r->m[1]=y[0];r->m[2]=z[0]; r->m[4]=x[1];r->m[5]=y[1];r->m[6]=z[1]; r->m[8]=x[2];r->m[9]=y[2];r->m[10]=z[2]; r->m[12]=-(x[0]*eye[0]+x[1]*eye[1]+x[2]*eye[2]); r->m[13]=-(y[0]*eye[0]+y[1]*eye[1]+y[2]*eye[2]); r->m[14]=-(z[0]*eye[0]+z[1]*eye[1]+z[2]*eye[2]); }
static void persp(M4* p){ zmem(p,sizeof(M4)); float f=cs(cam[5]*0.5f)/sn(cam[5]*0.5f),asp=(float)RW/(float)RH; p->m[0]=f/asp;p->m[5]=f;p->m[10]=cam[7]/(cam[6]-cam[7]);p->m[11]=-1;p->m[14]=(cam[6]*cam[7])/(cam[6]-cam[7]); }
static void ortho(M4* p,float w,float h,float n,float fa){ if(w<0.01f)w=0.01f; if(h<0.01f)h=0.01f; if(n<0.0001f)n=0.0001f; if(fa<=n+0.001f)fa=n+0.001f; zmem(p,sizeof(M4)); p->m[0]=2.0f/w;p->m[5]=2.0f/h;p->m[10]=1.0f/(n-fa);p->m[14]=n/(n-fa);p->m[15]=1.0f; }
static void viewproj(M4* out,float tsec){ float cp=cs(cam[4]), fwd[3]={sn(cam[3])*cp,sn(cam[4]),cs(cam[3])*cp}; float eye[3]={cam[0],cam[1],cam[2]}, at[3]={eye[0]+fwd[0],eye[1]+fwd[1],eye[2]+fwd[2]}, up[3]={0,1,0}; M4 v,p; lookat(&v,eye,at,up); persp(&p); *out=mmul(v,p); }
static void atlas(int ci,int cc,float* r){ if(cc<=1){r[0]=r[1]=1;r[2]=r[3]=0;return;} if(ci<0)ci=0; if(ci>3)ci=3; float x=0,y=0,w=1,h=1; for(int i=0;i<=ci;i++){ if((i&1)==0){r[0]=w*0.5f;r[1]=h;r[2]=x;r[3]=y;x+=r[0];w-=r[0];} else {r[0]=w;r[1]=h*0.5f;r[2]=x;r[3]=y;y+=r[1];h-=r[1];} } }
// Build the directional-light shadow matrices. One cascade uses the base
// ortho settings; 2-4 cascades use the split/size entries stored in dirlight.
static void shadowvp(SceneCB* sc){ float eye[3]={dl[0],dl[1],dl[2]},at[3]={dl[3],dl[4],dl[5]}; float ldx=at[0]-eye[0],ldy=at[1]-eye[1],ldz=at[2]-eye[2],li=rsq(ldx*ldx+ldy*ldy+ldz*ldz); float up[3]={0,1,0}; if(ab(ldy*li)>0.92f){up[1]=0;up[2]=1;} M4 lv,op,svp; lookat(&lv,eye,at,up); int cc=(int)dl[16]; if(cc<1)cc=1; if(cc>4)cc=4; sc->shadow_params[0]=(float)cc; sc->shadow_params[1]=cam[6]; sc->shadow_params[2]=cam[7]; ortho(&op,dl[14],dl[15],dl[12],dl[13]); svp=mmul(lv,op); for(int i=0;i<4;i++){ sc->shadow_cascade_splits[i]=cam[7]; sc->shadow_cascade_rects[i][0]=sc->shadow_cascade_rects[i][1]=sc->shadow_cascade_rects[i][2]=sc->shadow_cascade_rects[i][3]=0; cpy(sc->shadow_cascade_view_proj[i],svp.m,64); } if(cc>1){ float prev=cam[6]; for(int c=0;c<cc;c++){ int b=19+c*5; float sf=dl[b],mn=prev+0.001f; if(mn>cam[7])mn=cam[7]; if(sf<mn)sf=mn; if(sf>cam[7])sf=cam[7]; ortho(&op,dl[b+1],dl[b+2],dl[b+3],dl[b+4]); svp=mmul(lv,op); cpy(sc->shadow_cascade_view_proj[c],svp.m,64); sc->shadow_cascade_splits[c]=sf; atlas(c,cc,sc->shadow_cascade_rects[c]); prev=sf; if(c==0)cpy(sc->shadow_view_proj,svp.m,64); } sc->shadow_params[2]=sc->shadow_cascade_splits[cc-1]; } else { atlas(0,1,sc->shadow_cascade_rects[0]); cpy(sc->shadow_view_proj,svp.m,64); cpy(sc->shadow_cascade_view_proj[0],svp.m,64); sc->shadow_cascade_splits[0]=dl[13]; } cpy(sc->prev_shadow_view_proj,sc->shadow_view_proj,64); }
static int comps(int ty){ return (ty==1||ty==4)?1:(ty==2||ty==5)?2:(ty==3||ty==6)?3:(ty==7)?4:0; }
static int isint(int ty){ return ty>=1&&ty<=3; }
)LT64K", f);
    fputs(R"LT64K(// Timeline evaluation for compact key streams.
// Each track points to frame numbers in kfr[] and to either ki[] or kf[].
static int tr_frame(Tr* r,int k){ return kfr[r->fs+k]; }
static int tr_i(Tr* r,int k,int c){ return ki[r->ds+k*r->comp+c]; }
static float tr_f(Tr* r,int k,int c){ return kf[r->ds+k*r->comp+c]; }

// Evaluate one timeline track at the sampled frame and write the result directly
// into the live command, camera, light or user-variable array.
//
// Important editor-compatibility detail:
//   TL_LERP does NOT mean "hold the previous key until the next key".
//   The editor always interpolates numeric values between surrounding keys.
//   The option only decides whether the sampled frame is fractional (smooth)
//   or rounded down to an integer timeline frame (stepped at timeline FPS).
static void apply_track(Tr* r, float fr){
    int a=-1,b=-1,i,n=r->count;
    int oi[4]={0,0,0,0};
    float A[16],B[16],O[16];
    Q qq;
    zmem(A,sizeof(A)); zmem(B,sizeof(B)); zmem(O,sizeof(O));

    // Pick the nearest key at or before fr and the nearest key at or after fr.
    for(i=0;i<n;i++){
        int kf0=tr_frame(r,i);
        if((float)kf0<=fr)a=i;
        if((float)kf0>=fr){b=i;break;}
    }
    if(a<0)a=b>=0?b:0;
    if(b<0)b=a;

    if(r->integral){
        // Integral tracks are always stepped. Currently this is mainly command enabled.
        for(i=0;i<r->comp&&i<4;i++)oi[i]=tr_i(r,a,i);
    } else {
        for(i=0;i<r->comp&&i<16;i++){
            A[i]=tr_f(r,a,i);
            B[i]=tr_f(r,b,i);
            O[i]=A[i];
        }
        if(a!=b){
            int fa=tr_frame(r,a),fb=tr_frame(r,b);
            float tt=fb>fa?(fr-(float)fa)/(float)(fb-fa):0;
            tt=cl(tt,0,1);

            if(r->kind==2){
                // Command transform: position + quaternion + scale.
                for(i=0;i<3;i++)O[i]=lerp(A[i],B[i],tt);
                qq=ql(qfa(A+3),qfa(B+3),tt);
                O[3]=qq.x;O[4]=qq.y;O[5]=qq.z;O[6]=qq.w;
                for(i=7;i<10;i++)O[i]=lerp(A[i],B[i],tt);
            } else if(r->kind==4){
                // Camera: interpolate yaw as an angle so it wraps correctly.
                for(i=0;i<8;i++)O[i]=(i==3)?lerpa(A[i],B[i],tt):lerp(A[i],B[i],tt);
            } else {
                int cnt=r->comp;
                // User rotation sources can be stored either as Euler float3 or quaternion float4.
                if(r->kind==1 && r->target>=0&&r->target<UVN && uv[r->target].src==3 && uv[r->target].type==7){
                    qq=ql(qfa(A),qfa(B),tt);
                    O[0]=qq.x;O[1]=qq.y;O[2]=qq.z;O[3]=qq.w;
                } else {
                    for(i=0;i<cnt&&i<16;i++)O[i]=lerp(A[i],B[i],tt);
                }
            }
        }
    }

    if(r->kind==1 && r->target>=0&&r->target<UVN){
        int c=comps(uv[r->target].type);
        for(i=0;i<c&&i<4;i++){
            if(isint(uv[r->target].type))uv[r->target].iv[i]=oi[i];
            else uv[r->target].fv[i]=O[i];
        }
        if(uv[r->target].src_cmd>=0&&uv[r->target].src_cmd<CMDN){
            Cmd* cc=cmd+uv[r->target].src_cmd;
            if(uv[r->target].src==2)for(i=0;i<3;i++)cc->pos[i]=O[i];
            else if(uv[r->target].src==3){
                qq=uv[r->target].type==7?qfa(O):qfe(O);
                cc->q[0]=qq.x;cc->q[1]=qq.y;cc->q[2]=qq.z;cc->q[3]=qq.w;
            } else if(uv[r->target].src==4)for(i=0;i<3;i++)cc->scl[i]=O[i];
        } else if(uv[r->target].src==6){
            cam[3]=wrap(O[0]); cam[4]=cl(O[1],-1.50f,1.50f);
        }
    } else if(r->kind==2 && r->target>=0&&r->target<CMDN){
        Cmd* c=cmd+r->target;
        for(i=0;i<3;i++)c->pos[i]=O[i];
        for(i=0;i<4;i++)c->q[i]=O[3+i];
        for(i=0;i<3;i++)c->scl[i]=O[7+i];
    } else if(r->kind==3 && r->target>=0&&r->target<CMDN){
        cmd[r->target].enabled=oi[0]!=0;
    } else if(r->kind==4){
        for(i=0;i<8;i++)cam[i]=O[i];
        cam[3]=wrap(cam[3]); cam[4]=cl(cam[4],-1.50f,1.50f);
    } else if(r->kind==5){
        for(i=0;i<10;i++)dl[i]=O[i];
    }
}

static void timeline(float sec){
    if(!TL_ON||TL_LEN<1||TL_FPS<=0)return;
    // FPS is baked from timeline_settings.
    // TL_LERP=1: evaluate at fractional frame positions for smooth playback.
    // TL_LERP=0: quantize to integer frames before sampling. This gives a
    // visible step every timeline frame, while still interpolating between
    // key values exactly like the editor does.
    float end=(float)(TL_LEN-1),fr=sec*(float)TL_FPS;
    if(fr<0)fr=0;
    if(TL_LOOP&&end>0)while(fr>end)fr-=end;
    else if(fr>end)fr=end;
    if(!TL_LERP)fr=(float)((int)(fr+0.0001f));
    for(int i=0;i<TRN;i++)if(tr[i].enabled)apply_track(tr+i,fr);
}
static int invm(M4* a,M4* o){ float* m=a->m; float v[16],d; v[0]=m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10]; v[4]=-m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10]; v[8]=m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9]; v[12]=-m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9]; v[1]=-m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10]; v[5]=m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10]; v[9]=-m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9]; v[13]=m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9]; v[2]=m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6]; v[6]=-m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6]; v[10]=m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5]; v[14]=-m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5]; v[3]=-m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6]; v[7]=m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6]; v[11]=-m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5]; v[15]=m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5]; d=m[0]*v[0]+m[1]*v[4]+m[2]*v[8]+m[3]*v[12]; if(d==0){mid(o);return 0;} d=1.0f/d; for(int i=0;i<16;i++)o->m[i]=v[i]*d; return 1; }
// UserCB is rebuilt for each draw. Global user variables are written first;
// command parameters can then override the first slots for material data.
static void scene_src(float* o,int src,int src_cmd,int ty){ o[0]=o[1]=o[2]=0;o[3]=0; if((src==2||src==3||src==4)&&src_cmd>=0&&src_cmd<CMDN){ Cmd* c=cmd+src_cmd; if(src==2){o[0]=c->pos[0];o[1]=c->pos[1];o[2]=c->pos[2];o[3]=1;} else if(src==4){o[0]=c->scl[0];o[1]=c->scl[1];o[2]=c->scl[2];o[3]=1;} else if(ty==7){o[0]=c->q[0];o[1]=c->q[1];o[2]=c->q[2];o[3]=c->q[3];} } else if(src==5){o[0]=cam[0];o[1]=cam[1];o[2]=cam[2];o[3]=1;} else if(src==6){o[0]=cam[3];o[1]=cam[4];o[2]=cam[5];o[3]=0;} else if(src==7){o[0]=dl[0];o[1]=dl[1];o[2]=dl[2];o[3]=1;} else if(src==8){o[0]=dl[3];o[1]=dl[4];o[2]=dl[5];o[3]=1;} }
static void fill_user_base(UserCB* u){ int i,j; float sf[4]; zmem(u,sizeof(*u)); for(i=0;i<UVN&&i<64;i++){ if(isint(uv[i].type)) cpy(u->i[i],uv[i].iv,comps(uv[i].type)*4); else { if(uv[i].src>1){scene_src(sf,uv[i].src,uv[i].src_cmd,uv[i].type); for(j=0;j<comps(uv[i].type);j++)u->f[i][j]=sf[j];} else for(j=0;j<comps(uv[i].type);j++)u->f[i][j]=uv[i].fv[j]; } } }
static void fill_user_cmd(UserCB* u, Cmd* c){ int i,j,pi; float sf[4]; fill_user_base(u); for(i=0;i<c->pc&&i<64;i++){ pi=c->pst+i; if(pi<0||pi>=PN||!par[pi].enabled)continue; if(isint(par[pi].type)) cpy(u->i[i],par[pi].iv,comps(par[pi].type)*4); else { if(par[pi].src>1){scene_src(sf,par[pi].src,par[pi].src_cmd,par[pi].type); for(j=0;j<comps(par[pi].type);j++)u->f[i][j]=sf[j];} else for(j=0;j<comps(par[pi].type);j++)u->f[i][j]=par[pi].fv[j]; } } }
static void clear_vals(Cmd* c,float* cc,float* dc){ int i; for(i=0;i<4;i++)cc[i]=c->cc[i]; *dc=c->dc; if(c->ccs>=0&&c->ccs<UVN&&uv[c->ccs].type==7)for(i=0;i<4;i++)cc[i]=uv[c->ccs].fv[i]; if(c->dcs>=0&&c->dcs<UVN&&uv[c->dcs].type==4)*dc=cl(uv[c->dcs].fv[0],0,1); }
)LT64K", f);
    fputs(R"LT64K(static HRESULT compile(const char* src, unsigned int len, const char* e, const char* m, ID3DBlob** b){ ID3DBlob* er=0; HRESULT hr=D3DCompile(src,len,0,0,0,e,m,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,b,&er); if(er)ID3D10Blob_Release(er); return hr; }
// Compile embedded HLSL strings and create a POSITION/NORMAL/TEXCOORD layout
// when the shader accepts primitive geometry.
static void init_shaders(){ D3D11_INPUT_ELEMENT_DESC il[3]; zmem(il,sizeof(il)); il[0].SemanticName="POSITION";il[0].Format=DXGI_FORMAT_R32G32B32_FLOAT;il[0].AlignedByteOffset=0;il[0].InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA; il[1].SemanticName="NORMAL";il[1].Format=DXGI_FORMAT_R32G32B32_FLOAT;il[1].AlignedByteOffset=12;il[1].InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA; il[2].SemanticName="TEXCOORD";il[2].Format=DXGI_FORMAT_R32G32_FLOAT;il[2].AlignedByteOffset=24;il[2].InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA; for(int i=0;i<SHN;i++){ ID3DBlob *vs=0,*ps=0; if(SUCCEEDED(compile(sh[i].src,sh[i].len,"VSMain","vs_5_0",&vs))&&SUCCEEDED(compile(sh[i].src,sh[i].len,"PSMain","ps_5_0",&ps))){ ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),0,&sh[i].vs); ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(ps),ID3D10Blob_GetBufferSize(ps),0,&sh[i].ps); ID3D11Device_CreateInputLayout(dev,il,3,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),&sh[i].il); } if(vs)ID3D10Blob_Release(vs); if(ps)ID3D10Blob_Release(ps); } }
static void mkbuf(int id,float* v,unsigned int bytes){ D3D11_BUFFER_DESC d; D3D11_SUBRESOURCE_DATA s; zmem(&d,sizeof(d)); zmem(&s,sizeof(s)); d.ByteWidth=bytes; d.BindFlags=D3D11_BIND_VERTEX_BUFFER; s.pSysMem=v; ID3D11Device_CreateBuffer(dev,&d,&s,&prim_vb[id]); }
static void pv(float* v,int* n,float x,float y,float z,float nx,float ny,float nz,float u,float vv){ int i=*n; v[i+0]=x;v[i+1]=y;v[i+2]=z;v[i+3]=nx;v[i+4]=ny;v[i+5]=nz;v[i+6]=u;v[i+7]=vv; *n=i+8; }
// Create built-in procedural vertex buffers. Shader-only procedural draws use
// no input layout/VB; primitive draws use these tiny expanded triangle lists.
static void init_prims(){ static float cube[]={-1,-1,-1,0,0,-1,0,1, 1,1,-1,0,0,-1,1,0, 1,-1,-1,0,0,-1,1,1, -1,-1,-1,0,0,-1,0,1, -1,1,-1,0,0,-1,0,0, 1,1,-1,0,0,-1,1,0, -1,-1,1,0,0,1,0,1, 1,-1,1,0,0,1,1,1, 1,1,1,0,0,1,1,0, -1,-1,1,0,0,1,0,1, 1,1,1,0,0,1,1,0, -1,1,1,0,0,1,0,0, -1,-1,-1,-1,0,0,0,1, -1,-1,1,-1,0,0,1,1, -1,1,1,-1,0,0,1,0, -1,-1,-1,-1,0,0,0,1, -1,1,1,-1,0,0,1,0, -1,1,-1,-1,0,0,0,0, 1,-1,-1,1,0,0,0,1, 1,1,1,1,0,0,1,0, 1,-1,1,1,0,0,1,1, 1,-1,-1,1,0,0,0,1, 1,1,-1,1,0,0,0,0, 1,1,1,1,0,0,1,0, -1,-1,-1,0,-1,0,0,1, 1,-1,-1,0,-1,0,1,1, 1,-1,1,0,-1,0,1,0, -1,-1,-1,0,-1,0,0,1, 1,-1,1,0,-1,0,1,0, -1,-1,1,0,-1,0,0,0, -1,1,-1,0,1,0,0,1, -1,1,1,0,1,0,0,0, 1,1,1,0,1,0,1,0, -1,1,-1,0,1,0,0,1, 1,1,1,0,1,0,1,0, 1,1,-1,0,1,0,1,1};
 static float quad[]={-1,-1,0,0,0,1,0,1, 1,-1,0,0,0,1,1,1, 1,1,0,0,0,1,1,0, -1,-1,0,0,0,1,0,1, 1,1,0,0,0,1,1,0, -1,1,0,0,0,1,0,0};
 static float tet[]={1,1,1,0.57735f,0.57735f,0.57735f,0.5f,0, -1,-1,1,0.57735f,0.57735f,0.57735f,0,1, -1,1,-1,0.57735f,0.57735f,0.57735f,1,1, 1,1,1,0.57735f,-0.57735f,0.57735f,0.5f,0, 1,-1,-1,0.57735f,-0.57735f,0.57735f,0,1, -1,-1,1,0.57735f,-0.57735f,0.57735f,1,1, 1,1,1,0.57735f,0.57735f,-0.57735f,0.5f,0, -1,1,-1,0.57735f,0.57735f,-0.57735f,0,1, 1,-1,-1,0.57735f,0.57735f,-0.57735f,1,1, -1,-1,1,-0.57735f,-0.57735f,-0.57735f,0.5f,0, 1,-1,-1,-0.57735f,-0.57735f,-0.57735f,0,1, -1,1,-1,-0.57735f,-0.57735f,-0.57735f,1,1};
 static float fs[]={-1,-1,0,0,0,1,0,1, -1,3,0,0,0,1,0,-1, 3,-1,0,0,0,1,2,1};
)LT64K", f);
    fputs(R"LT64K( // The editor builds the sphere through an indexed mesh and calls
 // res_add_oriented_tri(), which flips every generated sphere triangle so
 // its winding is outward for the D3D11 rasterizer state.  The first 64k
 // rewrite expanded the triangles but kept the pre-flip order.  That rendered
 // the sphere with the opposite face orientation: color could still appear in
 // some views, but the shadow prepass wrote the wrong side of the sphere, so
 // the receiver sampled shadows that seemed to rotate/swim with the camera.
 //
 // Keep this expanded list in the same post-flip order as resources.cpp:
 //   top cap:    i0,i3,i1
 //   middle A:   i0,i2,i1
 //   middle B:   i2,i3,i1
 //   bottom cap: i0,i2,i1
 static float sph[2880*8]; int n=0; for(int r=0;r<16;r++){ for(int s=0;s<32;s++){ float v0=(float)r/16.0f,v1=(float)(r+1)/16.0f,u0=(float)s/32.0f,u1=(float)(s+1)/32.0f; float th0=v0*LT_PI,th1=v1*LT_PI,ph0=u0*6.28318530718f,ph1=u1*6.28318530718f; float sr0=sn(th0),sr1=sn(th1); float p0[8]={sr0*cs(ph0),cs(th0),sr0*sn(ph0),sr0*cs(ph0),cs(th0),sr0*sn(ph0),u0,v0}; float p1[8]={sr1*cs(ph0),cs(th1),sr1*sn(ph0),sr1*cs(ph0),cs(th1),sr1*sn(ph0),u0,v1}; float p2[8]={sr0*cs(ph1),cs(th0),sr0*sn(ph1),sr0*cs(ph1),cs(th0),sr0*sn(ph1),u1,v0}; float p3[8]={sr1*cs(ph1),cs(th1),sr1*sn(ph1),sr1*cs(ph1),cs(th1),sr1*sn(ph1),u1,v1}; if(r==0){pv(sph,&n,p0[0],p0[1],p0[2],p0[3],p0[4],p0[5],p0[6],p0[7]);pv(sph,&n,p3[0],p3[1],p3[2],p3[3],p3[4],p3[5],p3[6],p3[7]);pv(sph,&n,p1[0],p1[1],p1[2],p1[3],p1[4],p1[5],p1[6],p1[7]);} else if(r==15){pv(sph,&n,p0[0],p0[1],p0[2],p0[3],p0[4],p0[5],p0[6],p0[7]);pv(sph,&n,p2[0],p2[1],p2[2],p2[3],p2[4],p2[5],p2[6],p2[7]);pv(sph,&n,p1[0],p1[1],p1[2],p1[3],p1[4],p1[5],p1[6],p1[7]);} else {pv(sph,&n,p0[0],p0[1],p0[2],p0[3],p0[4],p0[5],p0[6],p0[7]);pv(sph,&n,p2[0],p2[1],p2[2],p2[3],p2[4],p2[5],p2[6],p2[7]);pv(sph,&n,p1[0],p1[1],p1[2],p1[3],p1[4],p1[5],p1[6],p1[7]);pv(sph,&n,p2[0],p2[1],p2[2],p2[3],p2[4],p2[5],p2[6],p2[7]);pv(sph,&n,p3[0],p3[1],p3[2],p3[3],p3[4],p3[5],p3[6],p3[7]);pv(sph,&n,p1[0],p1[1],p1[2],p1[3],p1[4],p1[5],p1[6],p1[7]);} } } mkbuf(1,cube,sizeof(cube)); mkbuf(2,quad,sizeof(quad)); mkbuf(3,tet,sizeof(tet)); mkbuf(4,sph,(unsigned int)(n*4)); mkbuf(5,fs,sizeof(fs)); }
)LT64K", f);
    fputs(R"LT64K(static int rtw(int i){ int d=rtd[i].div,w; if(d>0)w=(RW+d-1)/d; else w=rtd[i].w>0?rtd[i].w:RW; return w>0?w:1; }
static int rth(int i){ int d=rtd[i].div,h; if(d>0)h=(RH+d-1)/d; else h=rtd[i].h>0?rtd[i].h:RH; return h>0?h:1; }
static DXGI_FORMAT sfmt(DXGI_FORMAT f){ if(f==DXGI_FORMAT_D24_UNORM_S8_UINT||f==DXGI_FORMAT_R24G8_TYPELESS)return DXGI_FORMAT_R24G8_TYPELESS; if(f==DXGI_FORMAT_D32_FLOAT||f==DXGI_FORMAT_R32_TYPELESS)return DXGI_FORMAT_R32_TYPELESS; return f; }
static DXGI_FORMAT srvfmt(DXGI_FORMAT f){ if(f==DXGI_FORMAT_D24_UNORM_S8_UINT||f==DXGI_FORMAT_R24G8_TYPELESS)return DXGI_FORMAT_R24_UNORM_X8_TYPELESS; if(f==DXGI_FORMAT_D32_FLOAT||f==DXGI_FORMAT_R32_TYPELESS)return DXGI_FORMAT_R32_FLOAT; return f; }
// Allocate internal render textures requested by the project. They replace file
// textures in the 64k path and support post-processing chains.
static void init_rts(){ for(int i=0;i<RTN;i++){ D3D11_TEXTURE2D_DESC d; zmem(&d,sizeof(d)); d.Width=rtw(i); d.Height=rth(i); d.MipLevels=1; d.ArraySize=1; d.Format=sfmt((DXGI_FORMAT)rtd[i].fmt); d.SampleDesc.Count=1; d.BindFlags=(rtd[i].rtv?D3D11_BIND_RENDER_TARGET:0)|(rtd[i].srv?D3D11_BIND_SHADER_RESOURCE:0)|(rtd[i].uav?D3D11_BIND_UNORDERED_ACCESS:0); if(!d.BindFlags)d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE; if(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&d,0,&rt_tex[i]))){ if(rtd[i].rtv)ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)rt_tex[i],0,&rt_rtv[i]); if(rtd[i].srv){ D3D11_SHADER_RESOURCE_VIEW_DESC sv; zmem(&sv,sizeof(sv)); sv.Format=srvfmt((DXGI_FORMAT)rtd[i].fmt); sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels=1; ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)rt_tex[i],&sv,&rt_srv[i]); } } } }
// Create the main depth buffer and the shared shadow-map atlas. Both also have
// SRVs so post effects and material shaders can sample them.
static void init_depth_shadow(){ D3D11_TEXTURE2D_DESC d; D3D11_DEPTH_STENCIL_VIEW_DESC dv; D3D11_SHADER_RESOURCE_VIEW_DESC sv; zmem(&d,sizeof(d)); d.Width=RW;d.Height=RH;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R24G8_TYPELESS;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE; if(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&d,0,&depth_tex))){ zmem(&dv,sizeof(dv)); zmem(&sv,sizeof(sv)); dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D; ID3D11Device_CreateDepthStencilView(dev,(ID3D11Resource*)depth_tex,&dv,&dsv); sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels=1; ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)depth_tex,&sv,&depth_srv); } zmem(&d,sizeof(d)); d.Width=LT_SHADOW_W;d.Height=LT_SHADOW_H;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R24G8_TYPELESS;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE; if(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&d,0,&shadow_tex))){ zmem(&dv,sizeof(dv)); zmem(&sv,sizeof(sv)); dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D; ID3D11Device_CreateDepthStencilView(dev,(ID3D11Resource*)shadow_tex,&dv,&shadow_dsv); sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels=1; ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)shadow_tex,&sv,&shadow_srv); } }
static void init_states(){ D3D11_SAMPLER_DESC sp; zmem(&sp,sizeof(sp)); sp.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; sp.AddressU=sp.AddressV=sp.AddressW=D3D11_TEXTURE_ADDRESS_WRAP; sp.MaxLOD=3.402823466e38f; ID3D11Device_CreateSamplerState(dev,&sp,&smp_lin); sp.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT; sp.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL; sp.AddressU=sp.AddressV=sp.AddressW=D3D11_TEXTURE_ADDRESS_BORDER; sp.BorderColor[0]=sp.BorderColor[1]=sp.BorderColor[2]=sp.BorderColor[3]=1.0f; ID3D11Device_CreateSamplerState(dev,&sp,&smp_cmp); for(int i=0;i<4;i++){ D3D11_DEPTH_STENCIL_DESC dd; zmem(&dd,sizeof(dd)); dd.DepthEnable=(i&1)!=0; dd.DepthWriteMask=(i&2)?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO; /* Keep the tiny-player depth test as LESS_EQUAL, matching the previous working 64k path.
           The editor uses LESS for the interactive viewport, but this standalone path also
           uses generated primitive shadow shaders and very small procedural meshes; LEQUAL
           is more robust for the shadow/color depth state and fixes the black test2 output
           introduced in v7. */ dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL; ID3D11Device_CreateDepthStencilState(dev,&dd,&ds[i]); D3D11_BLEND_DESC bd; zmem(&bd,sizeof(bd)); bd.RenderTarget[0].BlendEnable=(i&1)!=0; bd.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD; bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask=(i&2)?D3D11_COLOR_WRITE_ENABLE_ALL:0; ID3D11Device_CreateBlendState(dev,&bd,&bs[i]); } for(int i=0;i<2;i++){ D3D11_RASTERIZER_DESC rd; zmem(&rd,sizeof(rd)); rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=i?D3D11_CULL_BACK:D3D11_CULL_NONE; rd.DepthClipEnable=TRUE; ID3D11Device_CreateRasterizerState(dev,&rd,&rs[i]); } }
static void cb_make(ID3D11Buffer** b, unsigned int sz){ D3D11_BUFFER_DESC d; zmem(&d,sizeof(d)); d.ByteWidth=(sz+15)&~15; d.Usage=D3D11_USAGE_DYNAMIC; d.BindFlags=D3D11_BIND_CONSTANT_BUFFER; d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE; ID3D11Device_CreateBuffer(dev,&d,0,b); }
static void cb_up(ID3D11Buffer* b, void* data, unsigned int sz){ D3D11_MAPPED_SUBRESOURCE m; if(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)b,0,D3D11_MAP_WRITE_DISCARD,0,&m))){ cpy(m.pData,data,sz); ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)b,0); } }
static ID3D11ShaderResourceView* srvof(int s){ if(s>=0&&s<RTN)return rt_srv[s]; if(s==-2)return depth_srv; if(s==-3)return shadow_srv; return 0; }
static ID3D11RenderTargetView* rtvof(int t){ if(t==-2)return rtv; if(t>=0&&t<RTN)return rt_rtv[t]; return 0; }
static void set_target(Cmd* c){ ID3D11ShaderResourceView* nulls[16]; zmem(nulls,sizeof(nulls)); ID3D11DeviceContext_PSSetShaderResources(ctx,0,16,nulls); ID3D11DeviceContext_VSSetShaderResources(ctx,0,16,nulls); ID3D11RenderTargetView* rv=rtvof(c->rt); ID3D11DepthStencilView* dv=(c->dep==-2)?dsv:0; ID3D11DeviceContext_OMSetRenderTargets(ctx,rv?1:0,rv?&rv:0,dv); D3D11_VIEWPORT vp; zmem(&vp,sizeof(vp)); if(c->rt>=0&&c->rt<RTN){ vp.Width=(float)rtw(c->rt); vp.Height=(float)rth(c->rt); } else if(c->rt==-2){ vp.Width=(float)W; vp.Height=(float)H; } else { vp.Width=(float)RW; vp.Height=(float)RH; } vp.MinDepth=0;vp.MaxDepth=1; ID3D11DeviceContext_RSSetViewports(ctx,1,&vp); }
static void shadow_draw(Cmd* c){ if(!c->enabled||!c->scast||c->shs<0||c->shs>=SHN||!sh[c->shs].vs)return; UserCB uc; ObjectCB oc; M4 w; world(c,&w); cpy(oc.world,w.m,64); fill_user_cmd(&uc,c); cb_up(object_cb,&oc,sizeof(oc)); cb_up(user_cb,&uc,sizeof(uc)); ID3D11DeviceContext_RSSetState(ctx,rs[c->cb?1:0]); ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&scene_cb); ID3D11DeviceContext_VSSetConstantBuffers(ctx,1,1,&object_cb); ID3D11DeviceContext_VSSetConstantBuffers(ctx,2,1,&user_cb); if(c->mk>0&&c->mk<6&&prim_vb[c->mk]){ unsigned int st=32,off=0; ID3D11DeviceContext_IASetInputLayout(ctx,sh[c->shs].il); ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&prim_vb[c->mk],&st,&off); } else { ID3D11DeviceContext_IASetInputLayout(ctx,0); ID3D11DeviceContext_IASetVertexBuffers(ctx,0,0,0,0,0); } ID3D11DeviceContext_IASetIndexBuffer(ctx,0,DXGI_FORMAT_R32_UINT,0); ID3D11DeviceContext_IASetPrimitiveTopology(ctx,c->topology?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D11_PRIMITIVE_TOPOLOGY_POINTLIST); ID3D11DeviceContext_VSSetShader(ctx,sh[c->shs].vs,0,0); ID3D11DeviceContext_PSSetShader(ctx,0,0,0); ID3D11DeviceContext_DrawInstanced(ctx,c->vc,c->ic,0,0); }
// Render all shadow-casting commands into the atlas before the main pass.
static void shadowpass(SceneCB* sc){ if(!shadow_dsv)return; int need=0; for(int i=0;i<CMDN;i++)if(cmd[i].enabled&&cmd[i].scast)need=1; if(!need)return; ID3D11ShaderResourceView* nulls[16]; zmem(nulls,sizeof(nulls)); ID3D11DeviceContext_PSSetShaderResources(ctx,0,16,nulls); ID3D11DeviceContext_VSSetShaderResources(ctx,0,16,nulls); ID3D11DeviceContext_OMSetRenderTargets(ctx,0,0,shadow_dsv); ID3D11DeviceContext_ClearDepthStencilView(ctx,shadow_dsv,D3D11_CLEAR_DEPTH,1,0); ID3D11DeviceContext_OMSetDepthStencilState(ctx,ds[3],0); float bf[4]={0,0,0,0}; ID3D11DeviceContext_OMSetBlendState(ctx,bs[0],bf,0xffffffff); int cc=(int)sc->shadow_params[0]; if(cc<1)cc=1;if(cc>4)cc=4; for(int ci=0;ci<cc;ci++){ float* r=sc->shadow_cascade_rects[ci]; D3D11_VIEWPORT vp; zmem(&vp,sizeof(vp)); vp.TopLeftX=r[2]*(float)LT_SHADOW_W;vp.TopLeftY=r[3]*(float)LT_SHADOW_H;vp.Width=(r[0]>0?r[0]:1)*(float)LT_SHADOW_W;vp.Height=(r[1]>0?r[1]:1)*(float)LT_SHADOW_H;vp.MinDepth=0;vp.MaxDepth=1; ID3D11DeviceContext_RSSetViewports(ctx,1,&vp); SceneCB t=*sc; cpy(t.shadow_view_proj,sc->shadow_cascade_view_proj[ci],64); cb_up(scene_cb,&t,sizeof(t)); for(int i=0;i<CMDN;i++)shadow_draw(cmd+i); } cb_up(scene_cb,sc,sizeof(*sc)); }
)LT64K", f);
    fputs(R"LT64K(// Main frame: update animation, render shadows, then execute the command list.
// Draw a tiny, optional FPS overlay directly over the presented window.
// It is compiled out by default because final/demo builds should not show any
// diagnostic text and should not spend even a small amount of time in GDI.
//
// Enable it with /DLT_DEBUG_FPS=1 when you need to check pacing. It uses GDI
// after Present instead of D3D, so it does not add another shader, texture,
// buffer or render pass. Press F1 to toggle it while the program is running.
#if LT_DEBUG_FPS
static char* apu(char* p,unsigned int v){ char b[10]; int n=0; if(!v){*p++='0';return p;} while(v&&n<10){b[n++]=(char)('0'+v%10);v/=10;} while(n--)*p++=b[n]; return p; }
static char* aps(char* p,const char* s){ while(*s)*p++=*s++; return p; }
static void dbg_fps(void){
    static DWORD last_tick=0, win0=0;
    static unsigned int frames=0, fps=0, avg10=0, maxms=0, maxwin=0;
    DWORD now=GetTickCount();
    if(!last_tick) last_tick=now;
    if(!win0) win0=now;

    // Use wall-clock milliseconds rather than the timeline clock. The timeline
    // may clamp very large deltas to avoid animation jumps, but the debug HUD
    // should report the real frame pacing that the user sees.
    { DWORD frame_ms=now-last_tick; if(frame_ms>maxwin)maxwin=frame_ms; last_tick=now; }
    frames++;
    { DWORD dt=now-win0; if(dt>=500){ fps=(unsigned int)((frames*1000u + dt/2u)/dt); avg10=(unsigned int)((dt*10u + frames/2u)/frames); maxms=maxwin; frames=0; maxwin=0; win0=now; } }

    if(dbg_on){
        char txt[128]; char* p=txt;
        p=aps(p,"FPS "); p=apu(p,fps);
        p=aps(p,"  avg "); p=apu(p,avg10/10u); *p++='.'; *p++=(char)('0'+(avg10%10u)); p=aps(p,"ms");
        p=aps(p,"  max "); p=apu(p,maxms); p=aps(p,"ms");
        p=aps(p,"  rt "); p=apu(p,(unsigned int)RW); *p++='x'; p=apu(p,(unsigned int)RH);
        p=aps(p,"  win "); p=apu(p,(unsigned int)W); *p++='x'; p=apu(p,(unsigned int)H);
        p=aps(p,LT_VSYNC?"  vsync":"  immediate");
        *p=0;
        HDC dc=GetDC(wh);
        if(dc){ SetBkMode(dc,OPAQUE); SetBkColor(dc,0x00000000); SetTextColor(dc,0x00ffffff); TextOutA(dc,12,12,txt,(int)(p-txt)); ReleaseDC(wh,dc); }
    }
}
#else
static void dbg_fps(void){}
#endif
static void render(float sec){ cpy(cam,cam0,sizeof(cam));cpy(dl,dl0,sizeof(dl)); timeline(sec); float clr[4]={0,0,0,1},bf[4]={0,0,0,0}; SceneCB sc; ObjectCB oc; M4 vp,ivp; zmem(&sc,sizeof(sc)); viewproj(&vp,sec); invm(&vp,&ivp); cpy(sc.view_proj,vp.m,64); cpy(sc.prev_view_proj,vp.m,64); cpy(sc.inv_view_proj,ivp.m,64); cpy(sc.prev_inv_view_proj,ivp.m,64); shadowvp(&sc); sc.time_vec[0]=sec; sc.time_vec[2]=sec*60.0f; sc.cam_pos[0]=cam[0];sc.cam_pos[1]=cam[1];sc.cam_pos[2]=cam[2]; float cp=cs(cam[4]); sc.cam_dir[0]=sn(cam[3])*cp;sc.cam_dir[1]=sn(cam[4]);sc.cam_dir[2]=cs(cam[3])*cp; float ldx=dl[3]-dl[0],ldy=dl[4]-dl[1],ldz=dl[5]-dl[2],li=rsq(ldx*ldx+ldy*ldy+ldz*ldz); sc.light_dir[0]=ldx*li;sc.light_dir[1]=ldy*li;sc.light_dir[2]=ldz*li;sc.light_dir[3]=dl[9]; sc.light_color[0]=dl[6];sc.light_color[1]=dl[7];sc.light_color[2]=dl[8]; shadowpass(&sc); cb_up(scene_cb,&sc,sizeof(sc)); ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,clr); if(dsv)ID3D11DeviceContext_ClearDepthStencilView(ctx,dsv,D3D11_CLEAR_DEPTH,1,0); for(int i=0;i<CMDN;i++){ Cmd* c=cmd+i; if(!c->enabled)continue; set_target(c); if(c->type==1){ ID3D11RenderTargetView* rv=rtvof(c->rt); float cc[4],dc; clear_vals(c,cc,&dc); if(c->ccen&&rv)ID3D11DeviceContext_ClearRenderTargetView(ctx,rv,cc); if(c->cden&&c->dep==-2&&dsv)ID3D11DeviceContext_ClearDepthStencilView(ctx,dsv,D3D11_CLEAR_DEPTH,dc,0); } else if(c->type==2 && c->shader>=0&&c->shader<SHN&&sh[c->shader].vs&&sh[c->shader].ps){ UserCB uc; M4 w; world(c,&w); cpy(oc.world,w.m,64); fill_user_cmd(&uc,c); cb_up(object_cb,&oc,sizeof(oc)); cb_up(user_cb,&uc,sizeof(uc)); ID3D11DeviceContext_OMSetDepthStencilState(ctx,ds[(c->dt?1:0)|(c->dw?2:0)],0); ID3D11DeviceContext_RSSetState(ctx,rs[c->cb?1:0]); ID3D11DeviceContext_OMSetBlendState(ctx,bs[(c->ab?1:0)|(c->cw?2:0)],bf,0xffffffff); ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&scene_cb); ID3D11DeviceContext_PSSetConstantBuffers(ctx,0,1,&scene_cb); ID3D11DeviceContext_VSSetConstantBuffers(ctx,1,1,&object_cb); ID3D11DeviceContext_PSSetConstantBuffers(ctx,1,1,&object_cb); ID3D11DeviceContext_VSSetConstantBuffers(ctx,2,1,&user_cb); ID3D11DeviceContext_PSSetConstantBuffers(ctx,2,1,&user_cb); ID3D11DeviceContext_PSSetSamplers(ctx,0,1,&smp_lin); ID3D11DeviceContext_PSSetSamplers(ctx,1,1,&smp_cmp); for(int t=0;t<c->tc;t++){ ID3D11ShaderResourceView* sv=srvof(c->tex[t]); ID3D11DeviceContext_PSSetShaderResources(ctx,c->tsl[t],1,&sv); } if(c->srecv){ ID3D11ShaderResourceView* sv=shadow_srv; ID3D11DeviceContext_PSSetShaderResources(ctx,7,1,&sv); } for(int t=0;t<c->sc;t++){ ID3D11ShaderResourceView* sv=srvof(c->srv[t]); ID3D11DeviceContext_VSSetShaderResources(ctx,c->ssl[t],1,&sv); } if(c->mk>0&&c->mk<6&&prim_vb[c->mk]){ unsigned int st=32,off=0; ID3D11DeviceContext_IASetInputLayout(ctx,sh[c->shader].il); ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&prim_vb[c->mk],&st,&off); } else { ID3D11DeviceContext_IASetInputLayout(ctx,0); ID3D11DeviceContext_IASetVertexBuffers(ctx,0,0,0,0,0); } ID3D11DeviceContext_IASetIndexBuffer(ctx,0,DXGI_FORMAT_R32_UINT,0); ID3D11DeviceContext_IASetPrimitiveTopology(ctx,c->topology?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D11_PRIMITIVE_TOPOLOGY_POINTLIST); ID3D11DeviceContext_VSSetShader(ctx,sh[c->shader].vs,0,0); ID3D11DeviceContext_PSSetShader(ctx,sh[c->shader].ps,0,0); ID3D11DeviceContext_DrawInstanced(ctx,c->vc,c->ic,0,0); } } IDXGISwapChain_Present(swp,LT_VSYNC?1:0,0); dbg_fps(); }
static LRESULT CALLBACK wp(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_KEYDOWN&&w==VK_ESCAPE)PostQuitMessage(0);
#if LT_DEBUG_FPS
    // F1 is a runtime toggle for the optional FPS overlay. In release mode
    // LT_DEBUG_FPS is 0, this branch is removed by the preprocessor.
    if(m==WM_KEYDOWN&&w==VK_F1){dbg_on=!dbg_on;return 0;}
#endif
    if(m==WM_CLOSE||m==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcA(h,m,w,l);
}
// Return the elapsed seconds between two QPC samples without using 64-bit-to-double conversion.
// MSVC x86 emits the CRT helper __ltod3 for `(double)LARGE_INTEGER.QuadPart`; that helper is
// intentionally unavailable in this /NODEFAULTLIB tiny build.  We only need the delta between
// consecutive frames, so the unsigned 32-bit low-part difference is enough and is wrap-safe as
// long as a single frame does not last several minutes.
static float qpcdt(LARGE_INTEGER* now,LARGE_INTEGER* prev,LARGE_INTEGER* freq){ unsigned int dt=(unsigned int)now->LowPart-(unsigned int)prev->LowPart; unsigned int fq=(unsigned int)freq->LowPart? (unsigned int)freq->LowPart:1; float s=(float)dt/(float)fq; if(s>0.10f)s=0.10f; return s; }
void WINAPI WinMainCRTStartup(void){ WNDCLASSA wc; zmem(&wc,sizeof(wc)); wc.lpfnWndProc=wp; wc.hInstance=GetModuleHandleA(0); wc.lpszClassName=LT_WNDCLS; RegisterClassA(&wc); // Fullscreen is implemented as borderless desktop fullscreen instead of exclusive mode.
// That keeps startup simple, avoids display-mode changes, and is friendly to demos compressed with UPX.
// Fullscreen-only size policy: render at the real monitor/backbuffer size.
// RW/RH are deliberately equal to W/H, so the camera aspect ratio cannot be
// different from the final presentation aspect ratio.
W=GetSystemMetrics(SM_CXSCREEN); H=GetSystemMetrics(SM_CYSCREEN); RW=W; RH=H; HWND hw=CreateWindowExA(WS_EX_TOPMOST,LT_WNDCLS,"lt64k",WS_POPUP|WS_VISIBLE,0,0,W,H,0,0,wc.hInstance,0); wh=hw; SetWindowPos(hw,HWND_TOPMOST,0,0,W,H,SWP_SHOWWINDOW); ShowCursor(FALSE); DXGI_SWAP_CHAIN_DESC sd; zmem(&sd,sizeof(sd)); sd.BufferCount=1; sd.BufferDesc.Width=W; sd.BufferDesc.Height=H; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=hw; sd.SampleDesc.Count=1; sd.Windowed=TRUE; if(FAILED(D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&sd,&swp,&dev,0,&ctx)))ExitProcess(1); ID3D11Texture2D* bb=0; IDXGISwapChain_GetBuffer(swp,0,&IID_ID3D11Texture2D,(void**)&bb); ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)bb,0,&rtv); ID3D11Texture2D_Release(bb); init_depth_shadow(); init_rts(); init_states(); cb_make(&scene_cb,sizeof(SceneCB)); cb_make(&object_cb,sizeof(ObjectCB)); cb_make(&user_cb,sizeof(UserCB)); init_shaders(); init_prims(); LARGE_INTEGER fq,t0,tn; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); float sec=0; MSG msg; for(;;){ while(PeekMessageA(&msg,0,0,0,PM_REMOVE)){ if(msg.message==WM_QUIT)ExitProcess(0); TranslateMessage(&msg); DispatchMessageA(&msg);} QueryPerformanceCounter(&tn); sec+=qpcdt(&tn,&t0,&fq); t0=tn; render(sec); } }
)LT64K", f);

    std::fclose(f);
    pretty_format_c_file(out_c);
    long long out_bytes = file_size_bytes(out_c);
    size_t raw_hlsl = 0, min_hlsl = 0, lit_hlsl = 0;
    for (size_t i = 0; i < shader_sources.size(); i++) {
        raw_hlsl += shader_raw_sizes[i];
        min_hlsl += shader_min_sizes[i];
        lit_hlsl += cstr_literal(shader_sources[i]).size();
    }
    size_t cmd_bytes = out_cmds.size() * sizeof(CommandDef);
    size_t param_bytes = flat_params.size() * sizeof(FlatParam);
    size_t key_bytes = flat_key_frames.size() * sizeof(int) + flat_key_ints.size() * sizeof(int) + flat_key_floats.size() * sizeof(float);
    size_t rt_bytes = rt_res_indices.size() * sizeof(ResourceDef);
    fprintf(stderr, "generated %s\n", out_c.c_str());
    fprintf(stderr, "commands: %u, shaders: %u, user vars: %u, timeline tracks: %u\n", (unsigned)out_cmds.size(), (unsigned)shader_sources.size(), (unsigned)p.user_vars.size(), (unsigned)flat_tracks.size());
    fprintf(stderr, "timeline: fps=%d length=%d loop=%s enabled=%s interpolation=%s%s\n", p.timeline_fps, p.timeline_length, p.timeline_loop?"on":"off", p.timeline_enabled?"on":"off", p.timeline_interpolate?"linear":"step", (p.timeline_loop && p.timeline_length>1)?" (loop period = length-1 frames, editor-compatible)":"");
    fprintf(stderr, "\n64k exporter size report (source-side, before MSVC/linker packing):\n");
    if (out_bytes >= 0) fprintf(stderr, "  out64k.c:                 %lld bytes\n", out_bytes);
    fprintf(stderr, "  HLSL expanded raw:        %u bytes\n", (unsigned)raw_hlsl);
    fprintf(stderr, "  HLSL embedded:            %u bytes  (%s)\n", (unsigned)min_hlsl, LT64K_MINIFY_HLSL ? "minified" : "editor-expanded");
    fprintf(stderr, "  HLSL as C string literal: %u bytes\n", (unsigned)lit_hlsl);
    fprintf(stderr, "  command defs estimate:    %u bytes\n", (unsigned)cmd_bytes);
    fprintf(stderr, "  param defs estimate:      %u bytes\n", (unsigned)param_bytes);
    fprintf(stderr, "  timeline data estimate:   %u bytes  (%u frames, %u ints, %u floats)\n", (unsigned)key_bytes, (unsigned)flat_key_frames.size(), (unsigned)flat_key_ints.size(), (unsigned)flat_key_floats.size());
    fprintf(stderr, "  render targets estimate:  %u bytes\n", (unsigned)rt_bytes);
    fprintf(stderr, "  shaders by expanded/embedded bytes:\n");
    for (size_t i = 0; i < shader_sources.size(); i++)
        fprintf(stderr, "    %-24s %6u -> %6u\n", shader_names[i].c_str(), (unsigned)shader_raw_sizes[i], (unsigned)shader_min_sizes[i]);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: build64k <project.lt> [out64k.c]\n");
        return 2;
    }
    std::string lt = argv[1];
    std::string out = argc >= 3 ? argv[2] : "out64k.c";
    Project p = parse_lt(lt);
    emit_generated_c(p, lt, out);
    return 0;
}
