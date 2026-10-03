/* Copyright 2026 by Frobenius Norm LLC 2026-08-22
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ############################################################################
 * #  GPLv3.  This is the w3_profinet DAEMON: it links against p-net.         #
 * #  NEVER link any of this into liblamblisp.a or a LambLispRT-v01-<env>     #
 * #  package.  w3_make_release_one aborts if p-net symbols ever appear in a  #
 * #  shipped artifact.  See README.md for why the process boundary exists.   #
 * ############################################################################
 *
 * P145 Part B -- the PROFINET daemon.
 *
 * Runs a p-net IO-Device and serves the LambLisp IPC protocol defined in
 * ../src/ll_vm_profinet_ipc.h.  LambLisp connects over AF_UNIX and drives the
 * device without ever linking a line of GPLv3 code.
 *
 * Two loops, deliberately separated:
 *
 *   the p-net tick      pnet_handle_periodic() every tick_us -- this is the
 *                       real-time obligation and it must never wait on the
 *                       socket.
 *   the IPC service     accept/read/reply -- may block briefly; a slow or
 *                       absent LambLisp must never stall the bus.
 *
 * A single thread runs both with a poll() timeout, so a hung client cannot
 * delay the tick beyond one service call.  Events raised by p-net callbacks go
 * into a ring that profinet-poll-event drains.
 *
 * Usage:
 *   pnd --if eth0 --station lamb-io-1 [--socket PATH] [--vendor N] [--device N]
 *
 * Needs CAP_NET_RAW.
 */
#define _GNU_SOURCE

#include "pnet_api.h"
#include "ll_vm_profinet_ipc.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PND_API              0        /* p-net API id; 0 is the standard one */
#define PND_MAX_SUBMOD       16
#define PND_EVENTQ           32
#define PND_TICK_US          1000

/* ── State ───────────────────────────────────────────────────────────────── */

typedef struct pnd_submod
{
   bool used;
   uint16_t slot, subslot;
   uint32_t ident;
   uint16_t in_len, out_len;
   uint8_t in_data[PN_IPC_MAX_PAYLOAD];
   uint8_t iops;                 /* our provider status */
   uint8_t iocs;                 /* the controller's consumer status */
} pnd_submod_t;

typedef struct pnd
{
   pnet_t * net;
   pnet_cfg_t cfg;
   const char * if_name;
   char station[64];
   uint16_t vendor_id, device_id;

   pnd_submod_t sub[PND_MAX_SUBMOD];
   int n_sub;

   uint32_t arep;
   int state;                    /* PnIpcState */

   uint8_t evq[PND_EVENTQ][PN_IPC_MAX_PAYLOAD];
   int ev_len[PND_EVENTQ];
   int ev_head, ev_tail;

   uint32_t cycles, missed, alarms, restarts;
   volatile bool running;
} pnd_t;

static pnd_t g_pnd;

static void pnd_log (const char * fmt, ...)
{
   va_list ap;
   struct timespec ts;

   clock_gettime (CLOCK_REALTIME, &ts);
   fprintf (stderr, "[pnd %ld.%03ld] ", (long) ts.tv_sec, ts.tv_nsec / 1000000);
   va_start (ap, fmt);
   vfprintf (stderr, fmt, ap);
   va_end (ap);
   fputc ('\n', stderr);
}

/* ── Event ring ──────────────────────────────────────────────────────────── */
/* Callbacks run on p-net's thread; the IPC service drains on ours.  The ring
 * is single-producer/single-consumer with a published head, the same discipline
 * as the PROFIBUS process image -- no lock on the fast path. */

static void pnd_event_push (uint8_t kind, const void * data, int len)
{
   const int next = (g_pnd.ev_head + 1) % PND_EVENTQ;

   if (next == g_pnd.ev_tail)
   {
      pnd_log ("event queue full, dropping kind 0x%02X", kind);
      return;
   }
   if (len > PN_IPC_MAX_PAYLOAD - 1)
   {
      len = PN_IPC_MAX_PAYLOAD - 1;
   }
   g_pnd.evq[g_pnd.ev_head][0] = kind;
   if (len > 0 && data != NULL)
   {
      memcpy (&g_pnd.evq[g_pnd.ev_head][1], data, (size_t) len);
   }
   g_pnd.ev_len[g_pnd.ev_head] = len + 1;
   g_pnd.ev_head = next;
}

