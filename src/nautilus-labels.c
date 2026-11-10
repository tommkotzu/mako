/*
 * nautilus-labels.c: star ratings, tags and folder colors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "nautilus-labels"

#include "nautilus-labels.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>

#include "nautilus-file.h"
#include "nautilus-metadata.h"
#include "nautilus-ui-utilities.h"

/* ------------------------------------------------------------ Schemes */

typedef struct
{
    const char *name;
    const char *slot_names[NAUTILUS_FOLDER_COLOR_SLOTS];
    guint32 colors[NAUTILUS_FOLDER_COLOR_SLOTS]; /* all 0 for Mono */
} Scheme;

/* The same schemes as File Flier, in the same order. */
static const Scheme schemes[] =
{
    {
        N_("Pastel"),
        { N_("Red"), N_("Orange"), N_("Yellow"), N_("Green"), N_("Teal"), N_("Blue"), N_("Purple"), N_("Pink") },
        { 0xFFA3A3, 0xFFC396, 0xFAE394, 0xB2E2A9, 0xA0DFD7, 0xA4C9F9, 0xCBB3F5, 0xFBB2D7 },
    },
    {
        N_("Vibrant"),
        { N_("Red"), N_("Orange"), N_("Yellow"), N_("Green"), N_("Teal"), N_("Blue"), N_("Purple"), N_("Pink") },
        { 0xE5484D, 0xF76B15, 0xFFC53D, 0x30A46C, 0x12A594, 0x0090FF, 0x8E4EC6, 0xD6409F },
    },
    {
        N_("Earth"),
        { N_("Terracotta"), N_("Clay"), N_("Ochre"), N_("Sand"), N_("Olive"), N_("Moss"), N_("Sage"), N_("Bark") },
        { 0xD0805F, 0xC9A27E, 0xD9A845, 0xE3D3A6, 0xA3A65A, 0x7E9A62, 0xA9BFA0, 0x8C6E5A },
    },
    {
        N_("Ocean"),
        { N_("Navy"), N_("Blue"), N_("Sky"), N_("Ice"), N_("Teal"), N_("Seafoam"), N_("Kelp"), N_("Slate") },
        { 0x3D5A8A, 0x4F86C6, 0x8EC3E8, 0xC8E3EE, 0x3FA7A0, 0x8FD3BF, 0x5F8F6E, 0x7D8DA3 },
    },
    {
        N_("Mono"),
        { N_("Lightest"), N_("Lighter"), N_("Light"), N_("Soft"), N_("Accent"), N_("Deep"), N_("Darker"), N_("Darkest") },
        { 0 },
    },
};

#define MONO_SCHEME 4

/* Colors new tags get, in turn (File Flier's). */
static const guint32 tag_palette[] =
{
    0x3584E4, 0x2EC27E, 0xF6D32D, 0xFF7800, 0xE01B24, 0x9141AC, 0x986A44, 0x5E5C64,
};

static GdkRGBA
rgba_from_hex (guint32 hex)
{
    return (GdkRGBA) {
        ((hex >> 16) & 0xff) / 255.0f,
        ((hex >> 8) & 0xff) / 255.0f,
        (hex & 0xff) / 255.0f,
        1.0f
    };
}

static GdkRGBA
mix (GdkRGBA a,
     GdkRGBA b,
     float   t)
{
    return (GdkRGBA) {
        a.red + (b.red - a.red) * t,
        a.green + (b.green - a.green) * t,
        a.blue + (b.blue - a.blue) * t,
        1.0f
    };
}

/* -------------------------------------------------------- The object */

typedef struct
{
    char *name;
    GdkRGBA color;
} TagDef;

struct _NautilusLabels
{
    GObject parent_instance;

    GPtrArray *tags; /* TagDef */
    gboolean tags_loaded;
    gboolean alt_held;

    int scheme;
    GdkRGBA scheme_colors[NAUTILUS_FOLDER_COLOR_SLOTS];
};

G_DEFINE_FINAL_TYPE (NautilusLabels, nautilus_labels, G_TYPE_OBJECT)

