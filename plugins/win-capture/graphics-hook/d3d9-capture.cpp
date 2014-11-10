#define _CRT_SECURE_NO_WARNINGS
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

#include "graphics-hook.h"
#include "../funchook.h"
#include "d3d9-patches.hpp"

typedef HRESULT (STDMETHODCALLTYPE *present_t)(IDirect3DDevice9*,
		CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *present_ex_t)(IDirect3DDevice9*,
		CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *reset_t)(IDirect3DDevice9*,
		D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *reset_ex_t)(IDirect3DDevice9*,
		D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

typedef HRESULT (WINAPI *createfactory1_t)(REFIID, void **);

static struct func_hook present;
static struct func_hook present_ex;
static struct func_hook reset;
static struct func_hook reset_ex;

struct d3d9_data {
	HMODULE                d3d9;
	uint32_t               cx;
	uint32_t               cy;
	D3DFORMAT              d3d9_format;
	DXGI_FORMAT            dxgi_format;
	IDirect3DDevice9       *device; /* do not release */
	bool                   using_shtex : 1;
	bool                   using_scale : 1;
	uint64_t               interval;

	union {
		/* shared texture */
		struct {
			IDirect3DSurface9      *d3d9_copytex;
			ID3D11Device           *d3d11_device;
			ID3D11DeviceContext    *d3d11_context;
			ID3D11Resource         *d3d11_tex;
			HANDLE                 handle;
			int                    patch;
			struct shtex_data      *shtex_info;
		};
		/* shared memory */
		struct {
			IDirect3DSurface9      *copy_surfaces[NUM_BUFFERS];
			IDirect3DSurface9      *render_targets[NUM_BUFFERS];
			IDirect3DQuery9        *queries[NUM_BUFFERS];
			volatile bool          issued_queries[NUM_BUFFERS];
			bool                   texture_mapped[NUM_BUFFERS];
			uint32_t               pitch;
			struct shmem_data      *shmem_info;
			int                    cur_tex;
			int                    copy_wait;
		};
	};
};

static struct d3d9_data data = {};

static void d3d9_free()
{
	if (data.using_shtex) {
		if (data.d3d11_tex)
			data.d3d11_tex->Release();
		if (data.d3d11_context)
			data.d3d11_context->Release();
		if (data.d3d11_device)
			data.d3d11_device->Release();
		if (data.d3d9_copytex)
			data.d3d9_copytex->Release();
	} else {
		for (size_t i = 0; i < NUM_BUFFERS; i++) {
			if (data.copy_surfaces[i]) {
				if (data.texture_mapped[i])
					data.copy_surfaces[i]->UnlockRect();
				data.copy_surfaces[i]->Release();
			}
			if (data.render_targets[i])
				data.render_targets[i]->Release();
			if (data.queries[i])
				data.queries[i]->Release();
		}
	}

	memset(&data, 0, sizeof(data));
	capture_free();

	hlog("----------------- d3d9 capture freed -----------------");
}

static DXGI_FORMAT d3d9_to_dxgi_format(D3DFORMAT format)
{
	switch (format) {
	case D3DFMT_A2B10G10R10: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case D3DFMT_A8R8G8B8:    return DXGI_FORMAT_B8G8R8A8_UNORM;
	case D3DFMT_X8R8G8B8:    return DXGI_FORMAT_B8G8R8X8_UNORM;
	}

	return DXGI_FORMAT_UNKNOWN;
}

const static D3D_FEATURE_LEVEL feature_levels[] =
{
	D3D_FEATURE_LEVEL_11_0,
	D3D_FEATURE_LEVEL_10_1,
	D3D_FEATURE_LEVEL_10_0,
	D3D_FEATURE_LEVEL_9_3,
};

static inline bool shex_init_d3d11()
{
	PFN_D3D11_CREATE_DEVICE create_device;
	createfactory1_t create_factory;
	D3D_FEATURE_LEVEL level_used;
	IDXGIFactory *factory;
	IDXGIAdapter *adapter;
	HMODULE d3d11;
	HMODULE dxgi;
	HRESULT hr;

	d3d11 = load_system_library("d3d11.dll");
	if (!d3d11) {
		hlog("d3d9_init: Failed to load D3D11");
		return false;
	}

	dxgi = load_system_library("dxgi.dll");
	if (!dxgi) {
		hlog("d3d9_init: Failed to load DXGI");
		return false;
	}

	create_factory = (createfactory1_t)GetProcAddress(dxgi,
			"CreateDXGIFactory1");
	if (!create_factory) {
		hlog("d3d9_init: Failed to get CreateDXGIFactory1 address");
		return false;
	}

	create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11,
			"D3D11CreateDevice");
	if (!create_device) {
		hlog("d3d9_init: Failed to get D3D11CreateDevice address");
		return false;
	}

	hr = create_factory(__uuidof(IDXGIFactory1), (void**)&factory);
	if (FAILED(hr)) {
		hlog_hr("d3d9_init: Failed to create factory object", hr);
		return false;
	}

	hr = factory->EnumAdapters(0, &adapter);
	factory->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_init: Failed to get adapter", hr);
		return false;
	}

	hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
			0, feature_levels,
			sizeof(feature_levels) / sizeof(D3D_FEATURE_LEVEL),
			D3D11_SDK_VERSION, &data.d3d11_device, &level_used,
			&data.d3d11_context);
	adapter->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_init: Failed to create D3D11 device", hr);
		return false;
	}

	return true;
}

