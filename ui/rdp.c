/*
 * RDP server: the guest's screen, keyboard and mouse for RDP clients,
 * next to VNC.
 *
 * The RDP protocol is ntrdps's code in ui/rdp/ (see ui/rdp/README.md),
 * unchanged. This file is its QEMU side:
 *
 *  - the core (CORE_API, ui/rdp/core.h): the guest's screen from a
 *    display listener, kept in a copy that the clients get (as VNC does);
 *    the guest's pointer; the clients' keys and mouse;
 *  - the connections: what ntrdps's session.c does with a thread and
 *    select(), this file does in the main loop, with a channel watch, a
 *    bottom half (session_wake) and a timer;
 *  - the log: ntrdps's log lines become trace events (rdp_log, rdp_hex).
 *
 * TLS (rdp-tls.c) and the password (rdp-login.c) are optional, as in
 * ntrdps (its settings AllowTLS, Login and LoginPlain).
 *
 * One client has the console at a time. A new client takes it from the
 * one before once it is through the login (ntrdps's NewClient 0).
 *
 * Everything runs in the main loop, with the BQL.
 *
 * Parts come from ntrdps's session.c.
 * Copyright (c) 2026 ntrdps authors.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/qapi-types-sockets.h"
#include "crypto/secret_common.h"
#include "io/channel-socket.h"
#include "io/net-listener.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "ui/qemu-pixman.h"
#include "ui/rdp.h"
#include "standard-headers/linux/input-event-codes.h"
#include "trace.h"

#include "rdp/session.h"
#include "rdp/frame.h"
#include "rdp/log.h"
#include "rdp/rdp.h"
#include "rdp-int.h"

#define RDP_TURN_MS        100  /* a connection's turn at least this often */
#define RDP_FLUSH_MAX_MS   5000 /* END_FLUSH: give up sending after this */
#define RDP_CLOSE_DRAIN_MS 2000 /* after our FIN: read until the client's */
#define RDP_TILE           16   /* the grid of the screen compare */
#define RDP_RECTS          64   /* areas per Damage call */
#define RDP_REFRESH_BASE   GUI_REFRESH_INTERVAL_DEFAULT
#define RDP_REFRESH_INC    30
#define RDP_REFRESH_MAX    250
#define RDP_KEY_DELAY_MS   10   /* between keys to the guest, as VNC */
#define RDP_WHEEL_MAX      10   /* wheel notches of one event, at most */
#define RDP_CONN_MAX       8    /* open connections (ntrdps's NET_SLOTS) */

/*
 * Our copy of the screen: blue, green, red and a spare byte in memory,
 * the 32 bpp frame that ntrdps reads (update.c).
 */
#if HOST_BIG_ENDIAN
#define RDP_FORMAT PIXMAN_b8g8r8x8
#else
#define RDP_FORMAT PIXMAN_x8r8g8b8
#endif

/* The Pause key: clients send it as E1 1D down, then 45 down. */
enum {
    E1_NONE,                    /* nothing waits */
    E1_HELD,                    /* E1 1D down came: Pause, or a Ctrl? */
    E1_PAUSE,                   /* Pause is down */
};

typedef struct RdpServer RdpServer;
typedef struct RdpConn RdpConn;

/* A session of the core (CORE_API SessionOpen). */
typedef struct RdpCoreSession {
    const FRONTEND_API *fe;
    void *fe_ctx;
    bool ready;                 /* it has the console (SessionReady) */
    bool view_only;
    QTAILQ_ENTRY(RdpCoreSession) next;
} RdpCoreSession;

/* One client connection: its socket, and its session while that runs. */
struct RdpConn {
    RdpServer *rs;
    QIOChannelSocket *sioc;
    QIOChannel *ioc;
    SESSION *s;                 /* NULL after the end, while we drain */
    guint watch;
    GIOCondition watch_cond;
    QEMUBH *bh;                 /* session_wake */
    QEMUTimer *timer;           /* a turn at least every RDP_TURN_MS */
    int64_t drain_end;          /* the close: at the latest then */
    bool closed;
    QTAILQ_ENTRY(RdpConn) next;
};

struct RdpServer {
    DisplayChangeListener dcl;
    QIONetListener *listener;
    QKbdState *kbd;
    Notifier led_notifier;
    bool leds_known;            /* the guest has set its keyboard LEDs */

    DisplaySurface *guest;      /* the console's screen */
    pixman_image_t *server;     /* our copy (RDP_FORMAT): what clients get */
    unsigned long *dirty;       /* tiles that the guest changed */
    unsigned long *hit;         /* the tiles of one row that differed */
    int cols, rows;             /* the tile grid */

    QEMUCursor *cursor;         /* the guest's pointer shape, or NULL */
    bool cursor_on;
    unsigned long ptr_serial;

    unsigned int buttons;       /* CORE_BUTTON_*: down in the guest */
    int last_x, last_y;         /* relative mice: the last position */
    bool last_valid;
    int e1;                     /* E1_* */

    QTAILQ_HEAD(, RdpCoreSession) sessions;
    QTAILQ_HEAD(, RdpConn) conns;
};

static RdpServer *rdp_server;

static uint32_t rdp_bmap[INPUT_BUTTON__MAX] = {
    [INPUT_BUTTON_LEFT]   = CORE_BUTTON_LEFT,
    [INPUT_BUTTON_RIGHT]  = CORE_BUTTON_RIGHT,
    [INPUT_BUTTON_MIDDLE] = CORE_BUTTON_MIDDLE,
};

/* ---- the log (ntrdps log.h) ---- */

/*
 * ntrdps's log.h has no format attributes, so GCC would ask for them on
 * the functions that take its format strings.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-format-attribute"

void log_msg(int level, const char *fmt, ...)
{
    va_list ap;
    char *msg;

    if (!trace_event_get_state_backends(TRACE_RDP_LOG)) {
        return;
    }
    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    trace_rdp_log(level, msg);
    g_free(msg);
}

void session_end(SESSION *s, int flush, const char *fmt, ...)
{
    va_list ap;

    if (s->ending != END_NO) {
        if (!flush) {
            s->ending = END_NOW;        /* an error while flushing */
        }
        return;
    }
    va_start(ap, fmt);
    vsnprintf(s->why, sizeof(s->why), fmt, ap);
    va_end(ap);
    s->ending = flush ? END_FLUSH : END_NOW;
    s->end_ms = GetTickCount();
}

