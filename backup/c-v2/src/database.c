#include "database.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

apr_status_t database_close(void *data) {
    log_trace("database_close: start");
    sqlite3 *db = (sqlite3 *)data;
    sqlite3_close(db);
    log_trace("database_close: finish");
    return APR_SUCCESS;
}

void database_open_readonly(const char *filename, sqlite3 **db) {
    int rc = sqlite3_open_v2(filename, db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        log_trace("database_open_readonly: Failed to open database: %s",
                  sqlite3_errmsg(*db));
        exit(-1);
    }
}

int database_count_tracks(sqlite3 *db) {
    const char *query = "SELECT count(*) FROM tracks";
    sqlite3_stmt *stmt;
    int count = 0;

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        log_trace("database_count_tracks: Failed to prepare statement: %s",
                  sqlite3_errmsg(db));
        exit(-1);
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

char **database_get_cids(sqlite3 *db, int track_id, int *num_cids) {
    const char *query = "SELECT cid FROM content WHERE track_id = ?";
    sqlite3_stmt *stmt;
    char **cids = NULL;
    int index = 0;

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        log_trace("database_get_cids: Failed to prepare statement: %s",
                  sqlite3_errmsg(db));
        exit(-1);
    }

    sqlite3_bind_int(stmt, 1, track_id);

    *num_cids = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        (*num_cids)++;
    }
    sqlite3_reset(stmt);

    cids = (char **)malloc((*num_cids) * sizeof(char *));
    if (cids == NULL) {
        log_trace("database_get_cids: Memory allocation failed");
        exit(-1);
    }

    index = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *cid = sqlite3_column_text(stmt, 0);
        cids[index] = strdup((const char *)cid);
        index++;
    }

    sqlite3_finalize(stmt);
    return cids;
}

char *database_get_track_name(sqlite3 *db, int track_id) {
    const char *query = "SELECT track_name FROM tracks WHERE track_id = ?";
    sqlite3_stmt *stmt;
    char *track_name = NULL;

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        log_trace("database_get_track_name: Failed to prepare statement: %s",
                  sqlite3_errmsg(db));
        exit(-1);
    }

    sqlite3_bind_int(stmt, 1, track_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        track_name = strdup((const char *)name);
    }

    sqlite3_finalize(stmt);
    return track_name;
}

char *database_get_album(sqlite3 *db, int track_id) {
    const char *query = "SELECT path FROM albums WHERE album_id = (SELECT "
                        "album_id FROM tracks WHERE track_id = ?)";
    sqlite3_stmt *stmt;
    char *album_path = NULL;

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        log_trace("database_get_album: Failed to prepare statement: %s",
                  sqlite3_errmsg(db));
        exit(-1);
    }

    sqlite3_bind_int(stmt, 1, track_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *path = sqlite3_column_text(stmt, 0);
        album_path = strdup((const char *)path);
    }

    sqlite3_finalize(stmt);
    return album_path;
}