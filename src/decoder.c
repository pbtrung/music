#include "utils.h"
#include <gst/gst.h>
#include <gst/tag/tag.h>

static void print_tags(GHashTable *tag_table) {
    GList *keys = g_hash_table_get_keys(tag_table);
    const int width = 17;
    for (GList *l = keys; l != NULL; l = l->next) {
        gchar *key = (gchar *)l->data;
        GValue *value = (GValue *)g_hash_table_lookup(tag_table, key);

        if (value) {
            g_print("%-*s: ", width, key);

            if (G_VALUE_HOLDS(value, G_TYPE_STRING)) {
                g_print("%s\n", g_value_get_string(value));
            }
        }
    }
    g_list_free(keys);
    g_hash_table_destroy(tag_table);
}

static void on_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    GstPad *sink_pad = gst_element_get_static_pad(GST_ELEMENT(data), "sink");
    GstPadLinkReturn ret;
    GstCaps *caps;
    GstStructure *str;

    /* Get pad capabilities and check media type */
    caps = gst_pad_query_caps(pad, NULL);
    if (gst_caps_is_empty(caps)) {
        g_printerr("Error: Could not get pad capabilities.\n");
        goto cleanup;
    }

    str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);

    if (g_str_has_prefix(name, "audio/x-raw")) {
        ret = gst_pad_link(pad, sink_pad);
        if (ret != GST_PAD_LINK_OK) {
            g_printerr("Error linking pads: %s\n", gst_pad_link_get_name(ret));
        }
    }

cleanup:
    gst_caps_unref(caps);
    gst_object_unref(sink_pad);
}

static void dedup_tags(const GstTagList *tags, GHashTable *tag_table) {
    guint num_tags = gst_tag_list_n_tags(tags);
    for (int i = 0; i < num_tags; i++) {
        const gchar *key = gst_tag_list_nth_tag_name(tags, i);
        const GValue *value = gst_tag_list_get_value_index(tags, key, 0);
        GValue *copied_value = g_new(GValue, 1);
        g_value_init(copied_value, G_VALUE_TYPE(value));
        g_value_copy(value, copied_value);
        gchar *copied_key = g_strdup(key);

        if (!g_hash_table_contains(tag_table, copied_key)) {
            g_hash_table_insert(tag_table, copied_key, copied_value);
        }
    }
}

static void handle_msg(gboolean *terminate, gint64 *duration, gboolean *playing,
                       GstMessage *msg, GHashTable *tag_table) {
    GError *err;
    gchar *debug_info;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
        gst_message_parse_error(msg, &err, &debug_info);
        g_printerr("Error received from element %s: %s\n",
                   GST_OBJECT_NAME(msg->src), err->message);
        g_printerr("Debugging information: %s\n",
                   debug_info ? debug_info : "none");
        g_clear_error(&err);
        g_free(debug_info);
        *terminate = TRUE;
        break;
    case GST_MESSAGE_EOS:
        *terminate = TRUE;
        break;
    case GST_MESSAGE_DURATION:
        /* The duration has changed, mark the current one as invalid */
        *duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_STATE_CHANGED: {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed(msg, &old_state, &new_state,
                                        &pending_state);
        *playing = (new_state == GST_STATE_PLAYING);
    } break;
    case GST_MESSAGE_TAG: {
        GstTagList *tag_list;
        gst_message_parse_tag(msg, &tag_list);
        if (tag_list) {
            // Process tags to deduplicate
            dedup_tags(tag_list, tag_table);
            gst_tag_list_unref(tag_list);
        }
    } break;
    default:
        /* We should not reach here */
        g_printerr("Unexpected message received.\n");
        break;
    }
    gst_message_unref(msg);
}

static void g_value_destroy(gpointer data) {
    GValue *value = (GValue *)data;
    g_value_unset(value);
    g_slice_free(GValue, value);
}

void decode_audio(config_t *config, const char *input_filename) {
    GstElement *source, *decoder, *audioconvert, *audioresample, *capsfilter,
        *sink;
    GstElement *pipeline = NULL;
    GstCaps *caps = NULL;
    GstBus *bus = NULL;
    GstMessage *msg = NULL;
    GHashTable *tag_table =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_value_destroy);

    /* Create GStreamer elements */
    pipeline = gst_pipeline_new("audio-pipeline");
    source = gst_element_factory_make("filesrc", "source");
    decoder = gst_element_factory_make("decodebin", "decoder");
    audioconvert = gst_element_factory_make("audioconvert", "audioconvert");
    audioresample = gst_element_factory_make("audioresample", "audioresample");
    capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    sink = gst_element_factory_make("filesink", "sink");

    if (!pipeline || !source || !decoder || !audioconvert || !audioresample ||
        !capsfilter || !sink) {
        g_printerr("Not all elements could be created.\n");
        goto cleanup;
    }

    /* Set element properties */
    g_object_set(G_OBJECT(source), "location", input_filename, NULL);
    // config->pipe_name
    g_object_set(G_OBJECT(sink), "location", "test.pcm", NULL);

    /* Set the desired caps */
    caps = gst_caps_new_simple("audio/x-raw", "rate", G_TYPE_INT, 48000,
                               "channels", G_TYPE_INT, 2, "format",
                               G_TYPE_STRING, "S16LE", NULL);
    g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
    gst_caps_unref(caps);

    /* Build the pipeline */
    gst_bin_add_many(GST_BIN(pipeline), source, decoder, audioconvert,
                     audioresample, capsfilter, sink, NULL);
    if (!gst_element_link(source, decoder)) {
        g_printerr("Source and decoder could not be linked.\n");
        gst_object_unref(pipeline);
        goto cleanup;
    }
    if (!gst_element_link_many(audioconvert, audioresample, capsfilter, sink,
                               NULL)) {
        g_printerr("Elements could not be linked.\n");
        gst_object_unref(pipeline);
        goto cleanup;
    }

    /* Connect the decoder's pad-added signal */
    g_signal_connect(decoder, "pad-added", G_CALLBACK(on_pad_added),
                     audioconvert);

    /* Start playing */
    int ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Unable to set the pipeline to the playing state.\n");
        goto cleanup;
    }

    bus = gst_element_get_bus(pipeline);
    gboolean terminate = FALSE;
    gboolean playing = FALSE;
    gint64 duration = GST_CLOCK_TIME_NONE;
    const int width = 17;

    int printed = 0;
    do {
        msg = gst_bus_timed_pop_filtered(
            bus, 100 * GST_MSECOND,
            GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
                GST_MESSAGE_DURATION | GST_MESSAGE_TAG);

        if (msg != NULL) {
            handle_msg(&terminate, &duration, &playing, msg, tag_table);
            if (playing && printed == 0) {
                print_tags(tag_table);
                printed = 1;
            }
        } else {
            /* We got no message, this means the timeout expired */
            if (playing) {
                gint64 current = -1;
                gboolean r1 = FALSE;
                gboolean r2 = TRUE;
                r1 = gst_element_query_position(pipeline, GST_FORMAT_TIME,
                                                &current);
                /* If we didn't know it yet, query the stream duration */
                if (!GST_CLOCK_TIME_IS_VALID(duration)) {
                    r2 = gst_element_query_duration(pipeline, GST_FORMAT_TIME,
                                                    &duration);
                }
                if (r1 && r2) {
                    g_print("%-*s: %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT
                            "\r",
                            width, "position", GST_TIME_ARGS(current),
                            GST_TIME_ARGS(duration));
                }
            }
        }
    } while (!terminate);

    g_print("\n\n");

cleanup:
    if (tag_table)
        g_hash_table_destroy(tag_table);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
