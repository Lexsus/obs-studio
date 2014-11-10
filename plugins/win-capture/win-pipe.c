#include <windows.h>
#include <stdlib.h>
#include "win-pipe.h"

#ifndef inline
#define inline __inline
#endif

#define PIPE_BUF_SIZE 4096

void win_pipe_server_free(win_pipe_server_t *pipe)
{
	if (!pipe)
		return;

	if (pipe->thread) {
		CancelIoEx(pipe->handle, &pipe->overlap);
		SetEvent(pipe->ready_event);
		WaitForSingleObject(pipe->thread, INFINITE);
		CloseHandle(pipe->thread);
	}
	if (pipe->ready_event)
		CloseHandle(pipe->ready_event);
	if (pipe->handle)
		CloseHandle(pipe->handle);

	free(pipe->read_data);
	memset(pipe, 0, sizeof(*pipe));
}

void win_pipe_client_free(win_pipe_client_t *pipe)
{
	if (!pipe)
		return;

	if (pipe->handle)
		CloseHandle(pipe->handle);

	memset(pipe, 0, sizeof(*pipe));
}

static inline bool io_pending(void)
{
	return GetLastError() == ERROR_IO_PENDING;
}

static inline bool create_events(win_pipe_server_t *pipe)
{
	pipe->ready_event = CreateEvent(NULL, false, false, NULL);
	return !!pipe->ready_event;
}

static inline bool create_pipe(win_pipe_server_t *pipe, const char *name)
{
	const DWORD access = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
	const DWORD flags = PIPE_TYPE_MESSAGE     |
	                    PIPE_READMODE_MESSAGE |
	                    PIPE_WAIT;

	pipe->handle = CreateNamedPipeA(name, access, flags, 1,
			PIPE_BUF_SIZE, PIPE_BUF_SIZE, 0, NULL);

	return pipe->handle != INVALID_HANDLE_VALUE;
}

static inline void ensure_capacity(win_pipe_server_t *pipe, size_t new_size)
{
	if (pipe->capacity >= new_size) {
		return;
	}

	pipe->read_data = realloc(pipe->read_data, new_size);
	pipe->capacity  = new_size;
}

static inline void append_bytes(win_pipe_server_t *pipe, uint8_t *bytes,
		size_t size)
{
	size_t new_size = pipe->size + size;
	ensure_capacity(pipe, new_size);
	memcpy(pipe->read_data + pipe->size, bytes, size);
	pipe->size = new_size;
}

static inline void close_callback(win_pipe_server_t *pipe)
{
	if (pipe->close_callback)
		pipe->close_callback(pipe->param);
}

static DWORD CALLBACK server_thread(LPVOID param)
{
	win_pipe_server_t *pipe = param;
	uint8_t buf[PIPE_BUF_SIZE];

	/* wait for connection */
	DWORD wait = WaitForSingleObject(pipe->ready_event, INFINITE);
	if (wait != WAIT_OBJECT_0) {
		close_callback(pipe);
		return 0;
	}

	for (;;) {
		DWORD bytes = 0;
		bool  success;

		success = !!ReadFile(pipe->handle, buf, PIPE_BUF_SIZE, NULL,
				&pipe->overlap);
		if (!success && !io_pending()) {
			break;
		}

		DWORD wait = WaitForSingleObject(pipe->ready_event, INFINITE);
		if (wait != WAIT_OBJECT_0) {
			break;
		}

		success = !!GetOverlappedResult(pipe->handle, &pipe->overlap,
				&bytes, true);
		if (!success || !bytes) {
			break;
		}

		append_bytes(pipe, buf, (size_t)bytes);

		if (success) {
			pipe->read_callback(pipe->param, pipe->read_data,
					pipe->size);
			pipe->size = 0;
		}
	}

	close_callback(pipe);
	return 0;
}

static inline bool start_server_thread(win_pipe_server_t *pipe)
{
	pipe->thread = CreateThread(NULL, 0, server_thread, pipe, 0, NULL);
	return pipe->thread != NULL;
}

static inline bool wait_for_connection(win_pipe_server_t *pipe)
{
	bool success;

	pipe->overlap.hEvent = pipe->ready_event;
	success = !!ConnectNamedPipe(pipe->handle, &pipe->overlap);
	return success || (!success && io_pending());
}

bool win_pipe_server_start(win_pipe_server_t *pipe, const char *name,
		win_pipe_read_t read_callback, win_pipe_close_t close_callback,
		void *param)
{
	pipe->read_callback  = read_callback;
	pipe->close_callback = close_callback;
	pipe->param          = param;

	if (!create_events(pipe)) {
		goto error;
	}
	if (!create_pipe(pipe, name)) {
		goto error;
	}
	if (!wait_for_connection(pipe)) {
		goto error;
	}
	if (!start_server_thread(pipe)) {
		goto error;
	}

	return true;

error:
	win_pipe_server_free(pipe);
	return false;
}

static inline bool open_pipe(win_pipe_client_t *pipe, const char *name)
{
	DWORD mode = PIPE_READMODE_MESSAGE;

	pipe->handle = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			OPEN_EXISTING, 0, NULL);
	if (pipe->handle == INVALID_HANDLE_VALUE) {
		return false;
	}

	return !!SetNamedPipeHandleState(pipe->handle, &mode, NULL, NULL);
}

bool win_pipe_client_open(win_pipe_client_t *pipe, const char *name)
{
	if (!open_pipe(pipe, name)) {
		win_pipe_client_free(pipe);
		return false;
	}

	return true;
}

bool win_pipe_client_write(win_pipe_client_t *pipe, const void *data,
		size_t size)
{
	DWORD bytes;

	if (!pipe) {
		return false;
	}

	if (!pipe->handle || pipe->handle == INVALID_HANDLE_VALUE) {
		return false;
	}

	return !!WriteFile(pipe->handle, data, (DWORD)size, &bytes, NULL);
}
