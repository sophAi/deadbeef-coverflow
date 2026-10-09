/*
 * DeaDBeeF Video & CoverArt GTK3 Widget Plugin
 *
 * Plays MP4 and video files with Nvidia NVDEC hardware acceleration via libmpv.
 * Displays high-quality CoverArt when playing non-video music files.
 * Supports smooth fullscreen toggle via double-click, F11, or F key.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <deadbeef/deadbeef.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <glib/gi18n.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <locale.h>
#include <X11/Xlib.h>

#include <mpv/client.h>
#include "../gtkui/gtkui_api.h"
#include "../artwork/artwork.h"

DB_functions_t *deadbeef;
static ddb_gtkui_t *gtkui_plugin;

/* Forward declaration */
typedef struct w_video_s w_video_t;

struct w_video_s {
    ddb_gtkui_widget_t base;

    GtkWidget *container;        /* Outer GtkBox */
    GtkWidget *event_box;        /* EventBox capturing clicks & key presses */
    GtkWidget *content_box;      /* Stacked layout */
    GtkWidget *video_area;       /* GtkDrawingArea for MPV X11 embedding */
    GtkWidget *cover_area;       /* GtkDrawingArea for CoverArt rendering */
    GtkWidget *fs_window;        /* Fullscreen GtkWindow */
    GtkWidget *fs_cover_area;    /* Fullscreen CoverArt GtkDrawingArea */

    mpv_handle *mpv;
    gboolean mpv_inited;
    gboolean is_video;
    gboolean is_fullscreen;
    gboolean is_paused;
    gboolean toggle_pending;

    GdkPixbuf *cover_pixbuf;
    char *current_uri;
    char *current_title;
    char *current_artist;

    ddb_artwork_plugin_t *artwork_plugin;
    int64_t artwork_source_id;

    guint sync_timer_id;
    guint cursor_hide_timer_id;
    gboolean cursor_hidden;
};

/* Keep a global list of active widget instances */
static GList *active_widgets = NULL;

/* ---------------- Helper: Format Detection ---------------- */

static gboolean
is_video_file (const char *uri) {
    if (!uri) {
        return FALSE;
    }
    const char *dot = strrchr (uri, '.');
    if (!dot) {
        return FALSE;
    }
    dot++;
    return (!strcasecmp (dot, "mp4")  || !strcasecmp (dot, "m4v")  ||
            !strcasecmp (dot, "mkv")  || !strcasecmp (dot, "webm") ||
            !strcasecmp (dot, "avi")  || !strcasecmp (dot, "mov")  ||
            !strcasecmp (dot, "flv")  || !strcasecmp (dot, "wmv")  ||
            !strcasecmp (dot, "ts")   || !strcasecmp (dot, "mpg")  ||
            !strcasecmp (dot, "mpeg") || !strcasecmp (dot, "vob"));
}

/* ---------------- Fullscreen & Cursor Management ---------------- */

static void
restore_normal_cursor (w_video_t *w) {
    if (!w || !w->cursor_hidden) {
        return;
    }
    GdkWindow *gdk_win = NULL;
    if (w->fs_window && gtk_widget_get_visible (w->fs_window)) {
        gdk_win = gtk_widget_get_window (w->fs_window);
    } else if (w->video_area && gtk_widget_get_realized (w->video_area)) {
        gdk_win = gtk_widget_get_window (w->video_area);
    }
    if (gdk_win) {
        gdk_window_set_cursor (gdk_win, NULL);
    }
    w->cursor_hidden = FALSE;
}

static gboolean
hide_cursor_cb (gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w || !w->is_fullscreen) {
        return G_SOURCE_REMOVE;
    }

    GdkWindow *gdk_win = gtk_widget_get_window (w->fs_window);
    if (gdk_win) {
        GdkDisplay *disp = gdk_window_get_display (gdk_win);
        GdkCursor *blank = gdk_cursor_new_for_display (disp, GDK_BLANK_CURSOR);
        gdk_window_set_cursor (gdk_win, blank);
        g_object_unref (blank);
        w->cursor_hidden = TRUE;
    }
    w->cursor_hide_timer_id = 0;
    return G_SOURCE_REMOVE;
}

static void
schedule_cursor_hide (w_video_t *w) {
    if (!w || !w->is_fullscreen) {
        return;
    }
    restore_normal_cursor (w);
    if (w->cursor_hide_timer_id) {
        g_source_remove (w->cursor_hide_timer_id);
    }
    w->cursor_hide_timer_id = g_timeout_add (2000, hide_cursor_cb, w);
}

