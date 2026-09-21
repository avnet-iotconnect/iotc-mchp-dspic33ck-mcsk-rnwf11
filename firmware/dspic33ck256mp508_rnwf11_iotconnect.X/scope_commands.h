#ifndef SCOPE_COMMANDS_H
#define	SCOPE_COMMANDS_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Software oscilloscope capture, driven by IoTConnect C2D commands - see
// IOTC_RNWF11_OnCommand() in iotconnect/iotconnect_rnwf11.c. Defined in
// bldc_main.c (the sampling tick runs in HAL_MC1ADCInterrupt() there). Kept
// in its own header (rather than bldc_main.h) for the same reason as
// motor_commands.h: including bldc_main.h from a second translation unit
// would duplicate its file-scope PWM_STATE* array definitions.

#define SCOPE_BUFFER_MAX 100U

// Channel indices for SCOPE_SetChannel()/SCOPE_GetChannel().
#define SCOPE_CHANNEL_VDC    0U // DC bus voltage, raw ADC (default)
#define SCOPE_CHANNEL_SPEED  1U // measured speed, RPM
#define SCOPE_CHANNEL_DUTY   2U // PWM duty cycle, raw compare count
#define SCOPE_CHANNEL_IBUS   3U // bus current, raw ADC
#define SCOPE_CHANNEL_IA     4U // phase A current (Op Amp 1 / AN0), raw ADC
#define SCOPE_CHANNEL_IB     5U // phase B current (Op Amp 2 / AN1), raw ADC
#define SCOPE_CHANNEL_MAX    SCOPE_CHANNEL_IB

void SCOPE_SetChannel(uint8_t channel);       // clamps to 0-SCOPE_CHANNEL_MAX (SCOPE_CHANNEL_*)
void SCOPE_SetRateMicroseconds(uint32_t us);  // rounds to the nearest 50us ADC tick, clamps to >=1 tick
void SCOPE_SetLength(uint16_t length);        // clamps to [10, SCOPE_BUFFER_MAX]
void SCOPE_StartCapture(void);                // resets the buffer and arms a new one-shot capture

bool SCOPE_IsReady(void);                     // true once an armed capture has filled its buffer
void SCOPE_ClearReady(void);                  // call after publishing a ready capture

uint8_t SCOPE_GetChannel(void);
uint32_t SCOPE_GetRateMicroseconds(void);     // actual achieved rate (tick-rounded), not necessarily the last requested value
uint16_t SCOPE_GetLength(void);
int16_t SCOPE_GetSample(uint16_t index);      // valid for index < SCOPE_GetLength(), once SCOPE_IsReady()

#ifdef	__cplusplus
}
#endif

#endif	/* SCOPE_COMMANDS_H */
