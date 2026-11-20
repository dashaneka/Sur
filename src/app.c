#define _POSIX_C_SOURCE 200809L

#include "core.h"
#include "mpris.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <adwaita.h>
#include <glib-unix.h>
#include <gst/gst.h>
#include <gtk/gtk.h>

#define APP_ID "io.github.rajnish.Sur"
#define SEEK_STEP 5.0
#define VOLUME_STEP 0.05
#define RESTART_THRESHOLD 3.0
#define USER_SEEK_GRACE_US 500000
#define SEEK_TIMEOUT_US 2000000

static const char BASE_CSS[] =
    ".sur-window{background-color:#1c1b22;}"
    "headerbar.sur-header{background:transparent;border-bottom:none;box-shadow:none;}"
    ".sur-cover-frame{border-radius:16px;background-color:alpha(white,0.06);"
    "box-shadow:0 12px 28px alpha(black,0.45);border:1px solid alpha(white,0.12);}"
    ".sur-title{font-size:16px;font-weight:700;}"
    ".sur-subtitle{font-size:13px;color:alpha(white,0.68);}"
    ".sur-badge{font-size:11px;font-weight:600;padding:2px 7px;border-radius:999px;"
    "background-color:alpha(white,0.1);color:alpha(white,0.78);}"
    ".sur-play-button{min-width:50px;min-height:50px;border-radius:999px;"
    "box-shadow:0 6px 18px alpha(black,0.35);}"
    ".sur-queue-sidebar{background-color:alpha(black,0.24);border-left:1px solid alpha(white,0.08);}"
    ".sur-queue-list,.sur-online-list{background:transparent;}"
    ".sur-queue-row{border-radius:10px;padding:6px 8px;}"
    ".sur-queue-row.sur-active{background-color:alpha(white,0.12);}"
    ".sur-online-row{border-radius:12px;background-color:alpha(white,0.05);"
    "border:1px solid alpha(white,0.08);padding:8px 10px;}"
    ".sur-time{font-size:12px;color:alpha(white,0.58);}"
    ".sur-save-overlay{min-width:32px;min-height:32px;border-radius:999px;"
    "background-color:alpha(#1e1d26,0.92);border:1px solid alpha(white,0.2);"
    "box-shadow:0 4px 12px alpha(black,0.4);}";

struct SurPlayer {
    gint refcount;
    AdwApplicationWindow *window;
    GtkCssProvider *tint_provider;
    GstElement *playbin;
    guint bus_watch_id;
    guint tick_id;
    bool gst_ready;

    AdwToastOverlay *toasts;
    AdwToast *active_toast;
    AdwOverlaySplitView *split;
    GtkStack *stack;

    GtkToggleButton *search_btn;
    GtkToggleButton *history_btn;
    GtkToggleButton *queue_btn;
    GtkMenuButton *volume_btn;
    GtkScale *volume_scale;
    GtkButton *mute_btn;

    /* Start screen */
    GtkBox *home_box;
    AdwStatusPage *start_page;

    /* History / Downloaded view */
    GtkToggleButton *hist_recent_tab;
    GtkToggleButton *hist_dl_tab;
    GtkButton *hist_clear_btn;
    GtkButton *hist_play_all_btn;
    GtkLabel *hist_empty_lbl;
    GtkListBox *hist_list;

    /* Now playing */
    GtkStack *cover_stack;
    GtkPicture *cover_picture;
    GtkButton *save_stream_btn;
    GtkSpinner *loading_spinner;
    GtkLabel *title_lbl;
    GtkLabel *codec_badge;
    GtkLabel *subtitle_lbl;
    GtkScale *seek_scale;
    GtkLabel *pos_lbl;
    GtkLabel *dur_lbl;
    GtkButton *shuffle_btn;
    GtkButton *prev_btn;
    GtkButton *play_btn;
    GtkButton *next_btn;
    GtkButton *repeat_btn;

    /* Queue */
    GtkLabel *queue_count_lbl;
    GtkListBox *queue_list;

    /* Online */
    GtkSearchEntry *search_entry;
    GtkSpinner *online_spinner;
    GtkLabel *online_status;
    GtkListBox *online_list;

    /* Runtime state */
    SurQueue queue;
    SurMpris *mpris;
    bool playing;
    bool muted;
    bool stopped;
    bool has_embedded_art;
    char *art_uri;
    gint64 duration_ns; /* -1 if unknown */
    gint64 last_user_seek_us;
    double seek_target;
    gint64 seek_in_flight_us;
    bool has_queued_seek;
    double queued_seek;
    uint64_t track_gen;
    uint64_t search_gen;
    GCancellable *scan_cancel;
    GHashTable *saved_urls;
    GHashTable *dl_urls;
    GPtrArray *online_results;
};

static SurPlayer *g_player = NULL;

static SurPlayer *player_ref(SurPlayer *p) {
    if (p) g_atomic_int_inc(&p->refcount);
    return p;
}

static void player_unref(SurPlayer *p) {
    if (!p) return;
    if (g_atomic_int_dec_and_test(&p->refcount)) {
        g_clear_pointer(&p->art_uri, g_free);
        g_clear_pointer(&p->saved_urls, g_hash_table_unref);
        g_clear_pointer(&p->dl_urls, g_hash_table_unref);
        g_clear_pointer(&p->online_results, g_ptr_array_unref);
        sur_queue_clear(&p->queue);
        g_free(p);
    }
}

/* Forward declarations */
static void play_current(SurPlayer *p);
static void refresh_home(SurPlayer *p);
static void refresh_history(SurPlayer *p);
static void rebuild_queue_ui(SurPlayer *p);
static void rebuild_online_results(SurPlayer *p);
static void sync_metadata_ui(SurPlayer *p);
static void sync_active_view(SurPlayer *p);
static void open_files(SurPlayer *p, GFile **files, int n_files);
static void download_online(SurPlayer *p, const char *page_url, const char *title);

/* ── Helpers ────────────────────────────────────────────────────────────── */

static GtkButton *make_icon_btn(const char *icon, const char *tip) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_add_css_class(b, "circular");
    return GTK_BUTTON(b);
}

static GtkLabel *make_time_lbl(void) {
    GtkWidget *l = gtk_label_new("0:00");
    gtk_widget_add_css_class(l, "numeric");
    gtk_widget_add_css_class(l, "sur-time");
    return GTK_LABEL(l);
}

static void clear_listbox(GtkListBox *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
        gtk_list_box_remove(box, child);
}

static void clear_box(GtkBox *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
        gtk_box_remove(box, child);
}

static void announce(SurPlayer *p, const char *msg) {
    AdwToast *t = adw_toast_new(msg);
    adw_toast_set_use_markup(t, FALSE);
    adw_toast_set_timeout(t, 3);
    if (p->active_toast) {
        adw_toast_dismiss(p->active_toast);
        g_clear_object(&p->active_toast);
    }
    p->active_toast = g_object_ref(t);
    adw_toast_overlay_add_toast(p->toasts, t);
}

static void mpris_notify(SurPlayer *p, const char *const *props) {
    if (p->mpris) sur_mpris_changed(p->mpris, props);
}

static void close_overlays(SurPlayer *p) {
    if (gtk_toggle_button_get_active(p->search_btn))
        gtk_toggle_button_set_active(p->search_btn, FALSE);
    if (gtk_toggle_button_get_active(p->history_btn))
        gtk_toggle_button_set_active(p->history_btn, FALSE);
}

static bool has_track(const SurPlayer *p) { return p->queue.tracks && p->queue.tracks->len > 0; }

static bool seek_pending(const SurPlayer *p) {
    gint64 now = g_get_monotonic_time();
    return (p->seek_in_flight_us > 0 && now - p->seek_in_flight_us < SEEK_TIMEOUT_US)
           || p->has_queued_seek
           || (p->last_user_seek_us > 0 && now - p->last_user_seek_us < USER_SEEK_GRACE_US);
}

static double current_position(SurPlayer *p) {
    if (seek_pending(p)) return p->seek_target;
    if (!p->playbin) return 0.0;
    gint64 pos = 0;
    if (gst_element_query_position(p->playbin, GST_FORMAT_TIME, &pos) && pos >= 0)
        return (double)pos / 1e9;
    return 0.0;
}

static void apply_duration(SurPlayer *p, gint64 dur_ns) {
    p->duration_ns = dur_ns;
    double dur_s = (double)dur_ns / 1e9;
    gtk_range_set_range(GTK_RANGE(p->seek_scale), 0.0, dur_s > 1.0 ? dur_s : 1.0);
    gtk_widget_set_sensitive(GTK_WIDGET(p->seek_scale), TRUE);
    char buf[32];
    sur_format_time(dur_ns / GST_SECOND, buf, sizeof(buf));
    gtk_label_set_label(p->dur_lbl, buf);
    const char *props[] = {"Metadata", NULL};
    mpris_notify(p, props);
}

static void seek_to(SurPlayer *p, double secs) {
    if (!p->playbin) return;
    if (secs < 0.0) secs = 0.0;
    p->seek_target = secs;
    gint64 now = g_get_monotonic_time();
    if (p->seek_in_flight_us > 0 && now - p->seek_in_flight_us < SEEK_TIMEOUT_US) {
        p->has_queued_seek = true;
        p->queued_seek = secs;
        return;
    }
    p->seek_in_flight_us = now;
    if (!gst_element_seek_simple(p->playbin, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                                 (gint64)(secs * 1e9))) {
        p->seek_in_flight_us = 0;
    } else if (p->mpris) {
        sur_mpris_seeked(p->mpris, secs);
    }
}

static void jump_to(SurPlayer *p, double target) {
    if (target < 0.0) target = 0.0;
    if (p->duration_ns > 0) {
        double max_s = (double)p->duration_ns / 1e9;
        if (target > max_s) target = max_s;
    }
    p->last_user_seek_us = g_get_monotonic_time();
    p->seek_target = target;
    gtk_range_set_value(GTK_RANGE(p->seek_scale), target);
    char buf[32];
    sur_format_time((int64_t)target, buf, sizeof(buf));
    gtk_label_set_label(p->pos_lbl, buf);
    seek_to(p, target);
}

static void seek_by(SurPlayer *p, double delta) {
    if (!has_track(p)) return;
    jump_to(p, current_position(p) + delta);
}

