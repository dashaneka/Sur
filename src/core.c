#define _POSIX_C_SOURCE 200809L

#include "core.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <gtk/gtk.h>

/* ── Audio extensions (sorted ASCII lowercase for bsearch) ──────────────── */

static const char *const AUDIO_EXTS[] = {
    "aac", "aif", "aiff", "alac", "ape", "flac", "m4a", "mka", "mp3",
    "mpc", "oga", "ogg",  "opus", "wav", "webm", "wma", "wv",
};

static int cmp_str_ptr(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

bool sur_is_audio_path(const char *path) {
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash) || !dot[1]) return false;
    char ext[16];
    size_t i = 0;
    for (const char *p = dot + 1; *p && i < sizeof(ext) - 1; p++, i++)
        ext[i] = (char)g_ascii_tolower(*p);
    if (dot[1 + i] != '\0') return false;
    ext[i] = '\0';
    const char *key = ext;
    return bsearch(&key, AUDIO_EXTS, G_N_ELEMENTS(AUDIO_EXTS), sizeof(char *), cmp_str_ptr) != NULL;
}

int sur_natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (g_ascii_isdigit(*a) && g_ascii_isdigit(*b)) {
            const char *as = a, *bs = b;
            while (g_ascii_isdigit(*a)) a++;
            while (g_ascii_isdigit(*b)) b++;
            const char *at = as, *bt = bs;
            while (at < a && *at == '0') at++;
            while (bt < b && *bt == '0') bt++;
            size_t al = (size_t)(a - at), bl = (size_t)(b - bt);
            if (al != bl) return al < bl ? -1 : 1;
            int c = memcmp(at, bt, al);
            if (c != 0) return c < 0 ? -1 : 1;
            size_t araw = (size_t)(a - as), braw = (size_t)(b - bs);
            if (araw != braw) return araw < braw ? -1 : 1;
        } else {
            int ca = g_ascii_tolower(*a++);
            int cb = g_ascii_tolower(*b++);
            if (ca != cb) return ca < cb ? -1 : 1;
        }
    }
    return (*a ? 1 : 0) - (*b ? 1 : 0);
}

static int cmp_path_natural(const void *a, const void *b) {
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    const char *na = strrchr(pa, '/');
    const char *nb = strrchr(pb, '/');
    int r = sur_natural_cmp(na ? na + 1 : pa, nb ? nb + 1 : pb);
    return r != 0 ? r : strcmp(pa, pb);
}

char *sur_title_from_path(const char *path) {
    char *base = g_path_get_basename(path);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = '\0';
    for (char *p = base; *p; p++) {
        if (*p == '_') *p = ' ';
    }
    g_strstrip(base);
    if (!base[0]) {
        g_free(base);
        return g_strdup("Unknown Track");
    }
    return base;
}

char *sur_codec_from_path(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash) || !dot[1]) return NULL;
    return g_ascii_strup(dot + 1, -1);
}

const char *sur_short_codec_name(const char *raw) {
    if (!raw) return NULL;
    char *lo = g_ascii_strdown(raw, -1);
    const char *res = NULL;
    if (strstr(lo, "flac")) res = "FLAC";
    else if (strstr(lo, "opus")) res = "OPUS";
    else if (strstr(lo, "vorbis")) res = "OGG";
    else if (strstr(lo, "mp3") || strstr(lo, "mpeg-1 layer 3")) res = "MP3";
    else if (strstr(lo, "aac") || strstr(lo, "mpeg-4") || strstr(lo, "mp4a")) res = "M4A";
    else if (strstr(lo, "alac")) res = "ALAC";
    else if (strstr(lo, "pcm") || strstr(lo, "wav")) res = "WAV";
    g_free(lo);
    return res;
}

void sur_format_time(int64_t total_secs, char *buf, size_t len) {
    if (total_secs < 0) total_secs = 0;
    int64_t h = total_secs / 3600;
    int64_t m = (total_secs / 60) % 60;
    int64_t s = total_secs % 60;
    if (h > 0) snprintf(buf, len, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    else snprintf(buf, len, "%lld:%02lld", (long long)m, (long long)s);
}

bool sur_looks_like_url(const char *s) {
    if (!s) return false;
    while (*s && g_ascii_isspace(*s)) s++;
    return g_str_has_prefix(s, "http://") || g_str_has_prefix(s, "https://");
}

static char *folder_label(const char *path) {
    char *dir = g_path_get_dirname(path);
    const char *home = g_get_home_dir();
    if (home && strcmp(dir, home) == 0) {
        g_free(dir);
        return g_strdup("Home");
    }
    char *base = g_path_get_basename(dir);
    g_free(dir);
    return base;
}

/* ── Folder scanning ────────────────────────────────────────────────────── */

GPtrArray *sur_scan_dir(const char *dir, GCancellable *cancel) {
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return paths;
    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (cancel && g_cancellable_is_cancelled(cancel)) break;
        if (!sur_is_audio_path(name)) continue;
        char *full = g_build_filename(dir, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_REGULAR))
            g_ptr_array_add(paths, full);
        else
            g_free(full);
    }
    g_dir_close(d);
    g_ptr_array_sort(paths, cmp_path_natural);
    return paths;
}

