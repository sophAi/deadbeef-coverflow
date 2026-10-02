/*
    Cover Flow Plugin for DeaDBeeF (GTK3 / OpenGL)
    Album grouping and playlist synchronization manager
*/

#include "album_manager.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern DB_functions_t *deadbeef;

void
album_manager_init (album_manager_t *mgr) {
    memset (mgr, 0, sizeof (*mgr));
    mgr->current_playing_album = -1;
}

void
album_manager_queue_delete_texture (album_manager_t *mgr, GLuint tex_id) {
    if (!mgr || tex_id == 0) {
        return;
    }
    if (mgr->pending_delete_count >= mgr->pending_delete_capacity) {
        mgr->pending_delete_capacity = mgr->pending_delete_capacity < 16 ? 16 : mgr->pending_delete_capacity * 2;
        mgr->pending_delete_textures = realloc (mgr->pending_delete_textures, mgr->pending_delete_capacity * sizeof (GLuint));
    }
    mgr->pending_delete_textures[mgr->pending_delete_count++] = tex_id;
}

void
album_manager_flush_pending_deletes (album_manager_t *mgr) {
    if (!mgr || mgr->pending_delete_count <= 0) {
        return;
    }
    glDeleteTextures (mgr->pending_delete_count, mgr->pending_delete_textures);
    mgr->pending_delete_count = 0;
}

void
album_manager_free_textures (album_manager_t *mgr) {
    if (!mgr || !mgr->albums) {
        return;
    }
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->albums[i].texture_id != 0) {
            album_manager_queue_delete_texture (mgr, mgr->albums[i].texture_id);
            mgr->albums[i].texture_id = 0;
        }
        if (mgr->albums[i].pending_image_path) {
            free (mgr->albums[i].pending_image_path);
            mgr->albums[i].pending_image_path = NULL;
        }
        mgr->albums[i].texture_loaded = FALSE;
        mgr->albums[i].is_fetching = FALSE;
    }
}

void
album_manager_clear (album_manager_t *mgr) {
    if (!mgr) {
        return;
    }
    if (mgr->albums) {
        for (int i = 0; i < mgr->count; i++) {
            coverflow_album_t *al = &mgr->albums[i];
            free (al->album_key);
            free (al->artist);
            free (al->album);
            free (al->year);
            free (al->track_indices);
            if (al->rep_track) {
                deadbeef->pl_item_unref (al->rep_track);
                al->rep_track = NULL;
            }
            if (al->pending_image_path) {
                free (al->pending_image_path);
                al->pending_image_path = NULL;
            }
            if (al->texture_id != 0) {
                album_manager_queue_delete_texture (mgr, al->texture_id);
                al->texture_id = 0;
            }
        }
        free (mgr->albums);
        mgr->albums = NULL;
    }
    mgr->count = 0;
    mgr->capacity = 0;
    mgr->current_playing_album = -1;
}

void
album_manager_free (album_manager_t *mgr) {
    album_manager_clear (mgr);
    if (mgr->pending_delete_textures) {
        free (mgr->pending_delete_textures);
        mgr->pending_delete_textures = NULL;
    }
    mgr->pending_delete_count = 0;
    mgr->pending_delete_capacity = 0;
}

static void
album_add_track_index (coverflow_album_t *al, int track_idx) {
    if (al->track_count >= al->track_capacity) {
        al->track_capacity = al->track_capacity < 8 ? 8 : al->track_capacity * 2;
        al->track_indices = realloc (al->track_indices, al->track_capacity * sizeof (int));
    }
    al->track_indices[al->track_count++] = track_idx;
}

static int
album_manager_add_album (album_manager_t *mgr, const char *key, const char *artist, const char *album, const char *year, DB_playItem_t *rep_track) {
    if (mgr->count >= mgr->capacity) {
        mgr->capacity = mgr->capacity < 16 ? 16 : mgr->capacity * 2;
        mgr->albums = realloc (mgr->albums, mgr->capacity * sizeof (coverflow_album_t));
    }
    int idx = mgr->count++;
    coverflow_album_t *al = &mgr->albums[idx];
    memset (al, 0, sizeof (*al));
    al->album_key = strdup (key);
    al->artist = strdup (artist ? artist : "Unknown Artist");
    al->album = strdup (album ? album : "Unknown Album");
    al->year = year && *year ? strdup (year) : NULL;
    al->rep_track = rep_track;
    if (rep_track) {
        deadbeef->pl_item_ref (rep_track);
    }
    return idx;
}

