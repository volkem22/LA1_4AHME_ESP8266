/**
 * @file cli.c
 * @author Thomas Jerman
 * @date 20.05.2023
 * @brief Core implementation of the command line interface.
 */

/****************************************************/
// INCLUDES
/****************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "cli.h"

/****************************************************/
// LOCAL DEFINES
/****************************************************/

#define CLI_PRINT_BUFFER_SIZE   128
#define STATUS_BAR_RESTORED     2

// Clear screen and move Curosor to upper left position
#define CLEAR_SCREEN            "\x1B[2J\x1B[H"

/****************************************************/
// LOCAL ENUMS
/****************************************************/

/****************************************************/
// LOCAL STRUCT TYPE DEFINITION
/****************************************************/

// Definition of the command line interface struct
typedef struct Cli
{
    uint8_t rcvIndex;                   // receive index
    uint8_t rcvIndexMax;                // maximum receive index
    char rcvBuf[CLI_BUFFER_SIZE];       // receive buffer
    uint8_t rcvChar;                    // received char (unsigned for deterministic handling)
    volatile uint8_t ringBufWriteIndex; // write index (ISR-safe on 8-bit MCUs)
    volatile uint8_t ringBufReadIndex;  // read index (ISR-safe on 8-bit MCUs)
    unsigned char ringBuf[CLI_BUFFER_SIZE];   // ring buffer
    char pwdChar;                       // password character
    uint8_t escSeqState;                // escape sequence state machine
    char histBuf[CLI_BUFFER_SIZE];      // history buffer
    int histIndex;                      // index to last processed cmd
    int lastCmdIndex;                   // index of last command
    char ctrlKey;                       // variable to save received CTRL-Key
    char *pCtrlKey;                     // pointer to handle CTRL-Keys
    uint8_t statusBarFlag;              // saves print status bar flag
    uint8_t statusBarFlagReminder;      // saves print status bar flag
    uint16_t lineFeedCounter;           // counts the number of sent line feeds
    uint8_t promptLength;               // length of prompt
    void (*cliStatusBar)(CliComPort*);  // function pointer to be set to the user implemented status bar setup function
    uint8_t cliId;                      // CLI ID
    uint8_t prompt[PROMPT_LENGTH];      // prompt
    uint8_t cursorSuppressed;           // suppress showing cursor during updates
    uint8_t cursorHidden;               // tracked cursor visibility state
    CliTransport transport;             // transport callbacks + context
}Cli;

/****************************************************/
// LOCAL STATIC STRUCTS and VARIABLES
/****************************************************/

// Definition (and zero-initialization) of the command line interface struct
static Cli cliArray[CLI_PORT_COUNT_MAX];            // support for CLI instances
static CliComPort cliComPortArray[CLI_PORT_COUNT_MAX];    // support for maximum 2 CLI ports
static uint8_t nextCliId = 0;                       // next available CLI ID

/****************************************************/
// LOCAL FUNCTIONS
/****************************************************/

// Sends one byte over the configured transport
static inline void sendByte(Cli* cli, TxMode mode, uint8_t b)
{
    if (cli && cli->transport.send)
        cli->transport.send(cli->transport.ctx, mode, b);
}

// Reads one byte from the transport in the requested mode
static inline uint8_t readByte(Cli* cli, TxMode mode)
{
    return (cli && cli->transport.read) ? cli->transport.read(cli->transport.ctx, mode) : 0;
}

// Checks if data is available on the transport
static inline uint8_t dataAvailable(Cli* cli)
{
    return (cli && cli->transport.available) ? cli->transport.available(cli->transport.ctx) : 0;
}

// Enables or disable RX for this transport
static inline void setRxEnabled(Cli* cli, uint8_t enabled)
{
    if (cli && cli->transport.set_rx_enabled)
        cli->transport.set_rx_enabled(cli->transport.ctx, enabled);
}

// Flushes pending RX data if supported
static inline void flushRx(Cli* cli)
{
    if (cli && cli->transport.flush_rx)
        cli->transport.flush_rx(cli->transport.ctx);
}

// Flushes pending TX data if supported
static inline void flushTx(Cli* cli)
{
    if (cli && cli->transport.flush_tx)
        cli->transport.flush_tx(cli->transport.ctx);
}

// Core vprintf wrapper that prefers transport hooks and falls back to vsnprintf+send
static int vPrint(Cli* cli, const char* fmt, va_list ap, uint8_t is_progmem)
{
    if (!cli)
        return -1;

    if (is_progmem && cli->transport.vprintf_progmem)
        return cli->transport.vprintf_progmem(cli->transport.ctx, fmt, ap);
    if (!is_progmem && cli->transport.vprintf)
        return cli->transport.vprintf(cli->transport.ctx, fmt, ap);

    char buf[CLI_PRINT_BUFFER_SIZE];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap_copy);
    va_end(ap_copy);

    size_t len = (n < 0) ? 0 : (n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    for (size_t i = 0; i < len; i++)
    {
        sendByte(cli, WHEN_READY, (uint8_t)buf[i]);
        if (buf[i] == '\n')
            cli->lineFeedCounter++;
    }
    return n;
}

// Convenience printf-style wrapper for PROGMEM format strings
static int printProgmem(Cli* cli, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vPrint(cli, fmt, ap, 1);
    va_end(ap);
    return n;
}

// Gets CLI instance by port pointer for internal use only
static inline Cli* getCliInstance(const CliComPort* cliComPort)
{
    return (cliComPort && cliComPort->cliId < CLI_PORT_COUNT_MAX) ? &cliArray[cliComPort->cliId] : NULL;
}

// Defines default status bar setup in case the user did not set a personal one
static void defaultStatusBar(CliComPort* cliComPort)
{
    cliPrintf_P(cliComPort, CLI_PSTR(TXT_WHITE_BRIGHT BCKGRND_RED "Status bar not set! Call cliSetStatusBar(yourFunction) after cliAddComPort(..)\n" TXT_RESET_FORMAT));
    cliSetStatusBarFlag(cliComPort, 0);
}

