/*
 * Nautilus
 *
 * Copyright (C) 1999, 2000 Red Hat, Inc.
 * Copyright (C) 1999, 2000 Eazel, Inc.
 *
 * Nautilus is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Nautilus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Elliot Lee <sopwith@redhat.com>,
 *          Darin Adler <darin@bentspoon.com>,
 *          John Sullivan <sullivan@eazel.com>
 *
 */

/* nautilus-main.c: Implementation of the routines that drive program lifecycle and main window creation/destruction. */

#include <config.h>

#include "nautilus-application.h"
#include "nautilus-resources.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>

#include <locale.h>
#include <sys/resource.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* With portals off (see main), GTK on Wayland falls back to built-in
 * defaults (Adwaita icons, Sans 10) instead of the desktop's settings. Feed
 * GtkSettings from the same GSettings keys the GNOME portal serves.
 * gtk-theme is left out on purpose: libadwaita sets its own empty theme,
 * and loading the desktop's GTK theme (Yaru on Ubuntu) on top of it gives
 * outlined header bar buttons, other sidebar colors and square corners. */
static const struct
{
    const char *schema;
    const char *key;
    const char *property;
} host_settings[] =
{
    { "org.gnome.desktop.interface", "icon-theme", "gtk-icon-theme-name" },
    { "org.gnome.desktop.interface", "cursor-theme", "gtk-cursor-theme-name" },
    { "org.gnome.desktop.interface", "cursor-size", "gtk-cursor-theme-size" },
    { "org.gnome.desktop.interface", "font-name", "gtk-font-name" },
    { "org.gnome.desktop.interface", "font-rendering", "gtk-font-rendering" },
    { "org.gnome.desktop.interface", "cursor-blink", "gtk-cursor-blink" },
    { "org.gnome.desktop.interface", "cursor-blink-time", "gtk-cursor-blink-time" },
    { "org.gnome.desktop.interface", "cursor-blink-timeout", "gtk-cursor-blink-timeout" },
    { "org.gnome.desktop.interface", "gtk-im-module", "gtk-im-module" },
    { "org.gnome.desktop.interface", "enable-animations", "gtk-enable-animations" },
    { "org.gnome.desktop.interface", "gtk-enable-primary-paste", "gtk-enable-primary-paste" },
    { "org.gnome.desktop.interface", "overlay-scrolling", "gtk-overlay-scrolling" },
    { "org.gnome.desktop.peripherals.mouse", "double-click", "gtk-double-click-time" },
    { "org.gnome.desktop.peripherals.mouse", "drag-threshold", "gtk-dnd-drag-threshold" },
    { "org.gnome.desktop.wm.preferences", "button-layout", "gtk-decoration-layout" },
    { "org.gnome.desktop.wm.preferences", "action-double-click-titlebar", "gtk-titlebar-double-click" },
    { "org.gnome.desktop.wm.preferences", "action-middle-click-titlebar", "gtk-titlebar-middle-click" },
    { "org.gnome.desktop.wm.preferences", "action-right-click-titlebar", "gtk-titlebar-right-click" },
    { "org.gnome.desktop.sound", "theme-name", "gtk-sound-theme-name" },
    { "org.gnome.desktop.sound", "event-sounds", "gtk-enable-event-sounds" },
    { "org.gnome.desktop.sound", "input-feedback-sounds", "gtk-enable-input-feedback-sounds" },
    { "org.gnome.desktop.privacy", "remember-recent-files", "gtk-recent-files-enabled" },
    { "org.gnome.desktop.privacy", "recent-files-max-age", "gtk-recent-files-max-age" },
};

static GSettings *
host_settings_get (GHashTable *cache,
                   const char *schema_id,
                   const char *key)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    g_autoptr (GSettingsSchema) schema = NULL;
    GSettings *settings;

    if (source == NULL)
    {
        return NULL;
    }
    schema = g_settings_schema_source_lookup (source, schema_id, TRUE);
    if (schema == NULL || (key != NULL && !g_settings_schema_has_key (schema, key)))
    {
        return NULL;
    }
    settings = g_hash_table_lookup (cache, schema_id);
    if (settings == NULL)
    {
        settings = g_settings_new_full (schema, NULL, NULL);
        g_hash_table_insert (cache, (gpointer) schema_id, settings);
    }
    return settings;
}

/* Text scaling and font smoothing are stored differently from how
 * GtkSettings wants them, so they are converted rather than bound. */
static void
host_settings_update_fonts (GSettings *interface)
{
    GtkSettings *gtk = gtk_settings_get_default ();
    g_autofree char *antialiasing = g_settings_get_string (interface, "font-antialiasing");
    g_autofree char *hinting = g_settings_get_string (interface, "font-hinting");
    g_autofree char *rgba_order = g_settings_get_string (interface, "font-rgba-order");
    double scale = g_settings_get_double (interface, "text-scaling-factor");
    g_autofree char *hintstyle = g_strconcat ("hint", hinting, NULL);
    gboolean subpixel = g_str_equal (antialiasing, "rgba");

    g_object_set (gtk,
                  "gtk-xft-dpi", (int) (scale * 96 * 1024),
                  "gtk-xft-antialias", g_str_equal (antialiasing, "none") ? 0 : 1,
                  "gtk-xft-hinting", g_str_equal (hinting, "none") ? 0 : 1,
                  "gtk-xft-hintstyle", hintstyle,
                  "gtk-xft-rgba", subpixel && !g_str_equal (rgba_order, "rgba") ? rgba_order : "none",
                  NULL);
}