static inline bool d3d9_shtex_init_shtex()
{
	IDXGIResource *res;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width                = data.cx;
	desc.Height               = data.cy;
	desc.Format               = data.dxgi_format;
	desc.MipLevels            = 1;
	desc.ArraySize            = 1;
	desc.SampleDesc.Count     = 1;
	desc.Usage                = D3D11_USAGE_DEFAULT;
	desc.MiscFlags            = D3D11_RESOURCE_MISC_SHARED;
	desc.BindFlags            = D3D11_BIND_RENDER_TARGET |
	                            D3D11_BIND_SHADER_RESOURCE;

	hr = data.d3d11_device->CreateTexture2D(&desc, nullptr,
			(ID3D11Texture2D**)&data.d3d11_tex);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_shtex: Failed to create D3D11 texture",
				hr);
		return false;
	}

	hr = data.d3d11_tex->QueryInterface(__uuidof(IDXGIResource),
			(void**)&res);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_shtex: Failed to query IDXGIResource",
				hr);
		return false;
	}

	hr = res->GetSharedHandle(&data.handle);
	res->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_shtex: Failed to get shared handle",
				hr);
		return false;
	}

	return true;
}

static inline bool d3d9_shtex_init_copytex()
{
	uint8_t *patch_addr = get_d3d9_patch_addr(data.d3d9, data.patch);
	uint8_t saved_data[PATCH_SIZE];
	IDirect3DTexture9 *tex;
	DWORD protect_val;
	HRESULT hr;

	if (patch_addr) {
		VirtualProtect(patch_addr, PATCH_SIZE, PAGE_EXECUTE_READWRITE,
				&protect_val);
		memcpy(saved_data, patch_addr, PATCH_SIZE);
		memcpy(patch_addr, patch[data.patch], PATCH_SIZE);
	}

	hr = data.device->CreateTexture(data.cx, data.cy, 1,
			D3DUSAGE_RENDERTARGET, data.d3d9_format,
			D3DPOOL_DEFAULT, &tex, &data.handle);

	if (patch_addr) {
		memcpy(patch_addr, saved_data, PATCH_SIZE);
		VirtualProtect(patch_addr, PATCH_SIZE, protect_val,
				&protect_val);
	}

	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_copytex: Failed to create shared texture",
				hr);
		return false;
	}

	hr = tex->GetSurfaceLevel(0, &data.d3d9_copytex);
	tex->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_copytex: Failed to get surface level", hr);
		return false;
	}

	return true;
}

