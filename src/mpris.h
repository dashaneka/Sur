#ifndef SUR_MPRIS_H
#define SUR_MPRIS_H

#include "core.h"

typedef struct SurPlayer SurPlayer;
typedef struct SurMpris SurMpris;

typedef struct {
    const char *(*get_status)(SurPlayer *p);
    SurRepeatMode (*get_repeat)(SurPlayer *p);
    bool (*get_shuffle)(SurPlayer *p);
    bool (*can_next)(SurPlayer *p);
    bool (*can_prev)(SurPlayer *p);
    bool (*has_track)(SurPlayer *p);
    double (*get_volume)(SurPlayer *p);
    double (*get_position)(SurPlayer *p);
    GVariant *(*build_metadata)(SurPlayer *p);
    void (*set_repeat)(SurPlayer *p, SurRepeatMode mode);
    void (*set_shuffle)(SurPlayer *p, bool enabled);
    void (*set_volume)(SurPlayer *p, double vol);
    void (*raise)(SurPlayer *p);
    void (*quit)(SurPlayer *p);
    void (*play)(SurPlayer *p);
    void (*pause)(SurPlayer *p);
    void (*stop)(SurPlayer *p);
    void (*play_pause)(SurPlayer *p);
    void (*next)(SurPlayer *p);
    void (*prev)(SurPlayer *p);
    void (*seek)(SurPlayer *p, double offset_secs);
    void (*set_position)(SurPlayer *p, double pos_secs);
    void (*open_uri)(SurPlayer *p, const char *uri);
} SurMprisVTable;

SurMpris *sur_mpris_new(SurPlayer *player, const SurMprisVTable *vt);
void sur_mpris_changed(SurMpris *m, const char *const *props);
void sur_mpris_seeked(SurMpris *m, double secs);
void sur_mpris_close(SurMpris *m);

#endif
