/*
 * nautilus-labels-ui.c: menu rows and dialogs for ratings, tags and
 * folder colors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-labels-ui.h"

#include <glib/gi18n.h>

#include "nautilus-file.h"
#include "nautilus-labels.h"
#include "nautilus-custom-themes.h"
#include "nautilus-application.h"

/* ---------------------------------------------------------- Rating row */

static void
close_menu (GtkWidget *widget)
{
    GtkWidget *popover = gtk_widget_get_ancestor (widget, GTK_TYPE_POPOVER);

    if (popover != NULL)
    {
        gtk_popover_popdown (GTK_POPOVER (popover));
    }
}

static void
on_star_clicked (GtkButton *button,
                 gpointer   user_data)
{
    guint stars = GPOINTER_TO_UINT (user_data);
    GtkWidget *row = gtk_widget_get_parent (GTK_WIDGET (button));
    guint current = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "rating"));

    /* Clicking the current rating's star clears it, as in File Flier. */
    if (stars == current)
    {
        stars = 0;
    }

    gtk_widget_activate_action (GTK_WIDGET (button), "view.rate", "u", stars);
    close_menu (GTK_WIDGET (button));
}

void
nautilus_rating_row_set_rating (GtkWidget *row,
                                guint      rating)
{
    int i = 1;

    g_object_set_data (G_OBJECT (row), "rating", GUINT_TO_POINTER (rating));

    for (GtkWidget *child = gtk_widget_get_first_child (row);
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        if (!GTK_IS_BUTTON (child) || g_object_get_data (G_OBJECT (child), "star") == NULL)
        {
            continue;
        }

        gtk_button_set_icon_name (GTK_BUTTON (child),
                                  i <= (int) rating ? "starred-symbolic" : "non-starred-symbolic");
        if (i <= (int) rating)
        {
            gtk_widget_add_css_class (child, "rated");
        }
        else
        {
            gtk_widget_remove_css_class (child, "rated");
        }
        i++;
    }
}

GtkWidget *
nautilus_rating_row_new (void)
{
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new (_("Rating"));

    gtk_widget_add_css_class (row, "nautilus-rating-row");
    gtk_widget_set_hexpand (label, TRUE);
    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_box_append (GTK_BOX (row), label);

    for (guint i = 1; i <= 5; i++)
    {
        GtkWidget *star = gtk_button_new_from_icon_name ("non-starred-symbolic");
        g_autofree char *tooltip = g_strdup_printf (ngettext ("%u star (Ctrl+Alt+%u)",
                                                              "%u stars (Ctrl+Alt+%u)", i),
                                                    i, i);

        gtk_widget_add_css_class (star, "flat");
        gtk_widget_add_css_class (star, "circular");
        gtk_widget_add_css_class (star, "star");
        gtk_widget_set_tooltip_text (star, tooltip);
        g_object_set_data (G_OBJECT (star), "star", GUINT_TO_POINTER (i));
        g_signal_connect (star, "clicked", G_CALLBACK (on_star_clicked), GUINT_TO_POINTER (i));
        gtk_box_append (GTK_BOX (row), star);
    }

    return row;
}

/* --------------------------------------------------------- Color row */

static void
on_swatch_clicked (GtkButton *button,
                   gpointer   user_data)
{
    const char *action = g_object_get_data (G_OBJECT (button), "color-action");

    gtk_widget_activate_action (GTK_WIDGET (button), action, "i", GPOINTER_TO_INT (user_data));
    close_menu (GTK_WIDGET (button));
}

static void
draw_swatch (GtkDrawingArea *area,
             cairo_t        *cr,
             int             width,
             int             height,
             gpointer        user_data)
{
    int slot = GPOINTER_TO_INT (user_data);
    double r = MIN (width, height) / 2.0 - 1;
    double cx = width / 2.0, cy = height / 2.0;

    if (slot == NAUTILUS_FOLDER_COLOR_RAINBOW)
    {
        /* A wheel of all eight colors. */
        for (int i = 0; i < NAUTILUS_FOLDER_COLOR_SLOTS; i++)
        {
            GdkRGBA c;

            nautilus_labels_get_slot_color (i, &c);
            gdk_cairo_set_source_rgba (cr, &c);
            cairo_move_to (cr, cx, cy);
            cairo_arc (cr, cx, cy, r,
                       (i - 2) * G_PI / 4, (i - 1) * G_PI / 4);
            cairo_close_path (cr);
            cairo_fill (cr);
        }
        return;
    }

    if (slot < 0)
    {
        /* None: an outlined circle with a stroke through it. */
        GdkRGBA fg;

        gtk_widget_get_color (GTK_WIDGET (area), &fg);
        fg.alpha *= 0.55;
        gdk_cairo_set_source_rgba (cr, &fg);
        cairo_set_line_width (cr, 1.5);
        cairo_arc (cr, cx, cy, r - 1, 0, 2 * G_PI);
        cairo_stroke (cr);
        cairo_move_to (cr, cx - r * 0.6, cy + r * 0.6);
        cairo_line_to (cr, cx + r * 0.6, cy - r * 0.6);
        cairo_stroke (cr);
        return;
    }

    GdkRGBA c;

    nautilus_labels_get_slot_color (slot, &c);
    gdk_cairo_set_source_rgba (cr, &c);
    cairo_arc (cr, cx, cy, r, 0, 2 * G_PI);
    cairo_fill (cr);
}

