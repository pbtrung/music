#ifndef MPV_H
#define MPV_H

#include "config.h"
#include <mpv/client.h>

mpv_handle *mpv_init(config_t *config);
void decode_audio(mpv_handle *ctx, const char *cmd[]);

#endif // MPV_H
