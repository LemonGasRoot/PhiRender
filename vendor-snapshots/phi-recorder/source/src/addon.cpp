// addon.cpp - optional post-process add-ons (see addon.h).
#include "addon.h"

#include "cJSON.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace addon {

    static bool fileExists(const std::string& p) {
        struct stat st;
        return stat(p.c_str(), &st) == 0;
    }

    static std::string readFile(const std::string& path, bool* ok) {
        std::ifstream f(path.c_str(), std::ios::binary);
        if (!f) {
            if (ok) *ok = false;
            return std::string();
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        if (ok) *ok = true;
        return ss.str();
    }

    std::string resolveDir(const std::string& idOrPath, const std::string& assetDir) {
        // Explicit paths (containing a separator) win, then the addons/ folder
        // next to the asset pack, then ./addons.
        if (idOrPath.find('/') != std::string::npos || idOrPath.find('\\') != std::string::npos) {
            return idOrPath;
        }
        const std::string candidates[] = {assetDir + "/../addons/" + idOrPath, "addons/" + idOrPath,
                                          assetDir + "/addons/" + idOrPath};
        for (const std::string& c : candidates) {
            if (fileExists(c + "/addon.json")) return c;
        }
        return "addons/" + idOrPath;
    }

    std::vector<std::string> list(const std::string& root) {
        std::vector<std::string> out;
        DIR* d = opendir(root.c_str());
        if (!d) return out;
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            if (fileExists(root + "/" + e->d_name + "/addon.json")) out.push_back(e->d_name);
        }
        closedir(d);
        std::sort(out.begin(), out.end());
        return out;
    }

    bool readChartConfig(
        const std::string& path,
        std::vector<std::pair<std::string, std::vector<std::pair<std::string, float>>>>* out,
        std::string* error) {
        out->clear();
        bool ok = false;
        const std::string text = readFile(path, &ok);
        if (!ok) return true;  // no config is fine
        cJSON* root = cJSON_Parse(text.c_str());
        if (!root) {
            if (error) {
                const char* at = cJSON_GetErrorPtr();
                *error = "invalid json in " + path;
                if (at) *error += std::string(" near: ") + at;
            }
            return false;
        }
        for (cJSON* e = root->child; e; e = e->next) {
            if (!cJSON_IsObject(e)) continue;
            std::vector<std::pair<std::string, float>> params;
            // Accept both {"id": {"p": 1}} and {"id": {"params": {"p": 1}}}.
            cJSON* inner = cJSON_GetObjectItemCaseSensitive(e, "params");
            cJSON* src = (inner && cJSON_IsObject(inner)) ? inner : e;
            for (cJSON* p = src->child; p; p = p->next) {
                if (cJSON_IsNumber(p)) params.emplace_back(p->string, (float)p->valuedouble);
            }
            out->emplace_back(e->string, std::move(params));
        }
        cJSON_Delete(root);
        return true;
    }

    bool Addon::load(const std::string& dir, std::string* error) {
        const std::string manifestPath = dir + "/addon.json";
        bool ok = false;
        const std::string text = readFile(manifestPath, &ok);
        if (!ok) {
            if (error) *error = "cannot read " + manifestPath;
            return false;
        }
        cJSON* root = cJSON_Parse(text.c_str());
        if (!root) {
            if (error) {
                const char* at = cJSON_GetErrorPtr();
                *error = "invalid json in " + manifestPath;
                if (at) *error += std::string(" near: ") + at;
            }
            return false;
        }
        auto getStr = [&](const char* key) -> std::string {
            cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
            return (it && cJSON_IsString(it) && it->valuestring) ? std::string(it->valuestring)
                                                                 : std::string();
        };
        id = dir;
        const size_t slash = id.find_last_of("/\\");
        if (slash != std::string::npos) id = id.substr(slash + 1);
        name = getStr("name");
        if (name.empty()) name = id;
        author = getStr("author");
        version = getStr("version");

        std::string shaderFile = getStr("shader");
        if (shaderFile.empty()) shaderFile = "shader.frag";
        if (shaderFile[0] != '/') shaderFile = dir + "/" + shaderFile;

        params.clear();
        cJSON* paramsJson = cJSON_GetObjectItemCaseSensitive(root, "params");
        if (paramsJson && cJSON_IsObject(paramsJson)) {
            for (cJSON* p = paramsJson->child; p; p = p->next) {
                if (cJSON_IsNumber(p)) params.emplace_back(p->string, (float)p->valuedouble);
            }
        }
        textures.clear();
        cJSON* texJson = cJSON_GetObjectItemCaseSensitive(root, "textures");
        if (texJson && cJSON_IsObject(texJson)) {
            for (cJSON* t = texJson->child; t; t = t->next) {
                if (!cJSON_IsString(t) || !t->valuestring) continue;
                TextureParam tp;
                tp.name = t->string;
                std::string file = t->valuestring;
                if (file[0] != '/' && file.find(':') == std::string::npos) file = dir + "/" + file;
                if (!tp.tex.loadFile(file, true, false)) {
                    if (error) *error = "cannot load texture " + file;
                    return false;
                }
                textures.push_back(std::move(tp));
            }
        }
        // Noise areas the add-on contributes to the scene.  They go through the
        // normal NoiseField pipeline, so unlike a shader overlay they occlude the
        // notes and get the same glow / edge treatment as the chart's own areas.
        blocks.clear();
        cJSON* blocksJson = cJSON_GetObjectItemCaseSensitive(root, "blocks");
        if (blocksJson && cJSON_IsArray(blocksJson)) {
            auto num = [](cJSON* o, const char* k, float dflt) {
                cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
                return (v && cJSON_IsNumber(v)) ? (float)v->valuedouble : dflt;
            };
            for (cJSON* b = blocksJson->child; b; b = b->next) {
                if (!cJSON_IsObject(b)) continue;
                BlockSpec s;
                s.appear = num(b, "appear", 0.0f);
                s.disappear = num(b, "disappear", 1e9f);
                s.enable = num(b, "enable", s.appear);
                s.disable = num(b, "disable", s.disappear);
                s.x = num(b, "x", 0.0f);
                s.y = num(b, "y", 0.0f);
                s.w = num(b, "w", 1.0f);
                s.h = num(b, "h", 0.4f);
                s.rot = num(b, "rot", 0.0f);
                cJSON* sub = cJSON_GetObjectItemCaseSensitive(b, "subtract");
                s.subtract = (sub && cJSON_IsBool(sub)) ? cJSON_IsTrue(sub) : false;
                // Reject nonsense early: a NaN or a zero-sized block would poison
                // the mask rasteriser and is much harder to debug downstream.
                const bool finite = std::isfinite(s.appear) && std::isfinite(s.disappear) &&
                                    std::isfinite(s.enable) && std::isfinite(s.disable) &&
                                    std::isfinite(s.x) && std::isfinite(s.y) &&
                                    std::isfinite(s.w) && std::isfinite(s.h) &&
                                    std::isfinite(s.rot);
                if (!finite || s.w <= 0.0f || s.h <= 0.0f || s.appear > s.disappear) {
                    std::fprintf(stderr, "addon %s: invalid block skipped\n", id.c_str());
                    continue;
                }
                blocks.push_back(s);
            }
        }
        notes.clear();
        lines.clear();
        hideNotes.clear();
        hideLines.clear();
        cJSON* chartJson = cJSON_GetObjectItemCaseSensitive(root, "chart");
        if (chartJson && cJSON_IsObject(chartJson)) {
            auto ranges = [](cJSON* o, const char* k, std::vector<std::pair<float, float>>* out) {
                cJSON* arr = cJSON_GetObjectItemCaseSensitive(o, k);
                if (!arr || !cJSON_IsArray(arr)) return;
                for (cJSON* r = arr->child; r; r = r->next) {
                    if (!cJSON_IsArray(r)) continue;
                    cJSON* a = r->child;
                    cJSON* b = a ? a->next : nullptr;
                    if (!a || !b || !cJSON_IsNumber(a) || !cJSON_IsNumber(b)) continue;
                    out->emplace_back((float)a->valuedouble, (float)b->valuedouble);
                }
            };
            ranges(chartJson, "hideNotes", &hideNotes);
            ranges(chartJson, "hideLines", &hideLines);
            auto num = [](cJSON* o, const char* k, float dflt) {
                cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
                return (v && cJSON_IsNumber(v)) ? (float)v->valuedouble : dflt;
            };
            cJSON* noteArr = cJSON_GetObjectItemCaseSensitive(chartJson, "notes");
            if (noteArr && cJSON_IsArray(noteArr)) {
                for (cJSON* n = noteArr->child; n; n = n->next) {
                    if (!cJSON_IsObject(n)) continue;
                    NoteSpec s;
                    s.time = num(n, "time", 0.0f);
                    s.x = num(n, "x", 0.0f);
                    s.type = (int)num(n, "type", 1.0f);
                    s.line = (int)num(n, "line", 0.0f);
                    s.size = num(n, "size", 1.0f);
                    notes.push_back(s);
                }
            }
            cJSON* lineArr = cJSON_GetObjectItemCaseSensitive(chartJson, "lines");
            if (lineArr && cJSON_IsArray(lineArr)) {
                for (cJSON* l = lineArr->child; l; l = l->next) {
                    if (!cJSON_IsObject(l)) continue;
                    LineSpec s;
                    s.appear = num(l, "appear", 0.0f);
                    s.disappear = num(l, "disappear", 1e9f);
                    s.x = num(l, "x", 0.0f);
                    s.y = num(l, "y", 0.0f);
                    s.rot = num(l, "rot", 0.0f);
                    lines.push_back(s);
                }
            }
        }
        cJSON_Delete(root);

        bool shaderOk = false;
        const std::string body = readFile(shaderFile, &shaderOk);
        if (!shaderOk) {
            if (error) *error = "cannot read " + shaderFile;
            return false;
        }

        // Shaders are authored as a bare main(): this header supplies the varying,
        // the output, the scene sampler and one uniform per manifest param.
        std::string src = "#version 330 core\n";
        src += "in vec2 vUv;\n";
        src += "out vec4 fragColor;\n";
        src += "uniform sampler2D uScene;\n";
        src += "uniform float uTime;\n";
        src += "uniform vec2 uResolution;\n";
        for (const auto& kv : params) src += "uniform float p_" + kv.first + ";\n";
        for (const auto& tp : textures) src += "uniform sampler2D t_" + tp.name + ";\n";
        src += "uniform float uFrame;\n";
        src += "uniform float uAspect;\n";
        src += "uniform float uProgress;\n";   // 0..1 through the chart
        src += "uniform float uDuration;\n";   // chart length in seconds
        src += "uniform float uCombo;\n";
        src += "uniform float uNotes;\n";
        src += "uniform float uBeat;\n";
        src += "uniform float uBpm;\n";
        src += "uniform float uSpeed;\n";
        src += "uniform float uHit;\n";
        src += "uniform vec2  uLastNote;\n";
        src += "uniform float uFps;\n";
        src += "uniform float uLines;\n";
        src += "uniform float uZones;\n";      // notes passed so far
        src += "#line 1\n";
        src += body;

        const char* vs = gpu::fullscreenVertexShader();
        if (!pass.build(vs, src.c_str(), "addon:" + id, error)) return false;

        active = true;
        return true;
    }

    void Addon::setParam(const std::string& key, float value) {
        for (auto& kv : params) {
            if (kv.first == key) {
                kv.second = value;
                return;
            }
        }
        params.emplace_back(key, value);
    }

    void Addon::apply(gpu::RenderTarget& scene, const Context& ctx) {
        if (!active || !pass.shader.program) return;
        if (scratch.width != scene.width || scratch.height != scene.height) {
            scratch.create(scene.width, scene.height, GL_RGBA8, true);
        }
        // 1) copy the finished scene aside with a framebuffer blit (no shader).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, scene.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scratch.fbo);
        glBlitFramebuffer(0, 0, scene.width, scene.height, 0, 0, scratch.width, scratch.height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, scene.fbo);

        // 2) draw the add-on over the scene.
        glViewport(0, 0, scene.width, scene.height);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        pass.shader.use();
        pass.shader.setTex("uScene", 0, scratch.color.id);
        pass.shader.set("uTime", ctx.time);
        pass.shader.set("uFrame", (float)ctx.frame);
        pass.shader.set("uProgress", ctx.progress);
        pass.shader.set("uDuration", ctx.duration);
        pass.shader.set("uCombo", ctx.combo);
        pass.shader.set("uNotes", ctx.notes);
        pass.shader.set("uBeat", ctx.beat);
        pass.shader.set("uBpm", ctx.bpm);
        pass.shader.set("uSpeed", ctx.speed);
        pass.shader.set("uHit", ctx.hit);
        pass.shader.set("uLastNote", Vec2(ctx.lastX, ctx.lastY));
        pass.shader.set("uFps", ctx.fps);
        pass.shader.set("uLines", ctx.lines);
        pass.shader.set("uZones", ctx.zones);
        pass.shader.set("uAspect", (float)scene.width / (float)scene.height);
        for (size_t i = 0; i < textures.size(); ++i) {
            pass.shader.setTex(("t_" + textures[i].name).c_str(), (int)i + 1, textures[i].tex.id);
        }
        pass.shader.set("uResolution", (float)scene.width, (float)scene.height);
        for (const auto& kv : params) {
            pass.shader.set(("p_" + kv.first).c_str(), kv.second);
        }
        pass.draw();
    }

    void Addon::destroy() {
        pass.shader.destroy();
        scratch.destroy();
        for (auto& tp : textures) tp.tex.destroy();
        textures.clear();
        active = false;
    }

}  // namespace addon
