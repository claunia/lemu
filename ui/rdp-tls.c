/*
 * RDP server: TLS 1.2 for the clients that ask for it, with BearSSL
 * (ui/rdp/bearssl). One suite, ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 on
 * P-256, with BearSSL's small i15 code.
 *
 * The engine pump comes from ntrdps's tls.c; here it runs on a
 * non-blocking QIOChannel, and the random bytes come from QEMU.
 *
 * The certificate is self-signed, made at the start from the key: a new
 * key each run, or the key in the tls-key file. The certificate is made
 * the same way each time (fixed dates and names, deterministic ECDSA), so
 * with a key file a client that remembers it sees the same certificate
 * after a restart. TLS gives a private channel and no identity; the
 * password does the check (rdp-login.c).
 *
 * Parts come from ntrdps's tls.c.
 * Copyright (c) 2026 ntrdps authors.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "crypto/random.h"
#include "io/channel.h"
#include "rdp-int.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "bearssl.h"
#pragma GCC diagnostic pop

#define RDP_CERT_NAME  "LEMU"
#define RDP_CERT_FROM  "260101000000Z"  /* UTCTime: 2026-01-01 */
#define RDP_CERT_UNTIL "491231235959Z"  /* the last UTCTime: 2049 */

struct tls_conn {
    QIOChannel *ioc;
    bool eof;                   /* the client closed the socket */
    bool sockerr;               /* a read or write failed */
    GIOCondition events;        /* what to wait for after WOULDBLOCK */
    char err[64];
    br_ssl_server_context sc;
    unsigned char ibuf[BR_SSL_BUFSIZE_INPUT];
    unsigned char obuf[BR_SSL_BUFSIZE_OUTPUT];
};

static const uint16_t rdp_suites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
};

/* The key, the certificate (rdp_tls_setup). */
static unsigned char rdp_key_x[32];
static br_ec_private_key rdp_key = {
    BR_EC_secp256r1, rdp_key_x, sizeof(rdp_key_x)
};
static br_x509_certificate rdp_chain[1];
static char rdp_fp[32 * 3 + 1];

/* The order n of P-256: a private key is 1 ... n - 1. */
static const unsigned char p256_n[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
    0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51
};

static const unsigned char oid_ecdsa_sha256[] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02
};
static const unsigned char oid_ec_public_key[] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01
};
static const unsigned char oid_p256[] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
};
static const unsigned char oid_cn[] = { 0x55, 0x04, 0x03 };
static const unsigned char oid_ski[] = { 0x55, 0x1D, 0x0E };
static const unsigned char oid_aki[] = { 0x55, 0x1D, 0x23 };
static const unsigned char oid_basic[] = { 0x55, 0x1D, 0x13 };

/* ---- DER, just enough ---- */

static void der_add(GByteArray *o, const void *p, size_t n)
{
    g_byte_array_append(o, p, n);
}

static void der_tlv(GByteArray *o, unsigned char tag, const void *v,
                    size_t n)
{
    unsigned char h[4];
    size_t k = 0;

    h[k++] = tag;
    if (n < 0x80) {
        h[k++] = n;
    } else if (n < 0x100) {
        h[k++] = 0x81;
        h[k++] = n;
    } else {
        h[k++] = 0x82;
        h[k++] = n >> 8;
        h[k++] = n;
    }
    der_add(o, h, k);
    der_add(o, v, n);
}

/* inner, wrapped in tag, onto o; inner is freed. */
static void der_wrap(GByteArray *o, unsigned char tag, GByteArray *inner)
{
    der_tlv(o, tag, inner->data, inner->len);
    g_byte_array_free(inner, true);
}

/* One element of p: its tag, its value. False: not DER. */
static bool der_next(const unsigned char **p, size_t *left,
                     unsigned char *tag, const unsigned char **v,
                     size_t *n)
{
    size_t h = 2, k, i;

    if (*left < 2) {
        return false;
    }
    *tag = (*p)[0];
    *n = (*p)[1];
    if (*n & 0x80) {
        k = *n & 0x7F;
        if (k == 0 || k > 2 || *left < 2 + k) {
            return false;
        }
        *n = 0;
        for (i = 0; i < k; i++) {
            *n = (*n << 8) | (*p)[2 + i];
        }
        h = 2 + k;
    }
    if (*left - h < *n) {
        return false;
    }
    *v = *p + h;
    *p += h + *n;
    *left -= h + *n;
    return true;
}