GPtrArray *sur_expand_selection(GFile **files, int n_files, GCancellable *cancel, int *out_start) {
    if (out_start) *out_start = 0;
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    if (n_files == 1) {
        char *p = g_file_get_path(files[0]);
        if (!p) return paths;
        if (g_file_test(p, G_FILE_TEST_IS_DIR)) {
            g_ptr_array_unref(paths);
            paths = sur_scan_dir(p, cancel);
            g_free(p);
            return paths;
        }
        char *dir = g_path_get_dirname(p);
        g_ptr_array_unref(paths);
        paths = sur_scan_dir(dir, cancel);
        g_free(dir);
        bool found = false;
        for (guint i = 0; i < paths->len; i++) {
            if (strcmp(g_ptr_array_index(paths, i), p) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_ptr_array_add(paths, g_strdup(p));
            g_ptr_array_sort(paths, cmp_path_natural);
        }
        for (guint i = 0; i < paths->len; i++) {
            if (strcmp(g_ptr_array_index(paths, i), p) == 0) {
                if (out_start) *out_start = (int)i;
                break;
            }
        }
        g_free(p);
        return paths;
    }

    for (int i = 0; i < n_files; i++) {
        if (cancel && g_cancellable_is_cancelled(cancel)) break;
        char *p = g_file_get_path(files[i]);
        if (!p) continue;
        if (g_file_test(p, G_FILE_TEST_IS_DIR)) {
            GPtrArray *sub = sur_scan_dir(p, cancel);
            for (guint j = 0; j < sub->len; j++)
                g_ptr_array_add(paths, g_strdup(g_ptr_array_index(sub, j)));
            g_ptr_array_unref(sub);
            g_free(p);
        } else if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
            g_ptr_array_add(paths, p);
        } else {
            g_free(p);
        }
    }
    g_ptr_array_sort(paths, cmp_path_natural);
    /* Deduplicate sorted array */
    for (guint i = 1; i < paths->len; ) {
        if (strcmp(g_ptr_array_index(paths, i - 1), g_ptr_array_index(paths, i)) == 0)
            g_ptr_array_remove_index(paths, i);
        else
            i++;
    }
    return paths;
}

typedef struct {
    char *path;
    time_t mtime;
} TimedPath;

static int cmp_timed_desc(const void *a, const void *b) {
    const TimedPath *ta = a, *tb = b;
    if (ta->mtime != tb->mtime) return ta->mtime > tb->mtime ? -1 : 1;
    return cmp_path_natural(&ta->path, &tb->path);
}

GPtrArray *sur_downloaded_list(void) {
    char *dir = sur_download_dir();
    GPtrArray *raw = sur_scan_dir(dir, NULL);
    g_free(dir);
    if (raw->len <= 1) return raw;

    TimedPath *arr = g_new(TimedPath, raw->len);
    for (guint i = 0; i < raw->len; i++) {
        const char *p = g_ptr_array_index(raw, i);
        struct stat st;
        arr[i].path = g_strdup(p);
        arr[i].mtime = (stat(p, &st) == 0) ? st.st_mtime : 0;
    }
    guint n = raw->len;
    g_ptr_array_unref(raw);
    qsort(arr, n, sizeof(TimedPath), cmp_timed_desc);

    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < n; i++) g_ptr_array_add(out, arr[i].path);
    g_free(arr);
    return out;
}

/* ── Track & Queue ──────────────────────────────────────────────────────── */

SurTrack *sur_track_new_local(const char *path) {
    SurTrack *t = g_new0(SurTrack, 1);
    t->is_online = false;
    t->path_or_url = g_strdup(path);
    t->title = sur_title_from_path(path);
    t->codec = sur_codec_from_path(path);
    t->duration_secs = -1;
    return t;
}

SurTrack *sur_track_new_online(const char *url, const char *title, const char *artist,
                               int64_t duration_secs, const char *thumbnail) {
    SurTrack *t = g_new0(SurTrack, 1);
    t->is_online = true;
    t->path_or_url = g_strdup(url);
    t->title = g_strdup(title && title[0] ? title : "Online Track");
    t->artist = (artist && artist[0]) ? g_strdup(artist) : NULL;
    t->thumbnail = (thumbnail && thumbnail[0]) ? g_strdup(thumbnail) : NULL;
    t->codec = g_strdup("STREAM");
    t->duration_secs = duration_secs;
    return t;
}

SurTrack *sur_track_copy(const SurTrack *src) {
    if (!src) return NULL;
    SurTrack *t = g_new0(SurTrack, 1);
    t->is_online = src->is_online;
    t->path_or_url = g_strdup(src->path_or_url);
    t->thumbnail = g_strdup(src->thumbnail);
    t->title = g_strdup(src->title);
    t->artist = g_strdup(src->artist);
    t->album = g_strdup(src->album);
    t->codec = g_strdup(src->codec);
    t->duration_secs = src->duration_secs;
    return t;
}

void sur_track_free(SurTrack *t) {
    if (!t) return;
    g_free(t->path_or_url);
    g_free(t->thumbnail);
    g_free(t->title);
    g_free(t->artist);
    g_free(t->album);
    g_free(t->codec);
    g_free(t);
}

char *sur_track_subtitle(const SurTrack *t) {
    if (!t) return g_strdup("");
    bool has_a = t->artist && t->artist[0];
    bool has_b = t->album && t->album[0];
    if (has_a && has_b) return g_strdup_printf("%s — %s", t->artist, t->album);
    if (has_a) return g_strdup(t->artist);
    if (has_b) return g_strdup(t->album);
    return t->is_online ? g_strdup("Online Stream") : folder_label(t->path_or_url);
}

static uint64_t rng_next(SurQueue *q) {
    uint64_t x = q->rng ? q->rng : 1;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    q->rng = x;
    return x * 0x2545f4914f6cdd1dULL;
}

static void rebuild_shuffled(SurQueue *q, int keep_first) {
    g_array_set_size(q->order, 0);
    for (guint i = 0; i < q->tracks->len; i++) g_array_append_val(q->order, i);
    if (q->order->len <= 1) return;
    guint *o = (guint *)q->order->data;
    if (keep_first >= 0 && (guint)keep_first < q->order->len) {
        guint tmp = o[0];
        o[0] = (guint)keep_first;
        o[keep_first] = tmp;
    }
    guint start = keep_first >= 0 ? 1 : 0;
    for (guint i = q->order->len - 1; i > start; i--) {
        guint j = start + (guint)(rng_next(q) % (i - start + 1));
        guint tmp = o[i];
        o[i] = o[j];
        o[j] = tmp;
    }
}

