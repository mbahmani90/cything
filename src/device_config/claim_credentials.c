/*
 * Empty claim credentials — the default when the application does not
 * provide any (see aws/claim_credentials.h). Weak, so a strong definition of
 * the same two arrays in the application takes over at link time, exactly
 * like the command hooks.
 */
#include <string.h>
#include "aws/claim_credentials.h"

__attribute__((weak)) const char claim_cert_pem[] = "";
__attribute__((weak)) const char claim_key_pem[]  = "";

/* "Present" means both look like PEM, not merely non-empty: the .pem.h
 * raw-string templates hold a newline even when nothing was pasted in. */
bool claim_credentials_present(void){
    return strstr(claim_cert_pem, "-----BEGIN") != NULL &&
           strstr(claim_key_pem,  "-----BEGIN") != NULL;
}
