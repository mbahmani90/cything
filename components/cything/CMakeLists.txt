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