static void set_playing(SurPlayer *p, bool playing) {
    if (playing && !has_track(p)) return;
    p->playing = playing;
    if (p->playbin) {
        GstStateChangeReturn r = gst_element_set_state(
            p->playbin, playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
        if (r == GST_STATE_CHANGE_FAILURE) {
            p->playing = false;
            announce(p, "Playback error");
            return;
        }
    }
    gtk_button_set_icon_name(p->play_btn, playing ? "media-playback-pause-symbolic"
                                                  : "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(p->play_btn), playing ? "Pause (Space)" : "Play (Space)");
    const char *props[] = {"PlaybackStatus", NULL};
    mpris_notify(p, props);
}

static void stop_playback(SurPlayer *p) {
    p->playing = false;
    if (p->playbin) gst_element_set_state(p->playbin, GST_STATE_READY);
    gtk_button_set_icon_name(p->play_btn, "media-playback-start-symbolic");
    gtk_range_set_value(GTK_RANGE(p->seek_scale), 0.0);
    gtk_label_set_label(p->pos_lbl, "0:00");
    const char *props[] = {"PlaybackStatus", NULL};
    mpris_notify(p, props);
}

static void apply_artwork(SurPlayer *p, SurArtwork *art, bool embedded) {
    if (!art) return;
    if (embedded) p->has_embedded_art = true;
    g_free(p->art_uri);
    p->art_uri = g_strdup(art->art_uri);
    gtk_picture_set_paintable(p->cover_picture, GDK_PAINTABLE(art->texture));
    gtk_stack_set_visible_child_name(p->cover_stack, "art");
    char *css = sur_tint_css(art->accent, art->tint);
    gtk_css_provider_load_from_string(p->tint_provider, css);
    g_free(css);
    const char *props[] = {"Metadata", NULL};
    mpris_notify(p, props);
}

/* ── Lazy GStreamer Initialization ──────────────────────────────────────── */

static void handle_tags(SurPlayer *p, GstTagList *tags) {
    SurTrack *t = sur_queue_current(&p->queue);
    if (!t) return;
    bool changed = false;
    if (!t->is_online) {
        char *val = NULL;
        if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &val) && val && val[0]) {
            g_strstrip(val);
            if (val[0] && g_strcmp0(t->title, val) != 0) {
                g_free(t->title);
                t->title = g_strdup(val);
                changed = true;
            }
        }
        g_clear_pointer(&val, g_free);
        if (gst_tag_list_get_string(tags, GST_TAG_ARTIST, &val) && val && val[0]) {
            g_strstrip(val);
            if (val[0] && g_strcmp0(t->artist, val) != 0) {
                g_free(t->artist);
                t->artist = g_strdup(val);
                changed = true;
            }
        }
        g_clear_pointer(&val, g_free);
        if (gst_tag_list_get_string(tags, GST_TAG_ALBUM, &val) && val && val[0]) {
            g_strstrip(val);
            if (val[0] && g_strcmp0(t->album, val) != 0) {
                g_free(t->album);
                t->album = g_strdup(val);
                changed = true;
            }
        }
        g_clear_pointer(&val, g_free);
    }
    char *codec_raw = NULL;
    if (gst_tag_list_get_string(tags, GST_TAG_AUDIO_CODEC, &codec_raw) && codec_raw) {
        const char *sc = sur_short_codec_name(codec_raw);
        if (sc && g_strcmp0(t->codec, sc) != 0) {
            g_free(t->codec);
            t->codec = g_strdup(sc);
            changed = true;
        }
    }
    g_free(codec_raw);

    if (changed) {
        sync_metadata_ui(p);
        rebuild_queue_ui(p);
        const char *props[] = {"Metadata", NULL};
        mpris_notify(p, props);
    }

    if (!p->has_embedded_art) {
        GstSample *sample = NULL;
        if (!gst_tag_list_get_sample(tags, GST_TAG_IMAGE, &sample))
            gst_tag_list_get_sample(tags, GST_TAG_PREVIEW_IMAGE, &sample);
        if (sample) {
            GstBuffer *buf = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
                char *uri = sur_cache_art_bytes(map.data, map.size);
                SurArtwork *art = sur_artwork_from_bytes(map.data, map.size, uri);
                if (art) {
                    apply_artwork(p, art, true);
                    sur_artwork_free(art);
                }
                g_free(uri);
                gst_buffer_unmap(buf, &map);
            }
            gst_sample_unref(sample);
        }
    }
}

static gboolean on_gst_bus(GstBus *bus, GstMessage *msg, gpointer ud) {
    (void)bus;
    SurPlayer *p = ud;
    if (p->stopped) return G_SOURCE_REMOVE;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gst_message_parse_error(msg, &err, NULL);
            set_playing(p, false);
            char *m = g_strdup_printf("Can't play track: %s", err ? err->message : "unknown");
            announce(p, m);
            g_free(m);
            g_clear_error(&err);
            break;
        }
        case GST_MESSAGE_EOS:
            if (sur_queue_advance(&p->queue, false)) {
                play_current(p);
            } else {
                set_playing(p, false);
                seek_to(p, 0.0);
                gtk_range_set_value(GTK_RANGE(p->seek_scale), 0.0);
                gtk_label_set_label(p->pos_lbl, "0:00");
            }
            break;
        case GST_MESSAGE_DURATION_CHANGED:
            p->duration_ns = -1;
            break;
        case GST_MESSAGE_ASYNC_DONE:
            p->seek_in_flight_us = 0;
            if (p->has_queued_seek) {
                double q = p->queued_seek;
                p->has_queued_seek = false;
                seek_to(p, q);
            }
            break;
        case GST_MESSAGE_TAG: {
            GstTagList *tags = NULL;
            gst_message_parse_tag(msg, &tags);
            if (tags) {
                handle_tags(p, tags);
                gst_tag_list_unref(tags);
            }
            break;
        }
        default: break;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_tick(gpointer ud) {
    SurPlayer *p = ud;
    if (p->stopped) return G_SOURCE_REMOVE;
    if (!p->playbin) return G_SOURCE_CONTINUE;
    if (p->duration_ns < 0) {
        gint64 dur = 0;
        if (gst_element_query_duration(p->playbin, GST_FORMAT_TIME, &dur) && dur > 0)
            apply_duration(p, dur);
    }
    if (!seek_pending(p)) {
        gint64 pos = 0;
        if (gst_element_query_position(p->playbin, GST_FORMAT_TIME, &pos) && pos >= 0) {
            gtk_range_set_value(GTK_RANGE(p->seek_scale), (double)pos / 1e9);
            char buf[32];
            sur_format_time(pos / GST_SECOND, buf, sizeof(buf));
            gtk_label_set_label(p->pos_lbl, buf);
        }
    }
    return G_SOURCE_CONTINUE;
}

static bool ensure_gst(SurPlayer *p) {
    if (p->gst_ready) return true;
    GError *err = NULL;
    if (!gst_init_check(NULL, NULL, &err)) {
        char *m = g_strdup_printf("Could not start GStreamer: %s", err ? err->message : "");
        announce(p, m);
        g_free(m);
        g_clear_error(&err);
        return false;
    }
    GstElement *sink = gst_element_factory_make("fakesink", NULL);
    p->playbin = gst_element_factory_make("playbin3", NULL);
    if (!p->playbin) {
        if (sink) gst_object_unref(sink);
        announce(p, "Missing GStreamer playbin3 plugin");
        return false;
    }
    if (sink) g_object_set(p->playbin, "video-sink", sink, NULL);
    double v = gtk_range_get_value(GTK_RANGE(p->volume_scale));
    g_object_set(p->playbin, "volume", v * v * v, "mute", p->muted, NULL);
    GstBus *bus = gst_element_get_bus(p->playbin);
    p->bus_watch_id = gst_bus_add_watch(bus, on_gst_bus, p);
    gst_object_unref(bus);
    p->tick_id = g_timeout_add(250, on_tick, p);
    p->gst_ready = true;
    return true;
}

/* ── Async Cover Art, Stream Resolution, Search, Download ───────────────── */

typedef struct {
    SurPlayer *player;
    uint64_t gen;
    char *arg;
    char *fallback_thumb;
} AsyncCtx;

static void async_ctx_free(gpointer d) {
    AsyncCtx *c = d;
    player_unref(c->player);
    g_free(c->arg);
    g_free(c->fallback_thumb);
    g_free(c);
}

typedef struct {
    GBytes *bytes;
    char *uri;
} ArtBytes;

static void art_bytes_free(gpointer d) {
    ArtBytes *a = d;
    if (!a) return;
    g_clear_pointer(&a->bytes, g_bytes_unref);
    g_free(a->uri);
    g_free(a);
}

static void folder_art_worker(GTask *task, gpointer src, gpointer td, GCancellable *c) {
    (void)src; (void)c;
    AsyncCtx *ctx = td;
    char *cover = sur_find_folder_cover(ctx->arg);
    if (!cover) { g_task_return_pointer(task, NULL, NULL); return; }
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(cover, &data, &len, NULL) && len > 0) {
        ArtBytes *ab = g_new0(ArtBytes, 1);
        ab->bytes = g_bytes_new_take(data, len);
        GFile *f = g_file_new_for_path(cover);
        ab->uri = g_file_get_uri(f);
        g_object_unref(f);
        g_task_return_pointer(task, ab, art_bytes_free);
    } else {
        g_free(data);
        g_task_return_pointer(task, NULL, NULL);
    }
    g_free(cover);
}

static void folder_art_done(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)src;
    AsyncCtx *ctx = ud;
    ArtBytes *ab = g_task_propagate_pointer(G_TASK(res), NULL);
    SurPlayer *p = ctx->player;
    if (ab && !p->stopped && p->track_gen == ctx->gen && !p->has_embedded_art) {
        gsize len = 0;
        const uint8_t *ptr = g_bytes_get_data(ab->bytes, &len);
        SurArtwork *art = sur_artwork_from_bytes(ptr, len, ab->uri);
        if (art) {
            apply_artwork(p, art, false);
            sur_artwork_free(art);
        }
    }
    art_bytes_free(ab);
}

static void remote_art_worker(GTask *task, gpointer src, gpointer td, GCancellable *c) {
    (void)src; (void)c;
    AsyncCtx *ctx = td;
    GBytes *b = sur_fetch_url_bytes(ctx->arg);
    if (!b) { g_task_return_pointer(task, NULL, NULL); return; }
    gsize len = 0;
    const uint8_t *ptr = g_bytes_get_data(b, &len);
    ArtBytes *ab = g_new0(ArtBytes, 1);
    ab->bytes = b;
    ab->uri = sur_cache_art_bytes(ptr, len);
    g_task_return_pointer(task, ab, art_bytes_free);
}

static void remote_art_done(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)src;
    AsyncCtx *ctx = ud;
    ArtBytes *ab = g_task_propagate_pointer(G_TASK(res), NULL);
    SurPlayer *p = ctx->player;
    if (ab && !p->stopped && p->track_gen == ctx->gen) {
        gsize len = 0;
        const uint8_t *ptr = g_bytes_get_data(ab->bytes, &len);
        SurArtwork *art = sur_artwork_from_bytes(ptr, len, ab->uri);
        if (art) {
            apply_artwork(p, art, true);
            sur_artwork_free(art);
        }
    }
    art_bytes_free(ab);
}

static void start_stream_uri(SurPlayer *p, const char *uri) {
    if (!ensure_gst(p)) return;
    g_object_set(p->playbin, "uri", uri, NULL);
    set_playing(p, true);
    const char *props[] = {"Metadata", "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", NULL};
    mpris_notify(p, props);
}

typedef struct {
    SurResolvedStream *stream;
    char *err;
} ResolveOut;

static void resolve_out_free(gpointer d) {
    ResolveOut *o = d;
    if (!o) return;
    sur_resolved_stream_free(o->stream);
    g_free(o->err);
    g_free(o);
}

static void resolve_worker(GTask *task, gpointer src, gpointer td, GCancellable *c) {
    (void)src; (void)c;
    AsyncCtx *ctx = td;
    ResolveOut *out = g_new0(ResolveOut, 1);
    out->stream = sur_online_resolve(ctx->arg, &out->err);
    g_task_return_pointer(task, out, resolve_out_free);
}

static void resolve_done(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)src;
    AsyncCtx *ctx = ud;
    ResolveOut *out = g_task_propagate_pointer(G_TASK(res), NULL);
    SurPlayer *p = ctx->player;
    if (!out || p->stopped || p->track_gen != ctx->gen) {
        resolve_out_free(out);
        return;
    }
    gtk_spinner_stop(p->loading_spinner);
    gtk_widget_set_visible(GTK_WIDGET(p->loading_spinner), FALSE);
    if (out->stream) {
        SurResolvedStream *rs = out->stream;
        const char *thumb = rs->thumbnail ? rs->thumbnail : ctx->fallback_thumb;
        SurTrack *cur = sur_queue_current(&p->queue);
        if (cur) {
            g_free(cur->title); cur->title = g_strdup(rs->title);
            if (rs->artist) { g_free(cur->artist); cur->artist = g_strdup(rs->artist); }
            if (rs->duration_secs >= 0) cur->duration_secs = rs->duration_secs;
            if (rs->codec) { g_free(cur->codec); cur->codec = g_strdup(rs->codec); }
        }
        sur_history_record_online(ctx->arg, rs->title, rs->artist, rs->duration_secs, thumb);
        refresh_home(p);
        refresh_history(p);
        sync_metadata_ui(p);
        rebuild_queue_ui(p);
        start_stream_uri(p, rs->stream_url);
        if (thumb && thumb[0]) {
            AsyncCtx *ac = g_new0(AsyncCtx, 1);
            ac->player = player_ref(p);
            ac->gen = ctx->gen;
            ac->arg = g_strdup(thumb);
            GTask *t = g_task_new(NULL, NULL, remote_art_done, ac);
            g_task_set_task_data(t, ac, async_ctx_free);
            g_task_run_in_thread(t, remote_art_worker);
            g_object_unref(t);
        }
    } else {
        char *m = g_strdup_printf("Stream failed: %s", out->err ? out->err : "unknown");
        announce(p, m);
        g_free(m);
        set_playing(p, false);
    }
    resolve_out_free(out);
}

