/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See tlscert.h. The decisions (dates, when to renew) are tlscert_core.c's;
 * this is mbedTLS and NVS around them.
 */
#include "tlscert.h"
#include "tlscert_core.h"
#include "netmgr.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/asn1.h"
#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/pem.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "tlscert";

#define NVS_NS   "tls"
#define ORG      "O=My UPS Pal"
#define CRT_MAX  1024           /* a P-256 certificate is about 500 bytes */
#define KEY_MAX  200            /* a P-256 private key, DER: 121 */

typedef struct {
    uint8_t *crt;
    size_t   crt_len;
    uint32_t not_after;
    char     names[96];         /* "host|ip" it was issued for */
} leaf_t;

static mbedtls_pk_context s_ca_key, s_key;
static char *s_ca_name;                 /* the CA's subject, the leaf's issuer */
static uint8_t *s_key_der;
static size_t s_key_len;
static char *s_ca_pem;
static char s_ca_sha[96];
static leaf_t *_Atomic s_leaf;
static SemaphoreHandle_t s_lock;        /* one issuer at a time */

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

/* Long-lived buffers go to PSRAM: nothing here is touched with the cache off. */
static void *alloc_spiram(size_t n)
{
    void *d = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return d ? d : malloc(n ? n : 1);
}

static void *dup_spiram(const void *p, size_t n)
{
    void *d = alloc_spiram(n);
    if (d && n) memcpy(d, p, n);
    return d;
}

static uint8_t *nvs_blob(nvs_handle_t h, const char *key, size_t *len)
{
    size_t n = 0;
    if (nvs_get_blob(h, key, NULL, &n) != ESP_OK || n == 0) return NULL;
    uint8_t *b = alloc_spiram(n);
    if (!b) return NULL;
    if (nvs_get_blob(h, key, b, &n) != ESP_OK) { free(b); return NULL; }
    *len = n;
    return b;
}

static int gen_key(mbedtls_pk_context *pk)
{
    int r = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (r == 0) r = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), rng, NULL);
    return r;
}

/* A private key as DER into NVS. */
static esp_err_t store_key(nvs_handle_t h, const char *name, mbedtls_pk_context *pk,
                           uint8_t **der_out, size_t *len_out)
{
    uint8_t *tmp = malloc(KEY_MAX);
    if (!tmp) return ESP_ERR_NO_MEM;
    int n = mbedtls_pk_write_key_der(pk, tmp, KEY_MAX);
    esp_err_t err = n > 0 ? nvs_set_blob(h, name, tmp + KEY_MAX - n, (size_t)n) : ESP_FAIL;
    if (err == ESP_OK && der_out) {
        *der_out = dup_spiram(tmp + KEY_MAX - n, (size_t)n);
        *len_out = (size_t)n;
        if (!*der_out) err = ESP_ERR_NO_MEM;
    }
    memset(tmp, 0, KEY_MAX);
    free(tmp);
    return err;
}

static bool parse_ip(const char *s, uint8_t out[4])
{
    unsigned a, b, c, d;
    char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255 || (a | b | c | d) == 0) return false;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return true;
}

/* Sign one certificate. The CA (ca) or the server's (host, ip). DER, on the
 * heap, into *der. */
static int issue(bool ca, const char *subject, mbedtls_pk_context *subject_key,
                 const char *not_before, const char *not_after,
                 const char *host, const char *ip, uint8_t **der, size_t *len)
{
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, subject_key);
    mbedtls_x509write_crt_set_issuer_key(&w, &s_ca_key);

    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] = (serial[0] & 0x7F) | 0x40;      /* positive, and not zero */

    int r = mbedtls_x509write_crt_set_subject_name(&w, subject);
    if (!r) r = mbedtls_x509write_crt_set_issuer_name(&w, s_ca_name);
    if (!r) r = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof(serial));
    if (!r) r = mbedtls_x509write_crt_set_validity(&w, not_before, not_after);
    if (!r) r = mbedtls_x509write_crt_set_basic_constraints(&w, ca ? 1 : 0, ca ? 0 : -1);
    if (!r) r = mbedtls_x509write_crt_set_subject_key_identifier(&w);
    if (!r) r = mbedtls_x509write_crt_set_authority_key_identifier(&w);
    if (!r) r = mbedtls_x509write_crt_set_key_usage(&w, ca
                    ? (MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN)
                    : MBEDTLS_X509_KU_DIGITAL_SIGNATURE);

    /* The server's: serverAuth, and every name it answers to in the
     * subjectAltName, which is all browsers look at (not the CN). */
    char local[72];
    uint8_t ip4[4];
    mbedtls_x509_san_list san[3];
    mbedtls_asn1_sequence eku = {
        .buf = { .tag = MBEDTLS_ASN1_OID, .len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH),
                 .p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH },
    };
    if (!ca && !r) {
        r = mbedtls_x509write_crt_set_ext_key_usage(&w, &eku);
        snprintf(local, sizeof(local), "%s.local", host);
        memset(san, 0, sizeof(san));
        san[0].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san[0].node.san.unstructured_name.p = (unsigned char *)local;
        san[0].node.san.unstructured_name.len = strlen(local);
        san[0].next = &san[1];
        san[1].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san[1].node.san.unstructured_name.p = (unsigned char *)host;
        san[1].node.san.unstructured_name.len = strlen(host);
        if (parse_ip(ip, ip4)) {
            san[1].next = &san[2];
            san[2].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
            san[2].node.san.unstructured_name.p = ip4;
            san[2].node.san.unstructured_name.len = 4;
        }
        if (!r) r = mbedtls_x509write_crt_set_subject_alternative_name(&w, san);
    }

    uint8_t *buf = r ? NULL : malloc(CRT_MAX);
    if (!r && !buf) r = MBEDTLS_ERR_X509_ALLOC_FAILED;
    if (!r) {
        int n = mbedtls_x509write_crt_der(&w, buf, CRT_MAX, rng, NULL);
        if (n > 0) {
            *der = dup_spiram(buf + CRT_MAX - n, (size_t)n);
            *len = (size_t)n;
            if (!*der) r = MBEDTLS_ERR_X509_ALLOC_FAILED;
        } else {
            r = n ? n : -1;
        }
    }
    free(buf);
    mbedtls_x509write_crt_free(&w);
    return r;
}

