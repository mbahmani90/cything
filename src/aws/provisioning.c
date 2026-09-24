#include "provisioning.h"

#include <stdio.h>
#include <string.h>
#include "lwip/sockets.h"
#include "nvs.h"

#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/version.h"

/* mbedTLS 4.x (ESP-IDF >= 6.0) vs 3.x (ESP-IDF 5.x) — the Arduino/PlatformIO
 * builds (doc/cy_thing_lib.md) are on 5.5, idf.py on 6.0. Differences:
 *   - 4.x removed the f_rng/p_rng arguments from mbedtls_pk_parse_key(),
 *     mbedtls_pk_sign() and mbedtls_x509write_csr_der() (RNG comes from PSA);
 *   - 4.x removed "transparent" PK contexts (mbedtls_pk_setup()/mbedtls_pk_ec(),
 *     see tf-psa-crypto 4.0-migration-guide/pk.md), so key generation goes
 *     through psa_generate_key() + mbedtls_pk_wrap_psa() instead of
 *     mbedtls_ecp_gen_key().
 * The PROV_* macros and prov_generate_key_pem() hide the difference. */
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#define PROV_PK_PARSE_KEY(pk, buf, len)         mbedtls_pk_parse_key(pk, buf, len, NULL, 0)
#define PROV_CSR_WRITE_DER(req, buf, size)      mbedtls_x509write_csr_der(req, buf, size)
#define PROV_PK_SIGN(pk, hash, hlen, sig, ssize, slen) \
        mbedtls_pk_sign(pk, MBEDTLS_MD_SHA256, hash, hlen, sig, ssize, slen)
#else
#include "mbedtls/ecp.h"
#include "esp_random.h"
/* mbedTLS 3.x wants an RNG callback; the ESP hardware RNG is fine once the
 * RF subsystem is up, which is always the case by the time a TCP client can
 * reach us. */
static int prov_rng(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}
#define PROV_PK_PARSE_KEY(pk, buf, len)         mbedtls_pk_parse_key(pk, buf, len, NULL, 0, prov_rng, NULL)
#define PROV_CSR_WRITE_DER(req, buf, size)      mbedtls_x509write_csr_der(req, buf, size, prov_rng, NULL)
#define PROV_PK_SIGN(pk, hash, hlen, sig, ssize, slen) \
        mbedtls_pk_sign(pk, MBEDTLS_MD_SHA256, hash, hlen, sig, ssize, slen, prov_rng, NULL)
#endif

#include "device_config/device_config.h"
#include "aws/claim_credentials.h"
#include "common/cy_log.h"
#include "common/reset_handler.h"

#define PROV_NVS_NAMESPACE   "prov"
#define PROV_NVS_KEY_DEVID   "devid"
#define PROV_NVS_KEY_SRCTERMID  "srctermid"
#define PROV_NVS_KEY_EP      "ep"
#define PROV_NVS_KEY_CERT    "cert"
#define PROV_NVS_KEY_OPKEY   "opkey"   /* the operational private key — generated once, kept across retries */

/* CSR DER is tiny (~100-150 bytes for a P-256 key); this is generous headroom. */
#define CSR_DER_BUF_LEN      256
/* Reassembly buffer for the incoming base64-encoded certificate PEM, sent in
 * CERT:<seq>:<total>:<b64 chunk> lines. Sized for a base64'd ~1.9 KB PEM
 * (base64 is ~4/3 the size of its input) with headroom. */
#define CERT_B64_BUF_LEN     2732

char g_prov_device_id[PROV_DEVICE_ID_MAX]     = {0};
char g_prov_source_terminal_id[PROV_SOURCE_TERMINAL_ID_MAX]   = {0};
char g_prov_iot_endpoint[PROV_ENDPOINT_MAX]   = {0};
char g_prov_cert_pem[PROV_CERT_PEM_MAX]       = {0};
char g_prov_key_pem[PROV_KEY_PEM_MAX]         = {0};

static bool s_active = false;

