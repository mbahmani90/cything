# The CyThing library as an ESP-IDF component: all of ../../src, wrapped so
# any idf.py project can add this directory (EXTRA_COMPONENT_DIRS, a
# submodule under its own components/) and get the same build the top-level
# `main` component pulls in for this repo's own firmware. See
# doc/cy_thing_lib.md §4/§5.
set(CYTHING_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")

set(CYTHING_SRCS
	"${CYTHING_SRC}/cything.c"

	"${CYTHING_SRC}/wifi/station_mode.c"
	"${CYTHING_SRC}/wifi/access_point.c"
	"${CYTHING_SRC}/wifi/wifi_info_handler.c"
	"${CYTHING_SRC}/wifi/wifi_trial.c"
	"${CYTHING_SRC}/ble/ble_host.c"
	"${CYTHING_SRC}/ble/ble_pairing.c"
	"${CYTHING_SRC}/ble/ble_beacon.c"
	"${CYTHING_SRC}/ble/discovery_mode.c"
	"${CYTHING_SRC}/tcp_server/tcp_server.c"
	"${CYTHING_SRC}/tcp_server/tcp_client_recv.c"
	"${CYTHING_SRC}/tcp_server/tcp_command.c"
	"${CYTHING_SRC}/tcp_server/pairing.c"
	"${CYTHING_SRC}/tcp_server/tcp_client_list.c"
	"${CYTHING_SRC}/tcp_server/tcp_response_fifo.c"
	"${CYTHING_SRC}/security/local_session.c"
	"${CYTHING_SRC}/security/device_password.c"
	"${CYTHING_SRC}/security/paired_list.c"
	"${CYTHING_SRC}/security/pake_handler.c"
	"${CYTHING_SRC}/security/local_auth.c"
	"${CYTHING_SRC}/security/enc_frame.c"
	"${CYTHING_SRC}/security/access_policy.c"
	"${CYTHING_SRC}/security/owner_commands.c"
	"${CYTHING_SRC}/security/pairing_events.c"
	"${CYTHING_SRC}/udp_socket/udp_server.c"
	"${CYTHING_SRC}/udp_socket/udp_response_handler.c"
	"${CYTHING_SRC}/ota_lib/ota_handler.c"
	"${CYTHING_SRC}/ota_lib/flash_boot_handler.c"
	"${CYTHING_SRC}/ota_lib/http_ota_handler.c"
	"${CYTHING_SRC}/memory_handler/flash_wifi_info_handler.c"
	"${CYTHING_SRC}/memory_handler/flash_reset_handler.c"
	"${CYTHING_SRC}/common/reset_handler.c"
	"${CYTHING_SRC}/common/response_fifo.c"
	"${CYTHING_SRC}/aws/provisioning.c"
	"${CYTHING_SRC}/aws/aws_iot_handler.c"
	"${CYTHING_SRC}/aws/mqtt_response_fifo.c"
	"${CYTHING_SRC}/device_config/root_cert_auth.c"
	"${CYTHING_SRC}/device_config/claim_credentials.c"
	"${CYTHING_SRC}/device_config/cy_config.c"

	# vendored: coreMQTT (+ its ESP-TLS transport port), backoffAlgorithm,
	# and the POSIX clock shim they expect
	"${CYTHING_SRC}/coreMQTT/core_mqtt.c"
	"${CYTHING_SRC}/coreMQTT/core_mqtt_state.c"
	"${CYTHING_SRC}/coreMQTT/core_mqtt_serializer.c"
	"${CYTHING_SRC}/coreMQTT/network_transport.c"
	"${CYTHING_SRC}/backoffAlgorithm/backoff_algorithm.c"
	"${CYTHING_SRC}/posix_compat/clock_esp.c"
	)

# Per-model claim certificate + key (optional). Looked for in the project's
# cert/ directory, or wherever CYTHING_CLAIM_DIR points (a CMake -D or an
# environment variable; relative paths are from the project directory), so
# the real keys can live outside the repo. Both claim_cert.pem.h and
# claim_key.pem.h must hold a real PEM; the shipped templates' "..."
# placeholder counts as missing. Without them the empty weak defaults in
# claim_credentials.c apply and the claim step is skipped.
idf_build_get_property(cything_project_dir PROJECT_DIR)
if(DEFINED CYTHING_CLAIM_DIR)
	set(cything_claim_dir "${CYTHING_CLAIM_DIR}")
elseif(DEFINED ENV{CYTHING_CLAIM_DIR})
	set(cything_claim_dir "$ENV{CYTHING_CLAIM_DIR}")
else()
	set(cything_claim_dir "${cything_project_dir}/cert")
endif()
get_filename_component(cything_claim_dir "${cything_claim_dir}" ABSOLUTE
	BASE_DIR "${cything_project_dir}")
set(cything_claim_cert "${cything_claim_dir}/claim_cert.pem.h")
set(cything_claim_key  "${cything_claim_dir}/claim_key.pem.h")

function(cything_pem_filled file out)
	set(filled FALSE)
	if(EXISTS "${file}")
		file(READ "${file}" pem)
		if(pem MATCHES "-----BEGIN" AND NOT pem MATCHES "\n[.][.][.]\r?\n")
			set(filled TRUE)
		endif()
	endif()
	set(${out} ${filled} PARENT_SCOPE)
endfunction()
cything_pem_filled("${cything_claim_cert}" cything_cert_filled)
cything_pem_filled("${cything_claim_key}"  cything_key_filled)
set(cything_use_claim_dir FALSE)
if(cything_cert_filled AND cything_key_filled)
	set(cything_use_claim_dir TRUE)
	list(APPEND CYTHING_SRCS "${CMAKE_CURRENT_LIST_DIR}/claim_credentials_cert_dir.cpp")
endif()

# WHOLE_ARCHIVE: the weak default hooks/config (tcp_command.c,
# aws_iot_handler.c, access_policy.c, claim_credentials.c, cy_config.c) live
# in this component's archive; the application's strong overrides live in
# main. A linker only extracts an archive member when it resolves a
# currently-undefined symbol, and a weak definition counts — so without this,
# whichever side the linker reaches first can silently win. Force every
# object in, then the strong definition always wins, as it does in a direct
# link. See doc/cy_thing_lib.md §4 "Weak symbols vs static archives".
idf_component_register(SRCS ${CYTHING_SRCS}
					   INCLUDE_DIRS "${CYTHING_SRC}"
					   REQUIRES esp-tls esp_timer esp_wifi esp_netif nvs_flash esp_https_ota app_update
					            esp_partition spi_flash mbedtls esp_driver_gpio lwip bt protocomm
					   WHOLE_ARCHIVE
					  )

set_source_files_properties("${CYTHING_SRC}/coreMQTT/core_mqtt.c"
	PROPERTIES COMPILE_FLAGS -Wno-format)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
	if(cything_use_claim_dir)
		message(STATUS "CyThing: claim certificate + key from ${cything_claim_dir}")
		set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/claim_credentials_cert_dir.cpp"
			PROPERTIES COMPILE_DEFINITIONS
			"CYTHING_CLAIM_CERT_FILE=\"${cything_claim_cert}\";CYTHING_CLAIM_KEY_FILE=\"${cything_claim_key}\"")
	elseif(cything_cert_filled OR cything_key_filled)
		message(WARNING "CyThing: only one of claim_cert.pem.h / claim_key.pem.h "
			"in ${cything_claim_dir} holds a PEM; the claim step is skipped")
	else()
		message(STATUS "CyThing: no claim certificate in ${cything_claim_dir}; claim step skipped")
	endif()
	# Re-run configure when the PEMs are filled in or edited, or when files are
	# added to the directory, so detection never needs a manual reconfigure.
	foreach(f "${cything_claim_dir}" "${cything_claim_cert}" "${cything_claim_key}")
		if(EXISTS "${f}")
			set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
		endif()
	endforeach()
endif()
