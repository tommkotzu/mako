/* nautilus-preferences-window.c - Functions to create and show the nautilus
 *  preference window.
 *
 *  Copyright (C) 2002 Jan Arne Petersen
 *  Copyright (C) 2016 Carlos Soriano <csoriano@gnome.com>
 *
 *  The Gnome Library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  The Gnome Library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with the Gnome Library; see the file COPYING.LIB.  If not,
 *  see <http://www.gnu.org/licenses/>.
 *
 *  Authors: Jan Arne Petersen <jpetersen@uni-bonn.de>
 */

#include <config.h>

#include "nautilus-preferences-dialog.h"
#include "nautilus-shortcuts.h"

#include "nautilus-labels-ui.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <nautilus-extension.h>

#include "nautilus-column-utilities.h"
#include "nautilus-date-utilities.h"
#include "nautilus-global-preferences.h"

/* bool preferences */
#define NAUTILUS_PREFERENCES_DIALOG_FOLDERS_FIRST_WIDGET                       \
        "sort_folders_first_row"
#define NAUTILUS_PREFERENCES_DIALOG_DELETE_PERMANENTLY_WIDGET                  \
        "show_delete_permanently_row"
#define NAUTILUS_PREFERENCES_DIALOG_CREATE_LINK_WIDGET                         \
        "show_create_link_row"
#define NAUTILUS_PREFERENCES_DIALOG_LIST_VIEW_USE_TREE_WIDGET                  \
        "use_tree_view_row"

/* combo preferences */
#define NAUTILUS_PREFERENCES_DIALOG_OPEN_ACTION_COMBO                          \
        "open_action_row"
#define NAUTILUS_PREFERENCES_DIALOG_SEARCH_RECURSIVE_ROW                       \
        "search_recursive_row"
#define NAUTILUS_PREFERENCES_DIALOG_THUMBNAILS_ROW                       \
        "thumbnails_row"
#define NAUTILUS_PREFERENCES_DIALOG_COUNT_ROW                       \
        "count_row"

static const char * const speed_tradeoff_values[] =
{
    "local-only", "always", "never",
    NULL
};

static const char * const click_behavior_values[] = {"single", "double", NULL};

static void
bind_builder_bool (GtkBuilder *builder,
                   GSettings  *settings,
                   const char *widget_name,
                   const char *prefs)
{
    g_settings_bind (settings, prefs, gtk_builder_get_object (builder, widget_name),
                     "active", G_SETTINGS_BIND_DEFAULT);
}

/* Translators: Both %s will be replaced with formatted timestamps. */
#define DATE_FORMAT_ROW_SUBTITLE _("Examples: “%s”, “%s”")

static void
setup_detailed_date (GtkBuilder *builder)
{
    AdwActionRow *simple_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "date_format_simple_row"));
    AdwActionRow *detailed_row = ADW_ACTION_ROW (gtk_builder_get_object (builder, "date_format_detailed_row"));

    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autoptr (GDateTime) earlier = g_date_time_add_days (now, -3);

    g_autofree gchar *simple_date_now = nautilus_date_preview_detailed_format (now, FALSE);
    g_autofree gchar *simple_date_earlier = nautilus_date_preview_detailed_format (earlier, FALSE);
    g_autofree gchar *simple_row_subtitle = g_strdup_printf (DATE_FORMAT_ROW_SUBTITLE,
                                                             simple_date_now,
                                                             simple_date_earlier);
    adw_action_row_set_subtitle (simple_row, simple_row_subtitle);

    g_autofree gchar *detailed_date_now = nautilus_date_preview_detailed_format (now, TRUE);
    g_autofree gchar *detailed_date_earlier = nautilus_date_preview_detailed_format (earlier, TRUE);
    g_autofree gchar *detailed_row_subtitle = g_strdup_printf (DATE_FORMAT_ROW_SUBTITLE,
                                                               detailed_date_now,
                                                               detailed_date_earlier);
    adw_action_row_set_subtitle (detailed_row, detailed_row_subtitle);
}

static GVariant *
combo_row_mapping_set (const GValue       *gvalue,
                       const GVariantType *expected_type,
                       gpointer            user_data)
{
    const gchar **values = user_data;

    return g_variant_new_string (values[g_value_get_uint (gvalue)]);
}

static gboolean
combo_row_mapping_get (GValue   *gvalue,
                       GVariant *variant,
                       gpointer  user_data)
{
    const gchar **values = user_data;
    const gchar *value;

    value = g_variant_get_string (variant, NULL);

    for (int i = 0; values[i]; i++)
    {
        if (g_strcmp0 (value, values[i]) == 0)
        {
            g_value_set_uint (gvalue, i);

            return TRUE;
        }
    }

    return FALSE;
}

