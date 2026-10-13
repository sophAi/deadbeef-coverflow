/*
 * DeaDBeeF Layout & Settings Manager Plugin
 *
 * Supports exporting and importing Design Mode UI layouts and plugin settings
 * (including CoverFlow, Video Player, Color Theme, Artwork, Columns, Notifications)
 * into a portable JSON bundle file (.ddblayout / .json) with dynamic hot-reload.
 */

#include <gtk/gtk.h>
#include <deadbeef/deadbeef.h>
#include "plugins/gtkui/gtkui_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define PLUGIN_ID "layout_manager"

static DB_functions_t *deadbeef;

/* ---------------- Category Definitions ---------------- */

typedef struct {
    const char *id;
    const char *title;
    const char *desc;
    const char **prefixes;
    int is_layout;
    int def_export;
} category_def_t;

static const char *prefixes_theme[] = {
    "gtkui.color.",
    "gtkui.override_bar_colors",
    "gtkui.override_listview_colors",
    "gtkui.override_tabstrip_colors",
    "gtkui.prefer_dark_theme",
    NULL
};

static const char *prefixes_columns[] = {
    "gtkui.columns.",
    NULL
};

static const char *prefixes_coverflow[] = {
    "coverflow.",
    NULL
};

static const char *prefixes_video[] = {
    "video.",
    NULL
};

static const char *prefixes_artwork[] = {
    "artwork.",
    NULL
};

static const char *prefixes_notify[] = {
    "notify.",
    NULL
};

static const category_def_t CATEGORIES[] = {
    {
        .id = "layout",
        .title = "設計模式版面配置 (Design Mode Layout)",
        .desc = "面板切割比例、元件佈局架構 (Splitters, CoverFlow, 影片播放器, 播放清單等)",
        .prefixes = NULL,
        .is_layout = 1,
        .def_export = 1,
    },
    {
        .id = "theme",
        .title = "色彩主題與外觀 (Color Theme & UI)",
        .desc = "播放清單與分頁配色、暗色主題喜好設定 (gtkui.color.*, prefer_dark_theme)",
        .prefixes = prefixes_theme,
        .is_layout = 0,
        .def_export = 1,
    },
    {
        .id = "columns",
        .title = "播放清單與搜尋欄位 (Playlist & Search Columns)",
        .desc = "曲目欄位定義、格式標籤與顯示寬度配置 (gtkui.columns.*)",
        .prefixes = prefixes_columns,
        .is_layout = 0,
        .def_export = 1,
    },
    {
        .id = "coverflow",
        .title = "Cover Flow 3D 視覺設定",
        .desc = "3D 視角、封面反射效果、快取與外觀參數 (coverflow.*)",
        .prefixes = prefixes_coverflow,
        .is_layout = 0,
        .def_export = 1,
    },
    {
        .id = "video",
        .title = "視訊與封面播放器設定 (Video & Cover Player)",
        .desc = "影片縮放比例、硬體解碼與偏好設定 (video.*)",
        .prefixes = prefixes_video,
        .is_layout = 0,
        .def_export = 1,
    },
    {
        .id = "artwork",
        .title = "專輯封面搜尋設定 (Artwork)",
        .desc = "封面檔名遮罩、搜尋目錄、快取尺寸與內嵌優先權 (artwork.*)",
        .prefixes = prefixes_artwork,
        .is_layout = 0,
        .def_export = 1,
    },
    {
        .id = "notify",
        .title = "桌面浮動通知設定 (OSD Notifications)",
        .desc = "曲目切換 OSD 樣式、字型大小與停留時間 (notify.*)",
        .prefixes = prefixes_notify,
        .is_layout = 0,
        .def_export = 0,
    },
};

#define NUM_CATEGORIES (sizeof(CATEGORIES) / sizeof(CATEGORIES[0]))

/* ---------------- Key-Value & Bundle Structures ---------------- */

typedef struct {
    char *key;
    char *val;
} kv_pair_t;

typedef struct {
    kv_pair_t *items;
    int count;
    int capacity;
} kv_list_t;

