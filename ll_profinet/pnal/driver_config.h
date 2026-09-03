/* Copyright 2026 by Frobenius Norm LLC 2026-08-22
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GPLv3 -- part of the w3_profinet daemon.  Never link into liblamblisp.a.
 *
 * P145 Part B -- port header #2 of 3.  Required by pnet_api.h.
 *
 * The only driver in the public p-net drop is src/drivers/lan9662 (a Microchip
 * switch with hardware IRT offload).  This port drives a plain AF_PACKET
 * socket, so no driver is configured.
 */
#ifndef DRIVER_CONFIG_H
#define DRIVER_CONFIG_H

#define PNET_OPTION_DRIVER_ENABLE 0

#endif /* DRIVER_CONFIG_H */