static void der_name(GByteArray *o)
{
    GByteArray *atv = g_byte_array_new(), *set = g_byte_array_new();
    GByteArray *rdn = g_byte_array_new();

    der_tlv(atv, 0x06, oid_cn, sizeof(oid_cn));
    der_tlv(atv, 0x0C, RDP_CERT_NAME, strlen(RDP_CERT_NAME));
    der_wrap(set, 0x30, atv);
    der_wrap(rdn, 0x31, set);
    der_wrap(o, 0x30, rdn);
}

static void der_alg(GByteArray *o)
{
    GByteArray *a = g_byte_array_new();

    der_tlv(a, 0x06, oid_ecdsa_sha256, sizeof(oid_ecdsa_sha256));
    der_wrap(o, 0x30, a);
}

/* ---- the key ---- */

static bool key_ok(const unsigned char *k)
{
    static const unsigned char zero[32];

    return memcmp(k, zero, 32) != 0 && memcmp(k, p256_n, 32) < 0;
}

static bool key_new(unsigned char *k, Error **errp)
{
    do {
        if (qcrypto_random_bytes(k, 32, errp) < 0) {
            return false;
        }
    } while (!key_ok(k));
    return true;
}

/* SEC1 ECPrivateKey: SEQUENCE { 1, OCTET STRING key, [0] curve, ... } */
static bool key_sec1(const unsigned char *d, size_t n, unsigned char *k)
{
    const unsigned char *p = d, *v, *q, *c;
    size_t left = n, len, ql, cl;
    unsigned char tag;

    if (!der_next(&p, &left, &tag, &q, &ql) || tag != 0x30 ||
        !der_next(&q, &ql, &tag, &v, &len) || tag != 0x02 || len != 1 ||
        v[0] != 1 ||
        !der_next(&q, &ql, &tag, &v, &len) || tag != 0x04 || len == 0 ||
        len > 32) {
        return false;
    }
    memset(k, 0, 32);
    memcpy(k + 32 - len, v, len);
    if (ql > 0 && q[0] == 0xA0) {
        if (!der_next(&q, &ql, &tag, &c, &cl) ||
            !der_next(&c, &cl, &tag, &v, &len) || tag != 0x06 ||
            len != sizeof(oid_p256) || memcmp(v, oid_p256, len) != 0) {
            return false;               /* another curve */
        }
    }
    return key_ok(k);
}

/* PKCS #8: SEQUENCE { 0, SEQUENCE { ecPublicKey, P-256 }, OCTET STRING
 * { SEC1 } } */
static bool key_pkcs8(const unsigned char *d, size_t n, unsigned char *k)
{
    const unsigned char *p = d, *v, *q, *a;
    size_t left = n, len, ql, al;
    unsigned char tag;

    if (!der_next(&p, &left, &tag, &q, &ql) || tag != 0x30 ||
        !der_next(&q, &ql, &tag, &v, &len) || tag != 0x02 || len != 1 ||
        v[0] != 0 ||
        !der_next(&q, &ql, &tag, &a, &al) || tag != 0x30 ||
        !der_next(&a, &al, &tag, &v, &len) || tag != 0x06 ||
        len != sizeof(oid_ec_public_key) ||
        memcmp(v, oid_ec_public_key, len) != 0 ||
        !der_next(&a, &al, &tag, &v, &len) || tag != 0x06 ||
        len != sizeof(oid_p256) || memcmp(v, oid_p256, len) != 0 ||
        !der_next(&q, &ql, &tag, &v, &len) || tag != 0x04) {
        return false;
    }
    return key_sec1(v, len, k);
}

/*
 * A PEM file ("EC PRIVATE KEY" or "PRIVATE KEY"; other blocks, such as
 * the "EC PARAMETERS" of "openssl ecparam -genkey", are skipped), or DER.
 * text ends with a NUL (g_file_get_contents).
 */
