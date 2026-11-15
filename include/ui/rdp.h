/*
 * RDP server (ui/rdp.c): the -rdp option.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_RDP_H
#define UI_RDP_H

/* -rdp: keeps the options for rdp_init_func. Exits on a bad one. */
void rdp_parse(const char *str);

/* For qemu_opts_foreach, once the consoles exist: starts the server. */
int rdp_init_func(void *opaque, QemuOpts *opts, Error **errp);

#endif
