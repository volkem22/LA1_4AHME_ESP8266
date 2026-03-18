/**
 * @file cli.h
 * @author Thomas Jerman
 * @date 20.05.2023
 * @version 2.0 (28.12.2025)
 * @brief Public API for the command line interface.
 *
 * Features:
 *  - Multiple ports via cliCreateComPort/cliAddComPort on arbitrary transports
 *  - CLI-scoped printing (RAM/PROGMEM) through cliPrintf/cliPrintf_P
 *  - Tokenization of CLI input with quoted parameters via cliGetFirstToken/cliGetNextToken
 *  - Intuitive prompting functionality
 *  - History and debug helpers: cliPrintCmdHistory, cliPrintCmdDetails, cliPrintRingBuffer, cliPrintRcvdString
 *  - Command flow: cliReceiveByte feeds a ring buffer; cliProcessRxData processes input; cliHasInput polls input
 *  - Status bar support: cliSetStatusBar, cliPrintStatusBar, cliSetStatusBarFlag, cliGetStatusBarFlag
 *  - Password masking: cliEnablePwdChar/cliDisablePwdChar
 *  - Control/arrow key handling and cursor visibility: cliEnableCtrlKeys, cliGetCtrlKey, cliHideCursor, cliShowCursor
 *  - History maintenance: cliClearCmdHistory
 *  - Transport access: (reserved for backends)
 *
 * @copyright
 * Released under the Apache License, Version 2.0, January 2004.
 * http://www.apache.org/licenses/
 */

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

/****************************************************/
// INCLUDES
/****************************************************/

#include <stdint.h>
#include "cli_transport.h"

#ifndef PSTR
#define PSTR(x) (x)
#endif
#ifndef printf_P
#define printf_P(...) printf(__VA_ARGS__)
#endif

/****************************************************/
// GLOBAL DEFINES
/****************************************************/

#ifndef CLI_PORT_COUNT_MAX
#define CLI_PORT_COUNT_MAX          0x02
#define CLI_SCREEN_LINES_MAX        0x20
#endif
#if (CLI_PORT_COUNT_MAX + 0) < 1
#error "CLI_PORT_COUNT_MAX must be >= 1"
#endif
#if (CLI_PORT_COUNT_MAX + 0) > 4
#error "CLI_PORT_COUNT_MAX should be <= 4"
#endif

#define STANDARD_PROMPT             "Cli"

#define CLI_REC_CHAR_MAX            0x3F    // only 0x1F, 0x3F, 0x7F possible
#define CLI_BUFFER_SIZE (CLI_REC_CHAR_MAX + 1)
#define PROMPT_LENGTH               0x20
#define RCV_BUFFER_PRINTING
#define HIST_BUFFER_PRINTING
#define CURSOR_HIDING

// Supported ASCII CTRL-Characters by Serial Monitor (VSC) and PuTTY, source: https://www.physics.udel.edu/~watson/scen103/ascii.html
#define CTRL_A                      0x01    // Start of heading
#define CTRL_C                      0x03    // End of text
#define CTRL_D                      0x04    // End of xmit
#define CTRL_L                      0x0C    // Form feed
#define CTRL_U                      0x15    // Neg acknowledge
#define CTRL_X                      0x18    // Cancel
#define CTRL_Y                      0x19    // End of Medium
#define CTRL_Z                      0x1A    // Substitute 

// Not Supported ASCII CTRL-Characters by Serial Monitor (VSC), but supported by PuTTY
// #define CTRL_B                   0x02    // Start of text (also used by VSC)
// #define CTRL_F                   0x06    // Acknowledge (also used bei VSC)
// #define CTRL_G                   0x07    // BELL (also used by VSC)
// #define CTRL_H                   0x08    // Backspace (DO NOT USE AS SENDER, INTERFERES WITH PROMPT)
// #define CTRL_J                   0x0A    // Line Feed (not sent by Serial Monitor of VSC)
// #define CTRL_K                   0x0B    // Vertical tab
// #define CTRL_M                   0x0D    // Carriage feed (acts as ENTER)
// #define CTRL_N                   0x0E    // Shift out (also used by VSC)
// #define CTRL_O                   0x0F    // SHift in (also used by VSC)
// #define CTRL_P                   0x10    // Data line escape (also used by VSC)
// #define CTRL_Q                   0x11    // Device control 1 (also used by VSC)
// #define CTRL_R                   0x12    // Device control 2 (also used by VSC)
// #define CTRL_S                   0x13    // Device control 3 (also used by VSC)
// #define CTRL_T                   0x14    // Device control 4 (also used by VSC)
// #define CTRL_V                   0x16    // Synchronous idle (also used by VSC)
// #define CTRL_W                   0x17    // End of xmit block (also used by VSC)

