/*
    Cover Flow Plugin for DeaDBeeF (GTK3 / OpenGL)
    Video thumbnail extraction and caching utility
*/

#ifndef __VIDEO_THUMB_H__
#define __VIDEO_THUMB_H__

#include <glib.h>

G_BEGIN_DECLS

gboolean video_thumb_is_video_uri (const char *uri);
char *   video_thumb_get_cache_path (const char *uri);
gboolean video_thumb_generate_thumbnail (const char *uri, const char *dest_path);

G_END_DECLS

#endif /* __VIDEO_THUMB_H__ */