static gboolean
on_fs_window_configure (GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && w->is_fullscreen && w->is_video && w->video_area && gtk_widget_get_realized (w->video_area)) {
        GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
        if (gdk_v) {
            Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
            Window xv = GDK_WINDOW_XID (gdk_v);
            XResizeWindow (dpy, xv, event->width, event->height);
            XFlush (dpy);
        }
    }
    return FALSE;
}

static gboolean
do_toggle_fullscreen_idle (gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w) {
        return G_SOURCE_REMOVE;
    }
    w->toggle_pending = FALSE;

    if (!w->is_fullscreen) {
        /* Enter Fullscreen */
        w->is_fullscreen = TRUE;
        gtk_window_fullscreen (GTK_WINDOW (w->fs_window));
        gtk_widget_show_all (w->fs_window);

        if (w->is_video) {
            if (w->fs_cover_area) {
                gtk_widget_hide (w->fs_cover_area);
            }
            if (w->video_area && gtk_widget_get_realized (w->video_area)) {
                GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
                GdkWindow *gdk_fs = gtk_widget_get_window (w->fs_window);
                if (gdk_v && gdk_fs) {
                    Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                    Window xv = GDK_WINDOW_XID (gdk_v);
                    Window xfs = GDK_WINDOW_XID (gdk_fs);
                    int fw = gdk_window_get_width (gdk_fs);
                    int fh = gdk_window_get_height (gdk_fs);
                    XReparentWindow (dpy, xv, xfs, 0, 0);
                    XResizeWindow (dpy, xv, fw, fh);
                    XMapWindow (dpy, xv);
                    XFlush (dpy);
                }
            }
        } else {
            if (w->fs_cover_area) {
                gtk_widget_show (w->fs_cover_area);
                gtk_widget_queue_draw (w->fs_cover_area);
            }
        }

        schedule_cursor_hide (w);
    } else {
        /* Exit Fullscreen */
        w->is_fullscreen = FALSE;
        if (w->cursor_hide_timer_id) {
            g_source_remove (w->cursor_hide_timer_id);
            w->cursor_hide_timer_id = 0;
        }
        restore_normal_cursor (w);

        if (w->is_video) {
            if (w->video_area && gtk_widget_get_realized (w->video_area) &&
                w->event_box && gtk_widget_get_realized (w->event_box)) {
                GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
                GdkWindow *gdk_parent = gtk_widget_get_window (w->event_box);
                if (gdk_v && gdk_parent) {
                    Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                    Window xv = GDK_WINDOW_XID (gdk_v);
                    Window xp = GDK_WINDOW_XID (gdk_parent);
                    GtkAllocation alloc;
                    gtk_widget_get_allocation (w->video_area, &alloc);
                    XReparentWindow (dpy, xv, xp, alloc.x, alloc.y);
                    XResizeWindow (dpy, xv, alloc.width, alloc.height);
                    XMapWindow (dpy, xv);
                    XFlush (dpy);
                }
            }
        }

        gtk_widget_hide (w->fs_window);
        gtk_window_unfullscreen (GTK_WINDOW (w->fs_window));

        if (!w->is_video && w->cover_area) {
            gtk_widget_queue_draw (w->cover_area);
        }

        gtk_widget_queue_resize (w->container);
    }

    return G_SOURCE_REMOVE;
}

static void
toggle_fullscreen (w_video_t *w) {
    if (!w || w->toggle_pending) {
        return;
    }
    w->toggle_pending = TRUE;
    g_idle_add (do_toggle_fullscreen_idle, w);
}

/* ---------------- Event Handlers ---------------- */