static void kv_list_init (kv_list_t *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void kv_list_add (kv_list_t *list, const char *key, const char *val) {
    if (!key || !val) return;
    if (list->count >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 32;
        list->items = realloc (list->items, sizeof (kv_pair_t) * list->capacity);
    }
    list->items[list->count].key = strdup (key);
    list->items[list->count].val = strdup (val);
    list->count++;
}

static void kv_list_free (kv_list_t *list) {
    if (!list || !list->items) return;
    for (int i = 0; i < list->count; i++) {
        free (list->items[i].key);
        free (list->items[i].val);
    }
    free (list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

typedef struct {
    char *format;
    int version;
    char *created_at;
    char *layout_json;     /* Raw JSON substring of widget tree */
    kv_list_t settings;    /* All key-value settings */
} layout_bundle_t;

static void bundle_init (layout_bundle_t *bundle) {
    bundle->format = NULL;
    bundle->version = 1;
    bundle->created_at = NULL;
    bundle->layout_json = NULL;
    kv_list_init (&bundle->settings);
}

static void bundle_free (layout_bundle_t *bundle) {
    if (!bundle) return;
    if (bundle->format) free (bundle->format);
    if (bundle->created_at) free (bundle->created_at);
    if (bundle->layout_json) free (bundle->layout_json);
    kv_list_free (&bundle->settings);
    bundle_init (bundle);
}

/* ---------------- Helper Matching & Exclusion Logic ---------------- */

static int key_matches_category (const char *key, const category_def_t *cat) {
    if (cat->is_layout || !cat->prefixes) return 0;
    for (int i = 0; cat->prefixes[i]; i++) {
        const char *p = cat->prefixes[i];
        size_t len = strlen (p);
        if (p[len - 1] == '.') {
            if (!strncasecmp (key, p, len)) return 1;
        } else {
            if (!strcasecmp (key, p)) return 1;
        }
    }
    return 0;
}

static int key_is_excluded (const char *key) {
    if (!key) return 1;
    if (strstr (key, ".geometry.")) return 1;
    if (!strncasecmp (key, "filechooser.", 12)) return 1;
    if (!strncasecmp (key, "playlist.cursor.", 16)) return 1;
    if (!strncasecmp (key, "playlist.scroll.", 16)) return 1;
    if (!strncasecmp (key, "playlist.tab.", 13)) return 1;
    if (!strcasecmp (key, "playlist.current")) return 1;
    if (!strncasecmp (key, "playback.", 9)) return 1;
    if (!strncasecmp (key, "resume.", 7)) return 1;
    if (!strncasecmp (key, "trkproperties.", 14)) return 1;
    if (!strcasecmp (key, "cli_add_playlist_name")) return 1;
    if (!strcasecmp (key, "deadbeef_version")) return 1;
    if (!strcasecmp (key, "gui_plugin")) return 1;
    if (!strcasecmp (key, "output_plugin")) return 1;
    if (!strcasecmp (key, "alsa_soundcard")) return 1;
    if (!strcasecmp (key, "network.proxy.port")) return 1;
    return 0;
}

/* ---------------- JSON Formatting ---------------- */

static void write_json_escaped_string (FILE *fp, const char *s) {
    if (!s) {
        fputs ("\"\"", fp);
        return;
    }
    fputc ('"', fp);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs ("\\\"", fp); break;
        case '\\': fputs ("\\\\", fp); break;
        case '\b': fputs ("\\b", fp);  break;
        case '\f': fputs ("\\f", fp);  break;
        case '\n': fputs ("\\n", fp);  break;
        case '\r': fputs ("\\r", fp);  break;
        case '\t': fputs ("\\t", fp);  break;
        default:
            if (*p < 32) {
                fprintf (fp, "\\u%04x", *p);
            } else {
                fputc (*p, fp);
            }
            break;
        }
    }
    fputc ('"', fp);
}

/* ---------------- Export Implementation ---------------- */

static int export_bundle_to_file (const char *filepath, const int *selected_cats, int *out_settings_count) {
    FILE *fp = fopen (filepath, "w");
    if (!fp) return -1;

    /* 1. Extract current layout if selected */
    char *layout_json = NULL;
    if (selected_cats[0]) {
        ddb_gtkui_t *gtkui = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
        if (gtkui && gtkui->w_get_rootwidget && gtkui->w_save_layout_to_conf_key) {
            ddb_gtkui_widget_t *root = gtkui->w_get_rootwidget ();
            if (root && root->children) {
                gtkui->w_save_layout_to_conf_key ("gtkui.layout.1.9.0", root->children);
            }
        }
        deadbeef->conf_lock ();
        const char *lconf = deadbeef->conf_get_str_fast ("gtkui.layout.1.9.0", NULL);
        if (lconf && *lconf) {
            layout_json = strdup (lconf);
        }
        deadbeef->conf_unlock ();
    }

    /* 2. Collect settings matching selected categories */
    kv_list_t exported_settings;
    kv_list_init (&exported_settings);

    deadbeef->conf_lock ();
    for (DB_conf_item_t *it = deadbeef->conf_find ("", NULL); it; it = deadbeef->conf_find ("", it)) {
        if (!it->key || !it->value) continue;
        if (key_is_excluded (it->key)) continue;

        for (size_t c = 1; c < NUM_CATEGORIES; c++) {
            if (selected_cats[c] && key_matches_category (it->key, &CATEGORIES[c])) {
                kv_list_add (&exported_settings, it->key, it->value);
                break;
            }
        }
    }
    deadbeef->conf_unlock ();

    /* 3. Output JSON bundle */
    fprintf (fp, "{\n");
    fprintf (fp, "  \"format\": \"deadbeef-layout-bundle\",\n");
    fprintf (fp, "  \"version\": 1,\n");

    time_t now = time (NULL);
    char time_str[64];
    strftime (time_str, sizeof (time_str), "%Y-%m-%dT%H:%M:%SZ", gmtime (&now));
    fprintf (fp, "  \"created_at\": \"%s\",\n", time_str);

    fprintf (fp, "  \"categories\": [\n");
    int cat_emitted = 0;
    for (size_t c = 0; c < NUM_CATEGORIES; c++) {
        if (selected_cats[c]) {
            if (cat_emitted > 0) fprintf (fp, ",\n");
            fprintf (fp, "    \"%s\"", CATEGORIES[c].id);
            cat_emitted++;
        }
    }
    fprintf (fp, "\n  ],\n");

    if (layout_json && *layout_json) {
        fprintf (fp, "  \"layout\": %s,\n", layout_json);
    } else {
        fprintf (fp, "  \"layout\": null,\n");
    }
    if (layout_json) free (layout_json);

    fprintf (fp, "  \"settings\": {\n");
    for (int i = 0; i < exported_settings.count; i++) {
        fprintf (fp, "    ");
        write_json_escaped_string (fp, exported_settings.items[i].key);
        fprintf (fp, ": ");
        write_json_escaped_string (fp, exported_settings.items[i].val);
        if (i < exported_settings.count - 1) fprintf (fp, ",");
        fprintf (fp, "\n");
    }
    fprintf (fp, "  }\n");
    fprintf (fp, "}\n");

    fclose (fp);
    if (out_settings_count) *out_settings_count = exported_settings.count;
    kv_list_free (&exported_settings);
    return 0;
}

/* ---------------- Robust JSON Parser for Layout Bundles ---------------- */

static const char *json_skip_ws (const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Parse a quoted JSON string, handling escapes. Returns pointer after closing quote. */
static const char *parse_json_str (const char *p, char **out_str) {
    *out_str = NULL;
    p = json_skip_ws (p);
    if (*p != '"') return p;
    p++; /* skip opening quote */

    size_t cap = 256;
    size_t len = 0;
    char *str = malloc (cap);

    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            p++;
            char esc = *p++;
            char out_c = esc;
            switch (esc) {
            case '"': out_c = '"'; break;
            case '\\': out_c = '\\'; break;
            case '/': out_c = '/'; break;
            case 'b': out_c = '\b'; break;
            case 'f': out_c = '\f'; break;
            case 'n': out_c = '\n'; break;
            case 'r': out_c = '\r'; break;
            case 't': out_c = '\t'; break;
            case 'u': {
                unsigned int hex = 0;
                int valid = 1;
                for (int h = 0; h < 4; h++) {
                    char hc = *p;
                    if (hc >= '0' && hc <= '9') hex = (hex << 4) | (hc - '0');
                    else if (hc >= 'a' && hc <= 'f') hex = (hex << 4) | (hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F') hex = (hex << 4) | (hc - 'A' + 10);
                    else { valid = 0; break; }
                    p++;
                }
                if (valid) {
                    if (hex < 0x80) {
                        out_c = (char)hex;
                    } else if (hex < 0x800) {
                        if (len + 2 >= cap) { cap *= 2; str = realloc (str, cap); }
                        str[len++] = (char)(0xC0 | (hex >> 6));
                        out_c = (char)(0x80 | (hex & 0x3F));
                    } else {
                        if (len + 3 >= cap) { cap *= 2; str = realloc (str, cap); }
                        str[len++] = (char)(0xE0 | (hex >> 12));
                        str[len++] = (char)(0x80 | ((hex >> 6) & 0x3F));
                        out_c = (char)(0x80 | (hex & 0x3F));
                    }
                } else {
                    out_c = '?';
                }
                break;
            }
            default:
                out_c = esc;
                break;
            }
            if (len + 1 >= cap) { cap *= 2; str = realloc (str, cap); }
            str[len++] = out_c;
        } else {
            if (len + 1 >= cap) { cap *= 2; str = realloc (str, cap); }
            str[len++] = *p++;
        }
    }
    if (*p == '"') p++;
    str[len] = '\0';
    *out_str = str;
    return p;
}

/* Skip any JSON value: string, object, array, number, boolean, null */
static const char *skip_json_value (const char *p) {
    p = json_skip_ws (p);
    if (!*p) return p;
    if (*p == '"') {
        char *dummy = NULL;
        p = parse_json_str (p, &dummy);
        free (dummy);
        return p;
    }
    if (*p == '{') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                char *dummy = NULL;
                p = parse_json_str (p, &dummy);
                free (dummy);
            } else {
                if (*p == '{') depth++;
                else if (*p == '}') {
                    depth--;
                    if (depth == 0) { p++; break; }
                }
                p++;
            }
        }
        return p;
    }
    if (*p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                char *dummy = NULL;
                p = parse_json_str (p, &dummy);
                free (dummy);
            } else {
                if (*p == '[') depth++;
                else if (*p == ']') {
                    depth--;
                    if (depth == 0) { p++; break; }
                }
                p++;
            }
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace ((unsigned char)*p)) p++;
    return p;
}

