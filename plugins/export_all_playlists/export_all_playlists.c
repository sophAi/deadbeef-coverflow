/*
 * DeaDBeeF Export & Import All Playlists Plugin
 * 
 * Supports exporting and importing all playlist tabs into a single plain-text
 * bundle file in both Extended M3U (EXTM3U) and JSON formats.
 */

#include <gtk/gtk.h>
#include <deadbeef/deadbeef.h>
#include "plugins/gtkui/gtkui_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define PLUGIN_ID "export_all_playlists"

static DB_functions_t *deadbeef;

/* ---------------- Helper Data Structures ---------------- */

typedef struct {
    char *title;
    char **uris;
    int count;
    int capacity;
} parsed_playlist_t;

typedef struct {
    parsed_playlist_t *items;
    int count;
    int capacity;
} playlist_bundle_t;

static void bundle_init (playlist_bundle_t *bundle) {
    bundle->items = NULL;
    bundle->count = 0;
    bundle->capacity = 0;
}

static parsed_playlist_t *bundle_add_playlist (playlist_bundle_t *bundle, const char *title) {
    if (bundle->count >= bundle->capacity) {
        bundle->capacity = bundle->capacity ? bundle->capacity * 2 : 8;
        bundle->items = realloc (bundle->items, sizeof (parsed_playlist_t) * bundle->capacity);
    }
    parsed_playlist_t *plt = &bundle->items[bundle->count++];
    plt->title = strdup ((title && *title) ? title : "Untitled Playlist");
    plt->uris = NULL;
    plt->count = 0;
    plt->capacity = 0;
    return plt;
}

static void playlist_add_track (parsed_playlist_t *plt, const char *uri) {
    if (!uri || !*uri) return;
    if (plt->count >= plt->capacity) {
        plt->capacity = plt->capacity ? plt->capacity * 2 : 32;
        plt->uris = realloc (plt->uris, sizeof (char *) * plt->capacity);
    }
    plt->uris[plt->count++] = strdup (uri);
}

static void bundle_free (playlist_bundle_t *bundle) {
    if (!bundle) return;
    for (int i = 0; i < bundle->count; i++) {
        parsed_playlist_t *plt = &bundle->items[i];
        if (plt->title) free (plt->title);
        for (int j = 0; j < plt->count; j++) {
            if (plt->uris[j]) free (plt->uris[j]);
        }
        if (plt->uris) free (plt->uris);
    }
    if (bundle->items) free (bundle->items);
    bundle->items = NULL;
    bundle->count = 0;
    bundle->capacity = 0;
}