#pragma GCC diagnostic pop

void log_hex(const char *direction, const void *buf, unsigned int len)
{
    const unsigned char *p = buf;
    unsigned int off, n, i;
    char line[96];
    int k;

    if (!trace_event_get_state_backends(TRACE_RDP_HEX)) {
        return;
    }
    trace_rdp_hex(direction, len);
    for (off = 0; off < len && off < LOG_HEX_MAX; off += 16) {
        n = MIN(16, len - off);
        k = snprintf(line, sizeof(line), "    %04x ", off);
        for (i = 0; i < 16; i++) {
            if (i < n) {
                k += snprintf(line + k, sizeof(line) - k, " %02x", p[off + i]);
            } else {
                k += snprintf(line + k, sizeof(line) - k, "   ");
            }
        }
        k += snprintf(line + k, sizeof(line) - k, "  ");
        for (i = 0; i < n; i++) {
            line[k++] = p[off + i] >= 0x20 && p[off + i] < 0x7F ? p[off + i]
                                                                : '.';
        }
        line[k] = '\0';
        trace_rdp_hex_line(line);
    }
    if (len > LOG_HEX_MAX) {
        snprintf(line, sizeof(line), "    (%u more bytes not shown)",
                 len - LOG_HEX_MAX);
        trace_rdp_hex_line(line);
    }
}

double log_ms(void)
{
    return g_get_monotonic_time() / 1000.0;
}

/* ---- the core: the screen ---- */

/* Input: look at the screen often again. */
static void rdp_refresh_soon(RdpServer *rs)
{
    if (rs->dcl.update_interval > RDP_REFRESH_BASE) {
        qemu_console_listener_set_refresh(&rs->dcl, RDP_REFRESH_BASE);
    }
}

/* The guest's whole screen into our copy. */
static void rdp_copy_all(RdpServer *rs)
{
    int w = MIN(surface_width(rs->guest), pixman_image_get_width(rs->server));
    int h = MIN(surface_height(rs->guest),
                pixman_image_get_height(rs->server));

    pixman_image_composite(PIXMAN_OP_SRC, rs->guest->image, NULL, rs->server,
                           0, 0, 0, 0, 0, 0, w, h);
    bitmap_zero(rs->dirty, rs->cols * rs->rows);
}

static void rdp_damage(RdpServer *rs, const RECT *r, int n)
{
    RdpCoreSession *cs;

    QTAILQ_FOREACH(cs, &rs->sessions, next) {
        cs->fe->Damage(cs->fe_ctx, r, n, 0);
    }
}

/*
 * The tiles that the guest changed: those that differ from our copy go
 * into it, and the sessions learn where. True if any differed.
 */
static bool rdp_compare(RdpServer *rs)
{
    int w = MIN(surface_width(rs->guest), pixman_image_get_width(rs->server));
    int h = MIN(surface_height(rs->guest),
                pixman_image_get_height(rs->server));
    pixman_format_code_t gf = surface_format(rs->guest);
    bool same = gf == RDP_FORMAT ||
                (!HOST_BIG_ENDIAN && gf == PIXMAN_a8r8g8b8);
    uint8_t *gdata = surface_data(rs->guest);
    int gstride = surface_stride(rs->guest);
    uint8_t *sdata = (uint8_t *)pixman_image_get_data(rs->server);
    int sstride = pixman_image_get_stride(rs->server);
    pixman_image_t *line = NULL;
    RECT rects[RDP_RECTS];
    int n = 0, tx, ty, end, x, y, y1, len;
    unsigned long base;
    const uint8_t *g;
    uint8_t *sv;
    bool any = false;

    if (!same) {
        line = qemu_pixman_linebuf_create(RDP_FORMAT, w);
    }
    for (ty = 0; ty < rs->rows && ty * RDP_TILE < h; ty++) {
        base = (unsigned long)ty * rs->cols;
        if (find_next_bit(rs->dirty, base + rs->cols, base) >=
            base + rs->cols) {
            continue;
        }
        y1 = MIN((ty + 1) * RDP_TILE, h);
        for (y = ty * RDP_TILE; y < y1; y++) {
            if (line) {
                qemu_pixman_linebuf_fill(line, rs->guest->image, w, 0, y);
                g = (const uint8_t *)pixman_image_get_data(line);
            } else {
                g = gdata + (size_t)y * gstride;
            }
            sv = sdata + (size_t)y * sstride;
            for (tx = find_next_bit(rs->dirty, base + rs->cols, base) - base;
                 tx < rs->cols;
                 tx = find_next_bit(rs->dirty, base + rs->cols,
                                    base + tx + 1) - base) {
                x = tx * RDP_TILE;
                if (x >= w) {
                    break;
                }
                len = MIN(RDP_TILE, w - x) * 4;
                if (memcmp(g + x * 4, sv + x * 4, len) != 0) {
                    memcpy(sv + x * 4, g + x * 4, len);
                    set_bit(tx, rs->hit);
                }
            }
        }
        bitmap_clear(rs->dirty, base, rs->cols);

        /* The row's changed tiles, a run at a time */
        for (tx = find_first_bit(rs->hit, rs->cols); tx < rs->cols;
             tx = find_next_bit(rs->hit, rs->cols, end)) {
            end = find_next_zero_bit(rs->hit, rs->cols, tx);
            if (n == RDP_RECTS) {
                rdp_damage(rs, rects, n);
                n = 0;
            }
            rects[n].left = tx * RDP_TILE;
            rects[n].top = ty * RDP_TILE;
            rects[n].right = MIN(end * RDP_TILE, w);
            rects[n].bottom = y1;
            n++;
            any = true;
        }
        bitmap_zero(rs->hit, rs->cols);
    }
    if (n > 0) {
        rdp_damage(rs, rects, n);
    }
    qemu_pixman_image_unref(line);
    return any;
}

