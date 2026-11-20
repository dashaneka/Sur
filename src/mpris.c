#define _POSIX_C_SOURCE 200809L

#include "mpris.h"

#include <string.h>
#include <unistd.h>

#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"
#define MPRIS_TRACK "/io/github/rajnish/Sur/Track"
#define SUR_APP_ID "io.github.rajnish.Sur"

static const char MPRIS_XML[] =
    "<node>"
    "<interface name='org.mpris.MediaPlayer2'>"
    "<method name='Raise'/><method name='Quit'/>"
    "<property name='CanQuit' type='b' access='read'/>"
    "<property name='CanRaise' type='b' access='read'/>"
    "<property name='HasTrackList' type='b' access='read'/>"
    "<property name='Identity' type='s' access='read'/>"
    "<property name='DesktopEntry' type='s' access='read'/>"
    "<property name='SupportedUriSchemes' type='as' access='read'/>"
    "<property name='SupportedMimeTypes' type='as' access='read'/>"
    "</interface>"
    "<interface name='org.mpris.MediaPlayer2.Player'>"
    "<method name='Next'/><method name='Previous'/><method name='Pause'/>"
    "<method name='PlayPause'/><method name='Stop'/><method name='Play'/>"
    "<method name='Seek'><arg direction='in' name='Offset' type='x'/></method>"
    "<method name='SetPosition'><arg direction='in' name='TrackId' type='o'/>"
    "<arg direction='in' name='Position' type='x'/></method>"
    "<method name='OpenUri'><arg direction='in' name='Uri' type='s'/></method>"
    "<signal name='Seeked'><arg name='Position' type='x'/></signal>"
    "<property name='PlaybackStatus' type='s' access='read'/>"
    "<property name='LoopStatus' type='s' access='readwrite'/>"
    "<property name='Rate' type='d' access='readwrite'/>"
    "<property name='Shuffle' type='b' access='readwrite'/>"
    "<property name='Metadata' type='a{sv}' access='read'/>"
    "<property name='Volume' type='d' access='readwrite'/>"
    "<property name='Position' type='x' access='read'/>"
    "<property name='MinimumRate' type='d' access='read'/>"
    "<property name='MaximumRate' type='d' access='read'/>"
    "<property name='CanGoNext' type='b' access='read'/>"
    "<property name='CanGoPrevious' type='b' access='read'/>"
    "<property name='CanPlay' type='b' access='read'/>"
    "<property name='CanPause' type='b' access='read'/>"
    "<property name='CanSeek' type='b' access='read'/>"
    "<property name='CanControl' type='b' access='read'/>"
    "</interface></node>";

struct SurMpris {
    GDBusConnection *conn;
    guint reg_root;
    guint reg_player;
    guint owner_id;
    SurPlayer *player;
    const SurMprisVTable *vt;
};

static GVariant *prop_value(SurMpris *m, const char *iface, const char *prop) {
    SurPlayer *p = m->player;
    const SurMprisVTable *vt = m->vt;
    if (strcmp(iface, MPRIS_ROOT) == 0) {
        if (strcmp(prop, "CanQuit") == 0 || strcmp(prop, "CanRaise") == 0)
            return g_variant_new_boolean(TRUE);
        if (strcmp(prop, "HasTrackList") == 0) return g_variant_new_boolean(FALSE);
        if (strcmp(prop, "Identity") == 0) return g_variant_new_string("Sur");
        if (strcmp(prop, "DesktopEntry") == 0) return g_variant_new_string(SUR_APP_ID);
        if (strcmp(prop, "SupportedUriSchemes") == 0) {
            const char *s[] = {"file", NULL};
            return g_variant_new_strv(s, -1);
        }
        if (strcmp(prop, "SupportedMimeTypes") == 0) {
            const char *s[] = {"audio/mpeg", "audio/flac", "audio/ogg", "audio/opus", "audio/mp4", "audio/wav", NULL};
            return g_variant_new_strv(s, -1);
        }
        return NULL;
    }
    if (strcmp(prop, "PlaybackStatus") == 0) return g_variant_new_string(vt->get_status(p));
    if (strcmp(prop, "LoopStatus") == 0) return g_variant_new_string(sur_repeat_mpris(vt->get_repeat(p)));
    if (strcmp(prop, "Rate") == 0 || strcmp(prop, "MinimumRate") == 0 || strcmp(prop, "MaximumRate") == 0)
        return g_variant_new_double(1.0);
    if (strcmp(prop, "Shuffle") == 0) return g_variant_new_boolean(vt->get_shuffle(p));
    if (strcmp(prop, "Metadata") == 0) return vt->build_metadata(p);
    if (strcmp(prop, "Volume") == 0) return g_variant_new_double(vt->get_volume(p));
    if (strcmp(prop, "Position") == 0) return g_variant_new_int64((gint64)(vt->get_position(p) * 1e6));
    if (strcmp(prop, "CanGoNext") == 0) return g_variant_new_boolean(vt->can_next(p));
    if (strcmp(prop, "CanGoPrevious") == 0) return g_variant_new_boolean(vt->can_prev(p));
    if (strcmp(prop, "CanPlay") == 0 || strcmp(prop, "CanPause") == 0 || strcmp(prop, "CanSeek") == 0)
        return g_variant_new_boolean(vt->has_track(p));
    if (strcmp(prop, "CanControl") == 0) return g_variant_new_boolean(TRUE);
    return NULL;
}