// Sets cursor on step right
static void setCursorRight(Cli* cli)
{
    if (!cli)
        return;
    sendByte(cli, WHEN_READY, '\x1B');
    sendByte(cli, WHEN_READY, '[');
    sendByte(cli, WHEN_READY, 'C');
}

// Sets cursor on step left
static void setCursorLeft(Cli* cli)
{
    if (!cli)
        return;
    sendByte(cli, WHEN_READY, '\x1B');
    sendByte(cli, WHEN_READY, '[');
    sendByte(cli, WHEN_READY, 'D');
}

// Tokenizer for a given buffer; updates cursor and supports quoted strings.
static char *getNextTokenInBuffer(char *buf, size_t buf_size, char **cursor)
{
    if (!buf || buf_size == 0 || !cursor || !*cursor)
        return NULL;

    if (*cursor < buf || *cursor >= buf + buf_size)
        return NULL;

    char *p = *cursor;
    char *buf_end = buf + buf_size - 1;

    // Skip leading whitespace
    while (p <= buf_end && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t'))
        p++;

    if (p > buf_end || *p == '\0')
    {
        *cursor = NULL;
        return NULL;
    }

    char *start = p;
    if (*p == '"')
    {
        // Quoted token
        start = ++p;
        char *endQuote = strchr(p, '"');
        if (endQuote)
        {
            *endQuote = '\0';
            *cursor = endQuote + 1;
        }
        else
        {
            // No closing quote: treat rest of line as token and stop further parsing
            char *lineEnd = strpbrk(p, "\r\n");
            if (lineEnd)
                *lineEnd = '\0';
            else
            {
                size_t remain = (p < buf_end) ? (size_t)(buf_end - p) : 0;
                p = p + strnlen(p, remain);
                if (p > buf_end)
                    p = buf_end;
            }
            *cursor = lineEnd ? lineEnd : p;
        }
    }
    else
    {
        // Unquoted token: terminate on whitespace or end
        while (p <= buf_end && *p && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t')
            p++;
        if (p <= buf_end && *p)
        {
            *p = '\0';
            *cursor = p + 1;
        }
        else
            *cursor = NULL;
    }

    // Guard against returning pointers outside the buffer
    if (start < buf || start >= buf + buf_size)
        return NULL;

    // Ignore empty tokens (e.g., unmatched quote immediately followed by EOL)
    if (*start == '\0')
    {
        *cursor = NULL;
        return NULL;
    }

    return start;
}

// Internal tokenizer: returns next token, supports quoted strings (even as first token).
// reset = 1 starts from beginning of rcvBuf; reset = 0 continues from previous position.
static char *getNextInputToken(Cli* cli, uint8_t reset)
{
    static char* cursor[CLI_PORT_COUNT_MAX] = {0};

    if (!cli || cli->cliId >= CLI_PORT_COUNT_MAX)
        return NULL;

    if (reset || cursor[cli->cliId] == NULL)
        cursor[cli->cliId] = cli->rcvBuf;

    return getNextTokenInBuffer(cli->rcvBuf, CLI_BUFFER_SIZE, &cursor[cli->cliId]);
}

/****************************************************/
// LOCAL MACROS
/****************************************************/

#define moveCursorForward(cli, t, o)       \
{                                          \
    sendByte(cli, WHEN_READY, '\x1B');     \
    sendByte(cli, WHEN_READY, '[');        \
    sendByte(cli, WHEN_READY, t);          \
    sendByte(cli, WHEN_READY, o);          \
    sendByte(cli, WHEN_READY, 'C');        \
}

#define moveCursorPreviousLine(cli, t, o)  \
{                                          \
    sendByte(cli, WHEN_READY, '\x1B');     \
    sendByte(cli, WHEN_READY, '[');        \
    sendByte(cli, WHEN_READY, t);          \
    sendByte(cli, WHEN_READY, o);          \
    sendByte(cli, WHEN_READY, 'F');        \
}

#define clearUserInput(cli)                \
{                                          \
    sendByte(cli, WHEN_READY, '\x1B');     \
    sendByte(cli, WHEN_READY, '[');        \
    sendByte(cli, WHEN_READY, 'K');        \
}

/****************************************************/
// GLOBAL FUNCTIONS
/****************************************************/

//Configures a CLI port and return its configuration
uint8_t cliCreateComPort(CliComPort** cliComPort, CliTransport transport)
{
    if (cliComPort == NULL || *cliComPort != NULL || nextCliId >= CLI_PORT_COUNT_MAX)
        return 0;

    CliComPort* entry = &cliComPortArray[nextCliId];
    *cliComPort = entry;
    (*cliComPort)->cliId = nextCliId++;
    (*cliComPort)->transport = transport;
    return 1;
}

// Initializes a CLI instance on the given port/transport
uint8_t cliAddComPort(CliComPort* cliComPort)
{
    if (!cliComPort)
        return 0;
    if (cliComPort->cliId >= CLI_PORT_COUNT_MAX)
        return 0;

    if (cliComPort->transport.bind_port)
        cliComPort->transport.bind_port(cliComPort->transport.ctx, cliComPort);

    Cli* cli = &cliArray[cliComPort->cliId];

    // Configure transport
    cli->transport = cliComPort->transport;
    if (cli->transport.init && cli->transport.bps)
    {
        if (cli->transport.init(cli->transport.ctx, cli->transport.bps) != 0)
            return 0;
    }
    // Clear any pending RX noise from the transport
    flushRx(cli);

    // Set struct variables
    cli->rcvIndex = 0;
    cli->rcvIndexMax = 0;
    memset(cli->rcvBuf, 0, CLI_BUFFER_SIZE);
    cli->rcvChar = 0;
    cli->ringBufWriteIndex = 0;
    cli->ringBufReadIndex = 0;
    memset(cli->ringBuf, 0, CLI_BUFFER_SIZE);
    cli->pwdChar = 0;
    cli->escSeqState = 0;
    memset(cli->histBuf, 0, CLI_BUFFER_SIZE);
    cli->histIndex = CLI_BUFFER_SIZE - 1;
    cli->lastCmdIndex = CLI_BUFFER_SIZE - 1;
    cli->ctrlKey = 0;
    cli->pCtrlKey = NULL;
    cli->statusBarFlag = 0;     // saves print status bar flag
    cli->statusBarFlagReminder = STATUS_BAR_RESTORED;
    cli->lineFeedCounter = 0;   // counts the number of sent line feeds
    cli->promptLength = 0;      // length of prompt
    cli->cliStatusBar = defaultStatusBar;
    cli->cliId = cliComPort->cliId;
    memset(cli->prompt, 0, PROMPT_LENGTH);
    cli->cursorSuppressed = 0;
    cli->cursorHidden = 0;
    cliRestoreStdPrompt(cliComPort);

    cliClearScreen(cliComPort);
    cliPrintf_P(cliComPort, CLI_PSTR("Command Line Interface#%d activated\n"), cliComPort->cliId);
    return 1;
}

// Sets the status bar rendering callback (NULL for default).
void cliSetStatusBar(CliComPort* cliComPort, void (*userStatusBar)(CliComPort*))
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    if (userStatusBar != NULL)
        cli->cliStatusBar = userStatusBar;
    else
        cli->cliStatusBar = defaultStatusBar;
}

// Sets the status bar flag (1 on, 0 off)
void cliSetStatusBarFlag(CliComPort* cliComPort, uint8_t statusBarFlag)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;

    if (statusBarFlag == 0xFF)
        statusBarFlag = 0;

    switch(statusBarFlag)
    {
        case 0: cli->statusBarFlag = 0;
                break;
        case 1: cli->statusBarFlag |= 1;
                break;
        default: break;
    }
}

