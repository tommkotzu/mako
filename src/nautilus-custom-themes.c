/*
 * Custom colour themes: CSS files in ~/.config/mako/themes.
 *
 * A theme file is ordinary GTK CSS written against the class .mako-theme.
 * Each file gets a class of its own (.theme-custom-ID), which the window
 * wears while the theme is chosen, and so does the theme's picture in
 * Preferences. ".mako-theme" in the file becomes that class, so a theme
 * can set the colour variables and also style anything below the window.
 * The folder is watched: saving a file restyles the windows at once.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-custom-themes.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <string.h>

#define THEMES_SIGNAL "changed"

/* Only there to say "changed". */
G_DECLARE_FINAL_TYPE (NautilusCustomThemes, nautilus_custom_themes, NAUTILUS, CUSTOM_THEMES, GObject)
struct _NautilusCustomThemes
{
    GObject parent_instance;
};
G_DEFINE_FINAL_TYPE (NautilusCustomThemes, nautilus_custom_themes, G_TYPE_OBJECT)

static void
nautilus_custom_themes_class_init (NautilusCustomThemesClass *klass)
{
    g_signal_new (THEMES_SIGNAL, G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
nautilus_custom_themes_init (NautilusCustomThemes *self)
{
}

static GObject *themes_object = NULL;
static GPtrArray *themes = NULL;
static GtkCssProvider *provider = NULL;
static GFileMonitor *monitor = NULL;
static guint reload_id = 0;

static void
theme_free (NautilusCustomTheme *theme)
{
    g_free (theme->id);
    g_free (theme->name);
    g_free (theme);
}

GFile *
nautilus_custom_themes_get_folder (void)
{
    return g_file_new_build_filename (g_get_user_config_dir (), "mako", "themes", NULL);
}

char *
nautilus_custom_themes_class (const char *id)
{
    GString *class = g_string_new ("theme-custom-");

    /* Only what a CSS class can hold. */
    for (const char *p = id; *p != '\0'; p++)
    {
        g_string_append_c (class, g_ascii_isalnum (*p) || *p == '-' || *p == '_' ? *p : '_');
    }
    return g_string_free (class, FALSE);
}

/* "Name: Tokyo Night" in the first comment, else the file name made
 * readable ("tokyo-night" becomes "Tokyo Night"). */
static char *
theme_name (const char *id,
            const char *css)
{
    const char *end = strstr (css, "*/");
    const char *line = strstr (css, "Name:");

    if (line != NULL && (end == NULL || line < end))
    {
        const char *start = line + strlen ("Name:");
        const char *stop = strpbrk (start, "\r\n");
        g_autofree char *name = g_strndup (start, stop != NULL ? (gsize) (stop - start) : strlen (start));

        g_strstrip (name);
        if (g_str_has_suffix (name, "*/"))
        {
            name[strlen (name) - 2] = '\0';
            g_strstrip (name);
        }
        if (*name != '\0')
        {
            return g_steal_pointer (&name);
        }
    }
    {
        g_auto (GStrv) words = g_strsplit_set (id, "-_ ", -1);

        for (int i = 0; words[i] != NULL; i++)
        {
            words[i][0] = g_ascii_toupper (words[i][0]);
        }
        return g_strjoinv (" ", words);
    }
}

static void
on_parsing_error (GtkCssProvider *css_provider,
                  GtkCssSection  *section,
                  GError         *error)
{
    g_autofree char *where = gtk_css_section_to_string (section);

    /* The provider holds all themes in one; each starts with a marker
     * comment, so the place says which file it is. */
    g_warning ("Custom theme: %s: %s", where, error->message);
}

static int
compare_themes (gconstpointer a,
                gconstpointer b)
{
    const NautilusCustomTheme *ta = *(NautilusCustomTheme **) a;
    const NautilusCustomTheme *tb = *(NautilusCustomTheme **) b;

    return g_utf8_collate (ta->name, tb->name);
}

static void
reload (void)
{
    g_autoptr (GFile) folder = nautilus_custom_themes_get_folder ();
    g_autoptr (GFileEnumerator) children = NULL;
    g_autoptr (GString) all = g_string_new (NULL);
    GFileInfo *info;

    g_ptr_array_set_size (themes, 0);
    children = g_file_enumerate_children (folder, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                          G_FILE_QUERY_INFO_NONE, NULL, NULL);
    while (children != NULL &&
           g_file_enumerator_iterate (children, &info, NULL, NULL, NULL) && info != NULL)
    {
        const char *file_name = g_file_info_get_name (info);
        g_autoptr (GFile) file = NULL;
        g_autofree char *css = NULL;
        g_autofree char *rewritten = NULL;
        g_autofree char *class = NULL;
        g_autofree char *selector = NULL;
        g_auto (GStrv) parts = NULL;
        NautilusCustomTheme *theme;

        if (!g_str_has_suffix (file_name, ".css") || file_name[0] == '.')
        {
            continue;
        }
        file = g_file_get_child (folder, file_name);
        if (!g_file_load_contents (file, NULL, &css, NULL, NULL, NULL) || !g_utf8_validate (css, -1, NULL))
        {
            continue;
        }

        theme = g_new0 (NautilusCustomTheme, 1);
        theme->id = g_strndup (file_name, strlen (file_name) - strlen (".css"));
        theme->name = theme_name (theme->id, css);
        g_ptr_array_add (themes, theme);

        class = nautilus_custom_themes_class (theme->id);
        selector = g_strconcat (".", class, NULL);
        parts = g_strsplit (css, ".mako-theme", -1);
        g_string_append_printf (all, "\n/* ==== %s ==== */\n", file_name);
        rewritten = g_strjoinv (selector, parts);
        g_string_append (all, rewritten);
        /* A comment or block the file leaves open stays in it. */
        g_string_append (all, "\n/* */\n");
    }
    g_ptr_array_sort (themes, compare_themes);
    gtk_css_provider_load_from_string (provider, all->str);
    g_signal_emit_by_name (themes_object, THEMES_SIGNAL);
}

static gboolean
reload_later (gpointer user_data)
{
    reload_id = 0;
    reload ();
    return G_SOURCE_REMOVE;
}

static void
on_folder_changed (GFileMonitor      *file_monitor,
                   GFile             *file,
                   GFile             *other_file,
                   GFileMonitorEvent  event)
{
    /* Editors save in several steps; once they're done. */
    g_clear_handle_id (&reload_id, g_source_remove);
    reload_id = g_timeout_add (200, reload_later, NULL);
}

static void
follow_dark (AdwStyleManager *manager,
             GParamSpec      *pspec,
             GtkCssProvider  *css_provider)
{
    g_object_set (css_provider, "prefers-color-scheme",
                  adw_style_manager_get_dark (manager) ? GTK_INTERFACE_COLOR_SCHEME_DARK
                                                        : GTK_INTERFACE_COLOR_SCHEME_LIGHT,
                  NULL);
}

GObject *
nautilus_custom_themes_get (void)
{
    if (themes_object == NULL)
    {
        g_autoptr (GFile) folder = nautilus_custom_themes_get_folder ();

        themes_object = g_object_new (nautilus_custom_themes_get_type (), NULL);
        themes = g_ptr_array_new_with_free_func ((GDestroyNotify) theme_free);

        provider = gtk_css_provider_new ();
        g_signal_connect (provider, "parsing-error", G_CALLBACK (on_parsing_error), NULL);
        gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);

        g_file_make_directory_with_parents (folder, NULL, NULL);
        monitor = g_file_monitor_directory (folder, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
        if (monitor != NULL)
        {
            g_signal_connect (monitor, "changed", G_CALLBACK (on_folder_changed), NULL);
        }
        /* A stylesheet's @media (prefers-color-scheme) follows its own
         * property, which libadwaita sets only on the stylesheets it loads:
         * this one follows libadwaita's light or dark. */
        g_signal_connect (adw_style_manager_get_default (), "notify::dark",
                          G_CALLBACK (follow_dark), provider);
        follow_dark (adw_style_manager_get_default (), NULL, provider);
        reload ();
    }
    return themes_object;
}

GPtrArray *
nautilus_custom_themes_list (void)
{
    nautilus_custom_themes_get ();
    return themes;
}

gboolean
nautilus_custom_themes_has (const char *id)
{
    GPtrArray *list = nautilus_custom_themes_list ();

    for (guint i = 0; id != NULL && i < list->len; i++)
    {
        if (g_str_equal (((NautilusCustomTheme *) g_ptr_array_index (list, i))->id, id))
        {
            return TRUE;
        }
    }
    return FALSE;
}

GFile *
nautilus_custom_themes_new_from_template (GError **error)
{
    g_autoptr (GFile) folder = nautilus_custom_themes_get_folder ();
    g_autoptr (GBytes) template = g_resources_lookup_data ("/org/gnome/nautilus/theme-template.css",
                                                           G_RESOURCE_LOOKUP_FLAGS_NONE, error);

    if (template == NULL)
    {
        return NULL;
    }
    g_file_make_directory_with_parents (folder, NULL, NULL);
    for (int i = 1; i < 100; i++)
    {
        g_autofree char *name = i == 1 ? g_strdup ("my-theme.css") : g_strdup_printf ("my-theme-%d.css", i);
        g_autoptr (GFile) file = g_file_get_child (folder, name);
        g_autoptr (GFileOutputStream) out = g_file_create (file, G_FILE_CREATE_NONE, NULL, NULL);

        if (out != NULL)
        {
            gsize size;
            const char *data = g_bytes_get_data (template, &size);

            if (!g_output_stream_write_all (G_OUTPUT_STREAM (out), data, size, NULL, NULL, error) ||
                !g_output_stream_close (G_OUTPUT_STREAM (out), NULL, error))
            {
                return NULL;
            }
            return g_steal_pointer (&file);
        }
    }
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS, _("Too many themes named “my-theme”"));
    return NULL;
}
