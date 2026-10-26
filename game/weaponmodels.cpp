// Real weapon meshes (x86, inside swkotor.exe). Files come from tools/weapon_export.py (see its header for the
// .kvrw / .kvrt layouts). Models and textures load lazily on first use and stay cached; drawing uses immediate
// mode (after the world pass, so the own-body hooks ignore it) with the game's own textures.

#include "weaponmodels.h"

#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace {

struct WMesh {
    std::string texture;
    unsigned flags = 0;  // 1 = additive saber blade
    std::vector<float> pos, uv;
    std::vector<unsigned short> idx;
};
struct WModel {
    bool ok = false;
    std::vector<WMesh> meshes;
};

std::string g_dir;
std::map<int, std::string> g_prefix;  // base item row -> model prefix (w_lghtsbr)
std::map<std::string, WModel> g_models;
std::map<std::string, GLuint> g_textures;  // 0 = failed

void(APIENTRY* p_glGenTextures)(GLsizei, GLuint*);
void(APIENTRY* p_glBindTexture)(GLenum, GLuint);
void(APIENTRY* p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void(APIENTRY* p_glTexParameteri)(GLenum, GLenum, GLint);
void(APIENTRY* p_glTexEnvi)(GLenum, GLenum, GLint);
void(APIENTRY* p_glEnable)(GLenum);
void(APIENTRY* p_glDisable)(GLenum);
void(APIENTRY* p_glBlendFunc)(GLenum, GLenum);
void(APIENTRY* p_glDepthMask)(GLboolean);
void(APIENTRY* p_glAlphaFunc)(GLenum, GLclampf);
void(APIENTRY* p_glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat);
void(APIENTRY* p_glBegin)(GLenum);
void(APIENTRY* p_glEnd)();
void(APIENTRY* p_glVertex3fv)(const GLfloat*);
void(APIENTRY* p_glTexCoord2fv)(const GLfloat*);
void(APIENTRY* p_glCompressedTexImage2DARB)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);

bool LoadGl() {
    if (p_glBegin) return true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
#define L(n) p_##n = (decltype(p_##n))GetProcAddress(gl, #n)
    L(glGenTextures); L(glBindTexture); L(glTexImage2D); L(glTexParameteri); L(glTexEnvi); L(glEnable);
    L(glDisable); L(glBlendFunc); L(glDepthMask); L(glAlphaFunc); L(glColor4f); L(glBegin); L(glEnd);
    L(glVertex3fv); L(glTexCoord2fv);
#undef L
    typedef PROC(WINAPI * GPA)(LPCSTR);
    GPA wgpa = (GPA)GetProcAddress(gl, "wglGetProcAddress");
    if (wgpa) p_glCompressedTexImage2DARB = (decltype(p_glCompressedTexImage2DARB))wgpa("glCompressedTexImage2DARB");
    return p_glBegin != nullptr;
}

bool ReadFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

const WModel& GetModel(const std::string& resref) {
    auto it = g_models.find(resref);
    if (it != g_models.end()) return it->second;
    WModel& m = g_models[resref];
    std::vector<unsigned char> d;
    if (!ReadFile(g_dir + "\\kvr\\models\\" + resref + ".kvrw", d) || d.size() < 12 || memcmp(d.data(), "KVRW", 4)) {
        Log("weaponmodels: no model file for %s", resref.c_str());
        return m;
    }
    size_t o = 12;
    unsigned count = *(unsigned*)(d.data() + 8);
    for (unsigned i = 0; i < count && o + 44 <= d.size(); i++) {
        WMesh me;
        me.texture.assign((const char*)d.data() + o, strnlen((const char*)d.data() + o, 32));
        unsigned flags = *(unsigned*)(d.data() + o + 32), vc = *(unsigned*)(d.data() + o + 36), ic = *(unsigned*)(d.data() + o + 40);
        o += 44;
        size_t need = (size_t)vc * 20 + (size_t)ic * 2 + (ic % 2 ? 2 : 0);
        if (o + need > d.size()) break;
        me.flags = flags;
        me.pos.assign((const float*)(d.data() + o), (const float*)(d.data() + o) + vc * 3);
        o += vc * 12;
        me.uv.assign((const float*)(d.data() + o), (const float*)(d.data() + o) + vc * 2);
        o += vc * 8;
        me.idx.assign((const unsigned short*)(d.data() + o), (const unsigned short*)(d.data() + o) + ic);
        o += ic * 2 + (ic % 2 ? 2 : 0);
        m.meshes.push_back(std::move(me));
    }
    m.ok = !m.meshes.empty();
    Log("weaponmodels: loaded %s (%zu meshes)", resref.c_str(), m.meshes.size());
    return m;
}