static void rdp_refresh(DisplayChangeListener *dcl)
{
    RdpServer *rs = container_of(dcl, RdpServer, dcl);

    if (QTAILQ_EMPTY(&rs->sessions)) {
        qemu_console_listener_set_refresh(dcl, GUI_REFRESH_INTERVAL_IDLE);
        return;
    }
    qemu_console_hw_update(dcl->con);
    if (rs->guest == NULL || rs->server == NULL) {
        return;
    }
    if (rdp_compare(rs)) {
        dcl->update_interval = RDP_REFRESH_BASE;
    } else if (dcl->update_interval < RDP_REFRESH_MAX) {
        dcl->update_interval = MIN(dcl->update_interval + RDP_REFRESH_INC,
                                   RDP_REFRESH_MAX);
    }
}

static void rdp_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    RdpServer *rs = container_of(dcl, RdpServer, dcl);
    int x1 = x + w, y1 = y + h, tx0, tx1, ty;

    if (rs->dirty == NULL) {
        return;
    }
    x = MAX(x, 0);
    y = MAX(y, 0);
    tx0 = x / RDP_TILE;
    tx1 = MIN(DIV_ROUND_UP(x1, RDP_TILE), rs->cols);
    for (ty = y / RDP_TILE; ty < rs->rows && ty * RDP_TILE < y1; ty++) {
        if (tx1 > tx0) {
            bitmap_set(rs->dirty, ty * rs->cols + tx0, tx1 - tx0);
        }
    }
}

static void rdp_gfx_switch(DisplayChangeListener *dcl,
                           DisplaySurface *surface)
{
    RdpServer *rs = container_of(dcl, RdpServer, dcl);
    RdpCoreSession *cs;
    int w, h;

    if (surface == NULL) {
        return;
    }
    w = surface_width(surface);
    h = surface_height(surface);
    if (w <= 0 || h <= 0) {
        return;
    }
    rs->guest = surface;
    if (rs->server != NULL && pixman_image_get_width(rs->server) == w &&
        pixman_image_get_height(rs->server) == h) {
        bitmap_set(rs->dirty, 0, rs->cols * rs->rows);  /* compare it all */
        return;
    }
    qemu_pixman_image_unref(rs->server);
    g_free(rs->dirty);
    g_free(rs->hit);
    rs->server = pixman_image_create_bits(RDP_FORMAT, w, h, NULL, 0);
    rs->cols = DIV_ROUND_UP(w, RDP_TILE);
    rs->rows = DIV_ROUND_UP(h, RDP_TILE);
    rs->dirty = bitmap_new(rs->cols * rs->rows);
    rs->hit = bitmap_new(rs->cols);
    rdp_copy_all(rs);
    QTAILQ_FOREACH(cs, &rs->sessions, next) {
        cs->fe->FrameChanged(cs->fe_ctx);
    }
}

/* ---- the core: the pointer ---- */

static void rdp_pointer_changed(RdpServer *rs)
{
    RdpCoreSession *cs;

    if (++rs->ptr_serial == 0) {
        rs->ptr_serial = 1;             /* 0: the client's own arrow */
    }
    QTAILQ_FOREACH(cs, &rs->sessions, next) {
        cs->fe->PointerShape(cs->fe_ctx);
    }
}

static void rdp_cursor_define(DisplayChangeListener *dcl, QEMUCursor *c)
{
    RdpServer *rs = container_of(dcl, RdpServer, dcl);

    cursor_ref(c);
    cursor_unref(rs->cursor);
    rs->cursor = c;
    rdp_pointer_changed(rs);
}

static void rdp_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on)
{
    RdpServer *rs = container_of(dcl, RdpServer, dcl);

    if (on != rs->cursor_on) {
        rs->cursor_on = on;
        rdp_pointer_changed(rs);
    }
}

/*
 * The pointer of a guest that draws its own into the screen: a small
 * dot, so the client shows where its mouse is.
 */
static const char *const rdp_dot[] = {
    ".###.",
    "#ooo#",
    "#ooo#",
    "#ooo#",
    ".###.",
};

/* An opaque pixel; the others stay transparent (AND 1, black). */
static void rdp_ptr_pixel(CORE_POINTER *p, int x, int y, uint32_t rgb)
{
    p->and_mask[y][x / 8] &= ~(0x80 >> (x % 8));
    p->color[y][x * 3] = rgb >> 16;
    p->color[y][x * 3 + 1] = rgb >> 8;
    p->color[y][x * 3 + 2] = rgb;
}

static int __cdecl rc_pointer_get(void *core, CORE_POINTER *p)
{
    RdpServer *rs = core;
    unsigned long cb = p->cb;
    QEMUCursor *c = rs->cursor;
    CORE_POINTER v;
    int x, y, x0 = 0, y0 = 0, w, h;
    uint32_t px;

    memset(&v, 0, sizeof(v));
    memset(v.and_mask, 0xFF, sizeof(v.and_mask));
    v.cb = cb;
    v.serial = rs->ptr_serial;
    if (c == NULL) {
        v.width = v.height = ARRAY_SIZE(rdp_dot);
        v.hot_x = v.hot_y = ARRAY_SIZE(rdp_dot) / 2;
        for (y = 0; y < ARRAY_SIZE(rdp_dot); y++) {
            for (x = 0; rdp_dot[y][x] != '\0'; x++) {
                if (rdp_dot[y][x] != '.') {
                    rdp_ptr_pixel(&v, x, y,
                                  rdp_dot[y][x] == '#' ? 0 : 0xFFFFFF);
                }
            }
        }
    } else if (rs->cursor_on && c->width > 0 && c->height > 0) {
        /* At most 32 x 32 (NT's size): the part around the hot spot */
        w = MIN(c->width, CORE_POINTER_MAX);
        h = MIN(c->height, CORE_POINTER_MAX);
        if (c->hot_x >= CORE_POINTER_MAX) {
            x0 = MIN(c->hot_x - CORE_POINTER_MAX / 2, c->width - w);
        }
        if (c->hot_y >= CORE_POINTER_MAX) {
            y0 = MIN(c->hot_y - CORE_POINTER_MAX / 2, c->height - h);
        }
        v.width = w;
        v.height = h;
        v.hot_x = MIN(MAX(c->hot_x - x0, 0), w - 1);
        v.hot_y = MIN(MAX(c->hot_y - y0, 0), h - 1);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                px = c->data[(y0 + y) * c->width + x0 + x];
                if ((px >> 24) >= 0x80) {
                    rdp_ptr_pixel(&v, x, y, px & 0xFFFFFF);
                }
            }
        }
    }                                   /* else hidden: 0 x 0 */
    memcpy(p, &v, MIN(cb, sizeof(v)));
    return 1;
}

