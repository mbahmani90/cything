/*
 * ESP-IDF only: the per-model claim certificate + key from the project's
 * cert/ directory (or CYTHING_CLAIM_DIR). CMakeLists.txt compiles this file
 * only when both .pem.h files there hold a real PEM, and passes their paths
 * in. These strong definitions override the empty weak defaults in
 * src/device_config/claim_credentials.c.
 *
 * Kept out of src/ on purpose: Arduino and PlatformIO compile everything in
 * src/, and have their own claim_credentials.ino / .cpp.
 *
 * C++ because the .pem.h files are C++11 raw strings; CyThingEsp32.h gives
 * the two arrays C linkage.
 */
#include "CyThingEsp32.h"

const char claim_cert_pem[] =
#include CYTHING_CLAIM_CERT_FILE
;

const char claim_key_pem[] =
#include CYTHING_CLAIM_KEY_FILE
;