// Not Supported ASCII CTRL-Characters by both Serial Monitor (VSC) AND PuTTY
// #define CTRL_E                   0x05    // Enquiry (also used bei VSC and PUTTY)
// #define CTRL_I                   0x09    // Horizontal tab (not sent by Serial Monitor of VSC and PUTTY)
// #define CTRL_ESC                 0x1B    // Escape, CTRL-[ (not sent by Serial Monitor of VSC)
// #define CTRL_FS                  0x1C    // File separator, CTRL-\ (not sent by Serial Monitor of VSC)
// #define CTRL_GS                  0x1D    // Group separator, CTRL-] (not sent by Serial Monitor of VSC)
// #define CTRL_RS                  0x1E    // Record separator, CTRL-^ (not sent by Serial Monitor of VSC)
// #define CTRL_US                  0x1F    // Unit seperator, CTRL-_ (not sent by Serial Monitor of VSC)

// Keys transmitted as escape sequences
#define UP_ARROW_KEY                0x41
#define DOWN_ARROW_KEY              0x42
#define RIGHT_ARROW_KEY             0x43
#define LEFT_ARROW_KEY              0x44
#define END                         0x46
#define POS1                        0x48
#define DEL                         0x33
#define END_ISO8850                 0x34
#define POS1_ISO8850                0x31

// Additional defines
#define BELL                        0x07    // to be sent from controller to terminal
#define BACKSPACE                   0x08    // to be sent from controller to terminal
#define ESCAPE                      0x1B
#define DELETE                      0x7F    // to be received from controller

// SGR (Select Graphic Rendition) parameters, source: https://en.wikipedia.org/wiki/ANSI_escape_code#SGR
#define TXT_RESET_FORMAT            "\x1B[0m"
#define TXT_BOLD                    "\x1B[1m"
#define TXT_DIM                     "\x1B[2m"
#define TXT_ITALIC                  "\x1B[3m"
#define TXT_UNDERLINED              "\x1B[4m"
#define TXT_BLINK                   "\x1B[5m"
#define TXT_BLINK_RAPIDLY           "\x1B[6m"
#define TXT_COLOR_REVERSE           "\x1B[7m"
#define TXT_HIDDEN                  "\x1B[8m"
#define TXT_STRIKED_OUT             "\x1B[9m"
#define TXT_UNDERLINED_TWICE        "\x1B[21m"
#define TXT_BLACK                   "\x1B[30m"
#define TXT_RED                     "\x1B[31m"
#define TXT_GREEN                   "\x1B[32m"
#define TXT_YELLOW                  "\x1B[33m"
#define TXT_BLUE                    "\x1B[34m"
#define TXT_MAGENTA                 "\x1B[35m"
#define TXT_CYAN                    "\x1B[36m"
#define TXT_WHITE                   "\x1B[37m"
#define BCKGRND_BLACK               "\x1B[40m"
#define BCKGRND_RED                 "\x1B[41m"
#define BCKGRND_GREEN               "\x1B[42m"
#define BCKGRND_YELLOW              "\x1B[43m"
#define BCKGRND_BLUE                "\x1B[44m"
#define BCKGRND_MAGENTA             "\x1B[45m"
#define BCKGRND_CYAN                "\x1B[46m"
#define BCKGRND_WHITE               "\x1B[47m"
#define TXT_FRAMED                  "\x1B[51m"
#define TXT_BLACK_BRIGHT            "\x1B[90m"
#define TXT_RED_BRIGHT              "\x1B[91m"
#define TXT_GREEN_BRIGHT            "\x1B[92m"
#define TXT_YELLOW_BRIGHT           "\x1B[93m"
#define TXT_BLUE_BRIGHT             "\x1B[94m"
#define TXT_MAGENTA_BRIGHT          "\x1B[95m"
#define TXT_CYAN_BRIGHT             "\x1B[96m"
#define TXT_WHITE_BRIGHT            "\x1B[97m"
#define BCKGRND_BLACK_BRIGHT        "\x1B[100m"
#define BCKGRND_RED_BRIGHT          "\x1B[101m"
#define BCKGRND_GREEN_BRIGHT        "\x1B[102m"
#define BCKGRND_YELLOW_BRIGHT       "\x1B[103m"
#define BCKGRND_BLUE_BRIGHT         "\x1B[104m"
#define BCKGRND_MAGENTA_BRIGHT      "\x1B[105m"
#define BCKGRND_CYAN_BRIGHT         "\x1B[106m"
#define BCKGRND_WHITE_BRIGHT        "\x1B[107m"
#define CURSOR_TO_UPPER_LEFT_POS    "\x1B[H"

#define HIDE_CURSOR_ON              1
#define HIDE_CURSOR_OFF             0

