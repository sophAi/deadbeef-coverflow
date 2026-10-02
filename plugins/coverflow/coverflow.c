/*
    Cover Flow Plugin for DeaDBeeF (GTK3 / OpenGL)
    Copyright (C) 2026 DeaDBeeF Cover Flow Contributors

    This plugin provides a 3D Mac-like Cover Flow interface using GtkGLArea and OpenGL,
    integrating album grouping and embedded MP3 album art via DeaDBeeF's artwork2 plugin.
*/

#include <deadbeef/deadbeef.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <gdk/gdkkeysyms.h>
#include <epoxy/gl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../gtkui/gtkui_api.h"
#include "../artwork/artwork.h"
#include "album_manager.h"
#include "gl_coverflow.h"

DB_functions_t *deadbeef;
static ddb_gtkui_t *gtkui_plugin;

typedef struct {
    int ref_count;
    gboolean alive;
} coverflow_lifecycle_t;

typedef struct {
    ddb_gtkui_widget_t base;
    ddb_gtkui_widget_extended_api_t exapi;

    GtkWidget *container;    /* Vertical GtkBox holding GLArea & bottom info panel */
    GtkWidget *gl_area;      /* GtkGLArea 3D viewport */
    GtkWidget *info_box;     /* Bottom bar with labels */
    GtkWidget *lbl_album;    /* Bold primary album label */
    GtkWidget *lbl_artist;   /* Artist label */
    GtkWidget *lbl_meta;     /* Track count and release year */

    album_manager_t album_mgr;
    gl_coverflow_renderer_t gl_renderer;

    ddb_artwork_plugin_t *artwork_plugin;
    int64_t artwork_source_id;
    coverflow_lifecycle_t *lifecycle;

    float current_pos;
    float target_pos;
    guint tick_callback_id;
    int playlist_version;

    /* Interactive drag state */
    gboolean is_dragging;
    double drag_start_x;
    float drag_start_pos;

    int last_displayed_album;
} w_coverflow_t;

/* ---------------- Helper: Playback Trigger ---------------- */

static void
play_selected_album (w_coverflow_t *w, int album_idx) {
    if (!w || album_idx < 0 || album_idx >= w->album_mgr.count) {
        return;
    }
    coverflow_album_t *al = &w->album_mgr.albums[album_idx];
    if (al->track_count > 0) {
        int first_track = al->track_indices[0];
        deadbeef->sendmessage (DB_EV_PLAY_NUM, 0, first_track, 0);
    }
}

/* ---------------- UI Metadata Labels Update ---------------- */

static void
update_info_labels (w_coverflow_t *w) {
    if (!w || !w->lbl_album) return;

    int idx = (int)roundf (w->current_pos);
    if (idx < 0 || idx >= w->album_mgr.count) {
        gtk_label_set_text (GTK_LABEL (w->lbl_album), "");
        gtk_label_set_text (GTK_LABEL (w->lbl_artist), _("No Albums in Playlist"));
        gtk_label_set_text (GTK_LABEL (w->lbl_meta), "");
        w->last_displayed_album = -1;
        return;
    }

    if (idx == w->last_displayed_album) {
        return;
    }
    w->last_displayed_album = idx;

    coverflow_album_t *al = &w->album_mgr.albums[idx];

    /* Format album title with markup */
    char *album_markup = g_markup_printf_escaped (
        "<span size='large' weight='bold' color='#f0f0f0'>%s</span>",
        al->album ? al->album : "Unknown Album");
    gtk_label_set_markup (GTK_LABEL (w->lbl_album), album_markup);
    g_free (album_markup);

    /* Format artist */
    char *artist_markup = g_markup_printf_escaped (
        "<span size='medium' color='#b0b0b8'>%s</span>",
        al->artist ? al->artist : "Unknown Artist");
    gtk_label_set_markup (GTK_LABEL (w->lbl_artist), artist_markup);
    g_free (artist_markup);

    /* Format metadata details */
    char meta_str[256];
    if (al->year && *al->year) {
        snprintf (meta_str, sizeof (meta_str), "%d tracks • %s", al->track_count, al->year);
    } else {
        snprintf (meta_str, sizeof (meta_str), "%d tracks", al->track_count);
    }
    gtk_label_set_text (GTK_LABEL (w->lbl_meta), meta_str);
}