static GtkWidget *
swatch_new (int         slot,
            const char *tooltip,
            const char *action)
{
    GtkWidget *button = gtk_button_new ();
    GtkWidget *area = gtk_drawing_area_new ();

    gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (area), 16);
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (area), 16);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw_swatch, GINT_TO_POINTER (slot), NULL);
    gtk_button_set_child (GTK_BUTTON (button), area);
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_add_css_class (button, "circular");
    gtk_widget_add_css_class (button, "swatch");
    gtk_widget_set_tooltip_text (button, tooltip);
    g_object_set_data (G_OBJECT (button), "color-action", (gpointer) action);
    g_signal_connect (button, "clicked", G_CALLBACK (on_swatch_clicked), GINT_TO_POINTER (slot));

    return button;
}

GtkWidget *
nautilus_color_row_new (const char *action,
                        gboolean    rainbow)
{
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_widget_add_css_class (row, "nautilus-folder-color-row");
    gtk_box_append (GTK_BOX (row), swatch_new (-1, _("No Color"), action));
    for (int i = 0; i < NAUTILUS_FOLDER_COLOR_SLOTS; i++)
    {
        gtk_box_append (GTK_BOX (row), swatch_new (i, nautilus_labels_get_slot_name (i), action));
    }
    if (rainbow)
    {
        gtk_box_append (GTK_BOX (row), swatch_new (NAUTILUS_FOLDER_COLOR_RAINBOW,
                                                   _("Rainbow: each selected folder gets the next color"),
                                                   action));
    }

    return row;
}

GtkWidget *
nautilus_folder_color_row_new (void)
{
    return nautilus_color_row_new ("view.folder-color", TRUE);
}

/* ------------------------------------------------------------ Tag dot */

static void
draw_tag_dot (GtkDrawingArea *area,
              cairo_t        *cr,
              int             width,
              int             height,
              gpointer        user_data)
{
    GdkRGBA color;

    nautilus_labels_get_tag_color (user_data, &color);
    gdk_cairo_set_source_rgba (cr, &color);
    cairo_arc (cr, width / 2.0, height / 2.0, MIN (width, height) / 2.0, 0, 2 * G_PI);
    cairo_fill (cr);
}

GtkWidget *
nautilus_tag_dot_new (const char *tag)
{
    GtkWidget *dot = gtk_drawing_area_new ();

    gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (dot), 10);
    gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (dot), 10);
    gtk_widget_set_valign (dot, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot), draw_tag_dot, g_strdup (tag), g_free);

    return dot;
}

/* --------------------------------------------------------- Tags dialog */

typedef struct
{
    AdwDialog *dialog;
    GList *files; /* NautilusFile, owned */
    AdwPreferencesGroup *group;
    GList *rows;
    gboolean updating;
} TagsDialog;

static void
tags_dialog_free (TagsDialog *td)
{
    nautilus_file_list_free (td->files);
    g_list_free (td->rows);
    g_free (td);
}

static void rebuild_tag_rows (TagsDialog *td);

static void
on_tag_toggled (GtkCheckButton *check,
                TagsDialog     *td)
{
    if (td->updating)
    {
        return;
    }

    const char *tag = g_object_get_data (G_OBJECT (check), "tag");

    gtk_check_button_set_inconsistent (check, FALSE);
    nautilus_labels_set_tag (td->files, tag, gtk_check_button_get_active (check));
}

static void
on_tag_color_set (GtkColorDialogButton *button,
                  GParamSpec           *pspec,
                  gpointer              user_data)
{
    const char *tag = g_object_get_data (G_OBJECT (button), "tag");

    nautilus_labels_set_tag_color (tag, gtk_color_dialog_button_get_rgba (button));
}

static void
on_tag_remove (GtkButton  *button,
               TagsDialog *td)
{
    g_autofree char *tag = g_strdup (g_object_get_data (G_OBJECT (button), "tag"));

    /* Take it off these files too, then forget it. */
    nautilus_labels_set_tag (td->files, tag, FALSE);
    nautilus_labels_remove_tag (tag);
}

