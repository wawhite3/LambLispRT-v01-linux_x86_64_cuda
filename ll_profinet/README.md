# w3_profinet — the PROFINET daemon (GPLv3)

**This directory is GPLv3. Nothing in it may be linked into `liblamblisp.a`,
`LambLisp.bin`, or any `LambLispRT-v01-<env>` package.**

## Why it is a separate process

PN-GATE (P145) was decided 2026-08-20: use the **public p-net sources**, free
arm, no licence fee. p-net's `LICENSE.md` grants this explicitly:

> Use of IPC interface(s) provided by the project is understood to be ordinary
> use, not within the meaning of "covered work" as defined in the GPLv3 license.

So the stack runs here, in its own process, and LambLisp talks to it over the
local socket defined in `src/ll_vm_profinet_ipc.h`. LambLisp does not become a
covered work, and the shipped packages keep their existing licence.

This is a **legal** boundary, not a stylistic one. Linking p-net directly into
the VM would convert a proprietary product into a GPLv3 one. If you are
tempted to "simplify" by linking it, stop and re-read this paragraph.

The architecture is also what P145 argued for on technical grounds — C++ owns
the wire deadline, Scheme owns policy — so the carve-out costs nothing in
design terms. LambLisp's own `defined?`-style boundary discipline already
assumes it.

## What upstream actually gives you

Measured 2026-08-20 by cloning and compiling, not read off the README:

| Component | Repo | Licence | Size |
|---|---|---|---|
| p-net protocol core | `github.com/rtlabs-com/p-net` branch `public` | **GPLv3** | 88 files, 59,299 LOC, 35 core `.c` |
| OS abstraction | `github.com/rtlabs-com/osal` | **BSD-3-Clause** | 51 files, 8,626 LOC |

The public p-net drop is the **protocol core only**. It does **not** build as
shipped. Absent: `CMakeLists.txt`, any sample app, the `ports/` directory,
`pnal_sys.h`, `pnal_cfg_t`, and every one of the **24 `pnal_*` implementations**.
`include/pnal.h` publishes the *contract*; you write the implementation.

Verified by compiling: all 35 core translation units fail without a port, and
after synthesising the CMake-generated `pnet_export.h` and a minimal
`pnal_config.h` they stop at `pnal_sys.h`.

`osal` is BSD, so it carries no copyleft — only the p-net core does.

## Remaining work (the honest list)

1. **`pnal` port** — `pnal_sys.h`, `pnal_cfg_t`, and 24 functions:
   raw Ethernet (`pnal_eth_init/send`), UDP (`open/close/sendto/recvfrom`),
   IP suite get/set, MAC + interface index, hostname, gateway, netmask,
   port statistics, `pnal_snmp_init` (stubbable for Conformance Class A),
   file `load/save/clear`, buffer `alloc/free/header`, uptime.
   Linux: `AF_PACKET` + sockets + netlink. **~4–6 days.**
2. **Build** — no CMake needed; these are plain C sources. Either a small
   `Makefile` here or a pio env that is never released.
3. **IPC server** — implement the `ll_vm_profinet_ipc.h` protocol on top of
   `pnet_api.h` callbacks. The mock (`w3_ai_scripts/profinet_mock_daemon.py`)
   is the reference for the protocol and already passes 25 LambLisp-side tests.
4. **Bring-up** — a soft-PLC IO-Controller, then a real S7-1200. **~3 days.**

## Certification is still required

Writing or vendoring the stack changes nothing about PI. A Vendor ID (free), a
GSDML matching the firmware device model, and Test Lab certification per
Conformance Class remain mandatory before anything may be sold or marketed as
PROFINET. Until then the wording is "PROFINET-compatible (uncertified)".

## Getting the sources

Not vendored here deliberately — pulling 59k lines of GPLv3 into this repo is a
decision to make on purpose, not a side effect of a scaffold:

    git clone --depth 1 https://github.com/rtlabs-com/p-net.git
    git clone --depth 1 https://github.com/rtlabs-com/osal.git

---

## `pnal/` — the platform port (written 2026-08-22)

`include/pnal.h` declares 24 functions and p-net publishes no implementation.
`pnal/` is that implementation for Linux.