typedef enum {
    HS_IDLE = 0,
    HS_GOT_PROV,        /* have pending sourceTerminalId/iotEndpoint, waiting for CSRREQ */
    HS_SENT_CSR,        /* CSR sent, waiting for DEVID: */
    HS_RECEIVING_CERT,  /* deviceId known, reassembling CERT: chunks */
} handshake_state_t;

static handshake_state_t s_state = HS_IDLE;
static char s_pending_source_terminal_id[PROV_SOURCE_TERMINAL_ID_MAX];
static char s_pending_iot_endpoint[PROV_ENDPOINT_MAX];
static char s_pending_device_id[PROV_DEVICE_ID_MAX];
static char s_cert_b64[CERT_B64_BUF_LEN];
static size_t s_cert_b64_len = 0;
static int s_cert_total_chunks = 0;
static int s_cert_next_seq = 1; /* 1-based, matching the doc's <seq>:<total> */

/* ------------------------------------------------------------- NVS I/O --- */

static esp_err_t nvs_get_string(const char *key, char *out, size_t out_size) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = out_size;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_set_string(const char *key, const char *value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Strip trailing CR/LF/space in place. */
static void trim_trailing_ws(char *s) {
    for (size_t n = strlen(s);
         n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ');
         n--) {
        s[n - 1] = '\0';
    }
}

void provisioning_init(void) {
    bool have_devid  = nvs_get_string(PROV_NVS_KEY_DEVID,  g_prov_device_id,   sizeof(g_prov_device_id))   == ESP_OK;
    bool have_srctermid = nvs_get_string(PROV_NVS_KEY_SRCTERMID, g_prov_source_terminal_id,  sizeof(g_prov_source_terminal_id))  == ESP_OK;
    bool have_ep     = nvs_get_string(PROV_NVS_KEY_EP,     g_prov_iot_endpoint,sizeof(g_prov_iot_endpoint))== ESP_OK;
    bool have_cert   = nvs_get_string(PROV_NVS_KEY_CERT,   g_prov_cert_pem,    sizeof(g_prov_cert_pem))    == ESP_OK;
    bool have_key    = nvs_get_string(PROV_NVS_KEY_OPKEY,  g_prov_key_pem,     sizeof(g_prov_key_pem))     == ESP_OK;

    /* Heal identifiers persisted by older firmware that stored the TCP line's
     * trailing "\n" (endpoint -> getaddrinfo() 202; deviceId -> client-id
     * mismatch). Not the PEMs — a trailing newline there is legitimate. */
    trim_trailing_ws(g_prov_device_id);
    trim_trailing_ws(g_prov_source_terminal_id);
    trim_trailing_ws(g_prov_iot_endpoint);

    s_active = have_devid && have_srctermid && have_ep && have_cert && have_key;

    CY_LOGI(PROVISIONING_DB, "provisioning_init: active=%d", s_active);
}

bool provisioning_is_active(void) {
    return s_active;
}

void provisioning_get_scan_fields(char *source_terminal_id_out, size_t source_terminal_id_out_size,
                                   char *device_id_out, size_t device_id_out_size,
                                   char *provision_state_out, size_t provision_state_out_size) {
    if (s_active) {
        snprintf(source_terminal_id_out, source_terminal_id_out_size, "%s", g_prov_source_terminal_id);
        snprintf(device_id_out, device_id_out_size, "%s", g_prov_device_id);
        snprintf(provision_state_out, provision_state_out_size, "claimed");
    } else {
        source_terminal_id_out[0] = '\0';
        device_id_out[0] = '\0';
        snprintf(provision_state_out, provision_state_out_size, "unprovisioned");
    }
}

/* --------------------------------------------------------- key + CSR ---- */

/* Generates a fresh EC P-256 keypair and writes it as PEM into `pem`.
 * Returns 0 on success, a negative mbedTLS/PSA error otherwise. */
#if MBEDTLS_VERSION_MAJOR >= 4
static int prov_generate_key_pem(char *pem, size_t pem_size) {
    psa_status_t psa_ret = psa_crypto_init();
    if (psa_ret != PSA_SUCCESS) {
        CY_LOGE(PROVISIONING_DB, "provisioning: psa_crypto_init failed: %ld", (long)psa_ret);
        return -1;
    }

    /* Volatile PSA key, wrapped only for the PEM write, then destroyed. */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id;
    psa_ret = psa_generate_key(&attr, &key_id);
    if (psa_ret != PSA_SUCCESS) {
        CY_LOGE(PROVISIONING_DB, "provisioning: psa_generate_key failed: %ld", (long)psa_ret);
        return -1;
    }

    mbedtls_pk_context tmp_pk;
    mbedtls_pk_init(&tmp_pk);
    int ret = mbedtls_pk_wrap_psa(&tmp_pk, key_id);
    if (ret == 0) {
        ret = mbedtls_pk_write_key_pem(&tmp_pk, (unsigned char *)pem, pem_size);
    }
    mbedtls_pk_free(&tmp_pk);
    psa_destroy_key(key_id);
    return ret;
}
#else
static int prov_generate_key_pem(char *pem, size_t pem_size) {
    mbedtls_pk_context tmp_pk;
    mbedtls_pk_init(&tmp_pk);
    int ret = mbedtls_pk_setup(&tmp_pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret == 0) {
        ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(tmp_pk), prov_rng, NULL);
    }
    if (ret == 0) {
        ret = mbedtls_pk_write_key_pem(&tmp_pk, (unsigned char *)pem, pem_size);
    }
    mbedtls_pk_free(&tmp_pk);
    if (ret != 0) {
        CY_LOGE(PROVISIONING_DB, "provisioning: EC key generation failed: -0x%04x", -ret);
    }
    return ret;
}
#endif