static gboolean
on_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (event->type == GDK_2BUTTON_PRESS && event->button == 1) {
        /* Double click on video or cover -> toggle fullscreen */
        toggle_fullscreen (w);
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_key_press_event (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (event->keyval == GDK_KEY_Escape && w->is_fullscreen) {
        toggle_fullscreen (w);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_F11 || event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F) {
        toggle_fullscreen (w);
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_motion_notify_event (GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w->is_fullscreen) {
        schedule_cursor_hide (w);
    }
    return FALSE;
}

/* ---------------- CoverArt Rendering ---------------- */

static gboolean
on_cover_area_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation (widget, &alloc);

    /* Background: Deep dark gradient / solid dark */
    cairo_set_source_rgb (cr, 0.07, 0.07, 0.07);
    cairo_paint (cr);

    if (w->cover_pixbuf) {
        int pw = gdk_pixbuf_get_width (w->cover_pixbuf);
        int ph = gdk_pixbuf_get_height (w->cover_pixbuf);
        if (pw > 0 && ph > 0 && alloc.width > 20 && alloc.height > 20) {
            double scale_x = (double)(alloc.width - 24) / pw;
            double scale_y = (double)(alloc.height - 24) / ph;
            double scale = fmin (scale_x, scale_y);
            if (scale > 1.2) {
                scale = 1.2;
            }

            int dw = (int)(pw * scale);
            int dh = (int)(ph * scale);
            int dx = (alloc.width - dw) / 2;
            int dy = (alloc.height - dh) / 2;

            GdkPixbuf *scaled = gdk_pixbuf_scale_simple (w->cover_pixbuf, dw, dh, GDK_INTERP_BILINEAR);
            if (scaled) {
                /* Drop shadow behind cover */
                cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.5);
                cairo_rectangle (cr, dx + 4, dy + 4, dw, dh);
                cairo_fill (cr);

                /* Cover image */
                gdk_cairo_set_source_pixbuf (cr, scaled, dx, dy);
                cairo_paint (cr);
                g_object_unref (scaled);
            }
        }
    } else {
        /* No Cover Art placeholder */
        cairo_set_source_rgb (cr, 0.35, 0.35, 0.35);
        cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size (cr, 18.0);
        cairo_text_extents_t extents;
        const char *txt = (w->current_title && w->current_title[0]) ? w->current_title : "DeaDBeeF Audio Player";
        cairo_text_extents (cr, txt, &extents);
        cairo_move_to (cr, (alloc.width - extents.width) / 2.0, (alloc.height / 2.0));
        cairo_show_text (cr, txt);

        if (w->current_artist && w->current_artist[0]) {
            cairo_set_source_rgb (cr, 0.25, 0.25, 0.25);
            cairo_set_font_size (cr, 14.0);
            cairo_text_extents_t a_ext;
            cairo_text_extents (cr, w->current_artist, &a_ext);
            cairo_move_to (cr, (alloc.width - a_ext.width) / 2.0, (alloc.height / 2.0) + 26.0);
            cairo_show_text (cr, w->current_artist);
        }
    }
    return FALSE;
}

/* ---------------- Artwork Callback Handling ---------------- */

typedef struct {
    w_video_t *w;
    char *image_path;
} cover_load_data_t;

static gboolean
on_cover_loaded_main_thread (gpointer user_data) {
    cover_load_data_t *data = (cover_load_data_t *)user_data;
    if (data && data->w) {
        if (data->w->cover_pixbuf) {
            g_object_unref (data->w->cover_pixbuf);
            data->w->cover_pixbuf = NULL;
        }
        if (data->image_path) {
            data->w->cover_pixbuf = gdk_pixbuf_new_from_file (data->image_path, NULL);
            free (data->image_path);
        }
        if (data->w->cover_area) {
            gtk_widget_queue_draw (data->w->cover_area);
        }
        if (data->w->fs_cover_area) {
            gtk_widget_queue_draw (data->w->fs_cover_area);
        }
    }
    free (data);
    return G_SOURCE_REMOVE;
}

static void
on_cover_query_callback (int error, ddb_cover_query_t *query, ddb_cover_info_t *cover) {
    if (!query) {
        return;
    }
    w_video_t *w = (w_video_t *)query->user_data;
    if (!(query->flags & DDB_ARTWORK_FLAG_CANCELLED) && w) {
        cover_load_data_t *data = malloc (sizeof (cover_load_data_t));
        data->w = w;
        if (!error && cover && cover->cover_found && cover->image_filename) {
            data->image_path = strdup (cover->image_filename);
        } else {
            data->image_path = NULL;
        }
        g_idle_add (on_cover_loaded_main_thread, data);
    }

    if (cover) {
        ddb_artwork_plugin_t *art = (ddb_artwork_plugin_t *)deadbeef->plug_get_for_id ("artwork2");
        if (art && art->cover_info_release) {
            art->cover_info_release (cover);
        }
    }
    if (query->track) {
        deadbeef->pl_item_unref (query->track);
    }
    free (query);
}