void
album_manager_rebuild (album_manager_t *mgr, ddb_playlist_t *plt) {
    album_manager_clear (mgr);

    if (!plt) {
        return;
    }

    deadbeef->pl_lock ();

    int total_tracks = deadbeef->plt_get_item_count (plt, PL_MAIN);
    if (total_tracks <= 0) {
        deadbeef->pl_unlock ();
        return;
    }

    GHashTable *album_map = g_hash_table_new_full (g_str_hash, g_str_equal, free, NULL);

    for (int i = 0; i < total_tracks; i++) {
        DB_playItem_t *it = deadbeef->plt_get_item_for_idx (plt, i, PL_MAIN);
        if (!it) {
            continue;
        }

        const char *artist = deadbeef->pl_find_meta (it, "albumartist");
        if (!artist || !*artist) {
            artist = deadbeef->pl_find_meta (it, "artist");
        }
        if (!artist || !*artist) {
            artist = "Unknown Artist";
        }

        const char *album = deadbeef->pl_find_meta (it, "album");
        char dir_fallback[256] = {0};
        if (!album || !*album) {
            /* Try to use folder name as album fallback */
            const char *uri = deadbeef->pl_find_meta (it, ":URI");
            if (uri) {
                const char *last_slash = strrchr (uri, '/');
                if (last_slash) {
                    const char *prev_slash = last_slash - 1;
                    while (prev_slash > uri && *prev_slash != '/') {
                        prev_slash--;
                    }
                    if (*prev_slash == '/') {
                        size_t len = last_slash - prev_slash - 1;
                        if (len > 0 && len < sizeof (dir_fallback)) {
                            strncpy (dir_fallback, prev_slash + 1, len);
                            dir_fallback[len] = '\0';
                            album = dir_fallback;
                        }
                    }
                }
            }
            if (!album || !*album) {
                album = "Unknown Album";
            }
        }

        const char *year = deadbeef->pl_find_meta (it, "year");
        if (!year || !*year) {
            year = deadbeef->pl_find_meta (it, "date");
        }

        /* Group key: Only group by Album Title */
        char key_buf[512];
        g_strlcpy (key_buf, album, sizeof (key_buf));
        g_strstrip (key_buf);

        gpointer val = NULL;
        int target_idx = -1;
        if (g_hash_table_lookup_extended (album_map, key_buf, NULL, &val)) {
            target_idx = GPOINTER_TO_INT (val);
            coverflow_album_t *al = &mgr->albums[target_idx];
            /* Prefer explicit albumartist if available */
            const char *alb_artist = deadbeef->pl_find_meta (it, "albumartist");
            if (alb_artist && *alb_artist) {
                if (!al->artist || strcmp (al->artist, alb_artist) != 0) {
                    free (al->artist);
                    al->artist = strdup (alb_artist);
                }
            } else if (al->artist && strcmp (al->artist, "Various Artists") != 0 && strcmp (al->artist, artist) != 0) {
                /* Tracks under the same album have different artists */
                free (al->artist);
                al->artist = strdup ("Various Artists");
            }
        } else {
            const char *display_artist = deadbeef->pl_find_meta (it, "albumartist");
            if (!display_artist || !*display_artist) {
                display_artist = artist;
            }
            target_idx = album_manager_add_album (mgr, key_buf, display_artist, album, year, it);
            g_hash_table_insert (album_map, strdup (key_buf), GINT_TO_POINTER (target_idx));
        }

        album_add_track_index (&mgr->albums[target_idx], i);

        deadbeef->pl_item_unref (it);
    }

    g_hash_table_destroy (album_map);

    deadbeef->pl_unlock ();
}

int
album_manager_find_album_for_track (album_manager_t *mgr, int track_idx) {
    if (!mgr || track_idx < 0) {
        return -1;
    }
    for (int a = 0; a < mgr->count; a++) {
        coverflow_album_t *al = &mgr->albums[a];
        for (int t = 0; t < al->track_count; t++) {
            if (al->track_indices[t] == track_idx) {
                return a;
            }
        }
    }
    return -1;
}
