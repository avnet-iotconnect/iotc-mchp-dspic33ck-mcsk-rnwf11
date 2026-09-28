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

#define SCOPE_BUFFER_MAX 1000U
#define SCOPE_LENGTH_MIN 10U

// The true achievable range of SCOPE_SetRateMicroseconds() - 1 ADC tick
// (50us, the fastest possible) to 0xFFFF ticks (the counter's width), the
// same bounds that function's own clamping enforces internally (bldc_main.c).
// Exposed here so IOTC_RNWF11_OnCommand() (iotconnect_rnwf11.c, a different
// translation unit) can REJECT an out-of-range scope-rate command with a
// precise message instead of silently accepting it and letting the setter
// clamp it to a different value than what was asked for.
#define SCOPE_RATE_US_MIN 50UL
#define SCOPE_RATE_US_MAX 3276750UL

// Channel indices for SCOPE_SetChannel()/SCOPE_GetChannel().
#define SCOPE_CHANNEL_VDC    0U // DC bus voltage, raw ADC (default)
#define SCOPE_CHANNEL_DUTY   1U // PWM duty cycle, raw compare count
#define SCOPE_CHANNEL_IBUS   2U // bus current, raw ADC
#define SCOPE_CHANNEL_IA     3U // phase A current (Op Amp 1 / AN0), raw ADC
#define SCOPE_CHANNEL_IB     4U // phase B current (Op Amp 2 / AN1), raw ADC
// Measured speed is deliberately not a channel: it only updates once per
// electrical revolution (hall interrupt), too slow to be worth scoping - it's
// already in the regular telemetry as "spd".
#define SCOPE_CHANNEL_MAX    SCOPE_CHANNEL_IB

void SCOPE_SetChannel(uint8_t channel);       // clamps to 0-SCOPE_CHANNEL_MAX (SCOPE_CHANNEL_*)
void SCOPE_SetRateMicroseconds(uint32_t us);  // rounds to the nearest 50us ADC tick, clamps to [SCOPE_RATE_US_MIN, SCOPE_RATE_US_MAX]
void SCOPE_SetLength(uint16_t length);        // clamps to [SCOPE_LENGTH_MIN, SCOPE_BUFFER_MAX]
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