void sur_queue_init(SurQueue *q) {
    memset(q, 0, sizeof(*q));
    q->tracks = g_ptr_array_new_with_free_func((GDestroyNotify)sur_track_free);
    q->order = g_array_new(FALSE, FALSE, sizeof(guint));
    q->pos = -1;
    q->rng = ((uint64_t)g_get_real_time() ^ 0xa0761d6478bd642fULL) | 1ULL;
}

void sur_queue_clear(SurQueue *q) {
    if (q->tracks) g_ptr_array_unref(q->tracks);
    if (q->order) g_array_unref(q->order);
    q->tracks = NULL;
    q->order = NULL;
    q->pos = -1;
}

void sur_queue_set_tracks(SurQueue *q, GPtrArray *tracks, int start_index) {
    g_ptr_array_unref(q->tracks);
    q->tracks = tracks;
    g_array_set_size(q->order, 0);
    if (q->tracks->len == 0) {
        q->pos = -1;
        return;
    }
    if (start_index < 0) start_index = 0;
    if ((guint)start_index >= q->tracks->len) start_index = (int)q->tracks->len - 1;
    if (q->shuffle) {
        rebuild_shuffled(q, start_index);
        q->pos = 0;
    } else {
        for (guint i = 0; i < q->tracks->len; i++) g_array_append_val(q->order, i);
        q->pos = start_index;
    }
}

void sur_queue_append(SurQueue *q, SurTrack *track) {
    guint idx = q->tracks->len;
    g_ptr_array_add(q->tracks, track);
    g_array_append_val(q->order, idx);
    if (q->pos < 0) q->pos = 0;
}

int sur_queue_current_track_index(const SurQueue *q) {
    if (q->pos < 0 || (guint)q->pos >= q->order->len) return -1;
    return (int)g_array_index(q->order, guint, (guint)q->pos);
}

SurTrack *sur_queue_current(const SurQueue *q) {
    int idx = sur_queue_current_track_index(q);
    return (idx >= 0 && (guint)idx < q->tracks->len) ? g_ptr_array_index(q->tracks, (guint)idx) : NULL;
}

void sur_queue_set_shuffle(SurQueue *q, bool enabled) {
    if (q->shuffle == enabled) return;
    q->shuffle = enabled;
    int cur = sur_queue_current_track_index(q);
    if (enabled) {
        rebuild_shuffled(q, cur);
        q->pos = q->tracks->len > 0 ? 0 : -1;
    } else {
        g_array_set_size(q->order, 0);
        for (guint i = 0; i < q->tracks->len; i++) g_array_append_val(q->order, i);
        q->pos = cur;
    }
}

SurRepeatMode sur_queue_cycle_repeat(SurQueue *q) {
    q->repeat = (SurRepeatMode)((q->repeat + 1) % 3);
    return q->repeat;
}

const char *sur_repeat_label(SurRepeatMode m) {
    return m == SUR_REPEAT_ONE ? "Repeat track" : (m == SUR_REPEAT_ALL ? "Repeat queue" : "Repeat off");
}

const char *sur_repeat_icon(SurRepeatMode m) {
    return m == SUR_REPEAT_ONE ? "media-playlist-repeat-song-symbolic" : "media-playlist-repeat-symbolic";
}

const char *sur_repeat_mpris(SurRepeatMode m) {
    return m == SUR_REPEAT_ONE ? "Track" : (m == SUR_REPEAT_ALL ? "Playlist" : "None");
}

SurRepeatMode sur_repeat_from_mpris(const char *s) {
    if (!s) return SUR_REPEAT_OFF;
    if (strcmp(s, "Track") == 0) return SUR_REPEAT_ONE;
    if (strcmp(s, "Playlist") == 0) return SUR_REPEAT_ALL;
    return SUR_REPEAT_OFF;
}

int sur_queue_peek_next(const SurQueue *q, bool manual) {
    if (q->pos < 0 || q->order->len == 0) return -1;
    if (!manual && q->repeat == SUR_REPEAT_ONE) return q->pos;
    if ((guint)(q->pos + 1) < q->order->len) return q->pos + 1;
    if (q->repeat == SUR_REPEAT_ALL || (manual && q->order->len > 1)) return 0;
    return -1;
}

SurTrack *sur_queue_advance(SurQueue *q, bool manual) {
    int nxt = sur_queue_peek_next(q, manual);
    if (nxt < 0) return NULL;
    q->pos = nxt;
    return sur_queue_current(q);
}

SurTrack *sur_queue_previous(SurQueue *q) {
    if (q->pos < 0 || q->order->len == 0) return NULL;
    if (q->pos > 0) q->pos--;
    else if (q->repeat == SUR_REPEAT_ALL || q->order->len > 1) q->pos = (int)q->order->len - 1;
    else return NULL;
    return sur_queue_current(q);
}

SurTrack *sur_queue_jump_order(SurQueue *q, int order_pos) {
    if (order_pos < 0 || (guint)order_pos >= q->order->len) return NULL;
    q->pos = order_pos;
    return sur_queue_current(q);
}

bool sur_queue_can_next(const SurQueue *q) { return sur_queue_peek_next(q, true) >= 0; }
bool sur_queue_can_prev(const SurQueue *q) { return q->order->len > 1; }

/* ── History & Recent ───────────────────────────────────────────────────── */

