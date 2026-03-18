/**
 * @file esp8266comport.h
 * @brief CLI transport factories for ESP8266 (HardwareSerial + WiFi TCP socket).
 *
 * This header exposes creation helpers that return populated CliTransport
 * instances for the ESP8266. The implementations live in src/esp8266/esp8266comport.c.
 */

#pragma once

#include <stdint.h>
#include "cli_transport.h"

#define COMPORT_RECONNECTED 1

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Create a transport backed by an Arduino HardwareSerial instance.
 * @param serialId 0 = Serial, 1 = Serial1 (if available)
 * @param bps Desired baud rate; 0 skips initialization
 */
CliTransport esp8266comportCreateSerialTx(uint8_t serialId, uint32_t bps);

/**
 * Create a transport that serves the CLI over a plain WiFi TCP socket.
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @param port TCP port to listen on (e.g., 23)
 */
CliTransport esp8266comportCreateWiFiTcpSocketTx(const char* ssid, const char* password, uint16_t port);

/**
 * Check WiFi TCP socket connection state.
 * @param wifiTcpSocketCli CLI port created via esp8266comportCreateWiFiTcpSocketTx
 * @param wifiTcpSocketPort port number used for display purposes
 * @return non-zero when a new client connection was detected since the last call
 */
uint8_t esp8266comportWiFiTcpSocketCheckConnection(CliComPort* wifiTcpSocketCli);

/**
 * Get current WiFi TCP socket client connection state.
 * @param wifiTcpSocketCli CLI port created via esp8266comportCreateWiFiTcpSocketTx
 * @return non-zero while a TCP client is currently connected
 */
uint8_t esp8266comportWiFiTcpSocketIsConnected(CliComPort* wifiTcpSocketCli);

#if defined(__cplusplus)
} // extern "C"
#endif
