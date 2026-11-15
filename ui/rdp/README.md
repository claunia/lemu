# RDP protocol code (from ntrdps)

TL;DR: the protocol part of LEMU's RDP server (`-rdp`). It comes from
[ntrdps](https://github.com/tenox7/ntrdps) 0.9 (2026-11-14), an RDP
console server for Windows NT, as ntrdps publishes it on GitHub (commit
85fc5f4). MIT license, see [LICENSE](LICENSE).

## What is here

| Files | What | Changes |
|---|---|---|
| `*.c`, `*.h` | the protocol: X.224, MCS, GCC, security, licensing, capabilities, input, screen updates, bitmaps, orders, pointer, MPPC | none, except LF line ends |
| `session.h` | the session state | LEMU's own version, see below |
| `posix/windows.h` | the few Windows types and calls that the code uses, for macOS and Linux | LEMU's own; Windows hosts use the real `<windows.h>` |
| `bearssl/` | [BearSSL](https://bearssl.org/) git 7bea48e: the 53 files for one TLS 1.2 suite (ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 on P-256), as in ntrdps | none, except LF line ends; its `BR_NT31` parts are not used here. MIT, see [bearssl/LICENSE.txt](bearssl/LICENSE.txt) |
| `lfont.c` | the X11 misc-fixed 9x15 font, for the login box | none, except LF line ends; public domain |

## LEMU's own parts (in `ui/`)

- `rdp.c`: the core that ntrdps's protocol code talks to (the guest's
  screen from a display listener, the guest's pointer shape, the clients'
  keys and mouse), the connections, and the `-rdp` options.
- `rdp-tls.c`: TLS. The engine pump comes from ntrdps's `tls.c`. The key
  and a self-signed certificate are made at start.
- `rdp-login.c`: the password box, from ntrdps's `login.c`.

## How it differs from ntrdps

- **No threads.** ntrdps runs each connection in a thread with
  `select()`. Here each connection runs in QEMU's main loop: a channel
  watch, a bottom half for `session_wake`, and a timer for a turn at
  least every 100 ms. `session_input_waiting` always says yes, so a big
  screen update goes out in 20 ms slices and the guest keeps running.
- **The log** becomes the trace events `rdp_log` and `rdp_hex`
  (`-trace 'rdp_*'`).
- **TLS** is always built in and on by default (ntrdps's AllowTLS).
  The certificate's name is "LEMU". It is made from the key, the same way
  each time, so with `tls-key=` it stays the same from run to run.
- **The login** is one password from a QEMU `secret` object (ntrdps's
  Login and LoginPlain). There are no user names and no NT logon dialog.
  The box knows the US keyboard layout only.
- **Not taken:** ntrdps's capture back-ends and display drivers, its
  service, settings, `auth.c`, `ldlg.c` and `net.c`.
- **Connections:** at most 8 open at once, as ntrdps's `NET_SLOTS`.
- **One client has the screen.** A new client takes it from the one before
  once it has logged on (ntrdps's NewClient 0). The console prompt of
  ntrdps is not used.

## Clients

| Client | Status |
|---|---|
| FreeRDP 3.33 (`xfreerdp /v:HOST /cert:ignore`) | tested in LEMU: TLS and plain RDP, with and without a password |
| rdesktop, mstsc 5.0 and 5.1 | tested with ntrdps, not yet in LEMU |
| the Windows App, mstsc 6.0 and later | not tested yet |

## Session.h

The same fields as ntrdps's, except:

- `struct RdpConn *conn` replaces the socket and the stop flag.
- The wake socket and the loop counters are gone (no thread).
- `NTRDPS_TLS` and `NTRDPS_LOGIN` are always defined.
- `NET_PEER_MAX` is 64, not 24: a peer name can be an IPv6 address.

## Taking a newer ntrdps

1. Copy the files of the first table again from the public ntrdps on
   GitHub, never from a development copy: `src/rdp`, `src/core` (`core.h`,
   `draw.c`, `draw.h`), `src/service/log.h` and `src/tls/bearssl`. Change
   their line ends to LF.
2. Compare ntrdps's new `session.h` with this one, and bring over the new
   fields.
3. Check what `core.h` asks of the core (`CORE_VERSION`), and what
   `login.c` and `tls.c` changed, for `rdp.c`, `rdp-login.c` and
   `rdp-tls.c`.
4. Build with `--enable-rdp` and test with a client: TLS and plain,
   with and without a password.

LEMU is GPL version 2. MIT code may be part of it.