void sur_history_item_free(SurHistoryItem *item) {
    if (!item) return;
    g_free(item->path_or_url);
    g_free(item->title);
    g_free(item->artist_or_sub);
    g_free(item->thumbnail);
    g_free(item);
}

SurHistoryItem *sur_history_item_copy(const SurHistoryItem *src) {
    if (!src) return NULL;
    SurHistoryItem *c = g_new0(SurHistoryItem, 1);
    c->is_online = src->is_online;
    c->path_or_url = g_strdup(src->path_or_url);
    c->title = g_strdup(src->title);
    c->artist_or_sub = g_strdup(src->artist_or_sub);
    c->thumbnail = g_strdup(src->thumbnail);
    c->duration_secs = src->duration_secs;
    return c;
}

static char *history_file_path(void) {
    return g_build_filename(g_get_user_data_dir(), "sur", "history.tsv", NULL);
}

static char *sanitize_field(const char *raw) {
    if (!raw) return g_strdup("");
    char *s = g_strdup(raw);
    for (char *p = s; *p; p++) {
        if (*p == SUR_SEP || *p == '\n' || *p == '\r') *p = ' ';
    }
    return g_strstrip(s);
}

static char *history_key(const SurHistoryItem *item) {
    return g_strdup_printf("%c:%s", item->is_online ? 'O' : 'L', item->path_or_url);
}

char *sur_history_serialize(GPtrArray *items) {
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < items->len; i++) {
        SurHistoryItem *it = g_ptr_array_index(items, i);
        char *p = sanitize_field(it->path_or_url);
        char *t = sanitize_field(it->title);
        char *s = sanitize_field(it->artist_or_sub);
        if (!it->is_online) {
            g_string_append_printf(out, "L%c%s%c%s%c%s\n", SUR_SEP, p, SUR_SEP, t, SUR_SEP, s);
        } else {
            char *th = sanitize_field(it->thumbnail);
            if (it->duration_secs >= 0)
                g_string_append_printf(out, "O%c%s%c%s%c%s%c%lld%c%s\n",
                                       SUR_SEP, p, SUR_SEP, t, SUR_SEP, s, SUR_SEP,
                                       (long long)it->duration_secs, SUR_SEP, th);
            else
                g_string_append_printf(out, "O%c%s%c%s%c%s%c%c%s\n",
                                       SUR_SEP, p, SUR_SEP, t, SUR_SEP, s, SUR_SEP, SUR_SEP, th);
            g_free(th);
        }
        g_free(p);
        g_free(t);
        g_free(s);
    }
    return g_string_free(out, FALSE);
}

GPtrArray *sur_history_parse(const char *text) {
    GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)sur_history_item_free);
    if (!text) return items;
    char sep_str[2] = {SUR_SEP, '\0'};
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!lines[i][0]) continue;
        char **parts = g_strsplit(lines[i], sep_str, -1);
        guint n = g_strv_length(parts);
        if (n >= 4 && strcmp(parts[0], "L") == 0 && parts[1][0]) {
            SurHistoryItem *it = g_new0(SurHistoryItem, 1);
            it->is_online = false;
            it->path_or_url = g_strdup(parts[1]);
            it->title = parts[2][0] ? g_strdup(parts[2]) : sur_title_from_path(parts[1]);
            it->artist_or_sub = g_strdup(parts[3]);
            it->duration_secs = -1;
            g_ptr_array_add(items, it);
        } else if (n >= 6 && strcmp(parts[0], "O") == 0 && parts[1][0] && parts[2][0]) {
            SurHistoryItem *it = g_new0(SurHistoryItem, 1);
            it->is_online = true;
            it->path_or_url = g_strdup(parts[1]);
            it->title = g_strdup(parts[2]);
            it->artist_or_sub = parts[3][0] ? g_strdup(parts[3]) : NULL;
            it->duration_secs = parts[4][0] ? g_ascii_strtoll(parts[4], NULL, 10) : -1;
            it->thumbnail = parts[5][0] ? g_strdup(parts[5]) : NULL;
            g_ptr_array_add(items, it);
        }
        g_strfreev(parts);
    }
    g_strfreev(lines);
    return items;
}