enum
{
    PROP_0,
    PROP_ALT_HELD,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum
{
    TAGS_CHANGED,
    COLORS_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
tag_def_free (TagDef *def)
{
    g_free (def->name);
    g_free (def);
}

GSettings *
nautilus_flier_settings (void)
{
    static GSettings *settings = NULL;

    if (g_once_init_enter_pointer (&settings))
    {
        g_once_init_leave_pointer (&settings, g_settings_new ("org.gnome.nautilus.flier"));
    }

    return settings;
}

guint
nautilus_density_icon_size (guint size)
{
    /* The standard icon sizes, which GNOME Files' zoom levels also use. */
    static const guint sizes[] = { 16, 24, 32, 48, 64, 96, 128, 192, 256 };
    /* How many sizes down: comfortable, compact, dense, extra-dense. */
    static const guint steps[] = { 0, 1, 1, 2 };
    guint density = g_settings_get_enum (nautilus_flier_settings (), "density");
    guint down = density < G_N_ELEMENTS (steps) ? steps[density] : 0;

    for (guint i = 0; i < down; i++)
    {
        guint smaller = sizes[0];

        for (guint j = 0; j < G_N_ELEMENTS (sizes) && sizes[j] < size; j++)
        {
            smaller = sizes[j];
        }
        size = smaller;
    }
    return size;
}

static void
update_scheme_colors (NautilusLabels *self)
{
    self->scheme = CLAMP (g_settings_get_enum (nautilus_flier_settings (), "folder-color-scheme"),
                          0, (int) G_N_ELEMENTS (schemes) - 1);
    nautilus_labels_get_scheme_colors (self->scheme, self->scheme_colors);
}

static void
on_colors_changed (NautilusLabels *self)
{
    update_scheme_colors (self);
    g_signal_emit (self, signals[COLORS_CHANGED], 0);
}

NautilusLabels *
nautilus_labels_get (void)
{
    static NautilusLabels *labels = NULL;

    if (labels == NULL)
    {
        labels = g_object_new (NAUTILUS_TYPE_LABELS, NULL);
    }

    return labels;
}

/* ------------------------------------------------- Ratings and tags */

typedef struct
{
    GFile *location;
    gboolean write_rating;
    guint rating;
    gboolean write_tags;
    GStrv tags;
} MarkWrite;

static void
mark_write_free (MarkWrite *write)
{
    g_object_unref (write->location);
    g_strfreev (write->tags);
    g_free (write);
}

/* GIO takes xattr values escaped (see unescape_xattr() in nautilus-file.c). */
static char *
escape_xattr (const char *value)
{
    GString *out = g_string_sized_new (strlen (value));

    for (const guchar *p = (const guchar *) value; *p != '\0'; p++)
    {
        if (*p < 0x20 || *p > 0x7e || *p == '\\')
        {
            g_string_append_printf (out, "\\x%02x", *p);
        }
        else
        {
            g_string_append_c (out, (char) *p);
        }
    }

    return g_string_free (out, FALSE);
}

static void
set_or_remove_xattr (GFile      *location,
                     const char *attribute,
                     const char *value)
{
    g_autoptr (GError) error = NULL;

    if (value != NULL)
    {
        g_autofree char *escaped = escape_xattr (value);

        g_file_set_attribute_string (location, attribute, escaped,
                                     G_FILE_QUERY_INFO_NONE, NULL, &error);
    }
    else
    {
        g_file_set_attribute (location, attribute, G_FILE_ATTRIBUTE_TYPE_INVALID, NULL,
                              G_FILE_QUERY_INFO_NONE, NULL, &error);
    }

    if (error != NULL &&
        !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
        g_autofree char *uri = g_file_get_uri (location);

        g_warning ("Could not save %s on %s: %s", attribute, uri, error->message);
    }
}

static void
write_marks_thread (GTask        *task,
                    gpointer      source_object,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
    GPtrArray *writes = task_data;

    for (guint i = 0; i < writes->len; i++)
    {
        MarkWrite *write = g_ptr_array_index (writes, i);

        if (write->write_rating)
        {
            g_autofree char *value = write->rating > 0
                                     ? g_strdup_printf ("%u", write->rating * 2)
                                     : NULL;

            set_or_remove_xattr (write->location, "xattr::baloo.rating", value);
        }

        if (write->write_tags)
        {
            g_autofree char *value = (write->tags != NULL && write->tags[0] != NULL)
                                     ? g_strjoinv (",", write->tags)
                                     : NULL;

            set_or_remove_xattr (write->location, "xattr::xdg.tags", value);
        }
    }

    g_task_return_boolean (task, TRUE);
}

static void
write_marks (GPtrArray *writes)
{
    if (writes->len == 0)
    {
        g_ptr_array_unref (writes);
        return;
    }

    g_autoptr (GTask) task = g_task_new (NULL, NULL, NULL, NULL);

    g_task_set_task_data (task, writes, (GDestroyNotify) g_ptr_array_unref);
    g_task_run_in_thread (task, write_marks_thread);
}

static gboolean
can_mark (NautilusFile *file)
{
    g_autoptr (GFile) location = nautilus_file_get_location (file);

    /* Extended attributes need a real file; skip trash, recent, search… */
    return g_file_is_native (location);
}

void
nautilus_labels_set_rating (GList *files,
                            guint  stars)
{
    GPtrArray *writes = g_ptr_array_new_with_free_func ((GDestroyNotify) mark_write_free);

    stars = MIN (stars, 5);

    for (GList *l = files; l != NULL; l = l->next)
    {
        NautilusFile *file = l->data;

        if (!can_mark (file) || nautilus_file_get_rating (file) == stars)
        {
            continue;
        }

        MarkWrite *write = g_new0 (MarkWrite, 1);

        write->location = nautilus_file_get_location (file);
        write->write_rating = TRUE;
        write->rating = stars;
        g_ptr_array_add (writes, write);

        nautilus_file_set_marks_cached (file, stars, nautilus_file_get_tags (file));
    }

    write_marks (writes);
}

void
nautilus_labels_set_tag (GList      *files,
                         const char *tag,
                         gboolean    on)
{
    GPtrArray *writes = g_ptr_array_new_with_free_func ((GDestroyNotify) mark_write_free);

    if (tag == NULL || tag[0] == '\0' || strchr (tag, ',') != NULL)
    {
        g_ptr_array_unref (writes);
        return;
    }

    if (on)
    {
        nautilus_labels_ensure_tag (tag);
    }

    for (GList *l = files; l != NULL; l = l->next)
    {
        NautilusFile *file = l->data;

        if (!can_mark (file) || nautilus_file_has_tag (file, tag) == on)
        {
            continue;
        }

        g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
        const char * const *tags = nautilus_file_get_tags (file);

        for (guint i = 0; tags != NULL && tags[i] != NULL; i++)
        {
            if (g_strcmp0 (tags[i], tag) != 0)
            {
                g_strv_builder_add (builder, tags[i]);
            }
        }
        if (on)
        {
            g_strv_builder_add (builder, tag);
        }

        MarkWrite *write = g_new0 (MarkWrite, 1);

        write->location = nautilus_file_get_location (file);
        write->write_tags = TRUE;
        write->tags = g_strv_builder_end (builder);
        g_ptr_array_add (writes, write);

        nautilus_file_set_marks_cached (file, nautilus_file_get_rating (file),
                                        (const char * const *) write->tags);
    }

    write_marks (writes);
}

gboolean
nautilus_labels_get_alt_held (void)
{
    return nautilus_labels_get ()->alt_held;
}

void
nautilus_labels_set_alt_held (gboolean held)
{
    NautilusLabels *self = nautilus_labels_get ();

    if (self->alt_held != held)
    {
        self->alt_held = held;
        g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ALT_HELD]);
    }
}

