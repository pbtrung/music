#include "mpv.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_metadata(mpv_handle *ctx) {
    // Fetch the metadata as a node
    mpv_node node;
    if (mpv_get_property(ctx, "metadata", MPV_FORMAT_NODE, &node) < 0) {
        printf("No metadata available\n");
        return;
    }

    // Ensure it's a map
    if (node.format == MPV_FORMAT_NODE_MAP) {
        for (int i = 0; i < node.u.list->num; i++) {
            to_lowercase(node.u.list->keys[i]);
            printf("%-17s: %s\n", node.u.list->keys[i],
                   node.u.list->values[i].u.string);
        }
    }

    // Free the node
    mpv_free_node_contents(&node);
}

void print_duration(mpv_handle *ctx) {
    double duration = 0;
    if (mpv_get_property(ctx, "duration", MPV_FORMAT_DOUBLE, &duration) ==
        MPV_ERROR_SUCCESS) {
        printf("%-17s: %.2f\n", "duration", duration);
    } else {
        printf("%-17s: Unknown\n", "duration");
    }
}

void handle_mpv_event(mpv_event *event) {
    switch (event->event_id) {
    case MPV_EVENT_PLAYBACK_RESTART:
        printf("Playback restarted.\n");
        break;
    case MPV_EVENT_END_FILE: {
        mpv_event_end_file *end_file_data = (mpv_event_end_file *)event->data;
        printf("Playback ended. Reason: %d\n", end_file_data->reason);
        break;
    }
    case MPV_EVENT_FILE_LOADED:
        printf("File loaded.\n");
        break;
    case MPV_EVENT_LOG_MESSAGE: {
        mpv_event_log_message *msg = (mpv_event_log_message *)event->data;
        printf("[%s] %s", msg->prefix, msg->text);
        break;
    }
    case MPV_EVENT_IDLE:
        printf("Idle.\n");
        break;
    case MPV_EVENT_SEEK:
        printf("Seek initiated.\n");
        break;
    default:
        printf("Other event: %d\n", event->event_id);
        break;
    }
}

mpv_handle *mpv_init(config_t *config) {
    mpv_handle *ctx = mpv_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create mpv context.\n");
        exit(-1);
    }

    mpv_set_option_string(ctx, "audio-display", "no");
    mpv_set_option_string(ctx, "audio-channels", "stereo");
    mpv_set_option_string(ctx, "audio-samplerate", "48000");
    mpv_set_option_string(ctx, "audio-format", "s16");
    mpv_set_option_string(ctx, "ao", "pcm");
    mpv_set_option_string(ctx, "ao-pcm-file", config->pipe_name);
    mpv_set_option_string(ctx, "demuxer-lavf-o",
                          "protocol_whitelist=file,http,https,tcp,udp");

    char *timeout = int_to_str(config->timeout);
    mpv_set_option_string(ctx, "network-timeout", timeout);
    free(timeout);

    mpv_set_option_string(ctx, "script", "no");
    mpv_set_option_string(ctx, "ytdl", "no");
    // mpv_request_log_messages(ctx, "debug");

    mpv_initialize(ctx);
    return ctx;
}

void decode_audio(mpv_handle *ctx, const char *cmd[]) {
    mpv_command(ctx, cmd);
    while (1) {
        mpv_event *event = mpv_wait_event(ctx, -1);
        if (event->event_id == MPV_EVENT_END_FILE) {
            fprintf(stderr, "End of file event\n");
            break;
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            break;
        }
    }

    print_metadata(ctx);
    print_duration(ctx);
    fflush(stdout);

    while (1) {
        mpv_event *event = mpv_wait_event(ctx, -1);
        // handle_mpv_event(event);
        if (event->event_id == MPV_EVENT_SHUTDOWN ||
            event->event_id == MPV_EVENT_END_FILE ||
            event->event_id == MPV_EVENT_IDLE)
            break;
    }
    return;
}
