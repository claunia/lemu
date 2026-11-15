/*
 * RDP server: the password (ntrdps's setting Login), and the box that
 * asks for it.
 *
 * With a password (-rdp ...,password-secret=ID), a client gets the
 * guest's screen only after it gave the right one: the password of its
 * Client Info when it asks to log on with it (a password saved in the
 * client), else what the user types into a box that this file draws.
 * There is one password and no user name, as with VNC.
 *
 * The box is a small core of its own (a CORE_API table). While it shows,
 * s->core is this table, so the screen updates, the pointer and the
 * input of the session go to the box, never to the guest. Its frame has
 * the size and format of the guest's screen, so after the login the
 * session goes on in the same share with the real screen (rdp_console).
 * The box knows the US keyboard layout only.
 *
 * Without TLS the password crosses the network in clear: plain-login=off
 * refuses such clients (ntrdps's LoginPlain 0).
 *
 * Parts come from ntrdps's login.c.
 * Copyright (c) 2026 ntrdps authors.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "rdp-int.h"

#include "rdp/session.h"
#include "rdp/lfont.h"
#include "rdp/log.h"
#include "rdp/login.h"
#include "rdp/rdp.h"
#include "rdp/sec.h"
#include "rdp/share.h"
#include "rdp/update.h"

/* The box, from its top left corner. Rows are the tops of text lines. */
#define BOX_W    460
#define EDGE     3              /* its frame */
#define TITLE_H  22
#define MARGIN   16
#define FIELD_X  (MARGIN + 12 * LFONT_W)
#define FIELD_W  (BOX_W - FIELD_X - MARGIN)
#define FIELD_H  (LFONT_H + 8)
#define FIELD_IN 4              /* the text inside the field */
#define BTN_W    90
#define BTN_H    (LFONT_H + 12)
#define ROW_INFO (EDGE + TITLE_H + 14)
#define ROW_PW   (ROW_INFO + 34)
#define ROW_MSG  (ROW_PW + 36)
#define ROW_LAST (ROW_MSG + 34)
#define BOX_H    (ROW_LAST + LFONT_H + 16 + EDGE)

/* After a wrong password, the next check waits this long (any client). */
#define LOGIN_PAUSE_MS 1000

/* The colors (rgb below). */
#define C_DESK   0
#define C_FACE   1
#define C_WHITE  2
#define C_SHADOW 3
#define C_BLACK  4
#define C_TITLE  5
#define C_RED    6
#define COLORS   7

static const unsigned char rgb[COLORS][3] = {
    { 0, 128, 128 },            /* the desktop */
    { 192, 192, 192 },          /* the box */
    { 255, 255, 255 },
    { 128, 128, 128 },
    { 0, 0, 0 },
    { 0, 0, 128 },              /* the title bar */
    { 128, 0, 0 }               /* errors */
};

/* Modifier keys held (LOGIN.mods). */
#define M_LSHIFT 0x01
#define M_RSHIFT 0x02
#define M_LCTRL  0x04
#define M_RCTRL  0x08
#define M_LALT   0x10
#define M_RALT   0x20
#define M_SHIFT  (M_LSHIFT | M_RSHIFT)

/* The US layout: the characters of scancodes 0x00 - 0x39. */
static const char us_plain[0x3A] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};
static const char us_shift[0x3A] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

typedef struct LOGIN_s {
    SESSION        *s;
    const CORE_API *real;           /* the core, while the box shows */
    void           *real_ctx;
    CORE_FRAME      f;              /* the box's frame: the console's size
                                     * and format */
    unsigned char  *bits;
    unsigned char   pal[256][3];    /* 8 bpp: the console's palette */
    int             pb;             /* bytes a pixel */
    unsigned long   col[COLORS];    /* the colors as pixel values */
    int             x0, y0;         /* the box's top left corner */
    char            title[64];
    WCHAR           pw[AUTH_PW_MAX + 1];
    unsigned int    len;
    char            msg[64];        /* the message line */
    int             msg_color;
    int             tries;          /* wrong tries so far */
    DWORD           start_ms;       /* when the box came */
    int             check_due;      /* 1: check once the screen went out */
    DWORD           check_ms;
    unsigned int    locks;          /* CORE_LOCK_*: the client's */
    unsigned int    mods;           /* M_* */
    int             enter_down;     /* Enter went down in the box */
    unsigned int    buttons;        /* CORE_BUTTON_* held */
    int             press;          /* the left button went down on the
                                     * button "Log on" */
} LOGIN;

