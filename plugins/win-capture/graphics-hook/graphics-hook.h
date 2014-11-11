#pragma once

/* conversion from data/function pointer */
#pragma warning(disable: 4152)

#include "../graphics-hook-info.h"

#ifdef __cplusplus
extern "C" {
#else
#ifndef inline
#define inline __inline
#endif
#endif

#define NUM_BUFFERS 3

extern void hlog(const char *format, ...);
extern void hlog_hr(const char *text, HRESULT hr);
extern inline const char *get_process_name(void);
extern inline HMODULE get_system_module(const char *module);
extern inline HMODULE load_system_library(const char *module);
extern uint64_t os_gettime_ns(void);

extern inline bool capture_active(void);
extern inline bool capture_should_stop(void);
extern inline bool capture_should_init(void);

extern inline void shmem_copy_data(int idx, void *volatile data);
extern inline bool shmem_texture_data_lock(int idx);
extern inline void shmem_texture_data_unlock(int idx);

extern inline bool frame_ready(uint64_t interval);

extern bool hook_ddraw(void);
extern bool hook_d3d8(void);
extern bool hook_d3d9(void);
extern bool hook_dxgi(void);
extern bool hook_gl(void);

extern void d3d10_capture(void *swap);
extern void d3d10_free(void);
extern void d3d11_capture(void *swap);
extern void d3d11_free(void);

extern uint8_t *get_d3d1x_vertex_shader(size_t *size);
extern uint8_t *get_d3d1x_pixel_shader(size_t *size);

extern bool rehook_gl(void);

extern bool capture_init_shtex(struct shtex_data **data,
		unsigned int cx, unsigned int cy, uint32_t format, bool flip,
		uint32_t handle);
extern bool capture_init_shmem(struct shmem_data **data,
		unsigned int cx, unsigned int cy, uint32_t pitch,
		uint32_t format, bool flip);
extern void capture_free(void);

extern struct hook_info *global_hook_info;

struct vertex {
	struct {
		float x, y, z, w;
	} pos;
	struct {
		float u, v;
	} tex;
};

static inline bool duplicate_handle(HANDLE *dst, HANDLE src)
{
	return !!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(),
			dst, 0, false, DUPLICATE_SAME_ACCESS);
}

static inline void *get_offset_addr(HMODULE module, uint32_t offset)
{
	return (void*)((uintptr_t)module + (uintptr_t)offset);
}

#ifdef __cplusplus
}
#endif
