/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Zackary Andraka <zackary.andraka@avnet.com> et al.
 */

#ifndef IOTCONNECT_RNWF11_H
#define IOTCONNECT_RNWF11_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint16_t motor_running;
    uint16_t state;
    uint16_t sector;
    uint16_t requested_speed_rpm;
    uint16_t measured_speed_rpm;
    int16_t requested_current;
    int16_t measured_current;
    int16_t duty_cycle;
    int16_t dc_bus_adc;
} IotcRnwf11Telemetry;

void IOTC_RNWF11_Initialize(void);
void IOTC_RNWF11_Tick1ms(void);
void IOTC_RNWF11_SetTelemetry(const IotcRnwf11Telemetry *sample);
void IOTC_RNWF11_Task(void);
bool IOTC_RNWF11_IsConnected(void);

// Non-blocking per-loop check for an incoming provisioning request
// (tools/provision_device_config.py/.ps1) on the debug console. Only reads
// bytes already sitting in UART1's receive buffer, so it never adds
// unbounded latency to the caller's loop, safe to call every iteration
// alongside IOTC_RNWF11_Task(). A completed provisioning exchange briefly
// blocks (see provisioning.c), which is fine here since motor control runs
// entirely in the ADC ISR, independent of this superloop.
void IOTC_RNWF11_CheckProvisioning(void);

#ifdef __cplusplus
}
#endif

#endif