static pnd_submod_t * pnd_submod_find (uint16_t slot, uint16_t subslot)
{
   int i;

   for (i = 0; i < g_pnd.n_sub; i++)
   {
      if (g_pnd.sub[i].used && g_pnd.sub[i].slot == slot &&
          g_pnd.sub[i].subslot == subslot)
      {
         return &g_pnd.sub[i];
      }
   }
   return NULL;
}

/* ── p-net callbacks -> IPC events ───────────────────────────────────────── */

static int pnd_cb_connect (pnet_t * net, void * arg, uint32_t arep, pnet_result_t * p_result)
{
   (void) net; (void) arg; (void) p_result;
   g_pnd.arep = arep;
   g_pnd.state = PN_IPC_CONNECTED;
   pnd_event_push (PN_EV_CONNECT, NULL, 0);
   pnd_log ("connect: arep %u", arep);
   return 0;
}

static int pnd_cb_release (pnet_t * net, void * arg, uint32_t arep, pnet_result_t * p_result)
{
   (void) net; (void) arg; (void) arep; (void) p_result;
   g_pnd.state = PN_IPC_DCP_ONLY;
   pnd_event_push (PN_EV_RELEASE, NULL, 0);
   pnd_log ("release");
   return 0;
}

static int pnd_cb_state (pnet_t * net, void * arg, uint32_t arep, pnet_event_values_t state)
{
   (void) arg;
   g_pnd.arep = arep;
   if (state == PNET_EVENT_PRMEND)
   {
      /* Parameterisation finished: tell p-net our data is valid, then declare
       * the application ready.  Doing this here is what moves a device from
       * "connected" to actually exchanging data. */
      uint16_t i;

      for (i = 0; i < (uint16_t) g_pnd.n_sub; i++)
      {
         if (!g_pnd.sub[i].used)
         {
            continue;
         }
         (void) pnet_input_set_data_and_iops (
            net,
            PND_API,
            g_pnd.sub[i].slot,
            g_pnd.sub[i].subslot,
            g_pnd.sub[i].in_data,
            g_pnd.sub[i].in_len,
            g_pnd.sub[i].iops);
         (void) pnet_output_set_iocs (
            net, PND_API, g_pnd.sub[i].slot, g_pnd.sub[i].subslot, PNET_IOXS_GOOD);
      }
      (void) pnet_set_provider_state (net, true);
      (void) pnet_application_ready (net, arep);
      pnd_event_push (PN_EV_PARAM_END, NULL, 0);
      pnd_log ("param-end -> application ready");
   }
   else if (state == PNET_EVENT_APPLRDY)
   {
      g_pnd.state = PN_IPC_OPERATE;
      pnd_event_push (PN_EV_APP_READY, NULL, 0);
      pnd_log ("application ready -> OPERATE");
   }
   else if (state == PNET_EVENT_ABORT)
   {
      g_pnd.state = PN_IPC_DCP_ONLY;
      g_pnd.restarts++;
      pnd_event_push (PN_EV_RELEASE, NULL, 0);
      pnd_log ("abort");
   }
   return 0;
}

/* The controller tells us what it EXPECTS to find; we plug what we have.
 * Refusing an unexpected module here is what makes a GSDML mismatch show up as
 * a clean configuration error instead of silent misbehaviour. */
static int pnd_cb_exp_module (pnet_t * net, void * arg, uint32_t api, uint16_t slot, uint32_t module_ident)
{
   (void) arg;
   pnd_log ("expected module: slot %u ident 0x%08X", slot, module_ident);
   return pnet_plug_module (net, api, slot, module_ident);
}

static int pnd_cb_exp_submodule (
   pnet_t * net,
   void * arg,
   uint32_t api,
   uint16_t slot,
   uint16_t subslot,
   uint32_t module_ident,
   uint32_t submodule_ident,
   const pnet_data_cfg_t * p_exp_data)
{
   (void) arg;
   pnd_log (
      "expected submodule: %u/%u ident 0x%08X in %u out %u",
      slot,
      subslot,
      submodule_ident,
      p_exp_data ? p_exp_data->insize : 0,
      p_exp_data ? p_exp_data->outsize : 0);
   return pnet_plug_submodule (
      net, api, slot, subslot, module_ident, submodule_ident,
      p_exp_data ? p_exp_data->data_dir : PNET_DIR_IO,
      p_exp_data ? p_exp_data->insize : 0,
      p_exp_data ? p_exp_data->outsize : 0);
}

