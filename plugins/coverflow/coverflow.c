/*
    Cover Flow Plugin for DeaDBeeF (GTK3 / OpenGL)
    Copyright (C) 2026 DeaDBeeF Cover Flow Contributors

    This plugin provides a 3D Mac-like Cover Flow interface using GtkGLArea and OpenGL,
    integrating album grouping and embedded MP3 album art via DeaDBeeF's artwork2 plugin.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <deadbeef/deadbeef.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <gdk/gdkkeysyms.h>
#include <epoxy/gl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>

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

    GtkWidget *container;    /* Vertical GtkBox holding top title label & GLArea */
    GtkWidget *gl_area;      /* GtkGLArea 3D viewport */
    GtkWidget *lbl_album;    /* Album title label displayed above 3D covers */

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
        w->last_displayed_album = -1;
        return;
    }

    if (idx == w->last_displayed_album) {
        return;
    }
    w->last_displayed_album = idx;

    coverflow_album_t *al = &w->album_mgr.albums[idx];

    /* Format album title (and artist if available) with escaped markup */
    char *album_markup = NULL;
    if (al->artist && al->artist[0] && strcmp (al->artist, "Unknown Artist") != 0) {
        album_markup = g_markup_printf_escaped (
            "<span size='large' weight='bold' color='#ffffff'>%s</span>  <span size='medium' color='#a6a6b0'>• %s</span>",
            al->album ? al->album : "Unknown Album", al->artist);
    } else {
        album_markup = g_markup_printf_escaped (
            "<span size='large' weight='bold' color='#ffffff'>%s</span>",
            al->album ? al->album : "Unknown Album");
    }
    gtk_label_set_markup (GTK_LABEL (w->lbl_album), album_markup);
    g_free (album_markup);
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
    int fetch_radius = 24;
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

static int s_current_dark_theme = -1;
static GtkCssProvider *s_dark_css_provider = NULL;

static void
set_dark_theme_config (int dark) {
    if (dark) {
        /* Enable overrides for Tabstrip & Listview */
        deadbeef->conf_set_int ("gtkui.override_tabstrip_colors", 1);
        deadbeef->conf_set_int ("gtkui.override_listview_colors", 1);

        /* Tabstrip Palette (16-bit RGB 0..65535 format: "R G B") */
        /* Active tab background & listview column header background: #202025 */
        deadbeef->conf_set_str ("gtkui.color.tabstrip_base", "8224 8224 9508");
        /* Inactive tab & tabstrip bar background: #16161a */
        deadbeef->conf_set_str ("gtkui.color.tabstrip_mid", "5654 5654 6682");
        /* Border / outer frame / divider lines: #101014 */
        deadbeef->conf_set_str ("gtkui.color.tabstrip_dark", "4112 4112 5140");
        /* Inner frame highlight / divider line: #2c2c36 */
        deadbeef->conf_set_str ("gtkui.color.tabstrip_light", "11308 11308 13878");
        /* Inactive tab label text: #9e9ea8 */
        deadbeef->conf_set_str ("gtkui.color.tabstrip_text", "40606 40606 43176");
        /* Active tab label text: #ffffff */
        deadbeef->conf_set_str ("gtkui.color.tabstrip_selected_text", "65535 65535 65535");
        /* Playing tab label text: #5dade2 (vibrant cyan/blue) */
        deadbeef->conf_set_str ("gtkui.color.tabstrip_playing_text", "23901 44461 57825");

        /* Listview Headers & Rows Palette */
        /* Column header text: #dcdce4 */
        deadbeef->conf_set_str ("gtkui.color.listview_column_text", "56540 56540 58600");
        /* Even row background: #19191e */
        deadbeef->conf_set_str ("gtkui.color.listview_even_row", "6425 6425 7710");
        /* Odd row background: #1e1e24 */
        deadbeef->conf_set_str ("gtkui.color.listview_odd_row", "7710 7710 9252");
        /* Selected row background: #24528c */
        deadbeef->conf_set_str ("gtkui.color.listview_selection", "9252 21074 35980");
        /* Normal row text: #e0e0e8 */
        deadbeef->conf_set_str ("gtkui.color.listview_text", "57568 57568 59624");
        /* Selected row text: #ffffff */
        deadbeef->conf_set_str ("gtkui.color.listview_selected_text", "65535 65535 65535");
        /* Playing row text: #5dade2 */
        deadbeef->conf_set_str ("gtkui.color.listview_playing_text", "23901 44461 57825");
        /* Group header text: #a4a4b4 */
        deadbeef->conf_set_str ("gtkui.color.listview_group_text", "42148 42148 46260");
        /* Cursor border: #5dade2 */
        deadbeef->conf_set_str ("gtkui.color.listview_cursor", "23901 44461 57825");
    } else {
        /* Disable overrides so DeaDBeeF returns to system/default theme */
        deadbeef->conf_set_int ("gtkui.override_tabstrip_colors", 0);
        deadbeef->conf_set_int ("gtkui.override_listview_colors", 0);
    }
    deadbeef->conf_save ();
}

