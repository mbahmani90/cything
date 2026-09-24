# The `main` component is only the ESP-IDF harness: the two command hooks
# plus app_main(){ cything_begin(); } — exactly what a sketch contains. The
# CyThing library itself is the `cything` component (../components/cything,
# which compiles ../src) — see doc/cy_thing_lib.md §4/§5.

# Per-model claim cert + key: optional, git-ignored, overrides the library's
# empty weak defaults (see claim_credentials.cpp.example).
set(CYTHING_APP_SRCS "app_main.c")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/claim_credentials.cpp")
	list(APPEND CYTHING_APP_SRCS "claim_credentials.cpp")
endif()
# Same idea for the generated channel/model config (see cything_config.cpp.example);
# absent, the weak defaults in device_config/cy_config.c apply.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/cything_config.cpp")
	list(APPEND CYTHING_APP_SRCS "cything_config.cpp")
endif()

# No WHOLE_ARCHIVE here: app_main.o is always pulled in (ESP-IDF's startup
# code calls app_main() unconditionally), so its strong hook definitions
# always reach the final link. The WHOLE_ARCHIVE side of the override lives
# on the `cything` component, which owns the weak defaults.
idf_component_register(SRCS ${CYTHING_APP_SRCS}
					   INCLUDE_DIRS "."
					   PRIV_REQUIRES cything
					  )