// Gets the status bar flag (0 = off, 1 = on, 2 = on without )
uint8_t cliGetStatusBarFlag(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return 0;
    return cli->statusBarFlag;
}

// Updates the status bar (if set by cliSetStatusBar())
void cliPrintStatusBar(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    uint8_t lineFeedsTotal = (uint8_t)(cli->lineFeedCounter > CLI_SCREEN_LINES_MAX ? CLI_SCREEN_LINES_MAX : cli->lineFeedCounter);
    if (lineFeedsTotal)
    {
        cliHideCursor(cliComPort);
        // Move cursor numOflineFeeds up and set it to its beginning
        moveCursorPreviousLine(cli, '0' + lineFeedsTotal / 10, '0' + lineFeedsTotal % 10);
        // Reset UART.lineFeedCounter to count how many line feeds are printed by cliPrintStatusBarLines and afterwards
        // until cliPrintStatusBar is called again. This way all postPromptLineFeeds can be counted as well
        cli->lineFeedCounter = 0;
        // Print status bar lines
        cli->cliStatusBar(cliComPort);
        // Move cursor to next line(s)
        while (cli->lineFeedCounter < lineFeedsTotal)
            printProgmem(cli, CLI_PSTR("\n"));
        // Move cursor horizontally to where it was before printing the status bar
        lineFeedsTotal = cli->rcvIndex + cli->promptLength; // re-using lineFeedsTotal for previous horizontal cursor position
        moveCursorForward(cli, '0' + (lineFeedsTotal / 10), '0' + (lineFeedsTotal % 10));
        cliShowCursor(cliComPort);
    }
    else cli->lineFeedCounter = 0;
}

// Prints to the selected CLI using a RAM-resident format string
int cliPrintf(CliComPort* cliComPort, const char* fmt, ...) // RAM fmt
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vPrint(cli, fmt, ap, 0);
    va_end(ap);
    return n;
}

// Prints to the selected CLI using a PROGMEM-resident format string
int cliPrintf_P(CliComPort* cliComPort, const char* fmt, ...)     // PROGMEM fmt
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vPrint(cli, fmt, ap, 1);
    va_end(ap);
    return n;
}

// Resets input state and prints the prompt for a next command to be entered
void cliPrintPrompt(CliComPort* cliComPort, const char promptFormatting[])
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    memset(cli->rcvBuf, 0, CLI_BUFFER_SIZE);
    flushRx(cli); // flush receive buffer if supported
    cli->rcvIndex = 0;
    cli->rcvIndexMax = 0;
    cli->rcvChar = 0;
    cli->ctrlKey = 0;
    cli->pCtrlKey = NULL;
    cli->escSeqState = 0;
    if (cli->statusBarFlag == 1 || cli->statusBarFlag == 7)
    {
        cli->lineFeedCounter = 0;
        cli->cliStatusBar(cliComPort);
        cli->statusBarFlag &= ~4;   // Reset CLEAR_SCREEN flag
    }
    cli->promptLength = strlen((const char*)cli->prompt);
    printProgmem(cli, CLI_PSTR("%s%s" TXT_RESET_FORMAT), promptFormatting ? promptFormatting : "", cli->prompt);
    cliShowCursor(cliComPort);
    setRxEnabled(cli, 1);
}

// Gets first token (space-separated substring) from receive buffer
const char *cliGetFirstToken(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return NULL;
    return getNextInputToken(cli, 1);
}

// Gets next token on each consecutive call of this function, which
// processes strings under quotes as one single string
const char *cliGetNextToken(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return NULL;
    return getNextInputToken(cli, 0);
}

