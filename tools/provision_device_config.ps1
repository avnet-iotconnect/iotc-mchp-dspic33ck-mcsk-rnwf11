# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Avnet
#
# Windows/PowerShell equivalent of provision_device_config.py - see that file's
# header comment for what this does and when to run it. This script has no
# external module dependencies (System.IO.Ports and Invoke-RestMethod are
# both built into PowerShell 5.1+), so there's no install step before running it.

[CmdletBinding()]
param(
    # Resolved relative to this script's own location ($PSScriptRoot) by
    # default, so it works no matter what directory the script is invoked from.
    # Forward slashes here are deliberate, not a typo - .NET's Path APIs treat
    # them as separators on Windows too, and this needs to resolve correctly
    # on Linux PowerShell 7+ as well (the README supports both).
    [string]$ConfigHeader = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../firmware/dspic33ck256mp508_rnwf11_iotconnect.X/iotconnect/iotconnect_rnwf11_config.h")),
    # Optional - only needed to ALSO live-provision an already-flashed,
    # already-running board over serial without rebuilding; omit it to just
    # update -ConfigHeader.
    [string]$Port = "",
    [int]$Baud = 115200,
    [Parameter(Mandatory)] [string]$WifiSsid,
    [Parameter(Mandatory)] [string]$WifiPassword,
    [Parameter(Mandatory)] [string]$Cpid,
    [Parameter(Mandatory)] [string]$Env,
    [Parameter(Mandatory)] [string]$Duid,
    [ValidateSet("aws", "az")] [string]$Platform = "aws",
    [string]$CaName = "root-ca",
    [string]$CertName = "device-cert",
    [string]$KeyName = "device-key"
)

$ErrorActionPreference = "Stop"
$MqttPort = 8883

# Windows PowerShell 5.1 runs on .NET Framework, whose default
# ServicePointManager.SecurityProtocol on plenty of real-world Windows 10/11
# installs does NOT include TLS 1.2 - Invoke-RestMethod against
# discovery.iotconnect.io then fails outright with a generic "Could not
# create SSL/TLS secure channel" error, before this script even gets to send
# a real request. PowerShell 7+ (.NET 5+) already defaults to modern TLS and
# ignores this setting, so forcing it here is harmless there too.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# WifiSsid/WifiPassword/Cpid/Env travel over the firmware's line-based
# provisioning protocol (PROVISION/KEY=VALUE/END, see provisioning.h), which
# has no per-field escaping - an embedded CR/LF would silently corrupt the
# stream with an extra phantom line. A literal '"' is otherwise harmless
# here, unlike the name fields below.
function Test-NoLineBreak {
    param([string]$Value, [string]$ParamName)
    if ($Value -match '[\r\n]') {
        throw "-$ParamName must not contain a line break"
    }
}
foreach ($pair in @(
    @{ Name = "WifiSsid"; Value = $WifiSsid }
    @{ Name = "WifiPassword"; Value = $WifiPassword }
    @{ Name = "Cpid"; Value = $Cpid }
    @{ Name = "Env"; Value = $Env }
)) {
    Test-NoLineBreak -Value $pair.Value -ParamName $pair.Name
}

# CaName/CertName/KeyName get stored on the device and later embedded by the
# firmware itself in a quoted AT+TLSC filename argument sent to the RNWF11
# (iotconnect_rnwf11.c's Configure()) - same safe, boring character set
# provision_rnwf11_cert.ps1 already requires for these same names, so an
# embedded '"' can never reach that AT command in the first place.
function Test-SafeIdentifier {
    param([string]$Value, [string]$ParamName)
    if ($Value -notmatch '^[A-Za-z0-9_.-]+$') {
        throw "-$ParamName may only contain letters, digits, '.', '_', and '-' (got: '$Value')"
    }
}
foreach ($pair in @(
    @{ Name = "CaName"; Value = $CaName }
    @{ Name = "CertName"; Value = $CertName }
    @{ Name = "KeyName"; Value = $KeyName }
)) {
    Test-SafeIdentifier -Value $pair.Value -ParamName $pair.Name
}