static char *trim_whitespace (char *str) {
    if (!str) return NULL;
    while (isspace ((unsigned char)*str)) str++;
    if (!*str) return str;
    char *end = str + strlen (str) - 1;
    while (end > str && isspace ((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

/* ---------------- JSON Formatting & Parsing ---------------- */

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

static int export_as_json (const char *filepath, int *out_playlists, int *out_tracks) {
    FILE *fp = fopen (filepath, "w");
    if (!fp) return -1;

    int plt_cnt = deadbeef->plt_get_count ();
    int total_tracks = 0;

    fprintf (fp, "{\n");
    fprintf (fp, "  \"format\": \"deadbeef-playlists-bundle\",\n");
    fprintf (fp, "  \"version\": 1,\n");
    fprintf (fp, "  \"playlist_count\": %d,\n", plt_cnt);
    fprintf (fp, "  \"playlists\": [\n");

    for (int i = 0; i < plt_cnt; i++) {
        ddb_playlist_t *plt = deadbeef->plt_get_for_idx (i);
        if (!plt) continue;

        char title[512] = "";
        deadbeef->plt_get_title (plt, title, sizeof (title));

        fprintf (fp, "    {\n");
        fprintf (fp, "      \"name\": ");
        write_json_escaped_string (fp, title);
        fprintf (fp, ",\n      \"tracks\": [\n");

        DB_playItem_t *it = deadbeef->plt_get_first (plt, PL_MAIN);
        int track_idx = 0;
        while (it) {
            const char *uri = deadbeef->pl_find_meta (it, ":URI");
            if (uri) {
                if (track_idx > 0) fprintf (fp, ",\n");
                fprintf (fp, "        ");
                write_json_escaped_string (fp, uri);
                track_idx++;
                total_tracks++;
            }
            DB_playItem_t *next = deadbeef->pl_get_next (it, PL_MAIN);
            deadbeef->pl_item_unref (it);
            it = next;
        }

        deadbeef->plt_unref (plt);
        fprintf (fp, "\n      ]\n    }%s\n", (i == plt_cnt - 1) ? "" : ",");
    }

    fprintf (fp, "  ]\n}\n");
    fclose (fp);

    if (out_playlists) *out_playlists = plt_cnt;
    if (out_tracks) *out_tracks = total_tracks;
    return 0;
}

/* Robust lightweight JSON tokenizer & parser for Playlist Bundle */

typedef enum {
    TOK_END,
    TOK_OBJ_START,
    TOK_OBJ_END,
    TOK_ARR_START,
    TOK_ARR_END,
    TOK_COLON,
    TOK_COMMA,
    TOK_STRING,
    TOK_VALUE
} json_tok_type_t;

static const char *json_skip_ws (const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *json_next_tok (const char *p, json_tok_type_t *type, char **val) {
    *val = NULL;
    p = json_skip_ws (p);
    if (!*p) {
        *type = TOK_END;
        return p;
    }

    char c = *p++;
    switch (c) {
    case '{': *type = TOK_OBJ_START; return p;
    case '}': *type = TOK_OBJ_END; return p;
    case '[': *type = TOK_ARR_START; return p;
    case ']': *type = TOK_ARR_END; return p;
    case ':': *type = TOK_COLON; return p;
    case ',': *type = TOK_COMMA; return p;
    case '"': {
        *type = TOK_STRING;
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
                }
                if (len + 1 >= cap) {
                    cap *= 2;
                    str = realloc (str, cap);
                }
                str[len++] = out_c;
            } else {
                if (len + 1 >= cap) {
                    cap *= 2;
                    str = realloc (str, cap);
                }
                str[len++] = *p++;
            }
        }
        if (*p == '"') p++;
        str[len] = '\0';
        *val = str;
        return p;
    }
    default: {
        *type = TOK_VALUE;
        const char *start = p - 1;
        while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
        size_t len = p - start;
        char *str = malloc (len + 1);
        memcpy (str, start, len);
        str[len] = '\0';
        *val = str;
        return p;
    }
    }
}

static int parse_json_content (const char *content, playlist_bundle_t *bundle) {
    const char *p = content;
    json_tok_type_t tok;
    char *val = NULL;

    int in_playlists_arr = 0;
    int in_playlist_obj = 0;
    int in_tracks_arr = 0;
    int in_track_obj = 0;

    parsed_playlist_t *cur_plt = NULL;
    char *pending_key = NULL;

    while (*(p = json_skip_ws (p))) {
        p = json_next_tok (p, &tok, &val);
        if (tok == TOK_END) break;

        if (tok == TOK_STRING && !in_tracks_arr) {
            if (!pending_key) {
                pending_key = val;
                val = NULL;
            } else {
                /* Key-Value pair */
                if (in_playlist_obj && (!strcasecmp (pending_key, "name") || !strcasecmp (pending_key, "title"))) {
                    if (cur_plt) {
                        free (cur_plt->title);
                        cur_plt->title = strdup (val);
                    }
                }
                free (pending_key);
                pending_key = NULL;
                free (val);
                val = NULL;
            }
        } else if (tok == TOK_COLON) {
            /* Next token will be value */
        } else if (tok == TOK_ARR_START) {
            if (pending_key && !strcasecmp (pending_key, "playlists")) {
                in_playlists_arr = 1;
            } else if (pending_key && !strcasecmp (pending_key, "tracks")) {
                in_tracks_arr = 1;
            }
            if (pending_key) {
                free (pending_key);
                pending_key = NULL;
            }
        } else if (tok == TOK_ARR_END) {
            if (in_tracks_arr) {
                in_tracks_arr = 0;
            } else if (in_playlists_arr) {
                in_playlists_arr = 0;
            }
        } else if (tok == TOK_OBJ_START) {
            if (in_tracks_arr) {
                in_track_obj = 1;
            } else if (in_playlists_arr) {
                in_playlist_obj = 1;
                cur_plt = bundle_add_playlist (bundle, "Imported Playlist");
            }
        } else if (tok == TOK_OBJ_END) {
            if (in_track_obj) {
                in_track_obj = 0;
            } else if (in_playlist_obj) {
                in_playlist_obj = 0;
            }
        } else if (tok == TOK_STRING && in_tracks_arr) {
            if (!in_track_obj) {
                /* Array of strings directly */
                if (cur_plt && val) {
                    playlist_add_track (cur_plt, val);
                }
                free (val);
                val = NULL;
            } else {
                /* Inside track object: {"uri": "..."} */
                if (!pending_key) {
                    pending_key = val;
                    val = NULL;
                } else {
                    if (cur_plt && val && (!strcasecmp (pending_key, "uri") ||
                                           !strcasecmp (pending_key, "url") ||
                                           !strcasecmp (pending_key, "path") ||
                                           !strcasecmp (pending_key, "file"))) {
                        playlist_add_track (cur_plt, val);
                    }
                    free (pending_key);
                    pending_key = NULL;
                    free (val);
                    val = NULL;
                }
            }
        } else {
            if (val) {
                free (val);
                val = NULL;
            }
        }
    }

    if (pending_key) free (pending_key);
    if (val) free (val);
    return 0;
}

/* ---------------- EXTM3U Formatting & Parsing ---------------- */

static int export_as_m3u (const char *filepath, int *out_playlists, int *out_tracks) {
    FILE *fp = fopen (filepath, "w");
    if (!fp) return -1;

    int plt_cnt = deadbeef->plt_get_count ();
    int total_tracks = 0;

    fprintf (fp, "#EXTM3U\n");
    fprintf (fp, "# Generated by DeaDBeeF Export & Import All Playlists\n\n");

    for (int i = 0; i < plt_cnt; i++) {
        ddb_playlist_t *plt = deadbeef->plt_get_for_idx (i);
        if (!plt) continue;

        char title[512] = "";
        deadbeef->plt_get_title (plt, title, sizeof (title));
        fprintf (fp, "#PLAYLIST: %s\n", title);

        DB_playItem_t *it = deadbeef->plt_get_first (plt, PL_MAIN);
        while (it) {
            const char *uri = deadbeef->pl_find_meta (it, ":URI");
            if (uri) {
                const char *artist = deadbeef->pl_find_meta (it, "artist");
                const char *track_title = deadbeef->pl_find_meta (it, "title");
                float dur = deadbeef->pl_get_item_duration (it);
                if (track_title && track_title[0]) {
                    fprintf (fp, "#EXTINF:%d,%s%s%s\n",
                             (int)dur,
                             artist ? artist : "",
                             (artist && artist[0]) ? " - " : "",
                             track_title);
                }
                fprintf (fp, "%s\n", uri);
                total_tracks++;
            }
            DB_playItem_t *next = deadbeef->pl_get_next (it, PL_MAIN);
            deadbeef->pl_item_unref (it);
            it = next;
        }
        fprintf (fp, "\n");
        deadbeef->plt_unref (plt);
    }

    fclose (fp);
    if (out_playlists) *out_playlists = plt_cnt;
    if (out_tracks) *out_tracks = total_tracks;
    return 0;
}

static int parse_m3u_file (const char *filepath, playlist_bundle_t *bundle) {
    FILE *fp = fopen (filepath, "r");
    if (!fp) return -1;

    char line[4096];
    parsed_playlist_t *cur_plt = NULL;
    int is_first_line = 1;

    while (fgets (line, sizeof (line), fp)) {
        char *p = line;
        if (is_first_line) {
            is_first_line = 0;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
                p += 3;
            }
        }
        p = trim_whitespace (p);
        if (!*p) continue;

        if (!strncasecmp (p, "#PLAYLIST:", 10)) {
            char *name = trim_whitespace (p + 10);
            cur_plt = bundle_add_playlist (bundle, (*name) ? name : "Untitled Playlist");
            continue;
        }
        if (!strncasecmp (p, "#GROUP:", 7) || !strncasecmp (p, "#NAME:", 6)) {
            char *colon = strchr (p, ':');
            char *name = trim_whitespace (colon + 1);
            cur_plt = bundle_add_playlist (bundle, (*name) ? name : "Untitled Playlist");
            continue;
        }

        if (*p == '#') {
            /* Skip comments or standard #EXTINF headers */
            continue;
        }

        /* Track URI line */
        if (!cur_plt) {
            cur_plt = bundle_add_playlist (bundle, "Imported Playlist");
        }
        playlist_add_track (cur_plt, p);
    }

    fclose (fp);
    return 0;
}

/* ---------------- File Detection & Loading ---------------- */

static int parse_bundle_file (const char *filepath, playlist_bundle_t *bundle) {
    FILE *fp = fopen (filepath, "rb");
    if (!fp) return -1;

    char sample[512] = "";
    size_t n = fread (sample, 1, sizeof (sample) - 1, fp);
    fclose (fp);
    sample[n] = '\0';

    const char *p = sample;
    /* Skip UTF-8 BOM if present */
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    while (*p && isspace ((unsigned char)*p)) p++;

    if (*p == '{' || *p == '[') {
        /* Read entire JSON file into memory buffer */
        FILE *jfp = fopen (filepath, "rb");
        if (!jfp) return -1;
        fseek (jfp, 0, SEEK_END);
        long sz = ftell (jfp);
        fseek (jfp, 0, SEEK_SET);

        char *buf = malloc (sz + 1);
        if (!buf) {
            fclose (jfp);
            return -1;
        }
        size_t rd = fread (buf, 1, sz, jfp);
        buf[rd] = '\0';
        fclose (jfp);

        const char *json_start = buf;
        if ((unsigned char)json_start[0] == 0xEF && (unsigned char)json_start[1] == 0xBB && (unsigned char)json_start[2] == 0xBF) {
            json_start += 3;
        }
        int res = parse_json_content (json_start, bundle);
        free (buf);
        return res;
    } else {
        return parse_m3u_file (filepath, bundle);
    }
}

/* ---------------- UI Callback & Workers ---------------- */

typedef struct {
    char *filename;
    int mode_replace; /* 1 = Replace all playlists, 0 = Append */
    GtkWidget *parent_win;
} import_job_t;

typedef struct {
    int success;
    int playlist_count;
    int track_count;
    char *err_msg;
    GtkWidget *parent_win;
} import_result_t;

static gboolean on_import_finished_main_thread (gpointer user_data) {
    import_result_t *res = (import_result_t *)user_data;
    if (res->success) {
        deadbeef->plt_set_curr_idx (0);
        deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CREATED, 0);
        deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);
        deadbeef->sendmessage (DB_EV_PLAYLISTSWITCHED, 0, 0, 0);
        deadbeef->conf_save ();

        char msg[256];
        snprintf (msg, sizeof (msg), "匯入完成！\n\n已成功匯入 %d 個播放清單，共載入 %d 首曲目。",
                  res->playlist_count, res->track_count);
        GtkWidget *dialog = gtk_message_dialog_new (
            res->parent_win ? GTK_WINDOW (res->parent_win) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO,
            GTK_BUTTONS_OK,
            "%s", msg
        );
        gtk_window_set_title (GTK_WINDOW (dialog), "匯入所有播放清單成功");
        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
    } else {
        GtkWidget *dialog = gtk_message_dialog_new (
            res->parent_win ? GTK_WINDOW (res->parent_win) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "匯入失敗：%s", res->err_msg ? res->err_msg : "無法解析檔案或檔案為空。"
        );
        gtk_window_set_title (GTK_WINDOW (dialog), "匯入播放清單失敗");
        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
    }

    if (res->err_msg) free (res->err_msg);
    free (res);
    return G_SOURCE_REMOVE;
}