// ---------------------------------------------------------------- hands (tools/hand_export.py .kvrh)
struct HSeg {
    std::string name;
    int parent;
    float pivot[3];
    std::vector<float> pos, uv;
    std::vector<unsigned short> idx;
};
struct HModel {
    bool ok = false;
    std::string texture;
    std::vector<HSeg> segs;
};
std::map<std::string, HModel> g_hands;
std::map<int, std::vector<std::string>> g_bodies;  // appearance row -> model a..j, tex a..j ("-" = none)

const HModel& GetHand(const std::string& key) {
    auto it = g_hands.find(key);
    if (it != g_hands.end()) return it->second;
    HModel& h = g_hands[key];
    std::vector<unsigned char> d;
    if (!ReadFile(g_dir + "\\kvr\\hands\\" + key + ".kvrh", d) || d.size() < 44 || memcmp(d.data(), "KVRH", 4)) {
        Log("weaponmodels: no hand file for %s", key.c_str());
        return h;
    }
    h.texture.assign((const char*)d.data() + 8, strnlen((const char*)d.data() + 8, 32));
    unsigned n = *(unsigned*)(d.data() + 40);
    size_t o = 44;
    for (unsigned i = 0; i < n && o + 40 <= d.size(); i++) {
        HSeg s;
        s.name.assign((const char*)d.data() + o, strnlen((const char*)d.data() + o, 16));
        s.parent = *(int*)(d.data() + o + 16);
        memcpy(s.pivot, d.data() + o + 20, sizeof s.pivot);
        unsigned vc = *(unsigned*)(d.data() + o + 32), ic = *(unsigned*)(d.data() + o + 36);
        o += 40;
        size_t need = (size_t)vc * 20 + (size_t)ic * 2 + (ic % 2 ? 2 : 0);
        if (o + need > d.size()) break;
        s.pos.assign((const float*)(d.data() + o), (const float*)(d.data() + o) + vc * 3);
        o += vc * 12;
        s.uv.assign((const float*)(d.data() + o), (const float*)(d.data() + o) + vc * 2);
        o += vc * 8;
        s.idx.assign((const unsigned short*)(d.data() + o), (const unsigned short*)(d.data() + o) + ic);
        o += ic * 2 + (ic % 2 ? 2 : 0);
        h.segs.push_back(std::move(s));
    }
    h.ok = !h.segs.empty();
    Log("weaponmodels: loaded hand %s (%zu segments, texture %s)", key.c_str(), h.segs.size(), h.texture.c_str());
    return h;
}

GLuint GetTexture(const std::string& name) {
    auto it = g_textures.find(name);
    if (it != g_textures.end()) return it->second;
    GLuint& id = g_textures[name];
    id = 0;
    std::vector<unsigned char> d;
    if (!ReadFile(g_dir + "\\kvr\\textures\\" + name + ".kvrt", d) || d.size() < 20 || memcmp(d.data(), "KVRT", 4)) return 0;
    unsigned fmt = *(unsigned*)(d.data() + 4), w = *(unsigned*)(d.data() + 8), h = *(unsigned*)(d.data() + 12),
             mips = *(unsigned*)(d.data() + 16);
    if (fmt != 0 && !p_glCompressedTexImage2DARB) return 0;
    p_glGenTextures(1, &id);
    p_glBindTexture(GL_TEXTURE_2D, id);
    size_t o = 20;
    unsigned uploaded = 0;
    for (unsigned i = 0; i < mips && o + 4 <= d.size(); i++) {
        unsigned size = *(unsigned*)(d.data() + o);
        o += 4;
        if (o + size > d.size()) break;
        GLsizei mw = (GLsizei)(w >> i ? w >> i : 1), mh = (GLsizei)(h >> i ? h >> i : 1);
        if (fmt == 0)
            p_glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA, mw, mh, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.data() + o);
        else
            p_glCompressedTexImage2DARB(GL_TEXTURE_2D, i, fmt == 1 ? 0x83F1 /* DXT1 RGBA */ : 0x83F3 /* DXT5 */, mw, mh, 0,
                                        (GLsizei)size, d.data() + o);
        o += size;
        uploaded++;
    }
    p_glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, uploaded ? (GLint)uploaded - 1 : 0);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, uploaded > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return id;
}

}  // namespace