static int parse_bundle_file (const char *filepath, layout_bundle_t *bundle) {
    bundle_init (bundle);

    FILE *fp = fopen (filepath, "rb");
    if (!fp) return -1;
    fseek (fp, 0, SEEK_END);
    long sz = ftell (fp);
    fseek (fp, 0, SEEK_SET);

    if (sz <= 0 || sz > 50 * 1024 * 1024) { fclose (fp); return -1; }
    char *buf = malloc (sz + 1);
    if (!buf) { fclose (fp); return -1; }
    size_t rd = fread (buf, 1, sz, fp);
    fclose (fp);
    buf[rd] = '\0';

    const char *p = json_skip_ws (buf);
    if (*p != '{') { free (buf); return -1; }

    /* Check if raw layout */
    const char *type_pos = strstr (p, "\"type\"");
    if (type_pos && type_pos < p + 64 && !strstr (p, "\"format\"") && !strstr (p, "\"settings\"")) {
        bundle->format = strdup ("deadbeef-raw-layout");
        bundle->version = 1;
        bundle->layout_json = strdup (p);
        free (buf);
        return 0;
    }

    p++; /* skip root '{' */

    while (*(p = json_skip_ws (p))) {
        if (*p == '}') { p++; break; }
        if (*p == ',') { p++; continue; }

        if (*p != '"') {
            p++;
            continue;
        }

        char *key = NULL;
        p = parse_json_str (p, &key);
        if (!key) break;

        p = json_skip_ws (p);
        if (*p == ':') p++;
        p = json_skip_ws (p);

        if (!strcasecmp (key, "format")) {
            char *sval = NULL;
            p = parse_json_str (p, &sval);
            bundle->format = sval;
        } else if (!strcasecmp (key, "version")) {
            bundle->version = atoi (p);
            p = skip_json_value (p);
        } else if (!strcasecmp (key, "created_at")) {
            char *sval = NULL;
            p = parse_json_str (p, &sval);
            bundle->created_at = sval;
        } else if (!strcasecmp (key, "layout")) {
            if (*p == '{') {
                const char *start = p;
                int depth = 0;
                while (*p) {
                    if (*p == '"') {
                        char *dummy = NULL;
                        p = parse_json_str (p, &dummy);
                        free (dummy);
                    } else {
                        if (*p == '{') depth++;
                        else if (*p == '}') {
                            depth--;
                            if (depth == 0) { p++; break; }
                        }
                        p++;
                    }
                }
                size_t len = p - start;
                bundle->layout_json = g_strndup (start, len);
            } else {
                p = skip_json_value (p);
            }
        } else if (!strcasecmp (key, "settings")) {
            if (*p == '{') {
                p++; /* enter settings object */
                while (*(p = json_skip_ws (p))) {
                    if (*p == '}') { p++; break; }
                    if (*p == ',') { p++; continue; }
                    if (*p != '"') { p++; continue; }

                    char *skey = NULL;
                    p = parse_json_str (p, &skey);
                    if (!skey) break;

                    p = json_skip_ws (p);
                    if (*p == ':') p++;
                    p = json_skip_ws (p);

                    char *sval = NULL;
                    if (*p == '"') {
                        p = parse_json_str (p, &sval);
                    } else {
                        const char *vstart = p;
                        p = skip_json_value (p);
                        sval = g_strndup (vstart, p - vstart);
                    }
                    kv_list_add (&bundle->settings, skey, sval);
                    free (skey);
                    free (sval);
                }
            } else {
                p = skip_json_value (p);
            }
        } else {
            p = skip_json_value (p);
        }
        free (key);
    }

    free (buf);
    return 0;
}

