#pragma once

// [library:3d] Render backend abstraction for the Direct3D immediate-mode emulation.
// The game submits pre-transformed D3DTLVERTEX triangles; a backend is a rasterizer
// state machine (SDL3 GPU / OpenGL 3.3 / OpenGL ES 3).

#include <SDL3/SDL.h>
#include <miniwin/d3d.h>

// Snapshot of the D3D render states a draw call depends on.
struct MiniwinRasterState {
	bool zEnable = true;
	bool zWrite = true;
	D3DCMPFUNC zFunc = D3DCMP_LESSEQUAL;
	bool alphaBlend = false;
	D3DBLEND srcBlend = D3DBLEND_SRCALPHA;
	D3DBLEND destBlend = D3DBLEND_INVSRCALPHA;
	bool alphaTest = false;
	D3DCMPFUNC alphaFunc = D3DCMP_ALWAYS;
	float alphaRef = 0.f;
	D3DCULL cullMode = D3DCULL_NONE;
	bool specular = false;
	bool textured = false;
	Uint32 textureId = 0;
	bool textureLinear = true;
	bool textureWrap = true;
};

class MiniwinRenderBackend {
public:
	virtual ~MiniwinRenderBackend() = default;

	virtual bool Init(SDL_Window* p_window) = 0;
	virtual void Resize(int p_width, int p_height) = 0;

	// Texture management. Pixels are RGBA8, tightly packed.
	virtual Uint32 CreateTexture(int p_width, int p_height, const void* p_rgba, bool p_mipmaps) = 0;
	virtual void UpdateTexture(Uint32 p_id, int p_width, int p_height, const void* p_rgba, bool p_mipmaps) = 0;
	virtual void DestroyTexture(Uint32 p_id) = 0;

	virtual void BeginScene() = 0;
	virtual void Clear(const SDL_Rect* p_rect, float p_r, float p_g, float p_b, bool p_color, bool p_depth) = 0;
	virtual void DrawTriangles(
		const MiniwinRasterState& p_state,
		const D3DTLVERTEX* p_vertices,
		Uint32 p_vertexCount,
		const Uint16* p_indices,
		Uint32 p_indexCount
	) = 0;
	virtual void EndScene() = 0;
	virtual void Present() = 0;

	// Reads the current backbuffer into an SDL_Surface (caller owns it).
	virtual SDL_Surface* ReadBackbuffer() = 0;

	// The logical render size the game targets (e.g. 640x480).
	int m_width = 640;
	int m_height = 480;
};

// Cross-object draw statistics (RACERS_GL_STATS).
extern Uint32 g_miniwinStatSetRenderState;
extern Uint32 g_miniwinStatSetTexture;

// Configures SDL window attributes required by the selected backend; returns extra
// SDL_WindowFlags to OR into SDL_CreateWindow. Must run on the main thread before the
// window exists.
Uint32 MiniwinBackend_PrepareWindowFlags();

// Creates the configured backend for a window (called on the game thread; the GL
// context is created and bound there).
MiniwinRenderBackend* MiniwinBackend_Create(SDL_Window* p_window, int p_width, int p_height);