static bool d3d9_shtex_init()
{
	data.using_shtex = true;

	if (!shex_init_d3d11()) {
		return false;
	}
	if (!d3d9_shtex_init_shtex()) {
		return false;
	}
	if (!d3d9_shtex_init_copytex()) {
		return false;
	}
	if (!capture_init_shtex(&data.shtex_info, data.cx, data.cy,
				data.dxgi_format, false,
				(uint32_t)data.handle)) {
		return false;
	}

	hlog("d3d9 shared texture capture successful");
	return true;
}

static bool d3d9_shmem_init_buffers(size_t buffer)
{
	HRESULT hr;

	hr = data.device->CreateOffscreenPlainSurface(data.cx, data.cy,
			data.d3d9_format, D3DPOOL_SYSTEMMEM,
			&data.copy_surfaces[buffer], nullptr);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_init_buffers: Failed to create surface",
				hr);
		return false;
	}

	if (buffer == 0) {
		D3DLOCKED_RECT rect;
		hr = data.copy_surfaces[buffer]->LockRect(&rect, nullptr,
				D3DLOCK_READONLY);
		if (FAILED(hr)) {
			hlog_hr("d3d9_shmem_init_buffers: Failed to lock "
			        "buffer", hr);
			return false;
		}

		data.pitch = rect.Pitch;
		data.copy_surfaces[buffer]->UnlockRect();
	}	

	hr = data.device->CreateRenderTarget(data.cx, data.cy,
			data.d3d9_format, D3DMULTISAMPLE_NONE, 0, false,
			&data.render_targets[buffer], nullptr);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_init_buffers: Failed to create render "
		        "target", hr);
		return false;
	}

	hr = data.device->CreateQuery(D3DQUERYTYPE_EVENT,
			&data.queries[buffer]);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_init_buffers: Failed to create query", hr);
		return false;
	}

	return true;
}

static bool d3d9_shmem_init()
{
	data.using_shtex = false;

	for (size_t i = 0; i < NUM_BUFFERS; i++) {
		if (!d3d9_shmem_init_buffers(i)) {
			return false;
		}
	}
	if (!capture_init_shmem(&data.shmem_info, data.cx, data.cy,
				data.pitch, data.dxgi_format, false)) {
		return false;
	}

	hlog("d3d9 memory capture successful");
	return true;
}

static bool d3d9_init_format_backbuffer()
{
	IDirect3DSurface9 *back_buffer = nullptr;
	D3DSURFACE_DESC desc;
	HRESULT hr;

	hr = data.device->GetRenderTarget(0, &back_buffer);
	if (FAILED(hr)) {
		return false;
	}

	hr = back_buffer->GetDesc(&desc);
	back_buffer->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_init_format_backbuffer: Failed to get "
		        "backbuffer descriptor", hr);
		return false;
	}

	data.d3d9_format = desc.Format;
	data.dxgi_format = d3d9_to_dxgi_format(desc.Format);
	data.using_scale = global_hook_info->use_scale;
	data.interval = global_hook_info->frame_interval;

	if (data.using_scale) {
		data.cx = global_hook_info->cx;
		data.cy = global_hook_info->cy;
	} else {
		data.cx = desc.Width;
		data.cy = desc.Height;
	}

	return true;
}

static bool d3d9_init_format_swapchain()
{
	IDirect3DSwapChain9 *swap = nullptr;
	D3DPRESENT_PARAMETERS pp;
	HRESULT hr;

	hr = data.device->GetSwapChain(0, &swap);
	if (FAILED(hr)) {
		hlog_hr("d3d9_init_format_swapchain: Failed to get swap chain",
				hr);
		return false;
	}

	hr = swap->GetPresentParameters(&pp);
	swap->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_init_format_swapchain: Failed to get "
		        "presentation parameters", hr);
		return false;
	}

	data.dxgi_format = d3d9_to_dxgi_format(pp.BackBufferFormat);
	data.d3d9_format = pp.BackBufferFormat;
	data.using_scale = global_hook_info->use_scale;
	data.interval = global_hook_info->frame_interval;

	if (data.using_scale) {
		data.cx = global_hook_info->cx;
		data.cy = global_hook_info->cy;
	} else {
		data.cx = pp.BackBufferWidth;
		data.cy = pp.BackBufferHeight;
	}

	return true;
}