static void
rebuild_tag_rows (TagsDialog *td)
{
    for (GList *l = td->rows; l != NULL; l = l->next)
    {
        adw_preferences_group_remove (td->group, l->data);
    }
    g_clear_pointer (&td->rows, g_list_free);

    td->updating = TRUE;

    for (guint i = 0; i < nautilus_labels_get_n_tags (); i++)
    {
        const char *tag = nautilus_labels_get_tag_name (i);
        GtkWidget *row = adw_action_row_new ();
        GtkWidget *check = gtk_check_button_new ();
        GtkWidget *color = gtk_color_dialog_button_new (gtk_color_dialog_new ());
        GtkWidget *remove = gtk_button_new_from_icon_name ("user-trash-symbolic");
        guint with = 0, total = 0;
        GdkRGBA rgba;

        for (GList *f = td->files; f != NULL; f = f->next, total++)
        {
            with += nautilus_file_has_tag (f->data, tag) ? 1 : 0;
        }

        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), tag);
        adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);

        g_object_set_data_full (G_OBJECT (check), "tag", g_strdup (tag), g_free);
        gtk_check_button_set_active (GTK_CHECK_BUTTON (check), total > 0 && with == total);
        gtk_check_button_set_inconsistent (GTK_CHECK_BUTTON (check), with > 0 && with < total);
        gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
        g_signal_connect (check, "toggled", G_CALLBACK (on_tag_toggled), td);
        if (td->files != NULL)
        {
            adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
            adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);
        }
        else
        {
            /* Managing tags only, from Preferences. */
            g_object_ref_sink (check);
            g_object_unref (check);
            adw_action_row_add_prefix (ADW_ACTION_ROW (row), nautilus_tag_dot_new (tag));
        }

        nautilus_labels_get_tag_color (tag, &rgba);
        gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (color), &rgba);
        gtk_widget_set_valign (color, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text (color, _("Tag Color"));
        g_object_set_data_full (G_OBJECT (color), "tag", g_strdup (tag), g_free);
        g_signal_connect (color, "notify::rgba", G_CALLBACK (on_tag_color_set), NULL);
        adw_action_row_add_suffix (ADW_ACTION_ROW (row), color);

        gtk_widget_add_css_class (remove, "flat");
        gtk_widget_set_valign (remove, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text (remove, _("Delete Tag"));
        g_object_set_data_full (G_OBJECT (remove), "tag", g_strdup (tag), g_free);
        g_signal_connect (remove, "clicked", G_CALLBACK (on_tag_remove), td);
        adw_action_row_add_suffix (ADW_ACTION_ROW (row), remove);

        adw_preferences_group_add (td->group, row);
        td->rows = g_list_append (td->rows, row);
    }

    td->updating = FALSE;
}

static void
on_tags_changed (NautilusLabels *labels,
                 AdwDialog      *dialog)
{
    rebuild_tag_rows (g_object_get_data (G_OBJECT (dialog), "tags-dialog"));
}

static void
on_new_tag (AdwEntryRow *entry,
            TagsDialog  *td)
{
    g_autofree char *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));

    if (name[0] == '\0' || strchr (name, ',') != NULL)
    {
        gtk_widget_error_bell (GTK_WIDGET (entry));
        return;
    }

    /* A new tag goes straight onto the files. */
    if (td->files != NULL)
    {
        nautilus_labels_set_tag (td->files, name, TRUE);
    }
    else
    {
        nautilus_labels_ensure_tag (name);
    }
    gtk_editable_set_text (GTK_EDITABLE (entry), "");
}

void
nautilus_tags_dialog_present (GList     *files,
                              GtkWidget *parent)
{
    TagsDialog *td = g_new0 (TagsDialog, 1);
    GtkWidget *toolbar = adw_toolbar_view_new ();
    GtkWidget *page = adw_preferences_page_new ();
    GtkWidget *new_group = adw_preferences_group_new ();
    GtkWidget *entry = adw_entry_row_new ();
    guint n = g_list_length (files);

    td->files = nautilus_file_list_copy (files);
    td->dialog = adw_dialog_new ();
    td->group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

    adw_dialog_set_title (td->dialog, _("Tags"));
    adw_dialog_set_content_width (td->dialog, 380);
    adw_dialog_set_content_height (td->dialog, 460);

    g_autofree char *description = n == 0
                                   ? g_strdup (_("Give files tags from their context menu or with Ctrl+Alt+A."))
                                   : n == 1
                                   ? g_strdup_printf (_("Tags of “%s”"), nautilus_file_get_display_name (files->data))
                                   : g_strdup_printf (ngettext ("Tags of %u item", "Tags of %u items", n), n);
    adw_preferences_group_set_description (td->group, description);

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (entry), _("New Tag"));
    adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (entry), TRUE);
    g_signal_connect (entry, "apply", G_CALLBACK (on_new_tag), td);
    g_signal_connect (entry, "entry-activated", G_CALLBACK (on_new_tag), td);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (new_group), entry);

    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (new_group));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), td->group);
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), page);
    adw_dialog_set_child (td->dialog, toolbar);

    rebuild_tag_rows (td);
    g_object_set_data_full (G_OBJECT (td->dialog), "tags-dialog", td, (GDestroyNotify) tags_dialog_free);
    g_signal_connect_object (nautilus_labels_get (), "tags-changed",
                             G_CALLBACK (on_tags_changed), td->dialog, G_CONNECT_DEFAULT);

    adw_dialog_present (td->dialog, parent);
    gtk_widget_grab_focus (entry);
}

/* ---------------------------------------------------- Preferences page */

