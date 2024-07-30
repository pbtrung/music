#ifndef TRACK_H
#define TRACK_H

#include "downloader.h"

char *track_extract_metadata(file_downloader_t *infos, int num_files);
void track_decode(uv_work_t *req);

#endif // TRACK_H
