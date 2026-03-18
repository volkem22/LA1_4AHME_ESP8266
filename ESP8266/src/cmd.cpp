/**
 * @file cmd.cpp
 * @author Kerstin
 * @date 11.03.2026
 * @brief Command execution
 */

/****************************************************/
// INCLUDES
/****************************************************/

#include <string.h>
#include <Servo.h>

#include "cmd.h"

/****************************************************/
// LOCAL DEFINES
/****************************************************/

/****************************************************/
// LOCAL ENUMS
/****************************************************/

/****************************************************/
// LOCAL STRUCT TYPE DEFINITION
/****************************************************/

/****************************************************/
// LOCAL STATIC STRUCTS and VARIABLES
/****************************************************/

/****************************************************/
// LOCAL FUNCTIONS
/****************************************************/

/****************************************************/
// LOCAL MACROS
/****************************************************/

/****************************************************/
// GLOBAL FUNCTIONS
/****************************************************/

static Servo servo;
static bool servoAttached = false;

uint8_t cmdExecuteCommand(CliComPort *cliComPort) {
    const char *cmd = cliGetFirstToken(cliComPort);

    if (strcmp(cmd, "cls") == 0) {
        cliClearScreen(cliComPort);
    } else if (strcmp(cmd, "servo") == 0) {
        const char *arg = cliGetNextToken(cliComPort);

        if (!servoAttached) {
            servo.attach(D2);
            servoAttached = true;
        }

        if (arg != NULL) {
            int angle = atoi(arg);

            if (angle < 0)
            {
                angle = 0;
                cliPrintf(cliComPort, "Angle is too small, unable to move Servo\n");
            }
            if (angle > 180)
            {
                angle = 180;
                cliPrintf(cliComPort, "Angle is too big, unable to move Servo\n");
            }
            servo.write(angle);
            cliPrintf(cliComPort, "Servo angle set to %d degrees\n", angle);
        }else{
                cliPrintf(cliComPort, "Add an angle parameter (0 to 180) to set the servo position");
        }
    }

    return 0;
}