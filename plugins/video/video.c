/*
 * DeaDBeeF Video & CoverArt GTK3 Widget Plugin
 *
 * Plays MP4 and video files with Nvidia NVDEC hardware acceleration via libmpv.
 * Displays high-quality CoverArt when playing non-video music files.
 * Features Windowed mode with draggable, resizable window and playback controls,
 * as well as smooth Fullscreen toggle via double-click, F11, or F key.
 *
 * Fullscreen mode automatically hides the control bar and only reveals it when
 * the mouse hovers near the bottom of the screen.
 * Left/Right keys control playback progress, Up/Down keys control volume.
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

    /* Embedded widgets in DeaDBeeF main window */
    GtkWidget *container;        /* Outer GtkBox */
    GtkWidget *event_box;        /* EventBox capturing clicks & key presses */
    GtkWidget *content_box;      /* Stacked layout */
    GtkWidget *video_area;       /* GtkDrawingArea for MPV X11 embedding */
    GtkWidget *cover_area;       /* GtkDrawingArea for CoverArt rendering */

    /* Detached / Windowed / Fullscreen Window */
    GtkWidget *det_window;       /* Standalone GtkWindow */
    GtkWidget *det_main_box;     /* Vertical box inside det_window */
    GtkWidget *det_display_box;  /* EventBox hosting active video or cover art */
    GtkWidget *det_cover_area;   /* DrawingArea for CoverArt in detached window */
    GtkWidget *control_bar;      /* Bottom playback control bar */

    /* Control bar widgets */
    GtkWidget *btn_play;         /* Play / Pause button */
    GtkWidget *btn_stop;         /* Stop button */
    GtkWidget *btn_prev;         /* Previous track */
    GtkWidget *btn_next;         /* Next track */
    GtkWidget *lbl_time_cur;     /* Current playback time label */
    GtkWidget *seek_scale;       /* Progress seek slider */
    GtkWidget *lbl_time_total;   /* Total duration label */
    GtkWidget *vol_btn;          /* Volume button */
    GtkWidget *btn_fs_toggle;    /* Windowed / Fullscreen toggle */
    GtkWidget *btn_dock;         /* Return / Dock back to main window */

    /* Player state */
    mpv_handle *mpv;
    gboolean mpv_inited;
    gboolean is_video;
    gboolean is_detached;        /* TRUE if popped out into windowed or fullscreen */
    gboolean is_fullscreen;      /* TRUE if in fullscreen mode */
    gboolean is_seeking;         /* TRUE while user is actively dragging seek slider */
    gboolean updating_scale;     /* Guard against recursive scale events */
    gboolean updating_volume;    /* Guard against recursive volume events */

    /* Mouse dragging in windowed mode */
    gboolean is_mouse_down;
    double mouse_down_x;
    double mouse_down_y;

    /* Metadata & Cover */
    GdkPixbuf *cover_pixbuf;
    char *current_uri;
    char *current_title;
    char *current_artist;

    ddb_artwork_plugin_t *artwork_plugin;
    int64_t artwork_source_id;

    /* Timers */
    guint sync_timer_id;         /* Periodic A/V sync & progress timer (200ms) */
    guint hide_timer_id;         /* Fullscreen auto-hide timer for controls & cursor */
    gboolean cursor_hidden;
};

/* Keep a thread-safe global list of active widget instances */
static GList *active_widgets = NULL;
static GMutex widgets_mutex;

/* Message dispatch structure for main thread execution */
typedef struct {
    w_video_t *w;
    uint32_t id;
    uint32_t p1;
    uint32_t p2;
} plugin_msg_event_t;

/* ---------------- Helper: Time Formatting ---------------- */

