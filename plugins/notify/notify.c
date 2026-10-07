/*
    OSD Notification plugin for DeaDBeeF Player
    Copyright (C) 2009-2014 Oleksiy Yakovenko and contributors
    Copyright (C) 2026 Enhanced for Linux Mint MATE and modern Desktop Notification Specs

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
*/

#include <dbus/dbus.h>
#include <deadbeef/deadbeef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "../../gettext.h"
#include "../artwork/artwork.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define MAX_ALBUM_ART_FILE_SIZE (40 * 1024 * 1024)
#define E_NOTIFICATION_BUS_NAME "org.freedesktop.Notifications"
#define E_NOTIFICATION_INTERFACE "org.freedesktop.Notifications"
#define E_NOTIFICATION_PATH "/org/freedesktop/Notifications"

static DB_functions_t *deadbeef;
static DB_misc_t plugin;
static ddb_artwork_plugin_t *artwork_plugin;
static DB_playItem_t *last_track = NULL;
static dbus_uint32_t _replaces_id = 0;
static time_t request_timer = 0;
static int terminate = 0;
static pthread_mutex_t notify_mutex = PTHREAD_MUTEX_INITIALIZER;

#define NOTIFY_DEFAULT_TITLE "%title%"
#define NOTIFY_DEFAULT_CONTENT "%artist%$if($and(%artist%,%album%), - ,)%album%"

static char *tf_title = NULL;
static char *tf_content = NULL;

static void
init_tf (void) {
    pthread_mutex_lock (&notify_mutex);
    if (tf_title) {
        deadbeef->tf_free (tf_title);
        tf_title = NULL;
    }
    if (tf_content) {
        deadbeef->tf_free (tf_content);
        tf_content = NULL;
    }
    char format[512];
    deadbeef->conf_get_str ("notify.format_title_tf", NOTIFY_DEFAULT_TITLE, format, sizeof (format));
    tf_title = deadbeef->tf_compile (format);
    deadbeef->conf_get_str ("notify.format_content_tf", NOTIFY_DEFAULT_CONTENT, format, sizeof (format));
    tf_content = deadbeef->tf_compile (format);
    pthread_mutex_unlock (&notify_mutex);
}

static dbus_uint32_t
notify_send (DBusMessage *msg, dbus_uint32_t replaces_id) {
    DBusMessage *reply = NULL;
    DBusError error;
    dbus_error_init (&error);

    DBusConnection *conn = dbus_bus_get (DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set (&error)) {
        fprintf (stderr, "[notify] Connection failed: %s\n", error.message);
        dbus_error_free (&error);
        dbus_message_unref (msg);
        return 0;
    }

    reply = dbus_connection_send_with_reply_and_block (conn, msg, -1, &error);
    if (dbus_error_is_set (&error)) {
        fprintf (stderr, "[notify] send_with_reply_and_block error: (%s)\n", error.message);
        dbus_error_free (&error);
        dbus_message_unref (msg);
        dbus_connection_unref (conn);
        return 0;
    }

    dbus_uint32_t id = 0;
    if (reply != NULL) {
        DBusMessageIter args;
        if (dbus_message_iter_init (reply, &args)) {
            if (DBUS_TYPE_UINT32 == dbus_message_iter_get_arg_type (&args)) {
                dbus_message_iter_get_basic (&args, &id);
            }
        }
        dbus_message_unref (reply);
    }

    dbus_message_unref (msg);
    dbus_connection_unref (conn);
    return id;
}

static void
esc_xml (const char *cmd, char *esc, int size) {
    const char *src = cmd;
    char *dst = esc;
    char *end = dst + size - 1;
    while (*src && dst < end) {
        if (*src == '&') {
            if (end - dst < 5) break;
            strcpy (dst, "&amp;");
            dst += 5;
            src++;
        }
        else if (*src == '<') {
            if (end - dst < 4) break;
            strcpy (dst, "&lt;");
            dst += 4;
            src++;
        }
        else if (*src == '>') {
            if (end - dst < 4) break;
            strcpy (dst, "&gt;");
            dst += 4;
            src++;
        }
        else if (*src == '"') {
            if (end - dst < 6) break;
            strcpy (dst, "&quot;");
            dst += 6;
            src++;
        }
        else if (*src == '\\' && *(src + 1) == 'n') {
            strcpy (dst, "\n");
            dst++;
            src += 2;
        }
        else {
            *dst++ = *src++;
        }
    }
    *dst = 0;
}