/* ---------------- Texture Upload Idle Callback ---------------- */

typedef struct {
    w_coverflow_t *w;
    coverflow_lifecycle_t *lifecycle;
    int playlist_version;
    int album_index;
    char *image_path;
} texture_ready_data_t;

static gboolean
on_texture_ready_in_main_thread (gpointer user_data) {
    texture_ready_data_t *data = (texture_ready_data_t *)user_data;

    if (data->lifecycle && data->lifecycle->alive) {
        w_coverflow_t *w = data->w;
        int idx = data->album_index;

        if (data->playlist_version == w->playlist_version &&
            idx >= 0 && idx < w->album_mgr.count) {
            coverflow_album_t *al = &w->album_mgr.albums[idx];
            if (al->pending_image_path) {
                free (al->pending_image_path);
                al->pending_image_path = NULL;
            }
            if (data->image_path && data->image_path[0]) {
                al->pending_image_path = data->image_path;
                data->image_path = NULL; /* Transfer ownership */
            }
            al->texture_loaded = TRUE;
            al->is_fetching = FALSE;

            if (w->gl_area && GTK_IS_WIDGET (w->gl_area)) {
                gtk_widget_queue_draw (w->gl_area);
            }
        }
    }

    if (data->image_path) {
        free (data->image_path);
    }
    if (data->lifecycle) {
        data->lifecycle->ref_count--;
        if (data->lifecycle->ref_count == 0) {
            free (data->lifecycle);
        }
    }
    free (data);
    return G_SOURCE_REMOVE;
}

/* Artwork Query Callback (Runs on artwork plugin background worker thread) */
static void
on_cover_query_callback (int error, ddb_cover_query_t *query, ddb_cover_info_t *cover) {
    if (!query) return;

    if (!(query->flags & DDB_ARTWORK_FLAG_CANCELLED) && query->user_data) {
        texture_ready_data_t *data = (texture_ready_data_t *)query->user_data;
        if (!error && cover && cover->cover_found && cover->image_filename) {
            data->image_path = strdup (cover->image_filename);
        } else {
            data->image_path = NULL;
        }
        g_idle_add (on_texture_ready_in_main_thread, data);
    } else if (query->user_data) {
        texture_ready_data_t *data = (texture_ready_data_t *)query->user_data;
        if (data->image_path) {
            free (data->image_path);
        }
        if (data->lifecycle) {
            data->lifecycle->ref_count--;
            if (data->lifecycle->ref_count == 0) {
                free (data->lifecycle);
            }
        }
        free (data);
    }

    if (query->track) {
        deadbeef->pl_item_unref (query->track);
    }
    free (query);
}

static void
check_and_fetch_artwork_for_visible_albums (w_coverflow_t *w) {
    if (!w->artwork_plugin || w->album_mgr.count == 0) return;

    int center = (int)roundf (w->current_pos);
    int fetch_radius = 8;
    int min_idx = center - fetch_radius;
    if (min_idx < 0) min_idx = 0;
    int max_idx = center + fetch_radius;
    if (max_idx >= w->album_mgr.count) max_idx = w->album_mgr.count - 1;

    for (int i = min_idx; i <= max_idx; i++) {
        coverflow_album_t *al = &w->album_mgr.albums[i];
        if (!al->texture_loaded && !al->is_fetching && al->rep_track) {
            al->is_fetching = TRUE;

            texture_ready_data_t *tdata = malloc (sizeof (texture_ready_data_t));
            tdata->w = w;
            tdata->lifecycle = w->lifecycle;
            if (w->lifecycle) {
                w->lifecycle->ref_count++;
            }
            tdata->playlist_version = w->playlist_version;
            tdata->album_index = i;
            tdata->image_path = NULL;

            ddb_cover_query_t *q = calloc (1, sizeof (ddb_cover_query_t));
            q->_size = sizeof (ddb_cover_query_t);
            q->track = al->rep_track;
            deadbeef->pl_item_ref (q->track);
            q->source_id = w->artwork_source_id;
            q->user_data = tdata;

            w->artwork_plugin->cover_get (q, on_cover_query_callback);
        }
    }
}

/* ---------------- Animation Tick Callback ---------------- */

