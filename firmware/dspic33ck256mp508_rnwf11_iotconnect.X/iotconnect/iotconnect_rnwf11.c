#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <xc.h>
#include "clock.h"
#include <libpic30.h> /* needs FCY from clock.h */
#include "debug_console.h"
#include "uart2.h"
#include "iotconnect_rnwf11.h"
#include "iotconnect_rnwf11_config.h"
#include "iotcl.h"
#include "iotcl_telemetry.h"
#include "../hal/device_config.h"
#include "../hal/nvm_flash.h"
#include "../motor_commands.h"
#include "../scope_commands.h"
#include "provisioning.h"

#define IOTC_RNWF11_RESPONSE_SIZE 384U
#define IOTC_RNWF11_COMMAND_TIMEOUT 5000000UL
#define IOTC_RNWF11_PROBE_TIMEOUT 200000UL
#define IOTC_RNWF11_WIFI_WAIT_MS 20000UL

static bool iotcConnected;
static bool iotcModulePresent;
static volatile uint32_t telemetryMilliseconds;
static volatile uint32_t retryMilliseconds;
static IOTC_RNWF11_Telemetry_t telemetry;
static char lastResponse[IOTC_RNWF11_RESPONSE_SIZE];
static bool lastOverrun;
static bool mqttLinkUp;
/* Assume up: the module stays associated across dsPIC resets and only
 * re-announces its IP on a fresh association. */
static bool netUp = true;

/* Incoming C2D messages (unsolicited "+MQTTSUBRX:" lines) are queued here by
 * PollEvents() and acted on later by Task(), never from inside PollEvents()
 * itself - see the "+MQTTSUBRX:" handling note in IOTC_RNWF11_HandleEventLine().
 * A queue rather than a single slot because Task() deliberately holds commands
 * back while a scope capture is being published (which can take many seconds
 * for a long capture), and more than one can arrive in that time. The module
 * inlines the payload directly in that line for messages under its read
 * threshold (AT+MQTTC=9, 128 bytes by default), which every C2D command this
 * firmware supports is well under. A payload at or above that threshold is not
 * handled (per Microchip's Appendix A.5, AT+MQTTSUBRD would be needed instead,
 * but that path is unverified against real hardware and unnecessary for our
 * short commands). */
#define IOTC_RNWF11_C2D_TOPIC_MAX 128U
#define IOTC_RNWF11_C2D_PAYLOAD_MAX 256U
#define IOTC_RNWF11_C2D_QUEUE_LEN 4U
static char c2dQueue[IOTC_RNWF11_C2D_QUEUE_LEN][IOTC_RNWF11_C2D_PAYLOAD_MAX];
static size_t c2dQueueLen[IOTC_RNWF11_C2D_QUEUE_LEN];
static uint8_t c2dQueueHead;
static uint8_t c2dQueueCount;

/* Free-running millisecond clock, driven from the ADC interrupt (see
 * IOTC_RNWF11_Tick1ms()) - unlike telemetryMilliseconds it is never reset. */
static volatile uint32_t msNow;

/* True from a QoS 1 AT+MQTTPUB being sent until its "+MQTTPUBACK" event is
 * seen in PollEvents(). The module rejects a second AT+MQTTPUB ("8.0,MQTT
 * Error") while one is still awaiting its ack - see IOTC_RNWF11_PublishStep().
 * pubAckSentMs is msNow when that publish went out. */
static volatile bool mqttPubAckPending;
static uint32_t pubAckSentMs;

/* Populated at boot from flash (see IOTC_RNWF11_Initialize()) if the
 * device has been provisioned via tools/provision_device_config.py/.ps1,
 * falling back to the compile-time IOTC_* defaults below otherwise - see
 * device_config.h. Updated in place by IOTC_RNWF11_CheckProvisioning()
 * whenever a new provisioning request comes in over the console. */
static device_config_t s_cfg;

static bool IOTC_RNWF11_IsConfigured(void)
{
    return (s_cfg.wifi_ssid[0] != '\0') &&
           (s_cfg.mqtt_broker_host[0] != '\0') &&
           (s_cfg.mqtt_client_id[0] != '\0') &&
           (s_cfg.mqtt_pub_topic[0] != '\0');
}

static void IOTC_RNWF11_UART2_Initialize(void)
{
    UART2_Initialize(IOTC_RNWF11_BAUD);
}

/* The module echoes every character, so keep draining while transmitting. */
static void IOTC_RNWF11_Collect(uint16_t *length)
{
    while (UART2_IsReceiveDataAvailable())
    {
        char c = (char)UART2_Read();
        if (*length < (IOTC_RNWF11_RESPONSE_SIZE - 1U))
        {
            lastResponse[*length] = c;
            (*length)++;
            lastResponse[*length] = '\0';
        }
    }
}

/* The module echoes every character; discard it so long commands still fit. */
static void IOTC_RNWF11_DiscardEcho(void)
{
    while (UART2_IsReceiveDataAvailable())
    {
        (void)UART2_Read();
    }
}

static void IOTC_RNWF11_Write(const char *text)
{
    while (*text != '\0')
    {
        UART2_Write((uint8_t)*text++);
        while (!UART2_IsTransmitComplete())
        {
            IOTC_RNWF11_DiscardEcho();
        }
        IOTC_RNWF11_DiscardEcho();
    }
}

/* msNow is 32 bits on a 16-bit CPU and written from an interrupt, so re-read
 * until two reads agree rather than risk a torn value. */
static uint32_t IOTC_RNWF11_Now(void)
{
    uint32_t a;
    uint32_t b;
    do
    {
        a = msNow;
        b = msNow;
    } while (a != b);
    return a;
}

static void IOTC_RNWF11_PollEvents(void);
static void IOTC_RNWF11_HandleEventLine(const char *line);
static void IOTC_RNWF11_OnCommand(IotclC2dEventData data);
static void IOTC_RNWF11_SendCmdAck(const char *ack_id, int status, const char *message);

/* Everything the module sends while we wait for an AT command's reply is
 * gathered into lastResponse by IOTC_RNWF11_Collect(), not parsed a line at a
 * time by PollEvents() - so a C2D message or publish ack that happens to land in
 * that window would otherwise be lost. Hand any such lines to the normal event
 * handler. Only these two kinds: the connection-state events (+WSTA*,
 * +MQTTCONN*) are deliberately left as they were, since Configure()'s flow was
 * tuned around them not being seen here. (A message arriving while the command
 * itself is still being transmitted is still lost - the module echoes the
 * command back and IOTC_RNWF11_Write() discards everything it reads then.) */
static void IOTC_RNWF11_RecoverSwallowedEvents(void)
{
    static char line[128];
    const char *p = lastResponse;

    while (*p != '\0')
    {
        const char *end = p;
        while ((*end != '\0') && (*end != '\r') && (*end != '\n'))
        {
            end++;
        }
        size_t n = (size_t)(end - p);
        if ((n > 0U) && (n < sizeof(line)) &&
            ((strncmp(p, "+MQTTSUBRX:", 11) == 0) || (strncmp(p, "+MQTTPUBACK", 11) == 0)))
        {
            memcpy(line, p, n);
            line[n] = '\0';
            IOTC_RNWF11_HandleEventLine(line);
        }
        p = end;
        while ((*p == '\r') || (*p == '\n'))
        {
            p++;
        }
    }
}