static void init_widgets_recursive (ddb_gtkui_widget_t *w) {
    if (!w) return;
    if (w->init) {
        w->init (w);
    }
    for (ddb_gtkui_widget_t *child = w->children; child; child = child->next) {
        init_widgets_recursive (child);
    }
}

/* ---------------- Apply Bundle Implementation ---------------- */

static int apply_imported_bundle (const layout_bundle_t *bundle, const int *selected_cats, char *err_msg, size_t err_sz) {
    ddb_gtkui_t *gtkui = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);

    /* 1. Apply Settings */
    int settings_applied = 0;
    for (int i = 0; i < bundle->settings.count; i++) {
        const char *key = bundle->settings.items[i].key;
        const char *val = bundle->settings.items[i].val;
        for (size_t c = 1; c < NUM_CATEGORIES; c++) {
            if (selected_cats[c] && key_matches_category (key, &CATEGORIES[c])) {
                deadbeef->conf_set_str (key, val);
                settings_applied++;
                break;
            }
        }
    }

    /* 2. Apply Layout if selected */
    int layout_applied = 0;
    if (selected_cats[0] && bundle->layout_json && *bundle->layout_json) {
        if (gtkui && gtkui->w_load_layout_from_conf_key && gtkui->w_get_rootwidget && gtkui->w_remove && gtkui->w_destroy && gtkui->w_append) {
            ddb_gtkui_widget_t *root = gtkui->w_get_rootwidget ();
            if (root) {
                /* Step A: Backup current layout config in case new layout fails to load */
                char *backup_layout = NULL;
                deadbeef->conf_lock ();
                const char *curr_conf = deadbeef->conf_get_str_fast ("gtkui.layout.1.9.0", NULL);
                if (curr_conf && *curr_conf) {
                    backup_layout = strdup (curr_conf);
                }
                deadbeef->conf_unlock ();

                /* Step B: Explicitly remove and destroy existing layout tree.
                 * This releases all existing widgets (crucial for DDB_WF_SINGLE_INSTANCE widgets
                 * like playlist/tabbed_playlist) to prevent instance collision. */
                if (root->children) {
                    ddb_gtkui_widget_t *old_w = root->children;
                    gtkui->w_remove (root, old_w);
                    gtkui->w_destroy (old_w);
                }

                /* Step C: Construct the new layout tree from JSON */
                deadbeef->conf_set_str ("gtkui.layout_temp_import", bundle->layout_json);
                ddb_gtkui_widget_t *new_widget = gtkui->w_load_layout_from_conf_key ("gtkui.layout_temp_import");
                deadbeef->conf_remove_items ("gtkui.layout_temp_import");

                /* Step D: Attach new widget or restore backup on failure */
                if (new_widget) {
                    gtkui->w_append (root, new_widget);
                    init_widgets_recursive (new_widget);
                    if (new_widget->widget) {
                        gtk_widget_show_all (new_widget->widget);
                    }
                    if (root->widget) {
                        gtk_widget_queue_resize (root->widget);
                    }
                    deadbeef->conf_set_str ("gtkui.layout.1.9.0", bundle->layout_json);
                    layout_applied = 1;

                    if (gtkui->w_get_design_mode && gtkui->w_set_design_mode) {
                        if (gtkui->w_get_design_mode ()) {
                            gtkui->w_set_design_mode (1);
                        }
                    }
                } else {
                    /* Restore backup if new layout failed */
                    if (backup_layout) {
                        deadbeef->conf_set_str ("gtkui.layout_temp_import", backup_layout);
                        ddb_gtkui_widget_t *restore_w = gtkui->w_load_layout_from_conf_key ("gtkui.layout_temp_import");
                        deadbeef->conf_remove_items ("gtkui.layout_temp_import");
                        if (restore_w) {
                            gtkui->w_append (root, restore_w);
                            init_widgets_recursive (restore_w);
                            if (restore_w->widget) {
                                gtk_widget_show_all (restore_w->widget);
                            }
                            if (root->widget) {
                                gtk_widget_queue_resize (root->widget);
                            }
                        }
                    }
                    if (err_msg && err_sz) {
                        snprintf (err_msg, err_sz, "版面配置解析失敗（格式錯誤或包含無效元件），已還原原版面。");
                    }
                }

                if (backup_layout) free (backup_layout);
            }
        }
    }

    /* 3. Save config & broadcast changes */
    deadbeef->conf_save ();
    deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);

    if (gtkui && gtkui->get_mainwin) {
        GtkWidget *win = gtkui->get_mainwin ();
        if (win) {
            gtk_widget_queue_draw (win);
        }
    }

    return (layout_applied || settings_applied > 0) ? 0 : -1;
}