static void
fetch_cover_for_track (w_video_t *w, DB_playItem_t *track) {
    if (!w || !track) {
        return;
    }
    if (!w->artwork_plugin) {
        w->artwork_plugin = (ddb_artwork_plugin_t *)deadbeef->plug_get_for_id ("artwork2");
    }
    if (!w->artwork_plugin || !w->artwork_plugin->cover_get) {
        return;
    }

    deadbeef->pl_item_ref (track);
    ddb_cover_query_t *q = calloc (1, sizeof (ddb_cover_query_t));
    q->_size = sizeof (ddb_cover_query_t);
    q->track = (ddb_playItem_t *)track;
    q->source_id = w->artwork_source_id;
    q->user_data = w;
    w->artwork_plugin->cover_get (q, on_cover_query_callback);
}

/* ---------------- A/V Synchronization Timer ---------------- */

static gboolean
av_sync_timer_cb (gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w || !w->mpv || !w->is_video) {
        return G_SOURCE_CONTINUE;
    }

    /* Check DeaDBeeF playback position */
    float db_pos = deadbeef->streamer_get_playpos ();
    if (db_pos < 0) {
        return G_SOURCE_CONTINUE;
    }

    double mpv_pos = 0;
    if (mpv_get_property (w->mpv, "time-pos", MPV_FORMAT_DOUBLE, &mpv_pos) >= 0) {
        double diff = fabs ((double)db_pos - mpv_pos);
        /* If drift exceeds 0.35 seconds, perform a soft seek to keep video and audio in lockstep */
        if (diff > 0.35 && db_pos > 0.5) {
            char pos_str[32];
            snprintf (pos_str, sizeof (pos_str), "%f", db_pos);
            const char *cmd[] = { "seek", pos_str, "absolute+exact", NULL };
            mpv_command_async (w->mpv, 0, cmd);
        }
    }

    return G_SOURCE_CONTINUE;
}

/* ---------------- Track Change & Playback Control ---------------- */

static void
update_playing_state (w_video_t *w) {
    if (!w) {
        return;
    }

    DB_playItem_t *track = deadbeef->streamer_get_playing_track ();
    if (!track) {
        /* Stopped */
        if (w->mpv) {
            const char *cmd[] = { "stop", NULL };
            mpv_command_async (w->mpv, 0, cmd);
        }
        w->is_video = FALSE;
        gtk_widget_hide (w->video_area);
        gtk_widget_show (w->cover_area);
        if (w->cover_pixbuf) {
            g_object_unref (w->cover_pixbuf);
            w->cover_pixbuf = NULL;
        }
        gtk_widget_queue_draw (w->cover_area);
        if (w->is_fullscreen) {
            if (w->video_area && gtk_widget_get_realized (w->video_area)) {
                GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
                if (gdk_v) {
                    Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                    Window xv = GDK_WINDOW_XID (gdk_v);
                    XUnmapWindow (dpy, xv);
                    XFlush (dpy);
                }
            }
            if (w->fs_cover_area) {
                gtk_widget_show (w->fs_cover_area);
                gtk_widget_queue_draw (w->fs_cover_area);
            }
        }
        return;
    }

    const char *uri = deadbeef->pl_find_meta (track, ":URI");
    const char *title = deadbeef->pl_find_meta (track, "title");
    const char *artist = deadbeef->pl_find_meta (track, "artist");

    if (w->current_title) free (w->current_title);
    if (w->current_artist) free (w->current_artist);
    w->current_title = title ? strdup (title) : NULL;
    w->current_artist = artist ? strdup (artist) : NULL;

    gboolean video = is_video_file (uri);
    w->is_video = video;

    if (video) {
        /* MP4 / Video Mode */
        gtk_widget_hide (w->cover_area);
        gtk_widget_show (w->video_area);

        if (w->is_fullscreen) {
            if (w->fs_cover_area) {
                gtk_widget_hide (w->fs_cover_area);
            }
            if (w->video_area && gtk_widget_get_realized (w->video_area)) {
                GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
                GdkWindow *gdk_fs = gtk_widget_get_window (w->fs_window);
                if (gdk_v && gdk_fs) {
                    Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                    Window xv = GDK_WINDOW_XID (gdk_v);
                    Window xfs = GDK_WINDOW_XID (gdk_fs);
                    int fw = gdk_window_get_width (gdk_fs);
                    int fh = gdk_window_get_height (gdk_fs);
                    XReparentWindow (dpy, xv, xfs, 0, 0);
                    XResizeWindow (dpy, xv, fw, fh);
                    XMapWindow (dpy, xv);
                    XFlush (dpy);
                }
            }
        }

        if (w->mpv && uri) {
            const char *cmd[] = { "loadfile", uri, "replace", NULL };
            setlocale (LC_NUMERIC, "C");
            mpv_command_async (w->mpv, 0, cmd);
            mpv_set_property_string (w->mpv, "pause", "no");

            float pos = deadbeef->streamer_get_playpos ();
            if (pos > 0.1f) {
                char pos_str[32];
                snprintf (pos_str, sizeof (pos_str), "%f", pos);
                const char *seek_cmd[] = { "seek", pos_str, "absolute+exact", NULL };
                setlocale (LC_NUMERIC, "C");
                mpv_command_async (w->mpv, 0, seek_cmd);
            }
        }
    } else {
        /* Audio Mode -> Show CoverArt */
        if (w->mpv) {
            const char *cmd[] = { "stop", NULL };
            setlocale (LC_NUMERIC, "C");
            mpv_command_async (w->mpv, 0, cmd);
        }
        gtk_widget_hide (w->video_area);
        gtk_widget_show (w->cover_area);

        if (w->is_fullscreen) {
            if (w->video_area && gtk_widget_get_realized (w->video_area)) {
                GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
                if (gdk_v) {
                    Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                    Window xv = GDK_WINDOW_XID (gdk_v);
                    XUnmapWindow (dpy, xv);
                    XFlush (dpy);
                }
            }
            if (w->fs_cover_area) {
                gtk_widget_show (w->fs_cover_area);
                gtk_widget_queue_draw (w->fs_cover_area);
            }
        }

        fetch_cover_for_track (w, track);
    }

    deadbeef->pl_item_unref (track);
}

