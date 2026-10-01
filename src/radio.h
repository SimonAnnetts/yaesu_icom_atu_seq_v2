#pragma once

#include <Arduino.h>

// Arduino-originated CAT commands (Port 2), driven by Serial0 debug keys for
// now. Everything goes through the bridge's arbiter, so it is safe with a PC
// polling at full rate and works equally with no PC attached.
//
//   ?  help        f  freq + mode      x  TX status      o  CAT on
//   u/l/a  set mode USB / LSB / AM     m  restore the mode last read by 'f'
//   k  key PTT: press twice within 5s; auto-releases after 3s (DUMMY LOAD)
//   z  PTT off now

void radioHandleChar(char c);

// Call every loop(): prints results, runs the PTT arm/auto-release timers.
void radioPoll();