void sur_history_prepend_file(const char *file_path, SurHistoryItem *entry) {
    char *contents = NULL;
    g_file_get_contents(file_path, &contents, NULL, NULL);
    GPtrArray *items = sur_history_parse(contents);
    g_free(contents);

    char *key = history_key(entry);
    for (guint i = 0; i < items->len; ) {
        char *ek = history_key(g_ptr_array_index(items, i));
        if (strcmp(ek, key) == 0) g_ptr_array_remove_index(items, i);
        else i++;
        g_free(ek);
    }
    g_free(key);

    g_ptr_array_insert(items, 0, entry);
    while (items->len > SUR_MAX_HISTORY) g_ptr_array_remove_index(items, items->len - 1);

    char *dir = g_path_get_dirname(file_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    char *serialized = sur_history_serialize(items);
    g_file_set_contents(file_path, serialized, -1, NULL);
    g_free(serialized);
    g_ptr_array_unref(items);
}

void sur_history_record_local(const char *path, const char *title, const char *artist) {
    GFile *f = g_file_new_for_path(path);
    char *uri = g_file_get_uri(f);
    g_object_unref(f);
    gtk_recent_manager_add_item(gtk_recent_manager_get_default(), uri);
    g_free(uri);

    SurHistoryItem *it = g_new0(SurHistoryItem, 1);
    it->is_online = false;
    it->path_or_url = g_strdup(path);
    it->title = sanitize_field(title);
    it->artist_or_sub = (artist && artist[0]) ? sanitize_field(artist) : folder_label(path);
    it->duration_secs = -1;

    char *hp = history_file_path();
    sur_history_prepend_file(hp, it);
    g_free(hp);
}

void sur_history_record_online(const char *url, const char *title, const char *artist,
                               int64_t duration_secs, const char *thumbnail) {
    SurHistoryItem *it = g_new0(SurHistoryItem, 1);
    it->is_online = true;
    it->path_or_url = sanitize_field(url);
    it->title = sanitize_field(title);
    it->artist_or_sub = (artist && artist[0]) ? sanitize_field(artist) : NULL;
    it->duration_secs = duration_secs;
    it->thumbnail = (thumbnail && thumbnail[0]) ? sanitize_field(thumbnail) : NULL;

    char *hp = history_file_path();
    sur_history_prepend_file(hp, it);
    g_free(hp);
}

void sur_history_clear(void) {
    char *hp = history_file_path();
    remove(hp);
    g_free(hp);
}

static int cmp_recent_desc(gconstpointer a, gconstpointer b) {
    GDateTime *da = gtk_recent_info_get_modified((GtkRecentInfo *)a);
    GDateTime *db = gtk_recent_info_get_modified((GtkRecentInfo *)b);
    gint64 ta = da ? g_date_time_to_unix(da) : 0;
    gint64 tb = db ? g_date_time_to_unix(db) : 0;
    return ta > tb ? -1 : (ta < tb ? 1 : 0);
}

GPtrArray *sur_history_load(size_t limit, bool include_gnome_recent) {
    char *hp = history_file_path();
    char *contents = NULL;
    g_file_get_contents(hp, &contents, NULL, NULL);
    g_free(hp);

    GPtrArray *items = sur_history_parse(contents);
    g_free(contents);

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < items->len; ) {
        SurHistoryItem *it = g_ptr_array_index(items, i);
        if (!it->is_online && !g_file_test(it->path_or_url, G_FILE_TEST_IS_REGULAR)) {
            g_ptr_array_remove_index(items, i);
            continue;
        }
        g_hash_table_add(seen, history_key(it));
        i++;
    }

    if (include_gnome_recent) {
        GList *recent = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
        recent = g_list_sort(recent, cmp_recent_desc);
        for (GList *l = recent; l && items->len < limit; l = l->next) {
            GtkRecentInfo *info = l->data;
            const char *mime = gtk_recent_info_get_mime_type(info);
            if (!mime || !g_str_has_prefix(mime, "audio/") || !gtk_recent_info_exists(info))
                continue;
            GFile *f = g_file_new_for_uri(gtk_recent_info_get_uri(info));
            char *path = g_file_get_path(f);
            g_object_unref(f);
            if (!path) continue;
            char *k = g_strdup_printf("L:%s", path);
            if (!g_hash_table_contains(seen, k)) {
                g_hash_table_add(seen, k);
                SurHistoryItem *it = g_new0(SurHistoryItem, 1);
                it->is_online = false;
                it->path_or_url = path;
                it->title = sur_title_from_path(path);
                it->artist_or_sub = folder_label(path);
                it->duration_secs = -1;
                g_ptr_array_add(items, it);
            } else {
                g_free(k);
                g_free(path);
            }
        }
        g_list_free_full(recent, (GDestroyNotify)gtk_recent_info_unref);
    }

    g_hash_table_unref(seen);
    while (items->len > limit) g_ptr_array_remove_index(items, items->len - 1);
    return items;
}

/* ── Online yt-dlp integration ──────────────────────────────────────────── */

char *sur_find_yt_dlp(void) {
    char *found = g_find_program_in_path("yt-dlp");
    if (found) return found;
    char *local = g_build_filename(g_get_home_dir(), ".local", "bin", "yt-dlp", NULL);
    if (g_file_test(local, G_FILE_TEST_IS_REGULAR)) return local;
    g_free(local);
    static const char *const FALLBACKS[] = {"/usr/local/bin/yt-dlp", "/usr/bin/yt-dlp"};
    for (size_t i = 0; i < G_N_ELEMENTS(FALLBACKS); i++) {
        if (g_file_test(FALLBACKS[i], G_FILE_TEST_IS_REGULAR))
            return g_strdup(FALLBACKS[i]);
    }
    return NULL;
}

char *sur_download_dir(void) {
    const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (music) return g_build_filename(music, "Sur", NULL);
    return g_build_filename(g_get_home_dir(), "Music", "Sur", NULL);
}

void sur_search_result_free(SurSearchResult *r) {
    if (!r) return;
    g_free(r->title);
    g_free(r->artist);
    g_free(r->page_url);
    g_free(r->thumbnail);
    g_free(r);
}

void sur_resolved_stream_free(SurResolvedStream *r) {
    if (!r) return;
    g_free(r->stream_url);
    g_free(r->title);
    g_free(r->artist);
    g_free(r->thumbnail);
    g_free(r->codec);
    g_free(r);
}

static char *clean_yt_field(const char *raw) {
    if (!raw) return NULL;
    char *s = g_strdup(raw);
    g_strstrip(s);
    if (!s[0] || strcmp(s, "NA") == 0 || strcmp(s, "None") == 0) {
        g_free(s);
        return NULL;
    }
    return s;
}

static int64_t parse_yt_dur(const char *raw) {
    char *c = clean_yt_field(raw);
    if (!c) return -1;
    char *end = NULL;
    double v = g_ascii_strtod(c, &end);
    bool ok = (end && end != c && isfinite(v) && v >= 0.0);
    g_free(c);
    return ok ? (int64_t)llround(v) : -1;
}