/* ---------------------------------------------------- Tag definitions */

static char *
tags_file_path (void)
{
    return g_build_filename (g_get_user_config_dir (), "nautilus", "tags.json", NULL);
}

/* Reads {"tags": [{"name": "…", "color": [r, g, b]}, …]}, the format of
 * File Flier's meta.json, which has the same list. */
static gboolean
read_tags_json (const char *path,
                GPtrArray  *into)
{
    g_autoptr (JsonParser) parser = json_parser_new ();

    if (!json_parser_load_from_mapped_file (parser, path, NULL))
    {
        return FALSE;
    }

    JsonNode *root = json_parser_get_root (parser);
    JsonObject *object = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
    JsonArray *array = (object != NULL && json_object_has_member (object, "tags"))
                       ? json_object_get_array_member (object, "tags")
                       : NULL;

    if (array == NULL)
    {
        return FALSE;
    }

    for (guint i = 0; i < json_array_get_length (array); i++)
    {
        JsonObject *tag = json_array_get_object_element (array, i);
        const char *name = tag != NULL ? json_object_get_string_member_with_default (tag, "name", NULL) : NULL;

        if (name == NULL || name[0] == '\0')
        {
            continue;
        }

        TagDef *def = g_new0 (TagDef, 1);
        JsonArray *color = json_object_has_member (tag, "color")
                           ? json_object_get_array_member (tag, "color")
                           : NULL;

        def->name = g_strdup (name);
        def->color = rgba_from_hex (tag_palette[into->len % G_N_ELEMENTS (tag_palette)]);
        if (color != NULL && json_array_get_length (color) >= 3)
        {
            def->color = (GdkRGBA) {
                json_array_get_int_element (color, 0) / 255.0f,
                json_array_get_int_element (color, 1) / 255.0f,
                json_array_get_int_element (color, 2) / 255.0f,
                1.0f
            };
        }
        g_ptr_array_add (into, def);
    }

    return TRUE;
}

