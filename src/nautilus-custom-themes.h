/*
 * Custom colour themes: CSS files in ~/.config/mako/themes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct
{
    char *id;      /* the file name without ".css" */
    char *name;    /* from a "Name:" line in the file's first comment, or the id */
} NautilusCustomTheme;

/* Loads the themes (once) and watches the folder; "changed" on the
 * returned object whenever a theme is added, edited or removed. */
GObject     *nautilus_custom_themes_get           (void);

/* The themes found, in name order (NautilusCustomTheme). Owned by the module. */
GPtrArray   *nautilus_custom_themes_list          (void);
gboolean     nautilus_custom_themes_has           (const char *id);

/* The style class a window (or a preview) gets for @id. */
char        *nautilus_custom_themes_class         (const char *id);

GFile       *nautilus_custom_themes_get_folder    (void);

/* A new theme from the template, named so it doesn't overwrite one;
 * returns its file. */
GFile       *nautilus_custom_themes_new_from_template (GError **error);

G_END_DECLS
