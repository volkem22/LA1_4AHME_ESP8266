// ESP8266 transport implementations for the CLI.
// Note: This file must be compiled as C++ (uses Arduino types).

#if defined(ARDUINO) && defined(ESP8266)
#ifndef __cplusplus
#error "esp8266comport.c requires C++ compilation."
#endif

#include <stdio.h>
#include <stdlib.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "cli.h"
#include "esp8266comport.h"

// Forward declarations for HardwareSerial-backed transport
static int serial_init(void *ctx, uint32_t bps);
static void serial_send(void *ctx, TxMode mode, uint8_t byte);
static uint8_t serial_read(void *ctx, TxMode mode);
static uint8_t serial_available(void *ctx);
static void serial_flush_rx(void *ctx);
static void serial_flush_tx(void *ctx);
static int serial_vprintf(void *ctx, const char *fmt, va_list ap);
static int serial_vprintf_progmem(void *ctx, const char *fmt, va_list ap);

// Forward declarations for WiFi TCP socket-backed transport
typedef struct {
    const char* ssid;
    const char* password;
    uint16_t port;
    WiFiServer* server;
    WiFiClient client;
    uint8_t started;
    uint8_t announced;
    uint8_t client_connected;
    uint8_t client_reconnected;
    CliComPort* owner;
} WiFiTcpSocketCtx;

typedef struct {
    HardwareSerial* serial;
    CliComPort* owner;
} SerialCtx;

static int wifiTcpSocket_init(void *ctx, uint32_t bps);
static void wifiTcpSocket_send(void *ctx, TxMode mode, uint8_t byte);
static uint8_t wifiTcpSocket_read(void *ctx, TxMode mode);
static uint8_t wifiTcpSocket_available(void *ctx);
static void wifiTcpSocket_flush_rx(void *ctx);
static void wifiTcpSocket_flush_tx(void *ctx);
static int wifiTcpSocket_vprintf(void *ctx, const char *fmt, va_list ap);
static int wifiTcpSocket_vprintf_progmem(void *ctx, const char *fmt, va_list ap);
static uint8_t wifiTcpSocket_client_connected(void);

static const size_t WIFITCPSOCKET_PRINT_BUF = 256;
static WiFiTcpSocketCtx* g_wifiTcpSocket_ctx = NULL;
static SerialCtx g_serial_ctxs[2] = {0};

// Select HardwareSerial by index
static HardwareSerial* get_serial(uint8_t serialId)
{
    switch (serialId)
    {
        case 0: return &Serial;
#if defined(HAVE_HWSERIAL1) || defined(ESP8266)
        case 1: return &Serial1;
#endif
        default: return NULL;
    }
}

static void serial_bind_port(void *ctx, CliComPort* cliComPort)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    if (s)
        s->owner = cliComPort;
}

static void wifiTcpSocket_bind_port(void *ctx, CliComPort* cliComPort)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (t)
        t->owner = cliComPort;
}

// ---- Serial transport callbacks ----
static int serial_init(void *ctx, uint32_t bps)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    HardwareSerial* serial = s ? s->serial : NULL;
    if (!serial)
        return -1;
    if (bps > 0)
        serial->begin(bps);
    return 0;
}

static void serial_send(void *ctx, TxMode mode, uint8_t byte)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    HardwareSerial* serial = s ? s->serial : NULL;
    if (!serial)
        return;
    if (mode == WHEN_READY)
        while (serial->availableForWrite() == 0) { yield(); }
    serial->write(byte);
}

static uint8_t serial_read(void *ctx, TxMode mode)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    HardwareSerial* serial = s ? s->serial : NULL;
    if (!serial)
        return 0;
    if (mode == WHEN_READY)
        while (!serial->available()) { yield(); }
    return serial->available() ? static_cast<uint8_t>(serial->read()) : 0;
}

static uint8_t serial_available(void *ctx)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    HardwareSerial* serial = s ? s->serial : NULL;
    return serial ? (serial->available() ? 1 : 0) : 0;
}

static void serial_flush_rx(void *ctx)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    HardwareSerial* serial = s ? s->serial : NULL;
    if (!serial)
        return;
    while (serial->available())
        (void)serial->read();
}

static void serial_flush_tx(void *ctx)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    HardwareSerial* serial = s ? s->serial : NULL;
    if (!serial)
        return;
    serial->flush();
}

static int serial_vprintf(void *ctx, const char *fmt, va_list ap)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    char stackBuf[128];
    char *buf = stackBuf;
    char *heapBuf = NULL;

    va_list ap_copy;
    va_copy(ap_copy, ap);
    int required = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (required < 0)
        return required;

    size_t bufSize = (size_t)required + 1;
    if (bufSize > sizeof(stackBuf))
    {
        heapBuf = (char*)malloc(bufSize);
        if (heapBuf)
            buf = heapBuf;
        else
            bufSize = sizeof(stackBuf);
    }

    va_list ap_copy2;
    va_copy(ap_copy2, ap);
    int written = vsnprintf(buf, bufSize, fmt, ap_copy2);
    va_end(ap_copy2);

    size_t len = (written >= 0 && (size_t)written < bufSize) ? (size_t)written : bufSize - 1;
    for (size_t i = 0; i < len; i++)
    {
        serial_send(ctx, WHEN_READY, (uint8_t)buf[i]);
        if (buf[i] == '\n')
            cliIncrementLineFeedCounter(s ? s->owner : NULL);
    }

    if (heapBuf)
        free(heapBuf);
    return written;
}