static void import_worker_thread (void *context) {
    import_job_t *job = (import_job_t *)context;
    playlist_bundle_t bundle;
    bundle_init (&bundle);

    int err = parse_bundle_file (job->filename, &bundle);
    import_result_t *res = calloc (1, sizeof (import_result_t));
    res->parent_win = job->parent_win;

    if (err < 0 || bundle.count == 0) {
        res->success = 0;
        res->err_msg = strdup ("無法讀取檔案或未在檔案中找到任何有效的播放清單資料。");
        bundle_free (&bundle);
        free (job->filename);
        free (job);
        g_idle_add (on_import_finished_main_thread, res);
        return;
    }

    /* Apply to DeaDBeeF Playlists */
    if (job->mode_replace) {
        /* Replace mode: clear first playlist, remove all others */
        if (deadbeef->plt_get_count () > 0) {
            ddb_playlist_t *p0 = deadbeef->plt_get_for_idx (0);
            if (p0) {
                deadbeef->plt_clear (p0);
                deadbeef->plt_set_title (p0, bundle.items[0].title);
                deadbeef->plt_unref (p0);
            }
        }
        while (deadbeef->plt_get_count () > 1) {
            deadbeef->plt_remove (1);
        }
    }

    int total_tracks = 0;
    for (int i = 0; i < bundle.count; i++) {
        parsed_playlist_t *p = &bundle.items[i];
        ddb_playlist_t *plt = NULL;
        if (job->mode_replace && i == 0) {
            if (deadbeef->plt_get_count () > 0) {
                plt = deadbeef->plt_get_for_idx (0);
            } else {
                int idx = deadbeef->plt_add (0, p->title);
                plt = deadbeef->plt_get_for_idx (idx);
            }
        } else {
            int idx = deadbeef->plt_add (deadbeef->plt_get_count (), p->title);
            plt = deadbeef->plt_get_for_idx (idx);
        }

        if (plt) {
            if (!deadbeef->plt_add_files_begin (plt, 0)) {
                for (int t = 0; t < p->count; t++) {
                    deadbeef->plt_add_file2 (0, plt, p->uris[t], NULL, NULL);
                    total_tracks++;
                }
                deadbeef->plt_add_files_end (plt, 0);
            }
            deadbeef->plt_save_config (plt);
            deadbeef->plt_unref (plt);
        }
    }

    res->success = 1;
    res->playlist_count = bundle.count;
    res->track_count = total_tracks;

    bundle_free (&bundle);
    free (job->filename);
    free (job);
    g_idle_add (on_import_finished_main_thread, res);
}