/* ---------------- GUI Dialog Callbacks ---------------- */

static void on_select_all_clicked (GtkButton *btn, gpointer user_data) {
    GtkWidget **chks = (GtkWidget **)user_data;
    for (size_t i = 0; i < NUM_CATEGORIES; i++) {
        if (chks[i] && gtk_widget_get_sensitive (chks[i])) {
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (chks[i]), TRUE);
        }
    }
}

static void on_deselect_all_clicked (GtkButton *btn, gpointer user_data) {
    GtkWidget **chks = (GtkWidget **)user_data;
    for (size_t i = 0; i < NUM_CATEGORIES; i++) {
        if (chks[i] && gtk_widget_get_sensitive (chks[i])) {
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (chks[i]), FALSE);
        }
    }
}

static gboolean export_dialog_idle_cb (gpointer user_data) {
    GtkWidget *parent = NULL;
    ddb_gtkui_t *gtkui = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (gtkui && gtkui->get_mainwin) {
        parent = gtkui->get_mainwin ();
    }

    /* Step 1: Category Selection Dialog */
    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        "匯出版面與外掛設定 (Export Layout & Settings)",
        parent ? GTK_WINDOW (parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消", GTK_RESPONSE_CANCEL,
        "下一步：儲存檔案...", GTK_RESPONSE_ACCEPT,
        NULL
    );
    gtk_window_set_default_size (GTK_WINDOW (dialog), 540, -1);
    gtk_container_set_border_width (GTK_CONTAINER (dialog), 12);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_box_set_spacing (GTK_BOX (content_area), 10);

    /* Header */
    GtkWidget *header_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title_lbl = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (title_lbl), "<span size=\"large\" weight=\"bold\">匯出 DeaDBeeF 版面配置與外掛設定</span>");
    gtk_label_set_xalign (GTK_LABEL (title_lbl), 0.0);
    gtk_box_pack_start (GTK_BOX (header_vbox), title_lbl, FALSE, FALSE, 0);

    GtkWidget *desc_lbl = gtk_label_new ("請勾選您希望包含在匯出設定檔中的項目：");
    gtk_label_set_xalign (GTK_LABEL (desc_lbl), 0.0);
    gtk_box_pack_start (GTK_BOX (header_vbox), desc_lbl, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (content_area), header_vbox, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content_area), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    /* Category Checkboxes */
    GtkWidget *list_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *chks[NUM_CATEGORIES];

    for (size_t i = 0; i < NUM_CATEGORIES; i++) {
        GtkWidget *item_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        chks[i] = gtk_check_button_new_with_label (CATEGORIES[i].title);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (chks[i]), CATEGORIES[i].def_export);
        gtk_box_pack_start (GTK_BOX (item_vbox), chks[i], FALSE, FALSE, 0);

        GtkWidget *sub_lbl = gtk_label_new (NULL);
        char markup[512];
        snprintf (markup, sizeof (markup), "<span size=\"small\" color=\"#777777\">%s</span>", CATEGORIES[i].desc);
        gtk_label_set_markup (GTK_LABEL (sub_lbl), markup);
        gtk_label_set_xalign (GTK_LABEL (sub_lbl), 0.0);
        gtk_widget_set_margin_start (sub_lbl, 24);
        gtk_box_pack_start (GTK_BOX (item_vbox), sub_lbl, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (list_vbox), item_vbox, FALSE, FALSE, 0);
    }
    gtk_box_pack_start (GTK_BOX (content_area), list_vbox, FALSE, FALSE, 0);

    /* Quick Toggle Buttons */
    GtkWidget *btn_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_all = gtk_button_new_with_label ("全選 (Select All)");
    GtkWidget *btn_none = gtk_button_new_with_label ("全部取消 (Deselect All)");
    g_signal_connect (btn_all, "clicked", G_CALLBACK (on_select_all_clicked), chks);
    g_signal_connect (btn_none, "clicked", G_CALLBACK (on_deselect_all_clicked), chks);
    gtk_box_pack_start (GTK_BOX (btn_hbox), btn_all, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (btn_hbox), btn_none, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content_area), btn_hbox, FALSE, FALSE, 4);

    gtk_widget_show_all (dialog);

    int selected_cats[NUM_CATEGORIES];
    memset (selected_cats, 0, sizeof (selected_cats));

    int res = gtk_dialog_run (GTK_DIALOG (dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        int count_selected = 0;
        for (size_t i = 0; i < NUM_CATEGORIES; i++) {
            selected_cats[i] = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (chks[i]));
            if (selected_cats[i]) count_selected++;
        }
        gtk_widget_destroy (dialog);

        if (count_selected == 0) {
            GtkWidget *warn = gtk_message_dialog_new (
                parent ? GTK_WINDOW (parent) : NULL,
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_WARNING,
                GTK_BUTTONS_OK,
                "未選取任何項目！請至少勾選一項要匯出的內容。"
            );
            gtk_window_set_title (GTK_WINDOW (warn), "提示");
            gtk_dialog_run (GTK_DIALOG (warn));
            gtk_widget_destroy (warn);
            return G_SOURCE_REMOVE;
        }

        /* Step 2: File Chooser Save Dialog */
        GtkWidget *fc = gtk_file_chooser_dialog_new (
            "儲存版面與設定檔 (Save Layout & Settings)",
            parent ? GTK_WINDOW (parent) : NULL,
            GTK_FILE_CHOOSER_ACTION_SAVE,
            "取消", GTK_RESPONSE_CANCEL,
            "儲存", GTK_RESPONSE_ACCEPT,
            NULL
        );
        gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (fc), TRUE);

        GtkFileFilter *filter_bundle = gtk_file_filter_new ();
        gtk_file_filter_set_name (filter_bundle, "DeaDBeeF 版面設定檔 (*.ddblayout, *.json)");
        gtk_file_filter_add_pattern (filter_bundle, "*.ddblayout");
        gtk_file_filter_add_pattern (filter_bundle, "*.json");
        gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (fc), filter_bundle);

        GtkFileFilter *filter_all = gtk_file_filter_new ();
        gtk_file_filter_set_name (filter_all, "所有檔案 (*.*)");
        gtk_file_filter_add_pattern (filter_all, "*");
        gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (fc), filter_all);

        time_t tnow = time (NULL);
        struct tm *tm_info = localtime (&tnow);
        char def_name[128];
        strftime (def_name, sizeof (def_name), "deadbeef_layout_%Y%m%d_%H%M%S.ddblayout", tm_info);
        gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (fc), def_name);

        if (gtk_dialog_run (GTK_DIALOG (fc)) == GTK_RESPONSE_ACCEPT) {
            char *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (fc));
            gtk_widget_destroy (fc);

            if (filename) {
                /* Append .ddblayout if no extension present */
                char final_path[1024];
                const char *dot = strrchr (filename, '.');
                if (!dot || (strcasecmp (dot, ".ddblayout") && strcasecmp (dot, ".json"))) {
                    snprintf (final_path, sizeof (final_path), "%s.ddblayout", filename);
                } else {
                    g_strlcpy (final_path, filename, sizeof (final_path));
                }

                int exported_settings = 0;
                int err = export_bundle_to_file (final_path, selected_cats, &exported_settings);
                if (err == 0) {
                    char msg[2048];
                    snprintf (msg, sizeof (msg),
                              "匯出成功！\n\n已成功將所選取的項目（版面佈局及 %d 項外掛設定）匯出至：\n%s",
                              exported_settings, final_path);
                    GtkWidget *info = gtk_message_dialog_new (
                        parent ? GTK_WINDOW (parent) : NULL,
                        GTK_DIALOG_MODAL,
                        GTK_MESSAGE_INFO,
                        GTK_BUTTONS_OK,
                        "%s", msg
                    );
                    gtk_window_set_title (GTK_WINDOW (info), "匯出完成");
                    gtk_dialog_run (GTK_DIALOG (info));
                    gtk_widget_destroy (info);
                } else {
                    GtkWidget *warn = gtk_message_dialog_new (
                        parent ? GTK_WINDOW (parent) : NULL,
                        GTK_DIALOG_MODAL,
                        GTK_MESSAGE_ERROR,
                        GTK_BUTTONS_OK,
                        "匯出失敗！無法寫入目標檔案：%s", final_path
                    );
                    gtk_window_set_title (GTK_WINDOW (warn), "匯出錯誤");
                    gtk_dialog_run (GTK_DIALOG (warn));
                    gtk_widget_destroy (warn);
                }
                g_free (filename);
            }
            return G_SOURCE_REMOVE;
        }
        gtk_widget_destroy (fc);
        return G_SOURCE_REMOVE;
    }

    gtk_widget_destroy (dialog);
    return G_SOURCE_REMOVE;
}

