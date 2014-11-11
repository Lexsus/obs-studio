#define _CRT_SECURE_NO_WARNINGS
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/config-file.h>

#include <windows.h>
#include "graphics-hook-info.h"

extern struct graphics_offsets offsets32;
extern struct graphics_offsets offsets64;

static inline bool create_offset_proc(bool is32bit, HANDLE pipe)
{
	wchar_t *w_get_offsets_exe = NULL;
	PROCESS_INFORMATION pi = {0};
	char *get_offsets_exe = NULL;
	struct dstr str = {0};
	STARTUPINFOW si = {0};
	bool success = false;

	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = pipe;

	dstr_copy(&str, "get-graphics-offsets");
	dstr_cat(&str, is32bit ? "32.exe" : "64.exe");
	get_offsets_exe = obs_module_file(str.array);

	os_utf8_to_wcs_ptr(get_offsets_exe, 0, &w_get_offsets_exe);
	bfree(get_offsets_exe);

	if (!w_get_offsets_exe) {
		blog(LOG_ERROR, "Could not find game capture file '%s'.",
				str.array);
		goto error;
	}

	success = !!CreateProcessW(w_get_offsets_exe, NULL, NULL, NULL,
			true, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

error:
	bfree(w_get_offsets_exe);
	dstr_free(&str);
	return success;
}

static inline bool create_pipe(HANDLE *input, HANDLE *output)
{
	SECURITY_ATTRIBUTES sa = {0};

	sa.nLength = sizeof(sa);
	sa.bInheritHandle = true;

	if (!CreatePipe(input, output, &sa, 0)) {
		return false;
	}
	if (!SetHandleInformation(*input, HANDLE_FLAG_INHERIT, false)) {
		return false;
	}

	return true;
}

static inline bool load_offsets_from_string(struct graphics_offsets *offsets,
		const char *str)
{
	config_t *config;

	if (config_open_string(&config, str) != CONFIG_SUCCESS) {
		return false;
	}

	offsets->d3d9.present = config_get_uint(config, "d3d9", "present");
	offsets->d3d9.present_ex = config_get_uint(config, "d3d9", "present_ex");
	offsets->d3d9.reset = config_get_uint(config, "d3d9", "reset");
	offsets->d3d9.reset_ex = config_get_uint(config, "d3d9", "reset_ex");

	offsets->dxgi.present = config_get_uint(config, "dxgi", "present");
	offsets->dxgi.resize = config_get_uint(config, "dxgi", "resize");

	config_close(config);
	return true;
}

bool load_graphics_offsets(bool is32bit)
{
	struct dstr str = {0};
	bool success = false;
	char data[128];
	DWORD len = 0;
	HANDLE output;
	HANDLE input;

	if (!create_pipe(&input, &output)) {
		blog(LOG_ERROR, "load_graphics_offsets: Failed to create "
		                "pipe: %lu", GetLastError());
		return false;
	}

	if (!create_offset_proc(is32bit, output)) {
		blog(LOG_ERROR, "load_graphics_offsets: Failed to start "
		                "graphics offset helper program");
		goto error;
	}

	CloseHandle(output);
	output = NULL;

	for(;;) {
		bool data_read = ReadFile(input, data, 128, &len, NULL);
		if (!data_read || !len)
			break;

		dstr_ncat(&str, data, len);
	}

	success = load_offsets_from_string(is32bit ? &offsets32 : &offsets64,
			str.array);
	if (!success) {
		blog(LOG_ERROR, "load_graphics_offsets: Failed to load "
		                "string");
	}

error:
	CloseHandle(input);
	CloseHandle(output);
	dstr_free(&str);
	return success;
}