static int pnd_cb_new_data_status (
   pnet_t * net, void * arg, uint32_t arep, uint32_t crep, uint8_t changes, uint8_t data_status)
{
   (void) net; (void) arg; (void) arep; (void) crep; (void) changes;
   /* Bit 2 (DataValid) clearing is the controller telling us its data is stale
    * -- the same signal our own staleness rule raises in the other direction. */
   pnd_log ("data status 0x%02X", data_status);
   return 0;
}

static int pnd_cb_alarm_ind (
   pnet_t * net, void * arg, uint32_t arep, const pnet_alarm_argument_t * p_alarm_arg,
   uint16_t data_len, uint16_t data_usi, const uint8_t * p_data)
{
   (void) net; (void) arg; (void) arep; (void) data_len; (void) p_data;
   uint8_t payload[2];

   payload[0] = (uint8_t) (data_usi >> 8);
   payload[1] = (uint8_t) (data_usi & 0xFF);
   g_pnd.alarms++;
   pnd_event_push (PN_EV_ALARM, payload, 2);
   (void) p_alarm_arg;
   return 0;
}

static int pnd_cb_signal_led (pnet_t * net, void * arg, bool led_state)
{
   (void) net; (void) arg;
   /* DCP "flash so the technician can find me".  Surfaced to Scheme rather than
    * handled here: which LED, and how, is application policy. */
   pnd_event_push (PN_EV_DCP_SIGNAL, &(uint8_t){led_state ? 1 : 0}, 1);
   pnd_log ("signal LED %s", led_state ? "ON" : "OFF");
   return 0;
}

static int pnd_cb_reset (
   pnet_t * net, void * arg, bool should_reset_application, uint16_t reset_mode)
{
   (void) net; (void) arg; (void) should_reset_application;
   pnd_log ("reset requested, mode %u", reset_mode);
   return 0;
}

/* Called by the pnal port when the controller commissions us over DCP.  This
 * is the ONLY observation point: p-net exposes no callback for a name/IP
 * change, but CMINA commits it through pnal_set_ip_suite(), which receives the
 * NameOfStation as `hostname`.  Surfacing it here is what lets a Scheme
 * application persist the name it was given. */
void pnd_on_dcp_commission (
   const char * interface_name,
   uint32_t ip,
   uint32_t mask,
   uint32_t gw,
   const char * hostname,
   int permanent)
{
   uint8_t payload[12];

   (void) interface_name;
   if (hostname != NULL && hostname[0] != '\0')
   {
      if (strcmp (hostname, g_pnd.station) != 0)
      {
         snprintf (g_pnd.station, sizeof (g_pnd.station), "%s", hostname);
         pnd_event_push (PN_EV_DCP_SET_NAME, hostname, (int) strlen (hostname));
         pnd_log ("DCP set NameOfStation = '%s'%s", hostname, permanent ? " (permanent)" : "");
      }
   }
   payload[0] = (uint8_t) (ip >> 24);   payload[1] = (uint8_t) (ip >> 16);
   payload[2] = (uint8_t) (ip >> 8);    payload[3] = (uint8_t) ip;
   payload[4] = (uint8_t) (mask >> 24); payload[5] = (uint8_t) (mask >> 16);
   payload[6] = (uint8_t) (mask >> 8);  payload[7] = (uint8_t) mask;
   payload[8] = (uint8_t) (gw >> 24);   payload[9] = (uint8_t) (gw >> 16);
   payload[10] = (uint8_t) (gw >> 8);   payload[11] = (uint8_t) gw;
   pnd_event_push (PN_EV_DCP_SET_IP, payload, 12);
   pnd_log ("DCP set IP = %u.%u.%u.%u", payload[0], payload[1], payload[2], payload[3]);
}

/* ── IPC service ─────────────────────────────────────────────────────────── */

