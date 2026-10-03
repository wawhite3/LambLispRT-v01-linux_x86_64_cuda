/* Copyright 2026 by Frobenius Norm LLC 2026-08-22
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ############################################################################
 * #  GPLv3.  Part of the w3_profinet DAEMON, which links against p-net.      #
 * #  NEVER compile this into liblamblisp.a or a LambLispRT-v01-<env>         #
 * #  package.  See w3_profinet/README.md.                                    #
 * ############################################################################
 *
 * P145 Part B -- the p-net platform abstraction layer (pnal) for Linux.
 *
 * p-net's public drop is the protocol core only: include/pnal.h declares 24
 * functions and no implementation is published.  This file is that
 * implementation.
 *
 *   raw Ethernet   AF_PACKET socket + a receive thread per interface
 *   UDP            ordinary BSD sockets (DCE/RPC on port 34964)
 *   IP config      SIOCGIF* ioctls; route table read for the gateway
 *   files          plain stdio, two-object form as the contract requires
 *   SNMP           stubbed -- not required for Conformance Class A
 *
 * Needs CAP_NET_RAW (setcap cap_net_raw+ep, or run as root) for AF_PACKET.
 */
#define _GNU_SOURCE

#include "pnal.h"
#include "osal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ── Time ────────────────────────────────────────────────────────────────── */

uint32_t pnal_get_system_uptime_10ms (void)
{
   struct timespec ts;
   if (clock_gettime (CLOCK_MONOTONIC, &ts) != 0)
   {
      return 0;
   }
   return (uint32_t) (ts.tv_sec * 100u + ts.tv_nsec / 10000000u);
}

/* ── Files ───────────────────────────────────────────────────────────────── */
/* The two-object form is the contract: p-net stores a header and a body in one
 * file.  Either object may be NULL/0. */

int pnal_save_file (
   const char * fullpath,
   const void * object_1,
   size_t size_1,
   const void * object_2,
   size_t size_2)
{
   FILE * f = fopen (fullpath, "wb");
   if (f == NULL)
   {
      return -1;
   }
   if (size_1 > 0 && object_1 != NULL && fwrite (object_1, size_1, 1, f) != 1)
   {
      fclose (f);
      return -1;
   }
   if (size_2 > 0 && object_2 != NULL && fwrite (object_2, size_2, 1, f) != 1)
   {
      fclose (f);
      return -1;
   }
   fclose (f);
   return 0;
}

void pnal_clear_file (const char * fullpath)
{
   (void) remove (fullpath);
}

int pnal_load_file (
   const char * fullpath,
   void * object_1,
   size_t size_1,
   void * object_2,
   size_t size_2)
{
   FILE * f = fopen (fullpath, "rb");
   if (f == NULL)
   {
      return -1;   /* absent is normal on first boot -- not an error to log */
   }
   if (size_1 > 0 && object_1 != NULL && fread (object_1, size_1, 1, f) != 1)
   {
      fclose (f);
      return -1;
   }
   if (size_2 > 0 && object_2 != NULL && fread (object_2, size_2, 1, f) != 1)
   {
      fclose (f);
      return -1;
   }
   fclose (f);
   return 0;
}

/* ── Buffers ─────────────────────────────────────────────────────────────── */
/* pnal_buf_header() moves payload backwards/forwards to prepend or strip a
 * header without copying, so the allocation is tracked separately from the
 * payload pointer.  Headroom is reserved up front for exactly this. */

#define PNAL_BUF_HEADROOM 32

pnal_buf_t * pnal_buf_alloc (uint16_t length)
{
   pnal_buf_t * p = (pnal_buf_t *) malloc (sizeof (pnal_buf_t));
   if (p == NULL)
   {
      return NULL;
   }
   p->_alloc_size = (uint16_t) (length + PNAL_BUF_HEADROOM);
   p->_alloc = (uint8_t *) malloc (p->_alloc_size);
   if (p->_alloc == NULL)
   {
      free (p);
      return NULL;
   }
   p->payload = p->_alloc + PNAL_BUF_HEADROOM;
   p->len = length;
   return p;
}

void pnal_buf_free (pnal_buf_t * p)
{
   if (p == NULL)
   {
      return;
   }
   free (p->_alloc);
   free (p);
}

