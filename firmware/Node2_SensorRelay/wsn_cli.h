/* =============================================================================
 *  wsn_cli.h  --  Serial command interface, identical on all four nodes.
 *
 *  Everything the demonstration needs can be driven from any node's serial
 *  console, and the commands that affect the network are broadcast as control
 *  frames so it does not matter which board you happen to have open.
 * ========================================================================== */

#ifndef WSN_CLI_H
#define WSN_CLI_H

#include <Arduino.h>

void cliBegin(void);
void cliLoop(void);
void cliPrintHelp(void);

#endif /* WSN_CLI_H */