static int pnd_read_exact (int fd, uint8_t * buf, int len)
{
   int off = 0;

   while (off < len)
   {
      ssize_t n = read (fd, buf + off, (size_t) (len - off));
      if (n == 0)
      {
         return -1;   /* client closed */
      }
      if (n < 0)
      {
         if (errno == EINTR)
         {
            continue;
         }
         return -1;
      }
      off += (int) n;
   }
   return 0;
}

static int pnd_reply (
   int fd, uint8_t cmd, uint8_t status, uint8_t flags, uint16_t slot,
   uint16_t subslot, const uint8_t * payload, int len)
{
   uint8_t msg[PN_IPC_MAX_MSG];

   if (len < 0 || len > PN_IPC_MAX_PAYLOAD)
   {
      len = 0;
   }
   pn_ipc_hdr_put (msg, cmd, status, flags, slot, subslot, (uint16_t) len);
   if (len > 0 && payload != NULL)
   {
      memcpy (msg + PN_IPC_HDR_LEN, payload, (size_t) len);
   }
   return (write (fd, msg, (size_t) (PN_IPC_HDR_LEN + len)) ==
           (ssize_t) (PN_IPC_HDR_LEN + len))
             ? 0
             : -1;
}

/* Handle one request.  Returns -1 when the client has gone. */
static int pnd_service (int fd)
{
   uint8_t hdr[PN_IPC_HDR_LEN];
   uint8_t body[PN_IPC_MAX_PAYLOAD];
   uint8_t cmd, status, flags;
   uint16_t slot, subslot, len;
   pnd_submod_t * s;

   if (pnd_read_exact (fd, hdr, PN_IPC_HDR_LEN) != 0)
   {
      return -1;
   }
   if (!pn_ipc_hdr_get (hdr, &cmd, &status, &flags, &slot, &subslot, &len))
   {
      pnd_log ("bad IPC header -- desync, dropping client");
      return -1;
   }
   if (len > 0 && pnd_read_exact (fd, body, len) != 0)
   {
      return -1;
   }

   switch (cmd)
   {
   case PN_IPC_HELLO:
      return pnd_reply (
         fd, cmd, PN_IPC_ST_OK, 0, 0, 0, (const uint8_t *) "w3_profinet/1", 13);

   case PN_IPC_ADD_MODULE:
      /* Recorded for the exp_module callback; p-net plugs on the controller's
       * request, not ours. */
      return pnd_reply (fd, cmd, (len == 4) ? PN_IPC_ST_OK : PN_IPC_ST_BADARG, 0, slot, subslot, NULL, 0);

   case PN_IPC_ADD_SUBMODULE:
   {
      if (len != 8 || g_pnd.n_sub >= PND_MAX_SUBMOD)
      {
         return pnd_reply (fd, cmd, PN_IPC_ST_BADARG, 0, slot, subslot, NULL, 0);
      }
      s = &g_pnd.sub[g_pnd.n_sub++];
      memset (s, 0, sizeof (*s));
      s->used = true;
      s->slot = slot;
      s->subslot = subslot;
      s->ident = ((uint32_t) body[0] << 24) | ((uint32_t) body[1] << 16) |
                 ((uint32_t) body[2] << 8) | body[3];
      s->in_len = (uint16_t) ((body[4] << 8) | body[5]);
      s->out_len = (uint16_t) ((body[6] << 8) | body[7]);
      s->iops = PNET_IOXS_GOOD;
      pnd_log ("submodule %u/%u in %u out %u", slot, subslot, s->in_len, s->out_len);
      return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, slot, subslot, NULL, 0);
   }

   case PN_IPC_START:
      if (len > 0)
      {
         int n = (len < (int) sizeof (g_pnd.station) - 1) ? len : (int) sizeof (g_pnd.station) - 1;
         memcpy (g_pnd.station, body, (size_t) n);
         g_pnd.station[n] = '\0';
      }
      if (g_pnd.state == PN_IPC_OFFLINE)
      {
         g_pnd.state = PN_IPC_DCP_ONLY;
      }
      pnd_log ("start: station '%s' -- waiting for a controller", g_pnd.station);
      return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, 0, 0, NULL, 0);

   case PN_IPC_STOP:
      g_pnd.state = PN_IPC_DCP_ONLY;
      return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, 0, 0, NULL, 0);

   case PN_IPC_SET_INPUTS:
      s = pnd_submod_find (slot, subslot);
      if (s == NULL)
      {
         return pnd_reply (fd, cmd, PN_IPC_ST_NOSUB, 0, slot, subslot, NULL, 0);
      }
      memcpy (s->in_data, body, (size_t) ((len < s->in_len) ? len : s->in_len));
      s->iops = flags ? flags : PNET_IOXS_GOOD;
      if (g_pnd.net != NULL && g_pnd.state >= PN_IPC_CONNECTED)
      {
         (void) pnet_input_set_data_and_iops (
            g_pnd.net, PND_API, slot, subslot, s->in_data, s->in_len, s->iops);
      }
      return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, slot, subslot, NULL, 0);

   case PN_IPC_SET_IOPS:
      s = pnd_submod_find (slot, subslot);
      if (s == NULL)
      {
         return pnd_reply (fd, cmd, PN_IPC_ST_NOSUB, 0, slot, subslot, NULL, 0);
      }
      s->iops = flags;
      return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, slot, subslot, NULL, 0);

   case PN_IPC_GET_OUTPUTS:
   {
      uint8_t data[PN_IPC_MAX_PAYLOAD];
      uint16_t data_len = sizeof (data);
      uint8_t iops = 0;
      bool is_new = false;

      s = pnd_submod_find (slot, subslot);
      if (s == NULL)
      {
         return pnd_reply (fd, cmd, PN_IPC_ST_NOSUB, 0, slot, subslot, NULL, 0);
      }
      if (g_pnd.net == NULL || g_pnd.state < PN_IPC_CONNECTED)
      {
         return pnd_reply (fd, cmd, PN_IPC_ST_NOTREADY, 0, slot, subslot, NULL, 0);
      }
      if (pnet_output_get_data_and_iops (
             g_pnd.net, PND_API, slot, subslot, &is_new, data, &data_len, &iops) != 0)
      {
         return pnd_reply (fd, cmd, PN_IPC_ST_INTERNAL, 0, slot, subslot, NULL, 0);
      }
      s->iocs = iops;
      return pnd_reply (fd, cmd, PN_IPC_ST_OK, iops, slot, subslot, data, (int) data_len);
   }

   case PN_IPC_POLL_EVENT:
      if (g_pnd.ev_tail == g_pnd.ev_head)
      {
         return pnd_reply (fd, cmd, PN_IPC_ST_EMPTY, 0, 0, 0, NULL, 0);
      }
      else
      {
         const int i = g_pnd.ev_tail;
         g_pnd.ev_tail = (g_pnd.ev_tail + 1) % PND_EVENTQ;
         return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, 0, 0, g_pnd.evq[i], g_pnd.ev_len[i]);
      }

   case PN_IPC_STATE:
   {
      uint8_t st = (uint8_t) g_pnd.state;
      return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, 0, 0, &st, 1);
   }

   case PN_IPC_STATS:
   {
      uint8_t out[PN_STAT_COUNT * 4];
      uint32_t v[PN_STAT_COUNT];
      int i;

      v[PN_STAT_CYCLES] = g_pnd.cycles;
      v[PN_STAT_MISSED] = g_pnd.missed;
      v[PN_STAT_CYCLE_COUNTER] = g_pnd.cycles & 0xFFFF;
      v[PN_STAT_JITTER_US] = 0;
      v[PN_STAT_ALARMS] = g_pnd.alarms;
      v[PN_STAT_RESTARTS] = g_pnd.restarts;
      for (i = 0; i < PN_STAT_COUNT; i++)
      {
         out[i * 4 + 0] = (uint8_t) (v[i] >> 24);
         out[i * 4 + 1] = (uint8_t) (v[i] >> 16);
         out[i * 4 + 2] = (uint8_t) (v[i] >> 8);
         out[i * 4 + 3] = (uint8_t) (v[i]);
      }
      return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, 0, 0, out, sizeof (out));
   }

   case PN_IPC_ALARM:
      if (g_pnd.net == NULL || g_pnd.state < PN_IPC_CONNECTED)
      {
         return pnd_reply (fd, cmd, PN_IPC_ST_NOTREADY, 0, slot, subslot, NULL, 0);
      }
      else
      {
         const uint16_t usi = (uint16_t) ((body[0] << 8) | body[1]);
         (void) pnet_alarm_send_process_alarm (
            g_pnd.net, g_pnd.arep, PND_API, slot, subslot, usi,
            (uint16_t) ((len > 2) ? len - 2 : 0), &body[2]);
         g_pnd.alarms++;
         return pnd_reply (fd, cmd, PN_IPC_ST_OK, 0, slot, subslot, NULL, 0);
      }

   default:
      return pnd_reply (fd, cmd, PN_IPC_ST_BADCMD, 0, slot, subslot, NULL, 0);
   }
}