static void
save_tags (NautilusLabels *self)
{
    g_autoptr (JsonBuilder) builder = json_builder_new ();

    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "tags");
    json_builder_begin_array (builder);
    for (guint i = 0; i < self->tags->len; i++)
    {
        TagDef *def = g_ptr_array_index (self->tags, i);

        json_builder_begin_object (builder);
        json_builder_set_member_name (builder, "name");
        json_builder_add_string_value (builder, def->name);
        json_builder_set_member_name (builder, "color");
        json_builder_begin_array (builder);
        json_builder_add_int_value (builder, (int) (def->color.red * 255 + 0.5));
        json_builder_add_int_value (builder, (int) (def->color.green * 255 + 0.5));
        json_builder_add_int_value (builder, (int) (def->color.blue * 255 + 0.5));
        json_builder_end_array (builder);
        json_builder_end_object (builder);
    }
    json_builder_end_array (builder);
    json_builder_end_object (builder);

    g_autoptr (JsonGenerator) generator = json_generator_new ();
    g_autoptr (JsonNode) root = json_builder_get_root (builder);
    g_autofree char *path = tags_file_path ();
    g_autofree char *dir = g_path_get_dirname (path);
    g_autoptr (GError) error = NULL;

    json_generator_set_root (generator, root);
    json_generator_set_pretty (generator, TRUE);
    g_mkdir_with_parents (dir, 0700);
    if (!json_generator_to_file (generator, path, &error))
    {
        g_warning ("Could not save tags: %s", error->message);
    }
}

static void
ensure_tags_loaded (NautilusLabels *self)
{
    if (self->tags_loaded)
    {
        return;
    }

    g_autofree char *path = tags_file_path ();

    self->tags_loaded = TRUE;
    if (!read_tags_json (path, self->tags))
    {
        /* First run: start from File Flier's tags, if there are any. */
        g_autofree char *flier = g_build_filename (g_get_user_config_dir (), "file-flier", "meta.json", NULL);

        if (read_tags_json (flier, self->tags) && self->tags->len > 0)
        {
            save_tags (self);
        }
    }
}

static TagDef *
find_tag (NautilusLabels *self,
          const char     *name)
{
    ensure_tags_loaded (self);

    for (guint i = 0; i < self->tags->len; i++)
    {
        TagDef *def = g_ptr_array_index (self->tags, i);

        if (g_strcmp0 (def->name, name) == 0)
        {
            return def;
        }
    }

    return NULL;
}

guint
nautilus_labels_get_n_tags (void)
{
    NautilusLabels *self = nautilus_labels_get ();

    ensure_tags_loaded (self);

    return self->tags->len;
}

const char *
nautilus_labels_get_tag_name (guint i)
{
    NautilusLabels *self = nautilus_labels_get ();

    ensure_tags_loaded (self);
    g_return_val_if_fail (i < self->tags->len, NULL);

    return ((TagDef *) g_ptr_array_index (self->tags, i))->name;
}

void
nautilus_labels_get_tag_color (const char *tag,
                               GdkRGBA    *color)
{
    TagDef *def = find_tag (nautilus_labels_get (), tag);

    *color = def != NULL ? def->color : rgba_from_hex (0x77767B);
}

void
nautilus_labels_ensure_tag (const char *tag)
{
    NautilusLabels *self = nautilus_labels_get ();

    if (tag == NULL || tag[0] == '\0' || find_tag (self, tag) != NULL)
    {
        return;
    }

    TagDef *def = g_new0 (TagDef, 1);

    def->name = g_strdup (tag);
    def->color = rgba_from_hex (tag_palette[self->tags->len % G_N_ELEMENTS (tag_palette)]);
    g_ptr_array_add (self->tags, def);
    save_tags (self);
    g_signal_emit (self, signals[TAGS_CHANGED], 0);
}

