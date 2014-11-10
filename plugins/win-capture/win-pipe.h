#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#else
#ifndef inline
#define inline __inline
#endif
#endif

typedef void (*win_pipe_read_t)(void *param, uint8_t *data, size_t size);
typedef void (*win_pipe_close_t)(void *param);

struct win_pipe_server {
	OVERLAPPED                 overlap;
	HANDLE                     handle;
	HANDLE                     ready_event;
	HANDLE                     thread;

	uint8_t                    *read_data;
	size_t                     size;
	size_t                     capacity;

	win_pipe_read_t            read_callback;
	win_pipe_close_t           close_callback;
	void                       *param;
};

struct win_pipe_client {
	HANDLE                     handle;
};

typedef struct win_pipe_server win_pipe_server_t;
typedef struct win_pipe_client win_pipe_client_t;

extern bool win_pipe_server_start(win_pipe_server_t *pipe, const char *name,
		win_pipe_read_t read_callback, win_pipe_close_t close_callback,
		void *param);
extern void win_pipe_server_free(win_pipe_server_t *pipe);

extern bool win_pipe_client_open(win_pipe_client_t *pipe, const char *name);
extern void win_pipe_client_free(win_pipe_client_t *pipe);
extern bool win_pipe_client_write(win_pipe_client_t *pipe, const void *data,
		size_t size);
static inline bool win_pipe_client_valid(win_pipe_client_t *pipe)
{
	return pipe->handle != NULL && pipe->handle != INVALID_HANDLE_VALUE;
}

#ifdef __cplusplus
}
#endif