// Prints the received command and parameters (debug)
void cliPrintCmdDetails(CliComPort* cliComPort)
{
    #ifdef RCV_BUFFER_PRINTING
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    size_t i = 0;
    // Determine length up to first zero to keep the temporary copy small
    size_t rcvLen = 0;
    while (rcvLen < CLI_BUFFER_SIZE && cli->rcvBuf[rcvLen] != '\0')
        rcvLen++;

    if (cli->rcvIndex != 0 && cli->rcvBuf[0] >= ' ' && rcvLen > 0)
    {
        // Copy only the used bytes plus terminator
        #if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
        char rcvBufCopy[rcvLen + 1];
        #else
        char rcvBufCopy[CLI_BUFFER_SIZE];
        #endif
        memcpy(rcvBufCopy, cli->rcvBuf, rcvLen);
        rcvBufCopy[rcvLen] = '\0';

        // Print received bytes from the compact copy
        printProgmem(cli, PSTR("Received bytes: "));
        while(i < rcvLen)
        {
            if(i % 8 == 0)
                printProgmem(cli, PSTR("\n"));
            printProgmem(cli, PSTR("0x%02X "), (unsigned char)rcvBufCopy[i]);
            i++;
        }
        printProgmem(cli, PSTR("\n"));

        // Tokenize using the local copy to avoid touching cli->rcvBuf
        uint8_t paramCount = 0;
        uint8_t tokenIndex = 0;
        char *cursor = rcvBufCopy;
        char *token = NULL;
        while ((token = getNextTokenInBuffer(rcvBufCopy, (size_t)rcvLen + 1, &cursor)) != NULL)
        {
            if (tokenIndex == 0)
                printProgmem(cli, PSTR("Command: [%s]\n"), token);
            else
                printProgmem(cli, PSTR("Param#%X: [%s]\n"), ++paramCount, token);
            tokenIndex++;
        }
    }
    #endif  //RCV_BUFFER_PRINTING
}

// Prints the current command history (if enabled)
void cliPrintCmdHistory(CliComPort* cliComPort)
{
    #ifdef HIST_BUFFER_PRINTING
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;

    int j = 0;
    uint8_t offset = 0;
    char histBufChar;
    if (cli->histIndex != cli->lastCmdIndex)
    {
        printProgmem(cli, CLI_PSTR("Command history:  "));
        while(j < CLI_BUFFER_SIZE)
        {
            if (++j + cli->histIndex > CLI_BUFFER_SIZE - 1)
                offset = CLI_BUFFER_SIZE;
            histBufChar = cli->histBuf[j + cli->histIndex - offset];
            if (histBufChar >= ' ')
                printProgmem(cli, CLI_PSTR("%c"), histBufChar);
            else if (histBufChar == '\n')
                printProgmem(cli, CLI_PSTR(", "));
        }
        // delete output of last printf
        printProgmem(cli, CLI_PSTR("\b\b  \n"));
    }
    #endif  //HIST_BUFFER_PRINTING
}

// Clears the command history buffer
void cliClearCmdHistory(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    memset(cli->histBuf, 0, CLI_BUFFER_SIZE);
    cli->histIndex = CLI_BUFFER_SIZE - 1;
    cli->lastCmdIndex = CLI_BUFFER_SIZE - 1;
}

// Enables password masking ('*') on input
void cliEnablePwdChar(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    cli->pwdChar = '*';
}

// Disables password masking on input
void cliDisablePwdChar(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    cli->pwdChar = 0;
}

// Enables CTRL/arrow key reporting to a caller-provided location.
// Return value:    1: pointer set successfully
//                  0: NULL pointer not accepted
uint8_t cliEnableCtrlKeys(CliComPort* cliComPort, char *pCtrlKey)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return 0;
    if(pCtrlKey)
    {
        cli->pCtrlKey = pCtrlKey;
        setRxEnabled(cli, 1);
        return 1;
    }
    printProgmem(cli, CLI_PSTR("Error: null pointer not accepted to enable CTRL-Keys.\n"));
    return 0;
}

// Disables CTRL/arrow key reporting
void cliDisableCtrlKeys(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    cli->pCtrlKey = NULL;
    cli->ctrlKey = 0;
}

// Gets the last supported CTRL key if received
char cliGetCtrlKey(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return 0;
    return cli->ctrlKey;
}

// Hides the terminal cursor
void cliHideCursor(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    if (cli->cursorHidden)
        return;
    sendByte(cli, WHEN_READY, '\x1B');
    sendByte(cli, WHEN_READY, '[');
    sendByte(cli, WHEN_READY, '?');
    sendByte(cli, WHEN_READY, '2');
    sendByte(cli, WHEN_READY, '5');
    sendByte(cli, WHEN_READY, 'l');
    cli->cursorHidden = 1;
}

// Shows the terminal cursor
void cliShowCursor(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    if (cli->cursorSuppressed)
        return;
    sendByte(cli, WHEN_READY, '\x1B');
    sendByte(cli, WHEN_READY, '[');
    sendByte(cli, WHEN_READY, '?');
    sendByte(cli, WHEN_READY, '2');
    sendByte(cli, WHEN_READY, '5');
    sendByte(cli, WHEN_READY, 'h');
    cli->cursorHidden = 0;
}

// Disables cursor display and suppresses cursor showing during internal updates
void cliDisableCursor(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    cli->cursorSuppressed = 1;
    cliHideCursor(cliComPort);
}

// Re-enables cursor display after it was disabled
void cliEnableCursor(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    cli->cursorSuppressed = 0;
    cliShowCursor(cliComPort);
}

