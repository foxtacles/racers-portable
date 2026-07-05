// [library:3d] IDirect3DDevice3 / IDirect3DViewport3 / IDirect3DMaterial3 emulation.
// M1: state is recorded and draws are dropped (null backend); the render backends
// (SDL3 GPU / OpenGL / OpenGL ES) attach here in M2.

#include "ddraw_impl.h"
#include "miniwin.h"
#include "renderbackend.h"

#include <miniwin/d3d.h>
#include <string.h>
#include <vector>

void MiniwinFillDeviceDesc(D3DDEVICEDESC* p_desc); // d3d.cpp

Uint32 g_miniwinStatSetRenderState = 0;
Uint32 g_miniwinStatSetTexture = 0;

// D3DMATERIALHANDLE is a 32-bit DWORD; hand out small ids instead of pointers.
static std::vector<IDirect3DMaterial3*> g_materialHandles;

struct MiniwinD3DDevice : public IDirect3DDevice3 {
	explicit MiniwinD3DDevice(MiniwinSurface* p_renderTarget) : m_renderTarget(p_renderTarget) {}

	HRESULT EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK lpd3dEnumPixelProc, LPVOID lpArg) override;
	HRESULT GetCaps(LPD3DDEVICEDESC lpD3DHWDevDesc, LPD3DDEVICEDESC lpD3DHELDevDesc) override;

	MiniwinRenderBackend* Backend()
	{
		return m_renderTarget && m_renderTarget->m_ddraw ? m_renderTarget->m_ddraw->GetBackend() : nullptr;
	}

	// Uploads (or refreshes) a texture surface to the backend and returns its id.
	Uint32 EnsureTexture(MiniwinSurface* p_surface)
	{
		MiniwinRenderBackend* backend = Backend();
		if (!backend || !p_surface) {
			return 0;
		}

		int paletteVersion = p_surface->m_palette ? p_surface->m_palette->m_version : -1;
		bool needsUpload = p_surface->m_backendTexture == 0 || p_surface->m_textureDirty ||
						   p_surface->m_paletteVersion != paletteVersion;
		if (!needsUpload) {
			return p_surface->m_backendTexture;
		}

		int width = (int) p_surface->m_desc.dwWidth;
		int height = (int) p_surface->m_desc.dwHeight;
		m_convertScratch.resize((size_t) width * height * 4);
		if (!p_surface->ConvertToRGBA(m_convertScratch.data())) {
			return 0;
		}

		bool mipmaps = (p_surface->m_desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP) != 0;
		if (p_surface->m_backendTexture == 0) {
			p_surface->m_backendTexture = backend->CreateTexture(width, height, m_convertScratch.data(), mipmaps);
		}
		else {
			backend->UpdateTexture(p_surface->m_backendTexture, width, height, m_convertScratch.data(), mipmaps);
		}
		if (getenv("RACERS_DUMP_TEX")) {
			SDL_LogInfo(
				LOG_CATEGORY_MINIWIN,
				"upload tex id=%u %dx%d flags=0x%x bpp=%u aMask=0x%x key=%d(%u) pal=%d",
				p_surface->m_backendTexture,
				width,
				height,
				(unsigned) p_surface->m_desc.ddpfPixelFormat.dwFlags,
				(unsigned) p_surface->m_desc.ddpfPixelFormat.dwRGBBitCount,
				(unsigned) p_surface->m_desc.ddpfPixelFormat.dwRGBAlphaBitMask,
				(int) p_surface->m_hasColorKey,
				(unsigned) p_surface->m_colorKey.dwColorSpaceLowValue,
				p_surface->m_palette != nullptr
			);
		}

		p_surface->m_textureDirty = false;
		p_surface->m_paletteVersion = paletteVersion;
		return p_surface->m_backendTexture;
	}

	MiniwinRasterState BuildState()
	{
		MiniwinRasterState state;
		DWORD zEnable = m_renderStates[D3DRENDERSTATE_ZENABLE];
		state.zEnable = zEnable != D3DZB_FALSE;
		state.zWrite = m_renderStates[D3DRENDERSTATE_ZWRITEENABLE] != 0;
		state.zFunc = (D3DCMPFUNC) m_renderStates[D3DRENDERSTATE_ZFUNC];
		if (state.zFunc == 0) {
			state.zFunc = D3DCMP_LESSEQUAL;
		}
		state.alphaBlend = m_renderStates[D3DRENDERSTATE_ALPHABLENDENABLE] != 0;
		state.srcBlend = (D3DBLEND) m_renderStates[D3DRENDERSTATE_SRCBLEND];
		if (state.srcBlend == 0) {
			state.srcBlend = D3DBLEND_ONE;
		}
		state.destBlend = (D3DBLEND) m_renderStates[D3DRENDERSTATE_DESTBLEND];
		if (state.destBlend == 0) {
			state.destBlend = D3DBLEND_ZERO;
		}
		state.alphaTest = m_renderStates[D3DRENDERSTATE_ALPHATESTENABLE] != 0;
		state.alphaFunc = (D3DCMPFUNC) m_renderStates[D3DRENDERSTATE_ALPHAFUNC];
		if (state.alphaFunc == 0) {
			state.alphaFunc = D3DCMP_ALWAYS;
		}
		state.alphaRef = (float) m_renderStates[D3DRENDERSTATE_ALPHAREF] / 255.0f;
		state.cullMode = (D3DCULL) m_renderStates[D3DRENDERSTATE_CULLMODE];
		if (state.cullMode == 0) {
			state.cullMode = D3DCULL_NONE;
		}
		state.specular = m_renderStates[D3DRENDERSTATE_SPECULARENABLE] != 0;

		// The game switches D3DTSS_COLOROP/ALPHAOP between MODULATE, SELECTARG1
		// (texture only) and SELECTARG2 (diffuse only) per material — and leaves the
		// previous texture bound for untextured materials, so the ops decide whether
		// the sample participates at all.
		state.colorOp = TextureOpFromStageState(m_textureStageStates[0][D3DTSS_COLOROP]);
		state.alphaOp = TextureOpFromStageState(m_textureStageStates[0][D3DTSS_ALPHAOP]);

		bool usesTexture = state.colorOp != MiniwinTextureOp::Diffuse || state.alphaOp != MiniwinTextureOp::Diffuse;
		state.textured = m_texture != nullptr && usesTexture;
		if (state.textured) {
			state.textureId = EnsureTexture(m_texture);
			state.textured = state.textureId != 0;
		}
		if (!state.textured) {
			state.colorOp = MiniwinTextureOp::Diffuse;
			state.alphaOp = MiniwinTextureOp::Diffuse;
		}
		state.textureLinear = m_textureStageStates[0][D3DTSS_MAGFILTER] != D3DTFG_POINT;
		state.textureWrap = m_textureStageStates[0][D3DTSS_ADDRESS] != D3DTADDRESS_CLAMP;

		// Color keying: keyed texels were converted to alpha 0 at upload; reject them
		// based on the sampled texture alpha alone, like the D3D6 texel-level colorkey
		// did. Gating on the combined alpha instead would erase whole draws whose
		// vertex alpha fades below the threshold (e.g. the magnet's sparkle column).
		state.colorKeyTest = state.textured && m_renderStates[D3DRENDERSTATE_COLORKEYENABLE] &&
							 m_texture->m_hasColorKey;

		return state;
	}

	static MiniwinTextureOp TextureOpFromStageState(DWORD p_op)
	{
		switch (p_op) {
		case D3DTOP_SELECTARG1:
			return MiniwinTextureOp::Texture;
		case D3DTOP_SELECTARG2:
		case D3DTOP_DISABLE:
			return MiniwinTextureOp::Diffuse;
		case D3DTOP_MODULATE:
		default:
			return MiniwinTextureOp::Modulate;
		}
	}

	MiniwinSurface* m_renderTarget;
	DWORD m_renderStates[256] = {};
	DWORD m_textureStageStates[2][32] = {};
	MiniwinSurface* m_texture = nullptr;
	IDirect3DViewport3* m_currentViewport = nullptr;
	std::vector<Uint8> m_convertScratch;
	std::vector<Uint16> m_stripIndices;
};