bool WeaponModels_DrawHand(const char* model, const char* texture, int side, float curl, float indexCurl, float forceGlow) {
    if (!model || !*model || !LoadGl()) return false;
    const HModel& h = GetHand(std::string(model) + (side ? "_r" : "_l"));
    if (!h.ok) return false;
    GLuint tex = GetTexture(texture && *texture ? texture : h.texture);
    if (!tex) tex = GetTexture(h.texture);
    // per segment: v' = R (v - P) + P, applied for the segment and then its parent (tips ride on their base)
    struct X { float R[9]; float P[3]; };
    std::vector<X> xf(h.segs.size());
    float dir = side ? 1.0f : -1.0f;  // fingers curl toward the palm: -X for the right hand, +X for the left
    for (size_t i = 0; i < h.segs.size(); i++) {
        const std::string& n = h.segs[i].name;
        bool thumb = n.find("thumb") != std::string::npos, index = n.find("afngr") != std::string::npos;
        bool tip = n.find("fngrt") != std::string::npos || n.find("thumbt") != std::string::npos;
        bool palm = n.find("hand") != std::string::npos;
        float c = index ? indexCurl : curl;
        float deg = palm ? 0 : thumb ? (tip ? 35 : 25) * c : (tip ? 85 : 75) * c;
        float a = dir * deg * 3.14159265f / 180.0f, cs = cosf(a), sn = sinf(a);
        float R[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};  // about the hand's Y axis (across the palm)
        memcpy(xf[i].R, R, sizeof R);
        memcpy(xf[i].P, h.segs[i].pivot, sizeof xf[i].P);
    }
    auto apply = [](const X& x, const float in[3], float out[3]) {
        float d[3] = {in[0] - x.P[0], in[1] - x.P[1], in[2] - x.P[2]};
        for (int r = 0; r < 3; r++) out[r] = x.R[r * 3] * d[0] + x.R[r * 3 + 1] * d[1] + x.R[r * 3 + 2] * d[2] + x.P[r];
    };
    p_glEnable(GL_TEXTURE_2D);
    p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_ALPHA_TEST);
    p_glDepthMask(GL_TRUE);
    for (int pass = 0; pass < (forceGlow > 0.01f ? 2 : 1); pass++) {
    if (pass == 1) {  // Force surge: the same hand again, additive blue
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        p_glDepthMask(GL_FALSE);
        p_glColor4f(0.45f, 0.65f, 1.0f, forceGlow);
    } else {
        p_glColor4f(0.72f, 0.72f, 0.75f, 1);
    }
    p_glBegin(GL_TRIANGLES);
    for (size_t i = 0; i < h.segs.size(); i++) {
        const HSeg& s = h.segs[i];
        for (unsigned short k : s.idx) {
            if ((size_t)k * 3 + 2 >= s.pos.size()) continue;
            float v[3];
            apply(xf[i], &s.pos[(size_t)k * 3], v);
            if (s.parent >= 0 && (size_t)s.parent < xf.size()) apply(xf[s.parent], v, v);
            p_glTexCoord2fv(&s.uv[(size_t)k * 2]);
            p_glVertex3fv(v);
        }
    }
    p_glEnd();
    }
    p_glDisable(GL_BLEND);
    p_glDepthMask(GL_TRUE);
    p_glDisable(GL_TEXTURE_2D);
    return true;
}

bool WeaponModels_BodyFor(int appearance, int letter, int texVar, std::string* model, std::string* texture) {
    auto it = g_bodies.find(appearance);
    if (it == g_bodies.end() || letter < 0 || letter > 9) return false;
    const std::string& m = it->second[letter];
    const std::string& t = it->second[10 + letter];
    if (m == "-") return false;
    *model = m;
    texture->clear();
    if (t != "-") {  // the armor's texture variation (PMBC + 01 -> pmbc01), else the base name
        char cand[2][64];
        sprintf_s(cand[0], "%s%02d", t.c_str(), texVar > 0 ? texVar : 1);
        sprintf_s(cand[1], "%s", t.c_str());
        for (auto& c : cand) {
            std::string path = g_dir + "\\kvr\\textures\\" + c + ".kvrt";
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                *texture = c;
                break;
            }
        }
    }
    return true;
}

