#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/display.h>

#include "log.h"
#include "module.h"
#include "modules/SceDisplay.h"
#include "util.h"

#define VITA_FB_WIDTH  960
#define VITA_FB_HEIGHT 544

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static uint32_t g_texture_width;
static uint32_t g_texture_height;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_frame_cond;
static pthread_cond_t g_vblank_cond;

static SceDisplayFrameBuf g_vita_conf_fb;
static pthread_t g_presenter_thread;
static bool g_presenter_run;
static bool g_presenter_ready;
static int g_presenter_result;
static bool g_initialized;

static void presenter_cleanup(void)
{
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }

    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }

    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void *presenter_thread(void *arg)
{
    (void)arg;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        LOG("SDL video init failed: %s", SDL_GetError());
        pthread_mutex_lock(&g_lock);
        g_presenter_result = -1;
        g_presenter_ready = true;
        pthread_cond_broadcast(&g_frame_cond);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    g_window = SDL_CreateWindow("vita2native", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                VITA_FB_WIDTH, VITA_FB_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        LOG("SDL_CreateWindow failed: %s", SDL_GetError());
        presenter_cleanup();
        pthread_mutex_lock(&g_lock);
        g_presenter_result = -1;
        g_presenter_ready = true;
        pthread_cond_broadcast(&g_frame_cond);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer)
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        LOG("SDL_CreateRenderer failed: %s", SDL_GetError());
        presenter_cleanup();
        pthread_mutex_lock(&g_lock);
        g_presenter_result = -1;
        g_presenter_ready = true;
        pthread_cond_broadcast(&g_frame_cond);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    SDL_RenderSetLogicalSize(g_renderer, VITA_FB_WIDTH, VITA_FB_HEIGHT);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);

    pthread_mutex_lock(&g_lock);
    g_presenter_result = 0;
    g_presenter_ready = true;
    pthread_cond_broadcast(&g_frame_cond);
    pthread_mutex_unlock(&g_lock);

    while (true) {
        SDL_Event event;
        SceDisplayFrameBuf fb;

        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                pthread_mutex_lock(&g_lock);
                g_presenter_run = false;
                pthread_mutex_unlock(&g_lock);
            }
        }

        pthread_mutex_lock(&g_lock);
        while (g_presenter_run && !g_vita_conf_fb.base) {
            struct timespec deadline;

            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 16 * 1000 * 1000;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }

            pthread_cond_timedwait(&g_frame_cond, &g_lock, &deadline);
        }

        if (!g_presenter_run) {
            pthread_mutex_unlock(&g_lock);
            break;
        }

        fb = g_vita_conf_fb;
        pthread_mutex_unlock(&g_lock);

        if (!fb.base || fb.width == 0 || fb.height == 0) {
            SDL_Delay(16);
            continue;
        }

        if (!g_texture || g_texture_width != fb.width || g_texture_height != fb.height) {
            if (g_texture)
                SDL_DestroyTexture(g_texture);
            g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888,
                                          SDL_TEXTUREACCESS_STREAMING, (int)fb.width, (int)fb.height);
            g_texture_width = fb.width;
            g_texture_height = fb.height;
            if (!g_texture) {
                LOG("SDL_CreateTexture failed: %s", SDL_GetError());
                SDL_Delay(16);
                continue;
            }
        }

        if (SDL_UpdateTexture(g_texture, NULL, fb.base, (int)(fb.pitch * 4)) != 0) {
            LOG("SDL_UpdateTexture failed: %s", SDL_GetError());
            SDL_Delay(16);
            continue;
        }

        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);

        pthread_mutex_lock(&g_lock);
        pthread_cond_broadcast(&g_vblank_cond);
        pthread_mutex_unlock(&g_lock);
    }

    presenter_cleanup();

    return NULL;
}

EXPORT(SceDisplayUser, 0x7A410B64, int, sceDisplaySetFrameBuf, const SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    (void)sync;

    if (!pParam)
        return 0;

    pthread_mutex_lock(&g_lock);
    g_vita_conf_fb = *pParam;
    pthread_cond_broadcast(&g_frame_cond);
    pthread_mutex_unlock(&g_lock);

    return 0;
}

EXPORT(SceDisplayUser, 0x42AE6BBC, int, sceDisplayGetFrameBuf, SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    (void)sync;

    if (!pParam)
        return 0;

    pthread_mutex_lock(&g_lock);
    *pParam = g_vita_conf_fb;
    pthread_mutex_unlock(&g_lock);

    return 0;
}

EXPORT(SceDisplay, 0x5795E898, int, sceDisplayWaitVblankStart, void)
{
    pthread_mutex_lock(&g_lock);
    pthread_cond_wait(&g_vblank_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);

    return 0;
}

DECLARE_LIBRARY(SceDisplay, 0x5ed8f994);
DECLARE_LIBRARY(SceDisplayUser, 0x4faacd11);

int SceDisplay_init(HostGpuDevice device)
{
    pthread_condattr_t attr;
    int ret;

    (void)device;

    if (g_initialized)
        return 0;

    pthread_condattr_init(&attr);
    pthread_cond_init(&g_frame_cond, &attr);
    pthread_cond_init(&g_vblank_cond, &attr);
    pthread_condattr_destroy(&attr);

    pthread_mutex_lock(&g_lock);
    g_presenter_run = true;
    g_presenter_ready = false;
    g_presenter_result = -1;
    pthread_mutex_unlock(&g_lock);

    ret = pthread_create(&g_presenter_thread, NULL, presenter_thread, NULL);
    if (ret != 0) {
        LOG("Could not create SDL presenter thread: %d", ret);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    while (!g_presenter_ready)
        pthread_cond_wait(&g_frame_cond, &g_lock);
    ret = g_presenter_result;
    pthread_mutex_unlock(&g_lock);

    if (ret != 0)
        return ret;

    g_initialized = true;

    return 0;
}

int SceDisplay_finish(void)
{
    if (!g_initialized)
        return 0;

    pthread_mutex_lock(&g_lock);
    g_presenter_run = false;
    pthread_cond_broadcast(&g_frame_cond);
    pthread_cond_broadcast(&g_vblank_cond);
    pthread_mutex_unlock(&g_lock);

    pthread_join(g_presenter_thread, NULL);

    pthread_cond_destroy(&g_frame_cond);
    pthread_cond_destroy(&g_vblank_cond);

    g_initialized = false;

    return 0;
}
