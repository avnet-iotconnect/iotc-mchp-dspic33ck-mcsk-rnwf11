/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Zackary Andraka <zackary.andraka@avnet.com> et al.
 */

#ifndef UART2_H
#define UART2_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// UART2 carries the RNWF11 AT link on the mikroBUS B header.

void UART2_Initialize(uint32_t baud);
void UART2_Write(uint8_t data);
bool UART2_IsTransmitComplete(void);

bool UART2_IsOverrun(void); // latches after a receive overflow and blocks reception until cleared
void UART2_ClearOverrun(void);
bool UART2_IsReceiveDataAvailable(void);
uint8_t UART2_Read(void);
void UART2_ReceiveFlush(void);

#ifdef __cplusplus
}
#endif

#endif /* UART2_H */