static gboolean
on_fs_window_delete_event (GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && w->is_fullscreen) {
        toggle_fullscreen (w);
    }
    return TRUE;
}

static void
on_video_area_realize (GtkWidget *widget, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w || !w->mpv) {
        return;
    }

    GdkWindow *gdk_win = gtk_widget_get_window (widget);
    if (gdk_win) {
        int64_t wid = (int64_t)GDK_WINDOW_XID (gdk_win);
        setlocale (LC_NUMERIC, "C");
        if (!w->mpv_inited) {
            mpv_set_option (w->mpv, "wid", MPV_FORMAT_INT64, &wid);
            if (mpv_initialize (w->mpv) >= 0) {
                w->mpv_inited = TRUE;
            }
        } else {
            mpv_set_option (w->mpv, "wid", MPV_FORMAT_INT64, &wid);
        }
    }
}

/* ---------------- Widget Lifecycle ---------------- */

static void
w_video_destroy (ddb_gtkui_widget_t *base) {
    w_video_t *w = (w_video_t *)base;
    if (!w) {
        return;
    }

    active_widgets = g_list_remove (active_widgets, w);

    if (w->sync_timer_id) {
        g_source_remove (w->sync_timer_id);
        w->sync_timer_id = 0;
    }
    if (w->cursor_hide_timer_id) {
        g_source_remove (w->cursor_hide_timer_id);
        w->cursor_hide_timer_id = 0;
    }

    if (w->artwork_plugin && w->artwork_source_id) {
        w->artwork_plugin->cancel_queries_with_source_id (w->artwork_source_id);
    }

    if (w->mpv) {
        setlocale (LC_NUMERIC, "C");
        mpv_destroy (w->mpv);
        w->mpv = NULL;
    }

    if (w->cover_pixbuf) {
        g_object_unref (w->cover_pixbuf);
        w->cover_pixbuf = NULL;
    }

    if (w->current_title) free (w->current_title);
    if (w->current_artist) free (w->current_artist);

    if (w->fs_window) {
        if (w->is_fullscreen && w->video_area && gtk_widget_get_realized (w->video_area) &&
            w->event_box && gtk_widget_get_realized (w->event_box)) {
            GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
            GdkWindow *gdk_parent = gtk_widget_get_window (w->event_box);
            if (gdk_v && gdk_parent) {
                Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                Window xv = GDK_WINDOW_XID (gdk_v);
                Window xp = GDK_WINDOW_XID (gdk_parent);
                XReparentWindow (dpy, xv, xp, 0, 0);
                XFlush (dpy);
            }
        }
        gtk_widget_destroy (w->fs_window);
        w->fs_window = NULL;
    }

    free (w);
}