static gboolean export_dialog_idle_cb (gpointer user_data) {
    GtkWidget *parent = NULL;
    ddb_gtkui_t *gtkui = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (gtkui && gtkui->get_mainwin) {
        parent = gtkui->get_mainwin ();
    }

    GtkWidget *dialog = gtk_file_chooser_dialog_new (
        "匯出所有播放清單 (Export All Playlists)",
        parent ? GTK_WINDOW (parent) : NULL,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "取消", GTK_RESPONSE_CANCEL,
        "儲存", GTK_RESPONSE_ACCEPT,
        NULL
    );

    gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);
    gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), "all_playlists.m3u8");

    /* Filter 1: M3U8 / M3U */
    GtkFileFilter *filter_m3u = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_m3u, "Extended M3U8 播放清單 (*.m3u8, *.m3u)");
    gtk_file_filter_add_pattern (filter_m3u, "*.m3u8");
    gtk_file_filter_add_pattern (filter_m3u, "*.m3u");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter_m3u);

    /* Filter 2: JSON */
    GtkFileFilter *filter_json = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_json, "JSON 播放清單封裝包 (*.json)");
    gtk_file_filter_add_pattern (filter_json, "*.json");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter_json);

    /* Filter 3: All files */
    GtkFileFilter *filter_all = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_all, "所有檔案 (*.*)");
    gtk_file_filter_add_pattern (filter_all, "*");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter_all);

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
        GtkFileFilter *selected_filter = gtk_file_chooser_get_filter (GTK_FILE_CHOOSER (dialog));

        int is_json = 0;
        if (selected_filter == filter_json) {
            is_json = 1;
        } else if (filename) {
            const char *dot = strrchr (filename, '.');
            if (dot && !strcasecmp (dot, ".json")) {
                is_json = 1;
            }
        }

        gtk_widget_destroy (dialog);

        if (filename) {
            int exported_plts = 0;
            int exported_tracks = 0;
            int err = is_json ? export_as_json (filename, &exported_plts, &exported_tracks)
                              : export_as_m3u (filename, &exported_plts, &exported_tracks);

            if (err == 0) {
                char msg[512];
                snprintf (msg, sizeof (msg),
                          "匯出成功！\n\n已將 %d 個播放清單（共 %d 首曲目）以 %s 格式匯出至：\n%s",
                          exported_plts, exported_tracks, is_json ? "JSON" : "EXTM3U", filename);
                GtkWidget *info = gtk_message_dialog_new (
                    parent ? GTK_WINDOW (parent) : NULL,
                    GTK_DIALOG_MODAL,
                    GTK_MESSAGE_INFO,
                    GTK_BUTTONS_OK,
                    "%s", msg
                );
                gtk_window_set_title (GTK_WINDOW (info), "匯出所有播放清單完成");
                gtk_dialog_run (GTK_DIALOG (info));
                gtk_widget_destroy (info);
            } else {
                GtkWidget *warn = gtk_message_dialog_new (
                    parent ? GTK_WINDOW (parent) : NULL,
                    GTK_DIALOG_MODAL,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    "匯出失敗！無法寫入目標檔案：%s", filename
                );
                gtk_window_set_title (GTK_WINDOW (warn), "匯出錯誤");
                gtk_dialog_run (GTK_DIALOG (warn));
                gtk_widget_destroy (warn);
            }
            g_free (filename);
        }
        return G_SOURCE_REMOVE;
    }

    gtk_widget_destroy (dialog);
    return G_SOURCE_REMOVE;
}

