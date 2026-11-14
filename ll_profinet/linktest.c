/* P145 Part B -- link proof: does p-net + osal + our pnal port actually
 * produce a working binary?  Calls pnet_init() for real. */
#include "pnet_api.h"
#include <stdio.h>
#include <string.h>

int main (void)
{
   pnet_cfg_t cfg;
   pnet_t * net;

   memset (&cfg, 0, sizeof (cfg));
   cfg.pnal_cfg.bg_worker_thread.prio = 5;
   cfg.pnal_cfg.bg_worker_thread.stack_size = 4096;
   cfg.tick_us = 1000;
   /* main_netif_name is a `const char *` -- strlen() on the zeroed config was
    * the segfault, i.e. p-net's own validator, not the port. */
   cfg.if_cfg.main_netif_name = "lo";
   cfg.if_cfg.physical_ports[0].netif_name = "lo";
   cfg.if_cfg.physical_ports[0].default_mau_type = 0x0010;
   snprintf (cfg.station_name, sizeof (cfg.station_name), "lamb-io-1");
   cfg.num_physical_ports = 1;
   /* 32 x 31.25 us = 1 ms -- the PROFINET cycle unit. */
   cfg.min_device_interval = 32;

   printf ("pnet_cfg_t size      : %zu bytes\n", sizeof (pnet_cfg_t));
   printf ("calling pnet_init()  : ");
   fflush (stdout);
   net = pnet_init (&cfg);
   printf ("%s\n", net ? "returned a handle" : "returned NULL");
   return 0;
}