static void
apply_gtk_dark_css (gboolean enable) {
    GdkScreen *screen = gdk_screen_get_default ();
    if (!screen) return;

    if (enable) {
        if (!s_dark_css_provider) {
            s_dark_css_provider = gtk_css_provider_new ();
            const char *dark_css =
                "window, .background {\n"
                "    background-color: #1a1a1f;\n"
                "    color: #dedee6;\n"
                "}\n"
                "headerbar, toolbar, menubar {\n"
                "    background-color: #141418;\n"
                "    color: #dedee6;\n"
                "}\n"
                "menu, .menu {\n"
                "    background-color: #222228;\n"
                "    color: #dedee6;\n"
                "    border: 1px solid #33333e;\n"
                "}\n"
                "menuitem, .menuitem {\n"
                "    color: #dedee6;\n"
                "}\n"
                "menuitem:hover, .menuitem:hover {\n"
                "    background-color: #32323e;\n"
                "    color: #ffffff;\n"
                "}\n"
                "scrollbar slider {\n"
                "    background-color: #3e3e4a;\n"
                "    border-radius: 4px;\n"
                "    min-width: 6px;\n"
                "    min-height: 6px;\n"
                "}\n"
                "scrollbar slider:hover {\n"
                "    background-color: #555566;\n"
                "}\n"
                "scrollbar trough {\n"
                "    background-color: #16161b;\n"
                "}\n"
                "treeview {\n"
                "    background-color: #1e1e24;\n"
                "    color: #dedee6;\n"
                "}\n"
                "notebook > header {\n"
                "    background-color: #141418;\n"
                "}\n"
                "notebook > header > tabs > tab {\n"
                "    background-color: #1c1c22;\n"
                "    color: #9e9ea8;\n"
                "}\n"
                "notebook > header > tabs > tab:checked {\n"
                "    background-color: #22222a;\n"
                "    color: #ffffff;\n"
                "}\n";
            gtk_css_provider_load_from_data (s_dark_css_provider, dark_css, -1, NULL);
            gtk_style_context_add_provider_for_screen (screen,
                GTK_STYLE_PROVIDER (s_dark_css_provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
    } else {
        if (s_dark_css_provider) {
            gtk_style_context_remove_provider_for_screen (screen,
                GTK_STYLE_PROVIDER (s_dark_css_provider));
            g_object_unref (s_dark_css_provider);
            s_dark_css_provider = NULL;
        }
    }
}

static gboolean
apply_dark_theme_idle (gpointer data) {
    int prefer_dark = GPOINTER_TO_INT (data);

    /* 1. Toggle GTK Application Prefer Dark Theme */
    GtkSettings *settings = gtk_settings_get_default ();
    if (settings) {
        g_object_set (settings, "gtk-application-prefer-dark-theme", prefer_dark ? TRUE : FALSE, NULL);
    }

    /* 2. Global Dark CSS */
    apply_gtk_dark_css (prefer_dark ? TRUE : FALSE);

    /* 3. Trigger gtkui theme color reload if available */
    void (*p_init_theme_colors)(void) = dlsym (RTLD_DEFAULT, "gtkui_init_theme_colors");
    if (!p_init_theme_colors) {
        void *h = dlopen ("ddb_gui_GTK3.so", RTLD_NOLOAD | RTLD_LAZY);
        if (h) {
            p_init_theme_colors = dlsym (h, "gtkui_init_theme_colors");
        }
    }
    if (p_init_theme_colors) {
        p_init_theme_colors ();
    }

    /* 4. Notify gtkui that tabstrip and listview settings changed */
    deadbeef->sendmessage (DB_EV_CONFIGCHANGED, (uintptr_t)"gtkui.override_tabstrip_colors", 0, 0);
    deadbeef->sendmessage (DB_EV_CONFIGCHANGED, (uintptr_t)"gtkui.override_listview_colors", 0, 0);

    /* 5. Queue redraw across all open GTK windows */
    GList *toplevels = gtk_window_list_toplevels ();
    for (GList *l = toplevels; l != NULL; l = l->next) {
        if (GTK_IS_WIDGET (l->data)) {
            gtk_widget_queue_draw (GTK_WIDGET (l->data));
        }
    }
    g_list_free (toplevels);

    return G_SOURCE_REMOVE;
}

static void
apply_dark_theme (int prefer_dark) {
    if (s_current_dark_theme == prefer_dark) {
        return;
    }
    s_current_dark_theme = prefer_dark;
    set_dark_theme_config (prefer_dark);
    g_idle_add (apply_dark_theme_idle, GINT_TO_POINTER (prefer_dark));
}

static void
on_menu_toggle_dark_theme (GtkCheckMenuItem *item, gpointer user_data) {
    gboolean active = gtk_check_menu_item_get_active (item);
    deadbeef->conf_set_int ("gtkui.prefer_dark_theme", active ? 1 : 0);
    deadbeef->conf_save ();
    apply_dark_theme (active ? 1 : 0);
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

    GtkWidget *separator2 = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), separator2);

    GtkWidget *mi_dark = gtk_check_menu_item_new_with_label (_("Dark Theme"));
    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (mi_dark), deadbeef->conf_get_int ("gtkui.prefer_dark_theme", 0));
    g_signal_connect (mi_dark, "toggled", G_CALLBACK (on_menu_toggle_dark_theme), NULL);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi_dark);

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
    case DB_EV_PLAYLISTSWITCHED:
    case DB_EV_TRACKINFOCHANGED: {
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

static void
on_container_realize (GtkWidget *widget, gpointer user_data) {
    GtkWidget *toplevel = gtk_widget_get_toplevel (widget);
    if (toplevel && gtk_widget_is_toplevel (toplevel) && GTK_IS_WINDOW (toplevel)) {
        int cur_w = 0, cur_h = 0;
        gtk_window_get_size (GTK_WINDOW (toplevel), &cur_w, &cur_h);
        if (cur_w < 1080) {
            gtk_window_resize (GTK_WINDOW (toplevel), 1080, cur_h > 0 ? cur_h : 600);
        }
    }
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

    /* Overlay container: Hosts the GtkGLArea and overlays the album title directly floating above */
    w->container = gtk_overlay_new ();
    w->base.widget = w->container;
    gtk_widget_set_size_request (w->container, 1080, 280);
    g_signal_connect (w->container, "realize", G_CALLBACK (on_container_realize), NULL);

    /* GtkGLArea 3D Viewport: Expands to fill available space with minimum 1080px width */
    w->gl_area = gtk_gl_area_new ();
    gtk_widget_set_can_focus (w->gl_area, TRUE);
    gtk_widget_set_size_request (w->gl_area, 1080, 280);
    gtk_gl_area_set_has_depth_buffer (GTK_GL_AREA (w->gl_area), TRUE);
    gtk_container_add (GTK_CONTAINER (w->container), w->gl_area);

    /* Album Title Overlay: Floating directly inside the 3D scene above the center album */
    w->lbl_album = gtk_label_new ("");
    gtk_label_set_ellipsize (GTK_LABEL (w->lbl_album), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign (w->lbl_album, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (w->lbl_album, GTK_ALIGN_START);
    gtk_widget_set_margin_top (w->lbl_album, 14);
    gtk_widget_set_margin_start (w->lbl_album, 20);
    gtk_widget_set_margin_end (w->lbl_album, 20);

    /* Semi-transparent dark pill background with subtle border and text shadow for legibility */
    GtkCssProvider *css_provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (css_provider,
        "label.coverflow-title {"
        "  color: #ffffff;"
        "  background-color: rgba(18, 18, 24, 0.72);"
        "  border: 1px solid rgba(255, 255, 255, 0.12);"
        "  border-radius: 14px;"
        "  padding: 4px 16px;"
        "  text-shadow: 0px 1px 3px rgba(0, 0, 0, 0.9);"
        "}", -1, NULL);
    GtkStyleContext *sc = gtk_widget_get_style_context (w->lbl_album);
    gtk_style_context_add_class (sc, "coverflow-title");
    gtk_style_context_add_provider (sc, GTK_STYLE_PROVIDER (css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (css_provider);

    gtk_overlay_add_overlay (GTK_OVERLAY (w->container), w->lbl_album);
    gtk_overlay_set_overlay_pass_through (GTK_OVERLAY (w->container), w->lbl_album, TRUE);

    g_signal_connect (w->gl_area, "destroy", G_CALLBACK (on_gl_area_destroy), w);

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

/* ---------------- Plugin Actions & Preferences ---------------- */

static GList *
get_selected_tracks_from_context (ddb_action_context_t ctx, ddb_playlist_t **out_plt) {
    ddb_playlist_t *plt = deadbeef->action_get_playlist ();
    if (!plt) {
        plt = deadbeef->plt_get_curr ();
    }
    if (!plt) {
        return NULL;
    }

    deadbeef->pl_lock ();
    GList *selected_list = NULL;
    int count = deadbeef->plt_get_item_count (plt, PL_MAIN);
    for (int i = 0; i < count; i++) {
        DB_playItem_t *it = deadbeef->plt_get_item_for_idx (plt, i, PL_MAIN);
        if (it) {
            if (deadbeef->pl_is_selected (it)) {
                deadbeef->pl_item_ref (it);
                selected_list = g_list_append (selected_list, it);
            }
            deadbeef->pl_item_unref (it);
        }
    }

    if (!selected_list) {
        int cursor = deadbeef->pl_get_cursor (PL_MAIN);
        if (cursor >= 0 && cursor < count) {
            DB_playItem_t *it = deadbeef->plt_get_item_for_idx (plt, cursor, PL_MAIN);
            if (it) {
                selected_list = g_list_append (selected_list, it);
            }
        }
    }
    deadbeef->pl_unlock ();

    if (!selected_list) {
        deadbeef->plt_unref (plt);
        return NULL;
    }

    if (out_plt) {
        *out_plt = plt;
    } else {
        deadbeef->plt_unref (plt);
    }
    return selected_list;
}

static int
action_set_album_title (DB_plugin_action_t *act, ddb_action_context_t ctx) {
    ddb_playlist_t *plt = NULL;
    GList *tracks = get_selected_tracks_from_context (ctx, &plt);
    if (!tracks) {
        return 0;
    }

    int num_tracks = g_list_length (tracks);
    DB_playItem_t *first_track = (DB_playItem_t *)tracks->data;
    const char *curr_album = deadbeef->pl_find_meta (first_track, "album");
    if (!curr_album) {
        curr_album = "";
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("批次更改專輯名稱 (Set Album Title)"),
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        _("取消 (Cancel)"), GTK_RESPONSE_CANCEL,
        _("套用 (Apply)"), GTK_RESPONSE_ACCEPT,
        NULL
    );
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 440, 160);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_set_border_width (GTK_CONTAINER (content_area), 14);

    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add (GTK_CONTAINER (content_area), vbox);

    char prompt_text[256];
    snprintf (prompt_text, sizeof (prompt_text),
              _("請輸入新的專輯名稱（將套用至 %d 首選取的音軌）："), num_tracks);
    GtkWidget *label = gtk_label_new (prompt_text);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (entry), curr_album);
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_box_pack_start (GTK_BOX (vbox), entry, FALSE, FALSE, 0);

    gtk_widget_show_all (dialog);

    gint response = gtk_dialog_run (GTK_DIALOG (dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        const char *new_text = gtk_entry_get_text (GTK_ENTRY (entry));
        char *new_title = new_text ? g_strdup (new_text) : NULL;
        if (new_title) {
            g_strstrip (new_title);
        }

        deadbeef->pl_lock ();
        DB_decoder_t **decoders = deadbeef->plug_get_decoder_list ();

        for (GList *l = tracks; l != NULL; l = l->next) {
            DB_playItem_t *it = (DB_playItem_t *)l->data;
            deadbeef->pl_delete_meta (it, "album");
            if (new_title && *new_title) {
                deadbeef->pl_append_meta (it, "album", new_title);
            }

            /* Write metadata tags to audio file */
            const char *dec_id = deadbeef->pl_find_meta_raw (it, ":DECODER");
            if (dec_id && decoders) {
                for (int d = 0; decoders[d]; d++) {
                    if (strcmp (decoders[d]->plugin.id, dec_id) == 0) {
                        if (decoders[d]->write_metadata) {
                            decoders[d]->write_metadata (it);
                        }
                        break;
                    }
                }
            }
        }
        deadbeef->pl_unlock ();

        if (new_title) {
            g_free (new_title);
        }

        if (plt) {
            deadbeef->plt_modified (plt);
        }
        deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);
    }

    gtk_widget_destroy (dialog);

    for (GList *l = tracks; l != NULL; l = l->next) {
        deadbeef->pl_item_unref ((DB_playItem_t *)l->data);
    }
    g_list_free (tracks);
    if (plt) {
        deadbeef->plt_unref (plt);
    }

    return 0;
}

static void
file_chooser_update_preview_cb (GtkFileChooser *chooser, gpointer user_data) {
    GtkWidget *image = GTK_WIDGET (user_data);
    char *filename = gtk_file_chooser_get_preview_filename (chooser);
    if (!filename) {
        gtk_file_chooser_set_preview_widget_active (chooser, FALSE);
        return;
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale (filename, 220, 220, TRUE, NULL);
    g_free (filename);
    if (pixbuf) {
        gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);
        g_object_unref (pixbuf);
        gtk_file_chooser_set_preview_widget_active (chooser, TRUE);
    } else {
        gtk_file_chooser_set_preview_widget_active (chooser, FALSE);
    }
}

static int
action_set_cover_art (DB_plugin_action_t *act, ddb_action_context_t ctx) {
    ddb_playlist_t *plt = NULL;
    GList *tracks = get_selected_tracks_from_context (ctx, &plt);
    if (!tracks) {
        return 0;
    }

    int num_tracks = g_list_length (tracks);

    /* Determine initial folder from first track */
    char *initial_folder = NULL;
    DB_playItem_t *first_track = (DB_playItem_t *)tracks->data;
    const char *uri = deadbeef->pl_find_meta (first_track, ":URI");
    if (uri && deadbeef->is_local_file (uri)) {
        char *path = NULL;
        if (strncmp (uri, "file://", 7) == 0) {
            path = g_filename_from_uri (uri, NULL, NULL);
        } else {
            path = g_strdup (uri);
        }
        if (path) {
            initial_folder = g_path_get_dirname (path);
            g_free (path);
        }
    }

    GtkWidget *dialog = gtk_file_chooser_dialog_new (
        _("選擇封面圖片 (Select Cover Art - PNG / JPG)"),
        NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        _("取消 (Cancel)"), GTK_RESPONSE_CANCEL,
        _("套用封面 (Apply Cover)"), GTK_RESPONSE_ACCEPT,
        NULL
    );
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

    if (initial_folder) {
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), initial_folder);
        g_free (initial_folder);
    }

    /* Filters for PNG and JPG */
    GtkFileFilter *filter_all = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_all, _("支援的圖片檔案 (*.png, *.jpg, *.jpeg)"));
    gtk_file_filter_add_pattern (filter_all, "*.png");
    gtk_file_filter_add_pattern (filter_all, "*.PNG");
    gtk_file_filter_add_pattern (filter_all, "*.jpg");
    gtk_file_filter_add_pattern (filter_all, "*.JPG");
    gtk_file_filter_add_pattern (filter_all, "*.jpeg");
    gtk_file_filter_add_pattern (filter_all, "*.JPEG");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter_all);

    GtkFileFilter *filter_png = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_png, _("PNG 圖片 (*.png)"));
    gtk_file_filter_add_pattern (filter_png, "*.png");
    gtk_file_filter_add_pattern (filter_png, "*.PNG");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter_png);

    GtkFileFilter *filter_jpg = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_jpg, _("JPEG 圖片 (*.jpg, *.jpeg)"));
    gtk_file_filter_add_pattern (filter_jpg, "*.jpg");
    gtk_file_filter_add_pattern (filter_jpg, "*.JPG");
    gtk_file_filter_add_pattern (filter_jpg, "*.jpeg");
    gtk_file_filter_add_pattern (filter_jpg, "*.JPEG");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter_jpg);

    /* Live Preview */
    GtkWidget *preview = gtk_image_new ();
    gtk_file_chooser_set_preview_widget (GTK_FILE_CHOOSER (dialog), preview);
    g_signal_connect (dialog, "update-preview", G_CALLBACK (file_chooser_update_preview_cb), preview);

    gint response = gtk_dialog_run (GTK_DIALOG (dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        char *chosen_file = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
        if (chosen_file) {
            GError *err = NULL;
            GdkPixbuf *pb = gdk_pixbuf_new_from_file (chosen_file, &err);
            if (!pb) {
                GtkWidget *err_dlg = gtk_message_dialog_new (
                    GTK_WINDOW (dialog),
                    GTK_DIALOG_MODAL,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    _("無法載入所選圖片檔案：%s"), err ? err->message : _("格式不符")
                );
                gtk_dialog_run (GTK_DIALOG (err_dlg));
                gtk_widget_destroy (err_dlg);
                if (err) g_error_free (err);
            } else {
                g_object_unref (pb);

                /* Detect PNG vs JPG */
                gboolean is_png = FALSE;
                const char *dot = strrchr (chosen_file, '.');
                if (dot && strcasecmp (dot, ".png") == 0) {
                    is_png = TRUE;
                }

                /* Collect distinct directories from selected tracks */
                GHashTable *dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

                for (GList *l = tracks; l != NULL; l = l->next) {
                    DB_playItem_t *it = (DB_playItem_t *)l->data;
                    const char *u = deadbeef->pl_find_meta (it, ":URI");
                    if (u && deadbeef->is_local_file (u)) {
                        char *p = NULL;
                        if (strncmp (u, "file://", 7) == 0) {
                            p = g_filename_from_uri (u, NULL, NULL);
                        } else {
                            p = g_strdup (u);
                        }
                        if (p) {
                            char *d = g_path_get_dirname (p);
                            if (d) {
                                g_hash_table_insert (dirs, d, GINT_TO_POINTER (1));
                            }
                            g_free (p);
                        }
                    }
                }

                GFile *src_file = g_file_new_for_path (chosen_file);
                GHashTableIter iter;
                gpointer key, val;
                g_hash_table_iter_init (&iter, dirs);
                int success_count = 0;

                while (g_hash_table_iter_next (&iter, &key, &val)) {
                    const char *target_dir = (const char *)key;

                    char dest_cover[PATH_MAX];
                    char dest_folder[PATH_MAX];
                    char old_cover[PATH_MAX];
                    char old_folder[PATH_MAX];

                    if (is_png) {
                        snprintf (dest_cover, sizeof (dest_cover), "%s/cover.png", target_dir);
                        snprintf (dest_folder, sizeof (dest_folder), "%s/folder.png", target_dir);
                        snprintf (old_cover, sizeof (old_cover), "%s/cover.jpg", target_dir);
                        snprintf (old_folder, sizeof (old_folder), "%s/folder.jpg", target_dir);
                    } else {
                        snprintf (dest_cover, sizeof (dest_cover), "%s/cover.jpg", target_dir);
                        snprintf (dest_folder, sizeof (dest_folder), "%s/folder.jpg", target_dir);
                        snprintf (old_cover, sizeof (old_cover), "%s/cover.png", target_dir);
                        snprintf (old_folder, sizeof (old_folder), "%s/folder.png", target_dir);
                    }

                    GFile *dst_c = g_file_new_for_path (dest_cover);
                    GFile *dst_f = g_file_new_for_path (dest_folder);

                    GError *copy_err = NULL;
                    if (g_file_copy (src_file, dst_c, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &copy_err)) {
                        g_file_copy (src_file, dst_f, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
                        unlink (old_cover);
                        unlink (old_folder);
                        success_count++;
                    } else if (copy_err) {
                        g_error_free (copy_err);
                    }

                    g_object_unref (dst_c);
                    g_object_unref (dst_f);
                }

                g_object_unref (src_file);
                g_hash_table_destroy (dirs);

                /* Invalidate DeaDBeeF artwork cache */
                time_t now = time (NULL);
                deadbeef->conf_set_int64 ("artwork.cache_reset_time", now);

                ddb_artwork_plugin_t *art = (ddb_artwork_plugin_t *)deadbeef->plug_get_for_id ("artwork2");
                if (art && art->reset) {
                    art->reset ();
                }

                /* Notify playlist and Cover Flow */
                deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);

                /* Friendly confirmation dialog */
                GtkWidget *done_dlg = gtk_message_dialog_new (
                    NULL,
                    GTK_DIALOG_MODAL,
                    GTK_MESSAGE_INFO,
                    GTK_BUTTONS_OK,
                    _("封面圖片設定完成！\n已成功將封面圖片套用至 %d 個目錄（共 %d 首音軌）。"),
                    success_count, num_tracks
                );
                gtk_window_set_title (GTK_WINDOW (done_dlg), _("Cover Art 設定成功"));
                gtk_dialog_run (GTK_DIALOG (done_dlg));
                gtk_widget_destroy (done_dlg);
            }
            g_free (chosen_file);
        }
    }

    gtk_widget_destroy (dialog);

    for (GList *l = tracks; l != NULL; l = l->next) {
        deadbeef->pl_item_unref ((DB_playItem_t *)l->data);
    }
    g_list_free (tracks);
    if (plt) {
        deadbeef->plt_unref (plt);
    }

    return 0;
}

