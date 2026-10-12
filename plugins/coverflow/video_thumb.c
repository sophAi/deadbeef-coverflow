/*
    Cover Flow Plugin for DeaDBeeF (GTK3 / OpenGL)
    Video thumbnail extraction and caching utility
*/

#include "video_thumb.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

gboolean
video_thumb_is_video_uri (const char *uri) {
    if (!uri || !*uri) {
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

char *
video_thumb_get_cache_path (const char *uri) {
    if (!uri || !*uri) {
        return NULL;
    }

    const char *cache_base = g_get_user_cache_dir ();
    char *cache_dir = g_build_filename (cache_base, "deadbeef", "video_covers", NULL);
    if (!g_file_test (cache_dir, G_FILE_TEST_IS_DIR)) {
        g_mkdir_with_parents (cache_dir, 0755);
    }

    char *md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
    char *filename = g_strdup_printf ("%s.jpg", md5);
    char *full_path = g_build_filename (cache_dir, filename, NULL);

    g_free (md5);
    g_free (filename);
    g_free (cache_dir);
    return full_path;
}

gboolean
video_thumb_generate_thumbnail (const char *uri, const char *dest_path) {
    if (!uri || !dest_path) {
        return FALSE;
    }

    char *local_path = NULL;
    if (g_str_has_prefix (uri, "file://")) {
        local_path = g_filename_from_uri (uri, NULL, NULL);
    }
    const char *input_file = local_path ? local_path : uri;

    if (!g_file_test (input_file, G_FILE_TEST_EXISTS)) {
        if (local_path) {
            g_free (local_path);
        }
        return FALSE;
    }

    /* 1. Try ffmpegthumbnailer at 1.0 second */
    char *argv_thumb1[] = {
        "ffmpegthumbnailer",
        "-i", (char *)input_file,
        "-o", (char *)dest_path,
        "-s", "512",
        "-t", "00:00:01",
        NULL
    };
    int exit_status = 0;
    gboolean success = g_spawn_sync (NULL, argv_thumb1, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, NULL);
    if (!success || exit_status != 0 || !g_file_test (dest_path, G_FILE_TEST_EXISTS)) {
        /* Retry ffmpegthumbnailer at 0 second in case video is shorter than 1s */
        char *argv_thumb0[] = {
            "ffmpegthumbnailer",
            "-i", (char *)input_file,
            "-o", (char *)dest_path,
            "-s", "512",
            "-t", "0",
            NULL
        };
        exit_status = 0;
        success = g_spawn_sync (NULL, argv_thumb0, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, NULL);
    }

    /* 2. Fallback to ffmpeg if ffmpegthumbnailer failed or is unavailable */
    if (!success || exit_status != 0 || !g_file_test (dest_path, G_FILE_TEST_EXISTS)) {
        char *argv_ffmpeg1[] = {
            "ffmpeg",
            "-y",
            "-ss", "00:00:01",
            "-i", (char *)input_file,
            "-vframes", "1",
            "-vf", "scale='min(512,iw)':'min(512,ih)':force_original_aspect_ratio=decrease",
            (char *)dest_path,
            NULL
        };
        exit_status = 0;
        success = g_spawn_sync (NULL, argv_ffmpeg1, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, NULL);
        if (!success || exit_status != 0 || !g_file_test (dest_path, G_FILE_TEST_EXISTS)) {
            char *argv_ffmpeg0[] = {
                "ffmpeg",
                "-y",
                "-ss", "00:00:00",
                "-i", (char *)input_file,
                "-vframes", "1",
                "-vf", "scale='min(512,iw)':'min(512,ih)':force_original_aspect_ratio=decrease",
                (char *)dest_path,
                NULL
            };
            exit_status = 0;
            g_spawn_sync (NULL, argv_ffmpeg0, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, NULL);
        }
    }

    if (local_path) {
        g_free (local_path);
    }

    return g_file_test (dest_path, G_FILE_TEST_EXISTS);
}