void
nautilus_labels_set_tag_color (const char    *tag,
                               const GdkRGBA *color)
{
    NautilusLabels *self = nautilus_labels_get ();
    TagDef *def = find_tag (self, tag);

    if (def != NULL)
    {
        def->color = *color;
        def->color.alpha = 1.0f;
        save_tags (self);
        g_signal_emit (self, signals[TAGS_CHANGED], 0);
    }
}

void
nautilus_labels_rename_tag (const char *tag,
                            const char *new_name)
{
    NautilusLabels *self = nautilus_labels_get ();
    TagDef *def = find_tag (self, tag);

    if (def == NULL || new_name == NULL || new_name[0] == '\0' ||
        strchr (new_name, ',') != NULL || find_tag (self, new_name) != NULL)
    {
        return;
    }

    /* Files keep the old name until they're tagged again; the list is what
     * renames. Tagged files show up under the new name once retagged. */
    g_free (def->name);
    def->name = g_strdup (new_name);
    save_tags (self);
    g_signal_emit (self, signals[TAGS_CHANGED], 0);
}

void
nautilus_labels_remove_tag (const char *tag)
{
    NautilusLabels *self = nautilus_labels_get ();
    TagDef *def = find_tag (self, tag);

    if (def != NULL)
    {
        g_ptr_array_remove (self->tags, def);
        save_tags (self);
        g_signal_emit (self, signals[TAGS_CHANGED], 0);
    }
}

/* ------------------------------------------------------- Folder colors */

int
nautilus_labels_get_folder_color (NautilusFile *file)
{
    const char *value = nautilus_file_get_metadata (file, NAUTILUS_METADATA_KEY_FOLDER_COLOR, NULL);

    if (value == NULL || value[0] < '0' || value[0] > '7' || value[1] != '\0')
    {
        return -1;
    }

    return value[0] - '0';
}

static int
compare_by_name (NautilusFile *a,
                 NautilusFile *b)
{
    const char *name = nautilus_file_get_display_name (b);

    return nautilus_file_compare_display_name (a, name);
}

void
nautilus_labels_set_folder_color (GList *files,
                                  int    slot)
{
    g_autoptr (GList) folders = NULL;

    for (GList *l = files; l != NULL; l = l->next)
    {
        if (nautilus_file_is_directory (l->data))
        {
            folders = g_list_prepend (folders, l->data);
        }
    }

    /* The rainbow goes through the colors in name order. */
    folders = g_list_sort (folders, (GCompareFunc) compare_by_name);

    int i = 0;
    for (GList *l = folders; l != NULL; l = l->next, i++)
    {
        int this_slot = slot == NAUTILUS_FOLDER_COLOR_RAINBOW
                        ? i % NAUTILUS_FOLDER_COLOR_SLOTS
                        : slot;
        char value[2] = { (char) ('0' + this_slot), '\0' };

        nautilus_file_set_metadata (l->data, NAUTILUS_METADATA_KEY_FOLDER_COLOR, NULL,
                                    this_slot >= 0 ? value : NULL);
    }
}

void
nautilus_labels_get_scheme_colors (int     scheme,
                                   GdkRGBA colors[NAUTILUS_FOLDER_COLOR_SLOTS])
{
    scheme = CLAMP (scheme, 0, (int) G_N_ELEMENTS (schemes) - 1);

    if (scheme == MONO_SCHEME)
    {
        /* Eight steps from a pale tint of the accent, through it, to a shade. */
        g_autoptr (GdkRGBA) accent = adw_style_manager_get_accent_color_rgba (adw_style_manager_get_default ());
        const GdkRGBA white = { 1, 1, 1, 1 };
        const GdkRGBA black = { 10 / 255.0f, 10 / 255.0f, 14 / 255.0f, 1 };
        const float steps[] = { 0.72f, 0.52f, 0.32f, 0.14f };

        for (int i = 0; i < 4; i++)
        {
            colors[i] = mix (*accent, white, steps[i]);
        }
        colors[4] = *accent;
        colors[5] = mix (*accent, black, 0.22f);
        colors[6] = mix (*accent, black, 0.40f);
        colors[7] = mix (*accent, black, 0.56f);
        return;
    }

    for (int i = 0; i < NAUTILUS_FOLDER_COLOR_SLOTS; i++)
    {
        colors[i] = rgba_from_hex (schemes[scheme].colors[i]);
    }
}

void
nautilus_labels_get_slot_color (int      slot,
                                GdkRGBA *color)
{
    NautilusLabels *self = nautilus_labels_get ();

    *color = self->scheme_colors[CLAMP (slot, 0, NAUTILUS_FOLDER_COLOR_SLOTS - 1)];
}