static bool IOTC_RNWF11_CommandWithTimeout(const char *command, uint32_t timeout)
{
    uint16_t length = 0;
    bool ok = false;

    /* Print anything pending rather than discarding a failure notification. */
    IOTC_RNWF11_PollEvents();

    lastResponse[0] = '\0';
    UART2_ReceiveFlush();
    lastOverrun = false;

    IOTC_RNWF11_Write(command);

    while (timeout-- != 0U)
    {
        IOTC_RNWF11_Collect(&length);
        if (UART2_IsOverrun())
        {
            lastOverrun = true;
            UART2_ClearOverrun();
        }
        if (strstr(lastResponse, "ERROR") != NULL)
        {
            break;
        }
        if (strstr(lastResponse, "OK") != NULL)
        {
            ok = true;
            break;
        }
    }
    IOTC_RNWF11_RecoverSwallowedEvents();
    return ok;
}

static bool IOTC_RNWF11_Command(const char *command)
{
    return IOTC_RNWF11_CommandWithTimeout(command, IOTC_RNWF11_COMMAND_TIMEOUT);
}

/* Labelled so failures are traceable without printing Wi-Fi credentials. */
static bool IOTC_RNWF11_Step(const char *label, const char *command)
{
    if (IOTC_RNWF11_Command(command))
    {
        return true;
    }
    DEBUG_Printf("IOTC: step %s failed%s, resp=[%s]\r\n",
                 label, lastOverrun ? " (rx overrun)" : "",
                 (lastResponse[0] != '\0') ? lastResponse : "<timeout>");
    return false;
}

/* Sends a query and prints whatever comes back, ignoring OK/ERROR. */
static void IOTC_RNWF11_Query(const char *command)
{
    uint16_t length = 0;

    lastResponse[0] = '\0';
    UART2_ReceiveFlush();
    IOTC_RNWF11_Write(command);

    for (uint16_t slice = 0; slice < 2000U; slice++)
    {
        __delay_us(250);
        IOTC_RNWF11_Collect(&length);
    }
    DEBUG_Printf("IOTC: query -> [%s]\r\n", lastResponse);
}

static void IOTC_RNWF11_Diagnose(void)
{
    static const char *const queries[] = {
        "AT+TIME\r\n",     /* confirm SNTP supplied a valid clock */
        "AT+TLSC=1\r\n",   /* did the certificate names actually stick */
        "AT+MQTTC\r\n",    /* host, port, client id, TLS selection */
    };

    DEBUG_Printf("IOTC: --- diagnostics ---\r\n");
    for (uint16_t i = 0; i < (sizeof(queries) / sizeof(queries[0])); i++)
    {
        IOTC_RNWF11_Query(queries[i]);
    }
    DEBUG_Printf("IOTC: --- end ---\r\n");
}

/* Expected to fail once the station is associated, so do not log it. */
static void IOTC_RNWF11_StepQuiet(const char *command)
{
    (void)IOTC_RNWF11_Command(command);
}

/* Best effort: the module may already keep time, so a failure is not fatal. */
static void IOTC_RNWF11_StepOptional(const char *label, const char *command)
{
    (void)IOTC_RNWF11_Step(label, command);
}

static bool IOTC_RNWF11_WaitForNetwork(void)
{
    netUp = false;
    /* Close any MQTT session first - tearing down WiFi out from under a
     * still-open MQTT session (below) leaves the module's own MQTT client
     * state confused, which was seen to make every later AT+MQTTCONN=1
     * fail instantly even after a fresh WiFi reassociation. Only the very
     * first connect attempt after a full power cycle ever worked. */
    IOTC_RNWF11_StepQuiet("AT+MQTTDISCONN\r\n");
    /* Force a real disconnect+reconnect: AT+WSTA=1 alone is a no-op if the
     * module is already associated from a prior boot (host resets/reflashes
     * don't power-cycle the module), which was seen to reach MQTTCONN in a
     * state that consistently fails - a genuinely fresh association is the
     * one thing that correlated with MQTTCONN actually succeeding. */
    IOTC_RNWF11_StepQuiet("AT+WSTA=0\r\n");
    IOTC_RNWF11_StepQuiet("AT+WSTA=1\r\n");

    for (uint32_t elapsed = 0; elapsed < IOTC_RNWF11_WIFI_WAIT_MS; elapsed++)
    {
        IOTC_RNWF11_PollEvents();
        if (netUp)
        {
            return true;
        }
        __delay_ms(1);
    }
    DEBUG_Printf("IOTC: step WiFi-IP failed\r\n");
    return false;
}

/* Subscriptions don't survive a fresh MQTTCONN (clean session), so this
 * has to be redone on every (re)connect, not just once at boot. */
static void IOTC_RNWF11_Subscribe(void)
{
    if (s_cfg.mqtt_c2d_topic[0] != '\0')
    {
        char command[192];
        snprintf(command, sizeof(command), "AT+MQTTSUB=\"%s\",1\r\n", s_cfg.mqtt_c2d_topic);
        (void)IOTC_RNWF11_Step("MQTTSUB", command);
    }
}

