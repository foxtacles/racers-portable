#pragma once

// [library:window] Support API for the SDL3 application shell: the thread-safe event
// queue bridging the main thread (SDL callbacks) and the game thread (blocking
// LegoRacers::Run()), main-thread dispatch, and portable configuration hooks.

#include "windows.h"

#include <SDL3/SDL.h>

// Event queue: the main thread pushes SDL events; the game thread drains them from
// Win32GolApp::Tick().
void MiniwinApp_PushEvent(const SDL_Event& p_event);
bool MiniwinApp_PollEvent(SDL_Event& p_event);

// Feeds a drained SDL event into the DirectInput emulation's buffered device data
// (keyboard/mouse rings). Call once per polled event, on the game thread.
void MiniwinInput_HandleEvent(const SDL_Event& p_event);

// Runs a callable synchronously on the main thread (window management must happen
// there on some platforms). Safe to call from the main thread itself.
template <typename F>
inline void MiniwinApp_RunOnMainThread(F&& p_fn)
{
	SDL_RunOnMainThread([](void* p_userdata) { (*static_cast<F*>(p_userdata))(); }, &p_fn, true);
}

// Seeds the emulated registry's LangID value (--language command-line argument).
void MiniwinSetRegistryLangId(DWORD p_langId);

// Fullscreen/window scaling of the logical resolution: preserve aspect ratio with
// black bars (default) or stretch to fill the drawable.
enum MiniwinScaleMode {
	MINIWIN_SCALE_LETTERBOX = 0,
	MINIWIN_SCALE_STRETCH = 1,
};
void MiniwinSetScaleMode(MiniwinScaleMode p_mode);
MiniwinScaleMode MiniwinGetScaleMode();

// Internal rasterization resolution: the native drawable size (default) or the
// game's original logical resolution (640x480) upscaled at present time.
enum MiniwinRenderResolution {
	MINIWIN_RESOLUTION_NATIVE = 0,
	MINIWIN_RESOLUTION_ORIGINAL = 1,
};
void MiniwinSetRenderResolution(MiniwinRenderResolution p_resolution);

// Mutes/unmutes the DirectSound emulation (used while the app is deactivated, like
// the original's non-GLOBALFOCUS DirectSound buffers behaved).
void MiniwinSound_SetSuspended(bool p_suspended);
MiniwinRenderResolution MiniwinGetRenderResolution();

// Configures SDL window attributes required by the render backend; returns extra
// SDL_WindowFlags to OR into SDL_CreateWindow. Main thread, before window creation.
Uint32 MiniwinBackend_PrepareWindowFlags();