// Prints the contents of the ring buffer (debug)
void cliPrintRingBuffer(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    printProgmem(cli, CLI_PSTR("Ring buffer\n"));
    for(int i = 0; i <= CLI_REC_CHAR_MAX; i++)
    {
        if (cli->ringBuf[i] == 0x1B)
            printProgmem(cli, CLI_PSTR("%02d:0x1B ESC\n"), i);
        else if (cli->ringBuf[i] == DELETE)
            printProgmem(cli, CLI_PSTR("%02d:0x7F DELETE\n"), i);
        else if (cli->ringBuf[i] == '\n')
            printProgmem(cli, CLI_PSTR("%02d:0x0A LINE FEED\n"), i);
        else if (cli->ringBuf[i] == '\r')
            printProgmem(cli, CLI_PSTR("%02d:0x0D CARRIAGE RETURN\n"), i);            
        else if (cli->ringBuf[i] == CTRL_A)
            printProgmem(cli, CLI_PSTR("%02d:0x01 Ctrl+A\n"), i);
        else if (cli->ringBuf[i] == CTRL_C)
            printProgmem(cli, CLI_PSTR("%02d:0x01 Ctrl+C\n"), i);
        else if (cli->ringBuf[i] == CTRL_D)
            printProgmem(cli, CLI_PSTR("%02d:0x01 Ctrl+D\n"), i);
        else if (cli->ringBuf[i] == CTRL_L)
            printProgmem(cli, CLI_PSTR("%02d:0x01 Ctrl+L\n"), i);
        else if (cli->ringBuf[i] == CTRL_U)
            printProgmem(cli, CLI_PSTR("%02d:0x01 Ctrl+U\n"), i);
        else if (cli->ringBuf[i] == CTRL_X)
            printProgmem(cli, CLI_PSTR("%02d:0x01 Ctrl+X\n"), i);
        else if (cli->ringBuf[i] == CTRL_Y)
            printProgmem(cli, CLI_PSTR("%02d:0x01 Ctrl+Y\n"), i);
        else if (cli->ringBuf[i] == CTRL_Z)
            printProgmem(cli, CLI_PSTR("%02d:0x01 Ctrl+Z\n"), i);                                                
        else
            printProgmem(cli, CLI_PSTR("%02d:0x%02X %c\n"), i, cli->ringBuf[i], cli->ringBuf[i]);
    }
}

// Receives serial data and stores them in a ring buffer
void cliReceiveByte(CliComPort* cliComPort)
{
    // Avoid shared state here; this is called from the ISR for all ports.
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    char c = readByte(cli, IMMEDIATE);
    if(c)   // refusing 0x00 being insertet into ring buffer
    {
        cli->ringBuf[cli->ringBufWriteIndex++ & CLI_REC_CHAR_MAX] = c;
        cli->ringBuf[cli->ringBufWriteIndex & CLI_REC_CHAR_MAX] = '\0';
    }
}