uint8_t pnal_buf_header (pnal_buf_t * p, int16_t header_size_increment)
{
   uint8_t * new_payload;

   if (p == NULL)
   {
      return 255;
   }
   new_payload = (uint8_t *) p->payload - header_size_increment;
   if (new_payload < p->_alloc || new_payload > p->_alloc + p->_alloc_size)
   {
      return 255;   /* would run off the allocation */
   }
   p->payload = new_payload;
   p->len = (uint16_t) (p->len + header_size_increment);
   return 0;
}

/* ── Interface queries ───────────────────────────────────────────────────── */

static int pnal_ioctl_ifr (const char * interface_name, unsigned long req, struct ifreq * ifr)
{
   int s;
   int ret;

   s = socket (AF_INET, SOCK_DGRAM, 0);
   if (s < 0)
   {
      return -1;
   }
   memset (ifr, 0, sizeof (*ifr));
   snprintf (ifr->ifr_name, IFNAMSIZ, "%s", interface_name);
   ret = ioctl (s, req, ifr);
   close (s);
   return ret;
}

int pnal_get_interface_index (const char * interface_name)
{
   return (int) if_nametoindex (interface_name);
}

int pnal_get_macaddress (const char * interface_name, pnal_ethaddr_t * p_mac)
{
   struct ifreq ifr;

   if (pnal_ioctl_ifr (interface_name, SIOCGIFHWADDR, &ifr) != 0)
   {
      return -1;
   }
   memcpy (p_mac->addr, ifr.ifr_hwaddr.sa_data, 6);
   return 0;
}

static pnal_ipaddr_t pnal_ifr_ipaddr (const char * interface_name, unsigned long req)
{
   struct ifreq ifr;
   struct sockaddr_in * sin;

   if (pnal_ioctl_ifr (interface_name, req, &ifr) != 0)
   {
      return 0;
   }
   sin = (struct sockaddr_in *) &ifr.ifr_addr;
   return (pnal_ipaddr_t) ntohl (sin->sin_addr.s_addr);
}

pnal_ipaddr_t pnal_get_ip_address (const char * interface_name)
{
   return pnal_ifr_ipaddr (interface_name, SIOCGIFADDR);
}

pnal_ipaddr_t pnal_get_netmask (const char * interface_name)
{
   return pnal_ifr_ipaddr (interface_name, SIOCGIFNETMASK);
}

/* The default gateway is not an interface property; read it out of
 * /proc/net/route (destination 0.0.0.0 on this interface). */
pnal_ipaddr_t pnal_get_gateway (const char * interface_name)
{
   FILE * f;
   char line[256];
   pnal_ipaddr_t gw = 0;

   f = fopen ("/proc/net/route", "r");
   if (f == NULL)
   {
      return 0;
   }
   /* skip the header line */
   if (fgets (line, sizeof (line), f) == NULL)
   {
      fclose (f);
      return 0;
   }
   while (fgets (line, sizeof (line), f) != NULL)
   {
      char iface[64];
      unsigned long dest;
      unsigned long gateway;

      if (sscanf (line, "%63s %lx %lx", iface, &dest, &gateway) != 3)
      {
         continue;
      }
      if (dest == 0 && strcmp (iface, interface_name) == 0)
      {
         /* /proc/net/route is little-endian hex in network byte order */
         gw = (pnal_ipaddr_t) ntohl ((uint32_t) gateway);
         break;
      }
   }
   fclose (f);
   return gw;
}

int pnal_get_hostname (char * hostname)
{
   if (gethostname (hostname, PNAL_HOSTNAME_MAX_SIZE) != 0)
   {
      return -1;
   }
   hostname[PNAL_HOSTNAME_MAX_SIZE - 1] = '\0';
   return 0;
}

int pnal_get_ip_suite (
   const char * interface_name,
   pnal_ipaddr_t * p_ipaddr,
   pnal_ipaddr_t * p_netmask,
   pnal_ipaddr_t * p_gw,
   char * hostname)
{
   *p_ipaddr = pnal_get_ip_address (interface_name);
   *p_netmask = pnal_get_netmask (interface_name);
   *p_gw = pnal_get_gateway (interface_name);
   return pnal_get_hostname (hostname);
}

/* DCP Set-IP from the controller: the commissioning step where an engineering
 * tool assigns this device its address.  Implemented with SIOCSIFADDR and
 * friends, which need CAP_NET_ADMIN (CAP_NET_RAW alone is not enough).
 *
 * Was deliberately stubbed to -1 at first, on the grounds that faking success
 * would report a commissioning result that never happened.  That turned out to
 * be load-bearing: p-net logs "CMINA(448): Failed to set network parameters"
 * and the device does not come up properly for DCP.  Better to implement it.
 *
 * `permanent` means the controller wants the address to survive a power cycle.
 * We do not write it to a config file here -- p-net already persists its own
 * view via pnal_save_file, and rewriting the host's network configuration
 * permanently is a policy decision for the integrator, not this port.  The
 * address is applied to the running interface either way. */
