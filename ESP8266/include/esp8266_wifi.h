/*
 * File:            template.h
 * Author:          XYZ
 * Date             DD.MM.YYYY
 *
 * Description:
 * Providing ...
 *
 * License:
 * This code is released under Creative Commons Legal Code CC0 1.0 Universal
 *
 * Contact:
 * email
 */

#pragma once

/****************************************************/
// INCLUDES
/****************************************************/

#include <Arduino.h>
#include <ESP8266WiFi.h>

/****************************************************/
// GLOBAL DEFINES
/****************************************************/

/****************************************************/
// GLOBAL ENUMS
/****************************************************/

/****************************************************/
// GLOBAL STRUCTURE TYPE DEFINITION
/****************************************************/

/****************************************************/
// GLOBAL STRUCTURE VARIABLE DECLARATION, INIT
/****************************************************/

/****************************************************/
// GLOBAL MACROS
/****************************************************/

/****************************************************/
// GLOBAL FUNCTIONS
/****************************************************/

#ifdef __cplusplus
extern "C" {
#endif

bool esp8266wifiConnect(const char* wifiSSID, const char* wifiPWD);
bool esp8266wifiCheckConnection(const char* wifiSSID, const char* wifiPWD);

#ifdef __cplusplus
}
#endif
