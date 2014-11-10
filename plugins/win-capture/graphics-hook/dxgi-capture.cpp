#define _CRT_SECURE_NO_WARNINGS
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include "d3d1x_shaders.hpp"
#include "graphics-hook.h"
#include "../funchook.h"

typedef HRESULT (WINAPI *resize_buffers_t)(IDXGISwapChain*, UINT, UINT, UINT,
		DXGI_FORMAT, UINT);
typedef HRESULT (WINAPI *present_t)(IDXGISwapChain*, UINT, UINT);

static struct func_hook resize_buffers;
static struct func_hook present;

struct dxgi_swap_data {
	IDXGISwapChain *swap;
	void (*capture)(void*);
	void (*free)(void);
};

static struct dxgi_swap_data data = {};

static bool setup_dxgi(IDXGISwapChain *swap)
{
	IUnknown *device;
	HRESULT hr;

	hr = swap->GetDevice(__uuidof(ID3D10Device), (void**)&device);
	if (SUCCEEDED(hr)) {
		data.swap = swap;
		data.capture = d3d10_capture;
		data.free = d3d10_free;
		device->Release();
		return true;
	}

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void**)&device);
	if (SUCCEEDED(hr)) {
		data.swap = swap;
		data.capture = d3d11_capture;
		data.free = d3d11_free;
		device->Release();
		return true;
	}

	return false;
}

static HRESULT STDMETHODCALLTYPE hook_resize_buffers(IDXGISwapChain *swap,
		UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format,
		UINT flags)
{
	HRESULT hr;

	if (!!data.free)
		data.free();

	data.swap = nullptr;
	data.free = nullptr;
	data.capture = nullptr;

	unhook(&resize_buffers);
	resize_buffers_t call = (resize_buffers_t)resize_buffers.call_addr;
	hr = call(swap, buffer_count, width, height, format, flags);
	rehook(&resize_buffers);

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain *swap,
		UINT sync_interval, UINT flags)
{
	bool test_draw = (flags & DXGI_PRESENT_TEST) != 0;
	HRESULT hr;

	if (!data.swap && !capture_active()) {
		setup_dxgi(swap);
	}

	if (!test_draw && swap == data.swap && !!data.capture) {
		data.capture(swap);
	}

	unhook(&present);
	present_t call = (present_t)present.call_addr;
	hr = call(swap, sync_interval, flags);
	rehook(&present);

	return hr;
}

static pD3DCompile get_compiler(void)
{
	pD3DCompile compile = nullptr;
	char d3dcompiler[40] = {};
	int ver = 49;

	while (ver > 30) {
		sprintf_s(d3dcompiler, 40, "D3DCompiler_%02d.dll", ver);

		HMODULE module = LoadLibraryA(d3dcompiler);
		if (module) {
			compile = (pD3DCompile)GetProcAddress(module,
					"D3DCompile");
			if (compile) {
				break;
			}
		}

		ver--;
	}

	return compile;
}

static uint8_t vertex_shader_data[1024];
static uint8_t pixel_shader_data[1024];
static size_t vertex_shader_size = 0;
static size_t pixel_shader_size = 0;

bool hook_dxgi(void)
{
	pD3DCompile compile;
	ID3D10Blob *blob;
	HMODULE dxgi_module = get_system_module("dxgi.dll");
	HRESULT hr;
	void *present_addr;
	void *resize_addr;

	if (!dxgi_module) {
		return false;
	}

	compile = get_compiler();
	if (!compile) {
		hlog("hook_dxgi: failed to find d3d compiler library");
		return true;
	}

	/* ---------------------- */

	hr = compile(vertex_shader_string, sizeof(vertex_shader_string),
			"vertex_shader_string", nullptr, nullptr, "main",
			"vs_4_0", D3D10_SHADER_OPTIMIZATION_LEVEL1, 0, &blob,
			nullptr);
	if (FAILED(hr)) {
		hlog_hr("hook_dxgi: failed to compile vertex shader", hr);
		return true;
	}

	vertex_shader_size = (size_t)blob->GetBufferSize();
	memcpy(vertex_shader_data, blob->GetBufferPointer(),
			blob->GetBufferSize());
	blob->Release();

	/* ---------------------- */

	hr = compile(pixel_shader_string, sizeof(pixel_shader_string),
			"pixel_shader_string", nullptr, nullptr, "main",
			"ps_4_0", D3D10_SHADER_OPTIMIZATION_LEVEL1, 0, &blob,
			nullptr);
	if (FAILED(hr)) {
		hlog_hr("hook_dxgi: failed to compile pixel shader", hr);
		return true;
	}

	pixel_shader_size = (size_t)blob->GetBufferSize();
	memcpy(pixel_shader_data, blob->GetBufferPointer(),
			blob->GetBufferSize());
	blob->Release();

	/* ---------------------- */

	present_addr = get_offset_addr(dxgi_module,
			global_hook_info->offsets.dxgi.present);
	resize_addr = get_offset_addr(dxgi_module,
			global_hook_info->offsets.dxgi.resize);

	hook_init(&present, present_addr, hook_present,
			"IDXGISwapChain::Present");
	hook_init(&resize_buffers, resize_addr, hook_resize_buffers,
			"IDXGISwapChain::ResizeBuffers");

	rehook(&resize_buffers);
	rehook(&present);

	hlog("Hooked DXGI");
	return true;
}

uint8_t *get_d3d1x_vertex_shader(size_t *size)
{
	*size = vertex_shader_size;
	return vertex_shader_data;
}

uint8_t *get_d3d1x_pixel_shader(size_t *size)
{
	*size = pixel_shader_size;
	return pixel_shader_data;
}