/* DCP commissioning notification.  p-net has NO callback for "the controller
 * changed my name / IP" -- the only place the application can observe it is
 * here, because CMINA commits the change through pnal_set_ip_suite() and
 * passes BOTH the address and the NameOfStation (as `hostname`).
 * Weak so that a build without a daemon (linktest) still links. */
__attribute__ ((weak)) void pnd_on_dcp_commission (
   const char * interface_name,
   uint32_t ip,
   uint32_t mask,
   uint32_t gw,
   const char * hostname,
   int permanent)
{
   (void) interface_name; (void) ip; (void) mask; (void) gw;
   (void) hostname; (void) permanent;
}

static int pnal_set_ifaddr (const char * ifname, unsigned long req, pnal_ipaddr_t addr)
{
   struct ifreq ifr;
   struct sockaddr_in * sin;
   int s;
   int ret;

   s = socket (AF_INET, SOCK_DGRAM, 0);
   if (s < 0)
   {
      return -1;
   }
   memset (&ifr, 0, sizeof (ifr));
   snprintf (ifr.ifr_name, IFNAMSIZ, "%s", ifname);
   sin = (struct sockaddr_in *) &ifr.ifr_addr;
   sin->sin_family = AF_INET;
   sin->sin_addr.s_addr = htonl (addr);
   ret = ioctl (s, req, &ifr);
   close (s);
   return ret;
}

int pnal_set_ip_suite (
   const char * interface_name,
   const pnal_ipaddr_t * p_ipaddr,
   const pnal_ipaddr_t * p_netmask,
   const pnal_ipaddr_t * p_gw,
   const char * hostname,
   bool permanent)
{
   /* An all-zero suite is DCP "clear my address"; nothing to apply. */
   if (p_ipaddr == NULL || *p_ipaddr == 0)
   {
      return 0;
   }
   if (pnal_set_ifaddr (interface_name, SIOCSIFADDR, *p_ipaddr) != 0)
   {
      fprintf (
         stderr,
         "pnal_set_ip_suite: SIOCSIFADDR on %s failed: %s\n"
         "  (needs CAP_NET_ADMIN -- setcap 'cap_net_raw,cap_net_admin+ep', or run as root)\n",
         interface_name,
         strerror (errno));
      return -1;
   }
   if (p_netmask != NULL && *p_netmask != 0)
   {
      (void) pnal_set_ifaddr (interface_name, SIOCSIFNETMASK, *p_netmask);
   }
   /* The default route is a routing-table change, not an interface property;
    * left to the integrator rather than silently rewriting host routing. */
   pnd_on_dcp_commission (
      interface_name,
      *p_ipaddr,
      (p_netmask != NULL) ? *p_netmask : 0,
      (p_gw != NULL) ? *p_gw : 0,
      hostname,
      permanent ? 1 : 0);
   return 0;
}

int pnal_eth_get_status (const char * interface_name, pnal_eth_status_t * status)
{
   struct ifreq ifr;

   memset (status, 0, sizeof (*status));
   if (pnal_ioctl_ifr (interface_name, SIOCGIFFLAGS, &ifr) != 0)
   {
      return -1;
   }
   status->is_autonegotiation_supported = true;
   status->is_autonegotiation_enabled = true;
   status->autonegotiation_advertised_capabilities = 0;
   status->running = (ifr.ifr_flags & IFF_RUNNING) ? true : false;
   /* Reports 100BASE-TX full duplex when the link is up, UNKNOWN when down --
    * which is what pnal.h specifies UNKNOWN means.  Correct for the common
    * case and honest about what this port does not probe: the real negotiated
    * speed/duplex needs ETHTOOL_GLINKSETTINGS, not SIOCGIFFLAGS. */
   status->operational_mau_type =
      status->running ? PNAL_ETH_MAU_COPPER_100BaseTX_FULL_DUPLEX : PNAL_ETH_MAU_UNKNOWN;
   return 0;
}