const char *
nautilus_labels_get_slot_name (int slot)
{
    NautilusLabels *self = nautilus_labels_get ();

    return _(schemes[self->scheme].slot_names[CLAMP (slot, 0, NAUTILUS_FOLDER_COLOR_SLOTS - 1)]);
}

const char *
nautilus_labels_get_scheme_name (int scheme)
{
    return _(schemes[CLAMP (scheme, 0, (int) G_N_ELEMENTS (schemes) - 1)].name);
}

guint
nautilus_labels_get_n_schemes (void)
{
    return G_N_ELEMENTS (schemes);
}

/* ---------------------------------------------------- Tinted paintable */

#define NAUTILUS_TYPE_TINT_PAINTABLE (nautilus_tint_paintable_get_type ())
G_DECLARE_FINAL_TYPE (NautilusTintPaintable, nautilus_tint_paintable, NAUTILUS, TINT_PAINTABLE, GObject)

struct _NautilusTintPaintable
{
    GObject parent_instance;

    GdkPaintable *icon;
    int slot;
    float luminance;
};

static void nautilus_tint_paintable_iface_init (GdkPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (NautilusTintPaintable, nautilus_tint_paintable, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE, nautilus_tint_paintable_iface_init))

/* Maps the icon's lightness onto the slot color, so the folder keeps its
 * shading: the lightness of the folder's front comes out as the color. It
 * is measured, since themes differ: Adwaita's folders are mid-tone, Yaru's
 * light. */
#define TINT_MID_LUMINANCE 0.45f
#define TINT_PROBE_SIZE 32

static int
compare_floats (const void *a,
                const void *b)
{
    float x = *(const float *) a, y = *(const float *) b;

    return (x > y) - (x < y);
}

static float
measure_luminance (GdkPaintable *icon)
{
    g_autoptr (GtkSnapshot) snapshot = gtk_snapshot_new ();
    g_autoptr (GskRenderNode) node = NULL;
    cairo_surface_t *surface;
    cairo_t *cr;

    gdk_paintable_snapshot (icon, GDK_SNAPSHOT (snapshot), TINT_PROBE_SIZE, TINT_PROBE_SIZE);
    node = gtk_snapshot_free_to_node (g_steal_pointer (&snapshot));
    if (node == NULL)
    {
        return TINT_MID_LUMINANCE;
    }

    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, TINT_PROBE_SIZE, TINT_PROBE_SIZE);
    cr = cairo_create (surface);
    gsk_render_node_draw (node, cr);
    cairo_destroy (cr);
    cairo_surface_flush (surface);

    const guchar *data = cairo_image_surface_get_data (surface);
    int stride = cairo_image_surface_get_stride (surface);
    float values[TINT_PROBE_SIZE * TINT_PROBE_SIZE];
    guint n = 0;

    for (int y = 0; y < TINT_PROBE_SIZE; y++)
    {
        const guint32 *row = (const guint32 *) (data + y * stride);

        for (int x = 0; x < TINT_PROBE_SIZE; x++)
        {
            guint32 p = row[x];
            guint a = p >> 24;

            /* Only the solid part; the channels are premultiplied. */
            if (a > 200)
            {
                values[n++] = (0.2126f * ((p >> 16) & 0xff) + 0.7152f * ((p >> 8) & 0xff) +
                               0.0722f * (p & 0xff)) / a;
            }
        }
    }
    cairo_surface_destroy (surface);

    if (n < 16)
    {
        return TINT_MID_LUMINANCE;
    }

    /* The front of the folder, its biggest and lightest part, is what should
     * come out as the color; the tab and the shading darker. */
    qsort (values, n, sizeof (float), compare_floats);
    return CLAMP (values[n * 7 / 10], 0.2f, 0.95f);
}

/* The lightness of each icon file, measured once. */
static float
icon_luminance (GdkPaintable *icon)
{
    static GHashTable *cache = NULL;
    g_autoptr (GFile) file = NULL;
    g_autofree char *key = NULL;

    if (GTK_IS_ICON_PAINTABLE (icon))
    {
        file = gtk_icon_paintable_get_file (GTK_ICON_PAINTABLE (icon));
    }
    if (file == NULL)
    {
        return measure_luminance (icon);
    }

    if (cache == NULL)
    {
        cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    }
    key = g_file_get_uri (file);

    gpointer value;
    if (g_hash_table_lookup_extended (cache, key, NULL, &value))
    {
        return GPOINTER_TO_INT (value) / 1000.0f;
    }

    float luminance = measure_luminance (icon);

    g_hash_table_insert (cache, g_steal_pointer (&key), GINT_TO_POINTER ((int) (luminance * 1000)));
    return luminance;
}

