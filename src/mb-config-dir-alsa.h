#ifndef MB_CONFIG_DIR_ALSA_H
#define MB_CONFIG_DIR_ALSA_H

#include "mb-config.h"

#include <alsa/asoundlib.h>
#include <stdbool.h>

typedef struct MbConfigDirAlsa {
    snd_seq_t *seq;
    GArray *ports;
} MbConfigDirAlsa;

bool mb_config_dir_alsa_open_ports(const MbConfig *cfg, MbConfigDirAlsa *alsa);
void mb_config_dir_alsa_close(MbConfigDirAlsa *alsa);

#endif /* MB_CONFIG_DIR_ALSA_H */
