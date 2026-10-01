/*
    Cover Flow Plugin for DeaDBeeF (GTK3 / OpenGL)
    Album grouping and playlist synchronization manager
*/

#ifndef __ALBUM_MANAGER_H__
#define __ALBUM_MANAGER_H__

#include <deadbeef/deadbeef.h>
#include <gtk/gtk.h>
#include <epoxy/gl.h>
#include <stdint.h>

typedef struct {
    char *album_key;           /* "Artist - Album" unique group key */
    char *artist;              /* Artist or Album Artist */
    char *album;               /* Album title */
    char *year;                /* Release year / date */
    int *track_indices;        /* Array of playlist item indices for this album */
    int track_count;
    int track_capacity;
    DB_playItem_t *rep_track;  /* Representative track for artwork fetching (refcounted) */

    /* OpenGL texture cache */
    GLuint texture_id;         /* 0 if not yet uploaded to GPU */
    int tex_width;
    int tex_height;
    gboolean texture_loaded;   /* TRUE once query completed (even if fallback is used) */
    gboolean is_fetching;      /* TRUE while background artwork query is in flight */
    int64_t last_accessed_frame; /* For LRU texture cache eviction */
} coverflow_album_t;

typedef struct {
    coverflow_album_t *albums;
    int count;
    int capacity;
    int current_playing_album; /* Index of album currently playing, or -1 */
} album_manager_t;

void album_manager_init (album_manager_t *mgr);
void album_manager_free (album_manager_t *mgr);
void album_manager_clear (album_manager_t *mgr);
void album_manager_rebuild (album_manager_t *mgr, ddb_playlist_t *plt);
int  album_manager_find_album_for_track (album_manager_t *mgr, int track_idx);
void album_manager_free_textures (album_manager_t *mgr);

#endif /* __ALBUM_MANAGER_H__ */
