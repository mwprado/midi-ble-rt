#ifndef MB_ORCHESTRATOR_H
#define MB_ORCHESTRATOR_H

/*
 * Process-level orchestration entry point.
 *
 * The public daemon binary should stay named midi-ble-rtd.  The orchestrator
 * owns the threaded RX/TX data path and session lifecycle policy.  Lower-level
 * ALSA, BlueZ/GATT and BLE-MIDI helpers are still being extracted from the
 * legacy daemon core in follow-up commits.
 */

int mb_orchestrator_main(int argc, char **argv);

#endif /* MB_ORCHESTRATOR_H */