/* ---- the core: input ---- */

/* What the client that had the console still held goes up. */
static void rdp_release_input(RdpServer *rs)
{
    qkbd_state_lift_all_keys(rs->kbd);
    if (rs->buttons != 0) {
        qemu_input_update_buttons(rs->dcl.con, rdp_bmap, rs->buttons, 0);
        qemu_input_event_sync();
        rs->buttons = 0;
    }
    rs->e1 = E1_NONE;
    rs->last_valid = false;
}

static void rdp_key(RdpServer *rs, unsigned int lnx, bool down)
{
    qkbd_state_key_event(rs->kbd, lnx, down);
}

static void __cdecl rc_key(void *core, void *session, unsigned int sc,
                             unsigned int prefix, int down)
{
    RdpServer *rs = core;
    RdpCoreSession *cs = session;
    unsigned int i, lnx;

    if (!cs->ready || cs->view_only) {
        return;
    }
    rdp_refresh_soon(rs);

    /* Pause: E1 1D, 45 down, then E1 1D, 45 up. A lone E1 1D is a Ctrl. */
    if (rs->e1 == E1_HELD) {
        rs->e1 = E1_NONE;
        if (prefix == 0 && sc == 0x45 && down) {
            rdp_key(rs, KEY_PAUSE, true);
            rs->e1 = E1_PAUSE;
            return;
        }
        rdp_key(rs, KEY_LEFTCTRL, true);
    } else if (rs->e1 == E1_PAUSE) {
        if (prefix == 0xE1 && sc == 0x1D && !down) {
            return;
        }
        rs->e1 = E1_NONE;
        if (prefix == 0 && sc == 0x45 && !down) {
            rdp_key(rs, KEY_PAUSE, false);
            return;
        }
    }
    if (prefix == 0xE1) {
        if (sc == 0x1D && down) {
            rs->e1 = E1_HELD;
        } else if (sc == 0x1D) {
            rdp_key(rs, KEY_LEFTCTRL, false);
        }
        return;
    }

    i = (prefix == 0xE0 ? 0xE000 : 0) | (sc & 0x7F);
    lnx = i < qemu_input_map_atset1_to_linux_len
        ? qemu_input_map_atset1_to_linux[i] : 0;
    if (lnx != 0) {
        rdp_key(rs, lnx, down);
    }
}

/*
 * The client's lock keys (its Synchronize event). Only when the guest
 * has shown its LEDs: else a guest without them would get its locks
 * toggled at each sync.
 */
static void __cdecl rc_key_locks(void *core, void *session,
                                   unsigned int locks)
{
    static const struct {
        unsigned int lock;
        uint32_t led;
        unsigned int key;
    } lk[] = {
        { CORE_LOCK_CAPS,   QEMU_CAPS_LOCK_LED,   KEY_CAPSLOCK },
        { CORE_LOCK_NUM,    QEMU_NUM_LOCK_LED,    KEY_NUMLOCK },
        { CORE_LOCK_SCROLL, QEMU_SCROLL_LOCK_LED, KEY_SCROLLLOCK },
    };
    RdpServer *rs = core;
    RdpCoreSession *cs = session;
    uint32_t leds;
    int i;

    if (!cs->ready || cs->view_only || !rs->leds_known) {
        return;
    }
    leds = qemu_input_get_leds_mask(rs->dcl.con);
    for (i = 0; i < ARRAY_SIZE(lk); i++) {
        if (!(locks & lk[i].lock) != !(leds & lk[i].led)) {
            rdp_key(rs, lk[i].key, true);
            rdp_key(rs, lk[i].key, false);
        }
    }
}

static void __cdecl rc_mouse(void *core, void *session, int x, int y,
                               unsigned int buttons, int wheel)
{
    RdpServer *rs = core;
    RdpCoreSession *cs = session;
    QemuConsole *con = rs->dcl.con;
    InputButton b;
    int i, n;

    if (!cs->ready || cs->view_only || rs->server == NULL) {
        return;
    }
    rdp_refresh_soon(rs);
    buttons &= CORE_BUTTON_LEFT | CORE_BUTTON_RIGHT | CORE_BUTTON_MIDDLE;
    if (buttons != rs->buttons) {
        qemu_input_update_buttons(con, rdp_bmap, rs->buttons, buttons);
        rs->buttons = buttons;
    }
    if (wheel != 0) {                   /* 120 a notch, + is away */
        b = wheel > 0 ? INPUT_BUTTON_WHEEL_UP : INPUT_BUTTON_WHEEL_DOWN;
        n = MIN(MAX(abs(wheel) / 120, 1), RDP_WHEEL_MAX);
        for (i = 0; i < n; i++) {
            qemu_input_queue_btn(con, b, true);
            qemu_input_event_sync();
            qemu_input_queue_btn(con, b, false);
            qemu_input_event_sync();
        }
        return;
    }
    if (qemu_input_is_absolute(con)) {
        qemu_input_queue_abs(con, INPUT_AXIS_X, x, 0,
                             pixman_image_get_width(rs->server));
        qemu_input_queue_abs(con, INPUT_AXIS_Y, y, 0,
                             pixman_image_get_height(rs->server));
    } else if (rs->last_valid) {
        qemu_input_queue_rel(con, INPUT_AXIS_X, x - rs->last_x);
        qemu_input_queue_rel(con, INPUT_AXIS_Y, y - rs->last_y);
    }
    rs->last_x = x;
    rs->last_y = y;
    rs->last_valid = true;
    qemu_input_event_sync();
}

