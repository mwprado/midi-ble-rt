#ifndef MB_LEGACY_CORE_H
#define MB_LEGACY_CORE_H

/*
 * Transitional boundary for the legacy daemon core.
 *
 * The orchestrator still needs the App/Config structs and helper functions that
 * currently live in midi-ble-rtd.c.  They are static because that file used to
 * own the whole daemon.  Keeping this include behind one explicit boundary makes
 * the technical debt visible and local while the helpers are extracted into
 * normal modules.
 *
 * Do not add new functionality here.  Remove this header once the legacy helpers
 * have been moved to mb-bluez, mb-gatt-midi and mb-alsa-port modules.
 */

#define main midi_ble_rtd_legacy_main
#include "midi-ble-rtd.c"
#undef main

#endif /* MB_LEGACY_CORE_H */