static int serial_vprintf_progmem(void *ctx, const char *fmt, va_list ap)
{
    SerialCtx* s = static_cast<SerialCtx*>(ctx);
    // Use the PROGMEM-aware formatter to avoid double-counting line feeds in the fallback path.
    char stackBuf[128];
    char *buf = stackBuf;
    char *heapBuf = NULL;

    va_list ap_copy;
    va_copy(ap_copy, ap);
    int required = vsnprintf_P(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (required < 0)
        return required;

    size_t bufSize = (size_t)required + 1;
    if (bufSize > sizeof(stackBuf))
    {
        heapBuf = (char*)malloc(bufSize);
        if (heapBuf)
            buf = heapBuf;
        else
            bufSize = sizeof(stackBuf);
    }

    va_list ap_copy2;
    va_copy(ap_copy2, ap);
    int written = vsnprintf_P(buf, bufSize, fmt, ap_copy2);
    va_end(ap_copy2);

    size_t len = (written >= 0 && (size_t)written < bufSize) ? (size_t)written : bufSize - 1;
    for (size_t i = 0; i < len; i++)
    {
        serial_send(ctx, WHEN_READY, (uint8_t)buf[i]);
        if (buf[i] == '\n')
            cliIncrementLineFeedCounter(s ? s->owner : NULL);
    }

    if (heapBuf)
        free(heapBuf);
    return written;
}

// ---- WiFi TCP socket helpers ----
static void ensure_client(WiFiTcpSocketCtx* t)
{
    if (!t || !t->server)
        return;

    if (t->client && t->client.connected())
    {
        t->client_connected = 1;
        return;
    }

    WiFiClient incoming = t->server->accept();
    if (incoming)
    {
        if (t->client)
            t->client.stop();
        t->client = incoming;
        t->client.setNoDelay(true);
        t->client_connected = 1;
        t->client_reconnected = 0;
    }
    else
    {
        t->client_connected = 0;
        t->client_reconnected = 0;
    }
}

// ---- WiFi TCP socket transport callbacks ----
static int wifiTcpSocket_init(void *ctx, uint32_t bps)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (!t)
        return -1;
    (void)bps; // unused for WiFi TCP socket

    if (!t->server)
        return -3; // no server allocated

    if (WiFi.status() != WL_CONNECTED)
        return -2;

    if (!t->started && t->server)
    {
        t->server->begin(t->port);
        t->server->setNoDelay(true);
        t->started = 1;
    }
    return 0;
}

static void wifiTcpSocket_send(void *ctx, TxMode mode, uint8_t byte)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (!t)
        return;
    ensure_client(t);
    if (!t->client || !t->client.connected())
        return;
    if (mode == WHEN_READY)
        while (t->client.connected() && t->client.availableForWrite() == 0)
            yield();
    if (t->client.connected())
        t->client.write(byte);
}

static uint8_t wifiTcpSocket_read(void *ctx, TxMode mode)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (!t)
        return 0;
    ensure_client(t);
    if (!t->client || !t->client.connected())
        return 0;
    if (mode == WHEN_READY)
        while (t->client.connected() && !t->client.available())
            yield();
    return (t->client.connected() && t->client.available()) ? (uint8_t)t->client.read() : 0;
}

static uint8_t wifiTcpSocket_available(void *ctx)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (!t)
        return 0;
    ensure_client(t);
    return (t->client && t->client.connected() && t->client.available()) ? 1 : 0;
}

static void wifiTcpSocket_flush_rx(void *ctx)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (!t || !t->client)
        return;
    while (t->client.connected() && t->client.available())
        (void)t->client.read();
}

static void wifiTcpSocket_flush_tx(void *ctx)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (!t || !t->client)
        return;
    if (t->client.connected())
        t->client.flush();
}

static int wifiTcpSocket_vprintf(void *ctx, const char *fmt, va_list ap)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (!t)
        return -1;

    va_list ap_copy;
    va_copy(ap_copy, ap);
    int required = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (required < 0)
        return required;

    size_t bufSize = (size_t)required + 1;
    char stackBuf[WIFITCPSOCKET_PRINT_BUF];
    char *buf = stackBuf;
    char *heapBuf = NULL;

    if (bufSize > sizeof(stackBuf))
    {
        heapBuf = (char*)malloc(bufSize);
        if (heapBuf)
            buf = heapBuf;
        else
            bufSize = sizeof(stackBuf);
    }

    va_list ap_copy2;
    va_copy(ap_copy2, ap);
    int written = vsnprintf(buf, bufSize, fmt, ap_copy2);
    va_end(ap_copy2);

    size_t len = (written >= 0 && (size_t)written < bufSize) ? (size_t)written : bufSize - 1;
    for (size_t i = 0; i < len; i++)
    {
        if (t->client && t->client.connected())
        {
            wifiTcpSocket_send(t, WHEN_READY, (uint8_t)buf[i]);
            if (buf[i] == '\n')
                cliIncrementLineFeedCounter(t->owner);
        }
    }

    if (heapBuf)
        free(heapBuf);
    return written;
}