static void
tint_snapshot (GdkPaintable *paintable,
               GdkSnapshot  *snapshot,
               double        width,
               double        height)
{
    NautilusTintPaintable *self = NAUTILUS_TINT_PAINTABLE (paintable);
    GdkRGBA c;
    graphene_matrix_t matrix;
    const float k = 1.0f / self->luminance;

    nautilus_labels_get_slot_color (self->slot, &c);

    /* Rows are the input channels, columns the output (as GTK's own filters). */
    graphene_matrix_init_from_float (&matrix, (float[16]) {
        0.2126f * k * c.red, 0.2126f * k * c.green, 0.2126f * k * c.blue, 0,
        0.7152f * k * c.red, 0.7152f * k * c.green, 0.7152f * k * c.blue, 0,
        0.0722f * k * c.red, 0.0722f * k * c.green, 0.0722f * k * c.blue, 0,
        0, 0, 0, 1,
    });

    gtk_snapshot_push_color_matrix (GTK_SNAPSHOT (snapshot), &matrix, graphene_vec4_zero ());
    gdk_paintable_snapshot (self->icon, snapshot, width, height);
    gtk_snapshot_pop (GTK_SNAPSHOT (snapshot));
}

static int
tint_get_intrinsic_width (GdkPaintable *paintable)
{
    return gdk_paintable_get_intrinsic_width (NAUTILUS_TINT_PAINTABLE (paintable)->icon);
}

static int
tint_get_intrinsic_height (GdkPaintable *paintable)
{
    return gdk_paintable_get_intrinsic_height (NAUTILUS_TINT_PAINTABLE (paintable)->icon);
}

static double
tint_get_intrinsic_aspect_ratio (GdkPaintable *paintable)
{
    return gdk_paintable_get_intrinsic_aspect_ratio (NAUTILUS_TINT_PAINTABLE (paintable)->icon);
}

static void
nautilus_tint_paintable_iface_init (GdkPaintableInterface *iface)
{
    iface->snapshot = tint_snapshot;
    iface->get_intrinsic_width = tint_get_intrinsic_width;
    iface->get_intrinsic_height = tint_get_intrinsic_height;
    iface->get_intrinsic_aspect_ratio = tint_get_intrinsic_aspect_ratio;
}

static void
nautilus_tint_paintable_dispose (GObject *object)
{
    NautilusTintPaintable *self = NAUTILUS_TINT_PAINTABLE (object);

    g_clear_object (&self->icon);

    G_OBJECT_CLASS (nautilus_tint_paintable_parent_class)->dispose (object);
}

static void
nautilus_tint_paintable_class_init (NautilusTintPaintableClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_tint_paintable_dispose;
}

static void
nautilus_tint_paintable_init (NautilusTintPaintable *self)
{
}

GdkPaintable *
nautilus_labels_tint_paintable (GdkPaintable *icon,
                                int           slot)
{
    NautilusTintPaintable *self = g_object_new (NAUTILUS_TYPE_TINT_PAINTABLE, NULL);

    self->icon = g_object_ref (icon);
    self->slot = CLAMP (slot, 0, NAUTILUS_FOLDER_COLOR_SLOTS - 1);
    self->luminance = icon_luminance (icon);

    /* Redraw in the new color when the scheme or the accent changes. */
    g_signal_connect_object (nautilus_labels_get (), "colors-changed",
                             G_CALLBACK (gdk_paintable_invalidate_contents), self,
                             G_CONNECT_SWAPPED);

    return GDK_PAINTABLE (self);
}

/* --------------------------------------------------------------- Stars */


void
nautilus_labels_draw_stars (GtkSnapshot           *snapshot,
                            const graphene_rect_t *bounds,
                            guint                  rating,
                            const GdkRGBA         *empty_color,
                            int                    scale)
{
    float size = MIN (bounds->size.height, bounds->size.width / 5);
    /* Filled stars in the empty ones' color at full strength (white on dark
     * and on the tiles' badges): the shape tells them apart, not a hue. */
    GdkRGBA star_color = *empty_color;

    star_color.alpha = 1.0f;

    for (guint i = 0; i < 5; i++)
    {
        graphene_rect_t star = GRAPHENE_RECT_INIT (bounds->origin.x + i * size,
                                                   bounds->origin.y + (bounds->size.height - size) / 2,
                                                   size, size);

        nautilus_ui_draw_symbolic_icon (snapshot,
                                        i < rating ? "starred-symbolic" : "non-starred-symbolic",
                                        &star,
                                        i < rating ? star_color : *empty_color,
                                        scale);
    }
}