static void
format_time (float sec, char *buf, size_t buf_size) {
    if (sec < 0.0f) {
        snprintf (buf, buf_size, "00:00");
        return;
    }
    int total_sec = (int)sec;
    int h = total_sec / 3600;
    int m = (total_sec % 3600) / 60;
    int s = total_sec % 60;
    if (h > 0) {
        snprintf (buf, buf_size, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf (buf, buf_size, "%02d:%02d", m, s);
    }
}

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

/* ---------------- CSS Styling ---------------- */

static void
apply_custom_css (void) {
    static gboolean css_applied = FALSE;
    if (css_applied) {
        return;
    }
    css_applied = TRUE;

    GtkCssProvider *provider = gtk_css_provider_new ();
    const char *css =
        ".video-control-bar {"
        "  background: rgba(18, 18, 18, 0.95);"
        "  border-top: 1px solid #333333;"
        "  padding: 6px 14px;"
        "}"
        ".video-control-bar button {"
        "  background: #282828;"
        "  color: #f0f0f0;"
        "  border: 1px solid #444444;"
        "  border-radius: 6px;"
        "  padding: 4px 10px;"
        "  font-size: 20px;"
        "  font-weight: bold;"
        "  min-height: 40px;"
        "  min-width: 48px;"
        "}"
        ".video-control-bar button:hover {"
        "  background: #404040;"
        "  color: #ffffff;"
        "  border-color: #666666;"
        "}"
        ".video-control-bar button.btn-play {"
        "  font-size: 24px;"
        "  min-width: 58px;"
        "  background: #2a3c54;"
        "  border-color: #43648c;"
        "}"
        ".video-control-bar button.btn-play:hover {"
        "  background: #395273;"
        "  border-color: #5c84b8;"
        "}"
        ".video-control-bar button image {"
        "  min-width: 22px;"
        "  min-height: 22px;"
        "}"
        ".video-control-bar label {"
        "  color: #d8d8d8;"
        "  font-family: Monospace, sans-serif;"
        "  font-size: 13px;"
        "  font-weight: bold;"
        "  padding: 0 4px;"
        "}"
        ".video-control-bar scale {"
        "  margin: 0 8px;"
        "}"
        ".video-control-bar scale trough {"
        "  min-height: 6px;"
        "  background: #383838;"
        "  border-radius: 3px;"
        "}"
        ".video-control-bar scale highlight {"
        "  background: #3584e4;"
        "  border-radius: 3px;"
        "}"
        ".video-control-bar scale slider {"
        "  min-height: 16px;"
        "  min-width: 16px;"
        "  margin: -5px 0;"
        "  border-radius: 8px;"
        "  background: #ffffff;"
        "}";

    gtk_css_provider_load_from_data (provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen (
        gdk_screen_get_default (),
        GTK_STYLE_PROVIDER (provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (provider);
}

/* ---------------- Window Title & Playback State ---------------- */

static void
update_window_title (w_video_t *w) {
    if (!w || !w->det_window) {
        return;
    }
    char title_buf[512];
    if (w->current_artist && w->current_artist[0] && w->current_title && w->current_title[0]) {
        snprintf (title_buf, sizeof (title_buf), "%s - %s — DeaDBeeF 影音播放器", w->current_artist, w->current_title);
    } else if (w->current_title && w->current_title[0]) {
        snprintf (title_buf, sizeof (title_buf), "%s — DeaDBeeF 影音播放器", w->current_title);
    } else {
        snprintf (title_buf, sizeof (title_buf), "DeaDBeeF 影音播放器");
    }
    gtk_window_set_title (GTK_WINDOW (w->det_window), title_buf);
}

static void
update_play_button_state (w_video_t *w) {
    if (!w || !w->btn_play) {
        return;
    }
    int state = DDB_PLAYBACK_STATE_STOPPED;
    if (deadbeef->get_output ()) {
        state = deadbeef->get_output ()->state ();
    }

    if (state == DDB_PLAYBACK_STATE_PLAYING) {
        gtk_button_set_label (GTK_BUTTON (w->btn_play), "❚❚");
        gtk_widget_set_tooltip_text (w->btn_play, "暫停 (Space)");
    } else {
        gtk_button_set_label (GTK_BUTTON (w->btn_play), "▶");
        gtk_widget_set_tooltip_text (w->btn_play, "播放 (Space)");
    }
}

static void
update_volume_state (w_video_t *w) {
    if (w && w->vol_btn && !w->updating_volume) {
        w->updating_volume = TRUE;
        float amp = deadbeef->volume_get_amp ();
        gtk_scale_button_set_value (GTK_SCALE_BUTTON (w->vol_btn), (double)amp);
        w->updating_volume = FALSE;
    }
}

/* ---------------- Fullscreen & Cursor Management ---------------- */

static void
restore_normal_cursor (w_video_t *w) {
    if (!w || !w->cursor_hidden) {
        return;
    }
    GdkWindow *gdk_win = NULL;
    if (w->det_window && gtk_widget_get_visible (w->det_window)) {
        gdk_win = gtk_widget_get_window (w->det_window);
    } else if (w->video_area && gtk_widget_get_realized (w->video_area)) {
        gdk_win = gtk_widget_get_window (w->video_area);
    }
    if (gdk_win) {
        gdk_window_set_cursor (gdk_win, NULL);
    }
    w->cursor_hidden = FALSE;
}

static gboolean
hide_controls_cb (gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w || !w->is_fullscreen) {
        return G_SOURCE_REMOVE;
    }

    /* Do not hide if user is currently seeking */
    if (w->is_seeking) {
        return G_SOURCE_CONTINUE;
    }

    /* Hide cursor */
    GdkWindow *gdk_win = gtk_widget_get_window (w->det_window);
    if (gdk_win) {
        GdkDisplay *disp = gdk_window_get_display (gdk_win);
        GdkCursor *blank = gdk_cursor_new_for_display (disp, GDK_BLANK_CURSOR);
        gdk_window_set_cursor (gdk_win, blank);
        g_object_unref (blank);
        w->cursor_hidden = TRUE;
    }

    /* Hide control bar in fullscreen */
    if (w->control_bar) {
        gtk_widget_hide (w->control_bar);
    }

    w->hide_timer_id = 0;
    return G_SOURCE_REMOVE;
}

/* ---------------- Size Allocation & X11 Sync ---------------- */

static void
sync_video_x11_size (w_video_t *w, int width, int height) {
    if (!w || !w->video_area || !gtk_widget_get_realized (w->video_area)) {
        return;
    }
    GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
    if (gdk_v) {
        Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
        Window xv = GDK_WINDOW_XID (gdk_v);
        XResizeWindow (dpy, xv, width > 1 ? width : 1, height > 1 ? height : 1);
        XFlush (dpy);
    }
}

static void
on_det_display_box_size_allocate (GtkWidget *widget, GtkAllocation *allocation, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && w->is_detached && w->is_video) {
        sync_video_x11_size (w, allocation->width, allocation->height);
    }
}

/* ---------------- Mode Switching: Windowed / Fullscreen / Dock ---------------- */

static void enter_windowed_mode (w_video_t *w);
static void enter_fullscreen_mode (w_video_t *w);
static void dock_to_embedded (w_video_t *w);

static void
toggle_fullscreen_or_windowed (w_video_t *w) {
    if (!w) {
        return;
    }
    if (w->is_fullscreen) {
        enter_windowed_mode (w);
    } else {
        enter_fullscreen_mode (w);
    }
}

static void
enter_windowed_mode (w_video_t *w) {
    if (!w) {
        return;
    }

    w->is_detached = TRUE;
    w->is_fullscreen = FALSE;

    if (w->hide_timer_id) {
        g_source_remove (w->hide_timer_id);
        w->hide_timer_id = 0;
    }
    restore_normal_cursor (w);

    gtk_window_unfullscreen (GTK_WINDOW (w->det_window));
    gtk_window_set_decorated (GTK_WINDOW (w->det_window), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (w->det_window), TRUE);
    update_window_title (w);

    if (w->btn_fs_toggle) {
        gtk_button_set_label (GTK_BUTTON (w->btn_fs_toggle), "⛶");
        gtk_widget_set_tooltip_text (w->btn_fs_toggle, "全螢幕 (F11 或 雙擊)");
    }

    /* In Windowed mode, the control bar is always visible */
    if (w->control_bar) {
        gtk_widget_show (w->control_bar);
    }

    gtk_widget_show_all (w->det_window);

    if (w->is_video) {
        if (w->det_cover_area) {
            gtk_widget_hide (w->det_cover_area);
        }
        if (w->video_area && gtk_widget_get_realized (w->video_area) &&
            w->det_display_box && gtk_widget_get_realized (w->det_display_box)) {
            GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
            GdkWindow *gdk_det = gtk_widget_get_window (w->det_display_box);
            if (gdk_v && gdk_det) {
                Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                Window xv = GDK_WINDOW_XID (gdk_v);
                Window xdet = GDK_WINDOW_XID (gdk_det);
                GtkAllocation alloc;
                gtk_widget_get_allocation (w->det_display_box, &alloc);
                int w_px = alloc.width > 10 ? alloc.width : 960;
                int h_px = alloc.height > 10 ? alloc.height : 500;
                XReparentWindow (dpy, xv, xdet, 0, 0);
                XResizeWindow (dpy, xv, w_px, h_px);
                XMapWindow (dpy, xv);
                XFlush (dpy);
            }
        }
    } else {
        if (w->video_area && gtk_widget_get_realized (w->video_area)) {
            GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
            if (gdk_v) {
                Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                Window xv = GDK_WINDOW_XID (gdk_v);
                XUnmapWindow (dpy, xv);
                XFlush (dpy);
            }
        }
        if (w->det_cover_area) {
            gtk_widget_show (w->det_cover_area);
            gtk_widget_queue_draw (w->det_cover_area);
        }
    }
}

static void
enter_fullscreen_mode (w_video_t *w) {
    if (!w) {
        return;
    }

    w->is_detached = TRUE;
    w->is_fullscreen = TRUE;

    gtk_window_set_decorated (GTK_WINDOW (w->det_window), FALSE);
    gtk_window_fullscreen (GTK_WINDOW (w->det_window));

    if (w->btn_fs_toggle) {
        gtk_button_set_label (GTK_BUTTON (w->btn_fs_toggle), "❐");
        gtk_widget_set_tooltip_text (w->btn_fs_toggle, "視窗化 (F11, Esc 或 雙擊)");
    }

    gtk_widget_show_all (w->det_window);

    /* In Fullscreen mode, control bar is auto-hidden by default */
    if (w->control_bar) {
        gtk_widget_hide (w->control_bar);
    }

    if (w->is_video) {
        if (w->det_cover_area) {
            gtk_widget_hide (w->det_cover_area);
        }
        if (w->video_area && gtk_widget_get_realized (w->video_area) &&
            w->det_display_box && gtk_widget_get_realized (w->det_display_box)) {
            GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
            GdkWindow *gdk_det = gtk_widget_get_window (w->det_display_box);
            if (gdk_v && gdk_det) {
                Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                Window xv = GDK_WINDOW_XID (gdk_v);
                Window xdet = GDK_WINDOW_XID (gdk_det);
                GtkAllocation alloc;
                gtk_widget_get_allocation (w->det_display_box, &alloc);
                XReparentWindow (dpy, xv, xdet, 0, 0);
                XResizeWindow (dpy, xv, alloc.width, alloc.height);
                XMapWindow (dpy, xv);
                XFlush (dpy);
            }
        }
    } else {
        if (w->det_cover_area) {
            gtk_widget_show (w->det_cover_area);
            gtk_widget_queue_draw (w->det_cover_area);
        }
    }

    /* Start idle timer for cursor hiding */
    if (w->hide_timer_id) {
        g_source_remove (w->hide_timer_id);
    }
    w->hide_timer_id = g_timeout_add (2000, hide_controls_cb, w);
}

static void
dock_to_embedded (w_video_t *w) {
    if (!w) {
        return;
    }

    if (w->hide_timer_id) {
        g_source_remove (w->hide_timer_id);
        w->hide_timer_id = 0;
    }
    restore_normal_cursor (w);

    if (w->is_fullscreen) {
        gtk_window_unfullscreen (GTK_WINDOW (w->det_window));
    }

    w->is_detached = FALSE;
    w->is_fullscreen = FALSE;

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

    gtk_widget_hide (w->det_window);

    if (!w->is_video && w->cover_area) {
        gtk_widget_queue_draw (w->cover_area);
    }

    gtk_widget_queue_resize (w->container);
}

/* ---------------- Event Handlers ---------------- */

static gboolean
on_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w) {
        return FALSE;
    }

    if (event->type == GDK_2BUTTON_PRESS && event->button == 1) {
        /* Double click */
        if (!w->is_detached) {
            enter_windowed_mode (w);
        } else {
            toggle_fullscreen_or_windowed (w);
        }
        return TRUE;
    }

    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        /* Record mouse press for potential window dragging */
        w->is_mouse_down = TRUE;
        w->mouse_down_x = event->x_root;
        w->mouse_down_y = event->y_root;
    }

    return FALSE;
}

