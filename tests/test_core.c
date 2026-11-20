#define _POSIX_C_SOURCE 200809L

#include "../src/core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_natural_sort(void) {
    assert(sur_natural_cmp("track 1.flac", "Track 2.flac") < 0);
    assert(sur_natural_cmp("Track 2.flac", "Track 10.flac") < 0);
    assert(sur_natural_cmp("Track 10.flac", "Track 20.flac") < 0);
    assert(sur_natural_cmp("same.mp3", "SAME.MP3") == 0);
}

static void test_audio_extensions(void) {
    assert(sur_is_audio_path("song.FLAC"));
    assert(sur_is_audio_path("track.Opus"));
    assert(sur_is_audio_path("/music/clip.m4a"));
    assert(!sur_is_audio_path("cover.jpg"));
    assert(!sur_is_audio_path("notes.txt"));
    assert(!sur_is_audio_path("no_extension"));
}

static void test_queue_repeat_and_shuffle(void) {
    SurQueue q;
    sur_queue_init(&q);

    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)sur_track_free);
    for (int i = 0; i < 10; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/music/track_%d.flac", i);
        g_ptr_array_add(tracks, sur_track_new_local(path));
    }
    sur_queue_set_tracks(&q, tracks, 4);
    assert(sur_queue_current_track_index(&q) == 4);

    sur_queue_set_shuffle(&q, true);
    assert(q.pos == 0);
    assert(sur_queue_current_track_index(&q) == 4);
    assert(q.tracks->len == 10);

    sur_queue_set_shuffle(&q, false);
    assert(q.pos == 4);
    assert(sur_queue_current_track_index(&q) == 4);

    GPtrArray *two = g_ptr_array_new_with_free_func((GDestroyNotify)sur_track_free);
    g_ptr_array_add(two, sur_track_new_local("/m/a.opus"));
    g_ptr_array_add(two, sur_track_new_local("/m/b.opus"));
    sur_queue_set_tracks(&q, two, 0);
    assert(strcmp(sur_queue_advance(&q, false)->title, "b") == 0);
    assert(sur_queue_advance(&q, false) == NULL);
    q.repeat = SUR_REPEAT_ALL;
    assert(strcmp(sur_queue_advance(&q, false)->title, "a") == 0);
    q.repeat = SUR_REPEAT_ONE;
    assert(strcmp(sur_queue_advance(&q, false)->title, "a") == 0);
    assert(strcmp(sur_queue_advance(&q, true)->title, "b") == 0);

    sur_queue_clear(&q);
}

static void test_online_parsers(void) {
    assert(sur_looks_like_url("https://music.youtube.com/watch?v=abc"));
    assert(!sur_looks_like_url("lofi beats"));

    char *raw = g_strdup_printf(
        "Midnight Rain%cCalm Keys%c224.0%chttps://www.youtube.com/watch?v=abc123%chttps://i.ytimg.com/a.jpg\n"
        "Short ID%cNA%cNA%cxyz789%cNA\n",
        SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP);
    GPtrArray *res = sur_parse_search_output(raw);
    g_free(raw);
    assert(res->len == 2);
    SurSearchResult *r0 = g_ptr_array_index(res, 0);
    SurSearchResult *r1 = g_ptr_array_index(res, 1);
    assert(strcmp(r0->title, "Midnight Rain") == 0);
    assert(r0->duration_secs == 224);
    assert(strcmp(r1->page_url, "https://www.youtube.com/watch?v=xyz789") == 0);
    assert(strcmp(r1->artist, "Unknown Artist") == 0);
    assert(r1->thumbnail == NULL);
    g_ptr_array_unref(res);

    char *line = g_strdup_printf(
        "https://example.invalid/a.opus%cRain%cStudio%c185%chttps://example.invalid/t.jpg%copus",
        SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP, SUR_SEP);
    SurResolvedStream *rs = sur_parse_resolved_line(line);
    g_free(line);
    assert(rs != NULL);
    assert(strcmp(rs->title, "Rain") == 0);
    assert(strcmp(rs->codec, "OPUS") == 0);
    assert(rs->duration_secs == 185);
    sur_resolved_stream_free(rs);
}

static void test_history_roundtrip(void) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/sur-nano-hist-test-%d.tsv", (int)getpid());
    remove(path);

    SurHistoryItem *a = g_new0(SurHistoryItem, 1);
    a->is_online = true;
    a->path_or_url = g_strdup("https://example.invalid/a");
    a->title = g_strdup("Track A");
    a->artist_or_sub = g_strdup("Artist A");
    a->duration_secs = 123;
    sur_history_prepend_file(path, a);

    SurHistoryItem *b = g_new0(SurHistoryItem, 1);
    b->is_online = false;
    b->path_or_url = g_strdup("/music/Sur/01.opus");
    b->title = g_strdup("01");
    b->artist_or_sub = g_strdup("Sur");
    b->duration_secs = -1;
    sur_history_prepend_file(path, b);

    SurHistoryItem *a2 = g_new0(SurHistoryItem, 1);
    a2->is_online = true;
    a2->path_or_url = g_strdup("https://example.invalid/a");
    a2->title = g_strdup("Track A");
    a2->artist_or_sub = g_strdup("Artist A");
    a2->duration_secs = 123;
    sur_history_prepend_file(path, a2);

    char *txt = NULL;
    assert(g_file_get_contents(path, &txt, NULL, NULL));
    GPtrArray *loaded = sur_history_parse(txt);
    g_free(txt);
    remove(path);
    assert(loaded->len == 2);
    SurHistoryItem *first = g_ptr_array_index(loaded, 0);
    assert(strcmp(first->title, "Track A") == 0);
    g_ptr_array_unref(loaded);
}

static void test_palette_and_css(void) {
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 64, 64);
    gdk_pixbuf_fill(pb, 0xe85038ff);
    uint8_t acc[3], tint[3];
    sur_extract_palette(pb, acc, tint);
    assert(acc[0] > acc[2]);
    char *css = sur_tint_css(acc, tint);
    assert(strstr(css, "sur-play-button") != NULL);
    g_free(css);

    gdk_pixbuf_fill(pb, 0x040404ff);
    sur_extract_palette(pb, acc, tint);
    uint8_t dacc[3], dtint[3];
    sur_default_palette(dacc, dtint);
    assert(memcmp(acc, dacc, 3) == 0);
    g_object_unref(pb);
}

int main(void) {
    test_natural_sort();
    test_audio_extensions();
    test_queue_repeat_and_shuffle();
    test_online_parsers();
    test_history_roundtrip();
    test_palette_and_css();
    printf("All Sur-Nano unit tests passed!\n");
    return 0;
}
