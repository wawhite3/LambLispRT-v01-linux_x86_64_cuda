/* Copyright 2026 by Frobenius Norm LLC 2026-08-22
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ############################################################################
 * #  GPLv3.  This file is part of the w3_profinet DAEMON, which links        #
 * #  against p-net.  It must NEVER be compiled into liblamblisp.a or any     #
 * #  LambLispRT-v01-<env> package -- see w3_profinet/README.md.              #
 * #  w3_make_release_one has a guard that aborts if it ever is.              #
 * ############################################################################
 *
 * P145 Part B -- platform types for the p-net port (Linux).
 *
 * p-net publishes include/pnal.h (the porting CONTRACT) but not pnal_sys.h,
 * which pnal.h includes and which must supply the platform-dependent types.
 * These definitions were derived from how the p-net core actually uses them,
 * not guessed:
 *
 *   pnal_buf_t              core touches exactly p_buf->payload and p_buf->len
 *   struct pnal_eth_handle  declared opaque in pnal.h; defined by the port
 *
 * NOT here, though a first attempt put them here and the compiler objected:
 *   pnal_eth_mau_t   pnal.h:83 already defines it
 *   pnal_cfg_t       must exist before pnal_sys.h is reached -> pnal_config.h
 */
#ifndef PNAL_SYS_H
#define PNAL_SYS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/* osal (rtlabs-com/osal, BSD-3-Clause) supplies the os_* primitives that the
 * p-net core calls: os_event_create, os_mutex_create, os_get_current_time_us,
 * os_tick_current.  osal is permissively licensed -- only the p-net core and
 * this port are GPLv3. */
#include "osal.h"
#include "pnal_config.h"

#define PNAL_BUF_MAX_SIZE 1522   /* 1500 payload + 14 Ethernet + 4 VLAN + 4 FCS */

/** Network buffer.  The core only ever touches payload and len. */
typedef struct pnal_buf
{
   void * payload;
   uint16_t len;
   /* Port-private: the backing store, so payload can be moved by
    * pnal_buf_header() without losing the allocation. */
   uint8_t * _alloc;
   uint16_t _alloc_size;
} pnal_buf_t;

/** Raw-Ethernet handle.  Declared opaque by pnal.h; the port defines it.
 *  One AF_PACKET socket bound to one interface, with a receive thread that
 *  hands frames to the p-net core through the registered callback. */
struct pnal_eth_handle
{
   int socket;
   int if_index;
   char if_name[32];
   uint16_t receive_type;          /* host order; 0 = ETH_P_ALL */
   void * arg;
   int (*callback) (struct pnal_eth_handle *, void *, pnal_buf_t *);
   pthread_t rx_thread;
   volatile bool running;
};

#ifdef __cplusplus
}
#endif

#endif /* PNAL_SYS_H */