/* Loads the operational keypair from NVS into `pk`, generating (and
 * persisting) a fresh EC P-256 one on first use. The private key is written
 * to NVS immediately, before any handshake completes, so a reboot mid-
 * handshake reuses the same key on retry rather than minting a new one every
 * attempt (matches provisionDevice's own requestId idempotency on the
 * backend side). Returns 0 on success. */
static int load_or_generate_keypair(mbedtls_pk_context *pk) {
    mbedtls_pk_init(pk);

    if (nvs_get_string(PROV_NVS_KEY_OPKEY, g_prov_key_pem, sizeof(g_prov_key_pem)) == ESP_OK) {
        int ret = PROV_PK_PARSE_KEY(pk, (const unsigned char *)g_prov_key_pem,
                                    strlen(g_prov_key_pem) + 1);
        if (ret == 0) return 0;
        /* Fall through and regenerate if the stored key is somehow unparseable. */
        CY_LOGE(PROVISIONING_DB, "provisioning: stored key unparseable (%d), regenerating", ret);
    }

    /* Generate into a temporary context just long enough to serialize it to
     * PEM, then re-parse the PEM into a self-owned context — the same shape
     * as the reload-from-NVS path above, so mbedtls_pk_free(pk) alone is
     * enough to clean up either way. */
    int ret = prov_generate_key_pem(g_prov_key_pem, sizeof(g_prov_key_pem));
    if (ret != 0) return ret;

    ret = PROV_PK_PARSE_KEY(pk, (const unsigned char *)g_prov_key_pem, strlen(g_prov_key_pem) + 1);
    if (ret != 0) return ret;

    esp_err_t nvs_err = nvs_set_string(PROV_NVS_KEY_OPKEY, g_prov_key_pem);
    if (nvs_err != ESP_OK) {
        CY_LOGE(PROVISIONING_DB, "provisioning: failed to persist operational key: %d", nvs_err);
        return -1;
    }

    return 0;
}

/* Builds a PKCS#10 CSR (DER) for the on-chip keypair and base64-encodes it
 * into `b64_out`. CN is a placeholder — AWS IoT binds identity by the
 * cert<->Thing attachment, not by CN (see doc/device-pairing.md). */