GPtrArray *sur_parse_search_output(const char *stdout_str) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)sur_search_result_free);
    if (!stdout_str) return out;
    char sep[2] = {SUR_SEP, '\0'};
    char **lines = g_strsplit(stdout_str, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!lines[i][0]) continue;
        char **p = g_strsplit(lines[i], sep, -1);
        guint n = g_strv_length(p);
        if (n >= 4) {
            char *title = clean_yt_field(p[0]);
            char *url = clean_yt_field(p[3]);
            if (title && url) {
                SurSearchResult *r = g_new0(SurSearchResult, 1);
                r->title = title;
                r->artist = clean_yt_field(p[1]);
                if (!r->artist) r->artist = g_strdup("Unknown Artist");
                r->duration_secs = parse_yt_dur(p[2]);
                if (!sur_looks_like_url(url)) {
                    r->page_url = g_strdup_printf("https://www.youtube.com/watch?v=%s", url);
                    g_free(url);
                } else {
                    r->page_url = url;
                }
                r->thumbnail = n >= 5 ? clean_yt_field(p[4]) : NULL;
                g_ptr_array_add(out, r);
            } else {
                g_free(title);
                g_free(url);
            }
        }
        g_strfreev(p);
    }
    g_strfreev(lines);
    return out;
}

SurResolvedStream *sur_parse_resolved_line(const char *line) {
    if (!line || !line[0]) return NULL;
    char sep[2] = {SUR_SEP, '\0'};
    char **p = g_strsplit(line, sep, -1);
    guint n = g_strv_length(p);
    SurResolvedStream *r = NULL;
    if (n >= 2) {
        char *stream_url = clean_yt_field(p[0]);
        if (stream_url && sur_looks_like_url(stream_url)) {
            r = g_new0(SurResolvedStream, 1);
            r->stream_url = stream_url;
            r->title = clean_yt_field(p[1]);
            if (!r->title) r->title = g_strdup("Online Track");
            r->artist = n >= 3 ? clean_yt_field(p[2]) : NULL;
            r->duration_secs = n >= 4 ? parse_yt_dur(p[3]) : -1;
            r->thumbnail = n >= 5 ? clean_yt_field(p[4]) : NULL;
            char *raw_codec = n >= 6 ? clean_yt_field(p[5]) : NULL;
            if (raw_codec) {
                char *dot = strchr(raw_codec, '.');
                if (dot) *dot = '\0';
                const char *sc = sur_short_codec_name(raw_codec);
                r->codec = sc ? g_strdup(sc) : g_ascii_strup(raw_codec, -1);
                g_free(raw_codec);
            }
        } else {
            g_free(stream_url);
        }
    }
    g_strfreev(p);
    return r;
}

static bool run_cmd_capture(const char *const *argv, char **out_stdout, char **out_stderr) {
    GError *err = NULL;
    int status = 0;
    bool ok = g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                           out_stdout, out_stderr, &status, &err);
    if (!ok) {
        if (out_stderr) *out_stderr = g_strdup(err ? err->message : "Spawn failed");
        g_clear_error(&err);
        return false;
    }
    return g_spawn_check_wait_status(status, NULL);
}

static char *last_line_or(const char *stderr_str, const char *fallback) {
    if (!stderr_str || !stderr_str[0]) return g_strdup(fallback);
    char *copy = g_strdup(stderr_str);
    g_strchomp(copy);
    char *nl = strrchr(copy, '\n');
    char *res = g_strdup(nl ? nl + 1 : copy);
    g_strstrip(res);
    g_free(copy);
    return res[0] ? res : (g_free(res), g_strdup(fallback));
}

GPtrArray *sur_online_search(const char *query, char **out_err) {
    char *ytdlp = sur_find_yt_dlp();
    if (!ytdlp) {
        if (out_err) *out_err = g_strdup("yt-dlp not found. Install yt-dlp to search online.");
        return NULL;
    }
    char *trimmed = g_strstrip(g_strdup(query));
    char *target = sur_looks_like_url(trimmed)
        ? g_strdup(trimmed)
        : g_strdup_printf("ytsearch10:%s", trimmed);
    char *tmpl = g_strdup_printf(
        "%%(title)s%c%%(channel,uploader,artist)s%c%%(duration)s%c%%(webpage_url,url)s%c%%(thumbnail)s",
        SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP);
    const char *argv[] = {
        ytdlp, "--flat-playlist", "--no-warnings", "--ignore-errors", "--print", tmpl, target, NULL
    };
    char *out = NULL, *err = NULL;
    bool ok = run_cmd_capture(argv, &out, &err);
    GPtrArray *results = sur_parse_search_output(out);
    if (results->len == 0 && !ok) {
        if (out_err) *out_err = last_line_or(err, "Online search failed");
        g_ptr_array_unref(results);
        results = NULL;
    }
    g_free(out);
    g_free(err);
    g_free(tmpl);
    g_free(target);
    g_free(trimmed);
    g_free(ytdlp);
    return results;
}

SurResolvedStream *sur_online_resolve(const char *page_url, char **out_err) {
    char *ytdlp = sur_find_yt_dlp();
    if (!ytdlp) {
        if (out_err) *out_err = g_strdup("yt-dlp not found.");
        return NULL;
    }
    char *tmpl = g_strdup_printf(
        "%%(url)s%c%%(title)s%c%%(artist,channel,uploader)s%c%%(duration)s%c%%(thumbnail)s%c%%(acodec,ext)s",
        SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP);
    const char *argv[] = {
        ytdlp, "-f", "bestaudio[ext=opus]/bestaudio[ext=webm]/bestaudio[ext=m4a]/bestaudio/best",
        "--no-playlist", "--no-warnings", "--print", tmpl, page_url, NULL
    };
    char *out = NULL, *err = NULL;
    bool ok = run_cmd_capture(argv, &out, &err);
    SurResolvedStream *res = NULL;
    if (ok && out) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
        res = sur_parse_resolved_line(out);
    }
    if (!res && out_err) *out_err = last_line_or(err, "Could not resolve audio stream");
    g_free(out);
    g_free(err);
    g_free(tmpl);
    g_free(ytdlp);
    return res;
}