static gboolean
on_animation_tick (GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    if (!w || !w->gl_area || !GTK_IS_WIDGET (widget)) {
        return G_SOURCE_REMOVE;
    }

    float diff = w->target_pos - w->current_pos;
    if (fabsf (diff) > 0.0005f) {
        /* Smooth spring/damped easing */
        w->current_pos += diff * 0.18f;
        update_info_labels (w);
        gtk_widget_queue_draw (widget);
    } else {
        w->current_pos = w->target_pos;
        update_info_labels (w);
    }

    return G_SOURCE_CONTINUE;
}

/* ---------------- OpenGL Signals ---------------- */

static void
on_gl_realize (GtkGLArea *area, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    gtk_gl_area_make_current (area);
    if (gtk_gl_area_get_error (area) != NULL) {
        return;
    }
    gl_coverflow_init (&w->gl_renderer);
}

static void
on_gl_unrealize (GtkGLArea *area, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    gtk_gl_area_make_current (area);
    gl_coverflow_cleanup (&w->gl_renderer);
    album_manager_flush_pending_deletes (&w->album_mgr);
    album_manager_free_textures (&w->album_mgr);
    album_manager_flush_pending_deletes (&w->album_mgr);
}

static void
on_gl_resize (GtkGLArea *area, int width, int height, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    gtk_gl_area_make_current (area);
    gl_coverflow_resize (&w->gl_renderer, width, height);
}

static gboolean
on_gl_render (GtkGLArea *area, GdkGLContext *context, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    if (!w) return FALSE;

    /* 1. Safely flush any deferred texture deletions in active GL context */
    album_manager_flush_pending_deletes (&w->album_mgr);

    /* 2. Upload any pending album textures */
    for (int i = 0; i < w->album_mgr.count; i++) {
        coverflow_album_t *al = &w->album_mgr.albums[i];
        if (al->pending_image_path) {
            int tw = 0, th = 0;
            GLuint tex = gl_coverflow_load_texture_from_file (al->pending_image_path, &tw, &th);
            if (tex) {
                if (al->texture_id) {
                    album_manager_queue_delete_texture (&w->album_mgr, al->texture_id);
                }
                al->texture_id = tex;
                al->tex_width = tw;
                al->tex_height = th;
            }
            free (al->pending_image_path);
            al->pending_image_path = NULL;
        }
    }

    /* 3. Query background artwork for visible albums */
    check_and_fetch_artwork_for_visible_albums (w);

    /* 4. Render 3D Cover Flow */
    gl_coverflow_render (&w->gl_renderer, &w->album_mgr, w->current_pos);

    return TRUE;
}

/* ---------------- Mouse & Keyboard Input Handling ---------------- */

static void
clamp_target_position (w_coverflow_t *w) {
    if (w->album_mgr.count <= 0) {
        w->target_pos = 0.0f;
        return;
    }
    if (w->target_pos < 0.0f) {
        w->target_pos = 0.0f;
    } else if (w->target_pos > (float)(w->album_mgr.count - 1)) {
        w->target_pos = (float)(w->album_mgr.count - 1);
    }
}

static gboolean
on_scroll_event (GtkWidget *widget, GdkEventScroll *event, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;

    if (event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_LEFT) {
        w->target_pos -= 1.0f;
    } else if (event->direction == GDK_SCROLL_DOWN || event->direction == GDK_SCROLL_RIGHT) {
        w->target_pos += 1.0f;
    } else if (event->direction == GDK_SCROLL_SMOOTH) {
        double dx = 0, dy = 0;
        gdk_event_get_scroll_deltas ((GdkEvent *)event, &dx, &dy);
        double delta = (fabs(dx) > fabs(dy)) ? dx : dy;
        w->target_pos += (float)delta * 0.8f;
    }

    clamp_target_position (w);
    return TRUE;
}

static void
on_menu_play_album (GtkMenuItem *item, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    int sel = (int)roundf (w->target_pos);
    play_selected_album (w, sel);
}

