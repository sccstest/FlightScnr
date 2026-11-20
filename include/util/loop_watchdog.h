#pragma once

// Loop watchdog (Waveshare 4C): restarts the device if loop() stops making progress,
// so a hang can never leave the gift frozen. No-op on the S3 boards and the simulator.
//
// loop() reports each pass with notePass(). A small monitor task feeds an ESP-IDF
// task-watchdog "user" entry while loop() is alive; once loop() has been silent for
// too long it stops feeding and the task watchdog panics (backtrace on the UART log)
// and reboots. Allowed silence: 45 s with Wi-Fi up, 5 min while Wi-Fi is down (a
// reconnect round tries every saved network, up to 3 x 15 s each, on the loop task).
// Long waits that block loop() on purpose call keepAlive() while they make progress
// (the Wi-Fi connect screen and setup portal via inputPollLongPress(), flash writes
// of a firmware upload via Update's progress callback).

namespace util::loopwdt {

/** Arm the watchdog. Call once from setup() (the loop task), at its end. */
void start();

/** loop() completed a pass. */
void notePass();

/** A long, legitimate wait on the loop task is still alive. Ignored when called from
 *  any other task, or before start(). Cheap: one atomic store. */
void keepAlive();

}  // namespace util::loopwdt