/* ── Core Playback Flow ─────────────────────────────────────────────────── */

static void update_nav_buttons(SurPlayer *p) {
    gtk_widget_set_sensitive(GTK_WIDGET(p->prev_btn), sur_queue_can_prev(&p->queue) || has_track(p));
    gtk_widget_set_sensitive(GTK_WIDGET(p->next_btn), sur_queue_can_next(&p->queue));
}

static void sync_active_view(SurPlayer *p) {
    if (gtk_toggle_button_get_active(p->search_btn)) {
        gtk_widget_remove_css_class(GTK_WIDGET(p->window), "sur-tinted");
        gtk_stack_set_visible_child_name(p->stack, "online");
        gtk_widget_grab_focus(GTK_WIDGET(p->search_entry));
    } else if (gtk_toggle_button_get_active(p->history_btn)) {
        gtk_widget_remove_css_class(GTK_WIDGET(p->window), "sur-tinted");
        gtk_stack_set_visible_child_name(p->stack, "history");
    } else if (has_track(p)) {
        gtk_widget_add_css_class(GTK_WIDGET(p->window), "sur-tinted");
        gtk_stack_set_visible_child_name(p->stack, "playing");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(p->window), "sur-tinted");
        gtk_stack_set_visible_child_name(p->stack, "start");
    }
}

static void sync_metadata_ui(SurPlayer *p) {
    SurTrack *t = sur_queue_current(&p->queue);
    if (!t) {
        gtk_window_set_title(GTK_WINDOW(p->window), "Sur");
        gtk_widget_set_visible(GTK_WIDGET(p->save_stream_btn), FALSE);
        return;
    }
    char *wt = g_strdup_printf("%s — Sur", t->title);
    gtk_window_set_title(GTK_WINDOW(p->window), wt);
    g_free(wt);
    gtk_label_set_label(p->title_lbl, t->title);
    char *sub = sur_track_subtitle(t);
    gtk_label_set_label(p->subtitle_lbl, sub);
    gtk_widget_set_visible(GTK_WIDGET(p->subtitle_lbl), sub[0] != '\0');
    g_free(sub);
    if (t->codec && t->codec[0]) {
        gtk_label_set_label(p->codec_badge, t->codec);
        gtk_widget_set_visible(GTK_WIDGET(p->codec_badge), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(p->codec_badge), FALSE);
    }
    if (t->is_online) {
        bool saved = g_hash_table_contains(p->saved_urls, t->path_or_url);
        bool busy = g_hash_table_contains(p->dl_urls, t->path_or_url);
        gtk_widget_set_visible(GTK_WIDGET(p->save_stream_btn), TRUE);
        gtk_widget_set_sensitive(GTK_WIDGET(p->save_stream_btn), !saved && !busy);
        gtk_button_set_icon_name(p->save_stream_btn, saved ? "emblem-ok-symbolic" : "document-save-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(p->save_stream_btn),
                                    saved ? "Saved to ~/Music/Sur"
                                          : (busy ? "Downloading…" : "Save best audio to ~/Music/Sur"));
    } else {
        gtk_widget_set_visible(GTK_WIDGET(p->save_stream_btn), FALSE);
    }
}

static void play_current(SurPlayer *p) {
    SurTrack *t = sur_queue_current(&p->queue);
    if (!t) return;
    p->track_gen++;
    uint64_t gen = p->track_gen;
    p->duration_ns = -1;
    p->has_embedded_art = false;
    g_clear_pointer(&p->art_uri, g_free);
    p->seek_in_flight_us = 0;
    p->has_queued_seek = false;
    p->last_user_seek_us = 0;

    if (p->playbin) gst_element_set_state(p->playbin, GST_STATE_READY);

    /* Reset track visuals */
    gtk_stack_set_visible_child_name(p->cover_stack, "placeholder");
    gtk_picture_set_paintable(p->cover_picture, NULL);
    uint8_t acc[3], tint[3];
    sur_default_palette(acc, tint);
    char *css = sur_tint_css(acc, tint);
    gtk_css_provider_load_from_string(p->tint_provider, css);
    g_free(css);
    gtk_spinner_stop(p->loading_spinner);
    gtk_widget_set_visible(GTK_WIDGET(p->loading_spinner), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(p->seek_scale), FALSE);
    gtk_range_set_range(GTK_RANGE(p->seek_scale), 0.0, 1.0);
    gtk_range_set_value(GTK_RANGE(p->seek_scale), 0.0);
    gtk_label_set_label(p->pos_lbl, "0:00");
    char dbuf[32] = "0:00";
    if (t->duration_secs >= 0) sur_format_time(t->duration_secs, dbuf, sizeof(dbuf));
    gtk_label_set_label(p->dur_lbl, dbuf);
    sync_metadata_ui(p);
    sync_active_view(p);
    rebuild_queue_ui(p);
    update_nav_buttons(p);

    if (!t->is_online) {
        GFile *f = g_file_new_for_path(t->path_or_url);
        char *uri = g_file_get_uri(f);
        g_object_unref(f);
        sur_history_record_local(t->path_or_url, t->title, t->artist);
        refresh_home(p);
        refresh_history(p);
        start_stream_uri(p, uri);
        g_free(uri);

        AsyncCtx *ac = g_new0(AsyncCtx, 1);
        ac->player = player_ref(p);
        ac->gen = gen;
        ac->arg = g_strdup(t->path_or_url);
        GTask *task = g_task_new(NULL, NULL, folder_art_done, ac);
        g_task_set_task_data(task, ac, async_ctx_free);
        g_task_run_in_thread(task, folder_art_worker);
        g_object_unref(task);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(p->loading_spinner), TRUE);
        gtk_spinner_start(p->loading_spinner);
        AsyncCtx *ac = g_new0(AsyncCtx, 1);
        ac->player = player_ref(p);
        ac->gen = gen;
        ac->arg = g_strdup(t->path_or_url);
        ac->fallback_thumb = g_strdup(t->thumbnail);
        GTask *task = g_task_new(NULL, NULL, resolve_done, ac);
        g_task_set_task_data(task, ac, async_ctx_free);
        g_task_run_in_thread(task, resolve_worker);
        g_object_unref(task);
    }
}

/* ── Open files / downloaded / history ──────────────────────────────────── */

typedef struct {
    SurPlayer *player;
    GFile **files;
    int n_files;
    int start_index;
} ExpandCtx;

static void expand_ctx_free(gpointer d) {
    ExpandCtx *c = d;
    player_unref(c->player);
    for (int i = 0; i < c->n_files; i++) g_object_unref(c->files[i]);
    g_free(c->files);
    g_free(c);
}

static void expand_worker(GTask *task, gpointer src, gpointer td, GCancellable *cancel) {
    (void)src;
    ExpandCtx *ctx = td;
    GPtrArray *paths = sur_expand_selection(ctx->files, ctx->n_files, cancel, &ctx->start_index);
    g_task_return_pointer(task, paths, (GDestroyNotify)g_ptr_array_unref);
}

static void expand_done(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)src;
    ExpandCtx *ctx = ud;
    GCancellable *c = g_task_get_cancellable(G_TASK(res));
    GPtrArray *paths = g_task_propagate_pointer(G_TASK(res), NULL);
    SurPlayer *p = ctx->player;
    if (!paths || p->stopped || (c && g_cancellable_is_cancelled(c))) {
        g_clear_pointer(&paths, g_ptr_array_unref);
        return;
    }
    g_clear_object(&p->scan_cancel);
    if (paths->len == 0) {
        announce(p, "No audio files found");
        g_ptr_array_unref(paths);
        return;
    }
    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)sur_track_free);
    for (guint i = 0; i < paths->len; i++)
        g_ptr_array_add(tracks, sur_track_new_local(g_ptr_array_index(paths, i)));
    g_ptr_array_unref(paths);
    sur_queue_set_tracks(&p->queue, tracks, ctx->start_index);
    close_overlays(p);
    play_current(p);
}

static void play_online_url(SurPlayer *p, const char *url) {
    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)sur_track_free);
    g_ptr_array_add(tracks, sur_track_new_online(url, "Loading stream…", NULL, -1, NULL));
    sur_queue_set_tracks(&p->queue, tracks, 0);
    close_overlays(p);
    play_current(p);
}

static void open_files(SurPlayer *p, GFile **files, int n_files) {
    if (n_files <= 0) return;
    if (n_files == 1) {
        char *path = g_file_get_path(files[0]);
        if (!path) {
            char *uri = g_file_get_uri(files[0]);
            bool is_url = sur_looks_like_url(uri);
            if (is_url) play_online_url(p, uri);
            g_free(uri);
            if (is_url) return;
        }
        g_free(path);
    }
    if (p->scan_cancel) {
        g_cancellable_cancel(p->scan_cancel);
        g_clear_object(&p->scan_cancel);
    }
    p->scan_cancel = g_cancellable_new();
    ExpandCtx *ctx = g_new0(ExpandCtx, 1);
    ctx->player = player_ref(p);
    ctx->n_files = n_files;
    ctx->files = g_new(GFile *, (gsize)n_files);
    for (int i = 0; i < n_files; i++) ctx->files[i] = g_object_ref(files[i]);
    GTask *task = g_task_new(NULL, p->scan_cancel, expand_done, ctx);
    g_task_set_task_data(task, ctx, expand_ctx_free);
    g_task_run_in_thread(task, expand_worker);
    g_object_unref(task);
}

static void play_downloaded(SurPlayer *p, GPtrArray *paths, int start_index) {
    if (!paths || paths->len == 0) return;
    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)sur_track_free);
    for (guint i = 0; i < paths->len; i++)
        g_ptr_array_add(tracks, sur_track_new_local(g_ptr_array_index(paths, i)));
    sur_queue_set_tracks(&p->queue, tracks, start_index);
    close_overlays(p);
    play_current(p);
}

static void play_history_item(SurPlayer *p, const SurHistoryItem *item) {
    if (!item->is_online) {
        GFile *f = g_file_new_for_path(item->path_or_url);
        open_files(p, &f, 1);
        g_object_unref(f);
    } else {
        GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)sur_track_free);
        g_ptr_array_add(tracks, sur_track_new_online(
            item->path_or_url, item->title, item->artist_or_sub, item->duration_secs, item->thumbnail));
        sur_queue_set_tracks(&p->queue, tracks, 0);
        close_overlays(p);
        play_current(p);
    }
}

/* ── UI Builders: Queue, Downloaded/History, Online ─────────────────────── */

typedef struct { SurPlayer *p; int idx; } QueueJumpCtx;

static void free_closure(gpointer d, GClosure *cl) { (void)cl; g_free(d); }

static void on_queue_row_clicked(GtkButton *b, gpointer ud) {
    (void)b;
    QueueJumpCtx *c = ud;
    if (sur_queue_jump_order(&c->p->queue, c->idx)) {
        close_overlays(c->p);
        play_current(c->p);
    }
}