#define MAIN_LEVEL                  0
#define CMD_LEVEL                   1

/****************************************************/
// GLOBAL ENUMS
/****************************************************/

/****************************************************/
// GLOBAL STRUCT TYPE DEFINITION
/****************************************************/

typedef struct CliPort {
  uint8_t cliId;
  CliTransport transport;
} CliComPort;

/****************************************************/
// LOCAL STATIC STRUCTS and VARIABLES
/****************************************************/

/****************************************************/
// GLOBAL MACROS
/****************************************************/

/****************************************************/
// GLOBAL FUNCTIONS
/****************************************************/

/** Configures a CLI port and return its configuration (1 on success, 0 on failure) */
uint8_t cliCreateComPort(CliComPort** cliComPort, CliTransport transport);

/** Initializes a CLI instance on the given port/transport (1 on success, 0 on failure); calls transport.bind_port if set */
uint8_t cliAddComPort(CliComPort* cliComPort);

/** Sets the status bar rendering callback (NULL for default) */
void cliSetStatusBar(CliComPort* cliComPort, void (*userStatusBar)(CliComPort*));

/** Sets the status bar flag (1 on, 0 off) */
void cliSetStatusBarFlag(CliComPort* cliComPort, unsigned char statusBarFlag);

/** Gets the status bar flag (1 on, 0 off) */
uint8_t cliGetStatusBarFlag(CliComPort* cliComPort);

/** Updates the status bar (if set by cliSetStatusBar()) */
void cliPrintStatusBar(CliComPort* cliComPort);

/** Prints to the selected CLI using a RAM-resident format string */
int cliPrintf(CliComPort* cliComPort, const char* fmt, ...);

/** Prints to the selected CLI using a PROGMEM-resident format string */
int cliPrintf_P(CliComPort* cliComPort, const char* fmt, ...);

/** Resets input state and prints the prompt for a next command to be entered */
void cliPrintPrompt(CliComPort* cliComPort, const char promptFormatting[]);

/** Gets first token (space-separated substring) from receive buffer
 * @return char pointer of first token (=command)
 */
const char *cliGetFirstToken(CliComPort* cliComPort);

/**
 * Gets next token on each consecutive call of this function, which
 * processes strings under quotes as one single string
 * @return char pointer of next token
 */
const char *cliGetNextToken(CliComPort* cliComPort);

/** Prints the received command and parameters (debug) */
void cliPrintCmdDetails(CliComPort* cliComPort);

/** Prints the current command history (if enabled) */
void cliPrintCmdHistory(CliComPort* cliComPort);

/** Clears the command history buffer */
void cliClearCmdHistory(CliComPort* cliComPort);

/** Enables password masking ('*') on input */
void cliEnablePwdChar(CliComPort* cliComPort);

/** Disables password masking on input */
void cliDisablePwdChar(CliComPort* cliComPort);

/**
 * Enables CTRL/arrow key reporting to a caller-provided location
 * @return 1 on success, 0 if pointer is NULL
 */
uint8_t cliEnableCtrlKeys(CliComPort* cliComPort, char *pCtrlKey);

/** Disables CTRL/arrow key reporting */
void cliDisableCtrlKeys(CliComPort* cliComPort);

/** Gets the last supported CTRL key if received */
char cliGetCtrlKey(CliComPort* cliComPort);

/** Hides the terminal cursor */
void cliHideCursor(CliComPort* cliComPort);

/** Shows the terminal cursor */
void cliShowCursor(CliComPort* cliComPort);

/** Disables cursor display and suppresses cursor showing during internal updates */
void cliDisableCursor(CliComPort* cliComPort);

/** Re-enables cursor display after it was disabled */
void cliEnableCursor(CliComPort* cliComPort);

/** Prints the contents of the ring buffer (debug) */
void cliPrintRingBuffer(CliComPort* cliComPort);

/** Receives serial data and stores them in a ring buffer */
void cliReceiveByte(CliComPort* cliComPort);

/** Actual Command Line Interface data processeing */
uint8_t cliProcessRxData(CliComPort* cliComPort);

/** Checks if the transport reports any available input */
uint8_t cliHasInput(CliComPort* cliComPort);

/** Increments line feed counter (typically handled by backends) */
void cliIncrementLineFeedCounter(CliComPort* cliComPort);

/** Sets the standard prompt to be used */
void cliChangeStdPrompt(CliComPort* cliComPort, const char *prompt);

/** Restores the standard prompt with CLI id appended */
void cliRestoreStdPrompt(CliComPort* cliComPort);

/** Clears entire screen in the users terminal */
void cliClearScreen(CliComPort* cliComPort);

#if defined(__cplusplus)
} // extern "C"
#endif
