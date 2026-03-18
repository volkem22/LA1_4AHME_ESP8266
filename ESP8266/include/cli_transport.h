 /**
 * @file cli_transport.h
 * @author Thomas Jerman
 * @date 18.12.2025
 * @version 2.0 (10.01.2026)
 * @brief Transport abstraction for cli.h.
 *
 * Features:
 *  - Generic transport interface (init/send/read/available)
 *  - Optional stdout binding via FILE* stream
 *  - Optional vprintf and PROGMEM-aware vprintf hooks
 *
 * @copyright
 * Released under the Apache License, Version 2.0, January 2004.
 * http://www.apache.org/licenses/
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************/
// INCLUDES
/****************************************************/

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

/****************************************************/
// GLOBAL DEFINES
/****************************************************/
#ifndef CLI_PSTR
#define CLI_PSTR(x) (x)
#endif

/****************************************************/
// GLOBAL ENUMS
/****************************************************/

// Transmission modes supported
typedef enum TxMode{
  IMMEDIATE  = 0,
  WHEN_READY = 1
} TxMode;

/****************************************************/
// GLOBAL STRUCTURE TYPE DEFINITION
/****************************************************/

// Forward declaration to allow helpers without including cli.h
typedef struct CliPort CliComPort;

// Generic transport abstraction used by the CLI.
typedef struct CliTransport {
  void *ctx;                                                  // transport-specific context
  uint32_t bps;                                               // configured baud rate (optional, 0 to skip init)
  int  (*init)(void *ctx, uint32_t bps);                      // optional; may be NULL if already configured
  void (*send)(void *ctx, TxMode mode, uint8_t byte);         // blocking/queued send
  uint8_t (*read)(void *ctx, TxMode mode);                    // blocking/peeked read depending on mode
  uint8_t (*available)(void *ctx);                            // >0 if data is ready to read
  void (*set_rx_enabled)(void *ctx, uint8_t enabled);         // optional; NULL if not needed
  void (*flush_rx)(void *ctx);                                // optional
  void (*flush_tx)(void *ctx);                                // optional
  FILE *stream;                                               // optional; set if stdout binding is desired
  int (*vprintf)(void *ctx, const char *fmt, va_list ap);     // optional; std string formatting
  int (*vprintf_progmem)(void *ctx, const char *fmt, va_list ap); // optional; PROGMEM-aware formatting (e.g. vfprintf_P)
  void (*bind_port)(void *ctx, CliComPort *cliComPort);       // optional; CLI port binding hook
} CliTransport;

/****************************************************/
// GLOBAL STRUCTURE VARIABLE DECLARATION, INIT
/****************************************************/

/****************************************************/
// GLOBAL MACROS
/****************************************************/

#ifdef __cplusplus
}
#endif