static void
bind_builder_combo_row (GtkBuilder  *builder,
                        GSettings   *settings,
                        const char  *widget_name,
                        const char  *prefs,
                        const char **values)
{
    g_settings_bind_with_mapping (settings, prefs, gtk_builder_get_object (builder, widget_name),
                                  "selected", G_SETTINGS_BIND_DEFAULT,
                                  combo_row_mapping_get, combo_row_mapping_set,
                                  (gpointer) values, NULL);
}

static void
setup_combo (GtkBuilder  *builder,
             const char  *widget_name,
             const char **strings)
{
    AdwComboRow *combo_row;
    g_autoptr (GtkStringList) list_store = NULL;

    combo_row = (AdwComboRow *) gtk_builder_get_object (builder, widget_name);
    g_assert (ADW_IS_COMBO_ROW (combo_row));

    list_store = gtk_string_list_new (strings);
    adw_combo_row_set_model (combo_row, G_LIST_MODEL (list_store));
}

static void
nautilus_preferences_dialog_setup (GtkBuilder *builder)
{
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_OPEN_ACTION_COMBO,
                 (const char *[]) { _("Single-Click"), _("Double-Click"), NULL });
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_SEARCH_RECURSIVE_ROW,
                 (const char *[]) { _("On This Device Only"), _("All Locations"), _("Never"), NULL });
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_THUMBNAILS_ROW,
                 (const char *[]) { _("On This Device Only"), _("All Files"), _("Never"), NULL });
    setup_combo (builder, NAUTILUS_PREFERENCES_DIALOG_COUNT_ROW,
                 (const char *[]) { _("On This Device Only"), _("All Folders"), _("Never"), NULL });

    /* setup preferences */
    bind_builder_bool (builder, gtk_filechooser_preferences,
                       NAUTILUS_PREFERENCES_DIALOG_FOLDERS_FIRST_WIDGET,
                       NAUTILUS_PREFERENCES_SORT_DIRECTORIES_FIRST);
    bind_builder_bool (builder, nautilus_list_view_preferences,
                       NAUTILUS_PREFERENCES_DIALOG_LIST_VIEW_USE_TREE_WIDGET,
                       NAUTILUS_PREFERENCES_LIST_VIEW_USE_TREE);
    bind_builder_bool (builder, nautilus_preferences,
                       NAUTILUS_PREFERENCES_DIALOG_CREATE_LINK_WIDGET,
                       NAUTILUS_PREFERENCES_SHOW_CREATE_LINK);
    bind_builder_bool (builder, nautilus_preferences,
                       NAUTILUS_PREFERENCES_DIALOG_DELETE_PERMANENTLY_WIDGET,
                       NAUTILUS_PREFERENCES_SHOW_DELETE_PERMANENTLY);

    setup_detailed_date (builder);

    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_OPEN_ACTION_COMBO,
                            NAUTILUS_PREFERENCES_CLICK_POLICY,
                            (const char **) click_behavior_values);
    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_SEARCH_RECURSIVE_ROW,
                            NAUTILUS_PREFERENCES_RECURSIVE_SEARCH,
                            (const char **) speed_tradeoff_values);
    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_THUMBNAILS_ROW,
                            NAUTILUS_PREFERENCES_SHOW_FILE_THUMBNAILS,
                            (const char **) speed_tradeoff_values);
    bind_builder_combo_row (builder, nautilus_preferences,
                            NAUTILUS_PREFERENCES_DIALOG_COUNT_ROW,
                            NAUTILUS_PREFERENCES_SHOW_DIRECTORY_ITEM_COUNTS,
                            (const char **) speed_tradeoff_values);
}

void
nautilus_preferences_dialog_show (GtkWidget *parent)
{
    nautilus_preferences_dialog_show_page (parent, NULL);
}

/* Tabs at the top, text only, like the file manager's tabs, instead of
 * the dialog's own switcher (with icons, in the header when wide and at
 * the bottom when narrow). The dialog doesn't offer that, so its own
 * switchers are hidden and a strip of tabs goes above its pages. */
static GtkWidget *
find_widget (GtkWidget *root,
             GType      type)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE (root, type))
    {
        return root;
    }
    for (GtkWidget *child = gtk_widget_get_first_child (root); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        GtkWidget *found = find_widget (child, type);

        if (found != NULL)
        {
            return found;
        }
    }
    return NULL;
}

static void
on_tab_toggled (GtkToggleButton *button,
                AdwViewStack    *stack)
{
    if (gtk_toggle_button_get_active (button))
    {
        adw_view_stack_set_visible_child_name (stack, g_object_get_data (G_OBJECT (button), "page"));
    }
}

static void
on_visible_page (AdwViewStack *stack,
                 GParamSpec   *pspec,
                 GtkWidget    *tabs)
{
    const char *name = adw_view_stack_get_visible_child_name (stack);

    for (GtkWidget *tab = gtk_widget_get_first_child (tabs); tab != NULL; tab = gtk_widget_get_next_sibling (tab))
    {
        if (g_strcmp0 (g_object_get_data (G_OBJECT (tab), "page"), name) == 0)
        {
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tab), TRUE);
        }
    }
}