static GVariant *on_get_prop(GDBusConnection *c, const char *s, const char *o,
                             const char *iface, const char *prop, GError **err, gpointer ud) {
    (void)c; (void)s; (void)o; (void)err;
    GVariant *v = prop_value((SurMpris *)ud, iface, prop);
    return v ? v : g_variant_new_boolean(FALSE);
}

static gboolean on_set_prop(GDBusConnection *c, const char *s, const char *o,
                            const char *iface, const char *prop, GVariant *val,
                            GError **err, gpointer ud) {
    (void)c; (void)s; (void)o; (void)iface; (void)err;
    SurMpris *m = ud;
    if (strcmp(prop, "LoopStatus") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING))
        m->vt->set_repeat(m->player, sur_repeat_from_mpris(g_variant_get_string(val, NULL)));
    else if (strcmp(prop, "Shuffle") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_BOOLEAN))
        m->vt->set_shuffle(m->player, g_variant_get_boolean(val));
    else if (strcmp(prop, "Volume") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_DOUBLE)) {
        double v = g_variant_get_double(val);
        m->vt->set_volume(m->player, v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
    }
    return TRUE;
}

static void on_method(GDBusConnection *c, const char *s, const char *o, const char *iface,
                      const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer ud) {
    (void)c; (void)s; (void)o; (void)iface;
    SurMpris *m = ud;
    SurPlayer *p = m->player;
    const SurMprisVTable *vt = m->vt;
    if (strcmp(method, "Raise") == 0) vt->raise(p);
    else if (strcmp(method, "Quit") == 0) vt->quit(p);
    else if (strcmp(method, "Play") == 0) vt->play(p);
    else if (strcmp(method, "Pause") == 0) vt->pause(p);
    else if (strcmp(method, "Stop") == 0) vt->stop(p);
    else if (strcmp(method, "PlayPause") == 0) vt->play_pause(p);
    else if (strcmp(method, "Next") == 0) vt->next(p);
    else if (strcmp(method, "Previous") == 0) vt->prev(p);
    else if (strcmp(method, "Seek") == 0) {
        gint64 off = 0;
        g_variant_get(params, "(x)", &off);
        vt->seek(p, (double)off / 1e6);
    } else if (strcmp(method, "SetPosition") == 0) {
        const char *tid = NULL;
        gint64 pos = 0;
        g_variant_get(params, "(&ox)", &tid, &pos);
        if (tid && strcmp(tid, MPRIS_TRACK) == 0) vt->set_position(p, (double)pos / 1e6);
    } else if (strcmp(method, "OpenUri") == 0) {
        const char *uri = NULL;
        g_variant_get(params, "(&s)", &uri);
        if (uri) vt->open_uri(p, uri);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable DBUS_VT = {on_method, on_get_prop, on_set_prop, {0}};
static guint g_inst = 0;

SurMpris *sur_mpris_new(SurPlayer *player, const SurMprisVTable *vt) {
    char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!addr) return NULL;
    GDBusConnection *conn = g_dbus_connection_new_for_address_sync(
        addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_free(addr);
    if (!conn) return NULL;

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(MPRIS_XML, NULL);
    if (!node) {
        g_dbus_connection_close(conn, NULL, NULL, NULL);
        g_object_unref(conn);
        return NULL;
    }

    SurMpris *m = g_new0(SurMpris, 1);
    m->conn = conn;
    m->player = player;
    m->vt = vt;
    m->reg_root = g_dbus_connection_register_object(
        conn, MPRIS_PATH, g_dbus_node_info_lookup_interface(node, MPRIS_ROOT), &DBUS_VT, m, NULL, NULL);
    m->reg_player = g_dbus_connection_register_object(
        conn, MPRIS_PATH, g_dbus_node_info_lookup_interface(node, MPRIS_PLAYER), &DBUS_VT, m, NULL, NULL);
    g_dbus_node_info_unref(node);

    if (!m->reg_root || !m->reg_player) {
        sur_mpris_close(m);
        return NULL;
    }
    char *name = g_strdup_printf("%s.sur.instance%d_%u", MPRIS_ROOT, (int)getpid(), g_inst++);
    m->owner_id = g_bus_own_name_on_connection(conn, name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
    g_free(name);
    return m;
}

void sur_mpris_changed(SurMpris *m, const char *const *props) {
    if (!m || !m->conn) return;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    for (int i = 0; props[i]; i++) {
        GVariant *v = prop_value(m, MPRIS_PLAYER, props[i]);
        if (v) g_variant_builder_add(&b, "{sv}", props[i], v);
    }
    GVariantBuilder inv;
    g_variant_builder_init(&inv, G_VARIANT_TYPE_STRING_ARRAY);
    g_dbus_connection_emit_signal(
        m->conn, NULL, MPRIS_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", MPRIS_PLAYER, &b, &inv), NULL);
}

void sur_mpris_seeked(SurMpris *m, double secs) {
    if (!m || !m->conn) return;
    g_dbus_connection_emit_signal(
        m->conn, NULL, MPRIS_PATH, MPRIS_PLAYER, "Seeked",
        g_variant_new("(x)", (gint64)(secs * 1e6)), NULL);
}

void sur_mpris_close(SurMpris *m) {
    if (!m) return;
    if (m->owner_id) g_bus_unown_name(m->owner_id);
    if (m->conn) {
        if (m->reg_root) g_dbus_connection_unregister_object(m->conn, m->reg_root);
        if (m->reg_player) g_dbus_connection_unregister_object(m->conn, m->reg_player);
        g_dbus_connection_close(m->conn, NULL, NULL, NULL);
        g_object_unref(m->conn);
    }
    g_free(m);
}