/* ── main ────────────────────────────────────────────────────────────────── */

static void pnd_sigint (int sig)
{
   (void) sig;
   g_pnd.running = false;
}

int main (int argc, char ** argv)
{
   const char * sock_path = PN_IPC_DEFAULT_PATH;
   int srv, client = -1;
   struct sockaddr_un sa;
   int i;

   memset (&g_pnd, 0, sizeof (g_pnd));
   g_pnd.if_name = "eth0";
   g_pnd.vendor_id = 0x0493;
   g_pnd.device_id = 0x0002;
   snprintf (g_pnd.station, sizeof (g_pnd.station), "lamb-io-1");

   for (i = 1; i < argc; i++)
   {
      if (strcmp (argv[i], "--if") == 0 && i + 1 < argc)
      {
         g_pnd.if_name = argv[++i];
      }
      else if (strcmp (argv[i], "--station") == 0 && i + 1 < argc)
      {
         snprintf (g_pnd.station, sizeof (g_pnd.station), "%s", argv[++i]);
      }
      else if (strcmp (argv[i], "--socket") == 0 && i + 1 < argc)
      {
         sock_path = argv[++i];
      }
      else if (strcmp (argv[i], "--vendor") == 0 && i + 1 < argc)
      {
         g_pnd.vendor_id = (uint16_t) strtoul (argv[++i], NULL, 0);
      }
      else if (strcmp (argv[i], "--device") == 0 && i + 1 < argc)
      {
         g_pnd.device_id = (uint16_t) strtoul (argv[++i], NULL, 0);
      }
      else
      {
         fprintf (stderr,
                  "usage: %s --if IFACE [--station NAME] [--socket PATH]\n"
                  "          [--vendor N] [--device N]\n", argv[0]);
         return 1;
      }
   }

   /* ── p-net configuration ─────────────────────────────────────────────── */
   memset (&g_pnd.cfg, 0, sizeof (g_pnd.cfg));
   g_pnd.cfg.tick_us = PND_TICK_US;
   g_pnd.cfg.min_device_interval = 32;      /* 32 x 31.25 us = 1 ms */
   g_pnd.cfg.if_cfg.main_netif_name = g_pnd.if_name;
   g_pnd.cfg.if_cfg.physical_ports[0].netif_name = g_pnd.if_name;
   g_pnd.cfg.if_cfg.physical_ports[0].default_mau_type = 0x0010;  /* copper 100 FD */
   g_pnd.cfg.num_physical_ports = 1;
   snprintf (g_pnd.cfg.station_name, sizeof (g_pnd.cfg.station_name), "%s", g_pnd.station);

   g_pnd.cfg.device_id.vendor_id_hi = (uint8_t) (g_pnd.vendor_id >> 8);
   g_pnd.cfg.device_id.vendor_id_lo = (uint8_t) (g_pnd.vendor_id & 0xFF);
   g_pnd.cfg.device_id.device_id_hi = (uint8_t) (g_pnd.device_id >> 8);
   g_pnd.cfg.device_id.device_id_lo = (uint8_t) (g_pnd.device_id & 0xFF);
   g_pnd.cfg.oem_device_id = g_pnd.cfg.device_id;

   g_pnd.cfg.state_cb = pnd_cb_state;
   g_pnd.cfg.connect_cb = pnd_cb_connect;
   g_pnd.cfg.release_cb = pnd_cb_release;
   g_pnd.cfg.exp_module_cb = pnd_cb_exp_module;
   g_pnd.cfg.exp_submodule_cb = pnd_cb_exp_submodule;
   g_pnd.cfg.new_data_status_cb = pnd_cb_new_data_status;
   g_pnd.cfg.alarm_ind_cb = pnd_cb_alarm_ind;
   g_pnd.cfg.signal_led_cb = pnd_cb_signal_led;
   g_pnd.cfg.reset_cb = pnd_cb_reset;
   g_pnd.cfg.pnal_cfg.bg_worker_thread.prio = 5;
   g_pnd.cfg.pnal_cfg.bg_worker_thread.stack_size = 4096;

   /* ── IPC socket first: LambLisp can connect and see OFFLINE even if the
    *    network side fails, which is better than a silent absence. ──────── */
   unlink (sock_path);
   srv = socket (AF_UNIX, SOCK_STREAM, 0);
   if (srv < 0)
   {
      pnd_log ("cannot create IPC socket: %s", strerror (errno));
      return 1;
   }
   memset (&sa, 0, sizeof (sa));
   sa.sun_family = AF_UNIX;
   snprintf (sa.sun_path, sizeof (sa.sun_path), "%s", sock_path);
   if (bind (srv, (struct sockaddr *) &sa, sizeof (sa)) != 0 || listen (srv, 2) != 0)
   {
      pnd_log ("cannot bind %s: %s", sock_path, strerror (errno));
      close (srv);
      return 1;
   }
   /* The daemon runs privileged (CAP_NET_RAW for AF_PACKET, CAP_NET_ADMIN for
    * DCP Set-IP) but LambLisp does not, so the default root-owned 0755 socket
    * is unreachable by the client -- connect() gets EACCES and the failure
    * looks like "no daemon" rather than "wrong permissions".  Widen it.
    * A deployment that cares should instead chown it to a dedicated group and
    * use 0660; this is a local socket on one machine. */
   if (chmod (sock_path, 0666) != 0)
   {
      pnd_log ("warning: cannot chmod %s: %s", sock_path, strerror (errno));
   }
   pnd_log ("IPC listening on %s", sock_path);

   g_pnd.net = pnet_init (&g_pnd.cfg);
   if (g_pnd.net == NULL)
   {
      pnd_log ("pnet_init failed on '%s' -- CAP_NET_RAW? interface up?", g_pnd.if_name);
      close (srv);
      unlink (sock_path);
      return 1;
   }
   g_pnd.state = PN_IPC_DCP_ONLY;
   pnd_log ("p-net up on '%s' as '%s'", g_pnd.if_name, g_pnd.station);

   signal (SIGINT, pnd_sigint);
   signal (SIGPIPE, SIG_IGN);
   g_pnd.running = true;

   /* ── The loop ────────────────────────────────────────────────────────── */
   /* poll() with a tick-sized timeout: the bus is serviced every tick whether
    * or not LambLisp says anything, so a slow or absent client cannot stall
    * the cycle.  That is the whole point of the process split. */
   while (g_pnd.running)
   {
      struct pollfd pfd[2];
      int nfds = 0;
      int rc;

      pfd[nfds].fd = srv;
      pfd[nfds].events = POLLIN;
      nfds++;
      if (client >= 0)
      {
         pfd[nfds].fd = client;
         pfd[nfds].events = POLLIN;
         nfds++;
      }

      rc = poll (pfd, (nfds_t) nfds, PND_TICK_US / 1000);
      pnet_handle_periodic (g_pnd.net);
      g_pnd.cycles++;

      if (rc <= 0)
      {
         continue;
      }
      if (pfd[0].revents & POLLIN)
      {
         int c = accept (srv, NULL, NULL);
         if (c >= 0)
         {
            if (client >= 0)
            {
               close (client);   /* one client at a time; newest wins */
            }
            client = c;
            pnd_log ("LambLisp connected");
         }
      }
      if (nfds > 1 && (pfd[1].revents & (POLLIN | POLLHUP)))
      {
         if (pnd_service (client) != 0)
         {
            pnd_log ("LambLisp disconnected");
            close (client);
            client = -1;
         }
      }
   }

   pnd_log ("shutting down");
   if (client >= 0)
   {
      close (client);
   }
   close (srv);
   unlink (sock_path);
   return 0;
}