static int build_csr_base64(char *b64_out, size_t b64_out_size, size_t *b64_out_len) {
    mbedtls_pk_context pk;
    int ret = load_or_generate_keypair(&pk);
    if (ret != 0) return ret;

    mbedtls_x509write_csr req;
    mbedtls_x509write_csr_init(&req);
    mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&req, &pk);

    ret = mbedtls_x509write_csr_set_subject_name(&req, "CN=cyl-device");
    if (ret != 0) goto done;

    unsigned char der_buf[CSR_DER_BUF_LEN];
    /* Written at the END of the buffer — the return value is the length; the
     * CSR bytes start at der_buf + sizeof(der_buf) - der_len. */
    int der_len = PROV_CSR_WRITE_DER(&req, der_buf, sizeof(der_buf));
    if (der_len < 0) {
        ret = der_len;
        goto done;
    }

    ret = mbedtls_base64_encode((unsigned char *)b64_out, b64_out_size, b64_out_len,
                                 der_buf + sizeof(der_buf) - der_len, (size_t)der_len);

done:
    mbedtls_x509write_csr_free(&req);
    mbedtls_pk_free(&pk);
    return ret;
}

/* ------------------------------------------------------ line handling --- */

/* Sends "<text>\n". Short replies (ACK, CERTOK, ...) go out as one segment
 * from a small stack buffer; the long CLAIM:/CSR: lines are sent as text then
 * "\n" so no 3 KB copy is needed. */
static void send_line(int sock, const char *text) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s\n", text);
    if (n < (int)sizeof(buf)) {
        send(sock, buf, n, 0);
    } else {
        send(sock, text, strlen(text), 0);
        send(sock, "\n", 1, 0);
    }
}

static void reset_handshake(void) {
    s_state = HS_IDLE;
    s_pending_source_terminal_id[0] = '\0';
    s_pending_iot_endpoint[0] = '\0';
    s_pending_device_id[0] = '\0';
    s_cert_b64_len = 0;
    s_cert_total_chunks = 0;
    s_cert_next_seq = 1;
}

