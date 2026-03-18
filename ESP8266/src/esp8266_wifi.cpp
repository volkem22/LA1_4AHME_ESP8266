/*
 * File:            template.c
 * Author:          XYZ
 * Date:            DD.MM.YYYY
 *
 * Description:
 * Providing ...
 */

/****************************************************/
// INCLUDES
/****************************************************/

#include "esp8266_wifi.h"

/****************************************************/
// LOCAL DEFINES
/****************************************************/

/****************************************************/
// LOCAL ENUMS
/****************************************************/

/****************************************************/
// LOCAL STRUCTURE TYPE DEFINITION
/****************************************************/

/****************************************************/
// LOCAL STRUCTURE VARIABLE DECLARATION, INIT
/****************************************************/

/****************************************************/
// LOCAL VARIABLES
/****************************************************/

// Shared state for connection attempts
static bool s_connecting = false;
static uint32_t s_lastAttempt = 0;
static const uint32_t kRetryMs = 2000; // shorten backoff to keep UI responsive
static const uint32_t kResetDelayMs = 10; // small pause after disconnect
static const uint32_t kAttemptIntervalMs = 10000; // only kick off begin() every 10s

/****************************************************/
// LOCAL MACROS
/****************************************************/

/****************************************************/
// LOCAL FUNCTIONS
/****************************************************/

/****************************************************/
// GLOBAL FUNCTIONS
/****************************************************/

//extern "C" {

// Non-blocking: kicks off (or retries) a connection attempt and returns
// true only when WL_CONNECTED. Intended to be polled from loop().
bool esp8266wifiCheckConnection(const char* ssid, const char* pwd)
{
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        s_connecting = false;
        return true;
    }

    bool terminal = (st == WL_CONNECT_FAILED || st == WL_WRONG_PASSWORD || st == WL_NO_SSID_AVAIL);
    bool dropped  = (st == WL_CONNECTION_LOST);
    if (terminal || dropped) {
        s_connecting = false;  // allow a re-attempt on next call
        s_lastAttempt = 0;     // clear backoff so we don't wait full retry after a hard fail
        WiFi.disconnect(true); // ensure we tear down any half-open state
        delay(kResetDelayMs);
    }

    uint32_t now = millis();
    if (s_connecting && now - s_lastAttempt >= kRetryMs) {
        // Timed out waiting; allow another attempt.
        s_connecting = false;
    }

    // Start (or retry) only after the backoff interval.
    if (!s_connecting && (s_lastAttempt == 0 || now - s_lastAttempt >= kAttemptIntervalMs)) {
        s_connecting = true;
        s_lastAttempt = now;
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pwd);
    }

    return false;
}

// Blocking helper for setup(): poll the non-blocking checker for up to 10s.
bool esp8266wifiConnect(const char* ssid, const char* pwd)
{
    // Simple blocking connect used during setup: start clean, wait up to 10s.
    s_connecting = false;
    s_lastAttempt = 0;

    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(kResetDelayMs);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pwd);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
    {
        delay(100);
        yield();
    }

    bool connected = (WiFi.status() == WL_CONNECTED);
    // Reset state for any later non-blocking checks in loop().
    s_connecting = false;
    s_lastAttempt = 0;
    return connected;
}


//} // extern "C"