guint
nautilus_labels_star_at (const graphene_rect_t *bounds,
                         double                 x)
{
    float size = MIN (bounds->size.height, bounds->size.width / 5);
    double offset = x - bounds->origin.x;

    if (size <= 0 || offset < 0 || offset >= 5 * size)
    {
        return 0;
    }

    return (guint) (offset / size) + 1;
}

/* ------------------------------------------------- File Flier import */

static void
import_thread (GTask        *task,
               gpointer      source_object,
               gpointer      task_data,
               GCancellable *cancellable)
{
    g_autofree char *path = g_build_filename (g_get_user_config_dir (), "file-flier", "config.json", NULL);
    g_autoptr (JsonParser) parser = json_parser_new ();

    if (!json_parser_load_from_mapped_file (parser, path, NULL))
    {
        g_task_return_int (task, -1);
        return;
    }

    JsonNode *root = json_parser_get_root (parser);
    JsonObject *config = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
    JsonObject *colors = (config != NULL && json_object_has_member (config, "folder_colors"))
                         ? json_object_get_object_member (config, "folder_colors")
                         : NULL;

    if (colors != NULL)
    {
        g_autoptr (GList) paths = json_object_get_members (colors);

        for (GList *l = paths; l != NULL; l = l->next)
        {
            gint64 slot = json_object_get_int_member_with_default (colors, l->data, -1);
            g_autoptr (GFile) location = g_file_new_for_path (l->data);
            char value[2] = { (char) ('0' + slot), '\0' };

            if (slot >= 0 && slot < NAUTILUS_FOLDER_COLOR_SLOTS)
            {
                g_file_set_attribute_string (location, "metadata::" NAUTILUS_METADATA_KEY_FOLDER_COLOR,
                                             value, G_FILE_QUERY_INFO_NONE, NULL, NULL);
            }
        }
    }

    g_task_return_int (task, config != NULL
                       ? json_object_get_int_member_with_default (config, "folder_scheme", -1)
                       : -1);
}

static void
import_done (GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
    gssize scheme = g_task_propagate_int (G_TASK (result), NULL);

    if (scheme >= 0 && scheme < (gssize) G_N_ELEMENTS (schemes))
    {
        g_settings_set_enum (nautilus_flier_settings (), "folder-color-scheme", (int) scheme);
    }
}

void
nautilus_labels_import_file_flier (void)
{
    GSettings *settings = nautilus_flier_settings ();

    if (g_settings_get_boolean (settings, "file-flier-imported"))
    {
        return;
    }

    g_settings_set_boolean (settings, "file-flier-imported", TRUE);
    ensure_tags_loaded (nautilus_labels_get ());

    g_autoptr (GTask) task = g_task_new (NULL, NULL, import_done, NULL);
    g_task_run_in_thread (task, import_thread);
}

/* -------------------------------------------------------------- GObject */

static void
nautilus_labels_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    NautilusLabels *self = NAUTILUS_LABELS (object);

    switch (prop_id)
    {
        case PROP_ALT_HELD:
        {
            g_value_set_boolean (value, self->alt_held);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_labels_class_init (NautilusLabelsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = nautilus_labels_get_property;

    properties[PROP_ALT_HELD] =
        g_param_spec_boolean ("alt-held", NULL, NULL, FALSE,
                              G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties (object_class, N_PROPS, properties);

    signals[TAGS_CHANGED] = g_signal_new ("tags-changed", G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
    signals[COLORS_CHANGED] = g_signal_new ("colors-changed", G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);
}

static void
nautilus_labels_init (NautilusLabels *self)
{
    self->tags = g_ptr_array_new_with_free_func ((GDestroyNotify) tag_def_free);
    update_scheme_colors (self);

    g_signal_connect_object (nautilus_flier_settings (), "changed::folder-color-scheme",
                             G_CALLBACK (on_colors_changed), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (adw_style_manager_get_default (), "notify::accent-color",
                             G_CALLBACK (on_colors_changed), self, G_CONNECT_SWAPPED);
}
