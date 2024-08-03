#include "utils.h"
#include <gst/gst.h>
#include <gst/tag/tag.h>

static void print_metadata(GstTagList *tags) {
    guint num_tags, i;
    num_tags = gst_tag_list_n_tags(tags);

    num_tags = gst_tag_list_n_tags(tags);
    const int width = 17;
    for (i = 0; i < num_tags; i++) {
        const gchar *key = gst_tag_list_nth_tag_name(tags, i);
        const GValue *value = gst_tag_list_get_value_index(tags, key, 0);

        if (value) {
            g_print("%-*s: ", width, key);
            if (G_VALUE_HOLDS(value, G_TYPE_STRING)) {
                g_print("%s\n", g_value_get_string(value));
            } else if (G_VALUE_HOLDS(value, G_TYPE_INT)) {
                g_print("%d\n", g_value_get_int(value));
            } else if (G_VALUE_HOLDS(value, G_TYPE_DOUBLE)) {
                g_print("%f\n", g_value_get_double(value));
            } else if (G_VALUE_HOLDS(value, G_TYPE_BOOLEAN)) {
                g_print("%s\n", g_value_get_boolean(value) ? "true" : "false");
            } else if (G_VALUE_HOLDS(value, GST_TYPE_TAG_LIST)) {
                g_print("[Tag List]\n");
                print_metadata(g_value_get_boxed(value));
            } else {
                g_print("Unknown type\n");
            }
        }
    }
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

void decode_audio(config_t *config, const char *input_filename) {
    GstElement *pipeline, *source, *decoder, *audioconvert, *audioresample,
        *capsfilter, *sink;
    GstCaps *caps = NULL;
    GstBus *bus = NULL;
    GstMessage *msg = NULL;

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
    g_object_set(G_OBJECT(sink), "location", config->pipe_name, NULL);

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
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* Wait until error or EOS */
    bus = gst_element_get_bus(pipeline);
    while (TRUE) {
        msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                         GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
                                             GST_MESSAGE_TAG);

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err;
            gchar *debug_info;
            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("Error: %s\n", err->message);
            g_printerr("Debug info: %s\n", debug_info ? debug_info : "none");
            g_clear_error(&err);
            g_free(debug_info);
            break;
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            break;
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_TAG) {
            GstTagList *tag_list;
            gst_message_parse_tag(msg, &tag_list);
            if (tag_list) {
                print_metadata(tag_list);
                gst_tag_list_unref(tag_list);
            }
        }
        gst_message_unref(msg);
    }

cleanup:
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