char *sur_online_download(const char *page_url, const char *dest_dir, char **out_err) {
    char *ytdlp = sur_find_yt_dlp();
    if (!ytdlp) {
        if (out_err) *out_err = g_strdup("yt-dlp not found.");
        return NULL;
    }
    g_mkdir_with_parents(dest_dir, 0755);
    char *tmpl = g_build_filename(dest_dir, "%(title)s.%(ext)s", NULL);
    const char *argv[] = {
        ytdlp, "-f", "bestaudio[ext=opus]/bestaudio[ext=webm]/bestaudio[ext=m4a]/bestaudio",
        "-x", "--audio-format", "best", "--embed-thumbnail", "--add-metadata",
        "--no-playlist", "--no-warnings", "--print", "after_move:filepath",
        "-o", tmpl, page_url, NULL
    };
    char *out = NULL, *err = NULL;
    bool ok = run_cmd_capture(argv, &out, &err);
    char *saved = NULL;
    if (ok && out) {
        g_strchomp(out);
        char *nl = strrchr(out, '\n');
        const char *last = nl ? nl + 1 : out;
        if (last[0]) saved = g_strdup(last);
    }
    if (!saved && out_err) *out_err = last_line_or(err, "Download failed");
    g_free(out);
    g_free(err);
    g_free(tmpl);
    g_free(ytdlp);
    return saved;
}

/* ── Artwork & Adaptive Color Extraction ────────────────────────────────── */

void sur_artwork_free(SurArtwork *art) {
    if (!art) return;
    g_clear_object(&art->texture);
    g_free(art->art_uri);
    g_free(art);
}

void sur_default_palette(uint8_t accent[3], uint8_t tint[3]) {
    accent[0] = 146; accent[1] = 123; accent[2] = 255;
    tint[0] = 123; tint[1] = 97; tint[2] = 255;
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double hue2rgb(double p, double q, double t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

static void hsl_to_rgb(double h, double s, double l, uint8_t out[3]) {
    if (s <= 0.0) {
        uint8_t v = (uint8_t)llround(clampd(l * 255.0, 0.0, 255.0));
        out[0] = out[1] = out[2] = v;
        return;
    }
    double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    out[0] = (uint8_t)llround(clampd(hue2rgb(p, q, h + 1.0 / 3.0) * 255.0, 0.0, 255.0));
    out[1] = (uint8_t)llround(clampd(hue2rgb(p, q, h) * 255.0, 0.0, 255.0));
    out[2] = (uint8_t)llround(clampd(hue2rgb(p, q, h - 1.0 / 3.0) * 255.0, 0.0, 255.0));
}

void sur_extract_palette(GdkPixbuf *pb, uint8_t accent[3], uint8_t tint[3]) {
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int ch = gdk_pixbuf_get_n_channels(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    const guchar *pixels = gdk_pixbuf_read_pixels(pb);
    if (!pixels || ch < 3 || w <= 0 || h <= 0) {
        sur_default_palette(accent, tint);
        return;
    }
    int sx = w / 24; if (sx < 1) sx = 1;
    int sy = h / 24; if (sy < 1) sy = 1;
    double sr = 0, sg = 0, sb = 0, tw = 0;
    for (int y = 0; y < h; y += sy) {
        const guchar *row = pixels + y * stride;
        for (int x = 0; x < w; x += sx) {
            const guchar *px = row + x * ch;
            double r = px[0], g = px[1], b = px[2];
            double mx = fmax(r, fmax(g, b));
            double mn = fmin(r, fmin(g, b));
            double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            if (lum < 22.0 || lum > 240.0) continue;
            double c = (mx - mn) / 255.0;
            double wt = 0.2 + c * c * 3.0;
            sr += r * wt; sg += g * wt; sb += b * wt; tw += wt;
        }
    }
    if (tw <= 0.0) {
        sur_default_palette(accent, tint);
        return;
    }
    double r = sr / tw / 255.0, g = sg / tw / 255.0, b = sb / tw / 255.0;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b));
    double l = (mx + mn) * 0.5;
    double d = mx - mn, hue = 0.0, sat = 0.0;
    if (d > 1e-6) {
        sat = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
        if (mx == r) hue = (g - b) / d + (g < b ? 6.0 : 0.0);
        else if (mx == g) hue = (b - r) / d + 2.0;
        else hue = (r - g) / d + 4.0;
        hue /= 6.0;
    }
    hsl_to_rgb(hue, clampd(sat, 0.45, 0.85), clampd(l, 0.58, 0.72), accent);
    hsl_to_rgb(hue, clampd(sat, 0.35, 0.75), clampd(l, 0.42, 0.56), tint);
}

char *sur_tint_css(const uint8_t accent[3], const uint8_t tint[3]) {
    double lum = 0.2126 * accent[0] + 0.7152 * accent[1] + 0.0722 * accent[2];
    const char *fg = lum > 145.0 ? "#14131c" : "#ffffff";
    return g_strdup_printf(
        ".sur-window.sur-tinted{background-image:radial-gradient(circle at 50%% 18%%,rgba(%u,%u,%u,0.34),rgba(26,24,34,0.98) 72%%);}"
        ".sur-play-button{background-color:rgb(%u,%u,%u);color:%s;}"
        ".sur-play-button:hover{background-color:rgba(%u,%u,%u,0.9);}"
        "scale.sur-seek highlight{background-color:rgb(%u,%u,%u);}"
        ".sur-toggle-active{color:rgb(%u,%u,%u);}",
        tint[0], tint[1], tint[2],
        accent[0], accent[1], accent[2], fg,
        accent[0], accent[1], accent[2],
        accent[0], accent[1], accent[2],
        accent[0], accent[1], accent[2]);
}

SurArtwork *sur_artwork_from_bytes(const uint8_t *bytes, size_t len, const char *file_uri) {
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    bool ok = gdk_pixbuf_loader_write(loader, bytes, len, NULL);
    ok = gdk_pixbuf_loader_close(loader, NULL) && ok;
    GdkPixbuf *pb = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : NULL;
    if (!pb) {
        g_object_unref(loader);
        return NULL;
    }
    GdkPixbuf *oriented = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(loader);
    if (!oriented) return NULL;

    int w = gdk_pixbuf_get_width(oriented);
    int h = gdk_pixbuf_get_height(oriented);
    if (w <= 0 || h <= 0) {
        g_object_unref(oriented);
        return NULL;
    }
    int side = w < h ? w : h;
    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(oriented, (w - side) / 2, (h - side) / 2, side, side);
    GdkPixbuf *scaled = side != 384
        ? gdk_pixbuf_scale_simple(sub, 384, 384, GDK_INTERP_BILINEAR)
        : g_object_ref(sub);
    g_object_unref(sub);
    g_object_unref(oriented);
    if (!scaled) return NULL;

    SurArtwork *art = g_new0(SurArtwork, 1);
    sur_extract_palette(scaled, art->accent, art->tint);
    GBytes *px = gdk_pixbuf_read_pixel_bytes(scaled);
    art->texture = gdk_memory_texture_new(
        gdk_pixbuf_get_width(scaled), gdk_pixbuf_get_height(scaled),
        gdk_pixbuf_get_has_alpha(scaled) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
        px, (gsize)gdk_pixbuf_get_rowstride(scaled));
    g_bytes_unref(px);
    g_object_unref(scaled);
    art->art_uri = g_strdup(file_uri);
    return art;
}

char *sur_find_folder_cover(const char *audio_path) {
    static const char *const NAMES[] = {
        "cover.jpg", "cover.jpeg", "cover.png", "cover.webp",
        "folder.jpg", "folder.jpeg", "folder.png",
        "front.jpg", "front.png", "album.jpg", "album.png"
    };
    char *dir = g_path_get_dirname(audio_path);
    for (size_t i = 0; i < G_N_ELEMENTS(NAMES); i++) {
        char *cand = g_build_filename(dir, NAMES[i], NULL);
        if (g_file_test(cand, G_FILE_TEST_IS_REGULAR)) {
            g_free(dir);
            return cand;
        }
        g_free(cand);
    }
    /* Case-insensitive fallback (e.g. Cover.JPG, Folder.PNG). */
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const char *fn;
        while ((fn = g_dir_read_name(d)) != NULL) {
            char *lo = g_ascii_strdown(fn, -1);
            bool matched = false;
            for (size_t i = 0; i < G_N_ELEMENTS(NAMES); i++) {
                if (strcmp(lo, NAMES[i]) == 0) { matched = true; break; }
            }
            g_free(lo);
            if (matched) {
                char *cand = g_build_filename(dir, fn, NULL);
                if (g_file_test(cand, G_FILE_TEST_IS_REGULAR)) {
                    g_dir_close(d);
                    g_free(dir);
                    return cand;
                }
                g_free(cand);
            }
        }
        g_dir_close(d);
    }
    g_free(dir);
    return NULL;
}