static int
action_toggle_dark_theme (DB_plugin_action_t *act, void *userdata) {
    int current = deadbeef->conf_get_int ("gtkui.prefer_dark_theme", 0);
    int new_val = !current;
    deadbeef->conf_set_int ("gtkui.prefer_dark_theme", new_val);
    deadbeef->conf_save ();
    apply_dark_theme (new_val);
    return 0;
}

static DB_plugin_action_t set_album_title_action = {
    .title = "批次更改專輯名稱 (Album Title)...",
    .name = "coverflow_set_album_title",
    .flags = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = action_set_album_title,
    .next = NULL,
};

static DB_plugin_action_t set_cover_art_action = {
    .title = "更改或插入封面圖片 (Cover Art)...",
    .name = "coverflow_set_cover_art",
    .flags = DB_ACTION_SINGLE_TRACK | DB_ACTION_MULTIPLE_TRACKS | DB_ACTION_ADD_MENU,
    .callback2 = action_set_cover_art,
    .next = &set_album_title_action,
};

static DB_plugin_action_t dark_theme_action = {
    .title = "View/Dark Theme",
    .name = "toggle_dark_theme",
    .flags = DB_ACTION_COMMON,
    .callback = action_toggle_dark_theme,
    .next = &set_cover_art_action,
};