static void rebuild_queue_ui(SurPlayer *p) {
    clear_listbox(p->queue_list);
    guint n = p->queue.order ? p->queue.order->len : 0;
    char cnt[32];
    snprintf(cnt, sizeof(cnt), n == 1 ? "1 track" : "%u tracks", n);
    gtk_label_set_label(p->queue_count_lbl, cnt);
    for (guint pos = 0; pos < n; pos++) {
        guint tidx = g_array_index(p->queue.order, guint, pos);
        SurTrack *t = g_ptr_array_index(p->queue.tracks, tidx);
        bool active = (int)pos == p->queue.pos;
        char *lbl_txt = active ? g_strdup_printf("▶ %s", t->title) : g_strdup(t->title);
        GtkWidget *tl = gtk_label_new(lbl_txt);
        g_free(lbl_txt);
        gtk_label_set_ellipsize(GTK_LABEL(tl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_halign(tl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(tl, TRUE);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(vbox, TRUE);
        gtk_box_append(GTK_BOX(vbox), tl);
        char *sub = sur_track_subtitle(t);
        if (sub[0]) {
            GtkWidget *sl = gtk_label_new(sub);
            gtk_label_set_ellipsize(GTK_LABEL(sl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_halign(sl, GTK_ALIGN_START);
            gtk_widget_add_css_class(sl, "sur-time");
            gtk_box_append(GTK_BOX(vbox), sl);
        }
        g_free(sub);

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(hbox), vbox);
        if (t->duration_secs >= 0) {
            char db[32];
            sur_format_time(t->duration_secs, db, sizeof(db));
            GtkWidget *dl = gtk_label_new(db);
            gtk_widget_set_valign(dl, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(dl, "numeric");
            gtk_widget_add_css_class(dl, "sur-time");
            gtk_box_append(GTK_BOX(hbox), dl);
        }

        GtkWidget *btn = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(btn), hbox);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_add_css_class(btn, "sur-queue-row");
        if (active) gtk_widget_add_css_class(btn, "sur-active");
        QueueJumpCtx *jc = g_new(QueueJumpCtx, 1);
        jc->p = p; jc->idx = (int)pos;
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_queue_row_clicked), jc, free_closure, 0);
        gtk_list_box_append(p->queue_list, btn);
    }
}

typedef struct { SurPlayer *p; GPtrArray *paths; int idx; } DlRowCtx;
static void dl_row_ctx_free(gpointer d, GClosure *cl) {
    (void)cl;
    DlRowCtx *c = d;
    g_ptr_array_unref(c->paths);
    g_free(c);
}
static void on_dl_row_activated(AdwActionRow *r, gpointer ud) {
    (void)r;
    DlRowCtx *c = ud;
    play_downloaded(c->p, c->paths, c->idx);
}

static GtkWidget *make_downloaded_row(SurPlayer *p, const char *path, GPtrArray *all_paths, int idx) {
    char *title = sur_title_from_path(path);
    char *ext = sur_codec_from_path(path);
    char *et = g_markup_escape_text(title, -1);
    char *es = g_markup_escape_text(ext ? ext : "AUDIO", -1);
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), et);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), es);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("audio-x-generic-symbolic"));
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("media-playback-start-symbolic"));
    g_free(title); g_free(ext); g_free(et); g_free(es);

    DlRowCtx *c = g_new(DlRowCtx, 1);
    c->p = p; c->paths = g_ptr_array_ref(all_paths); c->idx = idx;
    g_signal_connect_data(row, "activated", G_CALLBACK(on_dl_row_activated), c, dl_row_ctx_free, 0);
    return row;
}

typedef struct { SurPlayer *p; SurHistoryItem *item; } HistRowCtx;
static void hist_row_ctx_free(gpointer d, GClosure *cl) {
    (void)cl;
    HistRowCtx *c = d;
    sur_history_item_free(c->item);
    g_free(c);
}
static void on_hist_row_activated(AdwActionRow *r, gpointer ud) {
    (void)r;
    HistRowCtx *c = ud;
    play_history_item(c->p, c->item);
}

static GtkWidget *make_history_row(SurPlayer *p, const SurHistoryItem *item) {
    char *sub = item->is_online
        ? (item->artist_or_sub && item->artist_or_sub[0]
               ? g_strdup_printf("%s · Online", item->artist_or_sub)
               : g_strdup("Online Stream"))
        : g_strdup(item->artist_or_sub ? item->artist_or_sub : "");
    char *et = g_markup_escape_text(item->title, -1);
    char *es = g_markup_escape_text(sub, -1);
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), et);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), es);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row),
        gtk_image_new_from_icon_name(item->is_online ? "network-wireless-symbolic" : "audio-x-generic-symbolic"));
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
    g_free(sub); g_free(et); g_free(es);

    HistRowCtx *c = g_new(HistRowCtx, 1);
    c->p = p; c->item = sur_history_item_copy(item);
    g_signal_connect_data(row, "activated", G_CALLBACK(on_hist_row_activated), c, hist_row_ctx_free, 0);
    return row;
}

static void on_home_play_all(GtkButton *b, gpointer ud) {
    (void)b;
    SurPlayer *p = ud;
    GPtrArray *dls = sur_downloaded_list();
    if (dls->len > 0) play_downloaded(p, dls, 0);
    g_ptr_array_unref(dls);
}

static void refresh_home(SurPlayer *p) {
    clear_box(p->home_box);
    GPtrArray *dls = sur_downloaded_list();
    if (dls->len > 0) {
        gtk_widget_add_css_class(GTK_WIDGET(p->start_page), "compact");
        adw_status_page_set_title(p->start_page, "Downloaded");
        char desc[64];
        snprintf(desc, sizeof(desc),
                 dls->len == 1 ? "1 saved track in ~/Music/Sur" : "%u saved tracks in ~/Music/Sur", dls->len);
        adw_status_page_set_description(p->start_page, desc);

        GtkWidget *list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        gtk_widget_add_css_class(list, "boxed-list");
        for (guint i = 0; i < dls->len; i++)
            gtk_list_box_append(GTK_LIST_BOX(list),
                make_downloaded_row(p, g_ptr_array_index(dls, i), dls, (int)i));

        GtkWidget *heading = gtk_label_new("Downloaded");
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        gtk_widget_set_hexpand(heading, TRUE);
        gtk_widget_add_css_class(heading, "heading");
        GtkWidget *play_all = gtk_button_new_with_label("Play All");
        gtk_widget_add_css_class(play_all, "flat");
        g_signal_connect(play_all, "clicked", G_CALLBACK(on_home_play_all), p);
        GtkWidget *hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(hrow), heading);
        gtk_box_append(GTK_BOX(hrow), play_all);

        GtkWidget *sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_box_append(GTK_BOX(sec), hrow);
        gtk_box_append(GTK_BOX(sec), list);
        GtkWidget *clamp = adw_clamp_new();
        adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 400);
        adw_clamp_set_child(ADW_CLAMP(clamp), sec);
        gtk_box_append(p->home_box, clamp);
        g_ptr_array_unref(dls);
        return;
    }
    g_ptr_array_unref(dls);

    adw_status_page_set_title(p->start_page, "Drop Music Here");
    adw_status_page_set_description(p->start_page, "Play local files, folders, or search online");
    GPtrArray *items = sur_history_load(5, true);
    if (items->len == 0) {
        gtk_widget_remove_css_class(GTK_WIDGET(p->start_page), "compact");
        g_ptr_array_unref(items);
        return;
    }
    gtk_widget_add_css_class(GTK_WIDGET(p->start_page), "compact");
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    for (guint i = 0; i < items->len; i++)
        gtk_list_box_append(GTK_LIST_BOX(list), make_history_row(p, g_ptr_array_index(items, i)));
    g_ptr_array_unref(items);

    GtkWidget *heading = gtk_label_new("Recent");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_widget_add_css_class(heading, "heading");
    GtkWidget *sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(sec), heading);
    gtk_box_append(GTK_BOX(sec), list);
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 400);
    adw_clamp_set_child(ADW_CLAMP(clamp), sec);
    gtk_box_append(p->home_box, clamp);
}

static void refresh_history(SurPlayer *p) {
    clear_listbox(p->hist_list);
    if (gtk_toggle_button_get_active(p->hist_dl_tab)) {
        GPtrArray *dls = sur_downloaded_list();
        gtk_widget_set_visible(GTK_WIDGET(p->hist_clear_btn), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(p->hist_play_all_btn), dls->len > 0);
        gtk_label_set_label(p->hist_empty_lbl, "No downloaded tracks yet in ~/Music/Sur");
        gtk_widget_set_visible(GTK_WIDGET(p->hist_empty_lbl), dls->len == 0);
        gtk_widget_set_visible(GTK_WIDGET(p->hist_list), dls->len > 0);
        for (guint i = 0; i < dls->len; i++)
            gtk_list_box_append(p->hist_list,
                make_downloaded_row(p, g_ptr_array_index(dls, i), dls, (int)i));
        g_ptr_array_unref(dls);
    } else {
        GPtrArray *items = sur_history_load(50, true);
        gtk_widget_set_visible(GTK_WIDGET(p->hist_play_all_btn), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(p->hist_clear_btn), items->len > 0);
        gtk_label_set_label(p->hist_empty_lbl, "No recent tracks yet");
        gtk_widget_set_visible(GTK_WIDGET(p->hist_empty_lbl), items->len == 0);
        gtk_widget_set_visible(GTK_WIDGET(p->hist_list), items->len > 0);
        for (guint i = 0; i < items->len; i++)
            gtk_list_box_append(p->hist_list, make_history_row(p, g_ptr_array_index(items, i)));
        g_ptr_array_unref(items);
    }
}

/* ── Online Search & Download UI ────────────────────────────────────────── */

typedef struct {
    GPtrArray *results;
    char *err;
} SearchOut;
static void search_out_free(gpointer d) {
    SearchOut *o = d;
    if (!o) return;
    g_clear_pointer(&o->results, g_ptr_array_unref);
    g_free(o->err);
    g_free(o);
}

static void search_worker(GTask *task, gpointer src, gpointer td, GCancellable *c) {
    (void)src; (void)c;
    AsyncCtx *ctx = td;
    SearchOut *out = g_new0(SearchOut, 1);
    out->results = sur_online_search(ctx->arg, &out->err);
    g_task_return_pointer(task, out, search_out_free);
}

static void search_done(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)src;
    AsyncCtx *ctx = ud;
    SearchOut *out = g_task_propagate_pointer(G_TASK(res), NULL);
    SurPlayer *p = ctx->player;
    if (!out || p->stopped || p->search_gen != ctx->gen) {
        search_out_free(out);
        return;
    }
    gtk_spinner_stop(p->online_spinner);
    gtk_widget_set_visible(GTK_WIDGET(p->online_spinner), FALSE);
    if (out->results) {
        char st[32];
        snprintf(st, sizeof(st), out->results->len == 0 ? "No results found" : "%u results", out->results->len);
        gtk_label_set_label(p->online_status, st);
        g_clear_pointer(&p->online_results, g_ptr_array_unref);
        p->online_results = g_steal_pointer(&out->results);
        rebuild_online_results(p);
    } else {
        gtk_label_set_label(p->online_status, out->err ? out->err : "Search failed");
    }
    search_out_free(out);
}

static void on_search_activate(GtkSearchEntry *e, gpointer ud) {
    SurPlayer *p = ud;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(e));
    if (!q || !q[0]) return;
    p->search_gen++;
    gtk_widget_set_visible(GTK_WIDGET(p->online_spinner), TRUE);
    gtk_spinner_start(p->online_spinner);
    gtk_label_set_label(p->online_status, "Searching…");
    AsyncCtx *ac = g_new0(AsyncCtx, 1);
    ac->player = player_ref(p); ac->gen = p->search_gen; ac->arg = g_strdup(q);
    GTask *t = g_task_new(NULL, NULL, search_done, ac);
    g_task_set_task_data(t, ac, async_ctx_free);
    g_task_run_in_thread(t, search_worker);
    g_object_unref(t);
}

typedef struct { char *saved_path; char *err; } DlOut;
static void dl_out_free(gpointer d) {
    DlOut *o = d;
    if (!o) return;
    g_free(o->saved_path); g_free(o->err); g_free(o);
}