static void d3d9_init(IDirect3DDevice9 *device)
{
	IDirect3DDevice9Ex *d3d9ex = nullptr;
	bool success;
	HRESULT hr;

	data.d3d9 = get_system_module("d3d9.dll");
	data.device = device;

	hr = device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
			(void**)&d3d9ex);
	if (SUCCEEDED(hr)) {
		d3d9ex->Release();
		data.patch = -1;
	} else {
		data.patch = get_d3d9_patch(data.d3d9);
	}

	if (!d3d9_init_format_backbuffer()) {
		if (!d3d9_init_format_swapchain()) {
			return;
		}
	}

	if (global_hook_info->force_shmem || (!d3d9ex && data.patch == -1)) {
		success = d3d9_shmem_init();
	} else {
		success = d3d9_shtex_init();
	}

	if (!success)
		d3d9_free();
}

static inline HRESULT get_backbuffer(bool use_backbuffer,
		IDirect3DSurface9 **surface)
{
	if (use_backbuffer) {
		return data.device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
				surface);
	} else {
		return data.device->GetRenderTarget(0, surface);
	}
}

static inline void d3d9_shtex_capture(bool use_backbuffer)
{
	IDirect3DSurface9 *surface = nullptr;
	D3DTEXTUREFILTERTYPE filter;
	HRESULT hr;

	hr = get_backbuffer(use_backbuffer, &surface);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_capture: Failed to get backbuffer", hr);
		return;
	}

	filter = data.using_scale ? D3DTEXF_LINEAR : D3DTEXF_NONE;

	hr = data.device->StretchRect(surface, nullptr, data.d3d9_copytex,
			nullptr, filter);
	surface->Release();

	if (FAILED(hr))
		hlog_hr("d3d9_shtex_capture: StretchRect failed", hr);
}

static inline void d3d9_shmem_capture_queue_copy()
{
	for (int i = 0; i < NUM_BUFFERS; i++) {
		IDirect3DSurface9 *target = data.copy_surfaces[i];
		D3DLOCKED_RECT rect;
		HRESULT hr;

		if (!data.issued_queries[i]) {
			continue;
		}
		if (data.queries[i]->GetData(0, 0, 0) != S_OK) {
			continue;
		}

		data.issued_queries[i] = false;

		hr = target->LockRect(&rect, nullptr, D3DLOCK_READONLY);
		if (SUCCEEDED(hr)) {
			data.texture_mapped[i] = true;
			shmem_copy_data(i, rect.pBits);
		}
		break;
	}
}

static inline void d3d9_shmem_capture(bool use_backbuffer)
{
	IDirect3DSurface9 *surface = nullptr;
	D3DTEXTUREFILTERTYPE filter;
	IDirect3DSurface9 *copy;
	int next_tex;
	HRESULT hr;

	d3d9_shmem_capture_queue_copy();

	hr = get_backbuffer(use_backbuffer, &surface);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_capture: Failed to get backbuffer", hr);
		return;
	}

	next_tex = (data.cur_tex == NUM_BUFFERS - 1) ?  0 : data.cur_tex + 1;
	filter = data.using_scale ? D3DTEXF_LINEAR : D3DTEXF_NONE;
	copy = data.render_targets[data.cur_tex];

	hr = data.device->StretchRect(surface, nullptr, copy, nullptr, filter);
	surface->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_capture: StretchRect failed", hr);
		return;
	}

	if (data.copy_wait < NUM_BUFFERS - 1) {
		data.copy_wait++;
	} else {
		IDirect3DSurface9 *src = data.render_targets[next_tex];
		IDirect3DSurface9 *dst = data.copy_surfaces[next_tex];

		if (shmem_texture_data_lock(next_tex)) {
			dst->UnlockRect();
			data.texture_mapped[next_tex] = false;
			shmem_texture_data_unlock(next_tex);
		}

		hr = data.device->GetRenderTargetData(src, dst);
		if (FAILED(hr)) {
			hlog_hr("d3d9_shmem_capture: GetRenderTargetData "
			        "failed", hr);
		}

		data.queries[next_tex]->Issue(D3DISSUE_END);
		data.issued_queries[next_tex] = true;
	}

	data.cur_tex = next_tex;
}