static DB_plugin_action_t *
coverflow_get_actions (DB_playItem_t *it) {
    return &dark_theme_action;
}

static const char coverflow_settings_dlg[] =
    "property \"Prefer Dark Theme (GTK)\" checkbox gtkui.prefer_dark_theme 0;\n"
;

static int
coverflow_plugin_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    if (id == DB_EV_CONFIGCHANGED) {
        if (ctx == 0 || (ctx && strcmp ((const char *)ctx, "gtkui.prefer_dark_theme") == 0)) {
            int dark = deadbeef->conf_get_int ("gtkui.prefer_dark_theme", 0);
            apply_dark_theme (dark);
        }
    }
    return 0;
}

/* ---------------- Plugin Entry Point & Registration ---------------- */

static int
coverflow_connect (void) {
    gtkui_plugin = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (!gtkui_plugin) {
        return -1;
    }
    gtkui_plugin->w_reg_widget (_("Cover Flow"), 0, w_coverflow_create, "coverflow", NULL);

    /* Enforce Dark Theme preference on startup if enabled */
    int dark = deadbeef->conf_get_int ("gtkui.prefer_dark_theme", 0);
    if (dark) {
        apply_dark_theme (dark);
    }

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
    .plugin.message = coverflow_plugin_message,
    .plugin.get_actions = coverflow_get_actions,
    .plugin.configdialog = coverflow_settings_dlg,
};

DB_plugin_t *
coverflow_gtk3_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