static GdkPixbuf *
_load_image (const char *image_filename) {
    if (!image_filename || !image_filename[0]) {
        return NULL;
    }

    GError *error = NULL;
    GdkPixbuf *img = gdk_pixbuf_new_from_file (image_filename, &error);
    if (!img) {
        if (error) {
            g_error_free (error);
        }
        return NULL;
    }

    int max_image_size = deadbeef->conf_get_int ("notify.albumart_size", 64);
    if (max_image_size < 24) {
        max_image_size = 24;
    } else if (max_image_size > 256) {
        max_image_size = 256;
    }

    int orig_width = gdk_pixbuf_get_width (img);
    int orig_height = gdk_pixbuf_get_height (img);

    if (orig_width > max_image_size || orig_height > max_image_size) {
        double scale = min ((double)max_image_size / (double)orig_width, (double)max_image_size / (double)orig_height);
        int new_width = (int)(orig_width * scale);
        int new_height = (int)(orig_height * scale);
        if (new_width < 1) new_width = 1;
        if (new_height < 1) new_height = 1;

        GdkPixbuf *scaled_img = gdk_pixbuf_scale_simple (img, new_width, new_height, GDK_INTERP_BILINEAR);
        g_object_unref (img);
        img = scaled_img;
    }

    return img;
}

static void
append_image_data_hint (DBusMessageIter *hints_iter, const char *hint_name, GdkPixbuf *img) {
    if (!img) return;

    dbus_int32_t width = gdk_pixbuf_get_width (img);
    dbus_int32_t height = gdk_pixbuf_get_height (img);
    dbus_int32_t stride = gdk_pixbuf_get_rowstride (img);
    dbus_bool_t has_alpha = gdk_pixbuf_get_has_alpha (img);
    dbus_int32_t bits_per_sample = gdk_pixbuf_get_bits_per_sample (img);
    dbus_int32_t channels = gdk_pixbuf_get_n_channels (img);
    guchar *image_bytes = gdk_pixbuf_get_pixels (img);

    /* FreeDesktop Notification spec exact byte length:
       (height - 1) * stride + bytes_in_last_row */
    int bytes_per_pixel = (channels * bits_per_sample + 7) / 8;
    int byte_len = (height > 1) ? ((height - 1) * stride + width * bytes_per_pixel) : (width * bytes_per_pixel);

    DBusMessageIter dict_entry;
    dbus_message_iter_open_container (hints_iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry);
    dbus_message_iter_append_basic (&dict_entry, DBUS_TYPE_STRING, &hint_name);

    DBusMessageIter value_variant;
    dbus_message_iter_open_container (&dict_entry, DBUS_TYPE_VARIANT, "(iiibiiay)", &value_variant);

    DBusMessageIter struct_iter;
    dbus_message_iter_open_container (&value_variant, DBUS_TYPE_STRUCT, NULL, &struct_iter);
    dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_INT32, &width);
    dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_INT32, &height);
    dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_INT32, &stride);
    dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_BOOLEAN, &has_alpha);
    dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_INT32, &bits_per_sample);
    dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_INT32, &channels);

    DBusMessageIter data_array;
    dbus_message_iter_open_container (&struct_iter, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &data_array);
    dbus_message_iter_append_fixed_array (&data_array, DBUS_TYPE_BYTE, &image_bytes, byte_len);
    dbus_message_iter_close_container (&struct_iter, &data_array);

    dbus_message_iter_close_container (&value_variant, &struct_iter);
    dbus_message_iter_close_container (&dict_entry, &value_variant);
    dbus_message_iter_close_container (hints_iter, &dict_entry);
}