static gboolean import_dialog_idle_cb (gpointer user_data) {
    GtkWidget *parent = NULL;
    ddb_gtkui_t *gtkui = (ddb_gtkui_t *)deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (gtkui && gtkui->get_mainwin) {
        parent = gtkui->get_mainwin ();
    }

    GtkWidget *dialog = gtk_file_chooser_dialog_new (
        "匯入所有播放清單 (Import All Playlists)",
        parent ? GTK_WINDOW (parent) : NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "取消", GTK_RESPONSE_CANCEL,
        "開啟", GTK_RESPONSE_ACCEPT,
        NULL
    );

    GtkFileFilter *filter_all_sup = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_all_sup, "支援的播放清單檔案 (*.m3u8, *.m3u, *.json)");
    gtk_file_filter_add_pattern (filter_all_sup, "*.m3u8");
    gtk_file_filter_add_pattern (filter_all_sup, "*.m3u");
    gtk_file_filter_add_pattern (filter_all_sup, "*.json");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter_all_sup);

    GtkFileFilter *filter_all = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter_all, "所有檔案 (*.*)");
    gtk_file_filter_add_pattern (filter_all, "*");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter_all);

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
        gtk_widget_destroy (dialog);

        if (filename) {
            GtkWidget *ask = gtk_message_dialog_new (
                parent ? GTK_WINDOW (parent) : NULL,
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_QUESTION,
                GTK_BUTTONS_NONE,
                "匯入播放清單模式選擇\n\n請選擇您希望如何匯入此檔案中的播放清單："
            );
            gtk_dialog_add_button (GTK_DIALOG (ask), "追加至現有清單 (Append)", GTK_RESPONSE_YES);
            gtk_dialog_add_button (GTK_DIALOG (ask), "覆蓋清空現有清單 (Replace)", GTK_RESPONSE_NO);
            gtk_dialog_add_button (GTK_DIALOG (ask), "取消 (Cancel)", GTK_RESPONSE_CANCEL);

            int resp = gtk_dialog_run (GTK_DIALOG (ask));
            gtk_widget_destroy (ask);

            if (resp == GTK_RESPONSE_YES || resp == GTK_RESPONSE_NO) {
                import_job_t *job = calloc (1, sizeof (import_job_t));
                job->filename = filename;
                job->mode_replace = (resp == GTK_RESPONSE_NO);
                job->parent_win = parent;

                uintptr_t tid = deadbeef->thread_start (import_worker_thread, job);
                deadbeef->thread_detach (tid);
                return G_SOURCE_REMOVE;
            }
            g_free (filename);
        }
        return G_SOURCE_REMOVE;
    }

    gtk_widget_destroy (dialog);
    return G_SOURCE_REMOVE;
}