static gunichar2 *rdp_pw;           /* UTF-16, as clients send it */
static glong rdp_pw_len;
static int64_t rdp_pause_until;     /* QEMU_CLOCK_REALTIME ms */

void rdp_login_set_password(const char *password)
{
    if (rdp_pw != NULL) {
        memset(rdp_pw, 0, rdp_pw_len * sizeof(*rdp_pw));
        g_free(rdp_pw);
        rdp_pw = NULL;
        rdp_pw_len = 0;
    }
    if (password != NULL) {
        rdp_pw = g_utf8_to_utf16(password, -1, NULL, &rdp_pw_len, NULL);
    }
}

/* The same length and the same code units; the time does not tell
 * where the first wrong one is. */
static bool pw_right(const WCHAR *p, unsigned int len)
{
    unsigned int i, diff;

    if (rdp_pw == NULL) {
        return false;
    }
    diff = len ^ (unsigned int)rdp_pw_len;
    for (i = 0; i < len; i++) {
        diff |= p[i] ^ (i < rdp_pw_len ? rdp_pw[i] : 0);
    }
    return diff == 0;
}

static bool paused(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_REALTIME) < rdp_pause_until;
}

static void pause_start(void)
{
    rdp_pause_until = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + LOGIN_PAUSE_MS;
}

/* ---- drawing ---- */

/* The bits of mask that hold v (0 - 255): 16 bpp. */
static unsigned long scale(unsigned int v, unsigned long mask)
{
    unsigned int shift = 0, bits = 0;

    if (mask == 0) {
        return 0;
    }
    while (!(mask & 1)) {
        mask >>= 1;
        shift++;
    }
    while (mask & 1) {
        mask >>= 1;
        bits++;
    }
    if (bits > 8) {
        bits = 8;
    }
    return (unsigned long)(v >> (8 - bits)) << shift;
}

/* A color as a pixel value of the frame: 8 bpp, the nearest color of
 * the palette; 24 and 32 bpp: blue, green, red bytes (update.c). */
