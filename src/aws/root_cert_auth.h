#pragma once

#include <stddef.h>

/*
 * Amazon Root CA 1 as a NUL-terminated PEM string (device_config/
 * root_cert_auth.c). root_cert_auth_pem_len includes the NUL, which is the
 * length mbedTLS wants for a PEM. Used for the AWS IoT MQTT session and the
 * HTTPS OTA download.
 */
extern const char   root_cert_auth_pem[];
extern const size_t root_cert_auth_pem_len;
