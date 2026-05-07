// lazyTool 64k proof-of-concept exporter.
// Reads a .lt file and emits one tiny Win32/D3D11 C file for shader/procedural-only scenes.
// Intentionally old-school/c-style in the generated output: no editor, no ImGui, no assets.

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

static std::string read_text_file(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool file_exists(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return (bool)f;
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

enum ResKind {
    RK_NONE = 0,
    RK_VALUE,
    RK_MESH_PRIMITIVE,
    RK_MESH_FILE,
    RK_SHADER_VSPS,
    RK_SHADER_CS,
    RK_TEXTURE,
    RK_RT,
    RK_BUFFER
};

enum ValType {
    VT_NONE = 0,
    VT_INT, VT_INT2, VT_INT3,
    VT_FLOAT, VT_FLOAT2, VT_FLOAT3, VT_FLOAT4
};

enum CmdType {
    CT_NONE = 0,
    CT_CLEAR,
    CT_DRAW,
    CT_DRAW_INSTANCED,
    CT_DISPATCH
};

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

static CmdType parse_cmd_type(const std::string& s) {
    if (s == "clear") return CT_CLEAR;
    if (s == "draw_mesh") return CT_DRAW;
    if (s == "draw_instanced") return CT_DRAW_INSTANCED;
    if (s == "dispatch") return CT_DISPATCH;
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

struct ResourceDef {
    std::string name;
    ResKind kind = RK_NONE;
    ValType value_type = VT_NONE;
    std::string path;
    std::string primitive;
    int ival[4] = {};
    float fval[4] = {};
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
    int ival[4] = {};
    float fval[4] = {};
};

struct CommandDef {
    std::string name;
    CmdType type = CT_NONE;
    bool enabled = true;
    std::string mesh;
    std::string shader;
    bool procedural = false;
    int topology = 0; // 0 point, 1 tri
    int mesh_kind = 0; // 0 procedural/no VB, 1 cube, 2 quad, 3 tetrahedron, 4 sphere
    bool color_write = true;
    bool depth_test = true;
    bool depth_write = true;
    bool alpha_blend = false;
    bool cull_back = true;
    bool unsupported_bindings = false;
    float pos[3] = {0,0,0};
    float rotq[4] = {0,0,0,1};
    float scale[3] = {1,1,1};
    bool clear_color_enabled = true;
    float clear_color[4] = {0,0,0,1};
    bool clear_depth = true;
    float depth_clear = 1.0f;
    int vertex_count = 3;
    int instance_count = 1;
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

struct Project {
    float camera[8] = {5,5,5,-2.3561945f,-0.6154797f,1.047f,0.001f,100.0f};
    float dirlight[10] = {5,5,5,0,0,0,1,0.95f,0.9f,1};
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
            for (int i = 0; i < 10; i++) p.dirlight[i] = tokf(t, (size_t)i + 1, p.dirlight[i]);
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
            } else if (kind == "render_texture" || kind == "render_texture3d") {
                r.kind = RK_RT; r.name = t[2];
            } else if (kind == "structured_buffer") {
                r.kind = RK_BUFFER; r.name = t[2];
            }
            if (!r.name.empty()) p.resources.push_back(r);
        } else if (tag == "user_var" && t.size() >= 4) {
            UserVarDef u;
            u.name = t[1]; u.type = parse_val_type(t[2]);
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
            if (tag == "mesh_shader" && t.size() >= 3) {
                cur->mesh = ref_name(t[1]);
                cur->shader = ref_name(t[2]);
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
            } else if (tag == "vertex_count" && t.size() >= 2) {
                cur->vertex_count = toki(t,1,3);
            } else if (tag == "instance" && t.size() >= 2) {
                cur->instance_count = toki(t,1,1);
            } else if (tag == "textures" && t.size() > 1 && toki(t,1,0) > 0) {
                cur->unsupported_bindings = true;
                warnf("%s: texture bindings are not supported by the 64k procedural build", cur->name.c_str());
            } else if ((tag == "srvs" || tag == "uavs") && t.size() > 1 && toki(t,1,0) > 0) {
                cur->unsupported_bindings = true;
                warnf("%s: %s bindings are not supported by this 64k procedural path", cur->name.c_str(), tag.c_str());
            } else if (tag == "param" && t.size() >= 5) {
                ParamDef pd;
                pd.name = t[1]; pd.type = parse_val_type(t[2]); pd.enabled = toki(t,3,1) != 0;
                for (int i = 0; i < 4; i++) pd.ival[i] = toki(t, (size_t)5 + i, 0);
                for (int i = 0; i < 4; i++) pd.fval[i] = tokf(t, (size_t)9 + i, 0.0f);
                cur->params.push_back(pd);
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
            int n = 0;
            if (cur_track->kind == TK_COMMAND_TRANSFORM) n = 10;
            else if (cur_track->kind == TK_COMMAND_ENABLED) n = 1;
            else if (cur_track->kind == TK_CAMERA) n = 8;
            else if (cur_track->kind == TK_DIRLIGHT) n = 10;
            else n = val_components(cur_track->type);
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

static int primitive_mesh_kind(const std::string& prim) {
    if (prim == "cube") return 1;
    if (prim == "quad") return 2;
    if (prim == "tetrahedron") return 3;
    if (prim == "sphere") return 4;
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
    const char* named_structs[] = { "M4", "SceneCB", "UserCB", "ObjectCB", "Sh", "Cmd", "Uv", "Key", "Tr", "Q" };
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

static void emit_generated_c(const Project& p, const std::string& lt_path, const std::string& out_c) {
    std::string lt_dir = path_dir(lt_path);
    std::vector<std::string> roots;
    roots.push_back(".");
    roots.push_back(lt_dir);
    roots.push_back(path_join(lt_dir, ".."));
    roots.push_back(path_join(lt_dir, "../.."));

    std::vector<int> shader_res_indices;
    std::vector<std::string> shader_sources;
    for (size_t i = 0; i < p.resources.size(); i++) {
        const ResourceDef& r = p.resources[i];
        if (r.kind == RK_SHADER_VSPS) {
            std::string sp = find_file_for_path(r.path, roots);
            if (sp.empty()) die("cannot find shader '%s' for resource '%s'", r.path.c_str(), r.name.c_str());
            shader_res_indices.push_back((int)i);
            shader_sources.push_back(expand_hlsl_includes(sp, roots));
        } else if (r.kind == RK_TEXTURE || r.kind == RK_MESH_FILE) {
            warnf("resource '%s' is not part of the 64k path (%s)", r.name.c_str(), r.path.c_str());
        }
    }
    if (shader_sources.empty()) die("no shader_vsps resources found");

    std::vector<CommandDef> out_cmds;
    for (size_t i = 0; i < p.commands.size(); i++) {
        CommandDef c = p.commands[i];
        if (c.type == CT_CLEAR) { out_cmds.push_back(c); continue; }
        if (c.type == CT_DRAW || c.type == CT_DRAW_INSTANCED) {
            if (c.unsupported_bindings) { warnf("skipping draw '%s': texture/SRV/UAV bindings are not 64k-procedural assets", c.name.c_str()); continue; }
            int sh = shader_resource_index(p, c.shader, shader_res_indices);
            if (sh < 0) { warnf("skipping draw '%s': shader not found/supported", c.name.c_str()); continue; }
            // Names are deliberately ignored here. A draw is exported when its actual inputs are 64k-safe:
            // either explicit draw_source procedural, or a mesh_primitive generated by lazyTool.
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
                        c.mesh_kind = primitive_mesh_kind(prim); // fullscreen_triangle stays 0 / SV_VertexID path
                    }
                } else if (mr >= 0 && p.resources[mr].kind == RK_MESH_FILE) {
                    warnf("skipping draw '%s': mesh resource '%s' is external geometry, not a 64k procedural primitive", c.name.c_str(), c.mesh.c_str());
                    continue;
                }
            }
            if (!ok_proc) {
                warnf("skipping draw '%s': draw uses no supported 64k procedural source (use draw_source procedural or resource mesh_primitive)", c.name.c_str());
                continue;
            }
            out_cmds.push_back(c);
        } else if (c.type == CT_DISPATCH) {
            warnf("skipping compute '%s': compute is intentionally not in this first tiny player", c.name.c_str());
        }
    }
    if (out_cmds.empty()) warnf("the exported player has no render commands");

    std::map<std::string,int> cmd_out_index;
    for (size_t i = 0; i < out_cmds.size(); i++) cmd_out_index[out_cmds[i].name] = (int)i;

    struct FlatTrack { int kind, target, type, integral, start, count, enabled; };
    std::vector<FlatTrack> flat_tracks;
    std::vector<TimelineKeyDef> flat_keys;
    for (size_t i = 0; i < p.tracks.size(); i++) {
        const TimelineTrackDef& tr = p.tracks[i];
        if (!tr.enabled || tr.keys.empty()) continue;
        FlatTrack ft = {};
        ft.kind = (int)tr.kind;
        ft.type = (int)tr.type;
        ft.integral = (val_integral(tr.type) || tr.kind == TK_COMMAND_ENABLED) ? 1 : 0;
        ft.start = (int)flat_keys.size();
        ft.count = (int)tr.keys.size();
        ft.enabled = tr.enabled ? 1 : 0;
        ft.target = -1;
        if (tr.kind == TK_COMMAND_TRANSFORM || tr.kind == TK_COMMAND_ENABLED) {
            std::map<std::string,int>::iterator it = cmd_out_index.find(tr.target);
            if (it == cmd_out_index.end()) continue;
            ft.target = it->second;
        } else if (tr.kind == TK_USER_VAR) {
            ft.target = find_user_var(p, tr.target);
            if (ft.target < 0) continue;
        }
        for (size_t k = 0; k < tr.keys.size(); k++) flat_keys.push_back(tr.keys[k]);
        flat_tracks.push_back(ft);
    }

    FILE* f = std::fopen(out_c.c_str(), "wb");
    if (!f) die("cannot write %s", out_c.c_str());

    fprintf(f, "// generated by lazyTool build64k.cpp from %s\n", lt_path.c_str());
    fprintf(f, "#define WIN32_LEAN_AND_MEAN\n#define COBJMACROS\n#include <windows.h>\n#include <stddef.h>\n#include <d3d11.h>\n#include <d3dcompiler.h>\n\n");
    fprintf(f, "#define LT_WNDCLS \"lt64k_window\"\n#define LT_PI 3.14159265358979323846f\n");
    fprintf(f, "int _fltused=0; typedef unsigned int u32; typedef struct { float m[16]; } M4;\n");
    fprintf(f, "void* __cdecl memset(void* d,int c,size_t n){ unsigned char* p=(unsigned char*)d; while(n--)*p++=(unsigned char)c; return d; }\n");
    fprintf(f, "void* __cdecl memcpy(void* d,const void* s,size_t n){ unsigned char* p=(unsigned char*)d; const unsigned char* q=(const unsigned char*)s; while(n--)*p++=*q++; return d; }\n");
    fprintf(f, "typedef struct { float view_proj[16]; float time_vec[4]; float light_dir[4]; float light_color[4]; float cam_pos[4]; float shadow_view_proj[16]; float inv_view_proj[16]; float prev_view_proj[16]; float prev_inv_view_proj[16]; float prev_shadow_view_proj[16]; float cam_dir[4]; float shadow_cascade_splits[4]; float shadow_params[4]; float shadow_cascade_rects[4][4]; float shadow_cascade_view_proj[4][16]; } SceneCB;\n");
    fprintf(f, "typedef struct { float slots[64][4]; } UserCB; typedef struct { float world[16]; } ObjectCB;\n");
    fprintf(f, "static ID3D11Device* dev; static ID3D11DeviceContext* ctx; static IDXGISwapChain* swp; static ID3D11RenderTargetView* rtv; static ID3D11DepthStencilView* dsv; static ID3D11Buffer* scene_cb; static ID3D11Buffer* object_cb; static ID3D11Buffer* user_cb; static ID3D11Buffer* prim_vb[5]; static int W,H;\n");
    fprintf(f, "typedef struct { const char* src; unsigned int len; ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11InputLayout* il; } Sh;\n");

    for (size_t i = 0; i < shader_sources.size(); i++) {
        fprintf(f, "static const char sh_src_%u[] =\n%s;\n\n", (unsigned)i, cstr_literal(shader_sources[i]).c_str());
    }
    fprintf(f, "static Sh sh[%u] = {\n", (unsigned)shader_sources.size());
    for (size_t i = 0; i < shader_sources.size(); i++) fprintf(f, " { sh_src_%u, sizeof(sh_src_%u)-1, 0, 0, 0 },\n", (unsigned)i, (unsigned)i);
    fprintf(f, "};\n");

    fprintf(f, "typedef struct { int type,enabled,shader,topology,mk,vc,ic,ccen,cden; float cc[4],dc,pos[3],q[4],scl[3]; } Cmd;\n");
    fprintf(f, "static Cmd cmd[%u] = {\n", (unsigned)(out_cmds.empty() ? 1 : out_cmds.size()));
    if (out_cmds.empty()) fprintf(f, " {0,0,-1,1,0,0,0,0,0,{0.0f,0.0f,0.0f,1.0f},1.0f,{0.0f,0.0f,0.0f},{0.0f,0.0f,0.0f,1.0f},{1.0f,1.0f,1.0f}},\n");
    for (size_t i = 0; i < out_cmds.size(); i++) {
        const CommandDef& c = out_cmds[i];
        int shidx = shader_resource_index(p, c.shader, shader_res_indices);
        fprintf(f, " {%d,%d,%d,%d,%d,%d,%d,%d,%d,{", (c.type == CT_CLEAR ? 1 : 2), c.enabled?1:0, shidx, c.topology, c.mesh_kind, c.vertex_count, c.instance_count, c.clear_color_enabled?1:0, c.clear_depth?1:0);
        emit_float_array(f, c.clear_color, 4); fprintf(f, "},"); emit_float_literal(f, c.depth_clear); fprintf(f, ",{");
        emit_float_array(f, c.pos, 3); fprintf(f, "},{"); emit_float_array(f, c.rotq, 4); fprintf(f, "},{"); emit_float_array(f, c.scale, 3); fprintf(f, "}},\n");
    }
    fprintf(f, "};\n");

    fprintf(f, "typedef struct { int type,src,src_cmd; int iv[4]; float fv[4]; } Uv;\n");
    fprintf(f, "static Uv uv[%u] = {\n", (unsigned)(p.user_vars.empty() ? 1 : p.user_vars.size()));
    if (p.user_vars.empty()) fprintf(f, " {0,0,-1,{0,0,0,0},{0.0f,0.0f,0.0f,0.0f}},\n");
    for (size_t i = 0; i < p.user_vars.size(); i++) {
        const UserVarDef& u = p.user_vars[i];
        int src_cmd = -1;
        if (u.source_kind == SRC_CMD_POS || u.source_kind == SRC_CMD_ROT || u.source_kind == SRC_CMD_SCALE) {
            std::map<std::string,int>::iterator it = cmd_out_index.find(u.source_target);
            if (it != cmd_out_index.end()) src_cmd = it->second;
        }
        fprintf(f, " {%d,%d,%d,{", (int)u.type, (int)u.source_kind, src_cmd); emit_int_array(f, u.ival, 4); fprintf(f, "},{"); emit_float_array(f, u.fval, 4); fprintf(f, "}},\n");
    }
    fprintf(f, "};\n");

    fprintf(f, "typedef struct { int fr; int iv[4]; float fv[16]; } Key; typedef struct { int kind,target,type,integral,start,count,enabled; } Tr;\n");
    fprintf(f, "static Key key[%u] = {\n", (unsigned)(flat_keys.empty() ? 1 : flat_keys.size()));
    if (flat_keys.empty()) fprintf(f, " {0,{0,0,0,0},{0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f}},\n");
    for (size_t i = 0; i < flat_keys.size(); i++) {
        const TimelineKeyDef& k = flat_keys[i];
        fprintf(f, " {%d,{", k.frame); emit_int_array(f, k.ival, 4); fprintf(f, "},{"); emit_float_array(f, k.fval, 16); fprintf(f, "}},\n");
    }
    fprintf(f, "};\nstatic Tr tr[%u] = {\n", (unsigned)(flat_tracks.empty() ? 1 : flat_tracks.size()));
    if (flat_tracks.empty()) fprintf(f, " {0,-1,0,0,0,0,0},\n");
    for (size_t i = 0; i < flat_tracks.size(); i++) {
        const FlatTrack& t = flat_tracks[i];
        fprintf(f, " {%d,%d,%d,%d,%d,%d,%d},\n", t.kind,t.target,t.type,t.integral,t.start,t.count,t.enabled);
    }
    fprintf(f, "};\n");
    fprintf(f, "static float cam[8]={"); emit_float_array(f, p.camera, 8); fprintf(f, "}; static float dl[10]={"); emit_float_array(f, p.dirlight, 10); fprintf(f, "};\n");
    fprintf(f, "enum{ CMDN=%u, SHN=%u, UVN=%u, TRN=%u, KEYN=%u, TL_FPS=%d, TL_LEN=%d, TL_LOOP=%d, TL_ON=%d, TL_LERP=%d };\n",
            (unsigned)out_cmds.size(), (unsigned)shader_sources.size(), (unsigned)p.user_vars.size(), (unsigned)flat_tracks.size(), (unsigned)flat_keys.size(), p.timeline_fps, p.timeline_length, p.timeline_loop?1:0, p.timeline_enabled?1:0, p.timeline_interpolate?1:0);

    // Runtime support. Written as plain C and deliberately compact.
    fputs(R"LT64K(
static void zmem(void* p, int n){ unsigned char* b=(unsigned char*)p; while(n--)*b++=0; }
static void cpy(void* d,const void* s,int n){ unsigned char* a=(unsigned char*)d; const unsigned char* b=(const unsigned char*)s; while(n--)*a++=*b++; }
static float ab(float x){ return x<0?-x:x; }
static float cl(float x,float a,float b){ return x<a?a:(x>b?b:x); }
static float wrap(float x){ while(x>LT_PI)x-=6.28318530718f; while(x<-LT_PI)x+=6.28318530718f; return x; }
static float sn(float x){ x=wrap(x); float x2=x*x; return x*(1.0f-x2*(0.166666667f-x2*(0.00833333333f-x2*0.000198412698f))); }
static float cs(float x){ return sn(x+1.57079632679f); }
static float lerp(float a,float b,float t){return a+(b-a)*t;} static float lerpa(float a,float b,float t){float d=wrap(b-a);return wrap(a+d*t);}
static float rsq(float x){ union{float f; unsigned int i;} u; float xh=x*0.5f; if(x<=0) return 0; u.f=x; u.i=0x5f3759df-(u.i>>1); u.f=u.f*(1.5f-xh*u.f*u.f); return u.f; }
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
static void viewproj(M4* out,float tsec){ float cp=cs(cam[4]), fwd[3]={sn(cam[3])*cp,sn(cam[4]),cs(cam[3])*cp}; float eye[3]={cam[0],cam[1],cam[2]}, at[3]={eye[0]+fwd[0],eye[1]+fwd[1],eye[2]+fwd[2]}, up[3]={0,1,0}; float z[3]={eye[0]-at[0],eye[1]-at[1],eye[2]-at[2]}; float iz=rsq(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]); z[0]*=iz;z[1]*=iz;z[2]*=iz; float x[3]={z[1]*up[2]-z[2]*up[1],z[2]*up[0]-z[0]*up[2],z[0]*up[1]-z[1]*up[0]}; float ix=rsq(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]); x[0]*=ix;x[1]*=ix;x[2]*=ix; float y[3]={x[1]*z[2]-x[2]*z[1],x[2]*z[0]-x[0]*z[2],x[0]*z[1]-x[1]*z[0]}; M4 v,p; mid(&v); zmem(&p,sizeof(p)); v.m[0]=x[0];v.m[1]=y[0];v.m[2]=z[0]; v.m[4]=x[1];v.m[5]=y[1];v.m[6]=z[1]; v.m[8]=x[2];v.m[9]=y[2];v.m[10]=z[2]; v.m[12]=-(x[0]*eye[0]+x[1]*eye[1]+x[2]*eye[2]); v.m[13]=-(y[0]*eye[0]+y[1]*eye[1]+y[2]*eye[2]); v.m[14]=-(z[0]*eye[0]+z[1]*eye[1]+z[2]*eye[2]); float f=cs(cam[5]*0.5f)/sn(cam[5]*0.5f); float asp=(float)W/(float)H; p.m[0]=f/asp; p.m[5]=f; p.m[10]=cam[7]/(cam[6]-cam[7]); p.m[11]=-1; p.m[14]=(cam[6]*cam[7])/(cam[6]-cam[7]); *out=mmul(v,p); }
static int comps(int ty){ return (ty==1||ty==4)?1:(ty==2||ty==5)?2:(ty==3||ty==6)?3:(ty==7)?4:0; }
static int isint(int ty){ return ty>=1&&ty<=3; }
)LT64K", f);
    fputs(R"LT64K(static void apply_track(Tr* r, float fr){ int a=-1,b=-1,i,n=r->count; Q qq; for(i=0;i<n;i++){ if((float)key[r->start+i].fr<=fr)a=i; if((float)key[r->start+i].fr>=fr){b=i;break;} } if(a<0)a=b>=0?b:0; if(b<0)b=a; Key A=key[r->start+a],B=key[r->start+b],O=A; float tt=0; if(a!=b && !r->integral){ int span=B.fr-A.fr; tt=span>0?(fr-(float)A.fr)/(float)span:0; tt=cl(tt,0,1); if(r->kind==2){ for(i=0;i<3;i++)O.fv[i]=lerp(A.fv[i],B.fv[i],tt); qq=ql(qfa(A.fv+3),qfa(B.fv+3),tt); O.fv[3]=qq.x;O.fv[4]=qq.y;O.fv[5]=qq.z;O.fv[6]=qq.w; for(i=7;i<10;i++)O.fv[i]=lerp(A.fv[i],B.fv[i],tt); } else if(r->kind==4){ for(i=0;i<8;i++)O.fv[i]=(i==3)?lerpa(A.fv[i],B.fv[i],tt):lerp(A.fv[i],B.fv[i],tt); } else { int cnt=(r->kind==5)?10:comps(r->type); if(r->kind==1 && r->target>=0&&r->target<UVN && uv[r->target].src==3){ if(uv[r->target].type==7){ qq=ql(qfa(A.fv),qfa(B.fv),tt); O.fv[0]=qq.x;O.fv[1]=qq.y;O.fv[2]=qq.z;O.fv[3]=qq.w; } else { for(i=0;i<cnt;i++)O.fv[i]=lerp(A.fv[i],B.fv[i],tt); } } else for(i=0;i<cnt;i++)O.fv[i]=lerp(A.fv[i],B.fv[i],tt); } } if(r->kind==1 && r->target>=0&&r->target<UVN){ int c=comps(uv[r->target].type); for(i=0;i<c&&i<4;i++){ if(isint(uv[r->target].type))uv[r->target].iv[i]=O.iv[i]; else uv[r->target].fv[i]=O.fv[i]; } if(uv[r->target].src_cmd>=0&&uv[r->target].src_cmd<CMDN){ Cmd* cc=cmd+uv[r->target].src_cmd; if(uv[r->target].src==2)for(i=0;i<3;i++)cc->pos[i]=O.fv[i]; else if(uv[r->target].src==3){ qq=uv[r->target].type==7?qfa(O.fv):qfe(O.fv); cc->q[0]=qq.x;cc->q[1]=qq.y;cc->q[2]=qq.z;cc->q[3]=qq.w; } else if(uv[r->target].src==4)for(i=0;i<3;i++)cc->scl[i]=O.fv[i]; } else if(uv[r->target].src==6){ cam[3]=wrap(O.fv[0]); cam[4]=cl(O.fv[1],-1.50f,1.50f); } } else if(r->kind==2 && r->target>=0&&r->target<CMDN){ Cmd* c=cmd+r->target; for(i=0;i<3;i++)c->pos[i]=O.fv[i]; for(i=0;i<4;i++)c->q[i]=O.fv[3+i]; for(i=0;i<3;i++)c->scl[i]=O.fv[7+i]; } else if(r->kind==3 && r->target>=0&&r->target<CMDN){ cmd[r->target].enabled=O.iv[0]!=0; } else if(r->kind==4){ for(i=0;i<8;i++)cam[i]=O.fv[i]; cam[3]=wrap(cam[3]); cam[4]=cl(cam[4],-1.50f,1.50f); } else if(r->kind==5){ for(i=0;i<10;i++)dl[i]=O.fv[i]; } }
static void timeline(float sec){ if(!TL_ON||TL_LEN<1)return; float fr=sec*(float)TL_FPS; if(TL_LOOP)while(fr>=(float)TL_LEN)fr-=(float)TL_LEN; if(fr>(float)(TL_LEN-1))fr=(float)(TL_LEN-1); if(!TL_LERP)fr=(float)((int)fr); for(int i=0;i<TRN;i++)if(tr[i].enabled)apply_track(tr+i,fr); }
static void fill_user(UserCB* u){ int i,j; zmem(u,sizeof(*u)); for(i=0;i<UVN&&i<64;i++){ if(isint(uv[i].type)) cpy(u->slots[i],uv[i].iv,comps(uv[i].type)*4); else for(j=0;j<comps(uv[i].type);j++)u->slots[i][j]=uv[i].fv[j]; } }
static HRESULT compile(const char* src, unsigned int len, const char* e, const char* m, ID3DBlob** b){ ID3DBlob* er=0; HRESULT hr=D3DCompile(src,len,0,0,0,e,m,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,b,&er); if(er)ID3D10Blob_Release(er); return hr; }
static void init_shaders(){ D3D11_INPUT_ELEMENT_DESC il[3]; zmem(il,sizeof(il)); il[0].SemanticName="POSITION";il[0].Format=DXGI_FORMAT_R32G32B32_FLOAT;il[0].AlignedByteOffset=0;il[0].InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA; il[1].SemanticName="NORMAL";il[1].Format=DXGI_FORMAT_R32G32B32_FLOAT;il[1].AlignedByteOffset=12;il[1].InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA; il[2].SemanticName="TEXCOORD";il[2].Format=DXGI_FORMAT_R32G32_FLOAT;il[2].AlignedByteOffset=24;il[2].InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA; for(int i=0;i<SHN;i++){ ID3DBlob *vs=0,*ps=0; if(SUCCEEDED(compile(sh[i].src,sh[i].len,"VSMain","vs_5_0",&vs))&&SUCCEEDED(compile(sh[i].src,sh[i].len,"PSMain","ps_5_0",&ps))){ ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),0,&sh[i].vs); ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(ps),ID3D10Blob_GetBufferSize(ps),0,&sh[i].ps); ID3D11Device_CreateInputLayout(dev,il,3,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),&sh[i].il); } if(vs)ID3D10Blob_Release(vs); if(ps)ID3D10Blob_Release(ps); } }
static void mkbuf(int id,float* v,unsigned int bytes){ D3D11_BUFFER_DESC d; D3D11_SUBRESOURCE_DATA s; zmem(&d,sizeof(d)); zmem(&s,sizeof(s)); d.ByteWidth=bytes; d.BindFlags=D3D11_BIND_VERTEX_BUFFER; s.pSysMem=v; ID3D11Device_CreateBuffer(dev,&d,&s,&prim_vb[id]); }
static void pv(float* v,int* n,float x,float y,float z,float nx,float ny,float nz,float u,float vv){ int i=*n; v[i+0]=x;v[i+1]=y;v[i+2]=z;v[i+3]=nx;v[i+4]=ny;v[i+5]=nz;v[i+6]=u;v[i+7]=vv; *n=i+8; }
static void init_prims(){ static float cube[]={-1,-1,-1,0,0,-1,0,1, 1,1,-1,0,0,-1,1,0, 1,-1,-1,0,0,-1,1,1, -1,-1,-1,0,0,-1,0,1, -1,1,-1,0,0,-1,0,0, 1,1,-1,0,0,-1,1,0, -1,-1,1,0,0,1,0,1, 1,-1,1,0,0,1,1,1, 1,1,1,0,0,1,1,0, -1,-1,1,0,0,1,0,1, 1,1,1,0,0,1,1,0, -1,1,1,0,0,1,0,0, -1,-1,-1,-1,0,0,0,1, -1,-1,1,-1,0,0,1,1, -1,1,1,-1,0,0,1,0, -1,-1,-1,-1,0,0,0,1, -1,1,1,-1,0,0,1,0, -1,1,-1,-1,0,0,0,0, 1,-1,-1,1,0,0,0,1, 1,1,1,1,0,0,1,0, 1,-1,1,1,0,0,1,1, 1,-1,-1,1,0,0,0,1, 1,1,-1,1,0,0,0,0, 1,1,1,1,0,0,1,0, -1,-1,-1,0,-1,0,0,1, 1,-1,-1,0,-1,0,1,1, 1,-1,1,0,-1,0,1,0, -1,-1,-1,0,-1,0,0,1, 1,-1,1,0,-1,0,1,0, -1,-1,1,0,-1,0,0,0, -1,1,-1,0,1,0,0,1, -1,1,1,0,1,0,0,0, 1,1,1,0,1,0,1,0, -1,1,-1,0,1,0,0,1, 1,1,1,0,1,0,1,0, 1,1,-1,0,1,0,1,1};
 static float quad[]={-1,-1,0,0,0,1,0,1, 1,-1,0,0,0,1,1,1, 1,1,0,0,0,1,1,0, -1,-1,0,0,0,1,0,1, 1,1,0,0,0,1,1,0, -1,1,0,0,0,1,0,0};
 static float tet[]={1,1,1,0.57735f,0.57735f,0.57735f,0.5f,0, -1,-1,1,0.57735f,0.57735f,0.57735f,0,1, -1,1,-1,0.57735f,0.57735f,0.57735f,1,1, 1,1,1,0.57735f,-0.57735f,0.57735f,0.5f,0, 1,-1,-1,0.57735f,-0.57735f,0.57735f,0,1, -1,-1,1,0.57735f,-0.57735f,0.57735f,1,1, 1,1,1,0.57735f,0.57735f,-0.57735f,0.5f,0, -1,1,-1,0.57735f,0.57735f,-0.57735f,0,1, 1,-1,-1,0.57735f,0.57735f,-0.57735f,1,1, -1,-1,1,-0.57735f,-0.57735f,-0.57735f,0.5f,0, 1,-1,-1,-0.57735f,-0.57735f,-0.57735f,0,1, -1,1,-1,-0.57735f,-0.57735f,-0.57735f,1,1};
)LT64K", f);
    fputs(R"LT64K( static float sph[2880*8]; int n=0; for(int r=0;r<16;r++){ for(int s=0;s<32;s++){ float v0=(float)r/16.0f,v1=(float)(r+1)/16.0f,u0=(float)s/32.0f,u1=(float)(s+1)/32.0f; float th0=v0*LT_PI,th1=v1*LT_PI,ph0=u0*6.28318530718f,ph1=u1*6.28318530718f; float sr0=sn(th0),sr1=sn(th1); float p0[8]={sr0*cs(ph0),cs(th0),sr0*sn(ph0),sr0*cs(ph0),cs(th0),sr0*sn(ph0),u0,v0}; float p1[8]={sr1*cs(ph0),cs(th1),sr1*sn(ph0),sr1*cs(ph0),cs(th1),sr1*sn(ph0),u0,v1}; float p2[8]={sr0*cs(ph1),cs(th0),sr0*sn(ph1),sr0*cs(ph1),cs(th0),sr0*sn(ph1),u1,v0}; float p3[8]={sr1*cs(ph1),cs(th1),sr1*sn(ph1),sr1*cs(ph1),cs(th1),sr1*sn(ph1),u1,v1}; if(r==0){pv(sph,&n,p0[0],p0[1],p0[2],p0[3],p0[4],p0[5],p0[6],p0[7]);pv(sph,&n,p1[0],p1[1],p1[2],p1[3],p1[4],p1[5],p1[6],p1[7]);pv(sph,&n,p3[0],p3[1],p3[2],p3[3],p3[4],p3[5],p3[6],p3[7]);} else if(r==15){pv(sph,&n,p0[0],p0[1],p0[2],p0[3],p0[4],p0[5],p0[6],p0[7]);pv(sph,&n,p1[0],p1[1],p1[2],p1[3],p1[4],p1[5],p1[6],p1[7]);pv(sph,&n,p2[0],p2[1],p2[2],p2[3],p2[4],p2[5],p2[6],p2[7]);} else {pv(sph,&n,p0[0],p0[1],p0[2],p0[3],p0[4],p0[5],p0[6],p0[7]);pv(sph,&n,p1[0],p1[1],p1[2],p1[3],p1[4],p1[5],p1[6],p1[7]);pv(sph,&n,p2[0],p2[1],p2[2],p2[3],p2[4],p2[5],p2[6],p2[7]);pv(sph,&n,p2[0],p2[1],p2[2],p2[3],p2[4],p2[5],p2[6],p2[7]);pv(sph,&n,p1[0],p1[1],p1[2],p1[3],p1[4],p1[5],p1[6],p1[7]);pv(sph,&n,p3[0],p3[1],p3[2],p3[3],p3[4],p3[5],p3[6],p3[7]);} } }
 mkbuf(1,cube,sizeof(cube)); mkbuf(2,quad,sizeof(quad)); mkbuf(3,tet,sizeof(tet)); mkbuf(4,sph,(unsigned int)(n*4)); }
