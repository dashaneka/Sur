#ifndef SUR_CORE_H
#define SUR_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <gio/gio.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define SUR_SEP '\x1f'
#define SUR_MAX_HISTORY 50
#define SUR_MAX_ART_CACHE 50

typedef enum {
    SUR_REPEAT_OFF = 0,
    SUR_REPEAT_ALL = 1,
    SUR_REPEAT_ONE = 2,
} SurRepeatMode;

typedef struct {
    bool is_online;
    char *path_or_url;
    char *thumbnail;
    char *title;
    char *artist;
    char *album;
    char *codec;
    int64_t duration_secs; /* -1 if unknown */
} SurTrack;

typedef struct {
    GPtrArray *tracks; /* SurTrack* */
    GArray *order;     /* guint indices into tracks */
    int pos;           /* index in order, -1 if empty */
    bool shuffle;
    SurRepeatMode repeat;
    uint64_t rng;
} SurQueue;

typedef struct {
    bool is_online;
    char *path_or_url;
    char *title;
    char *artist_or_sub;
    char *thumbnail;
    int64_t duration_secs; /* -1 if unknown */
} SurHistoryItem;

typedef struct {
    char *title;
    char *artist;
    char *page_url;
    char *thumbnail;
    int64_t duration_secs;
} SurSearchResult;

typedef struct {
    char *stream_url;
    char *title;
    char *artist;
    char *thumbnail;
    char *codec;
    int64_t duration_secs;
} SurResolvedStream;

typedef struct {
    GdkTexture *texture;
    uint8_t accent[3];
    uint8_t tint[3];
    char *art_uri;
} SurArtwork;

/* String & path helpers */
int sur_natural_cmp(const char *a, const char *b);
bool sur_is_audio_path(const char *path);
char *sur_title_from_path(const char *path);
char *sur_codec_from_path(const char *path);
const char *sur_short_codec_name(const char *raw);
void sur_format_time(int64_t total_secs, char *buf, size_t len);
bool sur_looks_like_url(const char *s);

/* Folder scanning (returns GPtrArray of newly allocated char* paths) */
GPtrArray *sur_scan_dir(const char *dir, GCancellable *cancel);
GPtrArray *sur_expand_selection(GFile **files, int n_files, GCancellable *cancel, int *out_start);
GPtrArray *sur_downloaded_list(void);

/* Track & Queue */
SurTrack *sur_track_new_local(const char *path);
SurTrack *sur_track_new_online(const char *url, const char *title, const char *artist,
                               int64_t duration_secs, const char *thumbnail);
SurTrack *sur_track_copy(const SurTrack *src);
void sur_track_free(SurTrack *t);
char *sur_track_subtitle(const SurTrack *t);

void sur_queue_init(SurQueue *q);
void sur_queue_clear(SurQueue *q);
void sur_queue_set_tracks(SurQueue *q, GPtrArray *tracks, int start_index);
void sur_queue_append(SurQueue *q, SurTrack *track);
SurTrack *sur_queue_current(const SurQueue *q);
int sur_queue_current_track_index(const SurQueue *q);
void sur_queue_set_shuffle(SurQueue *q, bool enabled);
SurRepeatMode sur_queue_cycle_repeat(SurQueue *q);
const char *sur_repeat_label(SurRepeatMode m);
const char *sur_repeat_icon(SurRepeatMode m);
const char *sur_repeat_mpris(SurRepeatMode m);
SurRepeatMode sur_repeat_from_mpris(const char *s);
int sur_queue_peek_next(const SurQueue *q, bool manual);
SurTrack *sur_queue_advance(SurQueue *q, bool manual);
SurTrack *sur_queue_previous(SurQueue *q);
SurTrack *sur_queue_jump_order(SurQueue *q, int order_pos);
bool sur_queue_can_next(const SurQueue *q);
bool sur_queue_can_prev(const SurQueue *q);

/* History & Recent */
void sur_history_item_free(SurHistoryItem *item);
SurHistoryItem *sur_history_item_copy(const SurHistoryItem *src);
void sur_history_record_local(const char *path, const char *title, const char *artist);
void sur_history_record_online(const char *url, const char *title, const char *artist,
                               int64_t duration_secs, const char *thumbnail);
void sur_history_clear(void);
GPtrArray *sur_history_load(size_t limit, bool include_gnome_recent);
GPtrArray *sur_history_parse(const char *text);
char *sur_history_serialize(GPtrArray *items);
void sur_history_prepend_file(const char *file_path, SurHistoryItem *entry);

/* Online yt-dlp helpers */
char *sur_find_yt_dlp(void);
char *sur_download_dir(void);
void sur_search_result_free(SurSearchResult *r);
void sur_resolved_stream_free(SurResolvedStream *r);
GPtrArray *sur_parse_search_output(const char *stdout_str);
SurResolvedStream *sur_parse_resolved_line(const char *line);
GPtrArray *sur_online_search(const char *query, char **out_err);
SurResolvedStream *sur_online_resolve(const char *page_url, char **out_err);
char *sur_online_download(const char *page_url, const char *dest_dir, char **out_err);

/* Artwork & adaptive color palette */
void sur_artwork_free(SurArtwork *art);
void sur_default_palette(uint8_t accent[3], uint8_t tint[3]);
void sur_extract_palette(GdkPixbuf *pb, uint8_t accent[3], uint8_t tint[3]);
char *sur_tint_css(const uint8_t accent[3], const uint8_t tint[3]);
SurArtwork *sur_artwork_from_bytes(const uint8_t *bytes, size_t len, const char *file_uri);
char *sur_find_folder_cover(const char *audio_path);
char *sur_cache_art_bytes(const uint8_t *bytes, size_t len);
GBytes *sur_fetch_url_bytes(const char *url);

#endif