// Actual Command Line Interface data processeing
uint8_t cliProcessRxData(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return 0;

    if (cli->ringBuf[cli->ringBufReadIndex & CLI_REC_CHAR_MAX] != '\0')
    { 
    static unsigned char hideCursorFlag = 0;

        //STEP A - CHARACTER RECEIVING
        // save received character
        cli->rcvChar = cli->ringBuf[cli->ringBufReadIndex++ & CLI_REC_CHAR_MAX];

        // change UART.charRcvd to \n in case it is \r
        cli->rcvChar = cli->rcvChar != '\r' ? cli->rcvChar : '\n';

        // EXPERIMENTAL 06.01.2026
        /*
        // echo UTF-characters ° and § but do not further process them when passwords are read
        if(cli->pwdChar != 0 && (cli->rcvChar == 0xB0 || cli->rcvChar == 0xA7))
        {
            sendByte(cli, WHEN_READY, '*');
            return 0;
        }
        */
        // EXPERIMENTAL 06.01.2026

        // Ignore bytes with the high bit set (e.g., UTF-8 multibyte chars like umlauts, ° and §)
        if (cli->rcvChar >= 0x80)
            //if(cli->rcvChar != 0xB0 && cli->rcvChar != 0xA7 && cli->rcvChar != 0xC2)
                return 0;
        
        // check supported CTRL-characters
        if(cli->rcvChar < ESCAPE)
        {
            // only allow supported ASCII CTRL-Characters by Serial Monitor (VSC) and PuTTY
            if (cli->rcvChar == CTRL_A || cli->rcvChar == CTRL_C || cli->rcvChar == CTRL_D ||
                cli->rcvChar == CTRL_L || cli->rcvChar == CTRL_U || cli->rcvChar == CTRL_X ||
                cli->rcvChar == CTRL_Y || cli->rcvChar == CTRL_Z)
                {
                    // handle Ctrl-keys during command execution
                    if(cli->pCtrlKey)
                        *cli->pCtrlKey = cli->rcvChar;
                    else
                    {
                        // handle Ctrl-key to run trigger command execution and stop receiving
                        cli->ctrlKey = cli->rcvChar;
                        cli->lineFeedCounter++;
                        sendByte(cli, IMMEDIATE, '\n');
                    }
                    return 1;
                }
        }

        // Suppress any non-printable character except the new line character
        // by ending the if-else-if structure here, so no further code is being executed
        if(cli->rcvChar != '\n' && cli->rcvChar != ESCAPE && cli->rcvChar <= 0x1F)
            return 0;

        //STEP B - HANDLING DELETE-KEY
        // character deletion on backspace key
        else if (cli->rcvChar == DELETE && !cli->pCtrlKey)
        {
            // delete characters if cursor is not at position zero
            if (cli->rcvIndex > 0)
            {
                // delete last character in command line by moving back
                // the cursor to overwrite existing terminal character(s)
                //sendByte(cli, IMMEDIATE, BACKSPACE); CHANGED 2026-01-09
                sendByte(cli, WHEN_READY, BACKSPACE);
                // delete last character in UART.rcvBuf
                if (cli->rcvIndex == cli->rcvIndexMax)
                {
                    sendByte(cli, WHEN_READY, ' ');                     // overwrite last character to the right
                    sendByte(cli, WHEN_READY, BACKSPACE);               // move cursor left again
                    cli->rcvBuf[--cli->rcvIndex] = '\0';
                    cli->rcvIndexMax--;
                }
                // delete character within UART.rcvBuf
                else
                {
                    if (!cli->pCtrlKey)
                    {
                        cliHideCursor(cliComPort);
                        hideCursorFlag = 1;
                    }
                    // go back to the position of the character to be deleted and copy
                    // all following characters one position to the left
                    for (uint8_t i = --cli->rcvIndex; i < cli->rcvIndexMax; i++)
                        sendByte(cli, WHEN_READY, cli->rcvBuf[i] = cli->rcvBuf[i + 1]);
                    cli->rcvIndexMax--;
                    // overwrite last character in command line with a space character
                    sendByte(cli, WHEN_READY, ' ');
                    // set back cursor in command line where it was before overwritting characters
                    for (uint8_t i = cli->rcvIndex; i < cli->rcvIndexMax + 1; i++)
                        setCursorLeft(cli);
                }
            }
        }

        //STEP C - HANDLING MULTI-BYTE ESCAPE SEQUENCES
        // catch escape sequences (Arrow-Keys, POS1, DEL-KEY (ENTF) and END Key)
        else if (cli->rcvChar == 0x1B)
                cli->escSeqState = 1;
        else if ((cli->rcvChar == 0x4F || cli->rcvChar == 0x5B) && cli->escSeqState == 1)
                cli->escSeqState = 2;
        else if (cli->rcvChar >= 0x31 && cli->rcvChar <= 0x4F && cli->escSeqState == 2)
        {
            // Save the received key, when UART.pCtrlKey is set
            if (cli->pCtrlKey)
                *cli->pCtrlKey = cli->rcvChar;
            else
            {
                // Save type of key for later handling of OS1, DEL-KEY (ENTF) and END Keys
                cli->escSeqState = cli->rcvChar;
                // Arrow-Key handling if no password character is set
                if(cli->pwdChar == 0)
                {
                    if (cli->rcvChar >= UP_ARROW_KEY)
                    {
                            if (!cli->pCtrlKey)
                            {
                                //hideCursor(hideCursorFlag);
                                cliHideCursor(cliComPort);
                                hideCursorFlag = 1;
                            }

                        // overwrite current command line with space characters on UP- and DOWN Arrow-Key
                        if (cli->rcvChar == UP_ARROW_KEY || cli->rcvChar == DOWN_ARROW_KEY)
                        {
                            if(cli->rcvIndexMax)
                            {                        
                                // move cursor to the very left input position
                                while(cli->rcvIndex)
                                {
                                    setCursorLeft(cli);
                                    cli->rcvIndex--;
                                }

                                // delete all input characters shown in terminal
                                clearUserInput(cli);

                                // overwrite all characters stored in UART.rcvBuf
                                while (cli->rcvIndex < cli->rcvIndexMax)
                                    cli->rcvBuf[cli->rcvIndex++] = '\0';

                                // restore empty buffer variable states
                                cli->rcvIndex = 0;
                                cli->rcvIndexMax = 0;

                                // reset UART.lastCommandIndex to last entered command
                                if (cli->rcvChar == DOWN_ARROW_KEY)
                                    cli->lastCmdIndex = cli->histIndex + 1 > CLI_BUFFER_SIZE - 1 ? cli->histIndex + 1 - (CLI_BUFFER_SIZE) : cli->histIndex + 1;
                            }
                        }
                        // restore previous command line entered to the console on "arrow up" = 0x41
                        if (cli->rcvChar == UP_ARROW_KEY)
                        {
                            int8_t histIndex = cli->lastCmdIndex;
                            int8_t counter = 0;           
                            while(cli->histBuf[histIndex] != 0x0A && counter++ <= CLI_BUFFER_SIZE)
                            {
                                if(cli->histBuf[histIndex] != 0x00)
                                {
                                    cli->rcvBuf[cli->rcvIndex++] = cli->histBuf[histIndex];
                                    sendByte(cli, WHEN_READY, cli->histBuf[histIndex]);
                                    cli->rcvIndexMax++;
                                }
                                histIndex = ++histIndex > CLI_BUFFER_SIZE - 1 ? histIndex - (CLI_BUFFER_SIZE) : histIndex;
                            }
                            cli->lastCmdIndex = ++histIndex > CLI_BUFFER_SIZE - 1 ? histIndex - (CLI_BUFFER_SIZE) : histIndex;                        
                        }
                        // echo RIGHT-Arrow-Key
                        if (cli->rcvChar == RIGHT_ARROW_KEY)
                            if (cli->rcvBuf[cli->rcvIndex] != '\0')
                            {
                                setCursorRight(cli);
                                cli->rcvIndex++;
                            }
                        // echo LEFT-Arrow-Key
                        if (cli->rcvChar == LEFT_ARROW_KEY)
                            if (cli->rcvIndex)
                            {
                                setCursorLeft(cli);
                                cli->rcvIndex--;
                            }
                        // handle END-Key for Serial Monitor in VSC
                        if(cli->escSeqState == END)
                            while(cli->rcvIndex < cli->rcvIndexMax)
                            {
                                setCursorRight(cli);
                                cli->rcvIndex++;
                            }
                        // handle POS1-Key for Serial Monitor in VSC
                        if(cli->rcvChar == POS1)
                            while(cli->rcvIndex)
                            {
                                setCursorLeft(cli);
                                cli->rcvIndex--;
                            }
                    }
                }
            }
        }
        // suppress all CTRL+ARROW-KEYS from VSC
        else if (cli->rcvChar == 0x3B && cli->escSeqState == 0x31)
                cli->escSeqState = 0x3B;
        else if (cli->rcvChar == 0x32 && cli->escSeqState == 0x3B)
                cli->escSeqState = 0x32;
        else if (cli->rcvChar >= 0x41 && cli->rcvChar <= 0x48 && cli->escSeqState == 0x32)
                return 0;                
        else if (cli->rcvChar == 0x35 && cli->escSeqState == 0x3B)
                cli->escSeqState = 0x3C;
        else if (cli->rcvChar >= 0x41 && cli->rcvChar <= 0x44 && cli->escSeqState == 0x3C)
                return 0;
        // suppress CTRL+ENTF from VSC
        else if (cli->rcvChar == 0x3B && cli->escSeqState == 0x33)
                cli->escSeqState = 0x3D;
        else if (cli->rcvChar == 0x35 && cli->escSeqState == 0x3D)
                return 0;
        // suppress CTRL+PageUp from VSC
        else if (cli->rcvChar == 0x3B && cli->escSeqState == 0x35)
                cli->escSeqState = 0x3E;
        else if (cli->rcvChar == 0x35 && cli->escSeqState == 0x3E)
                return 0;
        // suppress CTRL+PageDown from VSC
        else if (cli->rcvChar == 0x3B && cli->escSeqState == 0x36)
                cli->escSeqState = 0x3F;
        else if (cli->rcvChar == 0x35 && cli->escSeqState == 0x3F)
                return 0;
        // suppress CTRL+END from VSC
        else if (cli->escSeqState == 0x3C && cli->rcvChar == 0x46)
                return 0;   
        // suppress CTRL+POS1 from VSC
        else if (cli->escSeqState == 0x3C && cli->rcvChar == 0x48)
                return 0;
        // handle POS1, DEL-KEY (ENTF) and END Key
        else if (cli->rcvChar == 0x7E && cli->escSeqState != 0)
        {
            if(!cli->pCtrlKey)
            {
                // only handle keys if no password character is set
                if(cli->pwdChar == 0)
                {
                    if (!cli->pCtrlKey)
                    {
                        //hideCursor(hideCursorFlag);
                        cliHideCursor(cliComPort);
                        hideCursorFlag = 1;
                    }

                    // handle POS1-Key
                    if(cli->escSeqState == POS1_ISO8850)
                        while(cli->rcvIndex)
                        {
                            setCursorLeft(cli);
                            cli->rcvIndex--;
                        }
                    // handle DEL-Key
                    if(cli->escSeqState == DEL)
                    {
                        if (cli->rcvIndex < cli->rcvIndexMax && cli->rcvChar != '\n')
                        {
                            for (int8_t i = cli->rcvIndex; i < cli->rcvIndexMax; i++)
                                sendByte(cli, WHEN_READY, cli->rcvBuf[i] = cli->rcvBuf[i + 1]);
                            sendByte(cli, WHEN_READY, ' ');
                            cli->rcvIndexMax--;
                            for (int8_t i = cli->rcvIndex; i <= cli->rcvIndexMax; i++)
                                setCursorLeft(cli);
                        }
                    }
                    // handle END-Key
                    else if(cli->escSeqState == END_ISO8850)
                        while(cli->rcvIndex < cli->rcvIndexMax)
                        {
                            setCursorRight(cli);
                            cli->rcvIndex++;
                        }
                }
            }
        }

        //STEP D - PROCESS STANDARD CHARACTER RECEIVING
        // standard character receiving
        else if (cli->rcvIndexMax < CLI_BUFFER_SIZE - 1 && !cli->pCtrlKey)
        {
            // insert characters within UART.rcvBuf if UART.rcvIndex < UART.rvcIndexMax
            if (cli->rcvIndex < cli->rcvIndexMax && cli->rcvChar != '\n')
            {
                for (int8_t i = cli->rcvIndexMax; i >= cli->rcvIndex; i--)
                    cli->rcvBuf[i + 1] = cli->rcvBuf[i];
                cli->rcvIndexMax++;
            }
            // password masking
            if (cli->pwdChar != 0 && cli->rcvChar != '\n')
            {
                //if(cli->rcvChar != 0xC2)    // First byte of UTF-encoding for ° and §
                {
                    sendByte(cli, WHEN_READY, cli->pwdChar);
                //    cli->rcvIndex--;
                }
            }
            // echo every character to sender
            else
            {
                if (cli->rcvChar == '\n')
                {
                    cliHideCursor(cliComPort);
                    cli->lineFeedCounter++;
                }
                sendByte(cli, WHEN_READY, cli->rcvChar);
            }
            // store command line end character after all received characters in UART.rcvBuf
            if (cli->rcvChar == '\n')
                cli->rcvBuf[cli->rcvIndexMax] = cli->rcvChar;
            else
            {
                // update command line due to character insertion within UART.rcvBuf
                if (cli->rcvIndex < cli->rcvIndexMax)
                {   
                    if (!cli->pCtrlKey)
                    {
                        //hideCursor(hideCursorFlag);
                        cliHideCursor(cliComPort);
                        hideCursorFlag = 1;
                    }

                    // rewrite all characters to the right of the inserted character
                    for (int8_t i = cli->rcvIndex + 1; i <= cli->rcvIndexMax; i++)
                        sendByte(cli, WHEN_READY, cli->rcvBuf[i]);
                    // set cursor left again to position it where it was before
                    for (int8_t i = cli->rcvIndex + 1; i < cli->rcvIndexMax; i++)
                        setCursorLeft(cli);
                    cli->rcvBuf[cli->rcvIndex++] = cli->rcvChar;
                }
                else
                {
                    // add normally received characters to UART.rcvBuf and increase indices
                    cli->rcvBuf[cli->rcvIndex++] = cli->rcvChar;
                    cli->rcvIndexMax++;
                }
            }
        }
        
        //STEP E - HANDLING LINE ENDING (NEW-LINE) ON FULL RECEIVE BUFFER 
        // line-end character returning when hitting enter but receive buffer is full
        else if (cli->rcvChar == '\n' && !cli->pCtrlKey)
        {
            cliHideCursor(cliComPort);
            sendByte(cli, WHEN_READY, '\n');
            cli->lineFeedCounter++;
            cli->rcvBuf[CLI_BUFFER_SIZE - 1] = '\n';
        }
        
        //STEP F - CLOSING DATA RECEIVING ON NEW-LINE CHARACTER
        // disable receive interrupt after receiving '\n'
        if (cli->rcvChar == '\n' && !cli->pCtrlKey)
        {
            int searchResult = 0;
            int compareIndex, index = 0;        
            if(cli->rcvIndex != 0)
            {
                // compare current command only with last added command in histBuf
                compareIndex = cli->histIndex + 1 > CLI_BUFFER_SIZE - 1 ? cli->histIndex + 1 - (CLI_BUFFER_SIZE) : cli->histIndex + 1;
                if (cli->histBuf[compareIndex] == cli->rcvBuf[0])
                {
                    do
                    {
                        index++;
                        compareIndex = compareIndex + index > CLI_BUFFER_SIZE - 1 ? compareIndex - (CLI_BUFFER_SIZE) : compareIndex;
                    } while (index <= cli->rcvIndex && cli->histBuf[compareIndex + index] == cli->rcvBuf[index]);
                    if (index == cli->rcvIndex + 1)
                        searchResult = 1;
                }

                cli->lastCmdIndex = cli->histIndex + 1 > CLI_BUFFER_SIZE - 1 ? cli->histIndex + 1 - (CLI_BUFFER_SIZE) : cli->histIndex + 1;
                if (searchResult == 0)
                {
                    // copy the received string into UART.histBuf, if no pwdChar is set
                    if (cli->pwdChar == 0 && cli->rcvIndexMax >= 1)           //UART.rvcIndexMax > 1
                    {         
                        for (int8_t counter = 0; counter <= cli->rcvIndexMax; counter++)
                        {
                            cli->histBuf[cli->histIndex--] = cli->rcvBuf[cli->rcvIndexMax - counter];
                            cli->histIndex = cli->histIndex < 0 ? cli->histIndex + CLI_BUFFER_SIZE : cli->histIndex;
                        }
                        cli->histIndex = cli->histIndex < 0 ? cli->histIndex + CLI_BUFFER_SIZE : cli->histIndex;

                        // delete partly overwritten commands in UART.histBuf
                        if (cli->histBuf[cli->histIndex] != 0x00 && cli->histBuf[cli->histIndex] != 0x0A)
                        {
                            int index = cli->histIndex;
                            for(int8_t counter = 0; cli->histBuf[index] != 0x0A && counter < CLI_BUFFER_SIZE; counter++)
                            {
                                cli->histBuf[index--] = 0x00;
                                index = index < 0 ? index + CLI_BUFFER_SIZE : index;        
                            }
                        }
                        // set index for last added command
                        cli->lastCmdIndex = cli->histIndex + 1 > CLI_BUFFER_SIZE - 1 ? cli->histIndex + 1 - (CLI_BUFFER_SIZE) : cli->histIndex + 1;
                    }
                }
            }
            setRxEnabled(cli, 0);
            return 1;
        }

        if (hideCursorFlag == 1)
        {
            cliShowCursor(cliComPort);
            hideCursorFlag = 0;
        }
    }
    return 0;
}