static bool IOTC_RNWF11_Configure(void)
{
    char command[256];

    /* Order matches the reference RNWF11 driver (main branch): WiFi is
     * brought up completely first as its own phase, then TLS+MQTT are
     * configured together right before connecting - not interleaved with
     * TLS config first, the way this file did before. */
    if (!IOTC_RNWF11_Step("AT", "AT\r\n")) return false;

    /* These are rejected while the station is already associated, which is fine. */
    snprintf(command, sizeof(command), "AT+WSTAC=1,\"%s\"\r\n", s_cfg.wifi_ssid);
    IOTC_RNWF11_StepQuiet(command);
    snprintf(command, sizeof(command), "AT+WSTAC=2,%u\r\n", IOTC_WIFI_SECURITY);
    IOTC_RNWF11_StepQuiet(command);
    snprintf(command, sizeof(command), "AT+WSTAC=3,\"%s\"\r\n", s_cfg.wifi_password);
    IOTC_RNWF11_StepQuiet(command);
    IOTC_RNWF11_StepQuiet("AT+WSTAC=4,0\r\n");
    if (!IOTC_RNWF11_WaitForNetwork()) return false;

    /* These are rejected once the module already has the identical value
     * persisted from an earlier provision_rnwf11_cert.py run (its own
     * non-volatile storage, unaffected by power cycling the host board) -
     * same "reject a no-op update" behavior already seen on WSTAC/SNTP, so
     * treat rejection as fine rather than aborting the whole sequence. */
    snprintf(command, sizeof(command), "AT+TLSC=1,1,\"%s\"\r\n", s_cfg.rnwf_ca_name);
    IOTC_RNWF11_StepQuiet(command);
    if (s_cfg.rnwf_cert_name[0] != '\0')
    {
        snprintf(command, sizeof(command), "AT+TLSC=1,2,\"%s\"\r\n", s_cfg.rnwf_cert_name);
        IOTC_RNWF11_StepQuiet(command);
    }
    if (s_cfg.rnwf_key_name[0] != '\0')
    {
        snprintf(command, sizeof(command), "AT+TLSC=1,3,\"%s\"\r\n", s_cfg.rnwf_key_name);
        IOTC_RNWF11_StepQuiet(command);
    }
    snprintf(command, sizeof(command), "AT+TLSC=1,5,\"%s\"\r\n", s_cfg.mqtt_broker_host);
    IOTC_RNWF11_StepQuiet(command);
    /* Field 8 is USE_ECC608 (use the module's secure-element key instead of
     * the uploaded device-key file), not a generic "verify" toggle - the
     * reference driver explicitly sends 0 here for the file-based cert path
     * used by provision_rnwf11_cert.py. Sending 1 makes the module try to
     * authenticate with its own secure-element identity instead of the
     * uploaded key matching the cert actually registered with AWS IoT.
     * TEMP DIAGNOSTIC: not resending this - it's already 0 in the module's
     * persisted config from an earlier successful run, and re-sending the
     * same value gets rejected with "0.6 Configuration Update Blocked"
     * every boot, which may be leaving the TLS config slot in a state that
     * breaks the MQTTCONN attempt right after it. */

    /* The module's SNTP client never produces a real synced clock on this
     * board (still investigating why), and the TLS stack validates AWS's
     * server certificate against whatever clock it has - a wildly wrong
     * one (module was seen free-running at ~year 2039/2096) makes a
     * perfectly valid server cert look expired/not-yet-valid and the
     * handshake gets rejected. AT+TIME=1,<unix> sets the clock directly,
     * bypassing SNTP - approximate is fine, cert validity windows span
     * years, not seconds. */
    IOTC_RNWF11_StepQuiet("AT+TIME=1,1788998400\r\n");

    snprintf(command, sizeof(command), "AT+MQTTC=1,\"%s\"\r\n", s_cfg.mqtt_broker_host);
    if (!IOTC_RNWF11_Step("MQTTC1-host", command)) return false;
    snprintf(command, sizeof(command), "AT+MQTTC=2,%u\r\n", s_cfg.mqtt_broker_port);
    if (!IOTC_RNWF11_Step("MQTTC2-port", command)) return false;
    snprintf(command, sizeof(command), "AT+MQTTC=3,\"%s\"\r\n", s_cfg.mqtt_client_id);
    if (!IOTC_RNWF11_Step("MQTTC3-clientid", command)) return false;
    /* Match the working RNWF11 flow: use the MQTT protocol version it sets. */
    if (!IOTC_RNWF11_Step("MQTTC8-protocol", "AT+MQTTC=8,3\r\n")) return false;
    /*
     * Always written, even when empty: the module keeps the previous value
     * across resets, and AWS rejects a CONNECT that carries a username.
     */
    snprintf(command, sizeof(command), "AT+MQTTC=4,\"%s\"\r\n", s_cfg.mqtt_username);
    IOTC_RNWF11_StepOptional("MQTTC4-user", command);
    IOTC_RNWF11_StepOptional("MQTTC5-pass", "AT+MQTTC=5,\"\"\r\n");
    if (!IOTC_RNWF11_Step("MQTTC7-tls", "AT+MQTTC=7,1\r\n")) return false;
    if (!IOTC_RNWF11_Step("MQTTC6-keepalive", "AT+MQTTC=6,60\r\n")) return false;
    if (!IOTC_RNWF11_Step("MQTTCONN", "AT+MQTTCONN=1\r\n"))
    {
        return false;
    }

    /* The broker result arrives asynchronously; watch for it rather than
     * guess. The TLS handshake to AWS was seen taking well over 5 seconds
     * (the +MQTTCONN:1 landed just after the old 5s window closed, the
     * attempt was declared failed, and the retry then tore that good session
     * down - repeatedly). ~20s here; Task() also adopts a session that comes
     * up even later than that. */
    for (uint16_t slice = 0; (slice < 40000U) && !mqttLinkUp; slice++)
    {
        __delay_us(500);
        IOTC_RNWF11_PollEvents();
    }
    if (!mqttLinkUp)
    {
        DEBUG_Printf("IOTC: step MQTT-connect-wait timed out (no +MQTTCONN:1)\r\n");
        return false;
    }

    IOTC_RNWF11_Subscribe();
    return mqttLinkUp;
}

#define IOTC_RNWF11_ESCAPED_JSON_MAX 256U

/* AT+MQTTPUB wraps the message in "..." with no escaping of its own - any
 * '"' inside the JSON payload prematurely closes that quoted argument. */
