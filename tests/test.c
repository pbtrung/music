#include <stdio.h>
#include <stdlib.h>
#include <id3tag.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <mp3_file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    struct id3_file *file = id3_file_open(filename, ID3_FILE_MODE_READONLY);
    if (!file) {
        fprintf(stderr, "Error opening file: %s\n", filename);
        return 1;
    }

    struct id3_tag *tag = id3_file_tag(file);
    if (tag) {
        struct id3_frame *frame;
        for (int i = 0; i < tag->nframes; ++i)
        {
            frame = tag->frames[i];
            // printf("id %s\n", frame->id);
            // printf("description %s\n", frame->description);
            union id3_field *field;
            for (int j = 0; j < frame->nfields; ++j)
            {
                field = &(frame->fields[j]);
                char *field_value = id3_field_getstring(field);
                if (field_value) {
                    printf("%s: %s\n", frame->id, field_value);
                    free(field_value);
                }
            }
        }
    } else {
        printf("No ID3 tag found in file: %s\n", filename);
    }

    id3_file_close(file);
    return 0;
}