static void
on_menu_queue_album (GtkMenuItem *item, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    int sel = (int)roundf (w->target_pos);
    if (sel >= 0 && sel < w->album_mgr.count) {
        coverflow_album_t *al = &w->album_mgr.albums[sel];
        ddb_playlist_t *plt = deadbeef->plt_get_curr ();
        if (plt) {
            deadbeef->pl_lock ();
            for (int i = 0; i < al->track_count; i++) {
                DB_playItem_t *it = deadbeef->plt_get_item_for_idx (plt, al->track_indices[i], PL_MAIN);
                if (it) {
                    deadbeef->playqueue_push (it);
                    deadbeef->pl_item_unref (it);
                }
            }
            deadbeef->pl_unlock ();
            deadbeef->plt_unref (plt);
        }
    }
}

static void
on_menu_reload_covers (GtkMenuItem *item, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    if (w->artwork_plugin && w->artwork_source_id) {
        w->artwork_plugin->cancel_queries_with_source_id (w->artwork_source_id);
    }
    w->playlist_version++;
    album_manager_free_textures (&w->album_mgr);
    if (w->gl_area) {
        gtk_widget_queue_draw (w->gl_area);
    }
}

static void
show_context_menu (w_coverflow_t *w, GdkEventButton *event) {
    GtkWidget *menu = gtk_menu_new ();

    GtkWidget *mi_play = gtk_menu_item_new_with_label (_("Play Album"));
    g_signal_connect (mi_play, "activate", G_CALLBACK (on_menu_play_album), w);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi_play);

    GtkWidget *mi_queue = gtk_menu_item_new_with_label (_("Add Album to Playback Queue"));
    g_signal_connect (mi_queue, "activate", G_CALLBACK (on_menu_queue_album), w);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi_queue);

    GtkWidget *separator = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), separator);

    GtkWidget *mi_reload = gtk_menu_item_new_with_label (_("Reload Cover Artwork"));
    g_signal_connect (mi_reload, "activate", G_CALLBACK (on_menu_reload_covers), w);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi_reload);

    gtk_widget_show_all (menu);
    gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *)event);
}

static gboolean
on_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;

    /* Give focus to receive arrow keys */
    gtk_widget_grab_focus (w->gl_area);

    if (event->type == GDK_2BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY) {
        /* Double-click: Play the current center album! */
        int sel = (int)roundf (w->target_pos);
        play_selected_album (w, sel);
        return TRUE;
    }

    if (event->button == GDK_BUTTON_SECONDARY) {
        /* Right-click: Context menu */
        show_context_menu (w, event);
        return TRUE;
    }

    if (event->button == GDK_BUTTON_PRIMARY) {
        w->is_dragging = TRUE;
        w->drag_start_x = event->x;
        w->drag_start_pos = w->target_pos;

        /* Click left or right side to flip directly */
        GtkAllocation alloc;
        gtk_widget_get_allocation (widget, &alloc);
        float center_x = alloc.width / 2.0f;
        float offset = (float)event->x - center_x;

        if (offset < -120.0f) {
            w->target_pos -= 1.0f;
            clamp_target_position (w);
        } else if (offset > 120.0f) {
            w->target_pos += 1.0f;
            clamp_target_position (w);
        }

        return TRUE;
    }

    return FALSE;
}