static int
w_video_message (ddb_gtkui_widget_t *base, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    w_video_t *w = (w_video_t *)base;
    if (!w) {
        return 0;
    }

    switch (id) {
    case DB_EV_SONGSTARTED:
    case DB_EV_SONGCHANGED:
    case DB_EV_STOP:
        update_playing_state (w);
        break;

    case DB_EV_PAUSED:
        if (w->mpv && w->is_video) {
            setlocale (LC_NUMERIC, "C");
            mpv_set_property_string (w->mpv, "pause", p1 ? "yes" : "no");
        }
        break;

    case DB_EV_SEEKED:
    case DB_EV_SEEK:
        if (w->mpv && w->is_video) {
            float pos = deadbeef->streamer_get_playpos ();
            if (pos >= 0) {
                char pos_str[32];
                snprintf (pos_str, sizeof (pos_str), "%f", pos);
                const char *cmd[] = { "seek", pos_str, "absolute+exact", NULL };
                setlocale (LC_NUMERIC, "C");
                mpv_command_async (w->mpv, 0, cmd);
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

static ddb_gtkui_widget_t *
w_video_create (void) {
    w_video_t *w = calloc (1, sizeof (w_video_t));
    if (!w) {
        return NULL;
    }

    w->base.type = "video_player";
    w->base.destroy = w_video_destroy;
    w->base.message = w_video_message;

    /* Container: main vertical box */
    w->container = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    w->base.widget = w->container;

    /* Event box to capture double clicks and keystrokes */
    w->event_box = gtk_event_box_new ();
    gtk_widget_set_can_focus (w->event_box, TRUE);
    gtk_widget_add_events (w->event_box,
                           GDK_BUTTON_PRESS_MASK |
                           GDK_BUTTON_RELEASE_MASK |
                           GDK_POINTER_MOTION_MASK |
                           GDK_KEY_PRESS_MASK);

    g_signal_connect (w->event_box, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->event_box, "key-press-event", G_CALLBACK (on_key_press_event), w);
    g_signal_connect (w->event_box, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);

    /* Content box */
    w->content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (w->event_box), w->content_box);
    gtk_box_pack_start (GTK_BOX (w->container), w->event_box, TRUE, TRUE, 0);

    /* Video Drawing Area */
    w->video_area = gtk_drawing_area_new ();
    gtk_widget_set_size_request (w->video_area, 320, 180);
    gtk_widget_add_events (w->video_area, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->video_area, "realize", G_CALLBACK (on_video_area_realize), w);
    g_signal_connect (w->video_area, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->video_area, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    gtk_box_pack_start (GTK_BOX (w->content_box), w->video_area, TRUE, TRUE, 0);

    /* CoverArt Drawing Area */
    w->cover_area = gtk_drawing_area_new ();
    gtk_widget_set_size_request (w->cover_area, 320, 180);
    gtk_widget_add_events (w->cover_area, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->cover_area, "draw", G_CALLBACK (on_cover_area_draw), w);
    g_signal_connect (w->cover_area, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->cover_area, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    gtk_box_pack_start (GTK_BOX (w->content_box), w->cover_area, TRUE, TRUE, 0);

    /* Fullscreen dedicated window */
    w->fs_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title (GTK_WINDOW (w->fs_window), "DeaDBeeF Video");
    gtk_window_set_decorated (GTK_WINDOW (w->fs_window), FALSE);
    gtk_widget_realize (w->fs_window);
    gtk_widget_add_events (w->fs_window, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->fs_window, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->fs_window, "key-press-event", G_CALLBACK (on_key_press_event), w);
    g_signal_connect (w->fs_window, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    g_signal_connect (w->fs_window, "delete-event", G_CALLBACK (on_fs_window_delete_event), w);
    g_signal_connect (w->fs_window, "configure-event", G_CALLBACK (on_fs_window_configure), w);

    /* Fullscreen CoverArt Drawing Area */
    w->fs_cover_area = gtk_drawing_area_new ();
    gtk_widget_add_events (w->fs_cover_area, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->fs_cover_area, "draw", G_CALLBACK (on_cover_area_draw), w);
    g_signal_connect (w->fs_cover_area, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->fs_cover_area, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    gtk_container_add (GTK_CONTAINER (w->fs_window), w->fs_cover_area);

    /* Initialize libmpv instance with C numeric locale */
    setlocale (LC_NUMERIC, "C");
    w->mpv = mpv_create ();
    if (w->mpv) {
        /* Rendering backend: GPU */
        mpv_set_option_string (w->mpv, "vo", "gpu");
        mpv_set_option_string (w->mpv, "gpu-context", "x11egl,x11");

        /* Hardware decoding: Prioritize Nvidia NVDEC */
        mpv_set_option_string (w->mpv, "hwdec", "nvdec,nvdec-copy,auto");

        /* Audio disabled to avoid duplicate sound with DeaDBeeF audio engine */
        mpv_set_option_string (w->mpv, "aid", "no");
        mpv_set_option_string (w->mpv, "audio", "no");

        /* Responsive UI options */
        mpv_set_option_string (w->mpv, "keep-open", "yes");
        mpv_set_option_string (w->mpv, "force-window", "yes");
        mpv_set_option_string (w->mpv, "input-default-bindings", "no");
        mpv_set_option_string (w->mpv, "input-vo-keyboard", "no");
        mpv_set_option_string (w->mpv, "input-cursor", "no");
    }

    /* Artwork source ID */
    w->artwork_plugin = (ddb_artwork_plugin_t *)deadbeef->plug_get_for_id ("artwork2");
    if (w->artwork_plugin && w->artwork_plugin->allocate_source_id) {
        w->artwork_source_id = w->artwork_plugin->allocate_source_id ();
    }

    /* Start periodic A/V sync timer (500ms) */
    w->sync_timer_id = g_timeout_add (500, av_sync_timer_cb, w);

    /* Default view: show cover area initially */
    gtk_widget_show (w->cover_area);
    gtk_widget_hide (w->video_area);
    gtk_widget_show_all (w->container);

    /* Register to global widget list */
    active_widgets = g_list_append (active_widgets, w);

    /* Check current playing track */
    update_playing_state (w);

    return &w->base;
}

/* ---------------- Plugin Message Dispatcher ---------------- */

static int
video_plugin_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_SONGSTARTED:
    case DB_EV_SONGCHANGED:
    case DB_EV_STOP:
        for (GList *l = active_widgets; l; l = l->next) {
            update_playing_state ((w_video_t *)l->data);
        }
        break;

    case DB_EV_PAUSED:
        for (GList *l = active_widgets; l; l = l->next) {
            w_video_t *w = (w_video_t *)l->data;
            if (w->mpv && w->is_video) {
                mpv_set_property_string (w->mpv, "pause", p1 ? "yes" : "no");
            }
        }
        break;

    case DB_EV_SEEKED:
    case DB_EV_SEEK:
        for (GList *l = active_widgets; l; l = l->next) {
            w_video_t *w = (w_video_t *)l->data;
            if (w->mpv && w->is_video) {
                float pos = deadbeef->streamer_get_playpos ();
                if (pos >= 0) {
                    char pos_str[32];
                    snprintf (pos_str, sizeof (pos_str), "%f", pos);
                    const char *cmd[] = { "seek", pos_str, "absolute+exact", NULL };
                    mpv_command_async (w->mpv, 0, cmd);
                }
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ---------------- Plugin Entry Point & Registration ---------------- */

static int
video_connect (void) {
    setlocale (LC_NUMERIC, "C");
    gtkui_plugin = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (!gtkui_plugin) {
        return -1;
    }
    gtkui_plugin->w_reg_widget (_("Video / Cover Player"), 0, w_video_create, "video_player", NULL);
    return 0;
}

static int
video_disconnect (void) {
    if (gtkui_plugin) {
        gtkui_plugin->w_unreg_widget ("video_player");
        gtkui_plugin = NULL;
    }
    return 0;
}

static DB_misc_t plugin = {
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_MISC,
    .plugin.id = "video_gtk3",
    .plugin.name = "Video & Cover Player",
    .plugin.descr = "MP4 video playback with Nvidia NVDEC hardware acceleration and CoverArt support (GTK3 / libmpv)",
    .plugin.copyright =
        "DeaDBeeF Video Plugin\n"
        "Copyright (C) 2026\n"
        "GPLv2 Licensed\n",
    .plugin.website = "https://github.com/sophAi/deadbeef-coverflow",
    .plugin.connect = video_connect,
    .plugin.disconnect = video_disconnect,
    .plugin.message = video_plugin_message,
};

DB_plugin_t *
video_gtk3_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
