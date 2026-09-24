#pragma once

#include <stdbool.h>

/*
 * Per-model CLAIM certificate and key (slice CC — see the CypressTerminalKmp
 * doc/defined-device.md). The SAME cert + RSA key is flashed onto every
 * genuine unit of this device model; the model creator gets them from the
 * app's "Claim certificate" screen (one-time download).
 *
 * On `REQID:<requestId>` the device replies `CLAIM:<b64 cert>,<b64 sig>`
 * where sig = RSA-SHA256(requestId) — provisionDevice's verifyClaim checks it
 * before signing the CSR. Empty strings => the claim step is skipped (the app
 * degrades gracefully while CLAIM_VERIFY_ENFORCE is off).
 *
 * The library ships EMPTY weak defaults (device_config/claim_credentials.c).
 * To enable the claim step the application defines both as NUL-terminated PEM
 * strings. The intended form is a claim_credentials.cpp/.ino that #includes
 * two raw-string files, claim_cert.pem.h and claim_key.pem.h, into which the
 * PEMs are pasted verbatim — see examples/Basic/ and, for ESP-IDF, main/
 * claim_credentials.cpp.example. NEVER commit the real PEMs.
 */
extern const char claim_cert_pem[];
extern const char claim_key_pem[];

/* True when both strings contain a PEM block (not just non-empty). */
bool claim_credentials_present(void);