static void d3d9_capture(IDirect3DDevice9 *device)
{
	static bool use_backbuffer = false;
	static bool checked_exceptions = false;

	if (!checked_exceptions) {
		if (_strcmpi(get_process_name(), "hotd_ng.exe") == 0)
			use_backbuffer = true;
		checked_exceptions = true;
	}

	if (capture_should_stop()) {
		d3d9_free();
	}
	if (capture_should_init()) {
		d3d9_init(device);
	}
	if (capture_active()) {
		if (!frame_ready(data.interval)) {
			return;
		}

		if (data.using_shtex)
			d3d9_shtex_capture(use_backbuffer);
		else
			d3d9_shmem_capture(use_backbuffer);
	}
}

/* this is used just in case Present calls PresentEx or vise versa. */
static int present_recurse = 0;

static HRESULT STDMETHODCALLTYPE hook_present(IDirect3DDevice9 *device,
		CONST RECT *src_rect, CONST RECT *dst_rect,
		HWND override_window, CONST RGNDATA *dirty_region)
{
	HRESULT hr;

	if (!present_recurse)
		d3d9_capture(device);

	present_recurse++;

	unhook(&present);
	present_t call = (present_t)present.call_addr;
	hr = call(device, src_rect, dst_rect, override_window, dirty_region);
	rehook(&present);

	present_recurse--;

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present_ex(IDirect3DDevice9 *device,
		CONST RECT *src_rect, CONST RECT *dst_rect,
		HWND override_window, CONST RGNDATA *dirty_region, DWORD flags)
{
	HRESULT hr;

	if (!present_recurse)
		d3d9_capture(device);

	present_recurse++;

	unhook(&present_ex);
	present_ex_t call = (present_ex_t)present_ex.call_addr;
	hr = call(device, src_rect, dst_rect, override_window, dirty_region,
			flags);
	rehook(&present_ex);

	present_recurse--;

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_reset(IDirect3DDevice9 *device,
		D3DPRESENT_PARAMETERS *params)
{
	HRESULT hr;

	if (capture_active())
		d3d9_free();

	unhook(&reset);
	reset_t call = (reset_t)reset.call_addr;
	hr = call(device, params);
	rehook(&reset);

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_reset_ex(IDirect3DDevice9 *device,
		D3DPRESENT_PARAMETERS *params, D3DDISPLAYMODEEX *dmex)
{
	HRESULT hr;

	if (capture_active())
		d3d9_free();

	unhook(&reset_ex);
	reset_ex_t call = (reset_ex_t)reset_ex.call_addr;
	hr = call(device, params, dmex);
	rehook(&reset_ex);

	return hr;
}

bool hook_d3d9(void)
{
	HMODULE d3d9_module = get_system_module("d3d9.dll");
	void *present_addr;
	void *present_ex_addr;
	void *reset_addr;
	void *reset_ex_addr;

	if (!d3d9_module) {
		return false;
	}

	present_addr = get_offset_addr(d3d9_module,
			global_hook_info->offsets.d3d9.present);
	present_ex_addr = get_offset_addr(d3d9_module,
			global_hook_info->offsets.d3d9.present_ex);
	reset_addr = get_offset_addr(d3d9_module,
			global_hook_info->offsets.d3d9.reset);
	reset_ex_addr = get_offset_addr(d3d9_module,
			global_hook_info->offsets.d3d9.reset_ex);

	hook_init(&present, present_addr, hook_present,
			"IDirect3DDevice9::Present");
	hook_init(&present_ex, present_ex_addr, hook_present_ex,
			"IDirect3DDevice9Ex::PresentEx");
	hook_init(&reset, reset_addr, hook_reset,
			"IDirect3DDevice9::Reset");
	hook_init(&reset_ex, reset_ex_addr, hook_reset_ex,
			"IDirect3DDevice9Ex::ResetEx");

	rehook(&reset_ex);
	rehook(&reset);
	rehook(&present_ex);
	rehook(&present);

	hlog("Hooked D3D9");
	return true;
}