bool provisioning_handle_tcp_line(char *line, int line_len, int sock) {
    (void)line_len;

    /* Only touch lines that are ours. `line` aliases the shared TCP rx buffer,
     * and the pairing parser in tcp_server/pairing.c keys off a literal "\n" suffix
     * (SET_ROUTER_SSID_SUFFIX etc.), so stripping the newline in place would
     * break "ssid:"/"pass:" capture for every non-provisioning line. */
    if (memcmp(line, "REQID:", 6) != 0 && memcmp(line, "PROV:", 5) != 0 &&
        memcmp(line, "CSRREQ", 6) != 0 && memcmp(line, "DEVID:", 6) != 0 &&
        memcmp(line, "CERT:", 5) != 0 && memcmp(line, "PFIN", 4) != 0) {
        return false;
    }

    /* The TCP framing leaves the trailing "\n" (and any "\r") inside the
     * NUL-terminated buffer — tcp_dispatch_line only trims the reported length,
     * not the string. Strip it now so it can never end up baked into a
     * persisted field: the IoT endpoint hostname (-> getaddrinfo fails), the
     * deviceId (-> MQTT client id != Thing name -> policy denies), or a cert
     * base64 chunk. */
    trim_trailing_ws(line);

    if (memcmp(line, "REQID:", 6) == 0) {
        /* slice CC — prove this is a genuine unit of the model: reply
         * CLAIM:<b64 claim-cert PEM>,<b64 RSA-SHA256(requestId)>. No claim
         * material flashed (claim_cert_pem/claim_key_pem empty) -> just ACK; the
         * app degrades to a null attestation, accepted while
         * CLAIM_VERIFY_ENFORCE is off. */
        const char *request_id = line + 6;
        if (!claim_credentials_present()) {
            send_line(sock, "ACK");
            return true;
        }

        unsigned char hash[32];
        int ret = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                             (const unsigned char *)request_id, strlen(request_id), hash);
        mbedtls_pk_context ck;
        mbedtls_pk_init(&ck);
        if (ret == 0) {
            ret = PROV_PK_PARSE_KEY(&ck, (const unsigned char *)claim_key_pem,
                                    strlen(claim_key_pem) + 1);
        }
        unsigned char sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
        size_t sig_len = 0;
        if (ret == 0) {
            ret = PROV_PK_SIGN(&ck, hash, sizeof(hash), sig, sizeof(sig), &sig_len);
        }
        mbedtls_pk_free(&ck);
        if (ret != 0) {
            CY_LOGE(PROVISIONING_DB, "provisioning: claim sign failed: %d", ret);
            send_line(sock, "ACK"); /* app times out the CLAIM wait and proceeds */
            return true;
        }

        /* static: the line handler is single-threaded per TCP connection, and
         * these are large enough to be worth keeping off the task stack. */
        static char claim_resp[2800];
        char sig_b64[512];
        size_t sig_b64_len = 0, cert_b64_len = 0;
        mbedtls_base64_encode((unsigned char *)sig_b64, sizeof(sig_b64), &sig_b64_len, sig, sig_len);
        if (mbedtls_base64_encode((unsigned char *)claim_resp, sizeof(claim_resp), &cert_b64_len,
                                  (const unsigned char *)claim_cert_pem, strlen(claim_cert_pem)) != 0) {
            send_line(sock, "ACK");
            return true;
        }
        claim_resp[cert_b64_len] = '\0';

        /* prepend "CLAIM:" and append ",<sig>" around the cert base64 in place. */
        static char claim_line[2900];
        snprintf(claim_line, sizeof(claim_line), "CLAIM:%s,%.*s",
                 claim_resp, (int)sig_b64_len, sig_b64);
        send_line(sock, claim_line);
        return true;
    }

    if (memcmp(line, "PROV:", 5) == 0) {
        /* PROV:<sourceTerminalId>,<deviceType>,<iotEndpoint> — deviceType is
         * ignored, this device already knows its own (DEVICE_TYPE, device_config.h);
         * the phone sends it back mainly for the doc'd wire-format symmetry.
         * sourceTerminalId is topic segment 1 (the root channel id). */
        char *rest = line + 5;
        char *comma1 = strchr(rest, ',');
        if (!comma1) { send_line(sock, "ACK"); return true; }
        char *comma2 = strchr(comma1 + 1, ',');
        if (!comma2) { send_line(sock, "ACK"); return true; }

        size_t chan_len = (size_t)(comma1 - rest);
        if (chan_len >= sizeof(s_pending_source_terminal_id)) chan_len = sizeof(s_pending_source_terminal_id) - 1;
        memcpy(s_pending_source_terminal_id, rest, chan_len);
        s_pending_source_terminal_id[chan_len] = '\0';

        snprintf(s_pending_iot_endpoint, sizeof(s_pending_iot_endpoint), "%s", comma2 + 1);

        s_state = HS_GOT_PROV;
        send_line(sock, "ACK");
        return true;
    }

    if (memcmp(line, "CSRREQ", 6) == 0) {
        char b64[400];
        size_t b64_len = 0;
        int ret = build_csr_base64(b64, sizeof(b64), &b64_len);
        if (ret != 0) {
            CY_LOGE(PROVISIONING_DB, "provisioning: CSR build failed: %d", ret);
            send_line(sock, "ACK"); /* nothing better to say in-protocol; app will time out and retry */
            return true;
        }
        b64[b64_len] = '\0';

        char resp[420];
        snprintf(resp, sizeof(resp), "CSR:%s", b64);
        send_line(sock, resp);
        s_state = HS_SENT_CSR;
        return true;
    }

    if (memcmp(line, "DEVID:", 6) == 0) {
        snprintf(s_pending_device_id, sizeof(s_pending_device_id), "%s", line + 6);
        s_state = HS_RECEIVING_CERT;
        s_cert_b64_len = 0;
        s_cert_total_chunks = 0;
        s_cert_next_seq = 1;
        send_line(sock, "ACK");
        return true;
    }

    if (memcmp(line, "CERT:", 5) == 0) {
        /* CERT:<seq>:<total>:<b64 chunk> */
        char *p = line + 5;
        int seq = atoi(p);
        char *c1 = strchr(p, ':');
        if (!c1) { send_line(sock, "ACK"); return true; }
        int total = atoi(c1 + 1);
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) { send_line(sock, "ACK"); return true; }
        const char *chunk = c2 + 1;
        size_t chunk_len = strlen(chunk);

        if (s_state != HS_RECEIVING_CERT || seq != s_cert_next_seq ||
            (s_cert_total_chunks != 0 && total != s_cert_total_chunks)) {
            CY_LOGE(PROVISIONING_DB, "provisioning: out-of-order CERT chunk (seq=%d expected=%d)", seq, s_cert_next_seq);
            send_line(sock, "ACK"); /* ignore; the app is expected to retry PFIN-less on timeout */
            return true;
        }
        s_cert_total_chunks = total;

        if (s_cert_b64_len + chunk_len >= sizeof(s_cert_b64)) {
            CY_LOGE(PROVISIONING_DB, "provisioning: CERT reassembly buffer overflow");
            reset_handshake();
            send_line(sock, "ACK");
            return true;
        }
        memcpy(s_cert_b64 + s_cert_b64_len, chunk, chunk_len);
        s_cert_b64_len += chunk_len;
        s_cert_next_seq++;

        if (seq == total) {
            /* Last chunk: decode the full reassembled base64 text back into
             * the PEM it originally was (see doc/device-pairing.md — PEM/DER
             * payloads are base64'd for safe line-delimited transport). */
            size_t pem_len = 0;
            int ret = mbedtls_base64_decode((unsigned char *)g_prov_cert_pem, sizeof(g_prov_cert_pem) - 1,
                                             &pem_len, (const unsigned char *)s_cert_b64, s_cert_b64_len);
            if (ret != 0) {
                CY_LOGE(PROVISIONING_DB, "provisioning: CERT base64 decode failed: %d", ret);
                reset_handshake();
                send_line(sock, "ACK");
                return true;
            }
            g_prov_cert_pem[pem_len] = '\0';
            send_line(sock, "CERTOK");
        } else {
            send_line(sock, "ACK");
        }
        return true;
    }

    if (memcmp(line, "PFIN", 4) == 0) {
        if (s_state != HS_RECEIVING_CERT || strlen(g_prov_cert_pem) == 0 ||
            strlen(s_pending_device_id) == 0) {
            /* Re-pair with only PROV:+PFIN (already-provisioned device moving to
             * a channel in a DIFFERENT channel-family) — cert/device id are
             * untouched, only sourceTerminalId changes. Same-family moves need
             * nothing. See doc/device-pairing.md "Re-pairing". */
            if (s_active && s_pending_source_terminal_id[0] != '\0') {
                snprintf(g_prov_source_terminal_id, sizeof(g_prov_source_terminal_id), "%s", s_pending_source_terminal_id);
                nvs_set_string(PROV_NVS_KEY_SRCTERMID, g_prov_source_terminal_id);
                send_line(sock, "ACK");
                reset_handshake();
                return true;
            }
            send_line(sock, "ACK");
            reset_handshake();
            return true;
        }

        snprintf(g_prov_device_id, sizeof(g_prov_device_id), "%s", s_pending_device_id);
        snprintf(g_prov_source_terminal_id, sizeof(g_prov_source_terminal_id), "%s", s_pending_source_terminal_id);
        snprintf(g_prov_iot_endpoint, sizeof(g_prov_iot_endpoint), "%s", s_pending_iot_endpoint);

        bool ok = nvs_set_string(PROV_NVS_KEY_DEVID, g_prov_device_id) == ESP_OK &&
                  nvs_set_string(PROV_NVS_KEY_SRCTERMID, g_prov_source_terminal_id) == ESP_OK &&
                  nvs_set_string(PROV_NVS_KEY_EP, g_prov_iot_endpoint) == ESP_OK &&
                  nvs_set_string(PROV_NVS_KEY_CERT, g_prov_cert_pem) == ESP_OK;

        if (!ok) {
            CY_LOGE(PROVISIONING_DB, "provisioning: failed to persist new identity");
            reset_handshake();
            send_line(sock, "ACK");
            return true;
        }

        send_line(sock, "ACK");
        reset_handshake();
        /* PFIN: everything is persisted; reboot into the new identity. A
         * reboot (not a live reconnect) mirrors how Wi-Fi pairing's FinishP
         * applies its changes — the cleanest way to guarantee every task
         * picks up the new identity consistently. 1 s for the ACK to leave. */
        reboot_after_ms(1000);
        return true;
    }

    return false;
}
