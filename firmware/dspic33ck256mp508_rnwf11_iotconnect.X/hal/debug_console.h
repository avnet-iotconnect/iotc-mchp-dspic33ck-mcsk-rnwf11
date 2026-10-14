/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Zackary Andraka <zackary.andraka@avnet.com> et al.
 */

#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

// Blocking text console on UART1 (RD13/RD14), reachable through the PKoB4
// virtual COM port on connector J13. Not safe to call from an ISR.

#define DEBUG_CONSOLE_BAUD 115200UL

void DEBUG_Initialize(void);
void DEBUG_Printf(const char *format, ...);

void DEBUG_Flush(void); // blocks until the last character has left the shift register

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_CONSOLE_H */
