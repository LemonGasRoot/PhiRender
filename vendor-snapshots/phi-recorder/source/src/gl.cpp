// gl.cpp - hidden-window OpenGL 3.3 core context + function loader (Windows)
#include "gl.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- function pointers -----------------------------------------------------
#define GL_IMPL(ret, name, params) PFN_##name name = nullptr;
GL_FUNC_LIST(GL_IMPL)
#undef GL_IMPL

namespace {
#ifdef _WIN32
    HWND g_hwnd = nullptr;
    HDC g_hdc = nullptr;
    HGLRC g_hglrc = nullptr;
    ATOM g_windowClass = 0;

    typedef HGLRC(WINAPI* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    void* getProc(const char* name) {
        void* p = (void*)wglGetProcAddress(name);
        if (!p) {
            HMODULE mod = GetModuleHandleA("opengl32.dll");
            if (mod) p = (void*)GetProcAddress(mod, name);
        }
        return p;
    }
#endif

    gl::ContextInfo g_info;
} // namespace

#define GL_LOAD(ret, fn, params)                                                         \
    {                                                                                    \
        fn = (PFN_##fn)getProc(#fn);                                                     \
        if (!fn) missing->push_back(std::string(#fn));                                   \
    }

namespace gl {

    bool loadFunctions(std::vector<std::string>* missing) {
        missing->clear();
        GL_FUNC_LIST(GL_LOAD)
        return missing->empty();
    }

    bool checkError(const char* where) {
        GLenum e = glGetError();
        if (e != 0) {
            std::fprintf(stderr, "[gl] error 0x%04X at %s\n", e, where);
            return false;
        }
        return true;
    }

    const ContextInfo& info() { return g_info; }

#ifdef _WIN32
    bool createContext(int width, int height, ContextInfo* info, std::string* error) {
        auto fail = [&](const char* msg) {
            if (error) *error = msg;
            return false;
        };

        HINSTANCE hinst = GetModuleHandleA(nullptr);
        const char* cls = "PhiRenderOffscreen";

        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        g_windowClass = RegisterClassExA(&wc);
        if (!g_windowClass) return fail("RegisterClassEx failed");

        g_hwnd = CreateWindowExA(0, cls, "phirender", WS_POPUP, 0, 0, width > 0 ? width : 64, height > 0 ? height : 64, nullptr, nullptr, hinst, nullptr);
        if (!g_hwnd) return fail("CreateWindowEx failed");

        g_hdc = GetDC(g_hwnd);
        if (!g_hdc) return fail("GetDC failed");

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.iLayerType = PFD_MAIN_PLANE;

        int pf = ChoosePixelFormat(g_hdc, &pfd);
        if (!pf) return fail("ChoosePixelFormat failed");
        if (!SetPixelFormat(g_hdc, pf, &pfd)) return fail("SetPixelFormat failed");

        // Bootstrap a legacy context to query wgl extensions.
        HGLRC legacy = wglCreateContext(g_hdc);
        if (!legacy) return fail("wglCreateContext failed");
        if (!wglMakeCurrent(g_hdc, legacy)) return fail("wglMakeCurrent(legacy) failed");

        PFN_wglCreateContextAttribsARB createAttribs =
            (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");

        HGLRC core = nullptr;
        if (createAttribs) {
            const int attribs[] = {
                0x2091, 3, // WGL_CONTEXT_MAJOR_VERSION_ARB
                0x2092, 3, // WGL_CONTEXT_MINOR_VERSION_ARB
                0x9126, 0x0001, // WGL_CONTEXT_PROFILE_MASK_ARB, CORE_PROFILE_BIT
                0, 0,
            };
            core = createAttribs(g_hdc, nullptr, attribs);
        }
        if (!core) {
            // Fallback: use the legacy context (may be a compatibility profile).
            core = legacy;
            legacy = nullptr;
        } else {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(legacy);
        }
        if (!wglMakeCurrent(g_hdc, core)) return fail("wglMakeCurrent(core) failed");
        g_hglrc = core;

        std::vector<std::string> missing;
        loadFunctions(&missing);
        if (!missing.empty()) {
            std::string msg = "missing GL functions:";
            for (auto& m : missing) msg += " " + m;
            if (error) *error = msg;
            return false;
        }

        g_info.width = width;
        g_info.height = height;
        g_info.vendor = (const char*)glGetString(GL_VENDOR);
        g_info.renderer = (const char*)glGetString(GL_RENDERER);
        g_info.version = (const char*)glGetString(GL_VERSION);
        g_info.glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        if (info) *info = g_info;
        return true;
    }

    void destroyContext() {
        if (g_hglrc) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(g_hglrc);
            g_hglrc = nullptr;
        }
        if (g_hdc && g_hwnd) {
            ReleaseDC(g_hwnd, g_hdc);
            g_hdc = nullptr;
        }
        if (g_hwnd) {
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
        }
        if (g_windowClass) {
            UnregisterClassA("PhiRenderOffscreen", GetModuleHandleA(nullptr));
            g_windowClass = 0;
        }
    }
#else
    bool createContext(int, int, ContextInfo*, std::string* error) {
        if (error) *error = "Only Windows is supported";
        return false;
    }
    void destroyContext() {}
#endif

} // namespace gl