/* Ctrl+Alt+Del. The protocol code sends these keys as they come. */
static void __cdecl rc_sas(void *core, void *session)
{
    RdpServer *rs = core;
    RdpCoreSession *cs = session;

    if (!cs->ready || cs->view_only) {
        return;
    }
    rdp_key(rs, KEY_LEFTCTRL, true);
    rdp_key(rs, KEY_LEFTALT, true);
    rdp_key(rs, KEY_DELETE, true);
    rdp_key(rs, KEY_DELETE, false);
    rdp_key(rs, KEY_LEFTALT, false);
    rdp_key(rs, KEY_LEFTCTRL, false);
}

static void rdp_leds(Notifier *n, void *data)
{
    RdpServer *rs = container_of(n, RdpServer, led_notifier);

    rs->leds_known = true;
}

/* ---- the core: sessions, the frame, the rest ---- */

static void *__cdecl rc_session_open(void *core, const FRONTEND_API *fe,
                                       void *fe_ctx, const char *peer,
                                       unsigned int flags)
{
    RdpServer *rs = core;
    RdpCoreSession *cs;

    if (QTAILQ_EMPTY(&rs->sessions)) {
        /* Nobody looked meanwhile: our copy is old */
        qemu_console_hw_update(rs->dcl.con);
        if (rs->guest != NULL && rs->server != NULL) {
            rdp_copy_all(rs);
        }
        qemu_console_listener_set_refresh(&rs->dcl, RDP_REFRESH_BASE);
    }
    if (rs->server == NULL) {
        return NULL;
    }
    cs = g_new0(RdpCoreSession, 1);
    cs->fe = fe;
    cs->fe_ctx = fe_ctx;
    cs->view_only = (flags & CORE_VIEW_ONLY) != 0;
    QTAILQ_INSERT_TAIL(&rs->sessions, cs, next);
    return cs;
}

static void __cdecl rc_session_close(void *core, void *session)
{
    RdpServer *rs = core;
    RdpCoreSession *cs = session;

    if (cs->ready) {
        rdp_release_input(rs);
    }
    QTAILQ_REMOVE(&rs->sessions, cs, next);
    g_free(cs);
}

static void __cdecl rc_view_only(void *core, void *session, int on)
{
    ((RdpCoreSession *)session)->view_only = on != 0;
}

/* The client takes the console: the one that had it goes. */
static int __cdecl rc_session_ready(void *core, void *session)
{
    RdpServer *rs = core;
    RdpCoreSession *cs = session, *o;

    QTAILQ_FOREACH(o, &rs->sessions, next) {
        if (o != cs && o->ready) {
            o->ready = false;
            rdp_release_input(rs);
            o->fe->Stop(o->fe_ctx, CORE_STOP_REPLACED);
        }
    }
    cs->ready = true;
    rs->last_valid = false;
    return 0;
}

static int __cdecl rc_frame_lock(void *core, CORE_FRAME *f)
{
    RdpServer *rs = core;
    unsigned long cb = f->cb;
    CORE_FRAME v;

    if (rs->server == NULL) {
        return 0;
    }
    memset(&v, 0, sizeof(v));
    v.cb = cb;
    v.width = pixman_image_get_width(rs->server);
    v.height = pixman_image_get_height(rs->server);
    v.bpp = 32;
    v.red_mask = 0xFF0000;
    v.green_mask = 0x00FF00;
    v.blue_mask = 0x0000FF;
    v.bits = (const unsigned char *)pixman_image_get_data(rs->server);
    v.stride = pixman_image_get_stride(rs->server);
    memcpy(f, &v, MIN(cb, sizeof(v)));
    return 1;
}

static void __cdecl rc_frame_unlock(void *core)
{
}

static void __cdecl rc_log(void *core, int level, const char *msg)
{
    log_msg(level, "%s", msg);
}

/* No drawing orders: the screen goes as bitmaps. */
static int __cdecl rc_orders_get(void *core, unsigned long *next,
                                   CORE_ORDER *out, int max)
{
    return 0;
}

static int __cdecl rc_text_get(void *core, const CORE_ORDER *o,
                                 CORE_GLYPHREF *refs)
{
    return 0;
}

static const CORE_GLYPH *__cdecl rc_glyph_get(void *core, unsigned int id,
                                                unsigned long serial)
{
    return NULL;
}

static const CORE_BITMAP *__cdecl rc_bitmap_get(void *core,
                                                  unsigned int id,
                                                  unsigned long serial)
{
    return NULL;
}

static const CORE_API rdp_core_api = {
    sizeof(CORE_API), CORE_VERSION,
    rc_session_open, rc_session_close, rc_view_only,
    rc_frame_lock, rc_frame_unlock,
    rc_pointer_get,
    rc_key, rc_key_locks, rc_mouse, rc_sas,
    rc_log,
    rc_session_ready,
    rc_orders_get,
    rc_text_get, rc_glyph_get,
    rc_bitmap_get,
};

static const DisplayChangeListenerOps rdp_dcl_ops = {
    .dpy_name             = "rdp",
    .dpy_refresh          = rdp_refresh,
    .dpy_gfx_update       = rdp_gfx_update,
    .dpy_gfx_switch       = rdp_gfx_switch,
    .dpy_gfx_check_format = qemu_pixman_check_format,
    .dpy_mouse_set        = rdp_mouse_set,
    .dpy_cursor_define    = rdp_cursor_define,
};

/* ---- the connections (ntrdps's session.c) ---- */

static void rdp_conn_after(RdpConn *c);

/* The plaintext Connection Confirm is out: TLS from here on. */
static void rdp_start_tls(SESSION *s)
{
    s->tls = rdp_tls_start(s->conn->ioc);
    if (s->tls == NULL) {
        session_end(s, 0, "TLS: cannot start the server");
        return;
    }
    s->want_tls = 0;
    log_msg(LOG_INFO, "session %s: TLS 1.2 starting", s->peer);
}