static gboolean
on_button_release_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && event->button == 1) {
        w->is_mouse_down = FALSE;
    }
    return FALSE;
}

static gboolean
on_key_press_event (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w) {
        return FALSE;
    }

    if (event->keyval == GDK_KEY_Escape) {
        if (w->is_fullscreen) {
            enter_windowed_mode (w);
        } else if (w->is_detached) {
            dock_to_embedded (w);
        }
        return TRUE;
    }

    if (event->keyval == GDK_KEY_F11 || event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F) {
        if (!w->is_detached) {
            enter_fullscreen_mode (w);
        } else {
            toggle_fullscreen_or_windowed (w);
        }
        return TRUE;
    }

    if (event->keyval == GDK_KEY_space) {
        deadbeef->sendmessage (DB_EV_TOGGLE_PAUSE, 0, 0, 0);
        return TRUE;
    }

    /* Left Arrow: Seek back 5 seconds */
    if (event->keyval == GDK_KEY_Left) {
        float pos = deadbeef->streamer_get_playpos ();
        float target = (pos > 5.0f) ? (pos - 5.0f) : 0.0f;
        deadbeef->sendmessage (DB_EV_SEEK, 0, (uint32_t)(target * 1000.0f), 0);
        if (w->mpv && w->is_video) {
            char pos_str[32];
            snprintf (pos_str, sizeof (pos_str), "%f", target);
            const char *cmd[] = { "seek", pos_str, "absolute+exact", NULL };
            setlocale (LC_NUMERIC, "C");
            mpv_command_async (w->mpv, 0, cmd);
        }
        return TRUE;
    }

    /* Right Arrow: Seek forward 5 seconds */
    if (event->keyval == GDK_KEY_Right) {
        float pos = deadbeef->streamer_get_playpos ();
        float target = pos + 5.0f;
        deadbeef->sendmessage (DB_EV_SEEK, 0, (uint32_t)(target * 1000.0f), 0);
        if (w->mpv && w->is_video) {
            char pos_str[32];
            snprintf (pos_str, sizeof (pos_str), "%f", target);
            const char *cmd[] = { "seek", pos_str, "absolute+exact", NULL };
            setlocale (LC_NUMERIC, "C");
            mpv_command_async (w->mpv, 0, cmd);
        }
        return TRUE;
    }

    /* Up Arrow: Increase volume by 5% */
    if (event->keyval == GDK_KEY_Up) {
        float amp = deadbeef->volume_get_amp ();
        amp = fminf (1.0f, amp + 0.05f);
        deadbeef->volume_set_amp (amp);
        update_volume_state (w);
        return TRUE;
    }

    /* Down Arrow: Decrease volume by 5% */
    if (event->keyval == GDK_KEY_Down) {
        float amp = deadbeef->volume_get_amp ();
        amp = fmaxf (0.0f, amp - 0.05f);
        deadbeef->volume_set_amp (amp);
        update_volume_state (w);
        return TRUE;
    }

    return FALSE;
}