IDirect3DDevice3* MiniwinCreateDevice(MiniwinSurface* p_renderTarget)
{
	return new MiniwinD3DDevice(p_renderTarget);
}

static void FillTextureFormat16(DDPIXELFORMAT* format, DWORD r, DWORD g, DWORD b, DWORD a)
{
	memset(format, 0, sizeof(*format));
	format->dwSize = sizeof(*format);
	format->dwFlags = a ? (DDPF_RGB | DDPF_ALPHAPIXELS) : DDPF_RGB;
	format->dwRGBBitCount = 16;
	format->dwRBitMask = r;
	format->dwGBitMask = g;
	format->dwBBitMask = b;
	format->dwRGBAlphaBitMask = a;
}

HRESULT MiniwinD3DDevice::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK lpd3dEnumPixelProc, LPVOID lpArg)
{
	DDPIXELFORMAT format;

	// RGB565
	FillTextureFormat16(&format, 0xF800, 0x07E0, 0x001F, 0);
	if (lpd3dEnumPixelProc(&format, lpArg) == 0) {
		return D3D_OK;
	}

	// ARGB1555
	FillTextureFormat16(&format, 0x7C00, 0x03E0, 0x001F, 0x8000);
	if (lpd3dEnumPixelProc(&format, lpArg) == 0) {
		return D3D_OK;
	}

	// ARGB4444
	FillTextureFormat16(&format, 0x0F00, 0x00F0, 0x000F, 0xF000);
	if (lpd3dEnumPixelProc(&format, lpArg) == 0) {
		return D3D_OK;
	}

	// XRGB8888
	memset(&format, 0, sizeof(format));
	format.dwSize = sizeof(format);
	format.dwFlags = DDPF_RGB;
	format.dwRGBBitCount = 32;
	format.dwRBitMask = 0x00FF0000;
	format.dwGBitMask = 0x0000FF00;
	format.dwBBitMask = 0x000000FF;
	if (lpd3dEnumPixelProc(&format, lpArg) == 0) {
		return D3D_OK;
	}

	// ARGB8888
	format.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
	format.dwRGBAlphaBitMask = 0xFF000000;
	if (lpd3dEnumPixelProc(&format, lpArg) == 0) {
		return D3D_OK;
	}

	// P8 (palettized)
	memset(&format, 0, sizeof(format));
	format.dwSize = sizeof(format);
	format.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
	format.dwRGBBitCount = 8;
	lpd3dEnumPixelProc(&format, lpArg);
	return D3D_OK;
}