/* Sends from the queue until it is empty or the socket takes no more. */
static void rdp_flush(SESSION *s)
{
    Error *err = NULL;
    ssize_t n;

    while (s->tx_pos < s->tx_len) {
        if (s->tls != NULL) {
            n = rdp_tls_write(s->tls, s->tx + s->tx_pos,
                              (int)(s->tx_len - s->tx_pos));
            if (n <= 0) {
                if (n == -1) {
                    session_end(s, 0, "TLS send failed: %s",
                                rdp_tls_error(s->tls));
                }
                return;                 /* RDP_TLS_WOULDBLOCK: wait */
            }
        } else {
            n = qio_channel_write(s->conn->ioc,
                                  (const char *)s->tx + s->tx_pos,
                                  s->tx_len - s->tx_pos, &err);
            if (n == QIO_CHANNEL_ERR_BLOCK) {
                return;
            }
            if (n < 0) {
                session_end(s, 0, "send failed: %s", error_get_pretty(err));
                error_free(err);
                return;
            }
        }
        s->tx_pos += n;
        s->tx_bytes += n;
    }
    s->tx_pos = 0;
    s->tx_len = 0;
    if (s->want_tls) {
        rdp_start_tls(s);
    } else if (s->tls != NULL && rdp_tls_flush(s->tls) == -1) {
        session_end(s, 0, "TLS flush failed: %s", rdp_tls_error(s->tls));
    }
}

int session_send(SESSION *s, const unsigned char *pkt, unsigned int len)
{
    if (s->ending == END_NOW) {
        return 0;
    }
    if (s->tx_len + len > SESS_TX_MAX && s->tx_pos > 0) {
        memmove(s->tx, s->tx + s->tx_pos, s->tx_len - s->tx_pos);
        s->tx_len -= s->tx_pos;
        s->tx_pos = 0;
    }
    if (s->tx_len + len > SESS_TX_MAX) {
        session_end(s, 0, "send queue full (%u bytes queued, %u more)",
                    s->tx_len, len);
        return 0;
    }
    log_hex("out", pkt, len);
    memcpy(s->tx + s->tx_len, pkt, len);
    s->tx_len += len;
    s->tx_packets++;
    rdp_flush(s);
    return 1;
}

void session_wake(SESSION *s)
{
    if (s->conn != NULL) {
        qemu_bh_schedule(s->conn->bh);
    }
}

/*
 * update_pump asks this every 20 ms while it sends. The pump stops; the
 * bottom half goes on with it after the main loop had its turn: the
 * guest, other clients, this client's input.
 */
int session_input_waiting(SESSION *s)
{
    session_wake(s);
    return 1;
}

/* Reads what the socket has, then hands over each whole packet. */
static void rdp_receive(SESSION *s)
{
    Error *err = NULL;
    unsigned int pos, len;
    ssize_t n;
    int kind;

    if (s->rx_len >= SESS_RX_MAX) {
        session_end(s, 0, "protocol error: %u bytes and no whole packet",
                    s->rx_len);
        return;
    }
    if (s->tls != NULL) {
        n = rdp_tls_read(s->tls, s->rx + s->rx_len,
                         (int)(SESS_RX_MAX - s->rx_len));
        if (n == RDP_TLS_WOULDBLOCK) {
            return;
        }
        if (n < 0) {
            session_end(s, 0, "TLS receive failed: %s",
                        rdp_tls_error(s->tls));
            return;
        }
    } else {
        n = qio_channel_read(s->conn->ioc, (char *)s->rx + s->rx_len,
                             SESS_RX_MAX - s->rx_len, &err);
        if (n == QIO_CHANNEL_ERR_BLOCK) {
            return;
        }
        if (n < 0) {
            session_end(s, 0, "recv failed: %s", error_get_pretty(err));
            error_free(err);
            return;
        }
    }
    if (n == 0) {
        if (s->rx_len > 0) {
            session_end(s, 0, "the client closed the connection in the "
                        "middle of a packet (%u bytes of it)", s->rx_len);
        } else {
            session_end(s, 0, "the client closed the connection");
        }
        return;
    }
    s->rx_len += n;
    s->rx_bytes += n;

    pos = 0;
    while (s->ending == END_NO) {
        kind = frame_length(s->rx + pos, s->rx_len - pos, &len);
        if (kind == FRAME_ERROR) {
            log_hex("in, not a packet", s->rx + pos,
                    MIN(s->rx_len - pos, 64));
            session_end(s, 0, "protocol error: the bytes are no TPKT and no "
                        "fast-path packet (first byte 0x%02x)",
                        (unsigned)s->rx[pos]);
            break;
        }
        if (kind == FRAME_MORE || len > s->rx_len - pos) {
            break;
        }
        s->rx_packets++;
        rdp_packet(s, kind, s->rx + pos, len);   /* logs it ("in") */
        pos += len;
    }
    if (pos > 0) {
        memmove(s->rx, s->rx + pos, s->rx_len - pos);
        s->rx_len -= pos;
    }
}

/* The session is over: its totals, then our FIN; the socket drains. */
static void rdp_conn_finish(RdpConn *c)
{
    SESSION *s = c->s;

    log_msg(LOG_INFO, "session %s: closed: %s. In: %lu bytes, %lu packet%s"
            " (%lu fast-path, %lu input events). Out: %lu bytes, %lu "
            "packet%s. %lu s, state %s.",
            s->peer, s->why, s->rx_bytes, s->rx_packets,
            s->rx_packets == 1 ? "" : "s", s->fastpath, s->in_events,
            s->tx_bytes, s->tx_packets, s->tx_packets == 1 ? "" : "s",
            (unsigned long)(GetTickCount() - s->start_ms) / 1000,
            rdp_state_name(s->state));
    rdp_done(s);                        /* after it, the core calls no more */
    if (s->tls != NULL) {
        rdp_tls_free(s->tls);
    }
    g_free(s);
    c->s = NULL;

    /*
     * Our FIN goes after the data. Closing a socket that has unread input
     * sends a RST, and a RST can lose our last packets.
     */
    qio_channel_shutdown(c->ioc, QIO_CHANNEL_SHUTDOWN_WRITE, NULL);
    c->drain_end = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + RDP_CLOSE_DRAIN_MS;
}