typedef struct { SurPlayer *p; char *path; } PlaySavedCtx;
static void play_saved_free(gpointer d, GClosure *cl) {
    (void)cl;
    PlaySavedCtx *c = d;
    player_unref(c->p);
    g_free(c->path);
    g_free(c);
}
static void on_toast_play_saved(AdwToast *t, gpointer ud) {
    (void)t;
    PlaySavedCtx *c = ud;
    if (c->p->stopped) return;
    GFile *f = g_file_new_for_path(c->path);
    open_files(c->p, &f, 1);
    g_object_unref(f);
}

static void dl_worker(GTask *task, gpointer src, gpointer td, GCancellable *c) {
    (void)src; (void)c;
    AsyncCtx *ctx = td;
    char *dir = sur_download_dir();
    DlOut *out = g_new0(DlOut, 1);
    out->saved_path = sur_online_download(ctx->arg, dir, &out->err);
    g_free(dir);
    g_task_return_pointer(task, out, dl_out_free);
}

static void dl_done(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)src;
    AsyncCtx *ctx = ud;
    DlOut *out = g_task_propagate_pointer(G_TASK(res), NULL);
    SurPlayer *p = ctx->player;
    if (!out || p->stopped) { dl_out_free(out); return; }
    g_hash_table_remove(p->dl_urls, ctx->arg);
    if (out->saved_path) {
        g_hash_table_add(p->saved_urls, g_strdup(ctx->arg));
        refresh_home(p);
        refresh_history(p);
        sync_metadata_ui(p);
        rebuild_online_results(p);
        char *base = g_path_get_basename(out->saved_path);
        char *msg = g_strdup_printf("Saved: %s", base);
        AdwToast *t = adw_toast_new(msg);
        adw_toast_set_use_markup(t, FALSE);
        adw_toast_set_button_label(t, "Play");
        adw_toast_set_timeout(t, 5);
        PlaySavedCtx *pc = g_new(PlaySavedCtx, 1);
        pc->p = player_ref(p); pc->path = g_strdup(out->saved_path);
        g_signal_connect_data(t, "button-clicked", G_CALLBACK(on_toast_play_saved), pc, play_saved_free, 0);
        adw_toast_overlay_add_toast(p->toasts, t);
        g_free(base); g_free(msg);
    } else {
        sync_metadata_ui(p);
        rebuild_online_results(p);
        char *m = g_strdup_printf("Download failed: %s", out->err ? out->err : "unknown");
        announce(p, m);
        g_free(m);
    }
    dl_out_free(out);
}

static void download_online(SurPlayer *p, const char *page_url, const char *title) {
    if (g_hash_table_contains(p->dl_urls, page_url) || g_hash_table_contains(p->saved_urls, page_url))
        return;
    g_hash_table_add(p->dl_urls, g_strdup(page_url));
    sync_metadata_ui(p);
    rebuild_online_results(p);
    char *m = g_strdup_printf("Saving best audio: %s", title);
    announce(p, m);
    g_free(m);

    AsyncCtx *ac = g_new0(AsyncCtx, 1);
    ac->player = player_ref(p); ac->arg = g_strdup(page_url); ac->fallback_thumb = g_strdup(title);
    GTask *t = g_task_new(NULL, NULL, dl_done, ac);
    g_task_set_task_data(t, ac, async_ctx_free);
    g_task_run_in_thread(t, dl_worker);
    g_object_unref(t);
}

typedef struct { SurPlayer *p; guint idx; } OnlineIdxCtx;

static void on_online_play(GtkButton *b, gpointer ud) {
    (void)b;
    OnlineIdxCtx *c = ud;
    if (!c->p->online_results || c->idx >= c->p->online_results->len) return;
    SurSearchResult *r = g_ptr_array_index(c->p->online_results, c->idx);
    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)sur_track_free);
    g_ptr_array_add(tracks, sur_track_new_online(r->page_url, r->title, r->artist, r->duration_secs, r->thumbnail));
    sur_queue_set_tracks(&c->p->queue, tracks, 0);
    close_overlays(c->p);
    play_current(c->p);
}

static void on_online_add(GtkButton *b, gpointer ud) {
    (void)b;
    OnlineIdxCtx *c = ud;
    if (!c->p->online_results || c->idx >= c->p->online_results->len) return;
    SurSearchResult *r = g_ptr_array_index(c->p->online_results, c->idx);
    bool empty = !has_track(c->p);
    sur_queue_append(&c->p->queue,
        sur_track_new_online(r->page_url, r->title, r->artist, r->duration_secs, r->thumbnail));
    rebuild_queue_ui(c->p);
    update_nav_buttons(c->p);
    if (empty) { close_overlays(c->p); play_current(c->p); }
    else {
        char *m = g_strdup_printf("Added to queue: %s", r->title);
        announce(c->p, m);
        g_free(m);
    }
}

static void on_online_save(GtkButton *b, gpointer ud) {
    (void)b;
    OnlineIdxCtx *c = ud;
    if (!c->p->online_results || c->idx >= c->p->online_results->len) return;
    SurSearchResult *r = g_ptr_array_index(c->p->online_results, c->idx);
    download_online(c->p, r->page_url, r->title);
}

static void rebuild_online_results(SurPlayer *p) {
    clear_listbox(p->online_list);
    if (!p->online_results) return;
    for (guint i = 0; i < p->online_results->len; i++) {
        SurSearchResult *r = g_ptr_array_index(p->online_results, i);
        GtkWidget *tl = gtk_label_new(r->title);
        gtk_label_set_ellipsize(GTK_LABEL(tl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_halign(tl, GTK_ALIGN_START);
        char *meta_txt;
        if (r->duration_secs >= 0) {
            char db[32];
            sur_format_time(r->duration_secs, db, sizeof(db));
            meta_txt = g_strdup_printf("%s · %s", r->artist, db);
        } else {
            meta_txt = g_strdup(r->artist);
        }
        GtkWidget *ml = gtk_label_new(meta_txt);
        g_free(meta_txt);
        gtk_label_set_ellipsize(GTK_LABEL(ml), PANGO_ELLIPSIZE_END);
        gtk_widget_set_halign(ml, GTK_ALIGN_START);
        gtk_widget_add_css_class(ml, "sur-time");

        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(col, TRUE);
        gtk_box_append(GTK_BOX(col), tl);
        gtk_box_append(GTK_BOX(col), ml);

        GtkButton *pb = make_icon_btn("media-playback-start-symbolic", "Stream now");
        GtkButton *ab = make_icon_btn("list-add-symbolic", "Add to Up Next");
        bool saved = g_hash_table_contains(p->saved_urls, r->page_url);
        bool busy = g_hash_table_contains(p->dl_urls, r->page_url);
        GtkButton *sb = make_icon_btn(saved ? "emblem-ok-symbolic" : "document-save-symbolic",
                                      saved ? "Saved in ~/Music/Sur"
                                            : (busy ? "Downloading…" : "Save best audio (.opus / .m4a)"));
        gtk_widget_set_sensitive(GTK_WIDGET(sb), !saved && !busy);

        OnlineIdxCtx *c1 = g_new(OnlineIdxCtx, 1); c1->p = p; c1->idx = i;
        OnlineIdxCtx *c2 = g_new(OnlineIdxCtx, 1); c2->p = p; c2->idx = i;
        OnlineIdxCtx *c3 = g_new(OnlineIdxCtx, 1); c3->p = p; c3->idx = i;
        g_signal_connect_data(pb, "clicked", G_CALLBACK(on_online_play), c1, free_closure, 0);
        g_signal_connect_data(ab, "clicked", G_CALLBACK(on_online_add), c2, free_closure, 0);
        g_signal_connect_data(sb, "clicked", G_CALLBACK(on_online_save), c3, free_closure, 0);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(row, "sur-online-row");
        gtk_widget_set_margin_bottom(row, 6);
        gtk_box_append(GTK_BOX(row), col);
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(pb));
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(ab));
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(sb));
        gtk_list_box_append(p->online_list, row);
    }
}

/* ── Signal Handlers & Actions ──────────────────────────────────────────── */