static void
draw_scheme_dot (GtkDrawingArea *area,
             cairo_t        *cr,
             int             width,
             int             height,
             gpointer        user_data)
{
    const GdkRGBA *color = user_data;

    cairo_arc (cr, width / 2.0, height / 2.0, MIN (width, height) / 2.0, 0, 2 * G_PI);
    gdk_cairo_set_source_rgba (cr, color);
    cairo_fill (cr);
}

static GtkWidget *
scheme_preview_new (int scheme)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GdkRGBA colors[NAUTILUS_FOLDER_COLOR_SLOTS];

    nautilus_labels_get_scheme_colors (scheme, colors);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);

    for (int i = 0; i < NAUTILUS_FOLDER_COLOR_SLOTS; i++)
    {
        GtkWidget *dot = gtk_drawing_area_new ();

        gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (dot), 14);
        gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (dot), 14);
        gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot), draw_scheme_dot,
                                        g_memdup2 (&colors[i], sizeof (GdkRGBA)), g_free);
        gtk_box_append (GTK_BOX (box), dot);
    }

    return box;
}

static GtkWidget *
switch_row_new (const char *title,
                const char *subtitle,
                const char *key)
{
    GtkWidget *row = adw_switch_row_new ();

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (row), TRUE);
    if (subtitle != NULL)
    {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
    }
    g_settings_bind (nautilus_flier_settings (), key, row, "active", G_SETTINGS_BIND_DEFAULT);

    return row;
}

/* A switch per sidebar group or place: on unless its id is in the row's
 * list key (sidebar-hidden-groups or sidebar-hidden-places). */
static void
sync_group_switch (GtkWidget *row)
{
    const char *key = g_object_get_data (G_OBJECT (row), "list-key");
    g_auto (GStrv) hidden = g_settings_get_strv (nautilus_flier_settings (), key);
    const char *id = g_object_get_data (G_OBJECT (row), "group-id");

    adw_switch_row_set_active (ADW_SWITCH_ROW (row), !g_strv_contains ((const char * const *) hidden, id));
}

static void
on_group_switch (GtkWidget *row)
{
    const char *key = g_object_get_data (G_OBJECT (row), "list-key");
    g_auto (GStrv) hidden = g_settings_get_strv (nautilus_flier_settings (), key);
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
    const char *id = g_object_get_data (G_OBJECT (row), "group-id");
    gboolean show = adw_switch_row_get_active (ADW_SWITCH_ROW (row));

    for (guint i = 0; hidden[i] != NULL; i++)
    {
        if (!g_str_equal (hidden[i], id))
        {
            g_strv_builder_add (builder, hidden[i]);
        }
    }
    if (!show)
    {
        g_strv_builder_add (builder, id);
    }

    g_auto (GStrv) result = g_strv_builder_end (builder);

    if (!g_strv_equal ((const char * const *) result, (const char * const *) hidden))
    {
        g_settings_set_strv (nautilus_flier_settings (), key, (const char * const *) result);
    }
}

static GtkWidget *
list_switch_new (const char *key,
                 const char *title,
                 const char *id)
{
    GtkWidget *row = adw_switch_row_new ();
    g_autofree char *signal = g_strconcat ("changed::", key, NULL);

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (row), TRUE);
    g_object_set_data (G_OBJECT (row), "group-id", (gpointer) id);
    g_object_set_data (G_OBJECT (row), "list-key", (gpointer) key);
    sync_group_switch (row);
    g_signal_connect (row, "notify::active", G_CALLBACK (on_group_switch), NULL);
    g_signal_connect_object (nautilus_flier_settings (), signal,
                             G_CALLBACK (sync_group_switch), row, G_CONNECT_SWAPPED);

    return row;
}

static GtkWidget *
group_switch_new (const char *title,
                  const char *id)
{
    return list_switch_new ("sidebar-hidden-groups", title, id);
}

static void
on_sidebar_density_selected (AdwComboRow *row)
{
    GSettings *settings = nautilus_flier_settings ();
    guint selected = adw_combo_row_get_selected (row);

    if (selected != GTK_INVALID_LIST_POSITION && (int) selected != g_settings_get_enum (settings, "sidebar-density"))
    {
        g_settings_set_enum (settings, "sidebar-density", selected);
    }
}

static void
on_sidebar_density_changed (GSettings   *settings,
                            const char  *key,
                            AdwComboRow *row)
{
    adw_combo_row_set_selected (row, g_settings_get_enum (settings, key));
}

/* The colour theme picker: a small window in each theme's colours. */
static const struct
{
    int value;          /* in the ColorTheme enum */
    const char *nick;
    const char *title;
} color_themes[] =
{
    /* As shown: the GNOME-style ones first. */
    { 0, "mako", N_("Mako") }, { 1, "uniform", N_("Uniform") }, { 2, "classic", N_("Classic") },
    { 3, "graphite", N_("Graphite") }, { 13, "fog", N_("Fog") }, { 4, "deep-sea", N_("Deep Sea") },
    { 5, "synthwave", N_("Synthwave") }, { 6, "matcha", N_("Matcha") }, { 7, "nord", N_("Nord") },
    { 8, "dracula", N_("Dracula") }, { 9, "gruvbox", N_("Gruvbox") }, { 10, "catppuccin", N_("Catppuccin") },
    { 11, "solarized", N_("Solarized") }, { 12, "kids", N_("Kids") },
};

