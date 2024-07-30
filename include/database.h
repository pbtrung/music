#ifndef DATABASE_H
#define DATABASE_H

#include <sqlite3.h>

int count_tracks(sqlite3 *db);
char **get_cids(sqlite3 *db, int track_id, int *num_cids);
char *get_track_name(sqlite3 *db, int track_id);
char *get_album(sqlite3 *db, int track_id);
int open_database_readonly(const char *filename, sqlite3 **db);

#endif // DATABASE_H