static void cb_make(ID3D11Buffer** b, unsigned int sz){ D3D11_BUFFER_DESC d; zmem(&d,sizeof(d)); d.ByteWidth=(sz+15)&~15; d.Usage=D3D11_USAGE_DYNAMIC; d.BindFlags=D3D11_BIND_CONSTANT_BUFFER; d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE; ID3D11Device_CreateBuffer(dev,&d,0,b); }
static void cb_up(ID3D11Buffer* b, void* data, unsigned int sz){ D3D11_MAPPED_SUBRESOURCE m; if(SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)b,0,D3D11_MAP_WRITE_DISCARD,0,&m))){ cpy(m.pData,data,sz); ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)b,0); } }
static void render(float sec){ timeline(sec); float clr[4]={0,0,0,1}; ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&rtv,dsv); SceneCB sc; UserCB uc; ObjectCB oc; M4 vp; zmem(&sc,sizeof(sc)); viewproj(&vp,sec); cpy(sc.view_proj,vp.m,64); cpy(sc.prev_view_proj,vp.m,64); sc.time_vec[0]=sec; sc.time_vec[2]=sec*60.0f; sc.cam_pos[0]=cam[0];sc.cam_pos[1]=cam[1];sc.cam_pos[2]=cam[2]; float cp=cs(cam[4]); sc.cam_dir[0]=sn(cam[3])*cp;sc.cam_dir[1]=sn(cam[4]);sc.cam_dir[2]=cs(cam[3])*cp; float ldx=dl[3]-dl[0],ldy=dl[4]-dl[1],ldz=dl[5]-dl[2],li=rsq(ldx*ldx+ldy*ldy+ldz*ldz); sc.light_dir[0]=ldx*li;sc.light_dir[1]=ldy*li;sc.light_dir[2]=ldz*li;sc.light_dir[3]=dl[9]; sc.light_color[0]=dl[6];sc.light_color[1]=dl[7];sc.light_color[2]=dl[8]; cb_up(scene_cb,&sc,sizeof(sc)); fill_user(&uc); cb_up(user_cb,&uc,sizeof(uc)); ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,clr); if(dsv)ID3D11DeviceContext_ClearDepthStencilView(ctx,dsv,D3D11_CLEAR_DEPTH,1,0); for(int i=0;i<CMDN;i++){ Cmd* c=cmd+i; if(!c->enabled)continue; if(c->type==1){ if(c->ccen)ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,c->cc); if(c->cden&&dsv)ID3D11DeviceContext_ClearDepthStencilView(ctx,dsv,D3D11_CLEAR_DEPTH,c->dc,0); } else if(c->type==2 && c->shader>=0&&c->shader<SHN&&sh[c->shader].vs&&sh[c->shader].ps){ M4 w; world(c,&w); cpy(oc.world,w.m,64); cb_up(object_cb,&oc,sizeof(oc)); ID3D11DeviceContext_VSSetConstantBuffers(ctx,0,1,&scene_cb); ID3D11DeviceContext_PSSetConstantBuffers(ctx,0,1,&scene_cb); ID3D11DeviceContext_VSSetConstantBuffers(ctx,1,1,&object_cb); ID3D11DeviceContext_PSSetConstantBuffers(ctx,1,1,&object_cb); ID3D11DeviceContext_VSSetConstantBuffers(ctx,2,1,&user_cb); ID3D11DeviceContext_PSSetConstantBuffers(ctx,2,1,&user_cb); if(c->mk>0&&c->mk<5&&prim_vb[c->mk]){ unsigned int st=32,off=0; ID3D11DeviceContext_IASetInputLayout(ctx,sh[c->shader].il); ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&prim_vb[c->mk],&st,&off); } else { ID3D11DeviceContext_IASetInputLayout(ctx,0); ID3D11DeviceContext_IASetVertexBuffers(ctx,0,0,0,0,0); } ID3D11DeviceContext_IASetIndexBuffer(ctx,0,DXGI_FORMAT_R32_UINT,0); ID3D11DeviceContext_IASetPrimitiveTopology(ctx,c->topology?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D11_PRIMITIVE_TOPOLOGY_POINTLIST); ID3D11DeviceContext_VSSetShader(ctx,sh[c->shader].vs,0,0); ID3D11DeviceContext_PSSetShader(ctx,sh[c->shader].ps,0,0); ID3D11DeviceContext_DrawInstanced(ctx,c->vc,c->ic,0,0); } } IDXGISwapChain_Present(swp,1,0); }
static LRESULT CALLBACK wp(HWND h,UINT m,WPARAM w,LPARAM l){ if(m==WM_KEYDOWN&&w==VK_ESCAPE)PostQuitMessage(0); if(m==WM_CLOSE||m==WM_DESTROY){PostQuitMessage(0);return 0;} return DefWindowProcA(h,m,w,l); }
)LT64K", f);
    fputs(R"LT64K(void WINAPI WinMainCRTStartup(void){ WNDCLASSA wc; zmem(&wc,sizeof(wc)); wc.lpfnWndProc=wp; wc.hInstance=GetModuleHandleA(0); wc.lpszClassName=LT_WNDCLS; RegisterClassA(&wc); W=GetSystemMetrics(SM_CXSCREEN); H=GetSystemMetrics(SM_CYSCREEN); HWND hw=CreateWindowExA(0,LT_WNDCLS,"lt64k",WS_POPUP|WS_VISIBLE,0,0,W,H,0,0,wc.hInstance,0); ShowCursor(FALSE); DXGI_SWAP_CHAIN_DESC sd; zmem(&sd,sizeof(sd)); sd.BufferCount=1; sd.BufferDesc.Width=W; sd.BufferDesc.Height=H; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=hw; sd.SampleDesc.Count=1; sd.Windowed=TRUE; if(FAILED(D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&sd,&swp,&dev,0,&ctx)))ExitProcess(1); ID3D11Texture2D* bb=0; IDXGISwapChain_GetBuffer(swp,0,&IID_ID3D11Texture2D,(void**)&bb); ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)bb,0,&rtv); ID3D11Texture2D_Release(bb); D3D11_TEXTURE2D_DESC dd; zmem(&dd,sizeof(dd)); dd.Width=W;dd.Height=H;dd.MipLevels=1;dd.ArraySize=1;dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dd.SampleDesc.Count=1;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL; ID3D11Texture2D* dt=0; if(SUCCEEDED(ID3D11Device_CreateTexture2D(dev,&dd,0,&dt))){ID3D11Device_CreateDepthStencilView(dev,(ID3D11Resource*)dt,0,&dsv);ID3D11Texture2D_Release(dt);} D3D11_VIEWPORT vp; zmem(&vp,sizeof(vp)); vp.Width=(float)W;vp.Height=(float)H;vp.MinDepth=0;vp.MaxDepth=1; ID3D11DeviceContext_RSSetViewports(ctx,1,&vp); cb_make(&scene_cb,sizeof(SceneCB)); cb_make(&object_cb,sizeof(ObjectCB)); cb_make(&user_cb,sizeof(UserCB)); init_shaders(); init_prims(); DWORD t0=GetTickCount(); MSG msg; for(;;){ while(PeekMessageA(&msg,0,0,0,PM_REMOVE)){ if(msg.message==WM_QUIT)ExitProcess(0); TranslateMessage(&msg); DispatchMessageA(&msg);} render((float)(GetTickCount()-t0)*0.001f); } }
)LT64K", f);

    std::fclose(f);
    pretty_format_c_file(out_c);
    fprintf(stderr, "generated %s\n", out_c.c_str());
    fprintf(stderr, "commands: %u, shaders: %u, user vars: %u, timeline tracks: %u\n", (unsigned)out_cmds.size(), (unsigned)shader_sources.size(), (unsigned)p.user_vars.size(), (unsigned)flat_tracks.size());
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