function ConvertTo-CString {
    param([string]$Value)
    return $Value.Replace('\', '\\').Replace('"', '\"')
}

# Replaces the quoted value on a '#define NAME "..."' line, preserving
# everything else on that line untouched.
function Set-Define {
    param([string[]]$Lines, [string]$Name, [string]$Value)
    $escaped = ConvertTo-CString -Value $Value
    # \s+ (any run of spaces/tabs), not a single hardcoded literal space - a
    # single space matches this project's own header as written today, but
    # is needlessly brittle: it would silently find zero matches (and throw)
    # against a header reformatted with tab alignment, whereas
    # provision_device_config.py's equivalent regex already tolerates it.
    # Leading indent and anything after the closing quote (e.g. a trailing
    # comment) are captured and preserved rather than anchored away, same
    # reasoning.
    $linePattern = "^(\s*)#define\s+$([regex]::Escape($Name))\s+`"[^`"]*`"(.*)$"
    $matchCount = 0
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match $linePattern) {
            $matchCount++
            # Built via plain string interpolation, not .NET's -replace
            # replacement-pattern syntax: $escaped can itself start with a
            # literal '$' (an AWS IoT Rule topic looks like
            # "$aws/rules/..."), which that syntax would otherwise try to
            # interpret as a group-reference token - interpolation has no
            # such special-casing, it just inserts the string value as-is.
            $Lines[$i] = "$($Matches[1])#define $Name `"$escaped`"$($Matches[2])"
        }
    }
    if ($matchCount -ne 1) {
        throw "Expected exactly one '#define $Name ""..."" ' line in the config header, found $matchCount"
    }
    return ,$Lines
}

function Invoke-DraRequest {
    # Invoke-RestMethod throws on non-2xx HTTP responses (same as Python's
    # urllib.request.urlopen), which the DRA API uses for some error cases
    # (e.g. an unrecognized CPID/Environment) - without this, that would
    # surface as a raw HttpResponseException stack trace instead of the
    # API's actual JSON error message.
    param([string]$Uri, [string]$What)
    try {
        return Invoke-RestMethod -Uri $Uri -Method Get
    } catch {
        $body = $_.ErrorDetails.Message
        $parsed = $null
        if ($body) {
            try { $parsed = $body | ConvertFrom-Json } catch { }
        }
        if ($parsed) {
            throw "$What failed. status=$($parsed.status) message=$($parsed.message)"
        }
        if ($body) {
            throw "$What failed: $body"
        }
        throw "$What failed: $_"
    }
}

function Resolve-ConnectionInfo {
    param([string]$Cpid, [string]$Env, [string]$Duid, [string]$Platform)

    $discoveryUrl = "https://discovery.iotconnect.io/api/v2.1/dsdk/cpId/$([uri]::EscapeDataString($Cpid))/env/$([uri]::EscapeDataString($Env))?pf=$Platform"
    Write-Host "Requesting Discovery Data $discoveryUrl..."
    $disc = Invoke-DraRequest -Uri $discoveryUrl -What "Discovery"
    if ($disc.status -ne 200 -or $disc.d.ec -ne 0) {
        throw "Discovery failed. status=$($disc.status) ec=$($disc.d.ec) message=$($disc.message)"
    }

    $identityUrl = "$($disc.d.bu)/uid/$([uri]::EscapeDataString($Duid))"
    Write-Host "Requesting Identity Data $identityUrl..."
    $ident = Invoke-DraRequest -Uri $identityUrl -What "Identity"
    if ($ident.status -ne 200 -or $ident.d.ec -ne 0) {
        throw "Identity failed. status=$($ident.status) ec=$($ident.d.ec) message=$($ident.message)"
    }
    if ([string]::IsNullOrEmpty($ident.d.p.topics.rpt)) {
        throw "Identity response did not include a telemetry (rpt) topic"
    }
    if ([string]::IsNullOrEmpty($ident.d.p.topics.c2d)) {
        throw "Identity response did not include a C2D (c2d) topic"
    }
    if ([string]::IsNullOrEmpty($ident.d.p.topics.ack)) {
        throw "Identity response did not include an acknowledgement (ack) topic"
    }

    return $ident.d.p
}

try {
    $identity = Resolve-ConnectionInfo -Cpid $Cpid -Env $Env -Duid $Duid -Platform $Platform
} catch {
    Write-Host "FAILED: could not resolve device connection info: $_"
    exit 1
}

Write-Host "Resolved broker host: $($identity.h)"
Write-Host "Resolved MQTT client ID: $($identity.id)"
Write-Host "Resolved MQTT username: $(if ($identity.un) { $identity.un } else { '(none)' })"
Write-Host "Resolved telemetry topic: $($identity.topics.rpt)"
Write-Host "Resolved C2D topic: $($identity.topics.c2d)"
Write-Host "Resolved ack topic: $($identity.topics.ack)"

# Only the default path is optional: someone who downloaded just the
# quickstart files (no firmware/ source tree - see the README) has nothing
# at this path, and that's fine as long as -Port is going to configure the
# board directly instead. An EXPLICITLY-passed -ConfigHeader is a real
# request to update that file, so a missing path there still fails loudly
# rather than being silently skipped.
$usingDefaultHeaderPath = -not $PSBoundParameters.ContainsKey('ConfigHeader')
$headerUpdated = $false
if ($usingDefaultHeaderPath -and -not (Test-Path -Path $ConfigHeader)) {
    Write-Host ("Note: $ConfigHeader not found - skipping the compiled-in config header " +
        "(expected if you downloaded just the quickstart files rather than the full firmware source).")
} else {
    try {
        # -Encoding ASCII explicitly on both read and write: this header is
        # plain 7-bit ASCII (matching the AT-command transport it's compiled
        # into), and Windows PowerShell 5.1 vs PowerShell 7+ disagree on the
        # DEFAULT text encoding (5.1's Set-Content -Encoding UTF8 always
        # writes a BOM; 7+'s plain default does not) - a stray BOM at the top
        # of a C header is usually tolerated but not guaranteed to be, and
        # pinning ASCII sidesteps the question on both PowerShell versions.
        $lines = Get-Content -Path $ConfigHeader -Encoding ASCII
        $defines = [ordered]@{
            IOTC_WIFI_SSID            = $WifiSsid
            IOTC_WIFI_PASSWORD        = $WifiPassword
            IOTC_MQTT_BROKER_HOST     = $identity.h
            IOTC_MQTT_CLIENT_ID       = $identity.id
            IOTC_MQTT_USERNAME        = $(if ($identity.un) { $identity.un } else { "" })
            IOTC_MQTT_TELEMETRY_TOPIC = $identity.topics.rpt
            IOTC_MQTT_C2D_TOPIC       = $identity.topics.c2d
            IOTC_MQTT_ACK_TOPIC       = $identity.topics.ack
        }
        foreach ($name in $defines.Keys) {
            $lines = Set-Define -Lines $lines -Name $name -Value $defines[$name]
        }
        Set-Content -Path $ConfigHeader -Value $lines -Encoding ASCII
        Write-Host "Updated $ConfigHeader with your WiFi credentials and resolved IoTConnect connection info."
        $headerUpdated = $true
    } catch {
        Write-Host "FAILED: could not update $ConfigHeader`: $_"
        exit 1
    }
}

if ([string]::IsNullOrEmpty($Port)) {
    if ($headerUpdated) {
        Write-Host "SUCCESS: config header updated. Rebuild and reflash the firmware to apply it."
        exit 0
    }
    Write-Host ("FAILED: nothing to do - pass -Port to configure an already-flashed board " +
        "directly over serial, or run this from a full clone of the repo (with the " +
        "firmware/ source present) to update the config header instead.")
    exit 1
}

$fields = [ordered]@{
    WIFI_SSID        = $WifiSsid
    WIFI_PASSWORD    = $WifiPassword
    IOTC_CPID        = $Cpid
    IOTC_ENV         = $Env
    # The firmware only ever uses this field as the MQTT client ID, so use
    # IoTConnect's own resolved client ID here rather than the raw -Duid:
    # they're the same for "dedicated" instances, but for "shared" instances
    # (e.g. POC/trial accounts) IoTConnect assigns a different client ID
    # (often "CPID-DUID"), and sending the wrong one gets the connection
    # silently rejected by the broker.
    IOTC_DUID        = $identity.id
    MQTT_BROKER_HOST = $identity.h
    MQTT_BROKER_PORT = $MqttPort
    MQTT_USERNAME    = $(if ($identity.un) { $identity.un } else { "" })
    MQTT_PUB_TOPIC   = $identity.topics.rpt
    MQTT_C2D_TOPIC   = $identity.topics.c2d
    MQTT_ACK_TOPIC   = $identity.topics.ack
    RNWF_CA_NAME     = $CaName
    RNWF_CERT_NAME   = $CertName
    RNWF_KEY_NAME    = $KeyName
}

Write-Host "`nConnecting to $Port at $Baud baud..."
try {
    $serialPort = New-Object -TypeName System.IO.Ports.SerialPort -ArgumentList @($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serialPort.NewLine = "`n"
    $serialPort.ReadTimeout = 5000
    $serialPort.Open()
} catch {
    Write-Host "FAILED: could not open $Port`: $_"
    exit 1
}

try {
    Start-Sleep -Milliseconds 200  # let the port settle before writing

    # The firmware's WiFi/MQTT connect attempts each block for many seconds
    # at a time (and this is a polled, non-interrupt-driven UART, so bytes
    # sent while it's not actively reading are lost, not just delayed) - a
    # single one-shot send has a real chance of landing in one of those
    # windows and getting no response at all, even though the device is
    # working fine. Resend the whole handshake periodically until either it
    # succeeds or we've comfortably outlasted a full connect-retry cycle.
    $overallTimeoutSec = 90
    $retryIntervalSec = 5
    $deadline = (Get-Date).AddSeconds($overallTimeoutSec)
    $attempt = 0
    $provisioned = $false

    while (-not $provisioned) {
        $attempt++
        $serialPort.DiscardInBuffer()  # discard anything stale from a previous attempt

        $serialPort.Write("PROVISION`n")
        foreach ($key in $fields.Keys) {
            $serialPort.Write("$key=$($fields[$key])`n")
        }
        $serialPort.Write("END`n")

        $response = $null
        try {
            $response = $serialPort.ReadLine().Trim()
        } catch [System.TimeoutException] {
            $response = $null
        }

        if ($response -eq "OK") {
            Write-Host "Device confirmed: OK"
            $provisioned = $true
        } elseif ($response) {
            Write-Host "FAILED: device reported an error: $response"
            exit 1
        } elseif ((Get-Date) -ge $deadline) {
            Write-Host "FAILED: device never responded - check the port/baud rate, and that the firmware is running"
            exit 1
        } else {
            Write-Host "No response yet (attempt $attempt) - the board may be mid-connection-attempt and not listening right now; retrying..."
            Start-Sleep -Seconds $retryIntervalSec
        }
    }
} finally {
    $serialPort.Close()
}

Write-Host "SUCCESS: config header updated, and the already-flashed device was live-provisioned over serial too."