static void
on_theme_choice_toggled (GtkToggleButton *button)
{
    GSettings *settings = nautilus_flier_settings ();
    const char *custom = g_object_get_data (G_OBJECT (button), "custom");
    int theme = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "theme"));
    g_autofree char *current = g_settings_get_string (settings, "custom-theme");

    if (!gtk_toggle_button_get_active (button))
    {
        return;
    }
    if (custom != NULL)
    {
        if (g_strcmp0 (current, custom) != 0)
        {
            g_settings_set_string (settings, "custom-theme", custom);
        }
        return;
    }
    if (*current != '\0')
    {
        g_settings_set_string (settings, "custom-theme", "");
    }
    if (g_settings_get_enum (settings, "color-theme") != theme)
    {
        g_settings_set_enum (settings, "color-theme", theme);
    }
}

/* The choice in use: a custom theme when one is set and there, else the
 * built-in one. */
static void
on_theme_setting_changed (GSettings  *settings,
                          const char *key,
                          GtkWidget  *picker)
{
    int theme = g_settings_get_enum (settings, "color-theme");
    g_autofree char *custom = g_settings_get_string (settings, "custom-theme");
    gboolean use_custom = *custom != '\0' && nautilus_custom_themes_has (custom);

    for (GtkWidget *child = gtk_widget_get_first_child (picker); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        GtkWidget *button = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
        const char *id = g_object_get_data (G_OBJECT (button), "custom");

        if (use_custom ? g_strcmp0 (id, custom) == 0
                       : id == NULL && GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "theme")) == theme)
        {
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
        }
    }
}

/* The GNOME-style previews show the accent colour of the desktop. */
static void
update_preview_accent (AdwStyleManager *manager,
                       GParamSpec      *pspec,
                       GtkCssProvider  *provider)
{
    GdkRGBA rgba;
    g_autofree char *color = NULL;
    g_autofree char *css = NULL;

    adw_accent_color_to_rgba (adw_style_manager_get_accent_color (manager), &rgba);
    color = gdk_rgba_to_string (&rgba);
    css = g_strdup_printf (".theme-preview.theme-mako, .theme-preview.theme-uniform, "
                           ".theme-preview.theme-classic, .theme-preview.theme-graphite "
                           "{ --accent-bg-color: %s; }", color);
    gtk_css_provider_load_from_string (provider, css);
}

static GtkWidget *
theme_preview_new (const char *nick)
{
    GtkWidget *preview = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *main = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *view = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
    g_autofree char *class = g_strconcat ("theme-", nick, NULL);

    gtk_widget_add_css_class (preview, "theme-preview");
    gtk_widget_add_css_class (preview, class);
    gtk_widget_set_overflow (preview, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class (side, "tp-side");
    gtk_widget_add_css_class (main, "tp-main");
    gtk_widget_add_css_class (head, "tp-head");
    gtk_widget_add_css_class (view, "tp-view");
    gtk_widget_set_hexpand (main, TRUE);
    gtk_widget_set_vexpand (view, TRUE);

    for (int i = 0; i < 3; i++)
    {
        GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

        gtk_widget_add_css_class (line, "tp-line");
        gtk_widget_set_margin_end (line, i == 1 ? 6 : 0);
        gtk_box_append (GTK_BOX (side), line);
    }
    {
        GtkWidget *accent = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

        gtk_widget_add_css_class (accent, "tp-accent");
        gtk_widget_set_halign (accent, GTK_ALIGN_START);
        gtk_box_append (GTK_BOX (view), accent);
    }
    for (int i = 0; i < 2; i++)
    {
        GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

        gtk_widget_add_css_class (line, "tp-line");
        gtk_widget_set_margin_end (line, i == 0 ? 14 : 30);
        gtk_box_append (GTK_BOX (view), line);
    }

    gtk_box_append (GTK_BOX (main), head);
    gtk_box_append (GTK_BOX (main), view);
    gtk_box_append (GTK_BOX (preview), side);
    gtk_box_append (GTK_BOX (preview), main);
    return preview;
}

static GtkWidget *
theme_choice_new (GtkWidget  *preview,
                  const char *title,
                  GtkWidget  *group)
{
    GtkWidget *button = gtk_toggle_button_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *label = gtk_label_new (title);

    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 14);
    gtk_box_append (GTK_BOX (box), preview);
    gtk_box_append (GTK_BOX (box), label);
    gtk_button_set_child (GTK_BUTTON (button), box);
    gtk_widget_add_css_class (button, "theme-choice");
    gtk_widget_set_tooltip_text (button, title);
    if (group != NULL)
    {
        gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (button), GTK_TOGGLE_BUTTON (group));
    }
    return button;
}