static gboolean
on_motion_notify_event (GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w) {
        return FALSE;
    }

    if (w->is_fullscreen) {
        restore_normal_cursor (w);

        /* Calculate distance from bottom of detached window */
        int wx = 0, wy = 0, wh = 0;
        GdkWindow *gdk_win = gtk_widget_get_window (w->det_window);
        if (gdk_win) {
            gdk_window_get_origin (gdk_win, &wx, &wy);
            wh = gdk_window_get_height (gdk_win);
        }

        double dist_from_bottom = (double)(wy + wh) - event->y_root;

        /* Only show control bar if mouse is near bottom (<= 90px) */
        if (dist_from_bottom <= 90.0) {
            if (w->control_bar && !gtk_widget_get_visible (w->control_bar)) {
                gtk_widget_show (w->control_bar);
            }
            if (w->hide_timer_id) {
                g_source_remove (w->hide_timer_id);
            }
            w->hide_timer_id = g_timeout_add (2500, hide_controls_cb, w);
        } else {
            /* Mouse is not at bottom -> hide control bar unless currently seeking */
            if (!w->is_seeking && w->control_bar && gtk_widget_get_visible (w->control_bar)) {
                gtk_widget_hide (w->control_bar);
            }
            if (w->hide_timer_id) {
                g_source_remove (w->hide_timer_id);
            }
            w->hide_timer_id = g_timeout_add (1500, hide_controls_cb, w);
        }
    } else if (w->is_detached && w->is_mouse_down) {
        /* In windowed mode: allow dragging window by mouse move if dragged past threshold */
        double dx = event->x_root - w->mouse_down_x;
        double dy = event->y_root - w->mouse_down_y;
        if (fabs (dx) > 6.0 || fabs (dy) > 6.0) {
            w->is_mouse_down = FALSE;
            gtk_window_begin_move_drag (GTK_WINDOW (w->det_window),
                                        1,
                                        (int)event->x_root,
                                        (int)event->y_root,
                                        event->time);
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
on_det_window_delete_event (GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && w->is_detached) {
        dock_to_embedded (w);
    }
    return TRUE; /* Returning TRUE prevents destroying the window */
}

/* ---------------- Control Bar Button Callbacks ---------------- */

static void
on_btn_play_clicked (GtkButton *btn, gpointer user_data) {
    deadbeef->sendmessage (DB_EV_TOGGLE_PAUSE, 0, 0, 0);
}

static void
on_btn_stop_clicked (GtkButton *btn, gpointer user_data) {
    deadbeef->sendmessage (DB_EV_STOP, 0, 0, 0);
}

static void
on_btn_prev_clicked (GtkButton *btn, gpointer user_data) {
    deadbeef->sendmessage (DB_EV_PREV, 0, 0, 0);
}

static void
on_btn_next_clicked (GtkButton *btn, gpointer user_data) {
    deadbeef->sendmessage (DB_EV_NEXT, 0, 0, 0);
}

static void
on_btn_fs_toggle_clicked (GtkButton *btn, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    toggle_fullscreen_or_windowed (w);
}

static void
on_btn_dock_clicked (GtkButton *btn, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    dock_to_embedded (w);
}

static gboolean
on_seek_scale_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && event->button == 1) {
        w->is_seeking = TRUE;
    }
    return FALSE;
}