| File | Purpose |
|---|---|
| `pnal_config.h` | `pnal_cfg_t`, `pnal_thread_cfg_t` — pulled in by `pnet_api.h` |
| `driver_config.h` | required by `pnet_api.h`; no driver (plain AF_PACKET) |
| `pnal_sys.h` | `pnal_buf_t`, `struct pnal_eth_handle` — pulled in by `pnal.h` |
| `pnal_linux.c` | all 24 functions |
| `pnet_options.h.example` | a working option set (see below) |

**Include order is load-bearing and not obvious:**

    pnal.h -> pnet_api.h -> pnal_config.h   (pnal_cfg_t must exist here)
           -> pnal_sys.h                    (pnal_buf_t, eth handle)

`pnet_api.h` embeds `pnal_cfg_t pnal_cfg;` inside `pnet_cfg_t`, so `pnal_cfg_t`
must be defined *before* `pnal_sys.h` is reached. That is why the port is three
headers, not one.

### Implementation notes

- **Raw Ethernet** — one `AF_PACKET` socket per interface plus a receive thread
  that hands frames to p-net's callback. `SO_RCVTIMEO` of 100 ms so the thread
  can observe `running` and shut down; a blocking `recv` would pin it forever.
  **Needs `CAP_NET_RAW`** (`setcap cap_net_raw+ep`, or run as root) — the init
  failure message says so rather than failing mutely.
- **Buffers** — `pnal_buf_header()` slides `payload` to prepend/strip headers
  without copying, so the allocation is tracked separately (`_alloc`) and
  32 bytes of headroom are reserved up front.
- **Gateway** — not an interface property; read from `/proc/net/route`.
- **Port statistics** — `/sys/class/net/<if>/statistics/*`.
- **`pnal_set_ip_suite` returns −1 deliberately.** DCP Set-IP from the
  controller needs root plus `SIOCSIFADDR` or a netlink/NetworkManager
  conversation. Silently returning success would make a device that reports a
  commissioning result it did not perform. **This is the first thing to
  implement for real DCP commissioning.**
- **`pnal_snmp_init` is a stub returning 0.** SNMP + LLDP-MIB is a Conformance
  Class B requirement; CC-A does not need it, and p-net treats a failure here
  as fatal.

### Verification status

Compiles clean under `-Wall -Wextra` against p-net's real headers, and **32 of
35 p-net core translation units compile against it** (0 of 35 before the port
existed). The remaining 3 are *configuration*, not port defects:

    pf_snmp.c   needs the SNMP option coherently set (or the file excluded)
    pf_cmdev.c  an option-gated struct member
    pf_cmrpc.c  #error "There must be at least 2 C..."  -- raise a MAX option

`pnet_options.h.example` is the working set derived by hand, because the public
drop has no `CMakeLists.txt` and therefore no option defaults. Two values were
found by hitting the guard rails: `PNET_MAX_ALARM_PAYLOAD_DATA_SIZE` must be
≤ 1408 and `PNET_MAX_DIAG_MANUF_DATA_SIZE` ≤ 1396.

### It links, and p-net runs on it (2026-08-22)

`build.sh` builds p-net + osal + this port into `libprofinet.a` (37 objects)
and links `linktest.c`, which calls the real `pnet_init()`:

    $ sudo setcap cap_net_raw+ep build/linktest
    $ build/linktest
    pnet_cfg_t size      : 984 bytes
    calling pnet_init()  : [ERROR] CMINA(448): Failed to set network parameters
    returned a handle

**`pnet_init()` returns a handle.** All 34 core translation units compile,
every symbol resolves, p-net's own configuration validator passes, and p-net
calls into this port to bring the interface up.

The remaining error is the honest one: `CMINA: Failed to set network
parameters` is p-net calling `pnal_set_ip_suite()`, which returns -1 on
purpose. Implementing it is the next task.

Getting there also confirmed the port is genuinely exercised rather than merely
linked: before `CAP_NET_RAW` was granted, the failure was this port's own
diagnostic --

    pnal_eth_init: AF_PACKET socket failed on lo: Operation not permitted
      (needs CAP_NET_RAW -- setcap cap_net_raw+ep <binary>, or run as root)