/* The CA's PEM and fingerprint, for the page. */
static void publish_ca(const uint8_t *der, size_t len)
{
    uint8_t sha[32];
    mbedtls_sha256(der, len, sha, 0);
    tlsc_hex_colons(sha, s_ca_sha);
    size_t cap = len * 4 / 3 + 128, olen = 0;
    char *pem = alloc_spiram(cap);
    if (pem && mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                                        "-----END CERTIFICATE-----\n",
                                        der, len, (unsigned char *)pem, cap, &olen) == 0) {
        s_ca_pem = pem;
    } else {
        free(pem);
    }
}

static esp_err_t load_or_make_ca(nvs_handle_t h, bool *made)
{
    size_t klen = 0, clen = 0;
    uint8_t *kder = nvs_blob(h, "ca_key", &klen);
    uint8_t *cder = nvs_blob(h, "ca_crt", &clen);
    *made = false;
    if (kder && cder &&
        mbedtls_pk_parse_key(&s_ca_key, kder, klen, NULL, 0, rng, NULL) == 0) {
        /* The issuer of every server certificate is this CA's subject as
         * stored, not one made from today's hostname: a renamed board keeps
         * the CA a person already trusts. */
        mbedtls_x509_crt c;
        mbedtls_x509_crt_init(&c);
        char dn[128];
        bool ok = mbedtls_x509_crt_parse_der(&c, cder, clen) == 0 &&
                  mbedtls_x509_dn_gets(dn, sizeof(dn), &c.subject) > 0;
        mbedtls_x509_crt_free(&c);
        memset(kder, 0, klen);
        free(kder);
        if (ok) {
            free(s_ca_name);
            s_ca_name = dup_spiram(dn, strlen(dn) + 1);
            publish_ca(cder, clen);
        }
        free(cder);
        if (ok && s_ca_name) return ESP_OK;
        kder = cder = NULL;
    }
    if (kder) { memset(kder, 0, klen); free(kder); }
    free(cder);
    mbedtls_pk_free(&s_ca_key);
    mbedtls_pk_init(&s_ca_key);

    ESP_LOGI(TAG, "making this board's CA (once)");
    int r = gen_key(&s_ca_key);
    uint8_t *der = NULL;
    size_t len = 0;
    if (!r) r = issue(true, s_ca_name, &s_ca_key, TLSC_CA_NOT_BEFORE, TLSC_CA_NOT_AFTER,
                      NULL, NULL, &der, &len);
    if (r) {
        ESP_LOGE(TAG, "could not make the CA: -0x%04x", (unsigned)-r);
        return ESP_FAIL;
    }
    esp_err_t err = store_key(h, "ca_key", &s_ca_key, NULL, NULL);
    if (err == ESP_OK) err = nvs_set_blob(h, "ca_crt", der, len);
    /* A server certificate from any earlier CA is no use with this one. */
    nvs_erase_key(h, "crt");
    if (err == ESP_OK) err = nvs_commit(h);
    publish_ca(der, len);
    free(der);
    *made = true;
    return err;
}

static esp_err_t load_or_make_key(nvs_handle_t h)
{
    size_t len = 0;
    uint8_t *der = nvs_blob(h, "key", &len);
    if (der && mbedtls_pk_parse_key(&s_key, der, len, NULL, 0, rng, NULL) == 0) {
        s_key_der = der;
        s_key_len = len;
        return ESP_OK;
    }
    if (der) { memset(der, 0, len); free(der); }
    mbedtls_pk_free(&s_key);
    mbedtls_pk_init(&s_key);
    int r = gen_key(&s_key);
    if (r) {
        ESP_LOGE(TAG, "could not make the server key: -0x%04x", (unsigned)-r);
        return ESP_FAIL;
    }
    nvs_erase_key(h, "crt");
    esp_err_t err = store_key(h, "key", &s_key, &s_key_der, &s_key_len);
    if (err == ESP_OK) err = nvs_commit(h);
    return err;
}