static void
add_text_tabs (AdwPreferencesDialog *dialog)
{
    GtkWidget *root = adw_dialog_get_child (ADW_DIALOG (dialog));
    GtkWidget *stack, *switcher, *bar;
    GtkWidget *toolbar, *tabs, *first = NULL;
    g_autoptr (GListModel) pages = NULL;

    if (root == NULL)
    {
        return;
    }
    stack = find_widget (root, ADW_TYPE_VIEW_STACK);
    switcher = find_widget (root, ADW_TYPE_VIEW_SWITCHER);
    bar = find_widget (root, ADW_TYPE_VIEW_SWITCHER_BAR);
    if (stack == NULL || switcher == NULL || bar == NULL)
    {
        return;
    }
    toolbar = gtk_widget_get_ancestor (stack, ADW_TYPE_TOOLBAR_VIEW);
    if (toolbar == NULL)
    {
        return;
    }

    gtk_widget_set_visible (switcher, FALSE);
    gtk_widget_set_visible (bar, FALSE);

    tabs = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class (tabs, "preferences-tabs");
    gtk_widget_set_halign (tabs, GTK_ALIGN_CENTER);
    pages = G_LIST_MODEL (adw_view_stack_get_pages (ADW_VIEW_STACK (stack)));
    for (guint i = 0; i < g_list_model_get_n_items (pages); i++)
    {
        g_autoptr (AdwViewStackPage) page = g_list_model_get_item (pages, i);
        const char *name = adw_view_stack_page_get_name (page);
        GtkWidget *tab;

        if (name == NULL || !gtk_widget_get_visible (adw_view_stack_page_get_child (page)))
        {
            continue;
        }
        tab = gtk_toggle_button_new_with_label (adw_view_stack_page_get_title (page));
        gtk_widget_add_css_class (tab, "preferences-tab");
        gtk_widget_set_focus_on_click (tab, FALSE);
        g_object_set_data_full (G_OBJECT (tab), "page", g_strdup (name), g_free);
        if (first != NULL)
        {
            gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (tab), GTK_TOGGLE_BUTTON (first));
        }
        else
        {
            first = tab;
        }
        g_signal_connect_object (tab, "toggled", G_CALLBACK (on_tab_toggled), stack, 0);
        gtk_box_append (GTK_BOX (tabs), tab);
    }
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), tabs);
    g_signal_connect_object (stack, "notify::visible-child-name", G_CALLBACK (on_visible_page), tabs, 0);
    on_visible_page (ADW_VIEW_STACK (stack), NULL, tabs);
}

void
nautilus_preferences_dialog_show_page (GtkWidget  *parent,
                                       const char *page)
{
    static AdwPreferencesDialog *preferences_dialog = NULL;
    g_autoptr (GtkBuilder) builder = NULL;
    g_autoptr (GSimpleActionGroup) action_group = g_simple_action_group_new ();
    g_autoptr (GAction) date_time_action = NULL;

    if (preferences_dialog != NULL)
    {
        /* Destroy existing window, which might be hidden behind other windows,
         * attached to another parent. */
        adw_dialog_force_close (ADW_DIALOG (preferences_dialog));
        g_clear_weak_pointer (&preferences_dialog);
    }

    builder = gtk_builder_new_from_resource ("/org/gnome/nautilus/ui/nautilus-preferences-dialog.ui");
    nautilus_preferences_dialog_setup (builder);

    preferences_dialog = ADW_PREFERENCES_DIALOG (gtk_builder_get_object (builder, "preferences_dialog"));
    g_object_add_weak_pointer (G_OBJECT (preferences_dialog), (gpointer *) &preferences_dialog);

    date_time_action = g_settings_create_action (nautilus_preferences,
                                                 NAUTILUS_PREFERENCES_DATE_TIME_FORMAT);
    g_action_map_add_action (G_ACTION_MAP (action_group), date_time_action);
    gtk_widget_insert_action_group (GTK_WIDGET (preferences_dialog),
                                    "preferences",
                                    G_ACTION_GROUP (action_group));

    {
        GtkWidget *shortcuts = nautilus_shortcuts_preferences_page_new ();

        nautilus_labels_add_preferences_pages (preferences_dialog,
                                               ADW_PREFERENCES_PAGE (gtk_builder_get_object (builder, "general_page")),
                                               ADW_PREFERENCES_PAGE (shortcuts));
        adw_preferences_dialog_add (preferences_dialog, ADW_PREFERENCES_PAGE (shortcuts));
    }

    add_text_tabs (preferences_dialog);

    if (page != NULL)
    {
        adw_preferences_dialog_set_visible_page_name (preferences_dialog, page);
    }
    adw_dialog_present (ADW_DIALOG (preferences_dialog), parent);
}