char *sur_cache_art_bytes(const uint8_t *bytes, size_t len) {
    char *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes, len);
    if (!sum) return NULL;
    char *dir = g_build_filename(g_get_user_cache_dir(), "sur", "art", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *name = g_strdup_printf("%s.img", sum);
    char *path = g_build_filename(dir, name, NULL);
    g_free(name);
    g_free(sum);
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_file_set_contents(path, (const char *)bytes, (gssize)len, NULL);
        /* Prune cache if it exceeds SUR_MAX_ART_CACHE files */
        GDir *d = g_dir_open(dir, 0, NULL);
        if (d) {
            GArray *entries = g_array_new(FALSE, FALSE, sizeof(TimedPath));
            const char *fn;
            while ((fn = g_dir_read_name(d)) != NULL) {
                char *fp = g_build_filename(dir, fn, NULL);
                struct stat st;
                TimedPath tp = {fp, stat(fp, &st) == 0 ? st.st_mtime : 0};
                g_array_append_val(entries, tp);
            }
            g_dir_close(d);
            if (entries->len > SUR_MAX_ART_CACHE) {
                qsort(entries->data, entries->len, sizeof(TimedPath), cmp_timed_desc);
                for (guint i = SUR_MAX_ART_CACHE; i < entries->len; i++) {
                    TimedPath *tp = &g_array_index(entries, TimedPath, i);
                    remove(tp->path);
                }
            }
            for (guint i = 0; i < entries->len; i++)
                g_free(g_array_index(entries, TimedPath, i).path);
            g_array_unref(entries);
        }
    }
    g_free(dir);
    GFile *f = g_file_new_for_path(path);
    char *uri = g_file_get_uri(f);
    g_object_unref(f);
    g_free(path);
    return uri;
}

GBytes *sur_fetch_url_bytes(const char *url) {
    const char *argv[] = {"curl", "-fsSL", "--max-time", "8", "--max-filesize", "8388608", url, NULL};
    GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (!proc) return NULL;
    GBytes *out = NULL;
    bool ok = g_subprocess_communicate(proc, NULL, NULL, &out, NULL, NULL)
              && g_subprocess_get_successful(proc);
    g_object_unref(proc);
    if (!ok || !out || g_bytes_get_size(out) == 0) {
        g_clear_pointer(&out, g_bytes_unref);
        return NULL;
    }
    return out;
}