/* After the end: read and drop until the client closes too. */
static void rdp_conn_drain(RdpConn *c)
{
    char junk[512];
    ssize_t n;
    int i;

    for (i = 0; i < 64; i++) {
        n = qio_channel_read(c->ioc, junk, sizeof(junk), NULL);
        if (n == QIO_CHANNEL_ERR_BLOCK) {
            break;
        }
        if (n <= 0) {
            c->closed = true;
            return;
        }
    }
    if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= c->drain_end) {
        c->closed = true;
    }
}

static void rdp_conn_free(RdpConn *c)
{
    if (c->watch != 0) {
        g_source_remove(c->watch);
    }
    timer_free(c->timer);
    qemu_bh_delete(c->bh);
    qio_channel_close(c->ioc, NULL);
    object_unref(OBJECT(c->sioc));
    QTAILQ_REMOVE(&c->rs->conns, c, next);
    g_free(c);
}

/*
 * One turn, as in ntrdps's loop: the timeouts, send, receive, then the
 * pump (what the core reported, the pointer, the screen).
 */
static void rdp_conn_turn(RdpConn *c, bool readable)
{
    SESSION *s = c->s;

    if (s == NULL) {
        rdp_conn_drain(c);
    } else if (s->ending != END_NOW) {
        rdp_check_time(s, GetTickCount());
        rdp_flush(s);
        if (s->ending == END_NO &&
            (readable || (s->tls != NULL && rdp_tls_pending(s->tls)))) {
            rdp_receive(s);
        }
        rdp_pump(s);
    }
    rdp_conn_after(c);
}

static gboolean rdp_conn_io(QIOChannel *ioc, GIOCondition cond,
                            gpointer opaque)
{
    RdpConn *c = opaque;

    c->watch = 0;                       /* rdp_conn_after makes the next */
    rdp_conn_turn(c, cond & (G_IO_IN | G_IO_HUP | G_IO_ERR));
    return G_SOURCE_REMOVE;
}

static void rdp_conn_watch(RdpConn *c, GIOCondition want)
{
    want |= G_IO_HUP | G_IO_ERR;
    if (c->watch != 0) {
        if (c->watch_cond == want) {
            return;
        }
        g_source_remove(c->watch);
    }
    c->watch_cond = want;
    c->watch = qio_channel_add_watch(c->ioc, want, rdp_conn_io, c, NULL);
}

static void rdp_conn_bh(void *opaque)
{
    rdp_conn_turn(opaque, false);
}

static void rdp_conn_tick(void *opaque)
{
    rdp_conn_turn(opaque, false);
}

/* After a turn: the end, and what to wait for. c may be gone after it. */
static void rdp_conn_after(RdpConn *c)
{
    SESSION *s = c->s;
    GIOCondition want = 0;
    int64_t now;

    if (s != NULL) {
        if (s->ending == END_NOW) {
            rdp_conn_finish(c);
        } else if (s->ending == END_FLUSH) {
            if (SESSION_QUEUED(s) == 0 &&
                (s->tls == NULL || !rdp_tls_unsent(s->tls))) {
                rdp_conn_finish(c);
            } else if (GetTickCount() - s->end_ms > RDP_FLUSH_MAX_MS) {
                log_msg(LOG_WARN, "session %s: %u bytes not sent within "
                        "%d s", s->peer, SESSION_QUEUED(s),
                        RDP_FLUSH_MAX_MS / 1000);
                rdp_conn_finish(c);
            }
        }
    }
    if (c->closed) {
        rdp_conn_free(c);
        return;
    }

    now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    s = c->s;
    if (s == NULL) {
        rdp_conn_watch(c, G_IO_IN);
        timer_mod(c->timer, MAX(c->drain_end, now));
        return;
    }
    if (s->ending == END_NO) {
        want |= G_IO_IN;
    }
    if (SESSION_QUEUED(s) > 0) {
        want |= G_IO_OUT;
    }
    if (s->tls != NULL) {
        want |= rdp_tls_events(s->tls) & (s->ending == END_NO
                                          ? G_IO_IN | G_IO_OUT : G_IO_OUT);
        if (rdp_tls_unsent(s->tls)) {
            want |= G_IO_OUT;
        }
        if (s->ending == END_NO && rdp_tls_pending(s->tls)) {
            qemu_bh_schedule(c->bh);    /* no socket event comes for it */
        }
    }
    rdp_conn_watch(c, want);
    timer_mod(c->timer, now + RDP_TURN_MS);
}

/* "a.b.c.d:port" or "[v6]:port", for the log. */
static void rdp_peer_name(QIOChannelSocket *sioc, char *out, size_t len)
{
    SocketAddress *a = qio_channel_socket_get_remote_address(sioc, NULL);

    if (a != NULL && a->type == SOCKET_ADDRESS_TYPE_INET) {
        snprintf(out, len, strchr(a->u.inet.host, ':') ? "[%s]:%s" : "%s:%s",
                 a->u.inet.host, a->u.inet.port);
    } else {
        snprintf(out, len, "?");
    }
    qapi_free_SocketAddress(a);
}