static void update_volume_icon(SurPlayer *p) {
    double v = gtk_range_get_value(GTK_RANGE(p->volume_scale));
    const char *icon = (p->muted || v == 0.0) ? "audio-volume-muted-symbolic"
                     : (v < 0.34 ? "audio-volume-low-symbolic"
                     : (v < 0.67 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic"));
    gtk_button_set_icon_name(p->mute_btn, icon);
    gtk_menu_button_set_icon_name(p->volume_btn, icon);
}

static void set_muted(SurPlayer *p, bool muted) {
    p->muted = muted;
    if (p->playbin) g_object_set(p->playbin, "mute", muted, NULL);
    update_volume_icon(p);
}

static void set_shuffle_ui(SurPlayer *p, bool enabled) {
    sur_queue_set_shuffle(&p->queue, enabled);
    if (enabled) gtk_widget_add_css_class(GTK_WIDGET(p->shuffle_btn), "sur-toggle-active");
    else gtk_widget_remove_css_class(GTK_WIDGET(p->shuffle_btn), "sur-toggle-active");
    rebuild_queue_ui(p);
    const char *props[] = {"Shuffle", "CanGoNext", "CanGoPrevious", NULL};
    mpris_notify(p, props);
}

static void apply_repeat_ui(SurPlayer *p, SurRepeatMode m) {
    gtk_button_set_icon_name(p->repeat_btn, sur_repeat_icon(m));
    gtk_widget_set_tooltip_text(GTK_WIDGET(p->repeat_btn), sur_repeat_label(m));
    if (m == SUR_REPEAT_OFF) gtk_widget_remove_css_class(GTK_WIDGET(p->repeat_btn), "sur-toggle-active");
    else gtk_widget_add_css_class(GTK_WIDGET(p->repeat_btn), "sur-toggle-active");
}

static void on_play_clicked(GtkButton *b, gpointer ud) { (void)b; SurPlayer *p = ud; set_playing(p, !p->playing); }
static void on_next_clicked(GtkButton *b, gpointer ud) {
    (void)b; SurPlayer *p = ud;
    if (sur_queue_advance(&p->queue, true)) play_current(p);
}
static void on_prev_clicked(GtkButton *b, gpointer ud) {
    (void)b; SurPlayer *p = ud;
    if (current_position(p) > RESTART_THRESHOLD) { jump_to(p, 0.0); return; }
    if (sur_queue_previous(&p->queue)) play_current(p);
    else if (has_track(p)) jump_to(p, 0.0);
}
static void on_shuffle_clicked(GtkButton *b, gpointer ud) {
    (void)b; SurPlayer *p = ud;
    bool nxt = !p->queue.shuffle;
    set_shuffle_ui(p, nxt);
    announce(p, nxt ? "Shuffle on" : "Shuffle off");
}
static void on_repeat_clicked(GtkButton *b, gpointer ud) {
    (void)b; SurPlayer *p = ud;
    SurRepeatMode m = sur_queue_cycle_repeat(&p->queue);
    apply_repeat_ui(p, m);
    announce(p, sur_repeat_label(m));
    const char *props[] = {"LoopStatus", "CanGoNext", "CanGoPrevious", NULL};
    mpris_notify(p, props);
}
static void on_save_cur_clicked(GtkButton *b, gpointer ud) {
    (void)b; SurPlayer *p = ud;
    SurTrack *t = sur_queue_current(&p->queue);
    if (t && t->is_online) download_online(p, t->path_or_url, t->title);
}
static void on_mute_clicked(GtkButton *b, gpointer ud) { (void)b; SurPlayer *p = ud; set_muted(p, !p->muted); }
static void on_volume_changed(GtkRange *r, gpointer ud) {
    SurPlayer *p = ud;
    if (p->stopped) return;
    double v = gtk_range_get_value(r);
    if (p->playbin) g_object_set(p->playbin, "volume", v * v * v, NULL);
    if (v > 0.0) set_muted(p, false);
    else update_volume_icon(p);
    const char *props[] = {"Volume", NULL};
    mpris_notify(p, props);
}
static gboolean on_seek_change(GtkRange *r, GtkScrollType s, double val, gpointer ud) {
    (void)s;
    SurPlayer *p = ud;
    if (p->stopped) return FALSE;
    double upper = gtk_adjustment_get_upper(gtk_range_get_adjustment(r));
    if (val < 0.0) val = 0.0;
    if (val > upper) val = upper;
    p->last_user_seek_us = g_get_monotonic_time();
    p->seek_target = val;
    char buf[32];
    sur_format_time((int64_t)val, buf, sizeof(buf));
    gtk_label_set_label(p->pos_lbl, buf);
    seek_to(p, val);
    return FALSE;
}

static void on_queue_toggled(GtkToggleButton *b, gpointer ud) {
    SurPlayer *p = ud;
    if (p->stopped) return;
    adw_overlay_split_view_set_show_sidebar(p->split, gtk_toggle_button_get_active(b));
}
static void on_split_changed(AdwOverlaySplitView *s, GParamSpec *ps, gpointer ud) {
    (void)ps;
    SurPlayer *p = ud;
    if (p->stopped) return;
    gboolean shown = adw_overlay_split_view_get_show_sidebar(s);
    if (gtk_toggle_button_get_active(p->queue_btn) != shown)
        gtk_toggle_button_set_active(p->queue_btn, shown);
}
static void on_search_toggled(GtkToggleButton *b, gpointer ud) {
    SurPlayer *p = ud;
    if (p->stopped) return;
    if (gtk_toggle_button_get_active(b) && gtk_toggle_button_get_active(p->history_btn))
        gtk_toggle_button_set_active(p->history_btn, FALSE);
    sync_active_view(p);
}
static void on_history_toggled(GtkToggleButton *b, gpointer ud) {
    SurPlayer *p = ud;
    if (p->stopped) return;
    if (gtk_toggle_button_get_active(b)) {
        if (gtk_toggle_button_get_active(p->search_btn))
            gtk_toggle_button_set_active(p->search_btn, FALSE);
        gtk_toggle_button_set_active(p->hist_recent_tab, TRUE);
        refresh_history(p);
    }
    sync_active_view(p);
}
static void on_hist_tab_toggled(GtkToggleButton *b, gpointer ud) {
    SurPlayer *p = ud;
    if (!p->stopped && gtk_toggle_button_get_active(b)) refresh_history(p);
}
static void on_hist_clear(GtkButton *b, gpointer ud) {
    (void)b;
    SurPlayer *p = ud;
    sur_history_clear();
    refresh_home(p);
    refresh_history(p);
    announce(p, "Cleared playback history");
}
static void on_online_pill(GtkButton *b, gpointer ud) {
    (void)b;
    gtk_toggle_button_set_active(((SurPlayer *)ud)->search_btn, TRUE);
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint key, guint kc, GdkModifierType mods, gpointer ud) {
    (void)c; (void)kc;
    SurPlayer *p = ud;
    if (gtk_widget_has_focus(GTK_WIDGET(p->search_entry))) {
        if (key == GDK_KEY_Escape) {
            gtk_toggle_button_set_active(p->search_btn, FALSE);
            return TRUE;
        }
        return FALSE;
    }
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
    bool shift = (mods & GDK_SHIFT_MASK) != 0;
    switch (gdk_keyval_to_lower(key)) {
        case GDK_KEY_space: set_playing(p, !p->playing); return TRUE;
        case GDK_KEY_Left: if (shift) on_prev_clicked(NULL, p); else seek_by(p, -SEEK_STEP); return TRUE;
        case GDK_KEY_Right: if (shift) on_next_clicked(NULL, p); else seek_by(p, SEEK_STEP); return TRUE;
        case GDK_KEY_p: on_prev_clicked(NULL, p); return TRUE;
        case GDK_KEY_n: on_next_clicked(NULL, p); return TRUE;
        case GDK_KEY_Up: {
            double v = gtk_range_get_value(GTK_RANGE(p->volume_scale)) + VOLUME_STEP;
            gtk_range_set_value(GTK_RANGE(p->volume_scale), v > 1.0 ? 1.0 : v);
            set_muted(p, false);
            return TRUE;
        }
        case GDK_KEY_Down: {
            double v = gtk_range_get_value(GTK_RANGE(p->volume_scale)) - VOLUME_STEP;
            gtk_range_set_value(GTK_RANGE(p->volume_scale), v < 0.0 ? 0.0 : v);
            return TRUE;
        }
        case GDK_KEY_m: set_muted(p, !p->muted); return TRUE;
        case GDK_KEY_s: on_shuffle_clicked(NULL, p); return TRUE;
        case GDK_KEY_l: on_repeat_clicked(NULL, p); return TRUE;
        case GDK_KEY_q: gtk_toggle_button_set_active(p->queue_btn, !gtk_toggle_button_get_active(p->queue_btn)); return TRUE;
        case GDK_KEY_h: gtk_toggle_button_set_active(p->history_btn, !gtk_toggle_button_get_active(p->history_btn)); return TRUE;
        case GDK_KEY_Escape:
            if (gtk_toggle_button_get_active(p->search_btn) || gtk_toggle_button_get_active(p->history_btn)) {
                close_overlays(p);
                return TRUE;
            }
            return FALSE;
        default: return FALSE;
    }
}

static void on_files_chosen(GObject *src, GAsyncResult *res, gpointer ud) {
    SurPlayer *p = ud;
    GListModel *m = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!m) return;
    guint n = g_list_model_get_n_items(m);
    if (n > 0) {
        GFile **arr = g_new(GFile *, n);
        for (guint i = 0; i < n; i++) arr[i] = g_list_model_get_item(m, i);
        open_files(p, arr, (int)n);
        for (guint i = 0; i < n; i++) g_object_unref(arr[i]);
        g_free(arr);
    }
    g_object_unref(m);
}

static void on_act_open(GSimpleAction *a, GVariant *par, gpointer ud) {
    (void)a; (void)par;
    SurPlayer *p = ud;
    GtkFileFilter *audio = gtk_file_filter_new();
    gtk_file_filter_set_name(audio, "Music");
    gtk_file_filter_add_mime_type(audio, "audio/*");
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All Files");
    gtk_file_filter_add_pattern(all, "*");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, audio);
    g_list_store_append(filters, all);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Open Music");
    gtk_file_dialog_set_modal(dlg, TRUE);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dlg, audio);
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (dir) {
        GFile *f = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dlg, f);
        g_object_unref(f);
    }
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(p->window), NULL, on_files_chosen, p);
    g_object_unref(audio); g_object_unref(all); g_object_unref(filters); g_object_unref(dlg);
}

static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer ud) {
    SurPlayer *p = ud;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) { open_files(p, &f, 1); g_object_unref(f); }
}

static void on_act_open_folder(GSimpleAction *a, GVariant *par, gpointer ud) {
    (void)a; (void)par;
    SurPlayer *p = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Open Music Folder");
    gtk_file_dialog_set_modal(dlg, TRUE);
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (dir) {
        GFile *f = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dlg, f);
        g_object_unref(f);
    }
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(p->window), NULL, on_folder_chosen, p);
    g_object_unref(dlg);
}

static void on_act_search(GSimpleAction *a, GVariant *par, gpointer ud) {
    (void)a; (void)par;
    SurPlayer *p = ud;
    gtk_toggle_button_set_active(p->search_btn, !gtk_toggle_button_get_active(p->search_btn));
}
static void on_act_history(GSimpleAction *a, GVariant *par, gpointer ud) {
    (void)a; (void)par;
    SurPlayer *p = ud;
    gtk_toggle_button_set_active(p->history_btn, !gtk_toggle_button_get_active(p->history_btn));
}

static gboolean on_drop(GtkDropTarget *dt, const GValue *val, double x, double y, gpointer ud) {
    (void)dt; (void)x; (void)y;
    SurPlayer *p = ud;
    if (!G_VALUE_HOLDS(val, GDK_TYPE_FILE_LIST)) return FALSE;
    GSList *list = g_value_get_boxed(val);
    if (!list) return FALSE;
    guint n = g_slist_length(list);
    GFile **arr = g_new(GFile *, n);
    guint i = 0;
    for (GSList *l = list; l; l = l->next) arr[i++] = l->data;
    open_files(p, arr, (int)n);
    g_free(arr);
    return TRUE;
}

/* ── MPRIS VTable ───────────────────────────────────────────────────────── */

static const char *m_status(SurPlayer *p) { return !has_track(p) ? "Stopped" : (p->playing ? "Playing" : "Paused"); }
static SurRepeatMode m_repeat(SurPlayer *p) { return p->queue.repeat; }
static bool m_shuffle(SurPlayer *p) { return p->queue.shuffle; }
static bool m_can_next(SurPlayer *p) { return sur_queue_can_next(&p->queue); }
static bool m_can_prev(SurPlayer *p) { return sur_queue_can_prev(&p->queue); }
static bool m_has_track(SurPlayer *p) { return has_track(p); }
static double m_vol(SurPlayer *p) { return gtk_range_get_value(GTK_RANGE(p->volume_scale)); }
static double m_pos(SurPlayer *p) { return current_position(p); }
static GVariant *m_meta(SurPlayer *p) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path("/io/github/rajnish/Sur/Track"));
    SurTrack *t = sur_queue_current(&p->queue);
    if (t) {
        if (t->title && t->title[0]) g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(t->title));
        if (t->artist && t->artist[0]) {
            const char *a[] = {t->artist, NULL};
            g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(a, -1));
        }
        if (t->album && t->album[0]) g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(t->album));
        if (t->is_online) {
            g_variant_builder_add(&b, "{sv}", "xesam:url", g_variant_new_string(t->path_or_url));
        } else {
            GFile *f = g_file_new_for_path(t->path_or_url);
            char *u = g_file_get_uri(f);
            g_object_unref(f);
            g_variant_builder_add(&b, "{sv}", "xesam:url", g_variant_new_string(u));
            g_free(u);
        }
        if (p->art_uri) g_variant_builder_add(&b, "{sv}", "mpris:artUrl", g_variant_new_string(p->art_uri));
        double dur = p->duration_ns > 0 ? (double)p->duration_ns / 1e9 : (double)t->duration_secs;
        if (dur > 0.0) g_variant_builder_add(&b, "{sv}", "mpris:length", g_variant_new_int64((gint64)(dur * 1e6)));
    }
    return g_variant_builder_end(&b);
}
static void m_set_repeat(SurPlayer *p, SurRepeatMode m) {
    p->queue.repeat = m;
    apply_repeat_ui(p, m);
    const char *props[] = {"LoopStatus", "CanGoNext", "CanGoPrevious", NULL};
    mpris_notify(p, props);
}
static void m_set_shuffle(SurPlayer *p, bool e) { set_shuffle_ui(p, e); }
static void m_set_vol(SurPlayer *p, double v) { gtk_range_set_value(GTK_RANGE(p->volume_scale), v); }
static void m_raise(SurPlayer *p) { gtk_window_present(GTK_WINDOW(p->window)); }
static void m_quit(SurPlayer *p) { gtk_window_close(GTK_WINDOW(p->window)); }
static void m_play(SurPlayer *p) { set_playing(p, true); }
static void m_pause(SurPlayer *p) { set_playing(p, false); }
static void m_stop(SurPlayer *p) { stop_playback(p); }
static void m_play_pause(SurPlayer *p) { set_playing(p, !p->playing); }
static void m_next(SurPlayer *p) { on_next_clicked(NULL, p); }
static void m_prev(SurPlayer *p) { on_prev_clicked(NULL, p); }
static void m_seek(SurPlayer *p, double off) { seek_by(p, off); }
static void m_set_pos(SurPlayer *p, double pos) { jump_to(p, pos); }
static void m_open_uri(SurPlayer *p, const char *uri) {
    if (sur_looks_like_url(uri)) play_online_url(p, uri);
    else {
        GFile *f = g_file_new_for_uri(uri);
        open_files(p, &f, 1);
        g_object_unref(f);
    }
}

