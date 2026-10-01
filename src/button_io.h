#pragma once

// Tune button (D6, active-low: pressed pulls the pin to ground). For now a press
// is only logged on Serial0; the tune cycle will hook in here later.

void buttonIoBegin();

// Call every loop().
void buttonIoPoll();