static gboolean
on_button_release_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    if (event->button == GDK_BUTTON_PRIMARY) {
        w->is_dragging = FALSE;
        w->target_pos = roundf (w->target_pos);
        clamp_target_position (w);
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_motion_notify_event (GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    if (w->is_dragging) {
        double delta_x = event->x - w->drag_start_x;
        float sensitivity = 0.008f;
        w->target_pos = w->drag_start_pos - (float)delta_x * sensitivity;
        clamp_target_position (w);
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_key_press_event (GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;

    switch (event->keyval) {
    case GDK_KEY_Left:
    case GDK_KEY_h:
        w->target_pos -= 1.0f;
        clamp_target_position (w);
        return TRUE;

    case GDK_KEY_Right:
    case GDK_KEY_l:
        w->target_pos += 1.0f;
        clamp_target_position (w);
        return TRUE;

    case GDK_KEY_Page_Up:
        w->target_pos -= 5.0f;
        clamp_target_position (w);
        return TRUE;

    case GDK_KEY_Page_Down:
        w->target_pos += 5.0f;
        clamp_target_position (w);
        return TRUE;

    case GDK_KEY_Home:
        w->target_pos = 0.0f;
        clamp_target_position (w);
        return TRUE;

    case GDK_KEY_End:
        w->target_pos = (float)(w->album_mgr.count - 1);
        clamp_target_position (w);
        return TRUE;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_space:
        play_selected_album (w, (int)roundf (w->target_pos));
        return TRUE;
    }

    return FALSE;
}

/* ---------------- Widget Lifecycle & Design Mode ---------------- */

static void
on_gl_area_destroy (GtkWidget *widget, gpointer user_data) {
    w_coverflow_t *w = (w_coverflow_t *)user_data;
    w->tick_callback_id = 0;
    w->gl_area = NULL;
}

static void
w_coverflow_destroy (ddb_gtkui_widget_t *base) {
    w_coverflow_t *w = (w_coverflow_t *)base;
    if (!w) return;

    if (w->lifecycle) {
        w->lifecycle->alive = FALSE;
        w->lifecycle->ref_count--;
        if (w->lifecycle->ref_count == 0) {
            free (w->lifecycle);
        }
        w->lifecycle = NULL;
    }

    if (w->tick_callback_id && w->gl_area && GTK_IS_WIDGET (w->gl_area)) {
        gtk_widget_remove_tick_callback (w->gl_area, w->tick_callback_id);
        w->tick_callback_id = 0;
    }

    if (w->artwork_plugin && w->artwork_source_id) {
        w->artwork_plugin->cancel_queries_with_source_id (w->artwork_source_id);
    }

    album_manager_free (&w->album_mgr);
    /* Note: Do NOT call free(w) here! DeaDBeeF gtkui's w_destroy() frees w. */
}

static int
w_coverflow_message (ddb_gtkui_widget_t *base, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    w_coverflow_t *w = (w_coverflow_t *)base;
    if (!w) return 0;

    switch (id) {
    case DB_EV_PLAYLISTCHANGED:
    case DB_EV_PLAYLISTSWITCHED: {
        ddb_playlist_t *plt = deadbeef->plt_get_curr ();
        if (plt) {
            if (w->artwork_plugin && w->artwork_source_id) {
                w->artwork_plugin->cancel_queries_with_source_id (w->artwork_source_id);
            }
            w->playlist_version++;
            album_manager_rebuild (&w->album_mgr, plt);
            deadbeef->plt_unref (plt);

            clamp_target_position (w);
            update_info_labels (w);
            if (w->gl_area && GTK_IS_WIDGET (w->gl_area)) {
                gtk_widget_queue_draw (w->gl_area);
            }
        }
    } break;

    case DB_EV_SONGSTARTED: {
        ddb_playItem_t *track = deadbeef->streamer_get_playing_track_safe ();
        if (track) {
            int idx = deadbeef->pl_get_idx_of (track);
            deadbeef->pl_item_unref (track);
            if (idx >= 0) {
                int alb_idx = album_manager_find_album_for_track (&w->album_mgr, idx);
                if (alb_idx >= 0) {
                    w->album_mgr.current_playing_album = alb_idx;
                }
            }
        }
    } break;
    }

    return 0;
}

static ddb_gtkui_widget_t *
w_coverflow_create (void) {
    w_coverflow_t *w = calloc (1, sizeof (w_coverflow_t));
    if (!w) return NULL;

    w->base.type = "coverflow";
    w->base.destroy = w_coverflow_destroy;
    w->base.message = w_coverflow_message;

    w->lifecycle = calloc (1, sizeof (coverflow_lifecycle_t));
    w->lifecycle->ref_count = 1;
    w->lifecycle->alive = TRUE;

    album_manager_init (&w->album_mgr);

    w->artwork_plugin = (ddb_artwork_plugin_t *)deadbeef->plug_get_for_id ("artwork2");
    if (w->artwork_plugin && w->artwork_plugin->allocate_source_id) {
        w->artwork_source_id = w->artwork_plugin->allocate_source_id ();
    }

    /* Container: Vertical GtkBox with black background styling */
    w->container = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    w->base.widget = w->container;

    /* GtkGLArea 3D Viewport */
    w->gl_area = gtk_gl_area_new ();
    gtk_widget_set_can_focus (w->gl_area, TRUE);
    gtk_widget_set_size_request (w->gl_area, 300, 240);
    gtk_gl_area_set_has_depth_buffer (GTK_GL_AREA (w->gl_area), TRUE);
    gtk_box_pack_start (GTK_BOX (w->container), w->gl_area, TRUE, TRUE, 0);

    g_signal_connect (w->gl_area, "destroy", G_CALLBACK (on_gl_area_destroy), w);

    /* Bottom Info Panel (Dark translucent bar for Artist / Album / Details) */
    w->info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top (w->info_box, 6);
    gtk_widget_set_margin_bottom (w->info_box, 12);

    w->lbl_album = gtk_label_new ("");
    gtk_label_set_ellipsize (GTK_LABEL (w->lbl_album), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start (GTK_BOX (w->info_box), w->lbl_album, FALSE, FALSE, 0);

    w->lbl_artist = gtk_label_new (_("No Albums"));
    gtk_label_set_ellipsize (GTK_LABEL (w->lbl_artist), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start (GTK_BOX (w->info_box), w->lbl_artist, FALSE, FALSE, 0);

    w->lbl_meta = gtk_label_new ("");
    gtk_box_pack_start (GTK_BOX (w->info_box), w->lbl_meta, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (w->container), w->info_box, FALSE, FALSE, 0);

    /* OpenGL Signals */
    g_signal_connect (w->gl_area, "realize", G_CALLBACK (on_gl_realize), w);
    g_signal_connect (w->gl_area, "unrealize", G_CALLBACK (on_gl_unrealize), w);
    g_signal_connect (w->gl_area, "resize", G_CALLBACK (on_gl_resize), w);
    g_signal_connect (w->gl_area, "render", G_CALLBACK (on_gl_render), w);

    /* Input Events */
    gtk_widget_add_events (w->gl_area,
                           GDK_SCROLL_MASK |
                           GDK_BUTTON_PRESS_MASK |
                           GDK_BUTTON_RELEASE_MASK |
                           GDK_POINTER_MOTION_MASK |
                           GDK_KEY_PRESS_MASK);

    g_signal_connect (w->gl_area, "scroll-event", G_CALLBACK (on_scroll_event), w);
    g_signal_connect (w->gl_area, "button-press-event", G_CALLBACK (on_button_press_event), w);
    g_signal_connect (w->gl_area, "button-release-event", G_CALLBACK (on_button_release_event), w);
    g_signal_connect (w->gl_area, "motion-notify-event", G_CALLBACK (on_motion_notify_event), w);
    g_signal_connect (w->gl_area, "key-press-event", G_CALLBACK (on_key_press_event), w);

    /* Animation Frame Clock (60fps smooth easing) */
    w->tick_callback_id = gtk_widget_add_tick_callback (w->gl_area, on_animation_tick, w, NULL);

    /* Initial playlist population */
    ddb_playlist_t *plt = deadbeef->plt_get_curr ();
    if (plt) {
        album_manager_rebuild (&w->album_mgr, plt);
        deadbeef->plt_unref (plt);
    }
    update_info_labels (w);

    /* Support DeaDBeeF Design Mode right-click hierarchy */
    if (gtkui_plugin) {
        gtkui_plugin->w_override_signals (w->container, w);
    }

    gtk_widget_show_all (w->container);
    return (ddb_gtkui_widget_t *)w;
}

/* ---------------- Plugin Entry Point & Registration ---------------- */

static int
coverflow_connect (void) {
    gtkui_plugin = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (!gtkui_plugin) {
        return -1;
    }
    gtkui_plugin->w_reg_widget (_("Cover Flow"), 0, w_coverflow_create, "coverflow", NULL);
    return 0;
}

static int
coverflow_disconnect (void) {
    if (gtkui_plugin) {
        gtkui_plugin->w_unreg_widget ("coverflow");
        gtkui_plugin = NULL;
    }
    return 0;
}

static DB_misc_t plugin = {
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_MISC,
    .plugin.id = "coverflow_gtk3",
    .plugin.name = "Cover Flow",
    .plugin.descr = "Mac-like 3D Cover Flow album browser with OpenGL (GTK3)",
    .plugin.copyright =
        "DeaDBeeF Cover Flow Plugin\n"
        "Copyright (C) 2026\n"
        "GPLv2 Licensed\n",
    .plugin.website = "https://github.com/sophAi/deadbeef-coverflow",
    .plugin.connect = coverflow_connect,
    .plugin.disconnect = coverflow_disconnect,
};

DB_plugin_t *
coverflow_gtk3_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
