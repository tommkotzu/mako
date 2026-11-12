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

static void
on_manage_tags (AdwActionRow *row)
{
    nautilus_tags_dialog_present (NULL, GTK_WIDGET (row));
}

GtkWidget *
nautilus_labels_preferences_page_new (void)
{
    GSettings *settings = nautilus_flier_settings ();
    GtkWidget *page = adw_preferences_page_new ();
    GtkWidget *group, *row, *colors_switch;
    g_autoptr (GSimpleActionGroup) actions = g_simple_action_group_new ();
    g_autoptr (GAction) scheme_action = g_settings_create_action (settings, "folder-color-scheme");
    g_autoptr (GAction) density_action = g_settings_create_action (settings, "density");
    GtkWidget *first_check = NULL;
    static const char *scheme_nicks[] = { "pastel", "vibrant", "earth", "ocean", "mono" };

    adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (page), _("Organize"));
    adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (page), "starred-symbolic");

    g_action_map_add_action (G_ACTION_MAP (actions), scheme_action);
    g_action_map_add_action (G_ACTION_MAP (actions), density_action);
    gtk_widget_insert_action_group (page, "flier", G_ACTION_GROUP (actions));

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
                                               _("The list, grid and columns. The sidebar has its own."));
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
        {
            const char *names[] = { _("Comfortable"), _("Compact"), _("Dense"), _("Extra Dense"), NULL };
            g_autoptr (GtkStringList) list = gtk_string_list_new (names);

            row = adw_combo_row_new ();
            adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("_Sidebar"));
            adw_preferences_row_set_use_underline (ADW_PREFERENCES_ROW (row), TRUE);
            adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (list));
            adw_combo_row_set_selected (ADW_COMBO_ROW (row),
                                        g_settings_get_enum (settings, "sidebar-density"));
            g_signal_connect (row, "notify::selected", G_CALLBACK (on_sidebar_density_selected), NULL);
            g_signal_connect_object (settings, "changed::sidebar-density",
                                     G_CALLBACK (on_sidebar_density_changed), row, 0);
            adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
        }
        adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));
    }

    /* Ratings and tags */
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
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Startup"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               switch_row_new (_("Run in the _Background"),
                                               _("Keep Mako running after the last window closes, so new windows open at once"),
                                               "run-in-background"));
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (group));

    /* See-through window */
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Appearance"));
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
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), _("Sidebar"));
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

    return page;
}