static int wifiTcpSocket_vprintf_progmem(void *ctx, const char *fmt, va_list ap)
{
    WiFiTcpSocketCtx* t = static_cast<WiFiTcpSocketCtx*>(ctx);
    if (!t)
        return -1;

    // Format PROGMEM strings without triggering the cli.c fallback that double-counts '\n'.
    char stackBuf[WIFITCPSOCKET_PRINT_BUF];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int written = vsnprintf_P(stackBuf, sizeof(stackBuf), fmt, ap_copy);
    va_end(ap_copy);

    size_t len = (written >= 0 && (size_t)written < sizeof(stackBuf)) ? (size_t)written : sizeof(stackBuf) - 1;
    for (size_t i = 0; i < len; i++)
    {
        if (t->client && t->client.connected())
        {
            wifiTcpSocket_send(t, WHEN_READY, (uint8_t)stackBuf[i]);
            if (stackBuf[i] == '\n')
                cliIncrementLineFeedCounter(t->owner);
        }
    }
    return written;
}

static uint8_t wifiTcpSocket_client_connected(void)
{
    WiFiTcpSocketCtx* t = g_wifiTcpSocket_ctx;
    if (!t)
        return 0;
    ensure_client(t);
    if (!t->client || !t->client.connected())
        t->client_connected = 0;
    return t->client_connected;
}

uint8_t esp8266comportWiFiTcpSocketCheckConnection(CliComPort* wifiTcpSocketCli)
{
    if (!wifiTcpSocketCli || !g_wifiTcpSocket_ctx)
        return 0;

    if (!wifiTcpSocket_client_connected())
    {
        g_wifiTcpSocket_ctx->client_reconnected = 0;
        return 0;
    }

    if (g_wifiTcpSocket_ctx->client_reconnected)
        return 0;

    g_wifiTcpSocket_ctx->client_reconnected = 1;
    return 1;
}

uint8_t esp8266comportWiFiTcpSocketIsConnected(CliComPort* wifiTcpSocketCli)
{
    if (!wifiTcpSocketCli || !g_wifiTcpSocket_ctx)
        return 0;

    return wifiTcpSocket_client_connected();
}

// ---- Public factory functions ----
CliTransport esp8266comportCreateSerialTx(uint8_t serialId, uint32_t bps)
{
    CliTransport t = {0};
    HardwareSerial* serial = get_serial(serialId);
    SerialCtx* ctx = (serial && serialId < 2) ? &g_serial_ctxs[serialId] : NULL;
    if (serial && ctx)
    {
        ctx->serial = serial;
        t.ctx = ctx;
        t.bps = bps;
        t.init = serial_init;
        t.send = serial_send;
        t.read = serial_read;
        t.available = serial_available;
        t.set_rx_enabled = NULL;
        t.flush_rx = serial_flush_rx;
        t.flush_tx = serial_flush_tx;
        t.stream = NULL;
        t.vprintf = serial_vprintf;
        t.vprintf_progmem = serial_vprintf_progmem;
        t.bind_port = serial_bind_port;
    }
    return t;
}

CliTransport esp8266comportCreateWiFiTcpSocketTx(const char* ssid, const char* password, uint16_t port)
{
    CliTransport t = {0};
    static WiFiTcpSocketCtx ctx = {0};
    static WiFiServer server(port);

    ctx.ssid = ssid;
    ctx.password = password;
    ctx.port = port;
    ctx.server = &server;
    ctx.client = WiFiClient();
    ctx.started = 0;
    ctx.announced = 0;
    ctx.client_connected = 0;
    ctx.client_reconnected = 0;
    ctx.owner = NULL;
    g_wifiTcpSocket_ctx = &ctx;

    t.ctx = &ctx;
    t.bps = 1; // non-zero to trigger init callback
    t.init = wifiTcpSocket_init;
    t.send = wifiTcpSocket_send;
    t.read = wifiTcpSocket_read;
    t.available = wifiTcpSocket_available;
    t.set_rx_enabled = NULL;
    t.flush_rx = wifiTcpSocket_flush_rx;
    t.flush_tx = wifiTcpSocket_flush_tx;
    t.stream = NULL;
    t.vprintf = wifiTcpSocket_vprintf;
    t.vprintf_progmem = wifiTcpSocket_vprintf_progmem;
    t.bind_port = wifiTcpSocket_bind_port;
    return t;
}

#endif // ARDUINO && ESP8266