static unsigned long pixel(const LOGIN *L, const unsigned char *c)
{
    unsigned long d, best = 0, bd = 0xFFFFFFFFUL;
    long dr, dg, db;
    unsigned int i;

    switch (L->f.bpp) {
    case 8:
        for (i = 0; i < 256; i++) {
            dr = (long)L->pal[i][0] - c[0];
            dg = (long)L->pal[i][1] - c[1];
            db = (long)L->pal[i][2] - c[2];
            d = (unsigned long)(dr * dr + dg * dg + db * db);
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        return best;
    case 16:
        return scale(c[0], L->f.red_mask) | scale(c[1], L->f.green_mask) |
               scale(c[2], L->f.blue_mask);
    }
    return (unsigned long)c[2] | ((unsigned long)c[1] << 8) |
           ((unsigned long)c[0] << 16);
}

/* The rectangle (x0, y0) - (x1, y1), right and bottom exclusive, cut to
 * the frame, in pixel value v. */
static void fill_px(LOGIN *L, int x0, int y0, int x1, int y1,
                    unsigned long v)
{
    unsigned char *p;
    int x, y, b = L->pb;

    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    x1 = MIN(x1, (int)L->f.width);
    y1 = MIN(y1, (int)L->f.height);
    for (y = y0; y < y1; y++) {
        p = L->bits + (long)y * L->f.stride + (long)x0 * b;
        for (x = x0; x < x1; x++, p += b) {
            p[0] = (unsigned char)v;
            if (b > 1) {
                p[1] = (unsigned char)(v >> 8);
            }
            if (b > 2) {
                p[2] = (unsigned char)(v >> 16);
            }
            if (b > 3) {
                p[3] = 0;
            }
        }
    }
}

static void fill(LOGIN *L, int x0, int y0, int x1, int y1, int color)
{
    fill_px(L, x0, y0, x1, y1, L->col[color]);
}

/* One character; (x, y) is the top left corner of its cell. */
static void glyph(LOGIN *L, int x, int y, unsigned int ch, int color)
{
    const unsigned short *g;
    int r, c;

    if (ch < LFONT_FIRST || ch >= LFONT_FIRST + LFONT_CHARS) {
        ch = '?';
    }
    g = lfont_bits[ch - LFONT_FIRST];
    for (r = 0; r < LFONT_H; r++) {
        for (c = 0; c < LFONT_W; c++) {
            if (g[r] & (0x8000U >> c)) {
                fill(L, x + c, y + r, x + c + 1, y + r + 1, color);
            }
        }
    }
}

/* ISO 8859-1 text, at most room pixels wide. */
static void text(LOGIN *L, int x, int y, const char *t, int color, int room)
{
    for (; *t != '\0' && room >= LFONT_W; t++) {
        glyph(L, x, y, (unsigned char)*t, color);
        x += LFONT_W;
        room -= LFONT_W;
    }
}

/* The area goes out (update.c); before the share is active, the whole
 * frame goes out anyway. */
static void damage(LOGIN *L, int x0, int y0, int x1, int y1)
{
    update_area(L->s, x0, y0, x1, y1);
}

/* A raised (in = 0) or sunken (in = 1) frame two pixels wide. */
static void edge(LOGIN *L, int x, int y, int w, int h, int in)
{
    int tl_out = in ? C_SHADOW : C_FACE, tl_in = in ? C_BLACK : C_WHITE;
    int br_out = in ? C_WHITE : C_BLACK, br_in = in ? C_FACE : C_SHADOW;

    fill(L, x, y, x + w, y + 1, tl_out);
    fill(L, x, y, x + 1, y + h, tl_out);
    fill(L, x, y + h - 1, x + w, y + h, br_out);
    fill(L, x + w - 1, y, x + w, y + h, br_out);
    fill(L, x + 1, y + 1, x + w - 1, y + 2, tl_in);
    fill(L, x + 1, y + 1, x + 2, y + h - 1, tl_in);
    fill(L, x + 1, y + h - 2, x + w - 1, y + h - 1, br_in);
    fill(L, x + w - 2, y + 1, x + w - 1, y + h - 1, br_in);
}

/* The password field: a star for each character, the end shows. */
static void draw_field(LOGIN *L)
{
    int x = L->x0 + FIELD_X, y = L->y0 + ROW_PW - FIELD_IN;
    unsigned int vis = (FIELD_W - 2 * FIELD_IN) / LFONT_W - 1;
    unsigned int first = L->len > vis ? L->len - vis : 0, k;
    int cx;

    edge(L, x, y, FIELD_W, FIELD_H, 1);
    fill(L, x + 2, y + 2, x + FIELD_W - 2, y + FIELD_H - 2, C_WHITE);
    for (k = first; k < L->len; k++) {
        glyph(L, x + FIELD_IN + (int)(k - first) * LFONT_W, y + FIELD_IN,
              '*', C_BLACK);
    }
    if (!L->check_due) {                        /* the caret */
        cx = x + FIELD_IN + (int)(L->len - first) * LFONT_W;
        fill(L, cx, y + 3, cx + 2, y + FIELD_H - 3, C_BLACK);
    }
    damage(L, x, y, x + FIELD_W, y + FIELD_H);
}

static void draw_msg(LOGIN *L)
{
    int x = L->x0 + MARGIN, y = L->y0 + ROW_MSG;

    fill(L, x, y, L->x0 + BOX_W - MARGIN, y + LFONT_H, C_FACE);
    text(L, x, y, L->msg, L->msg_color, BOX_W - 2 * MARGIN);
    damage(L, x, y, L->x0 + BOX_W - MARGIN, y + LFONT_H);
}

static void set_msg(LOGIN *L, const char *m, int color)
{
    g_strlcpy(L->msg, m, sizeof(L->msg));
    L->msg_color = color;
    draw_msg(L);
}

static void button_rect(const LOGIN *L, RECT *r)
{
    r->left = L->x0 + BOX_W - MARGIN - BTN_W;
    r->top = L->y0 + ROW_LAST - 6;
    r->right = r->left + BTN_W;
    r->bottom = r->top + BTN_H;
}

/* All of it: the desktop, the box, its field and texts. */
static void draw_all(LOGIN *L)
{
    int x = L->x0, y = L->y0, w = BOX_W, h = BOX_H;
    RECT b;

    fill(L, 0, 0, (int)L->f.width, (int)L->f.height, C_DESK);
    fill(L, x, y, x + w, y + h, C_FACE);
    edge(L, x, y, w, h, 0);
    fill(L, x + EDGE, y + EDGE, x + w - EDGE, y + EDGE + TITLE_H, C_TITLE);
    text(L, x + EDGE + 6, y + EDGE + (TITLE_H - LFONT_H) / 2, L->title,
         C_WHITE, w - 2 * EDGE - 12);
    text(L, x + MARGIN, y + ROW_INFO, "Type the password of this computer.",
         C_BLACK, w - 2 * MARGIN);
    text(L, x + MARGIN, y + ROW_PW, "Password:", C_BLACK, FIELD_X);
    draw_field(L);
    draw_msg(L);
    text(L, x + MARGIN, y + ROW_LAST, "Enter: log on", C_BLACK,
         w - 2 * MARGIN - BTN_W);
    button_rect(L, &b);
    fill(L, b.left, b.top, b.right, b.bottom, C_BLACK);  /* the default */
    fill(L, b.left + 1, b.top + 1, b.right - 1, b.bottom - 1, C_FACE);
    edge(L, b.left + 1, b.top + 1, BTN_W - 2, BTN_H - 2, 0);
    text(L, b.left + (BTN_W - 6 * LFONT_W) / 2,
         b.top + (BTN_H - LFONT_H) / 2, "Log on", C_BLACK, BTN_W);
    damage(L, 0, 0, (int)L->f.width, (int)L->f.height);
}

/* "Log on to <the VM's -name>", in the font's characters. */
static void make_title(LOGIN *L)
{
    size_t k;
    const char *p;
    unsigned char c;

    k = g_strlcpy(L->title, qemu_name != NULL && *qemu_name != '\0'
                  ? "Log on to " : "Log on", sizeof(L->title));
    for (p = qemu_name; p != NULL && *p != '\0' &&
         k < sizeof(L->title) - 1; p++) {
        c = *p;
        if (c >= 0x80 && c < 0xC0) {
            continue;                   /* the rest of a UTF-8 character */
        }
        L->title[k++] = c >= 0x20 && c < 0x7F ? c : '?';
    }
    L->title[k] = '\0';
}

/* ---- the password field ---- */

static void add_char(LOGIN *L, char c)
{
    if (L->len < AUTH_PW_MAX) {
        L->pw[L->len++] = (unsigned char)c;
        L->pw[L->len] = 0;
        draw_field(L);
    }
}

static void clear_field(LOGIN *L)
{
    memset(L->pw, 0, sizeof(L->pw));
    L->len = 0;
    draw_field(L);
}

/* The check waits for the screen (login_pump). */
static void submit(LOGIN *L)
{
    if (L->len == 0) {
        set_msg(L, "Type the password.", C_RED);
        return;
    }
    L->check_due = 1;
    L->check_ms = GetTickCount();
    set_msg(L, "Checking ...", C_BLACK);
    draw_field(L);                      /* without the caret */
}

/* ---- keys ---- */

static unsigned int mod_bit(unsigned int sc, unsigned int prefix)
{
    if (prefix == 0) {
        switch (sc) {
        case 0x2A: return M_LSHIFT;
        case 0x36: return M_RSHIFT;
        case 0x1D: return M_LCTRL;
        case 0x38: return M_LALT;
        }
    } else if (prefix == 0xE0) {
        switch (sc) {
        case 0x1D: return M_RCTRL;
        case 0x38: return M_RALT;
        }
    }
    return 0;
}

/* A key without prefix as a character of the US layout. Ctrl or Alt
 * with a key give none. */
static void type_key(LOGIN *L, unsigned int sc)
{
    char c;

    if ((L->mods & ~M_SHIFT) != 0 || sc >= sizeof(us_plain)) {
        return;
    }
    c = (L->mods & M_SHIFT) ? us_shift[sc] : us_plain[sc];
    if ((L->locks & CORE_LOCK_CAPS) && g_ascii_isalpha(c)) {
        c = g_ascii_isupper(c) ? g_ascii_tolower(c) : g_ascii_toupper(c);
    }
    if (c != 0) {
        add_char(L, c);
    }
}

static void __cdecl l_key(void *core, void *session, unsigned int sc,
                          unsigned int prefix, int down)
{
    static const char pad[] = "789-456+1230.";      /* 0x47 - 0x53 */
    LOGIN *L = core;
    unsigned int m = mod_bit(sc, prefix);

    if (m != 0) {
        if (down) {
            L->mods |= m;
        } else {
            L->mods &= ~m;
        }
        return;
    }
    if (prefix == 0 && down && (sc == 0x3A || sc == 0x45 || sc == 0x46)) {
        L->locks ^= sc == 0x3A ? CORE_LOCK_CAPS
                  : sc == 0x45 ? CORE_LOCK_NUM : CORE_LOCK_SCROLL;
        return;
    }
    if (L->check_due || L->s->ending != END_NO) {
        return;
    }
    if (sc == 0x1C && prefix != 0xE1) {         /* Enter acts when it */
        if (down) {                             /* goes up, so its key-up */
            L->enter_down = 1;                  /* never reaches the */
        } else if (L->enter_down) {             /* guest */
            L->enter_down = 0;
            submit(L);
        }
        return;
    }
    if (!down) {
        return;
    }
    if (prefix == 0xE0) {
        if (sc == 0x35) {                       /* the keypad's / */
            add_char(L, '/');
        }
        return;
    }
    if (prefix != 0) {
        return;
    }
    switch (sc) {
    case 0x0E:                                  /* Backspace */
        if (L->len > 0) {
            L->pw[--L->len] = 0;
            draw_field(L);
        }
        return;
    case 0x01:                                  /* Esc */
        clear_field(L);
        return;
    }
    if (sc >= 0x47 && sc <= 0x53) {             /* the keypad */
        if (sc == 0x4A || sc == 0x4E ||
            ((L->locks & CORE_LOCK_NUM) && !(L->mods & M_SHIFT))) {
            add_char(L, pad[sc - 0x47]);
        }
        return;
    }
    type_key(L, sc);
}

static void __cdecl l_key_locks(void *core, void *session,
                                unsigned int locks)
{
    ((LOGIN *)core)->locks = locks;
}

/* ---- the mouse ---- */

static int inside(int x, int y, int left, int top, int w, int h)
{
    return x >= left && x < left + w && y >= top && y < top + h;
}

static void __cdecl l_mouse(void *core, void *session, int x, int y,
                            unsigned int buttons, int wheel)
{
    LOGIN *L = core;
    int was = (L->buttons & CORE_BUTTON_LEFT) != 0;
    int now = (buttons & CORE_BUTTON_LEFT) != 0;
    RECT b;

    L->buttons = buttons;
    if (L->check_due || L->s->ending != END_NO || was == now) {
        return;
    }
    button_rect(L, &b);
    if (now) {
        L->press = inside(x, y, b.left, b.top, BTN_W, BTN_H);
        return;
    }
    if (L->press && inside(x, y, b.left, b.top, BTN_W, BTN_H)) {
        submit(L);
    }
    L->press = 0;
}

/* ---- the rest of the CORE_API table ---- */

static void *__cdecl l_open(void *core, const FRONTEND_API *fe,
                            void *fe_ctx, const char *peer,
                            unsigned int flags)
{
    return NULL;
}

static void __cdecl l_close(void *core, void *session)
{
}

static void __cdecl l_view_only(void *core, void *session, int on)
{
}

static int __cdecl l_frame_lock(void *core, CORE_FRAME *f)
{
    unsigned long cb = f->cb;

    memcpy(f, &((LOGIN *)core)->f, MIN(cb, sizeof(CORE_FRAME)));
    f->cb = cb;
    return 1;
}

static void __cdecl l_frame_unlock(void *core)
{
}

/* Serial 0: the client's own arrow (pointer.c). */
static int __cdecl l_pointer_get(void *core, CORE_POINTER *p)
{
    p->serial = 0;
    return 1;
}

static void __cdecl l_sas(void *core, void *session)
{
}

static void __cdecl l_log(void *core, int level, const char *msg)
{
    log_msg(level, "%s", msg);
}

static int __cdecl l_ready(void *core, void *session)
{
    return 0;
}

/* The box has no drawing orders: all of it goes as bitmaps. */
static int __cdecl l_orders_get(void *core, unsigned long *next,
                                CORE_ORDER *out, int max)
{
    return 0;
}

static int __cdecl l_text_get(void *core, const CORE_ORDER *o,
                              CORE_GLYPHREF *refs)
{
    return 0;
}

static const CORE_GLYPH *__cdecl l_glyph_get(void *core, unsigned int id,
                                             unsigned long serial)
{
    return NULL;
}

static const CORE_BITMAP *__cdecl l_bitmap_get(void *core, unsigned int id,
                                               unsigned long serial)
{
    return NULL;
}

static const CORE_API login_api = {
    sizeof(CORE_API), CORE_VERSION,
    l_open, l_close, l_view_only,
    l_frame_lock, l_frame_unlock,
    l_pointer_get,
    l_key, l_key_locks, l_mouse, l_sas,
    l_log,
    l_ready,
    l_orders_get,
    l_text_get, l_glyph_get,
    l_bitmap_get
};

/* ---- the login ---- */

void login_free(SESSION *s)
{
    LOGIN *L = s->login;

    if (L == NULL) {
        return;
    }
    s->core = L->real;
    s->core_ctx = L->real_ctx;
    s->login = NULL;
    memset(L->pw, 0, sizeof(L->pw));
    g_free(L->bits);
    g_free(L);
}

/* The box for s, in the console's size and format. msg: its first
 * message. 0: no frame (logged). */
static int make_box(SESSION *s, const char *msg)
{
    LOGIN *L;
    CORE_FRAME f;
    int i;

    memset(&f, 0, sizeof(f));
    f.cb = sizeof(f);
    if (!s->core->FrameLock(s->core_ctx, &f)) {
        log_msg(LOG_ERROR, "session %s: login: the core has no screen",
                s->peer);
        return 0;
    }
    L = g_new0(LOGIN, 1);
    L->f = f;
    L->pb = f.bpp == 32 ? 4 : (int)f.bpp / 8;
    L->f.stride = ((long)f.width * L->pb + 3) & ~3L;
    L->bits = g_malloc0((size_t)L->f.stride * f.height);
    if (f.bpp == 8 && f.palette != NULL) {
        memcpy(L->pal, f.palette, sizeof(L->pal));
    }
    s->core->FrameUnlock(s->core_ctx);
    if (!rdp_same_mode(s, &f)) {
        rdp_take_mode(s, &f);           /* before our Demand Active */
    }
    L->f.bits = L->bits;
    L->f.palette = f.bpp == 8 ? &L->pal[0][0] : NULL;
    for (i = 0; i < COLORS; i++) {
        L->col[i] = pixel(L, rgb[i]);
    }
    L->s = s;
    L->real = s->core;
    L->real_ctx = s->core_ctx;
    L->x0 = MAX(((int)f.width - BOX_W) / 2, 0);
    L->y0 = MAX(((int)f.height - BOX_H) / 2, 0);
    make_title(L);
    g_strlcpy(L->msg, msg, sizeof(L->msg));
    L->msg_color = C_RED;
    L->start_ms = GetTickCount();
    s->login = L;
    s->core = &login_api;
    s->core_ctx = L;
    draw_all(L);
    return 1;
}

int login_begin(SESSION *s, unsigned long *errinfo, const char **why)
{
    const char *msg = "";
    unsigned int n;
    bool ok;

    *errinfo = ERRINFO_DENIED;
    if (!rdp_login_on()) {
        memset(s->ci_pw, 0, sizeof(s->ci_pw));
        return LOGIN_GO;
    }
    if (s->tls == NULL && !rdp_login_plain()) {
        memset(s->ci_pw, 0, sizeof(s->ci_pw));
        log_msg(LOG_WARN, "session %s: login: refused, this client uses "
                "plain RDP (plain-login=off: only TLS clients may log in)",
                s->peer);
        *why = "login: plain RDP is not allowed (plain-login=off)";
        return LOGIN_REFUSE;
    }
    if ((s->info_flags & INFO_AUTOLOGON) && s->ci_pw[0] != 0) {
        if (paused()) {
            log_msg(LOG_INFO, "session %s: login: a wrong password came "
                    "less than %d ms ago, so the box asks", s->peer,
                    LOGIN_PAUSE_MS);
        } else {
            for (n = 0; n < AUTH_PW_MAX && s->ci_pw[n] != 0; n++) {
            }
            ok = pw_right(s->ci_pw, n);
            memset(s->ci_pw, 0, sizeof(s->ci_pw));
            if (ok) {
                log_msg(LOG_INFO, "session %s: login: the client's "
                        "password is right", s->peer);
                return LOGIN_GO;
            }
            /* Not a try: the user did not type it */
            pause_start();
            log_msg(LOG_WARN, "session %s: login: the client's password "
                    "is wrong", s->peer);
            msg = "Wrong password.";
        }
    }
    memset(s->ci_pw, 0, sizeof(s->ci_pw));
    if (!make_box(s, msg)) {
        *errinfo = ERRINFO_OUT_OF_MEMORY;
        *why = "login: the login box could not start";
        return LOGIN_REFUSE;
    }
    log_msg(LOG_INFO, "session %s: login: the login box shows (%s)",
            s->peer, s->tls != NULL ? "TLS" : "plain RDP: the password "
            "crosses the network in clear");
    return LOGIN_BOX;
}

/* The check of the typed password. */
static void check(LOGIN *L)
{
    SESSION *s = L->s;
    unsigned int locks = L->locks;
    char msg[64];
    int left;
    bool ok;

    ok = pw_right(L->pw, L->len);
    memset(L->pw, 0, sizeof(L->pw));
    L->len = 0;
    if (ok) {
        log_msg(LOG_INFO, "session %s: login: done, the screen comes",
                s->peer);
        login_free(s);                  /* the real core is back */
        rdp_console(s, locks);
        return;
    }
    pause_start();
    if (++L->tries >= LOGIN_TRIES) {
        log_msg(LOG_WARN, "session %s: login: %d wrong tries: the end",
                s->peer, L->tries);
        rdp_end(s, ERRINFO_DENIED, "login: too many wrong tries");
        return;
    }
    left = LOGIN_TRIES - L->tries;
    log_msg(LOG_INFO, "session %s: login: wrong password, %d %s left",
            s->peer, left, left == 1 ? "try" : "tries");
    snprintf(msg, sizeof(msg), "Wrong password. %d %s left.", left,
             left == 1 ? "try" : "tries");
    set_msg(L, msg, C_RED);
    draw_field(L);
}

void login_pump(SESSION *s)
{
    LOGIN *L = s->login;

    if (L == NULL || s->ending != END_NO || !L->check_due) {
        return;
    }
    /* "Checking ..." first; at most 1 s for it (a client that reads
     * nothing). After a wrong password the check waits a little. */
    if (GetTickCount() - L->check_ms < 1000 && !s->suppressed &&
        (SESSION_QUEUED(s) > 0 || s->map.dirty > 0 || s->tile.ready)) {
        return;
    }
    if (paused()) {
        return;
    }
    L->check_due = 0;
    check(L);
}

void login_check_time(SESSION *s, DWORD now)
{
    LOGIN *L = s->login;

    if (L == NULL || L->check_due || s->ending != END_NO ||
        now - L->start_ms <= LOGIN_TIMEOUT_MS) {
        return;
    }
    log_msg(LOG_WARN, "session %s: login: no log on within %d s: the end",
            s->peer, LOGIN_TIMEOUT_MS / 1000);
    rdp_end(s, ERRINFO_LOGON_TIMEOUT, "login: no log on in time");
}
