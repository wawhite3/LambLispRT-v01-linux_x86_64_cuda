/* Copyright 2026 by Frobenius Norm LLC 2026-08-22
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GPLv3 -- part of the w3_profinet daemon.  Never link into liblamblisp.a.
 *
 * P145 Part B -- port header #1 of 3.
 *
 * Include order matters and is not obvious:
 *
 *   pnal.h  ->  pnet_api.h  ->  pnal_config.h   (this file: pnal_cfg_t)
 *           ->  pnal_sys.h                      (pnal_buf_t, eth handle)
 *
 * pnet_api.h embeds `pnal_cfg_t pnal_cfg;` in pnet_cfg_t, so pnal_cfg_t must
 * exist BEFORE pnal_sys.h is reached.  That is why the port is split in two
 * headers rather than one.
 */
#ifndef PNAL_CONFIG_H
#define PNAL_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/** Thread parameters for a p-net worker. */
/* Member names are NOT free choice -- pf_bg_worker.c reads
 * bg_worker_thread.prio and .stack_size directly.  A first attempt called it
 * `priority` and the core would not compile. */
typedef struct pnal_thread_cfg
{
   const char * name;
   int prio;
   size_t stack_size;
} pnal_thread_cfg_t;

/** Platform configuration.  The p-net core reads only bg_worker_thread;
 *  anything else here is this port's own business. */
typedef struct pnal_cfg
{
   pnal_thread_cfg_t bg_worker_thread;
   int snmp_enable;   /* 0 = stubbed SNMP; fine for Conformance Class A */
} pnal_cfg_t;

#ifdef __cplusplus
}
#endif

#endif /* PNAL_CONFIG_H */