int pnal_get_port_statistics (const char * interface_name, pnal_port_stats_t * port_stats)
{
   char path[256];
   struct
   {
      const char * name;
      uint32_t * dst;
   } fields[] = {
      {"rx_bytes", &port_stats->if_in_octets},
      {"tx_bytes", &port_stats->if_out_octets},
      {"rx_dropped", &port_stats->if_in_discards},
      {"tx_dropped", &port_stats->if_out_discards},
      {"rx_errors", &port_stats->if_in_errors},
      {"tx_errors", &port_stats->if_out_errors},
   };
   size_t i;

   memset (port_stats, 0, sizeof (*port_stats));
   for (i = 0; i < sizeof (fields) / sizeof (fields[0]); i++)
   {
      FILE * f;
      unsigned long long v = 0;

      snprintf (
         path,
         sizeof (path),
         "/sys/class/net/%s/statistics/%s",
         interface_name,
         fields[i].name);
      f = fopen (path, "r");
      if (f == NULL)
      {
         return -1;
      }
      if (fscanf (f, "%llu", &v) == 1)
      {
         *fields[i].dst = (uint32_t) v;
      }
      fclose (f);
   }
   return 0;
}

/* ── Raw Ethernet ────────────────────────────────────────────────────────── */

static void * pnal_eth_rx_thread (void * arg)
{
   pnal_eth_handle_t * h = (pnal_eth_handle_t *) arg;

   if (getenv ("PND_RX_DEBUG") != NULL)
   {
      fprintf (stderr, "pnal rx thread START if=%s sock=%d proto=0x%04X\n",
               h->if_name, h->socket, h->receive_type);
   }
   while (h->running)
   {
      pnal_buf_t * buf;
      ssize_t n;

      buf = pnal_buf_alloc (PNAL_BUF_MAX_SIZE);
      if (buf == NULL)
      {
         continue;
      }
      n = recv (h->socket, buf->payload, PNAL_BUF_MAX_SIZE, 0);
      if (n < 0 && getenv ("PND_RX_DEBUG") != NULL && errno != EAGAIN &&
          errno != EWOULDBLOCK && errno != EINTR)
      {
         fprintf (stderr, "pnal rx: recv error %s\n", strerror (errno));
      }
      if (n <= 0)
      {
         pnal_buf_free (buf);
         if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
         {
            continue;
         }
         continue;
      }
      buf->len = (uint16_t) n;
      if (getenv ("PND_RX_DEBUG") != NULL)
      {
         fprintf (stderr, "pnal rx: %s %d bytes, ethertype 0x%02X%02X, cb=%p\n",
                  h->if_name, (int) n,
                  ((uint8_t *) buf->payload)[12], ((uint8_t *) buf->payload)[13],
                  (void *) h->callback);
      }
      /* The callback takes ownership of buf. */
      if (h->callback != NULL)
      {
         h->callback (h, h->arg, buf);
      }
      else
      {
         pnal_buf_free (buf);
      }
   }
   return NULL;
}

pnal_eth_handle_t * pnal_eth_init (
   const char * if_name,
   pnal_ethertype_t receive_type,
   const pnal_cfg_t * pnal_cfg,
   pnal_eth_callback_t * callback,
   void * arg)
{
   pnal_eth_handle_t * h;
   struct sockaddr_ll sll;
   struct timeval tv;
   int ifindex;
   /* p-net signals "give me everything" with PNAL_ETHTYPE_ALL (0xFFFF), NOT 0.
    * Binding an AF_PACKET socket to ethertype 0xFFFF matches no frame that
    * exists, so the receive thread saw nothing at all and the device never
    * answered a DCP Identify -- silently, because every layer reported success.
    * Found by instrumenting the rx path; a wire capture showed the request
    * arriving and no reply. */
   const uint16_t proto =
      (receive_type == PNAL_ETHTYPE_ALL || receive_type == 0)
         ? ETH_P_ALL
         : (uint16_t) receive_type;

   (void) pnal_cfg;

   ifindex = (int) if_nametoindex (if_name);
   if (ifindex == 0)
   {
      return NULL;
   }
   h = (pnal_eth_handle_t *) calloc (1, sizeof (*h));
   if (h == NULL)
   {
      return NULL;
   }
   h->socket = socket (AF_PACKET, SOCK_RAW, htons (proto));
   if (h->socket < 0)
   {
      /* Almost always CAP_NET_RAW.  Say so rather than failing mutely. */
      fprintf (
         stderr,
         "pnal_eth_init: AF_PACKET socket failed on %s: %s\n"
         "  (needs CAP_NET_RAW -- setcap cap_net_raw+ep <binary>, or run as root)\n",
         if_name,
         strerror (errno));
      free (h);
      return NULL;
   }
   h->if_index = ifindex;
   h->receive_type = proto;
   h->callback = callback;
   h->arg = arg;
   snprintf (h->if_name, sizeof (h->if_name), "%s", if_name);

   memset (&sll, 0, sizeof (sll));
   sll.sll_family = AF_PACKET;
   sll.sll_protocol = htons (proto);
   sll.sll_ifindex = ifindex;
   if (bind (h->socket, (struct sockaddr *) &sll, sizeof (sll)) != 0)
   {
      close (h->socket);
      free (h);
      return NULL;
   }

   /* A receive timeout keeps the rx thread able to observe h->running so the
    * daemon can shut down; a blocking recv would pin it forever. */
   tv.tv_sec = 0;
   tv.tv_usec = 100000;
   setsockopt (h->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));

   if (getenv ("PND_RX_DEBUG") != NULL)
   {
      fprintf (stderr, "pnal_eth_init: if=%s receive_type=0x%04X proto=0x%04X ifindex=%d sock=%d\n",
               if_name, receive_type, proto, ifindex, h->socket);
   }
   h->running = true;
   if (pthread_create (&h->rx_thread, NULL, pnal_eth_rx_thread, h) != 0)
   {
      close (h->socket);
      free (h);
      return NULL;
   }
   return h;
}

