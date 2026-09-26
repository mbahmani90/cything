# The `main` component is only the ESP-IDF harness: the two command hooks
# plus app_main(){ cything_begin(); } — exactly what a sketch contains. The
# CyThing library itself is the `cything` component (../components/cything,
# which compiles ../src) — see doc/cy_thing_lib.md §4/§5.

set(CYTHING_APP_SRCS "app_main.c")
# Generated channel/model config (see cything_config.cpp.example): optional,
# overrides the weak defaults in device_config/cy_config.c. (The claim cert +
# key are not handled here: the cything component picks them up from ../cert/.)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/cything_config.cpp")
	list(APPEND CYTHING_APP_SRCS "cything_config.cpp")
endif()

# WHOLE_ARCHIVE: app_main.o is always pulled in (ESP-IDF's startup code calls
# app_main() unconditionally), but cything_config.o is not — nothing
# references a symbol only it defines, because the `cything` component's weak
# defaults already satisfy every reference. Without this the linker never
# extracts it from libmain.a and the defaults silently win.
idf_component_register(SRCS ${CYTHING_APP_SRCS}
					   INCLUDE_DIRS "."
					   PRIV_REQUIRES cything
					   WHOLE_ARCHIVE
					  )
