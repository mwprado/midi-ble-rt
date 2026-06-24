/*
 * midi-ble-rtd process entry point.
 *
 * Keep the public daemon binary stable and delegate lifecycle policy to the
 * orchestrator layer.  This keeps main() small and makes debugging split cleanly
 * between process startup and runtime/session orchestration.
 */

#include "mb-orchestrator.h"

int main(int argc, char **argv) {
    return mb_orchestrator_main(argc, argv);
}
