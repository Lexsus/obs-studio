project(win-capture)

set(win-capture_HEADERS
	obfuscate.h
	win-pipe.h
	hook-helpers.h
	graphics-hook-info.h
	window-helpers.h
	dc-capture.h)

set(win-capture_SOURCES
	win-pipe.c
	dc-capture.c
	obfuscate.c
	window-helpers.c
	monitor-capture.c
	window-capture.c
	plugin-main.c)

add_library(win-capture MODULE
	${win-capture_SOURCES}
	${win-capture_HEADERS})
target_link_libraries(win-capture
	libobs
	psapi.lib)

install_obs_plugin_with_data(win-capture data)

add_subdirectory(graphics-hook)
add_subdirectory(get-graphics-offsets)
add_subdirectory(inject-helper)