void WeaponModels_Init(const char* gameDir) {
    g_dir = gameDir;
    {
        FILE* bf = nullptr;
        std::string bp = g_dir + "\\kvr\\hands\\appearance.txt";
        if (!fopen_s(&bf, bp.c_str(), "r") && bf) {
            char line[1024];
            while (fgets(line, sizeof line, bf)) {
                if (line[0] == '#') continue;
                std::vector<std::string> tok;
                for (char* t = strtok(line, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) tok.push_back(t);
                if (tok.size() == 21) g_bodies[atoi(tok[0].c_str())] = std::vector<std::string>(tok.begin() + 1, tok.end());
            }
            fclose(bf);
            Log("weaponmodels: %zu body appearances for hands", g_bodies.size());
        } else {
            Log("weaponmodels: %s missing - run tools/hand_export.py; no hands", bp.c_str());
        }
    }
    FILE* f = nullptr;
    std::string path = g_dir + "\\kvr\\models\\baseitems.txt";
    if (fopen_s(&f, path.c_str(), "r") || !f) {
        Log("weaponmodels: %s missing - run tools/weapon_export.py; using proxies", path.c_str());
        return;
    }
    int row;
    char prefix[64];
    while (fscanf_s(f, "%d %63s", &row, prefix, (unsigned)sizeof prefix) == 2) g_prefix[row] = prefix;
    fclose(f);
    Log("weaponmodels: %zu base item model prefixes", g_prefix.size());
}

// one mesh; saber blades are scaled along the blade axis (grip frame Z) by `blade`
static void EmitMesh(const WMesh& me, float blade) {
    bool scale = (me.flags & 1) && blade < 0.999f;
    p_glBegin(GL_TRIANGLES);
    for (unsigned short i : me.idx) {
        if ((size_t)i * 3 + 2 >= me.pos.size()) continue;
        p_glTexCoord2fv(&me.uv[(size_t)i * 2]);
        if (scale) {
            float v[3] = {me.pos[(size_t)i * 3], me.pos[(size_t)i * 3 + 1], me.pos[(size_t)i * 3 + 2] * blade};
            p_glVertex3fv(v);
        } else {
            p_glVertex3fv(&me.pos[(size_t)i * 3]);
        }
    }
    p_glEnd();
}

bool WeaponModels_Draw(int row, int variation, float glow, float blade) {
    auto pre = g_prefix.find(row);
    if (pre == g_prefix.end() || !LoadGl()) return false;
    char resref[64];
    sprintf_s(resref, "%s_%03d", pre->second.c_str(), variation > 0 ? variation : 1);
    const WModel* m = &GetModel(resref);
    if (!m->ok && variation != 1) {  // unknown variation: the class's base look
        sprintf_s(resref, "%s_001", pre->second.c_str());
        m = &GetModel(resref);
    }
    if (!m->ok) return false;

    p_glEnable(GL_TEXTURE_2D);
    p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    p_glColor4f(1, 1, 1, 1);
    for (int pass = 0; pass < 2; pass++) {  // opaque parts first, then additive blades
        for (const WMesh& me : m->meshes) {
            bool isBlade = (me.flags & 1) != 0;
            if (isBlade != (pass == 1)) continue;
            if (isBlade && blade <= 0.01f) continue;  // unlit saber: hilt only
            p_glBindTexture(GL_TEXTURE_2D, GetTexture(me.texture));
            if (isBlade) {
                p_glDisable(GL_ALPHA_TEST);
                p_glEnable(GL_BLEND);
                p_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                p_glDepthMask(GL_FALSE);
            } else {
                // KotOR textures use alpha as an env-map / specular mask, not transparency: draw solid
                p_glDisable(GL_BLEND);
                p_glDisable(GL_ALPHA_TEST);
                p_glDepthMask(GL_TRUE);
            }
            // unlit: dim the opaque parts a little so they sit in KotOR's dark scenes; blades stay full bright
            if (isBlade) p_glColor4f(1, 1, 1, 1);
            else p_glColor4f(0.72f, 0.72f, 0.75f, 1);
            EmitMesh(me, blade);
        }
    }
    if (glow > 0.01f) {  // cue glow: the same meshes again, additive, brighter on the blade than on the hilt / body
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        p_glDepthMask(GL_FALSE);
        for (const WMesh& me : m->meshes) {
            bool isBlade = (me.flags & 1) != 0;
            if (isBlade && blade <= 0.01f) continue;
            p_glBindTexture(GL_TEXTURE_2D, GetTexture(me.texture));
            p_glColor4f(1, 1, 1, glow * (isBlade ? 1.0f : 0.8f));
            EmitMesh(me, blade);
        }
        p_glDepthMask(GL_TRUE);
    }
    p_glDisable(GL_TEXTURE_2D);
    p_glDisable(GL_ALPHA_TEST);
    return true;
}
