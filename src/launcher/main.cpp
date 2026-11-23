#define GL_SILENCE_DEPRECATION
#include <SDL.h>

#include "input.h"
#include "state.h"
#include "ui.h"
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GLES2/gl2.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
using namespace launcher;

void configure_context()
{
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
}

// Snapshot mode makes it possible to inspect each screen without input devices.
void configure_snapshot(State &state)
{
    state.start = SDL_GetTicks() - 2000;
    const char *screen = getenv("V2N_LAUNCHER_SCREEN");
    if (screen && strcmp(screen, "import") == 0)
        state.screen = Screen::Import;
    if (screen && strcmp(screen, "all") == 0)
        state.screen = Screen::AllGames;
    if (screen && strcmp(screen, "home-focus") == 0)
        state.home_focus = state.previous_home_focus = HomeFocus::AllGames;
    if (screen && strcmp(screen, "picker") == 0) {
        state.screen = Screen::Import;
        state.browse(PickerKind::Package);
    }
    state.screen_start = SDL_GetTicks() - 1000;
    if (screen && strcmp(screen, "motion-right") == 0) {
        state.move_game(1);
        state.motion_start = SDL_GetTicks() - 100;
    }
    if (screen && strcmp(screen, "motion-left") == 0) {
        state.move_game(-1);
        state.motion_start = SDL_GetTicks() - 100;
    }
}

void save_snapshot(const char *path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Could not save screenshot\n");
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
            fwrite(&pixels[(static_cast<size_t>(y) * width + x) * 4], 1, 3, file);
    fclose(file);
}

void run(SDL_Window *window)
{
    State state;
    const char *capture = getenv("V2N_LAUNCHER_SCREENSHOT");
    if (capture)
        configure_snapshot(state);
    Input input;
    bool dirty = true;
    bool was_splash = true;
    bool was_animating = false;
    while (state.running) {
        const auto before = state.visible_state();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                 event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                 event.window.event == SDL_WINDOWEVENT_SHOWN))
                dirty = true;
            if (event.type == SDL_QUIT && !state.import_running())
                state.running = false;
            input.handle(event, state);
        }
        state.poll();
        state.update_motion();
        if (before != state.visible_state())
            dirty = true;
        const bool splash = SDL_GetTicks() - state.start < splash_duration;
        const bool animating = state.animating();
        if (splash || was_splash != splash || animating || was_animating)
            dirty = true;
        was_splash = splash;
        was_animating = animating;
        if (!dirty) {
            SDL_Delay(16);
            continue;
        }
        dirty = false;
        int width, height;
        SDL_GL_GetDrawableSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        ui::begin_frame(width);
        ui::draw(state);
        ui::end_frame();
        if (capture) {
            save_snapshot(capture, width, height);
            state.running = false;
        }
        SDL_GL_SwapWindow(window);
        SDL_Delay(16);
    }
}
} // namespace

int main(int, char **)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) <
        0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    configure_context();
    SDL_Window *window = SDL_CreateWindow(
        "V2N Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, screen_width, screen_height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        fprintf(stderr, "SDL GL: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    int result = 0;
    if (!launcher::ui::initialize()) {
        fprintf(stderr, "Failed to initialize NanoVG or font\n");
        result = 1;
    } else {
        run(window);
        launcher::ui::shutdown();
    }
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
