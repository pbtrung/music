#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "database.h"

int open_database_readonly(const char *filename, sqlite3 **db) {
    int rc = sqlite3_open_v2(filename, db, SQLITE_OPEN_READONLY, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(*db));
        sqlite3_close(*db);
        return rc;
    }

    return SQLITE_OK;
}

int count_tracks(sqlite3 *db) {
    const char *query = "SELECT count(*) FROM tracks";
    sqlite3_stmt *stmt;
    int count = 0;

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

char **get_cids(sqlite3 *db, int track_id, int *num_cids) {
    const char *query = "SELECT cid FROM content WHERE track_id = ?";
    sqlite3_stmt *stmt;
    char **cids = NULL;
    int index = 0;

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n",
                sqlite3_errmsg(db));
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, track_id);

    *num_cids = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        (*num_cids)++;
    }
    sqlite3_reset(stmt);

    cids = (char **)malloc((*num_cids) * sizeof(char *));
    if (cids == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        sqlite3_finalize(stmt);
        return NULL;
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

char *get_track_name(sqlite3 *db, int track_id) {
    const char *query = "SELECT track_name FROM tracks WHERE track_id = ?";
    sqlite3_stmt *stmt;
    char *track_name = NULL;

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n",
                sqlite3_errmsg(db));
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, track_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        track_name = strdup((const char *)name);
    }

    sqlite3_finalize(stmt);
    return track_name;
}

char *get_album(sqlite3 *db, int track_id) {
    const char *query =
        "SELECT path FROM albums WHERE album_id = (SELECT album_id FROM tracks WHERE track_id = ?)";
    sqlite3_stmt *stmt;
    char *album_path = NULL;

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n",
                sqlite3_errmsg(db));
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, track_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *path = sqlite3_column_text(stmt, 0);
        album_path = strdup((const char *)path);
    }

    sqlite3_finalize(stmt);
    return album_path;
}