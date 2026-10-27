// The controlled character's own models, recognised by draw size (see ownsig.h).
#include "ownsig.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

extern void Log(const char* fmt, ...);

namespace {

std::map<std::string, std::vector<int>> g_models;  // model -> index counts of its rendered meshes
std::map<int, std::string> g_heads;                // appearance row -> head model
std::vector<int> g_set;                            // the current character's counts (sorted)
std::string g_key;                                 // what g_set was built from

}  // namespace

void OwnSig_Init(const char* gameDir) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\kvr\\models\\signatures.txt", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) {
        Log("ownsig: %s missing (tools/sig_export.py): own weapons hidden by distance only", path);
        return;
    }
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char kind = line[0];
        char* p = line + 2;
        if (kind == 'h') {
            int row;
            char head[64];
            if (sscanf_s(p, "%d %63s", &row, head, (unsigned)sizeof head) == 2) g_heads[row] = head;
        } else if (kind == 'm') {
            char name[64];
            int used = 0;
            if (sscanf_s(p, "%63s%n", name, (unsigned)sizeof name, &used) != 1) continue;
            std::vector<int>& v = g_models[name];
            p += used;
            int c;
            while (sscanf_s(p, "%d%n", &c, &used) == 1) {
                v.push_back(c);
                p += used;
            }
        }
    }
    fclose(f);
    Log("ownsig: %zu model signatures, %zu heads", g_models.size(), g_heads.size());
}

void OwnSig_SetCharacter(int appearance, const char* bodyModel, const char* weapons[], int nWeapons) {
    std::string key = std::to_string(appearance) + "|" + (bodyModel ? bodyModel : "");
    for (int i = 0; i < nWeapons; i++) key += std::string("|") + (weapons[i] ? weapons[i] : "");
    if (key == g_key) return;
    g_key = key;
    g_set.clear();
    auto add = [&](const std::string& m) {
        auto it = g_models.find(m);
        if (it != g_models.end()) g_set.insert(g_set.end(), it->second.begin(), it->second.end());
    };
    if (bodyModel && bodyModel[0]) add(bodyModel);
    auto h = g_heads.find(appearance);
    if (h != g_heads.end()) add(h->second);
    for (int i = 0; i < nWeapons; i++)
        if (weapons[i] && weapons[i][0]) add(weapons[i]);
    std::sort(g_set.begin(), g_set.end());
    g_set.erase(std::unique(g_set.begin(), g_set.end()), g_set.end());
    Log("ownsig: character %s -> %zu mesh sizes", key.c_str(), g_set.size());
}

bool OwnSig_Match(int indexCount) { return std::binary_search(g_set.begin(), g_set.end(), indexCount); }

bool OwnSig_Ready() { return !g_set.empty(); }
