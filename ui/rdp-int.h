/*
 * RDP server: what ui/rdp.c, ui/rdp-tls.c and ui/rdp-login.c share.
 *
 * Copyright (c) 2026 LEMU authors.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef UI_RDP_INT_H
#define UI_RDP_INT_H

#include "io/channel.h"

struct tls_conn;

/* ---- rdp-tls.c: TLS 1.2 with BearSSL ---- */

/* rdp_tls_read, rdp_tls_write, rdp_tls_flush: the socket can take or
 * give nothing now. Not 0 (the client closed) and not -1 (an error). */
#define RDP_TLS_WOULDBLOCK (-2)

/*
 * Once, before the first client: the key and the self-signed
 * certificate. key_file NULL: a new key for this run. Else the key in
 * that file; a file that does not exist yet gets a new key (mode 0600).
 */
bool rdp_tls_setup(const char *key_file, Error **errp);

/* The SHA-256 of the certificate, as "ab:cd:...", for the log. */
const char *rdp_tls_fingerprint(void);

/* The TLS server on a connected, non-blocking channel. NULL: failed. */
struct tls_conn *rdp_tls_start(QIOChannel *ioc);

/* Like read(): > 0 bytes, 0 = the client closed, RDP_TLS_WOULDBLOCK or
 * -1 (rdp_tls_error says why). Drives the handshake too. */
int rdp_tls_read(struct tls_conn *t, void *buf, int len);

/* Like write(): > 0 bytes taken, RDP_TLS_WOULDBLOCK or -1. */
int rdp_tls_write(struct tls_conn *t, const void *buf, int len);

/* Sends the records that wait: 0 = all gone, RDP_TLS_WOULDBLOCK, -1. */
int rdp_tls_flush(struct tls_conn *t);

/* What the engine waits for after RDP_TLS_WOULDBLOCK: G_IO_IN, G_IO_OUT. */
GIOCondition rdp_tls_events(struct tls_conn *t);

/* True when the engine holds client data that rdp_tls_read did not take.
 * No socket event comes for it. */
bool rdp_tls_pending(struct tls_conn *t);

/* True when records still wait to go out. */
bool rdp_tls_unsent(struct tls_conn *t);

/* Why the last call failed, for the log. */
const char *rdp_tls_error(struct tls_conn *t);

void rdp_tls_free(struct tls_conn *t);

/* ---- rdp-login.c: the password ---- */

/* The password that clients must give (UTF-8), or NULL for none. */
void rdp_login_set_password(const char *password);

#endif