static bool key_parse(const char *text, size_t n, unsigned char *k)
{
    static const char *const kinds[] = {
        "-----BEGIN EC PRIVATE KEY-----", "-----BEGIN PRIVATE KEY-----"
    };
    const char *b = NULL, *e;
    g_autofree char *b64 = NULL;
    guchar *der;
    gsize len;
    bool ok;
    int i;

    for (i = 0; i < ARRAY_SIZE(kinds) && b == NULL; i++) {
        b = g_strstr_len(text, n, kinds[i]);
    }
    if (b == NULL) {
        if (g_strstr_len(text, n, "-----BEGIN ") != NULL) {
            return false;               /* PEM, without a private key */
        }
        return key_sec1((const unsigned char *)text, n, k) ||
               key_pkcs8((const unsigned char *)text, n, k);
    }
    b = strchr(b, '\n');
    e = b != NULL ? strstr(b, "-----END ") : NULL;
    if (e == NULL) {
        return false;
    }
    b64 = g_strndup(b, e - b);
    der = g_base64_decode(b64, &len);
    ok = key_sec1(der, len, k) || key_pkcs8(der, len, k);
    memset(der, 0, len);
    g_free(der);
    memset(b64, 0, strlen(b64));
    return ok;
}

/* A new key file: SEC1 in PEM, as "openssl ec" writes it. */
static bool key_save(const char *path, const unsigned char *k,
                     const unsigned char *pub, Error **errp)
{
    GByteArray *der = g_byte_array_new(), *t = g_byte_array_new();
    GByteArray *seq = g_byte_array_new();
    GString *pem = g_string_new("-----BEGIN EC PRIVATE KEY-----\n");
    unsigned char bits[66];
    char *b64;
    size_t i, len;
    int fd;
    bool ok;

    der_tlv(seq, 0x02, "\x01", 1);
    der_tlv(seq, 0x04, k, 32);
    der_tlv(t, 0x06, oid_p256, sizeof(oid_p256));
    der_wrap(seq, 0xA0, t);
    bits[0] = 0;
    memcpy(bits + 1, pub, 65);
    t = g_byte_array_new();
    der_tlv(t, 0x03, bits, sizeof(bits));
    der_wrap(seq, 0xA1, t);
    der_wrap(der, 0x30, seq);

    b64 = g_base64_encode(der->data, der->len);
    len = strlen(b64);
    for (i = 0; i < len; i += 64) {
        g_string_append_len(pem, b64 + i, MIN(64, len - i));
        g_string_append_c(pem, '\n');
    }
    g_string_append(pem, "-----END EC PRIVATE KEY-----\n");

    fd = qemu_create(path, O_WRONLY | O_EXCL, 0600, errp);
    ok = fd >= 0;
    if (ok && qemu_write_full(fd, pem->str, pem->len) != pem->len) {
        error_setg_errno(errp, errno, "rdp: cannot write %s", path);
        ok = false;
    }
    if (fd >= 0) {
        close(fd);
    }
    memset(der->data, 0, der->len);
    g_byte_array_free(der, true);
    memset(b64, 0, len);
    g_free(b64);
    memset(pem->str, 0, pem->len);
    g_string_free(pem, true);
    return ok;
}