// Checks if the transport reports any available input
uint8_t cliHasInput(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    return dataAvailable(cli);
}

// Increments line feed counter (typically handled by backends)
void cliIncrementLineFeedCounter(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (cli)
        cli->lineFeedCounter++;
}

// Sets the prompt to be used
void cliChangeStdPrompt(CliComPort* cliComPort, const char *prompt)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;

    // Disables lineFeedCounter reset and printing of statusbar in cliPrintPrompt
    if(cli->statusBarFlag == 1)
        cli->statusBarFlag |= 2;    // this makes statusBarFlag = 3
                                    // 3 = statusbar is fixed and updating
    size_t len = 0;
    if (prompt)
    {
        len = strnlen(prompt, PROMPT_LENGTH - 1);
        memcpy(cli->prompt, prompt, len);
    }
    cli->prompt[len] = '\0';
    cli->promptLength = (unsigned char)len;
}

// Restores the standard prompt with CLI id appended
void cliRestoreStdPrompt(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;
    
    char promptBuf[PROMPT_LENGTH];
    int n = snprintf(promptBuf, sizeof(promptBuf), STANDARD_PROMPT "%u>", cli->cliId);
    if (n < 0)
        promptBuf[0] = '\0';
    cliChangeStdPrompt(cliComPort, promptBuf);

    // Re-enables lineFeedCounter reset and printing of statusbar in cliPrintPrompt
    if(cli->statusBarFlag == 3)     // 3 = statusbar is fixed and updating
        cli->statusBarFlag &= ~2;   // deletes statusbar fixing
}