static const SurMprisVTable MPRIS_VT = {
    m_status, m_repeat, m_shuffle, m_can_next, m_can_prev, m_has_track,
    m_vol, m_pos, m_meta, m_set_repeat, m_set_shuffle, m_set_vol,
    m_raise, m_quit, m_play, m_pause, m_stop, m_play_pause,
    m_next, m_prev, m_seek, m_set_pos, m_open_uri
};

/* ── Window Lifecycle ───────────────────────────────────────────────────── */

static void shutdown_player(SurPlayer *p) {
    if (!p || p->stopped) return;
    p->stopped = true;
    p->playing = false;
    g_clear_pointer(&p->mpris, sur_mpris_close);
    if (p->scan_cancel) { g_cancellable_cancel(p->scan_cancel); g_clear_object(&p->scan_cancel); }
    if (p->tick_id) { g_source_remove(p->tick_id); p->tick_id = 0; }
    if (p->bus_watch_id) { g_source_remove(p->bus_watch_id); p->bus_watch_id = 0; }
    if (p->playbin) {
        gst_element_set_state(p->playbin, GST_STATE_NULL);
        g_clear_pointer(&p->playbin, gst_object_unref);
    }
    GdkDisplay *d = gdk_display_get_default();
    if (d && p->tint_provider)
        gtk_style_context_remove_provider_for_display(d, GTK_STYLE_PROVIDER(p->tint_provider));
    g_clear_object(&p->tint_provider);
    g_clear_object(&p->active_toast);
    if (g_player == p) g_player = NULL;
}

static gboolean on_close_req(GtkWindow *w, gpointer ud) { (void)w; shutdown_player((SurPlayer *)ud); return FALSE; }
static void on_destroy(GtkWidget *w, gpointer ud) { (void)w; shutdown_player((SurPlayer *)ud); }