/* The key of the file, or a new one into a file that does not exist. */
static bool key_load(const char *path, unsigned char *k, bool *made,
                     Error **errp)
{
    g_autoptr(GError) gerr = NULL;
    char *text;
    gsize n;
    bool ok;

    *made = false;
    if (!g_file_get_contents(path, &text, &n, &gerr)) {
        if (!g_error_matches(gerr, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            error_setg(errp, "rdp: cannot read %s: %s", path,
                       gerr->message);
            return false;
        }
        *made = true;
        return key_new(k, errp);
    }
    ok = key_parse(text, n, k);
    memset(text, 0, n);
    g_free(text);
    if (!ok) {
        error_setg(errp, "rdp: %s holds no P-256 private key (PEM or DER, "
                   "SEC1 or PKCS #8)", path);
    }
    return ok;
}

/* ---- the certificate ---- */

/* Self-signed, the layout of ntrdps's test certificate. */
static bool cert_make(const unsigned char *pub, Error **errp)
{
    static const unsigned char v3[] = { 0xA0, 0x03, 0x02, 0x01, 0x02 };
    static const unsigned char ca[] = { 0x30, 0x03, 0x01, 0x01, 0xFF };
    GByteArray *tbs = g_byte_array_new(), *t, *e, *exts, *o;
    unsigned char serial[32], ski[32], hash[32], bits[66], sig[80];
    br_sha256_context h;
    size_t sl;
    int i;

    /* serial and key id from the public key: the same for the same key */
    br_sha256_init(&h);
    br_sha256_update(&h, "LEMU RDP certificate 1", 22);
    br_sha256_update(&h, pub, 65);
    br_sha256_out(&h, serial);
    serial[0] = (serial[0] & 0x7F) | 0x40;      /* positive, 16 bytes */
    br_sha256_init(&h);
    br_sha256_update(&h, pub, 65);              /* RFC 7093 method 1 */
    br_sha256_out(&h, ski);

    der_add(tbs, v3, sizeof(v3));
    der_tlv(tbs, 0x02, serial, 16);
    der_alg(tbs);
    der_name(tbs);
    t = g_byte_array_new();
    der_tlv(t, 0x17, RDP_CERT_FROM, strlen(RDP_CERT_FROM));
    der_tlv(t, 0x17, RDP_CERT_UNTIL, strlen(RDP_CERT_UNTIL));
    der_wrap(tbs, 0x30, t);
    der_name(tbs);

    t = g_byte_array_new();                     /* the public key */
    e = g_byte_array_new();
    der_tlv(e, 0x06, oid_ec_public_key, sizeof(oid_ec_public_key));
    der_tlv(e, 0x06, oid_p256, sizeof(oid_p256));
    der_wrap(t, 0x30, e);
    bits[0] = 0;
    memcpy(bits + 1, pub, 65);
    der_tlv(t, 0x03, bits, sizeof(bits));
    der_wrap(tbs, 0x30, t);

    exts = g_byte_array_new();
    e = g_byte_array_new();                     /* subjectKeyIdentifier */
    der_tlv(e, 0x06, oid_ski, sizeof(oid_ski));
    t = g_byte_array_new();
    der_tlv(t, 0x04, ski, 20);
    der_wrap(e, 0x04, t);
    der_wrap(exts, 0x30, e);
    e = g_byte_array_new();                     /* authorityKeyIdentifier */
    der_tlv(e, 0x06, oid_aki, sizeof(oid_aki));
    t = g_byte_array_new();
    o = g_byte_array_new();
    der_tlv(o, 0x80, ski, 20);
    der_wrap(t, 0x30, o);
    der_wrap(e, 0x04, t);
    der_wrap(exts, 0x30, e);
    e = g_byte_array_new();                     /* basicConstraints CA */
    der_tlv(e, 0x06, oid_basic, sizeof(oid_basic));
    der_tlv(e, 0x01, "\xFF", 1);                /* critical */
    der_tlv(e, 0x04, ca, sizeof(ca));
    der_wrap(exts, 0x30, e);
    t = g_byte_array_new();
    der_wrap(t, 0x30, exts);
    der_wrap(tbs, 0xA3, t);

    t = g_byte_array_new();
    der_wrap(t, 0x30, tbs);                     /* the TBSCertificate */
    br_sha256_init(&h);
    br_sha256_update(&h, t->data, t->len);
    br_sha256_out(&h, hash);
    sl = br_ecdsa_i15_sign_asn1(&br_ec_p256_m15, &br_sha256_vtable, hash,
                                &rdp_key, sig + 1);
    if (sl == 0) {
        g_byte_array_free(t, true);
        error_setg(errp, "rdp: cannot sign the TLS certificate");
        return false;
    }
    der_alg(t);
    sig[0] = 0;
    der_tlv(t, 0x03, sig, sl + 1);
    o = g_byte_array_new();
    der_wrap(o, 0x30, t);

    rdp_chain[0].data_len = o->len;
    rdp_chain[0].data = g_byte_array_free(o, false);

    br_sha256_init(&h);
    br_sha256_update(&h, rdp_chain[0].data, rdp_chain[0].data_len);
    br_sha256_out(&h, hash);
    for (i = 0; i < 32; i++) {
        snprintf(rdp_fp + i * 3, sizeof(rdp_fp) - i * 3, "%02x:", hash[i]);
    }
    rdp_fp[32 * 3 - 1] = '\0';                 /* no colon at the end */
    return true;
}

bool rdp_tls_setup(const char *key_file, Error **errp)
{
    unsigned char pub[65];
    bool made = false;

    if (rdp_chain[0].data != NULL) {
        return true;
    }
    if (key_file != NULL ? !key_load(key_file, rdp_key_x, &made, errp)
                         : !key_new(rdp_key_x, errp)) {
        return false;
    }
    if (br_ec_p256_m15.mulgen(pub, rdp_key_x, 32, BR_EC_secp256r1) != 65) {
        error_setg(errp, "rdp: the TLS key is not a P-256 key");
        return false;
    }
    if (made && !key_save(key_file, rdp_key_x, pub, errp)) {
        return false;
    }
    return cert_make(pub, errp);
}

const char *rdp_tls_fingerprint(void)
{
    return rdp_fp;
}

/* ---- one connection ---- */

struct tls_conn *rdp_tls_start(QIOChannel *ioc)
{
    struct tls_conn *t = g_new0(struct tls_conn, 1);
    br_ssl_engine_context *eng = &t->sc.eng;
    unsigned char seed[32];

    t->ioc = ioc;
    br_ssl_server_zero(&t->sc);
    br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(eng, rdp_suites, ARRAY_SIZE(rdp_suites));
    br_ssl_engine_set_ec(eng, &br_ec_p256_m15);
    br_ssl_server_set_single_ec(&t->sc, rdp_chain, 1, &rdp_key,
                                BR_KEYTYPE_SIGN, 0, &br_ec_p256_m15,
                                br_ecdsa_i15_sign_asn1);
    br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_aes_ctr(eng, &br_aes_ct_ctr_vtable);
    br_ssl_engine_set_ghash(eng, &br_ghash_ctmul32);
    br_ssl_engine_set_gcm(eng, &br_sslrec_in_gcm_vtable,
                          &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_buffers_bidi(eng, t->ibuf, sizeof(t->ibuf),
                                   t->obuf, sizeof(t->obuf));
    br_ssl_engine_add_flags(eng, BR_OPT_NO_RENEGOTIATION);
    if (qcrypto_random_bytes(seed, sizeof(seed), NULL) < 0) {
        g_free(t);
        return NULL;
    }
    br_ssl_engine_inject_entropy(eng, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));
    if (!br_ssl_server_reset(&t->sc) ||
        br_ssl_engine_last_error(eng) != BR_ERR_OK) {
        g_free(t);
        return NULL;
    }
    return t;
}

/* The engine's records to the socket. 1 = sent some, 0 = the socket is
 * full (wait for G_IO_OUT), -1 = error. */
static int send_records(struct tls_conn *t)
{
    unsigned char *buf;
    size_t len;
    ssize_t n;

    buf = br_ssl_engine_sendrec_buf(&t->sc.eng, &len);
    n = qio_channel_write(t->ioc, (const char *)buf, len, NULL);
    if (n > 0) {
        br_ssl_engine_sendrec_ack(&t->sc.eng, n);
        return 1;
    }
    if (n == QIO_CHANNEL_ERR_BLOCK) {
        t->events |= G_IO_OUT;
        return 0;
    }
    t->sockerr = true;
    return -1;
}

/*
 * Moves records between the engine and the socket until the engine
 * wants application data (want = BR_SSL_SENDAPP) or has some
 * (BR_SSL_RECVAPP); the first time, that is the handshake. 1 = ready,
 * 0 = wait for the socket (t->events), -1 = closed or broken.
 */
static int run(struct tls_conn *t, unsigned want)
{
    br_ssl_engine_context *eng = &t->sc.eng;
    unsigned state;
    unsigned char *buf;
    size_t len;
    ssize_t n;

    for (;;) {
        t->events = 0;
        state = br_ssl_engine_current_state(eng);
        if (state & BR_SSL_CLOSED) {
            return -1;
        }
        if (state & BR_SSL_SENDREC) {
            n = send_records(t);
            if (n < 0) {
                return -1;
            }
            if (n > 0) {
                continue;
            }
        }
        if (state & want) {
            return 1;
        }
        if (state & BR_SSL_RECVREC) {
            buf = br_ssl_engine_recvrec_buf(eng, &len);
            n = qio_channel_read(t->ioc, (char *)buf, len, NULL);
            if (n > 0) {
                br_ssl_engine_recvrec_ack(eng, n);
                continue;
            }
            if (n == 0) {
                t->eof = true;
                return -1;
            }
            if (n != QIO_CHANNEL_ERR_BLOCK) {
                t->sockerr = true;
                return -1;
            }
            t->events |= G_IO_IN;
        }
        return t->events ? 0 : -1;
    }
}

int rdp_tls_read(struct tls_conn *t, void *buf, int len)
{
    unsigned char *p;
    size_t n;
    int r;

    r = run(t, BR_SSL_RECVAPP);
    if (r == 0) {
        return RDP_TLS_WOULDBLOCK;
    }
    if (r < 0) {
        /* A clean close_notify, or the client just dropped the socket */
        if (t->eof || (!t->sockerr &&
                       br_ssl_engine_last_error(&t->sc.eng) == BR_ERR_OK)) {
            return 0;
        }
        return -1;
    }
    p = br_ssl_engine_recvapp_buf(&t->sc.eng, &n);
    n = MIN(n, (size_t)len);
    memcpy(buf, p, n);
    br_ssl_engine_recvapp_ack(&t->sc.eng, n);
    return n;
}

int rdp_tls_write(struct tls_conn *t, const void *buf, int len)
{
    unsigned char *p;
    size_t n;
    int r;

    r = run(t, BR_SSL_SENDAPP);
    if (r == 0) {
        return RDP_TLS_WOULDBLOCK;
    }
    if (r < 0) {
        return -1;
    }
    p = br_ssl_engine_sendapp_buf(&t->sc.eng, &n);
    n = MIN(n, (size_t)len);
    memcpy(p, buf, n);
    br_ssl_engine_sendapp_ack(&t->sc.eng, n);
    /* Close the record and push what the socket takes now */
    br_ssl_engine_flush(&t->sc.eng, 0);
    while (br_ssl_engine_current_state(&t->sc.eng) & BR_SSL_SENDREC) {
        t->events = 0;
        if (send_records(t) <= 0) {
            break;              /* the rest: rdp_tls_flush, after G_IO_OUT */
        }
    }
    return n;
}

int rdp_tls_flush(struct tls_conn *t)
{
    unsigned state;
    int n;

    br_ssl_engine_flush(&t->sc.eng, 0);
    for (;;) {
        state = br_ssl_engine_current_state(&t->sc.eng);
        if (state & BR_SSL_CLOSED) {
            return -1;
        }
        if (!(state & BR_SSL_SENDREC)) {
            return 0;
        }
        t->events = 0;
        n = send_records(t);
        if (n <= 0) {
            return n == 0 ? RDP_TLS_WOULDBLOCK : -1;
        }
    }
}

GIOCondition rdp_tls_events(struct tls_conn *t)
{
    return t->events;
}

bool rdp_tls_pending(struct tls_conn *t)
{
    return (br_ssl_engine_current_state(&t->sc.eng) & BR_SSL_RECVAPP) != 0;
}

bool rdp_tls_unsent(struct tls_conn *t)
{
    unsigned state = br_ssl_engine_current_state(&t->sc.eng);

    return !(state & BR_SSL_CLOSED) && (state & BR_SSL_SENDREC);
}

const char *rdp_tls_error(struct tls_conn *t)
{
    int err;

    if (t->sockerr) {
        return "network error";
    }
    if (t->eof) {
        return "the client closed the connection";
    }
    err = br_ssl_engine_last_error(&t->sc.eng);
    if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_SEND_FATAL_ALERT) {
        snprintf(t->err, sizeof(t->err), "the client sent TLS alert %d",
                 err - BR_ERR_RECV_FATAL_ALERT);
    } else {
        snprintf(t->err, sizeof(t->err), "TLS error %d", err);
    }
    return t->err;
}

void rdp_tls_free(struct tls_conn *t)
{
    if (t != NULL) {
        memset(t, 0, sizeof(*t));       /* the session keys */
        g_free(t);
    }
}
