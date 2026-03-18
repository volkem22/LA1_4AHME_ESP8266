/**
 * @file cmd.h
 * @author Kerstin
 * @date 11.03.2026
 * @brief Command Execution
 *
 * Features:
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

#include "cli.h"

/****************************************************/
// GLOBAL DEFINES
/****************************************************/

/****************************************************/
// GLOBAL ENUMS
/****************************************************/

/****************************************************/
// GLOBAL STRUCT TYPE DEFINITION
/****************************************************/

/****************************************************/
// GLOBAL STATIC STRUCTS and VARIABLES
/****************************************************/

/****************************************************/
// GLOBAL MACROS
/****************************************************/

/****************************************************/
// GLOBAL FUNCTIONS
/****************************************************/

//Executes Commands defined by user
uint8_t cmdExecuteCommand(CliComPort *cliComPort);

#if defined(__cplusplus)
} // extern "C"
#endif
