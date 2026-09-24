/*
 * Per-model claim certificate + key (optional) — see README "Claim
 * certificate". Nothing to edit here: paste the PEMs into the two tabs
 * claim_cert.pem.h and claim_key.pem.h. Left empty, the claim step is
 * skipped. Keep the .pem.h files out of git.
 */
#include <CyThingEsp32.h>

const char claim_cert_pem[] =
#include "claim_cert.pem.h"
;

const char claim_key_pem[] =
#include "claim_key.pem.h"
;