static void load_leaf(nvs_handle_t h)
{
    leaf_t *l = alloc_spiram(sizeof(*l));
    if (!l) return;
    memset(l, 0, sizeof(*l));
    size_t nl = sizeof(l->names);
    l->crt = nvs_blob(h, "crt", &l->crt_len);
    if (!l->crt || nvs_get_u32(h, "na", &l->not_after) != ESP_OK ||
        nvs_get_str(h, "names", l->names, &nl) != ESP_OK) {
        free(l->crt);
        free(l);
        return;
    }
    atomic_store(&s_leaf, l);
}

static uint32_t now_ref(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    time_t t = time(NULL);
    return tlsc_ref_time(t > (time_t)TLSC_EPOCH_FLOOR ? (uint32_t)t : 0,
                         tlsc_build_epoch(app->date, app->time));
}

bool tlscert_stale(void)
{
    leaf_t *l = atomic_load(&s_leaf);
    if (!s_ca_pem) return false;                /* not loaded: nothing to renew yet */
    return tlsc_need_leaf(l ? l->names : NULL, l ? l->not_after : 0,
                          netmgr_hostname(), netmgr_ip(), now_ref());
}

bool tlscert_refresh(void)
{
    if (!s_lock || !s_ca_pem) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool issued = false;
    if (tlscert_stale()) {
        const char *host = netmgr_hostname();
        const char *ip = netmgr_ip();
        uint32_t nb = now_ref() - 86400;        /* a day's grace for a slow clock */
        uint32_t na = nb + TLSC_LEAF_DAYS * 86400u;
        char snb[15], sna[15], subject[96];
        tlsc_x509_time(nb, snb);
        tlsc_x509_time(na, sna);
        snprintf(subject, sizeof(subject), "CN=%s.local," ORG, host);

        leaf_t *l = alloc_spiram(sizeof(*l));
        int r = l ? 0 : -1;
        if (l) {
            memset(l, 0, sizeof(*l));
            r = issue(false, subject, &s_key, snb, sna, host, ip, &l->crt, &l->crt_len);
        }
        if (r == 0) {
            l->not_after = na;
            tlsc_names(l->names, sizeof(l->names), host, ip);
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                esp_err_t err = nvs_set_blob(h, "crt", l->crt, l->crt_len);
                if (err == ESP_OK) err = nvs_set_u32(h, "na", na);
                if (err == ESP_OK) err = nvs_set_str(h, "names", l->names);
                if (err == ESP_OK) err = nvs_commit(h);
                nvs_close(h);
                if (err != ESP_OK) ESP_LOGW(TAG, "could not save the certificate: %s", esp_err_to_name(err));
            }
            /* The old one is left allocated on purpose: see tlscert_get(). */
            atomic_store(&s_leaf, l);
            issued = true;
            ESP_LOGI(TAG, "server certificate for %s.local, %s, valid to %s", host, ip, sna);
        } else {
            ESP_LOGE(TAG, "could not issue the server certificate: -0x%04x", (unsigned)-r);
            if (l) free(l->crt);
            free(l);
        }
    }
    xSemaphoreGive(s_lock);
    return issued;
}

esp_err_t tlscert_load(void)
{
    if (s_ca_pem) return ESP_OK;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    mbedtls_pk_init(&s_ca_key);
    mbedtls_pk_init(&s_key);

    char name[96];
    snprintf(name, sizeof(name), "CN=%s CA," ORG, netmgr_hostname());
    s_ca_name = dup_spiram(name, strlen(name) + 1);
    if (!s_ca_name) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    bool made = false;
    err = load_or_make_ca(h, &made);
    if (err == ESP_OK) err = load_or_make_key(h);
    if (err == ESP_OK && !made) load_leaf(h);
    nvs_close(h);
    if (err != ESP_OK || !s_ca_pem) {
        ESP_LOGE(TAG, "no certificate: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }
    tlscert_refresh();
    if (!atomic_load(&s_leaf)) return ESP_FAIL;
    ESP_LOGI(TAG, "CA SHA-256 %s", s_ca_sha);
    return ESP_OK;
}

bool tlscert_get(const uint8_t **crt, size_t *crt_len, const uint8_t **key, size_t *key_len)
{
    leaf_t *l = atomic_load(&s_leaf);
    if (!l || !s_key_der) return false;
    *crt = l->crt;
    *crt_len = l->crt_len;
    *key = s_key_der;
    *key_len = s_key_len;
    return true;
}

const char *tlscert_ca_pem(void) { return s_ca_pem; }

const char *tlscert_ca_sha256(void) { return s_ca_sha; }

uint32_t tlscert_not_after(void)
{
    leaf_t *l = atomic_load(&s_leaf);
    return l ? l->not_after : 0;
}