-- i.e. p-net reached `pnal_eth_init()`, which behaved as designed.

Config values p-net demanded along the way, recorded so nobody re-derives them:
`tick_us` (1000), `if_cfg.main_netif_name`, `physical_ports[0].netif_name`,
`num_physical_ports`, and `min_device_interval` = **32**, which is 1 ms in
PROFINET's 31.25 us cycle units.

### First PROFINET frame on the wire (2026-08-22)

    $ sudo ip link add pn0 type veth peer name pn1 && sudo ip link set pn0 up && sudo ip link set pn1 up
    $ sudo w3_profinet/build/pnd --if pn0 --station lamb-io-1 &
    $ sudo python3 w3_ai_scripts/profinet_dcp_probe.py --if pn1

    DCP Identify RESPONSE from ba:df:45:53:ad:b2  (FrameID 0xFEFF, xid 0x0F000001)
       IPParameter    ip 0.0.0.0 mask 0.0.0.0 gw 0.0.0.0
       DeviceVendor
       NameOfStation  lamb-io-1
       DeviceID       vendor 0x0493 device 0x0002
       DeviceRole     0x01

That is the exchange an engineering tool performs when it scans a segment. No
PLC, no hardware, no licence -- a veth pair is enough.

### The bug that found: `PNAL_ETHTYPE_ALL` is 0xFFFF, not 0

The port bound its `AF_PACKET` socket with

    proto = (receive_type == 0) ? ETH_P_ALL : receive_type;

but p-net asks for everything with **`PNAL_ETHTYPE_ALL` = 0xFFFF**
(`pnal.h:121`). The socket was therefore bound to ethertype 0xFFFF, which no
frame on earth carries, so the receive thread saw nothing and the device never
answered -- **silently, because every layer reported success**: `pnet_init()`
returned a handle, the ticks incremented, the IPC worked, and LambLisp reported
`dcp-only`. Only a wire capture showed the request arriving with no reply.

Localised by instrumenting `pnal_eth_init` to print what p-net actually passed:
`receive_type=0xFFFF`. Guessing would not have found it; the fix is one line and
the diagnosis was the whole job. `PND_RX_DEBUG=1` leaves that instrumentation
available.

### DCP commissioning, end to end into Scheme (2026-08-22)

    $ sudo python3 w3_ai_scripts/profinet_dcp_probe.py --if pn1 \
          --set-name lamb-from-tia --set-ip 192.168.99.88/255.255.255.0/192.168.99.1
    Set response: ServiceID 4 ServiceType 1  (OK)

    daemon:  DCP set NameOfStation = 'lamb-from-tia'
             DCP set IP = 192.168.99.88

    LambLisp: EVENT (dcp-set-name lamb-from-tia)
              EVENT (dcp-set-ip #bytevector(12 ...))

The device applies both -- a re-Identify returns the new name, and `ip addr`
shows the interface really carrying 192.168.99.88/24 because
`pnal_set_ip_suite` did it. This is what happens when an engineering tool
commissions a device, and the Scheme application is told about it.

**p-net has no callback for a DCP name/IP change.** The only observation point
is `pnal_set_ip_suite()`, because CMINA commits the change through it and
passes the NameOfStation as the `hostname` argument. The port therefore calls a
weak `pnd_on_dcp_commission()` hook which the daemon implements; weak so a build
without a daemon still links. If p-net ever grows a proper callback, that hook
is the one place to change.

### Two deployment bugs this test found

- **The IPC socket was root-owned 0755.** The daemon needs CAP_NET_RAW and
  CAP_NET_ADMIN; LambLisp does not run as root, so `connect()` got EACCES and
  the failure looked like "no daemon" rather than "wrong permissions". The
  daemon now chmods the socket. A real deployment should use a dedicated group
  and 0660 instead.
- **Events must survive the client being absent.** Commissioning happens
  whether or not LambLisp is connected, so the daemon queues into a ring that a
  later client drains -- which is exactly how the test above works.

**Not yet done:** a run against a real IO-Controller, i.e. Connect -> parameter
end -> cyclic data exchange. Discovery and commissioning work; nothing has
exchanged cyclic I/O with a PLC.