static SurPlayer *ensure_player(AdwApplication *app) {
    if (g_player && !g_player->stopped) return g_player;
    SurPlayer *p = g_new0(SurPlayer, 1);
    p->refcount = 1;
    sur_queue_init(&p->queue);
    p->duration_ns = -1;
    p->saved_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    p->dl_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    GtkWidget *win = adw_application_window_new(GTK_APPLICATION(app));
    p->window = ADW_APPLICATION_WINDOW(win);
    g_object_set_data_full(G_OBJECT(win), "sur-player", p, (GDestroyNotify)player_unref);
    gtk_window_set_title(GTK_WINDOW(win), "Sur");
    gtk_window_set_default_size(GTK_WINDOW(win), 390, 510);
    gtk_widget_set_size_request(win, 340, 440);
    gtk_widget_add_css_class(win, "sur-window");

    p->tint_provider = gtk_css_provider_new();
    uint8_t acc[3], tint[3];
    sur_default_palette(acc, tint);
    char *tcss = sur_tint_css(acc, tint);
    gtk_css_provider_load_from_string(p->tint_provider, tcss);
    g_free(tcss);
    GdkDisplay *disp = gdk_display_get_default();
    if (disp) gtk_style_context_add_provider_for_display(
        disp, GTK_STYLE_PROVIDER(p->tint_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    /* Header bar */
    GtkWidget *open_btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(open_btn, "Open Music (Ctrl+O)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open_btn), "win.open");
    p->search_btn = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_icon_name(GTK_BUTTON(p->search_btn), "system-search-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(p->search_btn), "Search Online / Paste URL (Ctrl+F)");
    p->history_btn = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_icon_name(GTK_BUTTON(p->history_btn), "document-open-recent-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(p->history_btn), "History & Recent (H)");
    p->queue_btn = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_icon_name(GTK_BUTTON(p->queue_btn), "view-list-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(p->queue_btn), "Up Next Queue (Q)");

    p->mute_btn = make_icon_btn("audio-volume-high-symbolic", "Mute (M)");
    p->volume_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05));
    gtk_range_set_value(GTK_RANGE(p->volume_scale), 1.0);
    gtk_widget_set_size_request(GTK_WIDGET(p->volume_scale), 130, -1);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(vbox, 6); gtk_widget_set_margin_end(vbox, 6);
    gtk_widget_set_margin_top(vbox, 4); gtk_widget_set_margin_bottom(vbox, 4);
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(p->mute_btn));
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(p->volume_scale));
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), vbox);
    p->volume_btn = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(p->volume_btn, "audio-volume-high-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(p->volume_btn), "Volume (↑ / ↓)");
    gtk_menu_button_set_popover(p->volume_btn, pop);

    GtkWidget *hdr = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(hdr), GTK_WIDGET(adw_window_title_new("Sur", "")));
    gtk_widget_add_css_class(hdr, "sur-header");
    adw_header_bar_pack_start(ADW_HEADER_BAR(hdr), open_btn);
    adw_header_bar_pack_start(ADW_HEADER_BAR(hdr), GTK_WIDGET(p->search_btn));
    adw_header_bar_pack_start(ADW_HEADER_BAR(hdr), GTK_WIDGET(p->history_btn));
    adw_header_bar_pack_start(ADW_HEADER_BAR(hdr), GTK_WIDGET(p->queue_btn));
    adw_header_bar_pack_end(ADW_HEADER_BAR(hdr), GTK_WIDGET(p->volume_btn));

    /* Start page */
    GtkWidget *open_pill = gtk_button_new_with_label("Open Music…");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open_pill), "win.open");
    gtk_widget_add_css_class(open_pill, "pill");
    gtk_widget_add_css_class(open_pill, "suggested-action");
    GtkWidget *online_pill = gtk_button_new_with_label("Search Online");
    gtk_widget_add_css_class(online_pill, "pill");
    g_signal_connect(online_pill, "clicked", G_CALLBACK(on_online_pill), p);
    GtkWidget *sbtns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(sbtns, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(sbtns), open_pill);
    gtk_box_append(GTK_BOX(sbtns), online_pill);
    p->home_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 10));
    GtkWidget *scontent = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_box_append(GTK_BOX(scontent), sbtns);
    gtk_box_append(GTK_BOX(scontent), GTK_WIDGET(p->home_box));
    p->start_page = ADW_STATUS_PAGE(adw_status_page_new());
    adw_status_page_set_icon_name(p->start_page, "audio-x-generic-symbolic");
    adw_status_page_set_title(p->start_page, "Drop Music Here");
    adw_status_page_set_description(p->start_page, "Play local files, folders, or search online");
    adw_status_page_set_child(p->start_page, scontent);

    /* Playing view */
    GtkWidget *ph = gtk_image_new_from_icon_name("audio-x-generic-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ph), 72);
    gtk_widget_set_opacity(ph, 0.45);
    p->cover_picture = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(p->cover_picture, GTK_CONTENT_FIT_COVER);
    gtk_widget_set_hexpand(GTK_WIDGET(p->cover_picture), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(p->cover_picture), TRUE);
    p->cover_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(p->cover_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(p->cover_stack, 200);
    gtk_stack_add_named(p->cover_stack, ph, "placeholder");
    gtk_stack_add_named(p->cover_stack, GTK_WIDGET(p->cover_picture), "art");
    GtkWidget *cframe = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(cframe, 210, 210);
    gtk_widget_set_overflow(cframe, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(cframe, "sur-cover-frame");
    gtk_box_append(GTK_BOX(cframe), GTK_WIDGET(p->cover_stack));

    p->save_stream_btn = GTK_BUTTON(gtk_button_new_from_icon_name("document-save-symbolic"));
    gtk_widget_set_halign(GTK_WIDGET(p->save_stream_btn), GTK_ALIGN_END);
    gtk_widget_set_valign(GTK_WIDGET(p->save_stream_btn), GTK_ALIGN_END);
    gtk_widget_set_margin_end(GTK_WIDGET(p->save_stream_btn), 8);
    gtk_widget_set_margin_bottom(GTK_WIDGET(p->save_stream_btn), 8);
    gtk_widget_set_visible(GTK_WIDGET(p->save_stream_btn), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(p->save_stream_btn), "sur-save-overlay");

    p->loading_spinner = GTK_SPINNER(gtk_spinner_new());
    gtk_widget_set_size_request(GTK_WIDGET(p->loading_spinner), 28, 28);
    gtk_widget_set_halign(GTK_WIDGET(p->loading_spinner), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(p->loading_spinner), GTK_ALIGN_CENTER);
    gtk_widget_set_visible(GTK_WIDGET(p->loading_spinner), FALSE);

    GtkWidget *cov = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(cov), cframe);
    gtk_widget_set_halign(cov, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(cov), GTK_WIDGET(p->save_stream_btn));
    gtk_overlay_add_overlay(GTK_OVERLAY(cov), GTK_WIDGET(p->loading_spinner));

    p->title_lbl = GTK_LABEL(gtk_label_new("Sur"));
    gtk_label_set_ellipsize(p->title_lbl, PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(GTK_WIDGET(p->title_lbl), "sur-title");
    p->codec_badge = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_visible(GTK_WIDGET(p->codec_badge), FALSE);
    gtk_widget_set_valign(GTK_WIDGET(p->codec_badge), GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(GTK_WIDGET(p->codec_badge), "sur-badge");
    GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(trow, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(trow), GTK_WIDGET(p->title_lbl));
    gtk_box_append(GTK_BOX(trow), GTK_WIDGET(p->codec_badge));
    p->subtitle_lbl = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(p->subtitle_lbl, PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(GTK_WIDGET(p->subtitle_lbl), "sur-subtitle");
    GtkWidget *mbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(mbox), trow);
    gtk_box_append(GTK_BOX(mbox), GTK_WIDGET(p->subtitle_lbl));

    p->seek_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 1.0));
    gtk_widget_set_hexpand(GTK_WIDGET(p->seek_scale), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(p->seek_scale), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(p->seek_scale), "sur-seek");
    p->pos_lbl = make_time_lbl();
    p->dur_lbl = make_time_lbl();
    gtk_widget_set_halign(GTK_WIDGET(p->dur_lbl), GTK_ALIGN_END);
    gtk_widget_set_hexpand(GTK_WIDGET(p->dur_lbl), TRUE);
    GtkWidget *timerow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(timerow), GTK_WIDGET(p->pos_lbl));
    gtk_box_append(GTK_BOX(timerow), GTK_WIDGET(p->dur_lbl));
    GtkWidget *sbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(sbox), GTK_WIDGET(p->seek_scale));
    gtk_box_append(GTK_BOX(sbox), timerow);

    p->shuffle_btn = make_icon_btn("media-playlist-shuffle-symbolic", "Shuffle (S)");
    p->prev_btn = make_icon_btn("media-skip-backward-symbolic", "Previous (P)");
    p->play_btn = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-start-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(p->play_btn), "Play / Pause (Space)");
    gtk_widget_set_valign(GTK_WIDGET(p->play_btn), GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(GTK_WIDGET(p->play_btn), "sur-play-button");
    p->next_btn = make_icon_btn("media-skip-forward-symbolic", "Next (N)");
    p->repeat_btn = make_icon_btn("media-playlist-repeat-symbolic", "Repeat (L)");
    GtkWidget *cctr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_halign(cctr, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(cctr, TRUE);
    gtk_box_append(GTK_BOX(cctr), GTK_WIDGET(p->prev_btn));
    gtk_box_append(GTK_BOX(cctr), GTK_WIDGET(p->play_btn));
    gtk_box_append(GTK_BOX(cctr), GTK_WIDGET(p->next_btn));
    GtkWidget *crow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(crow), GTK_WIDGET(p->shuffle_btn));
    gtk_box_append(GTK_BOX(crow), cctr);
    gtk_box_append(GTK_BOX(crow), GTK_WIDGET(p->repeat_btn));

    GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(pbox, 24); gtk_widget_set_margin_end(pbox, 24);
    gtk_widget_set_margin_top(pbox, 14); gtk_widget_set_margin_bottom(pbox, 24);
    gtk_widget_set_valign(pbox, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(pbox), cov);
    gtk_box_append(GTK_BOX(pbox), mbox);
    gtk_box_append(GTK_BOX(pbox), sbox);
    gtk_box_append(GTK_BOX(pbox), crow);
    GtkWidget *pclamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(pclamp), 420);
    adw_clamp_set_child(ADW_CLAMP(pclamp), pbox);

    /* Online view */
    p->search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(p->search_entry, "Search YouTube or paste a link…");
    gtk_widget_set_hexpand(GTK_WIDGET(p->search_entry), TRUE);
    p->online_spinner = GTK_SPINNER(gtk_spinner_new());
    gtk_widget_set_visible(GTK_WIDGET(p->online_spinner), FALSE);
    GtkWidget *srow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(srow), GTK_WIDGET(p->search_entry));
    gtk_box_append(GTK_BOX(srow), GTK_WIDGET(p->online_spinner));
    char *ytdlp = sur_find_yt_dlp();
    p->online_status = GTK_LABEL(gtk_label_new(
        ytdlp ? "Press Enter to search, or paste any YouTube / YouTube Music URL"
              : "Install yt-dlp to search, stream, and download audio"));
    g_free(ytdlp);
    gtk_label_set_wrap(p->online_status, TRUE);
    gtk_widget_set_halign(GTK_WIDGET(p->online_status), GTK_ALIGN_START);
    gtk_widget_add_css_class(GTK_WIDGET(p->online_status), "sur-subtitle");
    p->online_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(p->online_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(p->online_list), "sur-online-list");
    GtkWidget *oscroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(oscroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(oscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(oscroll), GTK_WIDGET(p->online_list));
    char *ddir = sur_download_dir();
    char *hint_txt = g_strdup_printf("Saves best native audio (.opus / .m4a) with cover art to %s", ddir);
    GtkWidget *shint = gtk_label_new(hint_txt);
    g_free(ddir); g_free(hint_txt);
    gtk_label_set_ellipsize(GTK_LABEL(shint), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(shint, GTK_ALIGN_START);
    gtk_widget_add_css_class(shint, "sur-time");
    GtkWidget *obox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(obox, 16); gtk_widget_set_margin_end(obox, 16);
    gtk_widget_set_margin_top(obox, 6); gtk_widget_set_margin_bottom(obox, 14);
    gtk_box_append(GTK_BOX(obox), srow);
    gtk_box_append(GTK_BOX(obox), GTK_WIDGET(p->online_status));
    gtk_box_append(GTK_BOX(obox), oscroll);
    gtk_box_append(GTK_BOX(obox), shint);

    /* History / Downloaded view */
    p->hist_recent_tab = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Recent"));
    gtk_toggle_button_set_active(p->hist_recent_tab, TRUE);
    p->hist_dl_tab = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Downloaded"));
    gtk_toggle_button_set_group(p->hist_dl_tab, p->hist_recent_tab);
    GtkWidget *htabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(htabs, "linked");
    gtk_box_append(GTK_BOX(htabs), GTK_WIDGET(p->hist_recent_tab));
    gtk_box_append(GTK_BOX(htabs), GTK_WIDGET(p->hist_dl_tab));
    p->hist_clear_btn = GTK_BUTTON(gtk_button_new_with_label("Clear"));
    gtk_widget_add_css_class(GTK_WIDGET(p->hist_clear_btn), "flat");
    gtk_widget_set_visible(GTK_WIDGET(p->hist_clear_btn), FALSE);
    p->hist_play_all_btn = GTK_BUTTON(gtk_button_new_with_label("Play All"));
    gtk_widget_add_css_class(GTK_WIDGET(p->hist_play_all_btn), "flat");
    gtk_widget_set_visible(GTK_WIDGET(p->hist_play_all_btn), FALSE);
    GtkWidget *hsp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(hsp, TRUE);
    GtkWidget *hhdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(hhdr), htabs);
    gtk_box_append(GTK_BOX(hhdr), hsp);
    gtk_box_append(GTK_BOX(hhdr), GTK_WIDGET(p->hist_play_all_btn));
    gtk_box_append(GTK_BOX(hhdr), GTK_WIDGET(p->hist_clear_btn));
    p->hist_empty_lbl = GTK_LABEL(gtk_label_new("No recent tracks yet"));
    gtk_widget_set_margin_top(GTK_WIDGET(p->hist_empty_lbl), 48);
    gtk_widget_add_css_class(GTK_WIDGET(p->hist_empty_lbl), "sur-subtitle");
    p->hist_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(p->hist_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(p->hist_list), "boxed-list");
    GtkWidget *hinner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(hinner), GTK_WIDGET(p->hist_empty_lbl));
    gtk_box_append(GTK_BOX(hinner), GTK_WIDGET(p->hist_list));
    GtkWidget *hscroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(hscroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(hscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(hscroll), hinner);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(hbox, 16); gtk_widget_set_margin_end(hbox, 16);
    gtk_widget_set_margin_top(hbox, 8); gtk_widget_set_margin_bottom(hbox, 14);
    gtk_box_append(GTK_BOX(hbox), hhdr);
    gtk_box_append(GTK_BOX(hbox), hscroll);

    /* Stack + Queue SplitView */
    p->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(p->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(p->stack, 180);
    gtk_stack_add_named(p->stack, GTK_WIDGET(p->start_page), "start");
    gtk_stack_add_named(p->stack, pclamp, "playing");
    gtk_stack_add_named(p->stack, obox, "online");
    gtk_stack_add_named(p->stack, hbox, "history");

    GtkWidget *qhead = gtk_label_new("Up Next");
    gtk_widget_set_halign(qhead, GTK_ALIGN_START);
    gtk_widget_set_hexpand(qhead, TRUE);
    gtk_widget_add_css_class(qhead, "heading");
    p->queue_count_lbl = GTK_LABEL(gtk_label_new("0 tracks"));
    gtk_widget_add_css_class(GTK_WIDGET(p->queue_count_lbl), "sur-time");
    GtkWidget *qhr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(qhr, 12); gtk_widget_set_margin_end(qhr, 12);
    gtk_widget_set_margin_top(qhr, 12); gtk_widget_set_margin_bottom(qhr, 6);
    gtk_box_append(GTK_BOX(qhr), qhead);
    gtk_box_append(GTK_BOX(qhr), GTK_WIDGET(p->queue_count_lbl));
    p->queue_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(p->queue_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(p->queue_list), "sur-queue-list");
    GtkWidget *qscroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(qscroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(qscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(qscroll), GTK_WIDGET(p->queue_list));
    GtkWidget *qside = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(qside, 240, -1);
    gtk_widget_add_css_class(qside, "sur-queue-sidebar");
    gtk_box_append(GTK_BOX(qside), qhr);
    gtk_box_append(GTK_BOX(qside), qscroll);

    p->split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar_position(p->split, GTK_PACK_END);
    adw_overlay_split_view_set_show_sidebar(p->split, FALSE);
    adw_overlay_split_view_set_max_sidebar_width(p->split, 270.0);
    adw_overlay_split_view_set_min_sidebar_width(p->split, 220.0);
    adw_overlay_split_view_set_content(p->split, GTK_WIDGET(p->stack));
    adw_overlay_split_view_set_sidebar(p->split, qside);

    GtkWidget *tb = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tb), hdr);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tb), GTK_WIDGET(p->split));
    p->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(p->toasts, tb);
    adw_application_window_set_content(p->window, GTK_WIDGET(p->toasts));

    /* Actions */
    static const GActionEntry ACTS[] = {
        {"open", on_act_open, NULL, NULL, NULL, {0}},
        {"open-folder", on_act_open_folder, NULL, NULL, NULL, {0}},
        {"search", on_act_search, NULL, NULL, NULL, {0}},
        {"history", on_act_history, NULL, NULL, NULL, {0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(win), ACTS, G_N_ELEMENTS(ACTS), p);

    /* Signals */
    g_signal_connect(p->play_btn, "clicked", G_CALLBACK(on_play_clicked), p);
    g_signal_connect(p->prev_btn, "clicked", G_CALLBACK(on_prev_clicked), p);
    g_signal_connect(p->next_btn, "clicked", G_CALLBACK(on_next_clicked), p);
    g_signal_connect(p->shuffle_btn, "clicked", G_CALLBACK(on_shuffle_clicked), p);
    g_signal_connect(p->repeat_btn, "clicked", G_CALLBACK(on_repeat_clicked), p);
    g_signal_connect(p->save_stream_btn, "clicked", G_CALLBACK(on_save_cur_clicked), p);
    g_signal_connect(p->mute_btn, "clicked", G_CALLBACK(on_mute_clicked), p);
    g_signal_connect(p->volume_scale, "value-changed", G_CALLBACK(on_volume_changed), p);
    g_signal_connect(p->seek_scale, "change-value", G_CALLBACK(on_seek_change), p);
    g_signal_connect(p->queue_btn, "toggled", G_CALLBACK(on_queue_toggled), p);
    g_signal_connect(p->split, "notify::show-sidebar", G_CALLBACK(on_split_changed), p);
    g_signal_connect(p->search_btn, "toggled", G_CALLBACK(on_search_toggled), p);
    g_signal_connect(p->history_btn, "toggled", G_CALLBACK(on_history_toggled), p);
    g_signal_connect(p->hist_recent_tab, "toggled", G_CALLBACK(on_hist_tab_toggled), p);
    g_signal_connect(p->hist_dl_tab, "toggled", G_CALLBACK(on_hist_tab_toggled), p);
    g_signal_connect(p->hist_clear_btn, "clicked", G_CALLBACK(on_hist_clear), p);
    g_signal_connect(p->hist_play_all_btn, "clicked", G_CALLBACK(on_home_play_all), p);
    g_signal_connect(p->search_entry, "activate", G_CALLBACK(on_search_activate), p);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), p);
    gtk_widget_add_controller(win, keys);

    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(dt, "drop", G_CALLBACK(on_drop), p);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(dt));

    g_signal_connect(win, "close-request", G_CALLBACK(on_close_req), p);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), p);

    refresh_home(p);
    p->mpris = sur_mpris_new(p, &MPRIS_VT);
    g_player = p;
    gtk_window_present(GTK_WINDOW(win));
    return p;
}

static void on_startup(GApplication *app, gpointer ud) {
    (void)ud;
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
    const char *o1[] = {"<Control>o", NULL};
    const char *o2[] = {"<Control><Shift>o", NULL};
    const char *o3[] = {"<Control>f", NULL};
    const char *o4[] = {"<Control>h", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", o1);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open-folder", o2);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.search", o3);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.history", o4);

    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_string(prov, BASE_CSS);
    GdkDisplay *d = gdk_display_get_default();
    if (d) gtk_style_context_add_provider_for_display(d, GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
}

static void on_activate(GApplication *app, gpointer ud) {
    (void)ud;
    SurPlayer *p = ensure_player(ADW_APPLICATION(app));
    gtk_window_present(GTK_WINDOW(p->window));
}

static void on_open(GApplication *app, GFile **files, gint n_files, const char *hint, gpointer ud) {
    (void)hint; (void)ud;
    SurPlayer *p = ensure_player(ADW_APPLICATION(app));
    open_files(p, files, n_files);
    gtk_window_present(GTK_WINDOW(p->window));
}

static gboolean on_unix_signal(gpointer ud) {
    GApplication *app = ud;
    GList *wins = g_list_copy(gtk_application_get_windows(GTK_APPLICATION(app)));
    if (!wins) g_application_quit(app);
    for (GList *l = wins; l; l = l->next) {
        GtkWindow *w = l->data;
        if (gtk_widget_get_visible(GTK_WIDGET(w))) gtk_window_close(w);
        else gtk_window_destroy(w);
    }
    g_list_free(wins);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
    AdwApplication *app = adw_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, app);
    g_unix_signal_add(SIGTERM, on_unix_signal, app);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