static gboolean
on_seek_scale_button_release (GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && event->button == 1 && w->is_seeking) {
        double val = gtk_range_get_value (GTK_RANGE (w->seek_scale));
        deadbeef->sendmessage (DB_EV_SEEK, 0, (uint32_t)(val * 1000.0), 0);
        if (w->mpv && w->is_video) {
            char pos_str[32];
            snprintf (pos_str, sizeof (pos_str), "%f", val);
            const char *cmd[] = { "seek", pos_str, "absolute+exact", NULL };
            setlocale (LC_NUMERIC, "C");
            mpv_command_async (w->mpv, 0, cmd);
        }
        w->is_seeking = FALSE;
    }
    return FALSE;
}

static void
on_seek_scale_value_changed (GtkRange *range, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && w->is_seeking) {
        double val = gtk_range_get_value (range);
        char buf[32];
        format_time ((float)val, buf, sizeof (buf));
        gtk_label_set_text (GTK_LABEL (w->lbl_time_cur), buf);
    }
}

static void
on_volume_value_changed (GtkScaleButton *button, gdouble value, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (w && !w->updating_volume) {
        deadbeef->volume_set_amp ((float)value);
    }
}

/* ---------------- CoverArt Rendering ---------------- */

static gboolean
on_cover_area_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation (widget, &alloc);

    /* Background: Deep dark */
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
        if (data->w->det_cover_area) {
            gtk_widget_queue_draw (data->w->det_cover_area);
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

/* ---------------- Periodic A/V Sync & Progress Timer (200ms) ---------------- */

static gboolean
periodic_timer_cb (gpointer user_data) {
    w_video_t *w = (w_video_t *)user_data;
    if (!w) {
        return G_SOURCE_CONTINUE;
    }

    float pos = deadbeef->streamer_get_playpos ();
    float dur = 0.0f;
    DB_playItem_t *track = deadbeef->streamer_get_playing_track ();
    if (track) {
        dur = deadbeef->pl_get_item_duration (track);
        deadbeef->pl_item_unref (track);
    }

    /* Update controls if detached window is active */
    if (w->is_detached) {
        update_play_button_state (w);

        if (!w->is_seeking && w->seek_scale) {
            if (dur > 0.0f) {
                w->updating_scale = TRUE;
                gtk_range_set_range (GTK_RANGE (w->seek_scale), 0.0, (double)dur);
                gtk_range_set_value (GTK_RANGE (w->seek_scale), pos >= 0.0f ? (double)pos : 0.0);
                w->updating_scale = FALSE;

                char cur_str[32], total_str[32];
                format_time (pos, cur_str, sizeof (cur_str));
                format_time (dur, total_str, sizeof (total_str));
                gtk_label_set_text (GTK_LABEL (w->lbl_time_cur), cur_str);
                gtk_label_set_text (GTK_LABEL (w->lbl_time_total), total_str);
            } else {
                w->updating_scale = TRUE;
                gtk_range_set_range (GTK_RANGE (w->seek_scale), 0.0, 100.0);
                gtk_range_set_value (GTK_RANGE (w->seek_scale), 0.0);
                w->updating_scale = FALSE;

                gtk_label_set_text (GTK_LABEL (w->lbl_time_cur), "00:00");
                gtk_label_set_text (GTK_LABEL (w->lbl_time_total), "--:--");
            }
        }
    }

    /* A/V drift correction for video playback */
    if (w->mpv && w->is_video && pos >= 0.0f) {
        double mpv_pos = 0;
        if (mpv_get_property (w->mpv, "time-pos", MPV_FORMAT_DOUBLE, &mpv_pos) >= 0) {
            double diff = fabs ((double)pos - mpv_pos);
            if (diff > 0.35 && pos > 0.5f) {
                char pos_str[32];
                snprintf (pos_str, sizeof (pos_str), "%f", pos);
                const char *cmd[] = { "seek", pos_str, "absolute+exact", NULL };
                setlocale (LC_NUMERIC, "C");
                mpv_command_async (w->mpv, 0, cmd);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

/* ---------------- Track Change & Playback Control (Main Thread) ---------------- */

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
            setlocale (LC_NUMERIC, "C");
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

        if (w->is_detached) {
            update_window_title (w);
            update_play_button_state (w);
            if (w->video_area && gtk_widget_get_realized (w->video_area)) {
                GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
                if (gdk_v) {
                    Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                    Window xv = GDK_WINDOW_XID (gdk_v);
                    XUnmapWindow (dpy, xv);
                    XFlush (dpy);
                }
            }
            if (w->det_cover_area) {
                gtk_widget_show (w->det_cover_area);
                gtk_widget_queue_draw (w->det_cover_area);
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

    update_window_title (w);
    update_play_button_state (w);

    gboolean video = is_video_file (uri);
    w->is_video = video;

    if (video) {
        /* MP4 / Video Mode */
        gtk_widget_hide (w->cover_area);
        gtk_widget_show (w->video_area);

        if (w->is_detached) {
            if (w->det_cover_area) {
                gtk_widget_hide (w->det_cover_area);
            }
            if (w->video_area && gtk_widget_get_realized (w->video_area) &&
                w->det_display_box && gtk_widget_get_realized (w->det_display_box)) {
                GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
                GdkWindow *gdk_det = gtk_widget_get_window (w->det_display_box);
                if (gdk_v && gdk_det) {
                    Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                    Window xv = GDK_WINDOW_XID (gdk_v);
                    Window xdet = GDK_WINDOW_XID (gdk_det);
                    GtkAllocation alloc;
                    gtk_widget_get_allocation (w->det_display_box, &alloc);
                    int w_px = alloc.width > 10 ? alloc.width : 960;
                    int h_px = alloc.height > 10 ? alloc.height : 500;
                    XReparentWindow (dpy, xv, xdet, 0, 0);
                    XResizeWindow (dpy, xv, w_px, h_px);
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

        if (w->is_detached) {
            if (w->video_area && gtk_widget_get_realized (w->video_area)) {
                GdkWindow *gdk_v = gtk_widget_get_window (w->video_area);
                if (gdk_v) {
                    Display *dpy = GDK_WINDOW_XDISPLAY (gdk_v);
                    Window xv = GDK_WINDOW_XID (gdk_v);
                    XUnmapWindow (dpy, xv);
                    XFlush (dpy);
                }
            }
            if (w->det_cover_area) {
                gtk_widget_show (w->det_cover_area);
                gtk_widget_queue_draw (w->det_cover_area);
            }
        }

        fetch_cover_for_track (w, track);
    }

    deadbeef->pl_item_unref (track);
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

/* ---------------- Main Thread Message Dispatcher ---------------- */

static gboolean
on_plugin_msg_idle (gpointer user_data) {
    plugin_msg_event_t *ev = (plugin_msg_event_t *)user_data;
    if (!ev) {
        return G_SOURCE_REMOVE;
    }

    g_mutex_lock (&widgets_mutex);
    gboolean found = (g_list_find (active_widgets, ev->w) != NULL);
    g_mutex_unlock (&widgets_mutex);

    if (found) {
        w_video_t *w = ev->w;
        switch (ev->id) {
        case DB_EV_SONGSTARTED:
        case DB_EV_SONGCHANGED:
        case DB_EV_STOP:
            update_playing_state (w);
            break;

        case DB_EV_PAUSED:
            if (w->mpv && w->is_video) {
                setlocale (LC_NUMERIC, "C");
                mpv_set_property_string (w->mpv, "pause", ev->p1 ? "yes" : "no");
            }
            update_play_button_state (w);
            break;

        case DB_EV_VOLUMECHANGED:
            update_volume_state (w);
            break;

        case DB_EV_SEEKED:
        case DB_EV_SEEK:
            if (w->mpv && w->is_video) {
                float pos = deadbeef->streamer_get_playpos ();
                if (pos >= 0.0f) {
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
    }

    free (ev);
    return G_SOURCE_REMOVE;
}

/* ---------------- Widget Lifecycle ---------------- */

static void
w_video_destroy (ddb_gtkui_widget_t *base) {
    w_video_t *w = (w_video_t *)base;
    if (!w) {
        return;
    }

    g_mutex_lock (&widgets_mutex);
    active_widgets = g_list_remove (active_widgets, w);
    g_mutex_unlock (&widgets_mutex);

    if (w->sync_timer_id) {
        g_source_remove (w->sync_timer_id);
        w->sync_timer_id = 0;
    }
    if (w->hide_timer_id) {
        g_source_remove (w->hide_timer_id);
        w->hide_timer_id = 0;
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

    if (w->det_window) {
        if (w->is_detached && w->video_area && gtk_widget_get_realized (w->video_area) &&
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
        gtk_widget_destroy (w->det_window);
        w->det_window = NULL;
    }

    free (w);
}

static int
w_video_message (ddb_gtkui_widget_t *base, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    /* Widget-level messages are handled safely via video_plugin_message dispatch */
    return 0;
}

static ddb_gtkui_widget_t *
w_video_create (void) {
    apply_custom_css ();

    w_video_t *w = calloc (1, sizeof (w_video_t));
    if (!w) {
        return NULL;
    }

    w->base.type = "video_player";
    w->base.destroy = w_video_destroy;
    w->base.message = w_video_message;

    /* 1. Container: main vertical box for embedded mode */
    w->container = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    w->base.widget = w->container;

    /* Event box to capture double clicks and keystrokes in embedded mode */
    w->event_box = gtk_event_box_new ();
    gtk_widget_set_can_focus (w->event_box, TRUE);
    gtk_widget_add_events (w->event_box,
                           GDK_BUTTON_PRESS_MASK |
                           GDK_BUTTON_RELEASE_MASK |
                           GDK_POINTER_MOTION_MASK |
                           GDK_KEY_PRESS_MASK);

    g_signal_connect (w->event_box, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->event_box, "button-release-event", G_CALLBACK (on_button_release_event), w);
    g_signal_connect (w->event_box, "key-press-event", G_CALLBACK (on_key_press_event), w);
    g_signal_connect (w->event_box, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);

    /* Content box inside embedded event box */
    w->content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (w->event_box), w->content_box);
    gtk_box_pack_start (GTK_BOX (w->container), w->event_box, TRUE, TRUE, 0);

    /* Video Drawing Area */
    w->video_area = gtk_drawing_area_new ();
    gtk_widget_set_size_request (w->video_area, 320, 180);
    gtk_widget_add_events (w->video_area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->video_area, "realize", G_CALLBACK (on_video_area_realize), w);
    g_signal_connect (w->video_area, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->video_area, "button-release-event", G_CALLBACK (on_button_release_event), w);
    g_signal_connect (w->video_area, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    gtk_box_pack_start (GTK_BOX (w->content_box), w->video_area, TRUE, TRUE, 0);

    /* CoverArt Drawing Area for embedded mode */
    w->cover_area = gtk_drawing_area_new ();
    gtk_widget_set_size_request (w->cover_area, 320, 180);
    gtk_widget_add_events (w->cover_area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->cover_area, "draw", G_CALLBACK (on_cover_area_draw), w);
    g_signal_connect (w->cover_area, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->cover_area, "button-release-event", G_CALLBACK (on_button_release_event), w);
    g_signal_connect (w->cover_area, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    gtk_box_pack_start (GTK_BOX (w->content_box), w->cover_area, TRUE, TRUE, 0);

    /* 2. Standalone Detached Window (Windowed & Fullscreen) */
    w->det_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title (GTK_WINDOW (w->det_window), "DeaDBeeF 影音播放器");
    gtk_window_set_default_size (GTK_WINDOW (w->det_window), 960, 540);
    gtk_window_set_decorated (GTK_WINDOW (w->det_window), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (w->det_window), TRUE);

    gtk_widget_add_events (w->det_window,
                           GDK_BUTTON_PRESS_MASK |
                           GDK_BUTTON_RELEASE_MASK |
                           GDK_KEY_PRESS_MASK |
                           GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->det_window, "key-press-event", G_CALLBACK (on_key_press_event), w);
    g_signal_connect (w->det_window, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    g_signal_connect (w->det_window, "delete-event", G_CALLBACK (on_det_window_delete_event), w);

    /* Vertical box container inside detached window */
    w->det_main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (w->det_window), w->det_main_box);

    /* Display area container (GtkEventBox) */
    w->det_display_box = gtk_event_box_new ();
    gtk_widget_add_events (w->det_display_box,
                           GDK_BUTTON_PRESS_MASK |
                           GDK_BUTTON_RELEASE_MASK |
                           GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->det_display_box, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->det_display_box, "button-release-event", G_CALLBACK (on_button_release_event), w);
    g_signal_connect (w->det_display_box, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    g_signal_connect (w->det_display_box, "size-allocate", G_CALLBACK (on_det_display_box_size_allocate), w);
    gtk_box_pack_start (GTK_BOX (w->det_main_box), w->det_display_box, TRUE, TRUE, 0);

    /* Detached CoverArt Drawing Area */
    w->det_cover_area = gtk_drawing_area_new ();
    gtk_widget_add_events (w->det_cover_area,
                           GDK_BUTTON_PRESS_MASK |
                           GDK_BUTTON_RELEASE_MASK |
                           GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->det_cover_area, "draw", G_CALLBACK (on_cover_area_draw), w);
    g_signal_connect (w->det_cover_area, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->det_cover_area, "button-release-event", G_CALLBACK (on_button_release_event), w);
    g_signal_connect (w->det_cover_area, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    gtk_container_add (GTK_CONTAINER (w->det_display_box), w->det_cover_area);

    /* 3. Control Bar at bottom of detached window */
    w->control_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_size_request (w->control_bar, -1, 54);
    gtk_widget_add_events (w->control_bar, GDK_POINTER_MOTION_MASK);
    g_signal_connect (w->control_bar, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);

    GtkStyleContext *c_ctx = gtk_widget_get_style_context (w->control_bar);
    gtk_style_context_add_class (c_ctx, "video-control-bar");
    gtk_box_pack_start (GTK_BOX (w->det_main_box), w->control_bar, FALSE, FALSE, 0);

    /* Play / Pause button */
    w->btn_play = gtk_button_new_with_label ("▶");
    gtk_widget_set_can_focus (w->btn_play, FALSE);
    GtkStyleContext *play_ctx = gtk_widget_get_style_context (w->btn_play);
    gtk_style_context_add_class (play_ctx, "btn-play");
    gtk_widget_set_tooltip_text (w->btn_play, "播放 / 暫停 (Space)");
    g_signal_connect (w->btn_play, "clicked", G_CALLBACK (on_btn_play_clicked), w);
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->btn_play, FALSE, FALSE, 2);

    /* Stop button */
    w->btn_stop = gtk_button_new_with_label ("⏹");
    gtk_widget_set_can_focus (w->btn_stop, FALSE);
    gtk_widget_set_tooltip_text (w->btn_stop, "停止 (Stop)");
    g_signal_connect (w->btn_stop, "clicked", G_CALLBACK (on_btn_stop_clicked), w);
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->btn_stop, FALSE, FALSE, 2);

    /* Previous button */
    w->btn_prev = gtk_button_new_with_label ("⏮");
    gtk_widget_set_can_focus (w->btn_prev, FALSE);
    gtk_widget_set_tooltip_text (w->btn_prev, "上一首 (Previous)");
    g_signal_connect (w->btn_prev, "clicked", G_CALLBACK (on_btn_prev_clicked), w);
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->btn_prev, FALSE, FALSE, 2);

    /* Next button */
    w->btn_next = gtk_button_new_with_label ("⏭");
    gtk_widget_set_can_focus (w->btn_next, FALSE);
    gtk_widget_set_tooltip_text (w->btn_next, "下一首 (Next)");
    g_signal_connect (w->btn_next, "clicked", G_CALLBACK (on_btn_next_clicked), w);
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->btn_next, FALSE, FALSE, 2);

    /* Current time label */
    w->lbl_time_cur = gtk_label_new ("00:00");
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->lbl_time_cur, FALSE, FALSE, 4);

    /* Seek / Progress slider */
    w->seek_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_widget_set_can_focus (w->seek_scale, FALSE);
    gtk_scale_set_draw_value (GTK_SCALE (w->seek_scale), FALSE);
    g_signal_connect (w->seek_scale, "button-press-event", G_CALLBACK (on_seek_scale_button_press), w);
    g_signal_connect (w->seek_scale, "button-release-event", G_CALLBACK (on_seek_scale_button_release), w);
    g_signal_connect (w->seek_scale, "value-changed", G_CALLBACK (on_seek_scale_value_changed), w);
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->seek_scale, TRUE, TRUE, 4);

    /* Total time label */
    w->lbl_time_total = gtk_label_new ("00:00");
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->lbl_time_total, FALSE, FALSE, 4);

    /* Volume button */
    w->vol_btn = gtk_volume_button_new ();
    gtk_widget_set_can_focus (w->vol_btn, FALSE);
    gtk_widget_set_tooltip_text (w->vol_btn, "音量控制 (上下方向鍵)");
    w->updating_volume = TRUE;
    gtk_scale_button_set_value (GTK_SCALE_BUTTON (w->vol_btn), (double)deadbeef->volume_get_amp ());
    w->updating_volume = FALSE;
    g_signal_connect (w->vol_btn, "value-changed", G_CALLBACK (on_volume_value_changed), w);
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->vol_btn, FALSE, FALSE, 2);

    /* Windowed / Fullscreen toggle button */
    w->btn_fs_toggle = gtk_button_new_with_label ("⛶");
    gtk_widget_set_can_focus (w->btn_fs_toggle, FALSE);
    gtk_widget_set_tooltip_text (w->btn_fs_toggle, "全螢幕 / 視窗化切換 (F11)");
    g_signal_connect (w->btn_fs_toggle, "clicked", G_CALLBACK (on_btn_fs_toggle_clicked), w);
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->btn_fs_toggle, FALSE, FALSE, 2);

    /* Dock button */
    w->btn_dock = gtk_button_new_with_label ("⤓");
    gtk_widget_set_can_focus (w->btn_dock, FALSE);
    gtk_widget_set_tooltip_text (w->btn_dock, "還原嵌入主視窗 (Esc)");
    g_signal_connect (w->btn_dock, "clicked", G_CALLBACK (on_btn_dock_clicked), w);
    gtk_box_pack_start (GTK_BOX (w->control_bar), w->btn_dock, FALSE, FALSE, 2);

    /* Realize detached window so it has GdkWindow */
    gtk_widget_realize (w->det_window);

    /* 4. Initialize libmpv instance with C numeric locale */
    setlocale (LC_NUMERIC, "C");
    w->mpv = mpv_create ();
    if (w->mpv) {
        /* Rendering backend: GPU */
        mpv_set_option_string (w->mpv, "vo", "gpu");
        mpv_set_option_string (w->mpv, "gpu-context", "x11egl,x11");

        /* Hardware decoding: Prioritize Nvidia NVDEC */
        mpv_set_option_string (w->mpv, "hwdec", "nvdec,nvdec-copy,auto");

        /* Audio disabled in MPV to avoid duplicate sound with DeaDBeeF audio engine */
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

    /* Start periodic A/V sync & progress timer (200ms) */
    w->sync_timer_id = g_timeout_add (200, periodic_timer_cb, w);

    /* Default view: show cover area initially in embedded mode */
    gtk_widget_show (w->cover_area);
    gtk_widget_hide (w->video_area);
    gtk_widget_show_all (w->container);

    /* Register to global widget list thread-safely */
    g_mutex_lock (&widgets_mutex);
    active_widgets = g_list_append (active_widgets, w);
    g_mutex_unlock (&widgets_mutex);

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
    case DB_EV_PAUSED:
    case DB_EV_VOLUMECHANGED:
    case DB_EV_SEEKED:
    case DB_EV_SEEK:
        g_mutex_lock (&widgets_mutex);
        for (GList *l = active_widgets; l; l = l->next) {
            plugin_msg_event_t *ev = malloc (sizeof (plugin_msg_event_t));
            ev->w = (w_video_t *)l->data;
            ev->id = id;
            ev->p1 = p1;
            ev->p2 = p2;
            g_idle_add (on_plugin_msg_idle, ev);
        }
        g_mutex_unlock (&widgets_mutex);
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
    .plugin.descr = "MP4 video playback with Nvidia NVDEC hardware acceleration, windowed/fullscreen controls and CoverArt support (GTK3 / libmpv)",
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