/* The built-in themes, then the user's own. */
static void
fill_theme_picker (GtkWidget *picker)
{
    GPtrArray *custom = nautilus_custom_themes_list ();
    GtkWidget *first = NULL;

    gtk_flow_box_remove_all (GTK_FLOW_BOX (picker));
    for (guint i = 0; i < G_N_ELEMENTS (color_themes); i++)
    {
        GtkWidget *button = theme_choice_new (theme_preview_new (color_themes[i].nick),
                                              _(color_themes[i].title), first);

        first = first != NULL ? first : button;
        g_object_set_data (G_OBJECT (button), "theme", GINT_TO_POINTER (color_themes[i].value));
        gtk_flow_box_append (GTK_FLOW_BOX (picker), button);
    }
    for (guint i = 0; i < custom->len; i++)
    {
        NautilusCustomTheme *theme = g_ptr_array_index (custom, i);
        g_autofree char *class = nautilus_custom_themes_class (theme->id);
        GtkWidget *preview = theme_preview_new ("mako");
        GtkWidget *button;

        /* GNOME's colours under it, the theme's on top. */
        gtk_widget_add_css_class (preview, class);
        button = theme_choice_new (preview, theme->name, first);
        g_object_set_data_full (G_OBJECT (button), "custom", g_strdup (theme->id), g_free);
        gtk_flow_box_append (GTK_FLOW_BOX (picker), button);
    }

    on_theme_setting_changed (nautilus_flier_settings (), NULL, picker);
    for (GtkWidget *child = gtk_widget_get_first_child (picker); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        g_signal_connect (gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child)), "toggled",
                          G_CALLBACK (on_theme_choice_toggled), NULL);
    }
}

static GtkWidget *
theme_picker_new (void)
{
    GSettings *settings = nautilus_flier_settings ();
    GtkWidget *picker = gtk_flow_box_new ();
    static GtkCssProvider *provider = NULL;

    gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (picker), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (picker), TRUE);
    gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (picker), 3);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (picker), 5);
    gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (picker), 4);
    gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (picker), 4);

    /* Once for the app: the desktop's accent on the GNOME-style previews. */
    if (provider == NULL)
    {
        AdwStyleManager *manager = adw_style_manager_get_default ();

        provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        g_signal_connect (manager, "notify::accent-color", G_CALLBACK (update_preview_accent), provider);
        update_preview_accent (manager, NULL, provider);
    }

    fill_theme_picker (picker);
    g_signal_connect_object (settings, "changed::color-theme", G_CALLBACK (on_theme_setting_changed), picker, 0);
    g_signal_connect_object (settings, "changed::custom-theme", G_CALLBACK (on_theme_setting_changed), picker, 0);
    g_signal_connect_object (nautilus_custom_themes_get (), "changed", G_CALLBACK (fill_theme_picker), picker,
                             G_CONNECT_SWAPPED);
    return picker;
}

static void
on_new_theme (GtkButton *button)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GFile) file = nautilus_custom_themes_new_from_template (&error);

    if (file == NULL)
    {
        g_warning ("New theme: %s", error->message);
        return;
    }
    {
        g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);
        g_autofree char *id = g_file_get_basename (file);

        id[strlen (id) - strlen (".css")] = '\0';
        /* Picked at once, so every save shows in the window. */
        g_settings_set_string (nautilus_flier_settings (), "custom-theme", id);
        gtk_file_launcher_launch (launcher, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (button))), NULL, NULL, NULL);
    }
}

static void
on_open_themes_folder (GtkButton *button)
{
    g_autoptr (GFile) folder = nautilus_custom_themes_get_folder ();

    g_file_make_directory_with_parents (folder, NULL, NULL);
    nautilus_application_open_location_full (NAUTILUS_APPLICATION (g_application_get_default ()), folder,
                                             NAUTILUS_OPEN_FLAG_NEW_WINDOW, NULL, NULL);
}

static GtkWidget *
theme_buttons_new (void)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add = gtk_button_new_with_mnemonic (_("_New Theme"));
    GtkWidget *open = gtk_button_new_with_mnemonic (_("Open Themes _Folder"));

    gtk_widget_set_tooltip_text (add, _("Start from a template that explains every color, and edit it: each save shows at once"));
    gtk_widget_set_tooltip_text (open, _("Your themes live in ~/.config/mako/themes. Drop a theme's .css file there to add it"));
    gtk_widget_add_css_class (add, "pill");
    gtk_widget_add_css_class (open, "pill");
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top (box, 8);
    g_signal_connect (add, "clicked", G_CALLBACK (on_new_theme), NULL);
    g_signal_connect (open, "clicked", G_CALLBACK (on_open_themes_folder), NULL);
    gtk_box_append (GTK_BOX (box), add);
    gtk_box_append (GTK_BOX (box), open);
    return box;
}

static void
on_manage_tags (AdwActionRow *row)
{
    nautilus_tags_dialog_present (NULL, GTK_WIDGET (row));
}

/* Mako's pages of Preferences: Appearance, Sidebar and Organize; the
 * startup and copying settings go on @general, the shortcut hints on
 * @shortcuts. */