typedef struct {
    layout_bundle_t bundle;
    int selected_cats[NUM_CATEGORIES];
    GtkWidget *parent_window;
} import_job_t;

static gboolean deferred_apply_import_cb (gpointer user_data) {
    import_job_t *job = (import_job_t *)user_data;
    if (!job) return G_SOURCE_REMOVE;

    char err_msg[512] = "";
    int apply_res = apply_imported_bundle (&job->bundle, job->selected_cats, err_msg, sizeof (err_msg));
    GtkWidget *parent = job->parent_window;
    bundle_free (&job->bundle);
    free (job);

    if (apply_res == 0) {
        char msg[1024];
        snprintf (msg, sizeof (msg),
                  "匯入成功！\n\n已成功套用所選取的版面配置與外掛設定。%s%s",
                  (err_msg[0] ? "\n\n注意：" : ""), err_msg);
        GtkWidget *info = gtk_message_dialog_new (
            parent ? GTK_WINDOW (parent) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO,
            GTK_BUTTONS_OK,
            "%s", msg
        );
        gtk_window_set_title (GTK_WINDOW (info), "匯入完成");
        gtk_dialog_run (GTK_DIALOG (info));
        gtk_widget_destroy (info);
    } else {
        GtkWidget *err_dlg = gtk_message_dialog_new (
            parent ? GTK_WINDOW (parent) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "匯入失敗！未能成功套用設定。\n%s", err_msg
        );
        gtk_window_set_title (GTK_WINDOW (err_dlg), "匯入錯誤");
        gtk_dialog_run (GTK_DIALOG (err_dlg));
        gtk_widget_destroy (err_dlg);
    }

    return G_SOURCE_REMOVE;
}