static void
append_string_hint (DBusMessageIter *hints_iter, const char *hint_name, const char *val) {
    if (!val) return;
    DBusMessageIter dict_entry;
    dbus_message_iter_open_container (hints_iter, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry);
    dbus_message_iter_append_basic (&dict_entry, DBUS_TYPE_STRING, &hint_name);

    DBusMessageIter value_variant;
    dbus_message_iter_open_container (&dict_entry, DBUS_TYPE_VARIANT, "s", &value_variant);
    dbus_message_iter_append_basic (&value_variant, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container (&dict_entry, &value_variant);
    dbus_message_iter_close_container (hints_iter, &dict_entry);
}

static dbus_uint32_t
show_notification (DB_playItem_t *track, const char *image_filename, dbus_uint32_t replaces_id, int force) {
    if (!track) return replaces_id;

    char title[1024] = {0};
    char content[1024] = {0};

    pthread_mutex_lock (&notify_mutex);
    if (!tf_title || !tf_content) {
        pthread_mutex_unlock (&notify_mutex);
        init_tf ();
        pthread_mutex_lock (&notify_mutex);
    }

    ddb_tf_context_t ctx = {
        ._size = sizeof (ddb_tf_context_t),
        .it = track,
        .flags = DDB_TF_CONTEXT_MULTILINE | DDB_TF_CONTEXT_NO_DYNAMIC,
    };

    if (tf_title) {
        deadbeef->tf_eval (&ctx, tf_title, title, sizeof (title));
    }
    if (tf_content) {
        deadbeef->tf_eval (&ctx, tf_content, content, sizeof (content));
    }
    pthread_mutex_unlock (&notify_mutex);

    char esc_content[2048] = {0};
    esc_xml (content, esc_content, sizeof (esc_content));

    /* Optional: Wrap content in <small> if small font setting is enabled */
    if (deadbeef->conf_get_int ("notify.small_font", 0)) {
        char formatted[2048];
        snprintf (formatted, sizeof (formatted), "<small>%s</small>", esc_content);
        g_strlcpy (esc_content, formatted, sizeof (esc_content));
    }

    DBusMessage *msg = dbus_message_new_method_call (
        E_NOTIFICATION_BUS_NAME, E_NOTIFICATION_PATH, E_NOTIFICATION_INTERFACE, "Notify");
    if (!msg) return replaces_id;

    if (replaces_id == 0) {
        time_t new_time = time (NULL);
        if (last_track == track && !force) {
            if (new_time - request_timer < 1) {
                dbus_message_unref (msg);
                return replaces_id;
            }
        } else {
            deadbeef->pl_lock ();
            if (last_track) {
                deadbeef->pl_item_unref (last_track);
                last_track = NULL;
            }
            last_track = track;
            deadbeef->pl_item_ref (last_track);
            deadbeef->pl_unlock ();
        }
        request_timer = new_time;
    }

    const char *v_appname = "DeaDBeeF";

    /* Determine icon/image */
    bool has_cover_file = (image_filename && image_filename[0] && strcmp (image_filename, "deadbeef") != 0);
    const char *v_iconname = has_cover_file ? image_filename : "deadbeef";
    const char *v_summary = title[0] ? title : "DeaDBeeF";
    const char *v_body = esc_content;
    dbus_int32_t v_timeout = -1;

    DBusMessageIter iter, sub;
    dbus_message_iter_init_append (msg, &iter);

    dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &v_appname);
    dbus_message_iter_append_basic (&iter, DBUS_TYPE_UINT32, &replaces_id);
    dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &v_iconname);
    dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &v_summary);
    dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &v_body);

    /* Actions (empty array) */
    dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, "s", &sub);
    dbus_message_iter_close_container (&iter, &sub);

    /* Hints */
    dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, "{sv}", &sub);

    /* Pass both image-path and image_path for maximum desktop compatibility */
    if (has_cover_file) {
        append_string_hint (&sub, "image-path", image_filename);
        append_string_hint (&sub, "image_path", image_filename);
    }

    /* Also load pixbuf to pass raw image data for daemons requiring in-memory data */
    GdkPixbuf *img = has_cover_file ? _load_image (image_filename) : NULL;
    if (img) {
        /* Spec 1.2: image-data */
        append_image_data_hint (&sub, "image-data", img);
        /* Spec 1.1 (Linux Mint MATE / mate-notification-daemon): image_data */
        append_image_data_hint (&sub, "image_data", img);
        g_object_unref (img);
    }

    dbus_message_iter_close_container (&iter, &sub);

    dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &v_timeout);

    return notify_send (msg, replaces_id);
}

typedef struct {
    DB_playItem_t *track;
} notify_query_data_t;

static void
_cover_loaded_callback (int error, ddb_cover_query_t *query, ddb_cover_info_t *cover) {
    if (!query) return;

    if (!(query->flags & DDB_ARTWORK_FLAG_CANCELLED) && query->track) {
        char *image_filename = NULL;
        if (!error && cover && cover->cover_found && cover->image_filename) {
            image_filename = strdup (cover->image_filename);
        }

        bool should_apply_kde_fix = deadbeef->conf_get_int ("notify.fix_kde_5_23_5", 0) ? true : false;
        _replaces_id = show_notification (query->track, image_filename, should_apply_kde_fix ? 0 : _replaces_id, 0);

        if (image_filename) {
            free (image_filename);
        }
    }

    if (cover && artwork_plugin && artwork_plugin->cover_info_release) {
        artwork_plugin->cover_info_release (cover);
    }

    if (query->track) {
        deadbeef->pl_item_unref (query->track);
    }
    free (query);
}

