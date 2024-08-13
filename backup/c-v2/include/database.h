#ifndef DATABASE_H
#define DATABASE_H

#include <apr_pools.h>
#include <sqlite3.h>

int database_count_tracks(sqlite3 *db);
char **database_get_cids(sqlite3 *db, int track_id, int *num_cids);
char *database_get_track_name(sqlite3 *db, int track_id);
char *database_get_album(sqlite3 *db, int track_id);
void database_open_readonly(const char *filename, sqlite3 **db);
apr_status_t database_close(void *data);

#endif // DATABASE_H