/** Clears entire screen in the users terminal */
void cliClearScreen(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return;

    printProgmem(cli, CLI_PSTR(CLEAR_SCREEN));

    if(cli->statusBarFlag == 3)     // 3 = statusbar is fixed and updating
        cli->statusBarFlag |= 4;    // this makes statusBarFlag = 7
                                    // 7 = allow re-printing statusbar after CLEAR_SCREEN
                                    // will be immediately set back to 3 in next call of cliPrintPrompt
}

/*
// EXPERIMENTAL 06.01.2026

// Gets the entire received string without tokenizing
//const char *cliGetReceivedString(CliComPort* cliComPort);

// Gets the entire received string without tokenizing
const char *cliGetReceivedString(CliComPort* cliComPort)
{
    Cli* cli = getCliInstance(cliComPort);
    if (!cli)
        return NULL;
    int endIndex = (cli->ringBufWriteIndex & CLI_REC_CHAR_MAX) - 1;
    int i = 0;
    cli->ringBuf[endIndex] = '\0';
    printProgmem(cli, PSTR("WriteIndex: %d\n"), cli->ringBufWriteIndex & CLI_REC_CHAR_MAX);
    printProgmem(cli, PSTR("ReadIndex: %d\n"), cli->ringBufReadIndex & CLI_REC_CHAR_MAX);
    for(i = endIndex; i >= 0 && cli->ringBuf[i] != '\r'; i--);

    printProgmem(cli, PSTR("IndexFound: %d\n"), i + 1);
        return (char*) (&cli->ringBuf[i + 1]);
    return NULL;
}
// EXPERIMENTAL 06.01.2026
*/