int pnal_eth_send (pnal_eth_handle_t * handle, pnal_buf_t * buf)
{
   ssize_t n;

   if (handle == NULL || buf == NULL)
   {
      return -1;
   }
   n = send (handle->socket, buf->payload, buf->len, 0);
   return (n < 0) ? -1 : (int) n;
}

/* ── UDP ─────────────────────────────────────────────────────────────────── */

int pnal_udp_open (pnal_ipaddr_t addr, pnal_ipport_t port)
{
   struct sockaddr_in sa;
   int id;
   int enable = 1;

   id = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (id < 0)
   {
      return -1;
   }
   setsockopt (id, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof (enable));

   memset (&sa, 0, sizeof (sa));
   sa.sin_family = AF_INET;
   sa.sin_addr.s_addr = htonl (addr);
   sa.sin_port = htons (port);
   if (bind (id, (struct sockaddr *) &sa, sizeof (sa)) != 0)
   {
      close (id);
      return -1;
   }
   /* Non-blocking: the p-net core polls. */
   fcntl (id, F_SETFL, fcntl (id, F_GETFL, 0) | O_NONBLOCK);
   return id;
}

int pnal_udp_sendto (
   uint32_t id,
   pnal_ipaddr_t dst_addr,
   pnal_ipport_t dst_port,
   const uint8_t * data,
   int size)
{
   struct sockaddr_in sa;
   ssize_t n;

   memset (&sa, 0, sizeof (sa));
   sa.sin_family = AF_INET;
   sa.sin_addr.s_addr = htonl (dst_addr);
   sa.sin_port = htons (dst_port);
   n = sendto ((int) id, data, (size_t) size, 0, (struct sockaddr *) &sa, sizeof (sa));
   return (n < 0) ? -1 : (int) n;
}

int pnal_udp_recvfrom (
   uint32_t id,
   pnal_ipaddr_t * src_addr,
   pnal_ipport_t * src_port,
   uint8_t * data,
   int size)
{
   struct sockaddr_in sa;
   socklen_t sa_len = sizeof (sa);
   ssize_t n;

   memset (&sa, 0, sizeof (sa));
   n = recvfrom ((int) id, data, (size_t) size, 0, (struct sockaddr *) &sa, &sa_len);
   if (n < 0)
   {
      return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
   }
   *src_addr = (pnal_ipaddr_t) ntohl (sa.sin_addr.s_addr);
   *src_port = (pnal_ipport_t) ntohs (sa.sin_port);
   return (int) n;
}

void pnal_udp_close (uint32_t id)
{
   close ((int) id);
}

/* ── SNMP ────────────────────────────────────────────────────────────────── */
/* Stubbed.  SNMP + LLDP-MIB is a Conformance Class B requirement; CC-A does
 * not need it.  Returning 0 (success) is deliberate: p-net treats a failure
 * here as fatal, and a CC-A device is legitimately complete without SNMP.
 * Implementing it means an agent serving MIB-II and the LLDP-MIB. */
int pnal_snmp_init (pnet_t * net, const pnal_cfg_t * pnal_cfg)
{
   (void) net;
   (void) pnal_cfg;
   return 0;
}