static bool IOTC_RNWF11_EscapeJsonForAtCommand(const char *json, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; json[i] != '\0'; i++)
    {
        char c = json[i];
        bool needs_escape = (c == '"') || (c == '\\');
        size_t needed = needs_escape ? 2U : 1U;
        if (o + needed >= out_size)
        {
            return false;
        }
        if (needs_escape)
        {
            out[o++] = '\\';
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

/* Inverse of IOTC_RNWF11_EscapeJsonForAtCommand() - the module applies the
 * same backslash-quote escaping to inbound quoted string fields (seen in
 * "+MQTTSUBRX:" - see the c2dQueue comment above) as we apply to
 * outbound ones. Copies src into dst, un-escaping as it goes, stopping at
 * the first unescaped '"'. Returns a pointer just past that closing quote,
 * or NULL if src ends before an unescaped '"' is found. *out_len is set to
 * the un-escaped length copied into dst (truncated to fit dst_size, always
 * null-terminated).*/
static const char *IOTC_RNWF11_UnescapeQuotedString(const char *src, char *dst, size_t dst_size, size_t *out_len)
{
    size_t o = 0;
    while (*src != '\0')
    {
        if (*src == '"')
        {
            if (out_len != NULL)
            {
                *out_len = o;
            }
            dst[(o < dst_size) ? o : (dst_size - 1U)] = '\0';
            return src + 1;
        }
        char c = *src;
        if ((c == '\\') && (*(src + 1) != '\0'))
        {
            src++;
            c = *src;
        }
        if (o < (dst_size - 1U))
        {
            dst[o] = c;
        }
        o++;
        src++;
    }
    return NULL;
}

#define IOTC_RNWF11_PUBACK_WAIT_MS 2000U

/* Every AT+MQTTPUB goes through here. Confirmed on real hardware: a publish
 * sent while the previous one's "+MQTTPUBACK" is still outstanding fails with
 * "8.0,MQTT Error", regardless of QoS or size (the ack arrives just after the
 * failure). So wait for the outstanding ack first, and remember when this
 * publish will produce one (QoS 1 - the second field of the command). Gives
 * up waiting after IOTC_RNWF11_PUBACK_WAIT_MS rather than wedging if an ack is
 * lost. Only call from the top level of Task()/OnCommand(), like the Step()
 * calls it wraps. */
static bool IOTC_RNWF11_PublishStep(const char *label, const char *command, bool expectAck)
{
    if (mqttPubAckPending)
    {
        while (mqttPubAckPending && ((uint32_t)(IOTC_RNWF11_Now() - pubAckSentMs) < IOTC_RNWF11_PUBACK_WAIT_MS))
        {
            IOTC_RNWF11_PollEvents();
            __delay_ms(1);
        }
        DEBUG_Printf("IOTC: %s: waited %lu ms for PUBACK%s\r\n", label,
                     (unsigned long)(IOTC_RNWF11_Now() - pubAckSentMs),
                     mqttPubAckPending ? " (never came, sending anyway)" : "");
        mqttPubAckPending = false;
    }

    /* Set before sending, not after: the ack can be picked up while the send
     * itself is still being processed (see IOTC_RNWF11_RecoverSwallowedEvents()),
     * and setting the flag afterwards would then wait for an ack already seen. */
    if (expectAck)
    {
        pubAckSentMs = IOTC_RNWF11_Now();
        mqttPubAckPending = true;
    }
    bool sent = IOTC_RNWF11_Step(label, command);
    if (!sent)
    {
        mqttPubAckPending = false;
    }
    return sent;
}

static bool IOTC_RNWF11_PublishTelemetry(void)
{
    IotclMessageHandle msg = iotcl_telemetry_create();
    if (msg == NULL)
    {
        DEBUG_Printf("IOTC: iotcl_telemetry_create failed\r\n");
        return false;
    }

    iotcl_telemetry_set_number(msg, "run", telemetry.motorRunning);
    iotcl_telemetry_set_number(msg, "st", telemetry.state);
    iotcl_telemetry_set_number(msg, "sec", telemetry.sector);
    iotcl_telemetry_set_number(msg, "rpm", telemetry.requestedSpeedRpm);
    iotcl_telemetry_set_number(msg, "spd", telemetry.measuredSpeedRpm);
    iotcl_telemetry_set_number(msg, "ic", telemetry.requestedCurrent);
    iotcl_telemetry_set_number(msg, "im", telemetry.measuredCurrent);
    iotcl_telemetry_set_number(msg, "duty", telemetry.dutyCycle);
    iotcl_telemetry_set_number(msg, "vdc", telemetry.dcBusAdc);

    char *json = iotcl_telemetry_create_serialized_string(msg, false);
    iotcl_telemetry_destroy(msg);
    if (json == NULL)
    {
        DEBUG_Printf("IOTC: iotcl_telemetry_create_serialized_string failed\r\n");
        return false;
    }

    char escaped[IOTC_RNWF11_ESCAPED_JSON_MAX];
    bool escapedOk = IOTC_RNWF11_EscapeJsonForAtCommand(json, escaped, sizeof(escaped));

    bool sent = false;
    if (!escapedOk)
    {
        DEBUG_Printf("IOTC: telemetry JSON too long to escape (%s)\r\n", json);
    }
    else
    {
        char command[384];
        int written = snprintf(command, sizeof(command), "AT+MQTTPUB=1,1,0,\"%s\",\"%s\"\r\n",
                                s_cfg.mqtt_pub_topic, escaped);
        if ((written < 0) || ((size_t)written >= sizeof(command)))
        {
            DEBUG_Printf("IOTC: telemetry command too long to send\r\n");
        }
        else
        {
            sent = IOTC_RNWF11_PublishStep("MQTTPUB", command, true);
        }
    }

    iotcl_telemetry_destroy_serialized_string(json);
    return sent;
}

/* Kept as its own small, separate publish rather than folded into
 * IOTC_RNWF11_PublishTelemetry() above: the RNWF11 rejects any AT+MQTTPUB
 * command line over 195 bytes ("0.4,Invalid Parameter"), and appending these
 * 3 fields to the main message pushed it from 158 to 197. */
static bool IOTC_RNWF11_PublishScopeSettings(void)
{
    IotclMessageHandle msg = iotcl_telemetry_create();
    if (msg == NULL)
    {
        DEBUG_Printf("IOTC: iotcl_telemetry_create (scope settings) failed\r\n");
        return false;
    }

    iotcl_telemetry_set_number(msg, "osc_ch", SCOPE_GetChannel());
    iotcl_telemetry_set_number(msg, "osc_rate", SCOPE_GetRateMicroseconds());
    iotcl_telemetry_set_number(msg, "osc_len", SCOPE_GetLength());

    char *json = iotcl_telemetry_create_serialized_string(msg, false);
    iotcl_telemetry_destroy(msg);
    if (json == NULL)
    {
        DEBUG_Printf("IOTC: iotcl_telemetry_create_serialized_string (scope settings) failed\r\n");
        return false;
    }

    char escaped[IOTC_RNWF11_ESCAPED_JSON_MAX];
    bool escapedOk = IOTC_RNWF11_EscapeJsonForAtCommand(json, escaped, sizeof(escaped));

    bool sent = false;
    if (!escapedOk)
    {
        DEBUG_Printf("IOTC: scope settings JSON too long to escape (%s)\r\n", json);
    }
    else
    {
        /* Same command form as the main telemetry publish (the one form
         * known to work on this topic); PublishStep() waits out that
         * publish's ack before this one is sent. */
        char command[192];
        int written = snprintf(command, sizeof(command), "AT+MQTTPUB=1,1,0,\"%s\",\"%s\"\r\n",
                                s_cfg.mqtt_pub_topic, escaped);
        if ((written < 0) || ((size_t)written >= sizeof(command)))
        {
            DEBUG_Printf("IOTC: scope settings command too long to send\r\n");
        }
        else
        {
            sent = IOTC_RNWF11_PublishStep("MQTTPUB-osc-settings", command, true);
        }
    }

    iotcl_telemetry_destroy_serialized_string(json);
    return sent;
}

/* Hard limit found by bisecting on real hardware (see the README's
 * oscilloscope section): a complete AT+MQTTPUB command line - including the
 * trailing "\r\n" - of 195 bytes or fewer is accepted, 196 or more fails with
 * "0.4,Invalid Parameter". A whole scope capture is far bigger than that, so
 * it is published as a sequence of chunks, each holding as many samples as
 * still fit. */
#define IOTC_RNWF11_MQTTPUB_LINE_MAX 195U
/* Bytes of the command line that aren't the topic or the escaped payload:
 * AT+MQTTPUB=1,1,0," (18) + "," (3) + "\r\n (3). */
#define IOTC_RNWF11_MQTTPUB_FIXED 24U
#define IOTC_RNWF11_SCOPE_CHUNK_JSON_MAX 160U
#define IOTC_RNWF11_SCOPE_ARRAY_MAX 96U

/* Length the escaper would produce for json, without building it. */
static size_t IOTC_RNWF11_EscapedLength(const char *json)
{
    size_t n = 0;
    for (size_t i = 0; json[i] != '\0'; i++)
    {
        n += ((json[i] == '"') || (json[i] == '\\')) ? 2U : 1U;
    }
    return n;
}

/* Builds one chunk's JSON into json (the same "{"d":[{"d":{...}}]}" envelope
 * iotcl_telemetry_create() builds internally), greedily adding samples from
 * start while the finished AT command line still fits IOTC_RNWF11_MQTTPUB_LINE_MAX,
 * up to maxCount samples. Returns how many samples it included (0 if not even
 * one fits). osc_t/osc_v are STRING-typed template attributes holding
 * bracketed array text - the dashboard parses that back into numbers. */
static uint16_t IOTC_RNWF11_BuildScopeChunk(uint16_t start, uint16_t seq, uint16_t total,
                                            uint16_t maxCount, char *json, size_t jsonSize)
{
    uint16_t length = SCOPE_GetLength();
    uint32_t periodUs = SCOPE_GetRateMicroseconds();
    size_t budget = IOTC_RNWF11_MQTTPUB_LINE_MAX - IOTC_RNWF11_MQTTPUB_FIXED - strlen(s_cfg.mqtt_pub_topic);

    static char tArr[IOTC_RNWF11_SCOPE_ARRAY_MAX];
    static char vArr[IOTC_RNWF11_SCOPE_ARRAY_MAX];
    static char cand[IOTC_RNWF11_SCOPE_CHUNK_JSON_MAX];
    size_t tLen = 0;
    size_t vLen = 0;
    uint16_t count = 0;

    while (((uint16_t)(start + count) < length) && (count < maxCount))
    {
        uint16_t idx = (uint16_t)(start + count);
        int tw = snprintf(&tArr[tLen], sizeof(tArr) - tLen, "%s%lu",
                          (count == 0U) ? "" : ",", (unsigned long)((uint32_t)idx * periodUs));
        int vw = snprintf(&vArr[vLen], sizeof(vArr) - vLen, "%s%d",
                          (count == 0U) ? "" : ",", (int)SCOPE_GetSample(idx));
        if ((tw < 0) || ((size_t)tw >= (sizeof(tArr) - tLen)) ||
            (vw < 0) || ((size_t)vw >= (sizeof(vArr) - vLen)))
        {
            break;
        }
        int cw = snprintf(cand, sizeof(cand),
                          "{\"d\":[{\"d\":{\"osc_seq\":%u,\"osc_n\":%u,\"osc_t\":\"[%s]\",\"osc_v\":\"[%s]\"}}]}",
                          (unsigned)seq, (unsigned)total, tArr, vArr);
        if ((cw < 0) || ((size_t)cw >= sizeof(cand)) || (IOTC_RNWF11_EscapedLength(cand) > budget))
        {
            break;
        }
        memcpy(json, cand, (size_t)cw + 1U);
        (void)jsonSize;
        tLen += (size_t)tw;
        vLen += (size_t)vw;
        count++;
    }
    return count;
}

/* A finished capture is published by a small state machine that Task() advances
 * once per pass instead of one blocking call, because a long capture is a lot
 * of chunks (roughly 250 for 1000 samples, at ~80 ms each once the module's ack
 * round trip is counted): blocking for that long froze telemetry, incoming
 * commands and everything else in the main loop. Hand-formatted into small
 * static buffers (no cJSON tree - see the 4KB heap note in bldc.X's linker
 * settings). Only called from the top level of Task(), never from inside
 * PollEvents() - same reentrancy reasoning as IOTC_RNWF11_PublishTelemetry().
 *
 *  PLANNING - works out how many samples go in each chunk, a few chunks per
 *             pass, so every chunk can carry the final chunk count ("osc_n").
 *             seq/total are unknown at this point, so the sizing uses
 *             placeholders at least as wide as the real numbers (a chunk holds
 *             at least one sample, so the count never exceeds the capture
 *             length): the real, narrower values then always fit the plan.
 *  SENDING  - builds and publishes one chunk per pass ("osc_seq" 1..osc_n),
 *             waiting for the previous chunk's ack (without blocking) first.
 *
 * While a capture is being published Task() holds queued C2D commands back
 * (the capture buffer and its rate/length must not change underneath it), and
 * abandons the capture if the broker connection drops. */
typedef enum
{
    SCOPE_TX_IDLE = 0,
    SCOPE_TX_PLANNING,
    SCOPE_TX_SENDING
} scope_tx_state_t;

/* A chunk always holds at least two samples with the default topic, so this
 * covers SCOPE_BUFFER_MAX; a pathologically long topic that allowed only one
 * sample per chunk would overflow it and abort the publish cleanly. */
#define IOTC_RNWF11_SCOPE_MAX_CHUNKS 512U
#define IOTC_RNWF11_SCOPE_PLAN_PER_PASS 8U

static scope_tx_state_t scopeTxState;
static char scopeJson[IOTC_RNWF11_SCOPE_CHUNK_JSON_MAX];
static uint8_t scopeChunkSamples[IOTC_RNWF11_SCOPE_MAX_CHUNKS];
static uint16_t scopeChunkCount;
static uint16_t scopeTxLength;   /* samples in the capture being published */
static uint16_t scopePlanned;    /* samples assigned to a chunk so far (planning) */
static uint16_t scopeTxNext;     /* next chunk index to send */
static uint16_t scopeTxStart;    /* first sample of that chunk */

static void IOTC_RNWF11_ScopeTxAbort(const char *why)
{
    DEBUG_Printf("IOTC: scope publish aborted: %s\r\n", why);
    scopeTxState = SCOPE_TX_IDLE;
    SCOPE_ClearReady();
}

static void IOTC_RNWF11_ScopeTxBegin(void)
{
    scopeTxLength = SCOPE_GetLength();
    scopeChunkCount = 0;
    scopePlanned = 0;
    scopeTxState = SCOPE_TX_PLANNING;
}

static void IOTC_RNWF11_ScopeTxPlanStep(void)
{
    uint16_t placeholder = (scopeTxLength <= 99U) ? 99U : 999U;

    for (uint8_t i = 0; (i < IOTC_RNWF11_SCOPE_PLAN_PER_PASS) && (scopePlanned < scopeTxLength); i++)
    {
        uint16_t n = IOTC_RNWF11_BuildScopeChunk(scopePlanned, placeholder, placeholder, scopeTxLength,
                                                 scopeJson, sizeof(scopeJson));
        if ((n == 0U) || (scopeChunkCount >= IOTC_RNWF11_SCOPE_MAX_CHUNKS))
        {
            DEBUG_Printf("IOTC: scope chunk planning failed at sample %u\r\n", (unsigned)scopePlanned);
            IOTC_RNWF11_ScopeTxAbort("cannot fit samples in a message");
            return;
        }
        scopeChunkSamples[scopeChunkCount++] = (uint8_t)n;
        scopePlanned += n;
    }

    if (scopePlanned >= scopeTxLength)
    {
        scopeTxNext = 0;
        scopeTxStart = 0;
        scopeTxState = SCOPE_TX_SENDING;
        DEBUG_Printf("IOTC: scope publish: %u samples in %u chunks\r\n",
                     (unsigned)scopeTxLength, (unsigned)scopeChunkCount);
    }
}

static void IOTC_RNWF11_ScopeTxSendStep(void)
{
    if (!mqttLinkUp)
    {
        IOTC_RNWF11_ScopeTxAbort("broker disconnected");
        return;
    }
    if (mqttPubAckPending && ((uint32_t)(IOTC_RNWF11_Now() - pubAckSentMs) < IOTC_RNWF11_PUBACK_WAIT_MS))
    {
        return; /* previous chunk not acked yet - try again next pass, without blocking */
    }

    static char escaped[IOTC_RNWF11_SCOPE_CHUNK_JSON_MAX * 2U];
    static char command[IOTC_RNWF11_MQTTPUB_LINE_MAX + 8U];
    uint16_t c = scopeTxNext;
    uint16_t n = IOTC_RNWF11_BuildScopeChunk(scopeTxStart, (uint16_t)(c + 1U), scopeChunkCount,
                                             scopeChunkSamples[c], scopeJson, sizeof(scopeJson));
    int written = -1;
    if ((n == scopeChunkSamples[c]) && IOTC_RNWF11_EscapeJsonForAtCommand(scopeJson, escaped, sizeof(escaped)))
    {
        written = snprintf(command, sizeof(command), "AT+MQTTPUB=1,1,0,\"%s\",\"%s\"\r\n",
                           s_cfg.mqtt_pub_topic, escaped);
    }
    if ((written < 0) || ((size_t)written > IOTC_RNWF11_MQTTPUB_LINE_MAX))
    {
        DEBUG_Printf("IOTC: scope chunk %u/%u unsendable (cmd=%d)\r\n", (unsigned)(c + 1U), (unsigned)scopeChunkCount, written);
        IOTC_RNWF11_ScopeTxAbort("chunk did not fit");
        return;
    }
    if (!IOTC_RNWF11_PublishStep("MQTTPUB-scope", command, true))
    {
        IOTC_RNWF11_ScopeTxAbort("chunk publish failed");
        return;
    }

    scopeTxStart += n;
    scopeTxNext++;
    if (scopeTxNext >= scopeChunkCount)
    {
        DEBUG_Printf("IOTC: scope publish ok (%u samples in %u chunks, %lu us/sample)\r\n",
                     (unsigned)scopeTxLength, (unsigned)scopeChunkCount,
                     (unsigned long)SCOPE_GetRateMicroseconds());
        scopeTxState = SCOPE_TX_IDLE;
        SCOPE_ClearReady();
    }
}

/* Only ever called from the top level of Task()/OnCommand(), never from
 * inside PollEvents() - see the "+MQTTSUBRX:" handling note above. */
static void IOTC_RNWF11_SendCmdAck(const char *ack_id, int status, const char *message)
{
    if (s_cfg.mqtt_ack_topic[0] == '\0')
    {
        return;
    }

    char *json = iotcl_c2d_create_cmd_ack_json(ack_id, status, message);
    if (json == NULL)
    {
        DEBUG_Printf("IOTC: iotcl_c2d_create_cmd_ack_json failed\r\n");
        return;
    }

    char escaped[IOTC_RNWF11_ESCAPED_JSON_MAX];
    if (IOTC_RNWF11_EscapeJsonForAtCommand(json, escaped, sizeof(escaped)))
    {
        char command[384];
        int written = snprintf(command, sizeof(command), "AT+MQTTPUB=0,1,0,\"%s\",\"%s\"\r\n",
                                s_cfg.mqtt_ack_topic, escaped);
        if ((written >= 0) && ((size_t)written < sizeof(command)))
        {
            (void)IOTC_RNWF11_PublishStep("MQTTPUB-ack", command, true);
        }
    }
    else
    {
        DEBUG_Printf("IOTC: ack JSON too long to escape (%s)\r\n", json);
    }

    iotcl_c2d_destroy_ack_json(json);
}

/* Registered as events.cmd_cb in IOTC_RNWF11_Initialize(). Invoked
 * synchronously from within iotcl_mqtt_receive_c2d_with_length(), called only
 * from the top level of Task() (see its c2dQueue handling), so it's
 * safe to issue AT commands (for the ack) from here. */
static void IOTC_RNWF11_OnCommand(IotclC2dEventData data)
{
    const char *ack_id = iotcl_c2d_get_ack_id(data);
    const char *raw_command = iotcl_c2d_get_command(data);
    if (raw_command == NULL)
    {
        return;
    }

    /* Despite iotcl_c2d_get_command()'s header comment ("returns a malloc-ed
     * copy... must be freed"), the pinned iotc-c-lib version actually returns
     * cJSON_GetStringValue()'s raw pointer straight into the parsed tree, not
     * a copy - freeing it here double-frees it when iotcl_c2d_destroy_event()
     * (called automatically right after this callback returns) deletes the
     * whole tree, corrupting the heap. Copy it out; do not free raw_command. */
    char command[64];
    snprintf(command, sizeof(command), "%s", raw_command);

    char *arg = strchr(command, ' ');
    if (arg != NULL)
    {
        *arg = '\0';
        arg++;
    }

    int status = IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK;
    const char *message = NULL;

    if (strcmp(command, "motor-start") == 0)
    {
        MCAPP_MotorStart();
    }
    else if (strcmp(command, "motor-stop") == 0)
    {
        MCAPP_MotorStop();
    }
    else if (strcmp(command, "motor-reverse") == 0)
    {
        MCAPP_MotorReverse();
    }
    else if (strcmp(command, "motor-speed") == 0)
    {
        char *endptr = NULL;
        long percent = (arg != NULL) ? strtol(arg, &endptr, 10) : 0;
        if ((arg == NULL) || (endptr == arg) || (*endptr != '\0'))
        {
            status = IOTCL_C2D_EVT_CMD_FAILED;
            message = "motor-speed requires an integer 0-100 percent argument";
        }
        else
        {
            /* Clamp before the uint8_t cast below - clamping after would let
             * an out-of-range value (e.g. 300) wrap modulo 256 instead. */
            if (percent < 0)
            {
                percent = 0;
            }
            else if (percent > 100)
            {
                percent = 100;
            }
            MCAPP_MotorSetSpeedPercent((uint8_t)percent);
        }
    }
    else if (strcmp(command, "scope-channel") == 0)
    {
        char *endptr = NULL;
        long channel = (arg != NULL) ? strtol(arg, &endptr, 10) : -1;
        if ((arg == NULL) || (endptr == arg) || (*endptr != '\0') || (channel < 0) || (channel > SCOPE_CHANNEL_MAX))
        {
            status = IOTCL_C2D_EVT_CMD_FAILED;
            message = "scope-channel requires an integer 0-4 (0=vdc,1=duty,2=ibus,3=ia,4=ib)";
        }
        else
        {
            SCOPE_SetChannel((uint8_t)channel);
        }
    }
    else if (strcmp(command, "scope-rate") == 0)
    {
        char *endptr = NULL;
        long microseconds = (arg != NULL) ? strtol(arg, &endptr, 10) : 0;
        if ((arg == NULL) || (endptr == arg) || (*endptr != '\0') || (microseconds < 1))
        {
            status = IOTCL_C2D_EVT_CMD_FAILED;
            message = "scope-rate requires a positive integer (microseconds)";
        }
        else
        {
            SCOPE_SetRateMicroseconds((uint32_t)microseconds);
        }
    }
    else if (strcmp(command, "scope-length") == 0)
    {
        char *endptr = NULL;
        long samples = (arg != NULL) ? strtol(arg, &endptr, 10) : 0;
        if ((arg == NULL) || (endptr == arg) || (*endptr != '\0') || (samples < 1))
        {
            status = IOTCL_C2D_EVT_CMD_FAILED;
            message = "scope-length requires a positive integer (samples, clamped to 10-1000)";
        }
        else
        {
            /* Clamp before the uint16_t cast below, same reasoning as
             * motor-speed's clamp-before-cast above - an out-of-range value
             * (e.g. 70000) would otherwise wrap modulo 65536. */
            if (samples > SCOPE_BUFFER_MAX)
            {
                samples = SCOPE_BUFFER_MAX;
            }
            SCOPE_SetLength((uint16_t)samples);
        }
    }
    else if (strcmp(command, "scope-capture") == 0)
    {
        SCOPE_StartCapture();
    }
    else
    {
        status = IOTCL_C2D_EVT_CMD_FAILED;
        message = "unknown command";
    }

    DEBUG_Printf("IOTC: command \"%s%s%s\" -> %s\r\n", command, arg ? " " : "", arg ? arg : "",
                 (status == IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK) ? "ok" : "failed");

    if (ack_id != NULL)
    {
        IOTC_RNWF11_SendCmdAck(ack_id, status, message);
    }
}

void IOTC_RNWF11_Initialize(void)
{
#if IOTC_RNWF11_ENABLE
    IOTC_RNWF11_UART2_Initialize();
    iotcModulePresent = IOTC_RNWF11_CommandWithTimeout("AT\r\n", IOTC_RNWF11_PROBE_TIMEOUT);
    iotcConnected = false;

    if (DEVICE_CONFIG_Load(&s_cfg))
    {
        DEBUG_Printf("IOTC: loaded provisioned config for client %s\r\n", s_cfg.mqtt_client_id);
    }
    else
    {
        // Not yet provisioned (or flash was blank/corrupt) - fall back to
        // the compile-time defaults so the board still works exactly as
        // before without going through tools/provision_device_config.py/.ps1.
        // Also the one safe moment to self-test the flash driver: this
        // page holds no valid config yet, so there is nothing to lose.
        snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%s", IOTC_WIFI_SSID);
        snprintf(s_cfg.wifi_password, sizeof(s_cfg.wifi_password), "%s", IOTC_WIFI_PASSWORD);
        snprintf(s_cfg.mqtt_broker_host, sizeof(s_cfg.mqtt_broker_host), "%s", IOTC_MQTT_BROKER_HOST);
        s_cfg.mqtt_broker_port = IOTC_MQTT_BROKER_PORT;
        snprintf(s_cfg.mqtt_client_id, sizeof(s_cfg.mqtt_client_id), "%s", IOTC_MQTT_CLIENT_ID);
        snprintf(s_cfg.mqtt_username, sizeof(s_cfg.mqtt_username), "%s", IOTC_MQTT_USERNAME);
        snprintf(s_cfg.mqtt_pub_topic, sizeof(s_cfg.mqtt_pub_topic), "%s", IOTC_MQTT_TELEMETRY_TOPIC);
        snprintf(s_cfg.mqtt_c2d_topic, sizeof(s_cfg.mqtt_c2d_topic), "%s", IOTC_MQTT_C2D_TOPIC);
        snprintf(s_cfg.mqtt_ack_topic, sizeof(s_cfg.mqtt_ack_topic), "%s", IOTC_MQTT_ACK_TOPIC);
        snprintf(s_cfg.rnwf_ca_name, sizeof(s_cfg.rnwf_ca_name), "%s", IOTC_RNWF11_CA_NAME);
        snprintf(s_cfg.rnwf_cert_name, sizeof(s_cfg.rnwf_cert_name), "%s", IOTC_RNWF11_CERT_NAME);
        snprintf(s_cfg.rnwf_key_name, sizeof(s_cfg.rnwf_key_name), "%s", IOTC_RNWF11_KEY_NAME);

        DEBUG_Printf("IOTC: not provisioned, using compile-time config; flash self-test %s\r\n",
                     NVM_FLASH_SelfTest() ? "PASS" : "FAIL");
    }

    IotclClientConfig iotcl_cfg;
    iotcl_init_client_config(&iotcl_cfg);
    iotcl_cfg.device.instance_type = IOTCL_DCT_CUSTOM; // broker/topic are resolved at provisioning time - see iotconnect_rnwf11_config.h
    iotcl_cfg.events.cmd_cb = IOTC_RNWF11_OnCommand;
    iotcl_init(&iotcl_cfg);

#if !IOTC_RNWF11_RX_RPn
    DEBUG_Printf("IOTC: UART2 pins unset, set IOTC_RNWF11_RX_RPn/TX_RPnR\r\n");
#endif
    DEBUG_Printf("IOTC: module %s, config %s\r\n",
                 iotcModulePresent ? "detected" : "not responding",
                 IOTC_RNWF11_IsConfigured() ? "ok" : "incomplete");
#else
    iotcModulePresent = false;
    iotcConnected = false;
#endif
}

void IOTC_RNWF11_CheckProvisioning(void)
{
#if IOTC_RNWF11_ENABLE
    if (PROVISIONING_CheckForRequest())
    {
        device_config_t new_cfg;
        if (PROVISIONING_RunOnce(&new_cfg))
        {
            s_cfg = new_cfg;
            iotcConnected = false;              // force a fresh connect with the new config
            mqttLinkUp = false;
            retryMilliseconds = IOTC_RETRY_PERIOD_MS; // don't wait out the retry timer
            DEBUG_Printf("IOTC: reprovisioned, reconnecting\r\n");
        }
    }
#endif
}

void IOTC_RNWF11_Tick1ms(void)
{
#if IOTC_RNWF11_ENABLE
    telemetryMilliseconds++;
    retryMilliseconds++;
    msNow++;
#endif
}

void IOTC_RNWF11_SetTelemetry(const IOTC_RNWF11_Telemetry_t *sample)
{
    telemetry = *sample;
}

/* Queues a decoded C2D payload for Task() and, for the one command that must
 * not wait, acts on it immediately. Called from the event handler only. */
static void IOTC_RNWF11_QueueC2d(const char *payload, size_t payloadLen)
{
    /* Safety fast path: motor-stop takes effect the moment it is received, not
     * when Task() gets to it - Task() holds commands back while a scope capture
     * is being published, and a long one can take many seconds. Stopping is just
     * clearing a flag, so it is safe to do from here; the message is still queued
     * below so the normal handler runs it again (harmless) and sends its ack. */
    if (strstr(payload, "\"motor-stop\"") != NULL)
    {
        MCAPP_MotorStop();
        DEBUG_Printf("IOTC: motor-stop received - stopping immediately\r\n");
    }

    if (c2dQueueCount >= IOTC_RNWF11_C2D_QUEUE_LEN)
    {
        DEBUG_Printf("IOTC: C2D queue full, dropping message\r\n");
        return;
    }
    uint8_t slot = (uint8_t)((c2dQueueHead + c2dQueueCount) % IOTC_RNWF11_C2D_QUEUE_LEN);
    memcpy(c2dQueue[slot], payload, payloadLen + 1U);
    c2dQueueLen[slot] = payloadLen;
    c2dQueueCount++;
}

/*
 * Handles one complete line from the module: real connection state arrives
 * asynchronously (command replies only say whether the command itself was
 * accepted), as do C2D messages and publish acks.
 */
static void IOTC_RNWF11_HandleEventLine(const char *line)
{
    DEBUG_Printf("IOTC: event %s\r\n", line);

    /* Checked first: a C2D payload could contain any of the substrings the
     * other branches look for. */
    if (strncmp(line, "+MQTTSUBRX:", 11) == 0)
    {
        /* Observed format: +MQTTSUBRX:<DUP>,<QOS>,<RETAIN>,"<TOPIC_NAME>","<PAYLOAD>"
         * (confirmed against real hardware - the module inlines the payload
         * here rather than requiring a follow-up AT+MQTTSUBRD for messages
         * under its read threshold; see the c2dQueue comment above).
         * Deliberately does not act on the message from here (bar the
         * motor-stop fast path in IOTC_RNWF11_QueueC2d()) -
         * iotcl_mqtt_receive_c2d_with_length() may itself publish a command ack
         * (an AT command), and this function can run nested inside other
         * in-flight IOTC_RNWF11_Command() calls elsewhere in this file, so a
         * nested command here would stomp the shared lastResponse buffer out
         * from under the outer call. Task() picks the queued payload up instead,
         * from its own top-level call. */
        const char *p = line + 11;
        for (uint8_t skip = 0; (skip < 3U) && (p != NULL); skip++)
        {
            p = strchr(p, ',');
            if (p != NULL)
            {
                p++;
            }
        }
        if ((p != NULL) && (*p == '"'))
        {
            char discardTopic[IOTC_RNWF11_C2D_TOPIC_MAX];
            const char *afterTopic = IOTC_RNWF11_UnescapeQuotedString(p + 1, discardTopic, sizeof(discardTopic), NULL);
            if ((afterTopic != NULL) && (*afterTopic == ',') && (*(afterTopic + 1) == '"'))
            {
                static char scratch[IOTC_RNWF11_C2D_PAYLOAD_MAX];
                size_t payloadLen = 0;
                const char *afterPayload = IOTC_RNWF11_UnescapeQuotedString(afterTopic + 2, scratch,
                                                                             sizeof(scratch), &payloadLen);
                if (afterPayload != NULL)
                {
                    IOTC_RNWF11_QueueC2d(scratch, payloadLen);
                }
            }
        }
    }
    else if (strstr(line, "+MQTTCONN:1") != NULL)
    {
        mqttLinkUp = true;
    }
    else if (strstr(line, "+MQTTCONN:0") != NULL)
    {
        mqttLinkUp = false;
    }
    else if (strstr(line, "+MQTTPUBACK") != NULL)
    {
        mqttPubAckPending = false;
    }
    else if (strstr(line, "+WSTAAIP:") != NULL)
    {
        /* The leading number is a per-association counter, not
         * a fixed interface id - it increments every time the
         * module reconnects (seen going 1 -> 3 after adding an
         * explicit AT+WSTA=0/1 disconnect+reconnect cycle), so
         * matching only ":1" missed every later reconnect. */
        netUp = true;
    }
    else if (strstr(line, "+WSTALU:0") != NULL)
    {
        netUp = false;
        mqttLinkUp = false;
    }
    else if (strstr(line, "20.2") != NULL)
    {
        /* The module reports this when STA is already connected. */
        netUp = true;
    }
}

static void IOTC_RNWF11_PollEvents(void)
{
    static char line[128];
    static uint16_t len;

    while (UART2_IsReceiveDataAvailable())
    {
        char c = (char)UART2_Read();
        if ((c == '\r') || (c == '\n'))
        {
            if (len > 0U)
            {
                line[len] = '\0';
                IOTC_RNWF11_HandleEventLine(line);
                len = 0;
            }
        }
        else if (len < (sizeof(line) - 1U))
        {
            line[len++] = c;
        }
    }
}

void IOTC_RNWF11_Task(void)
{
#if IOTC_RNWF11_ENABLE
    if (!iotcModulePresent || !IOTC_RNWF11_IsConfigured())
    {
        return;
    }
    IOTC_RNWF11_PollEvents();
    if (!iotcConnected)
    {
        if (scopeTxState != SCOPE_TX_IDLE)
        {
            IOTC_RNWF11_ScopeTxAbort("connection lost");
        }
        if (netUp && mqttLinkUp)
        {
            /* The broker connected after Configure() had already given up
             * (see the wait loop there) - keep that session rather than
             * tearing it down with another WSTA/MQTTCONN cycle. */
            DEBUG_Printf("IOTC: broker connected late, adopting session\r\n");
            IOTC_RNWF11_Subscribe();
            iotcConnected = true;
            telemetryMilliseconds = 0;
            return;
        }
        if (!netUp)
        {
            return;
        }
        if (retryMilliseconds < IOTC_RETRY_PERIOD_MS)
        {
            return;
        }
        retryMilliseconds = 0;
        /* Clear any stale value from a prior session before this attempt -
         * a genuine "+MQTTCONN:1" during Configure() below (processed via
         * its own PollEvents() calls) sets this back to true; unconditionally
         * clearing it AFTER Configure() returns (as this used to do) stomped
         * on that real success every single time, making a connection that
         * actually worked look immediately "not connected" on the very next
         * check and forcing a pointless reconnect before telemetry ever had
         * a chance to publish. */
        mqttLinkUp = false;
        iotcConnected = IOTC_RNWF11_Configure();
        DEBUG_Printf("IOTC: provisioning %s\r\n", iotcConnected ? "complete" : "failed");
#if IOTC_DIAG_QUERIES
        IOTC_RNWF11_Diagnose();
#endif
        telemetryMilliseconds = 0;
        return;
    }
    /* Scope capture publishing, one small step per pass (see the state machine
     * comment above). Ahead of the command handling below so that a capture that
     * has just finished filling is picked up - and starts holding commands back -
     * before a queued scope-capture/rate/length command can disturb it. */
    if (scopeTxState == SCOPE_TX_PLANNING)
    {
        IOTC_RNWF11_ScopeTxPlanStep();
    }
    else if (scopeTxState == SCOPE_TX_SENDING)
    {
        IOTC_RNWF11_ScopeTxSendStep();
    }
    else if (SCOPE_IsReady())
    {
        IOTC_RNWF11_ScopeTxBegin();
    }

    /* Queued C2D commands run one per pass, and only once no capture is being
     * published: the capture buffer and its rate/length must not change under
     * the publisher. (motor-stop is the exception - it is acted on the instant it
     * arrives, see IOTC_RNWF11_QueueC2d().) */
    if ((c2dQueueCount > 0U) && (scopeTxState == SCOPE_TX_IDLE))
    {
        uint8_t head = c2dQueueHead;
        DEBUG_Printf("IOTC: C2D message (%u bytes): %s\r\n", (unsigned)c2dQueueLen[head], c2dQueue[head]);
        (void)iotcl_mqtt_receive_c2d_with_length((const uint8_t *)c2dQueue[head], c2dQueueLen[head]);
        /* Released only now: the handler above can publish an ack, which polls
         * events and may queue further messages - they must not land in this slot. */
        c2dQueueHead = (uint8_t)((head + 1U) % IOTC_RNWF11_C2D_QUEUE_LEN);
        c2dQueueCount--;
    }
    if (telemetryMilliseconds >= IOTC_TELEMETRY_PERIOD_MS)
    {
        telemetryMilliseconds = 0;
        if (!mqttLinkUp)
        {
            DEBUG_Printf("IOTC: broker not connected, retrying\r\n");
            iotcConnected = false;
            retryMilliseconds = 0;
            return;
        }
        bool sent = IOTC_RNWF11_PublishTelemetry();
        DEBUG_Printf("IOTC: publish %s state=%u rpm=%u duty=%d\r\n",
                 sent ? "ok" : "FAILED",
                 telemetry.state, telemetry.measuredSpeedRpm, telemetry.dutyCycle);

        bool oscSent = IOTC_RNWF11_PublishScopeSettings();
        DEBUG_Printf("IOTC: publish scope settings %s\r\n", oscSent ? "ok" : "FAILED");
    }
#endif
}

bool IOTC_RNWF11_IsConnected(void)
{
    return iotcConnected;
}