static void rdp_accept(QIONetListener *listener, QIOChannelSocket *sioc,
                       gpointer opaque)
{
    RdpServer *rs = opaque;
    char peer[NET_PEER_MAX];
    RdpConn *c;
    SESSION *s;
    int on = 1, n = 0;

    /* At most RDP_CONN_MAX, those that drain after their end too */
    QTAILQ_FOREACH(c, &rs->conns, next) {
        n++;
    }
    if (n >= RDP_CONN_MAX) {
        rdp_peer_name(sioc, peer, sizeof(peer));
        log_msg(LOG_WARN, "%s closed at once: %d connections are open",
                peer, RDP_CONN_MAX);
        qio_channel_close(QIO_CHANNEL(sioc), NULL);
        return;                         /* the listener drops sioc */
    }

    c = g_new0(RdpConn, 1);
    c->rs = rs;
    c->sioc = sioc;
    object_ref(OBJECT(sioc));
    c->ioc = QIO_CHANNEL(sioc);
    qio_channel_set_name(c->ioc, "rdp-server");
    qio_channel_set_blocking(c->ioc, false, NULL);
    qio_channel_set_delay(c->ioc, false);
    /*
     * A client that vanished (power off, cable) is found when we send, or
     * else by TCP keep-alive.
     */
    setsockopt(sioc->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    c->bh = qemu_bh_new(rdp_conn_bh, c);
    c->timer = timer_new_ms(QEMU_CLOCK_REALTIME, rdp_conn_tick, c);
    QTAILQ_INSERT_TAIL(&rs->conns, c, next);

    s = g_new0(SESSION, 1);
    s->conn = c;
    rdp_peer_name(sioc, s->peer, sizeof(s->peer));
    s->start_ms = GetTickCount();
    c->s = s;
    rdp_init(s);
    log_msg(LOG_INFO, "session %s: start", s->peer);
    rdp_conn_after(c);
}

/* ---- options ---- */

static QemuOptsList qemu_rdp_opts = {
    .name = "rdp",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_rdp_opts.head),
    .implied_opt_name = "addr",
    .merge_lists = true,
    .desc = {
        {
            .name = "addr",
            .type = QEMU_OPT_STRING,
        }, {
            .name = "tls",
            .type = QEMU_OPT_BOOL,
        }, {
            .name = "tls-key",
            .type = QEMU_OPT_STRING,
        }, {
            .name = "password-secret",
            .type = QEMU_OPT_STRING,
        }, {
            .name = "plain-login",
            .type = QEMU_OPT_BOOL,
        }, {
            .name = "display",
            .type = QEMU_OPT_STRING,
        }, {
            .name = "head",
            .type = QEMU_OPT_NUMBER,
        },
        { /* end of list */ }
    },
};

static void rdp_register_config(void)
{
    qemu_add_opts(&qemu_rdp_opts);
}
opts_init(rdp_register_config);

void rdp_parse(const char *str)
{
    if (!qemu_opts_parse_noisily(qemu_find_opts("rdp"), str, true)) {
        exit(1);
    }
}

int rdp_init_func(void *opaque, QemuOpts *opts, Error **errp)
{
    const char *addr = qemu_opt_get(opts, "addr");
    const char *key = qemu_opt_get(opts, "tls-key");
    const char *secret = qemu_opt_get(opts, "password-secret");
    const char *dev = qemu_opt_get(opts, "display");
    bool tls = qemu_opt_get_bool(opts, "tls", true);
    bool plain = qemu_opt_get_bool(opts, "plain-login", true);
    g_autofree char *port = NULL;
    char *pw = NULL;
    SocketAddress *saddr = NULL;
    QemuConsole *con;
    RdpServer *rs;
    int ret = -1;

    if (rdp_server != NULL) {
        error_setg(errp, "rdp: only one RDP server");
        return -1;
    }
    if (addr == NULL || *addr == '\0') {
        error_setg(errp, "rdp: give an address, such as localhost:3389");
        return -1;
    }
    if (key != NULL && !tls) {
        error_setg(errp, "rdp: tls-key needs tls=on");
        return -1;
    }
    if (!plain && secret == NULL) {
        error_setg(errp, "rdp: plain-login=off needs password-secret");
        return -1;
    }
    if (!plain && !tls) {
        error_setg(errp, "rdp: plain-login=off needs tls=on");
        return -1;
    }

    if (strchr(addr, ':') == NULL) {
        addr = port = g_strdup_printf(":%s", addr);     /* a port alone */
    }
    saddr = g_new0(SocketAddress, 1);
    saddr->type = SOCKET_ADDRESS_TYPE_INET;
    if (inet_parse(&saddr->u.inet, addr, errp) < 0) {
        goto out;
    }

    if (secret != NULL) {
        pw = qcrypto_secret_lookup_as_utf8(secret, errp);
        if (pw == NULL) {
            goto out;
        }
        if (*pw == '\0') {
            error_setg(errp, "rdp: the password of secret '%s' is empty",
                       secret);
            goto out;
        }
    }
    if (tls && !rdp_tls_setup(key, errp)) {
        goto out;
    }

    if (dev != NULL) {
        con = qemu_console_lookup_by_device_name(
            dev, qemu_opt_get_number(opts, "head", 0), errp);
        if (con == NULL) {
            goto out;
        }
    } else {
        con = qemu_console_lookup_default();
    }

    rs = g_new0(RdpServer, 1);
    QTAILQ_INIT(&rs->sessions);
    QTAILQ_INIT(&rs->conns);
    rs->cursor_on = true;
    rs->ptr_serial = 1;
    rs->listener = qio_net_listener_new();
    qio_net_listener_set_name(rs->listener, "rdp-listen");
    if (qio_net_listener_open_sync(rs->listener, saddr, 1, errp) < 0) {
        object_unref(OBJECT(rs->listener));
        g_free(rs);
        goto out;
    }
    qemu_console_register_listener(con, &rs->dcl, &rdp_dcl_ops);
    rs->kbd = qkbd_state_init(rs->dcl.con);
    qkbd_state_set_delay(rs->kbd, RDP_KEY_DELAY_MS);
    rs->led_notifier.notify = rdp_leds;
    qemu_input_led_notifier_add(&rs->led_notifier);

    rdp_set_allow_tls(tls);
    rdp_set_login(pw != NULL, plain);
    rdp_login_set_password(pw);
    rdp_start(&rdp_core_api, rs);
    rdp_server = rs;
    qio_net_listener_set_client_func(rs->listener, rdp_accept, rs, NULL);
    trace_rdp_listen(addr, tls, pw != NULL,
                     tls ? rdp_tls_fingerprint() : "-");
    ret = 0;

out:
    if (pw != NULL) {
        memset(pw, 0, strlen(pw));
        g_free(pw);
    }
    qapi_free_SocketAddress(saddr);
    return ret;
}