HRESULT MiniwinD3DDevice::GetCaps(LPD3DDEVICEDESC lpD3DHWDevDesc, LPD3DDEVICEDESC lpD3DHELDevDesc)
{
	if (lpD3DHWDevDesc) {
		MiniwinFillDeviceDesc(lpD3DHWDevDesc);
	}
	if (lpD3DHELDevDesc) {
		memset(lpD3DHELDevDesc, 0, sizeof(*lpD3DHELDevDesc));
		lpD3DHELDevDesc->dwSize = sizeof(*lpD3DHELDevDesc);
	}
	return D3D_OK;
}

// --- IDirect3DDevice3 default bodies (recorded state; draws dropped until M2) ---

HRESULT IDirect3DDevice3::AddViewport(LPDIRECT3DVIEWPORT3 lpDirect3DViewport)
{
	if (lpDirect3DViewport) {
		lpDirect3DViewport->m_device = this;
	}
	return D3D_OK;
}

HRESULT IDirect3DDevice3::DeleteViewport(LPDIRECT3DVIEWPORT3 lpDirect3DViewport)
{
	return D3D_OK;
}

HRESULT IDirect3DDevice3::SetCurrentViewport(LPDIRECT3DVIEWPORT3 lpDirect3DViewport)
{
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	self->m_currentViewport = lpDirect3DViewport;
	return D3D_OK;
}