static gboolean import_dialog_idle_cb (gpointer user_data) {
    GtkWidget *parent = NULL;
    ddb_gtkui_t *gtkui = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (gtkui && gtkui->get_mainwin) {
        parent = gtkui->get_mainwin ();
    }

    /* Step 1: File Chooser Open Dialog */
    GtkWidget *fc = gtk_file_chooser_dialog_new (
        "開啟版面與外掛設定檔 (Open Layout & Settings)",
        parent ? GTK_WINDOW (parent) : NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "取消", GTK_RESPONSE_CANCEL,
        "開啟", GTK_RESPONSE_ACCEPT,
        NULL
    );

    GtkFileFilter *filter_bundle = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_bundle, "支援的設定檔 (*.ddblayout, *.json)");
    gtk_file_filter_add_pattern (filter_bundle, "*.ddblayout");
    gtk_file_filter_add_pattern (filter_bundle, "*.json");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (fc), filter_bundle);

    GtkFileFilter *filter_all = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_all, "所有檔案 (*.*)");
    gtk_file_filter_add_pattern (filter_all, "*");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (fc), filter_all);

    if (gtk_dialog_run (GTK_DIALOG (fc)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy (fc);
        return G_SOURCE_REMOVE;
    }

    char *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (fc));
    gtk_widget_destroy (fc);
    if (!filename) return G_SOURCE_REMOVE;

    /* Step 2: Parse and inspect the bundle */
    layout_bundle_t bundle;
    int parse_err = parse_bundle_file (filename, &bundle);
    if (parse_err != 0 || (!bundle.layout_json && bundle.settings.count == 0)) {
        bundle_free (&bundle);
        GtkWidget *err_dlg = gtk_message_dialog_new (
            parent ? GTK_WINDOW (parent) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "檔案讀取失敗或格式不符！\n\n請確認此檔案為有效的 DeaDBeeF 版面與外掛設定檔。\n路徑：%s", filename
        );
        gtk_window_set_title (GTK_WINDOW (err_dlg), "匯入錯誤");
        gtk_dialog_run (GTK_DIALOG (err_dlg));
        gtk_widget_destroy (err_dlg);
        g_free (filename);
        return G_SOURCE_REMOVE;
    }

    /* Analyze presence of each category in file */
    int cat_available[NUM_CATEGORIES];
    int cat_item_count[NUM_CATEGORIES];
    int total_available = 0;

    /* Cat 0: Layout */
    cat_available[0] = (bundle.layout_json && *bundle.layout_json) ? 1 : 0;
    cat_item_count[0] = cat_available[0];
    if (cat_available[0]) total_available++;

    /* Cat 1..N: Plugin Settings */
    for (size_t c = 1; c < NUM_CATEGORIES; c++) {
        int cnt = 0;
        for (int i = 0; i < bundle.settings.count; i++) {
            if (key_matches_category (bundle.settings.items[i].key, &CATEGORIES[c])) {
                cnt++;
            }
        }
        cat_available[c] = (cnt > 0) ? 1 : 0;
        cat_item_count[c] = cnt;
        if (cat_available[c]) total_available++;
    }

    if (total_available == 0) {
        bundle_free (&bundle);
        GtkWidget *warn = gtk_message_dialog_new (
            parent ? GTK_WINDOW (parent) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_OK,
            "檔案中未包含任何可套用的版面或外掛設定項目！"
        );
        gtk_window_set_title (GTK_WINDOW (warn), "提示");
        gtk_dialog_run (GTK_DIALOG (warn));
        gtk_widget_destroy (warn);
        g_free (filename);
        return G_SOURCE_REMOVE;
    }

    /* Step 3: Selection Dialog for Import */
    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        "匯入版面與外掛設定 (Import Layout & Settings)",
        parent ? GTK_WINDOW (parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "取消", GTK_RESPONSE_CANCEL,
        "確認套用 (Apply)", GTK_RESPONSE_ACCEPT,
        NULL
    );
    gtk_window_set_default_size (GTK_WINDOW (dialog), 540, -1);
    gtk_container_set_border_width (GTK_CONTAINER (dialog), 12);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_box_set_spacing (GTK_BOX (content_area), 10);

    /* Header */
    GtkWidget *header_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title_lbl = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (title_lbl), "<span size=\"large\" weight=\"bold\">匯入 DeaDBeeF 版面配置與外掛設定</span>");
    gtk_label_set_xalign (GTK_LABEL (title_lbl), 0.0);
    gtk_box_pack_start (GTK_BOX (header_vbox), title_lbl, FALSE, FALSE, 0);

    const char *bname = g_path_get_basename (filename);
    char sub_markup[512];
    snprintf (sub_markup, sizeof (sub_markup), "來源檔案：<b>%s</b>\n請勾選您希望套用的設定項目：", bname);
    g_free ((char *)bname);
    GtkWidget *desc_lbl = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (desc_lbl), sub_markup);
    gtk_label_set_xalign (GTK_LABEL (desc_lbl), 0.0);
    gtk_box_pack_start (GTK_BOX (header_vbox), desc_lbl, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (content_area), header_vbox, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content_area), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    /* Checkboxes */
    GtkWidget *list_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *chks[NUM_CATEGORIES];

    for (size_t i = 0; i < NUM_CATEGORIES; i++) {
        GtkWidget *item_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);

        char chk_label[512];
        if (cat_available[i]) {
            if (CATEGORIES[i].is_layout) {
                snprintf (chk_label, sizeof (chk_label), "%s  [檔案中包含版面]", CATEGORIES[i].title);
            } else {
                snprintf (chk_label, sizeof (chk_label), "%s  [檔案中包含 %d 項]", CATEGORIES[i].title, cat_item_count[i]);
            }
        } else {
            snprintf (chk_label, sizeof (chk_label), "%s  (檔案中無此項)", CATEGORIES[i].title);
        }

        chks[i] = gtk_check_button_new_with_label (chk_label);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (chks[i]), cat_available[i] ? TRUE : FALSE);
        gtk_widget_set_sensitive (chks[i], cat_available[i] ? TRUE : FALSE);
        gtk_box_pack_start (GTK_BOX (item_vbox), chks[i], FALSE, FALSE, 0);

        GtkWidget *sub_lbl = gtk_label_new (NULL);
        char markup[512];
        snprintf (markup, sizeof (markup), "<span size=\"small\" color=\"#777777\">%s</span>", CATEGORIES[i].desc);
        gtk_label_set_markup (GTK_LABEL (sub_lbl), markup);
        gtk_label_set_xalign (GTK_LABEL (sub_lbl), 0.0);
        gtk_widget_set_margin_start (sub_lbl, 24);
        gtk_box_pack_start (GTK_BOX (item_vbox), sub_lbl, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (list_vbox), item_vbox, FALSE, FALSE, 0);
    }
    gtk_box_pack_start (GTK_BOX (content_area), list_vbox, FALSE, FALSE, 0);

    /* Quick Toggle Buttons */
    GtkWidget *btn_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *btn_all = gtk_button_new_with_label ("全選可用項目 (Select Available)");
    GtkWidget *btn_none = gtk_button_new_with_label ("全部取消 (Deselect All)");
    g_signal_connect (btn_all, "clicked", G_CALLBACK (on_select_all_clicked), chks);
    g_signal_connect (btn_none, "clicked", G_CALLBACK (on_deselect_all_clicked), chks);
    gtk_box_pack_start (GTK_BOX (btn_hbox), btn_all, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (btn_hbox), btn_none, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content_area), btn_hbox, FALSE, FALSE, 4);

    gtk_widget_show_all (dialog);

    int selected_cats[NUM_CATEGORIES];
    memset (selected_cats, 0, sizeof (selected_cats));

    int res = gtk_dialog_run (GTK_DIALOG (dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        int count_selected = 0;
        for (size_t i = 0; i < NUM_CATEGORIES; i++) {
            selected_cats[i] = (cat_available[i] && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (chks[i]))) ? 1 : 0;
            if (selected_cats[i]) count_selected++;
        }
        gtk_widget_destroy (dialog);

        if (count_selected == 0) {
            bundle_free (&bundle);
            g_free (filename);
            GtkWidget *warn = gtk_message_dialog_new (
                parent ? GTK_WINDOW (parent) : NULL,
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_WARNING,
                GTK_BUTTONS_OK,
                "未選取任何項目！未變更任何設定。"
            );
            gtk_window_set_title (GTK_WINDOW (warn), "提示");
            gtk_dialog_run (GTK_DIALOG (warn));
            gtk_widget_destroy (warn);
            return G_SOURCE_REMOVE;
        }

        /* Step 4: Defer apply to next main loop iteration after modal dialog has fully unwound */
        import_job_t *job = calloc (1, sizeof (import_job_t));
        job->bundle = bundle; /* Transfer bundle ownership */
        memcpy (job->selected_cats, selected_cats, sizeof (selected_cats));
        job->parent_window = parent;

        g_idle_add (deferred_apply_import_cb, job);
        g_free (filename);
        return G_SOURCE_REMOVE;
    }

    bundle_free (&bundle);
    g_free (filename);
    gtk_widget_destroy (dialog);
    return G_SOURCE_REMOVE;
}

