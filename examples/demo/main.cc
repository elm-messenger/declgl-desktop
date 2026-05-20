// declgl_demo — M1 smoke test.
//
// Opens an SDL3 window, creates a GL 3.3 Core context, loads function
// pointers via GLAD2, then enters a frame loop that does a colored clear
// until the window is closed. Validates the full vcpkg + CMake + GLAD
// stack before any real engine code is written.

#include <glad/gl.h>
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>

namespace {

constexpr int kInitialWidth  = 1280;
constexpr int kInitialHeight = 720;

void log_sdl_error(const char* what) {
    std::fprintf(stderr, "[declgl_demo] %s failed: %s\n", what, SDL_GetError());
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_sdl_error("SDL_Init");
        return 1;
    }

    // Request OpenGL 3.3 Core (forward-compatible on macOS).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        "declgl_demo (M1)",
        kInitialWidth, kInitialHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        log_sdl_error("SDL_CreateWindow");
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        log_sdl_error("SDL_GL_CreateContext");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!SDL_GL_MakeCurrent(window, gl_ctx)) {
        log_sdl_error("SDL_GL_MakeCurrent");
        SDL_GL_DestroyContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Try to enable adaptive vsync, fall back to plain vsync.
    if (!SDL_GL_SetSwapInterval(-1)) {
        SDL_GL_SetSwapInterval(1);
    }

    // GLAD2 unified loader: resolves all GL 3.3 core entry points via
    // SDL_GL_GetProcAddress.
    int gl_version = gladLoadGL(
        reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress));
    if (gl_version == 0) {
        std::fprintf(stderr, "[declgl_demo] gladLoadGL failed\n");
        SDL_GL_DestroyContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::printf("[declgl_demo] GL %d.%d  vendor=%s  renderer=%s  glsl=%s\n",
                GLAD_VERSION_MAJOR(gl_version),
                GLAD_VERSION_MINOR(gl_version),
                reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));

    bool running = true;
    Uint64 start_ticks = SDL_GetTicks();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_ESCAPE) running = false;
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    glViewport(0, 0, ev.window.data1, ev.window.data2);
                    break;
                default: break;
            }
        }

        // Resize viewport to current drawable size each frame (cheap).
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        glViewport(0, 0, w, h);

        const float t  = static_cast<float>(SDL_GetTicks() - start_ticks) * 0.001f;
        const float r  = 0.5f + 0.5f * std::sin(t * 0.7f);
        const float g  = 0.5f + 0.5f * std::sin(t * 0.9f + 2.0f);
        const float b  = 0.5f + 0.5f * std::sin(t * 1.1f + 4.0f);

        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