static void
notify_worker_thread (void *ctx) {
    DB_playItem_t *track = (DB_playItem_t *)ctx;
    if (!track) return;

    if (terminate) {
        deadbeef->pl_item_unref (track);
        return;
    }

    bool should_show_cover = deadbeef->conf_get_int ("notify.albumart", 1) && artwork_plugin;
    if (should_show_cover) {
        ddb_cover_query_t *q = calloc (1, sizeof (ddb_cover_query_t));
        q->_size = sizeof (ddb_cover_query_t);
        q->track = track; /* query takes over reference ownership */
        q->source_id = 0;
        artwork_plugin->cover_get (q, _cover_loaded_callback);
    } else {
        bool should_apply_kde_fix = deadbeef->conf_get_int ("notify.fix_kde_5_23_5", 0) ? true : false;
        _replaces_id = show_notification (track, NULL, should_apply_kde_fix ? 0 : _replaces_id, 0);
        deadbeef->pl_item_unref (track);
    }
}

static int
on_songstarted (DB_playItem_t *track) {
    if (track && deadbeef->conf_get_int ("notify.enable", 0)) {
        if (terminate) {
            return 0;
        }
        deadbeef->pl_item_ref (track);
        deadbeef->thread_start (notify_worker_thread, track);
    }
    return 0;
}

static int
notify_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_SONGSTARTED: {
        ddb_event_track_t *ev = (ddb_event_track_t *)ctx;
        on_songstarted (ev ? ev->track : NULL);
    } break;
    case DB_EV_SONGCHANGED: {
        ddb_event_trackchange_t *ev = (ddb_event_trackchange_t *)ctx;
        on_songstarted (ev ? ev->to : NULL);
    } break;
    case DB_EV_CONFIGCHANGED:
        init_tf ();
        break;
    }
    return 0;
}

int
notify_start (void) {
    terminate = 0;
    init_tf ();
    return 0;
}

int
notify_stop (void) {
    terminate = 1;

    deadbeef->pl_lock ();
    if (last_track) {
        deadbeef->pl_item_unref (last_track);
        last_track = NULL;
    }
    deadbeef->pl_unlock ();

    pthread_mutex_lock (&notify_mutex);
    if (tf_title) {
        deadbeef->tf_free (tf_title);
        tf_title = NULL;
    }
    if (tf_content) {
        deadbeef->tf_free (tf_content);
        tf_content = NULL;
    }
    pthread_mutex_unlock (&notify_mutex);
    return 0;
}

static int
notify_connect (void) {
    artwork_plugin = (ddb_artwork_plugin_t *)deadbeef->plug_get_for_id ("artwork2");
    return 0;
}

static int
notify_disconnect (void) {
    artwork_plugin = NULL;
    return 0;
}

static const char settings_dlg[] =
    "property \"Enable\" checkbox notify.enable 0;\n"
    "property \"Notification title format\" entry notify.format_title_tf \"" NOTIFY_DEFAULT_TITLE "\";\n"
    "property \"Notification content format\" entry notify.format_content_tf \"" NOTIFY_DEFAULT_CONTENT "\";\n"
    "property \"Show album art\" checkbox notify.albumart 1;\n"
    "property \"Album art size (px)\" entry notify.albumart_size 64;\n"
    "property \"Use small font size\" checkbox notify.small_font 0;\n"
    "property \"Don't reuse notifications (KDE quirk)\" checkbox notify.fix_kde_5_23_5 0;\n";

static DB_misc_t plugin = {
    DDB_PLUGIN_SET_API_VERSION.plugin.type = DB_PLUGIN_MISC,
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.id = "notify",
    .plugin.name = "OSD Notify",
    .plugin.descr =
        "Displays notifications when new track starts.\nRequires dbus and notification daemon to be running.\nNotification daemon should be provided by your desktop environment.\n",
    .plugin.copyright = "OSD Notification plugin for DeaDBeeF Player\n"
                        "Copyright (C) 2009-2014 Oleksiy Yakovenko and contributors\n"
                        "Copyright (C) 2026 Enhanced for Linux Mint MATE\n",
    .plugin.website = "http://deadbeef.sourceforge.net",
    .plugin.start = notify_start,
    .plugin.stop = notify_stop,
    .plugin.connect = notify_connect,
    .plugin.disconnect = notify_disconnect,
    .plugin.configdialog = settings_dlg,
    .plugin.message = notify_message,
};

DB_plugin_t *
notify_load (DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