/* ---------------- Plugin Actions & Registration ---------------- */

static int on_export_action_activate (DB_plugin_action_t *action, ddb_action_context_t ctx) {
    g_idle_add (export_dialog_idle_cb, NULL);
    return 0;
}

static int on_import_action_activate (DB_plugin_action_t *action, ddb_action_context_t ctx) {
    g_idle_add (import_dialog_idle_cb, NULL);
    return 0;
}

static DB_plugin_action_t view_export_action = {
    .title = "View/Export Layout & Settings...",
    .name = "view_export_layout_and_settings",
    .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
    .callback2 = on_export_action_activate,
    .next = NULL
};

static DB_plugin_action_t view_import_action = {
    .title = "View/Import Layout & Settings...",
    .name = "view_import_layout_and_settings",
    .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
    .callback2 = on_import_action_activate,
    .next = &view_export_action
};

static DB_plugin_action_t file_export_action = {
    .title = "File/Export Layout & Settings...",
    .name = "file_export_layout_and_settings",
    .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
    .callback2 = on_export_action_activate,
    .next = &view_import_action
};

static DB_plugin_action_t file_import_action = {
    .title = "File/Import Layout & Settings...",
    .name = "file_import_layout_and_settings",
    .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
    .callback2 = on_import_action_activate,
    .next = &file_export_action
};

static DB_plugin_action_t *layout_mgr_get_actions (DB_playItem_t *it) {
    if (!it) {
        return &file_import_action;
    }
    return NULL;
}

static int layout_mgr_connect (void) {
    return 0;
}

static int layout_mgr_disconnect (void) {
    return 0;
}

static DB_misc_t plugin = {
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_MISC,
    .plugin.id = PLUGIN_ID,
    .plugin.name = "Layout & Settings Manager",
    .plugin.descr = "Export and import Design Mode layout and plugin settings (CoverFlow, Video, Color theme, etc.) with hot-reload",
    .plugin.copyright = "GPLv2",
    .plugin.website = "https://github.com/sophAi/deadbeef-coverflow",
    .plugin.connect = layout_mgr_connect,
    .plugin.disconnect = layout_mgr_disconnect,
    .plugin.get_actions = layout_mgr_get_actions,
};

DB_plugin_t *layout_manager_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