void
nautilus_labels_add_preferences_pages (AdwPreferencesDialog *dialog,
                                       AdwPreferencesPage   *general,
                                       AdwPreferencesPage   *shortcuts)
{
    GSettings *settings = nautilus_flier_settings ();
    GtkWidget *page;
    GtkWidget *appearance = adw_preferences_page_new ();
    GtkWidget *sidebar = adw_preferences_page_new ();
    GtkWidget *organize = adw_preferences_page_new ();
    GtkWidget *group, *row, *colors_switch, *sidebar_density_row = NULL;
    g_autoptr (GSimpleActionGroup) actions = g_simple_action_group_new ();
    g_autoptr (GAction) scheme_action = g_settings_create_action (settings, "folder-color-scheme");
    g_autoptr (GAction) density_action = g_settings_create_action (settings, "density");
    GtkWidget *first_check = NULL;
    static const char *scheme_nicks[] = { "pastel", "vibrant", "earth", "ocean", "mono" };

    adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (appearance), _("Appearance"));
    adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (appearance), "applications-graphics-symbolic");
    adw_preferences_page_set_name (ADW_PREFERENCES_PAGE (appearance), "appearance");
    adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (sidebar), _("Sidebar"));
    adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (sidebar), "sidebar-show-symbolic");
    adw_preferences_page_set_name (ADW_PREFERENCES_PAGE (sidebar), "sidebar");
    adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (organize), _("Organize"));
    adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (organize), "starred-symbolic");
    adw_preferences_page_set_name (ADW_PREFERENCES_PAGE (organize), "organize");

    g_action_map_add_action (G_ACTION_MAP (actions), scheme_action);
    g_action_map_add_action (G_ACTION_MAP (actions), density_action);
    gtk_widget_insert_action_group (appearance, "flier", G_ACTION_GROUP (actions));

    /* Colour theme: a picture of each */
    page = appearance;
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Color Theme"));
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                           _("The first five keep to GNOME, the rest are for fun. Each follows light and dark. Your own themes come after them."));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), theme_picker_new ());
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), theme_buttons_new ());
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Window: frames and buttons */
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Window"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("_Framed Panels"),
                                               _("Each tile a rounded card of its own with a gap between them, and the inspector a card too"),
                                               "framed-panels"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("_Panel Buttons"),
                                               _("The sidebar and inspector buttons at the ends of the header bar. Without them, use F9 and F4, or the view menu"),
                                               "panel-buttons"));
    {
        GtkWidget *opacity_row = adw_spin_row_new_with_range (40, 100, 5);

        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (opacity_row), _("Window _Opacity"));
        adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (opacity_row), TRUE);
        adw_action_row_set_subtitle (ADW_ACTION_ROW (opacity_row),
                                     _("Below 100% the desktop shows through. To blur it, add a shell extension that blurs behind windows, such as Blur my Shell"));
        g_settings_bind (nautilus_flier_settings (), "window-opacity", opacity_row, "value", G_SETTINGS_BIND_DEFAULT);
        adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), opacity_row);
    }
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Density */
    {
        static const struct
        {
            const char *nick;
            const char *title;
            const char *subtitle;
        } densities[] =
        {
            { "comfortable", N_("_Comfortable"), N_("As in GNOME Files") },
            { "compact", N_("C_ompact"), N_("Less room around rows and items, smaller icons") },
            { "dense", N_("_Dense"), N_("Rows close together, smaller icons") },
            { "extra-dense", N_("_Extra Dense"), N_("As many rows as fit, the smallest icons") },
        };
        GtkWidget *first = NULL;

        group = adw_preferences_group_new ();
        adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Density"));
        adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                               _("The list, grid and columns. The sidebar has its own, under Sidebar."));
        for (guint i = 0; i < G_N_ELEMENTS (densities); i++)
        {
            GtkWidget *check = gtk_check_button_new ();
            g_autofree char *action = g_strdup_printf ("flier.density('%s')", densities[i].nick);

            row = adw_action_row_new ();
            adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _(densities[i].title));
            adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (row), TRUE);
            adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _(densities[i].subtitle));
            gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
            gtk_actionable_set_detailed_action_name (GTK_ACTIONABLE (check), action);
            if (first != NULL)
            {
                gtk_check_button_set_group (GTK_CHECK_BUTTON (check), GTK_CHECK_BUTTON (first));
            }
            else
            {
                first = check;
            }
            adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
            adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);
            adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
        }
        adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                                   switch_row_new (_("_Alternating Row Colors"),
                                                   _("Every other row a shade apart in the list, the expandable list and columns"),
                                                   "alternate-rows"));
        {
            const char *names[] = { _("Comfortable"), _("Compact"), _("Dense"), _("Extra Dense"), NULL };
            g_autoptr (GtkStringList) list = gtk_string_list_new (names);

            row = adw_combo_row_new ();
            adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("_Density"));
            adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (row), TRUE);
            adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (list));
            adw_combo_row_set_selected (ADW_COMBO_ROW (row),
                                        g_settings_get_enum (settings, "sidebar-density"));
            g_signal_connect (row, "notify::selected", G_CALLBACK (on_sidebar_density_selected), NULL);
            g_signal_connect_object (settings, "changed::sidebar-density",
                                     G_CALLBACK (on_sidebar_density_changed), row, 0);
            sidebar_density_row = row;
        }
        adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));
    }

    /* Ratings and tags */
    page = organize;
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Ratings and Tags"));
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                           _("Stored on the files themselves, so other apps can read them."));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("_Ratings"), _("Rate with Ctrl+Alt+1–5, or Alt and click the stars"), "ratings"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("_Tags"), NULL, "tags"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Show on _Grid Items"), NULL, "marks-on-tiles"));

    row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("_Manage Tags"));
    adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (row), TRUE);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("go-next-symbolic"));
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
    g_signal_connect (row, "activated", G_CALLBACK (on_manage_tags), NULL);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Folder colors */
    page = appearance;
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Folder Colors"));
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                           _("Folders keep their color when the scheme changes."));
    colors_switch = switch_row_new (_("_Folder Colors"), NULL, "folder-colors");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), colors_switch);

    for (guint i = 0; i < nautilus_labels_get_n_schemes () && i < G_N_ELEMENTS (scheme_nicks); i++)
    {
        GtkWidget *check = gtk_check_button_new ();
        g_autofree char *action = g_strdup_printf ("flier.folder-color-scheme('%s')", scheme_nicks[i]);

        row = adw_action_row_new ();
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), nautilus_labels_get_scheme_name (i));
        if (i == 4)
        {
            adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _("Shades of the accent color"));
        }

        gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
        gtk_actionable_set_detailed_action_name (GTK_ACTIONABLE (check), action);
        if (first_check != NULL)
        {
            gtk_check_button_set_group (GTK_CHECK_BUTTON (check), GTK_CHECK_BUTTON (first_check));
        }
        else
        {
            first_check = check;
        }
        adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
        adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);
        adw_action_row_add_suffix (ADW_ACTION_ROW (row), scheme_preview_new (i));

        g_object_bind_property (colors_switch, "active", row, "sensitive", G_BINDING_SYNC_CREATE);
        adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
    }
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Startup */
    page = GTK_WIDGET (general);
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Startup"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Run in the _Background"),
                                               _("Keep Mako running after the last window closes, so new windows open at once"),
                                               "run-in-background"));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Copying and moving */
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Copying"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("_Progress Panel"),
                                               _("Show running copies and other operations in the corner of the window"),
                                               "progress-popup"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Copy _One After Another"),
                                               _("A copy or move started while another runs waits for it, instead of both sharing the disk"),
                                               "queue-transfers"));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Learning the keys */
    page = GTK_WIDGET (shortcuts);
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Shortcuts"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Shortcuts in _Menus"),
                                               _("Show the keys in the bottom bar's view and sort menus"),
                                               "menu-shortcuts"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Shortcut _Hints"),
                                               _("Briefly show what a shortcut did"),
                                               "shortcut-hints"));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Sidebar */
    page = sidebar;
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Sidebar"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), sidebar_density_row);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Sidebar _Groups"),
                                               _("Bookmarks, Places, Storage, Recent and Tags as groups that fold away"),
                                               "sidebar-groups"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Storage _Usage"),
                                               _("A thin bar under each drive shows how full it is"),
                                               "sidebar-storage-usage"));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Which sidebar groups there are */
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Sidebar Sections"));
    adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
                                           _("Drag a section's header in the sidebar to move it."));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), group_switch_new (_("_Bookmarks"), "bookmarks"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), group_switch_new (_("_Places"), "places"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), group_switch_new (_("_Storage"), "devices"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), group_switch_new (_("_Cloud"), "cloud"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), group_switch_new (_("_Recent Folders"), "recent"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), group_switch_new (_("_Tags"), "tags"));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* What the Places section holds */
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Places"));
    {
        static const struct { const char *title; const char *id; } places[] =
        {
            { N_("Home"), "home" }, { N_("Recent Files"), "recent" }, { N_("Starred"), "starred" },
            { N_("Downloads"), "downloads" }, { N_("Documents"), "documents" },
            { N_("Pictures"), "pictures" }, { N_("Videos"), "videos" }, { N_("Music"), "music" },
            { N_("Network"), "network" }, { N_("Trash"), "trash" },
        };

        for (guint i = 0; i < G_N_ELEMENTS (places); i++)
        {
            adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                                       list_switch_new ("sidebar-hidden-places",
                                                        _(places[i].title), places[i].id));
        }
    }
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* Quick Look */
    page = organize;
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Quick Look"));
    row = switch_row_new (_("_Built-in Quick Look"),
                          _("Preview several files at once, rate them and play videos"),
                          "own-quick-look");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Show _Names"), NULL, "quick-look-names"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Show Ratings and Ta_gs"), NULL, "quick-look-marks"));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    adw_preferences_dialog_add (dialog, ADW_PREFERENCES_PAGE (appearance));
    adw_preferences_dialog_add (dialog, ADW_PREFERENCES_PAGE (sidebar));
    adw_preferences_dialog_add (dialog, ADW_PREFERENCES_PAGE (organize));
}