/* ---------------- Plugin Actions & Registration ---------------- */

static int on_export_all_playlists_activate (DB_plugin_action_t *action, ddb_action_context_t ctx) {
    g_idle_add (export_dialog_idle_cb, NULL);
    return 0;
}

static int on_import_all_playlists_activate (DB_plugin_action_t *action, ddb_action_context_t ctx) {
    g_idle_add (import_dialog_idle_cb, NULL);
    return 0;
}

static DB_plugin_action_t export_action = {
    .title = "File/Export All Playlists...",
    .name = "export_all_playlists",
    .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
    .callback2 = on_export_all_playlists_activate,
    .next = NULL
};

static DB_plugin_action_t import_action = {
    .title = "File/Import All Playlists...",
    .name = "import_all_playlists",
    .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
    .callback2 = on_import_all_playlists_activate,
    .next = &export_action
};

static DB_plugin_action_t *export_all_get_actions (DB_playItem_t *it) {
    if (!it) {
        return &import_action;
    }
    return NULL;
}

static int export_all_connect (void) {
    return 0;
}

static int export_all_disconnect (void) {
    return 0;
}

static DB_misc_t plugin = {
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_MISC,
    .plugin.id = PLUGIN_ID,
    .plugin.name = "Export & Import All Playlists",
    .plugin.descr = "Export and import all playlist tabs to a single file (supporting EXTM3U and JSON formats)",
    .plugin.copyright = "GPLv2",
    .plugin.website = "https://github.com/sophAi/deadbeef-coverflow",
    .plugin.connect = export_all_connect,
    .plugin.disconnect = export_all_disconnect,
    .plugin.get_actions = export_all_get_actions,
};

DB_plugin_t *export_all_playlists_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
