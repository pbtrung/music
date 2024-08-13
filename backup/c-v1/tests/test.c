#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>

void print_id3v1(mpg123_id3v1 *v1) {
    printf("ID3v1 Tag:\n");
    printf("  Title: %.30s\n", v1->title);
    printf("  Artist: %.30s\n", v1->artist);
    printf("  Album: %.30s\n", v1->album);
    printf("  Year: %.4s\n", v1->year);
    printf("  Comment: %.30s\n", v1->comment);
    printf("  Genre: %d\n", v1->genre);
}

void print_id3v2(mpg123_id3v2 *v2) {
    printf("ID3v2 Tag:\n");
    printf("  Title: %s\n", v2->title ? v2->title->p : "N/A");
    printf("  Artist: %s\n", v2->artist ? v2->artist->p : "N/A");
    printf("  Album: %s\n", v2->album ? v2->album->p : "N/A");
    printf("  Year: %s\n", v2->year ? v2->year->p : "N/A");
    printf("  Comment: %s\n", v2->comment ? v2->comment->p : "N/A");
    printf("  Genre: %s\n", v2->genre ? v2->genre->p : "N/A");

    printf("  Additional Frames:\n");
    for (int i = 0; i < v2->texts; i++) {
        printf("    %s: %s\n", v2->text[i].lang, v2->text[i].text.p);
    }
    for (int i = 0; i < v2->comments; i++) {
        printf("    Comment[%d]: %s: %s\n", i, v2->comment_list[i].lang, v2->comment_list[i].text.p);
    }
    for (int i = 0; i < v2->pictures; i++) {
        printf("    Picture[%d]: %s, %zu bytes\n", i, v2->picture[i].description.p, v2->picture[i].size);
    }
}

int main(int argc, char **argv) {
    if(argc < 2) {
        fprintf(stderr, "Usage: %s <mp3_file>\n", argv[0]);
        return 1;
    }

    mpg123_handle *mh;
    int err;

    // Initialize the mpg123 library
    if(mpg123_init() != MPG123_OK || (mh = mpg123_new(NULL, &err)) == NULL) {
        fprintf(stderr, "Unable to initialize mpg123: %s\n", mpg123_plain_strerror(err));
        return 1;
    }

    // Open the MP3 file
    if(mpg123_open(mh, argv[1]) != MPG123_OK) {
        fprintf(stderr, "Unable to open file: %s\n", mpg123_strerror(mh));
        mpg123_delete(mh);
        mpg123_exit();
        return 1;
    }

    // Check for and print metadata
    if(mpg123_meta_check(mh) & MPG123_ID3) {
        mpg123_id3v1 *v1;
        mpg123_id3v2 *v2;
        if(mpg123_id3(mh, &v1, &v2) == MPG123_OK) {
            if(v1) print_id3v1(v1);
            if(v2) print_id3v2(v2);
        } else {
            fprintf(stderr, "Error reading ID3 tags.\n");
        }
    } else {
        printf("No ID3 metadata found.\n");
    }

    // Cleanup
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();

    return 0;
}