HRESULT IDirect3DDevice3::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK, LPVOID)
{
	return D3DERR_GENERIC;
}

HRESULT IDirect3DDevice3::GetCaps(LPD3DDEVICEDESC, LPD3DDEVICEDESC)
{
	return D3DERR_GENERIC;
}

HRESULT IDirect3DDevice3::BeginScene()
{
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	if (MiniwinRenderBackend* backend = self->Backend()) {
		backend->BeginScene();
	}
	return D3D_OK;
}

HRESULT IDirect3DDevice3::EndScene()
{
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	if (MiniwinRenderBackend* backend = self->Backend()) {
		backend->EndScene();
	}
	return D3D_OK;
}

HRESULT IDirect3DDevice3::SetRenderState(D3DRENDERSTATETYPE dwRenderStateType, DWORD dwRenderState)
{
	g_miniwinStatSetRenderState++;
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	if ((unsigned) dwRenderStateType < 256) {
		self->m_renderStates[dwRenderStateType] = dwRenderState;
	}
	return D3D_OK;
}

HRESULT IDirect3DDevice3::GetRenderState(D3DRENDERSTATETYPE dwRenderStateType, LPDWORD lpdwRenderState)
{
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	if (lpdwRenderState && (unsigned) dwRenderStateType < 256) {
		*lpdwRenderState = self->m_renderStates[dwRenderStateType];
	}
	return D3D_OK;
}

HRESULT IDirect3DDevice3::SetTextureStageState(DWORD dwStage, D3DTEXTURESTAGESTATETYPE dwState, DWORD dwValue)
{
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	if (dwStage < 2 && (unsigned) dwState < 32) {
		self->m_textureStageStates[dwStage][dwState] = dwValue;
	}
	return D3D_OK;
}

HRESULT IDirect3DDevice3::SetTexture(DWORD dwStage, LPDIRECT3DTEXTURE2 lpTexture)
{
	g_miniwinStatSetTexture++;
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	if (dwStage == 0) {
		self->m_texture = static_cast<MiniwinSurface*>(lpTexture);
	}
	return D3D_OK;
}

HRESULT IDirect3DDevice3::SetRenderTarget(LPDIRECTDRAWSURFACE lpNewRenderTarget, DWORD dwFlags)
{
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	self->m_renderTarget = static_cast<MiniwinSurface*>(lpNewRenderTarget);
	return D3D_OK;
}

HRESULT IDirect3DDevice3::DrawPrimitive(
	D3DPRIMITIVETYPE dptPrimitiveType,
	DWORD dwVertexTypeDesc,
	LPVOID lpvVertices,
	DWORD dwVertexCount,
	DWORD dwFlags
)
{
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	MiniwinRenderBackend* backend = self->Backend();
	if (!backend || dwVertexCount < 3) {
		return D3D_OK;
	}

	MiniwinRasterState state = self->BuildState();
	const D3DTLVERTEX* vertices = (const D3DTLVERTEX*) lpvVertices;

	if (dptPrimitiveType == D3DPT_TRIANGLELIST) {
		backend->DrawTriangles(state, vertices, dwVertexCount, nullptr, 0);
	}
	else if (dptPrimitiveType == D3DPT_TRIANGLESTRIP) {
		// Triangulate the strip (alternating winding).
		self->m_stripIndices.clear();
		for (DWORD i = 2; i < dwVertexCount; i++) {
			if (i & 1) {
				self->m_stripIndices.push_back((Uint16) (i - 1));
				self->m_stripIndices.push_back((Uint16) (i - 2));
			}
			else {
				self->m_stripIndices.push_back((Uint16) (i - 2));
				self->m_stripIndices.push_back((Uint16) (i - 1));
			}
			self->m_stripIndices.push_back((Uint16) i);
		}
		backend->DrawTriangles(
			state,
			vertices,
			dwVertexCount,
			self->m_stripIndices.data(),
			(Uint32) self->m_stripIndices.size()
		);
	}

	return D3D_OK;
}