static void
host_settings_bind (GApplication *application)
{
    static GHashTable *cache = NULL;
    GtkSettings *gtk = gtk_settings_get_default ();
    GObjectClass *gtk_class;
    GSettings *interface;

    if (cache != NULL || gtk == NULL)
    {
        return;
    }
    cache = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
    gtk_class = G_OBJECT_GET_CLASS (gtk);

    for (gsize i = 0; i < G_N_ELEMENTS (host_settings); i++)
    {
        GSettings *settings = host_settings_get (cache, host_settings[i].schema, host_settings[i].key);

        if (settings != NULL && g_object_class_find_property (gtk_class, host_settings[i].property) != NULL)
        {
            g_settings_bind (settings, host_settings[i].key,
                             gtk, host_settings[i].property,
                             G_SETTINGS_BIND_GET);
        }
    }

    interface = host_settings_get (cache, "org.gnome.desktop.interface", "text-scaling-factor");
    if (interface != NULL &&
        host_settings_get (cache, "org.gnome.desktop.interface", "font-antialiasing") != NULL &&
        host_settings_get (cache, "org.gnome.desktop.interface", "font-hinting") != NULL &&
        host_settings_get (cache, "org.gnome.desktop.interface", "font-rgba-order") != NULL)
    {
        host_settings_update_fonts (interface);
        g_signal_connect (interface, "changed::text-scaling-factor", G_CALLBACK (host_settings_update_fonts), NULL);
        g_signal_connect (interface, "changed::font-antialiasing", G_CALLBACK (host_settings_update_fonts), NULL);
        g_signal_connect (interface, "changed::font-hinting", G_CALLBACK (host_settings_update_fonts), NULL);
        g_signal_connect (interface, "changed::font-rgba-order", G_CALLBACK (host_settings_update_fonts), NULL);
    }
}

static char *saved_gdk_debug;
static char *saved_adw_disable_portal;

static void
restore_variable (const char *name,
                  char       *value)
{
    if (value != NULL)
    {
        g_setenv (name, value, TRUE);
    }
    else
    {
        g_unsetenv (name);
    }
    g_free (value);
}

static void
restore_portal_environment (gpointer user_data)
{
    restore_variable ("GDK_DEBUG", g_steal_pointer (&saved_gdk_debug));
    restore_variable ("ADW_DISABLE_PORTAL", g_steal_pointer (&saved_adw_disable_portal));
}

int
main (int   argc,
      char *argv[])
{
    gint retval;
    NautilusApplication *application;
    /* Initialize gettext support */
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    g_set_prgname (APPLICATION_ID);

    /* The Vulkan renderer on NVIDIA holds a few file descriptors per window,
     * popover and texture, so a busy session can pass the default soft limit
     * of 1024 and lose its windows. Raise it toward the hard limit, as
     * browsers do. */
    {
        struct rlimit limit;

        if (getrlimit (RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < limit.rlim_max)
        {
            limit.rlim_cur = MIN (limit.rlim_max, (rlim_t) 8192);
            setrlimit (RLIMIT_NOFILE, &limit);
        }
    }

    /* Mako can be the desktop portal's FileChooser backend. At login the
     * portal starts Mako and waits for it, while GTK and libadwaita in Mako
     * ask that same portal for appearance settings: each waits for the other
     * until the 25 s D-Bus timeout, and every app asking the portal stalls
     * with them. Read appearance from GSettings instead; on the host it is
     * the same data. */
    {
        const char *gdk_debug = g_getenv ("GDK_DEBUG");

        saved_gdk_debug = g_strdup (gdk_debug);
        saved_adw_disable_portal = g_strdup (g_getenv ("ADW_DISABLE_PORTAL"));
        /* GDK and libadwaita read them while the application starts up;
         * apps started from Mako get the environment as it was. */
        g_idle_add_once (restore_portal_environment, NULL);

        if (gdk_debug == NULL || *gdk_debug == '\0')
        {
            g_setenv ("GDK_DEBUG", "no-portals", TRUE);
        }
        else if (strstr (gdk_debug, "portals") == NULL)
        {
            g_autofree char *value = g_strconcat (gdk_debug, ",no-portals", NULL);
            g_setenv ("GDK_DEBUG", value, TRUE);
        }
        g_setenv ("ADW_DISABLE_PORTAL", "1", FALSE);
    }

    if (getuid () == 0)
    {
        g_warning (_("Running as root is not supported. "
                     "Consider running `nautilus admin:///` instead."));

        exit (ENOTSUP);
    }

    nautilus_register_resource ();
    /* Run the nautilus application. */
    application = nautilus_application_new ();
    /* "startup" runs the class handler first, so GTK is initialized here. */
    g_signal_connect (application, "startup", G_CALLBACK (host_settings_bind), NULL);

    /* hold indefinitely if we're asked to persist */
    if (g_getenv ("NAUTILUS_PERSIST") != NULL)
    {
        g_application_hold (G_APPLICATION (application));
    }

    retval = g_application_run (G_APPLICATION (application),
                                argc, argv);

    g_object_unref (application);

    return retval;
}