HRESULT IDirect3DDevice3::DrawIndexedPrimitive(
	D3DPRIMITIVETYPE dptPrimitiveType,
	DWORD dwVertexTypeDesc,
	LPVOID lpvVertices,
	DWORD dwVertexCount,
	LPWORD lpwIndices,
	DWORD dwIndexCount,
	DWORD dwFlags
)
{
	MiniwinD3DDevice* self = static_cast<MiniwinD3DDevice*>(this);
	MiniwinRenderBackend* backend = self->Backend();
	if (!backend || dwIndexCount < 3) {
		return D3D_OK;
	}

	MiniwinRasterState state = self->BuildState();
	backend->DrawTriangles(state, (const D3DTLVERTEX*) lpvVertices, dwVertexCount, lpwIndices, dwIndexCount);
	return D3D_OK;
}

// --- IDirect3DViewport3 ---

HRESULT IDirect3DViewport3::SetViewport2(LPD3DVIEWPORT2 lpData)
{
	if (lpData) {
		m_viewport = *lpData;
	}
	return D3D_OK;
}

HRESULT IDirect3DViewport3::GetViewport2(LPD3DVIEWPORT2 lpData)
{
	if (lpData) {
		*lpData = m_viewport;
	}
	return D3D_OK;
}

HRESULT IDirect3DViewport3::SetBackground(D3DMATERIALHANDLE hMat)
{
	m_background = hMat;
	return D3D_OK;
}

HRESULT IDirect3DViewport3::Clear(DWORD dwCount, LPD3DRECT lpRects, DWORD dwFlags)
{
	MiniwinD3DDevice* device = static_cast<MiniwinD3DDevice*>(m_device);
	MiniwinRenderBackend* backend = device ? device->Backend() : nullptr;
	if (!backend) {
		return D3D_OK;
	}

	float r = 0.f;
	float g = 0.f;
	float b = 0.f;
	if (m_background && m_background <= (D3DMATERIALHANDLE) g_materialHandles.size()) {
		IDirect3DMaterial3* material = g_materialHandles[m_background - 1];
		r = material->m_material.diffuse.r;
		g = material->m_material.diffuse.g;
		b = material->m_material.diffuse.b;
	}

	SDL_Rect rect;
	SDL_Rect* rectPtr = nullptr;
	if (dwCount >= 1 && lpRects) {
		rect.x = lpRects[0].x1;
		rect.y = lpRects[0].y1;
		rect.w = lpRects[0].x2 - lpRects[0].x1;
		rect.h = lpRects[0].y2 - lpRects[0].y1;
		rectPtr = &rect;
	}

	backend->Clear(rectPtr, r, g, b, (dwFlags & D3DCLEAR_TARGET) != 0, (dwFlags & D3DCLEAR_ZBUFFER) != 0);
	return D3D_OK;
}

// --- IDirect3DMaterial3 ---

HRESULT IDirect3DMaterial3::SetMaterial(LPD3DMATERIAL lpMat)
{
	if (lpMat) {
		m_material = *lpMat;
	}
	return D3D_OK;
}

HRESULT IDirect3DMaterial3::GetMaterial(LPD3DMATERIAL lpMat)
{
	if (lpMat) {
		*lpMat = m_material;
	}
	return D3D_OK;
}

HRESULT IDirect3DMaterial3::GetHandle(LPDIRECT3DDEVICE3 lpDirect3DDevice, D3DMATERIALHANDLE* lpHandle)
{
	if (m_handle == 0) {
		g_materialHandles.push_back(this);
		m_handle = (D3DMATERIALHANDLE) g_materialHandles.size();
	}
	if (lpHandle) {
		*lpHandle = m_handle;
	}
	return D3D_OK;
}
