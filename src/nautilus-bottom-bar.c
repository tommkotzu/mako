/*
 * nautilus-bottom-bar.c: the bar under each tab's files
 *
 * Every tab has its own: the view buttons and the filter belong to the
 * folder shown there, not to the window. The filter narrows the folder as
 * you type (the slot's GtkFilter, which the view's model already follows);
 * search swaps the filter for the slot's query editor.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "nautilus-bottom-bar"

#include "nautilus-bottom-bar.h"
#include "nautilus-shortcuts.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "nautilus-columns.h"
#include "nautilus-file.h"
#include "nautilus-labels.h"
#include "nautilus-query-editor.h"
#include "nautilus-toolbar-menu-sections.h"
#include "nautilus-view-item.h"
#include "nautilus-window-slot.h"

struct _NautilusBottomBar
{
    GtkWidget parent_instance;

    NautilusWindowSlot *slot;   /* weak */
    NautilusColumns *columns;   /* weak */

    GtkWidget *box;
    GtkWidget *views_button;
    GtkWidget *sort_button;
    GMenu *sort_menu;
    GtkWidget *stack;
    GtkWidget *filter_entry;
    GtkWidget *search_box;
    GtkWidget *flat_button;

    GtkCustomFilter *filter;
    GStrv words;                /* casefolded, of the filter text */
    gboolean syncing;
};

G_DEFINE_TYPE (NautilusBottomBar, nautilus_bottom_bar, GTK_TYPE_WIDGET)

gboolean
nautilus_bottom_bar_enabled (void)
{
    return g_settings_get_boolean (nautilus_flier_settings (), "bottom-bar");
}

/* ---------------------------------------------------------- Filter */

static gboolean
matches (gpointer item,
         gpointer user_data)
{
    NautilusBottomBar *self = user_data;
    NautilusFile *file;
    g_autofree char *name = NULL;

    if (self->words == NULL)
    {
        return TRUE;
    }

    file = nautilus_view_item_get_file (item);
    name = g_utf8_casefold (nautilus_file_get_display_name (file), -1);
    for (guint i = 0; self->words[i] != NULL; i++)
    {
        if (strstr (name, self->words[i]) == NULL)
        {
            return FALSE;
        }
    }

    return TRUE;
}

static void
on_filter_changed (NautilusBottomBar *self)
{
    const char *text = gtk_editable_get_text (GTK_EDITABLE (self->filter_entry));
    g_autofree char *folded = g_utf8_casefold (text, -1);
    g_auto (GStrv) old = g_steal_pointer (&self->words);
    GtkFilterChange change = GTK_FILTER_CHANGE_DIFFERENT;
    g_autofree char *old_text = old != NULL ? g_strjoinv (" ", old) : g_strdup ("");
    g_auto (GStrv) parts = g_strsplit_set (folded, " \t", -1);
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();

    if (self->slot == NULL)
    {
        return;
    }

    for (guint i = 0; parts[i] != NULL; i++)
    {
        if (*parts[i] != '\0')
        {
            g_strv_builder_add (builder, parts[i]);
        }
    }
    self->words = g_strv_builder_end (builder);
    if (self->words[0] == NULL)
    {
        g_clear_pointer (&self->words, g_strfreev);
        nautilus_window_slot_set_filter (self->slot, NULL);
        return;
    }

    /* Typing on narrows what's shown, deleting widens it: GTK only looks at
     * the items that can change. */
    {
        g_autofree char *new_text = g_strjoinv (" ", self->words);

        if (old != NULL && g_str_has_prefix (new_text, old_text) && g_strv_length (old) == g_strv_length (self->words))
        {
            change = GTK_FILTER_CHANGE_MORE_STRICT;
        }
        else if (old != NULL && g_str_has_prefix (old_text, new_text) && g_strv_length (old) == g_strv_length (self->words))
        {
            change = GTK_FILTER_CHANGE_LESS_STRICT;
        }
    }

    if (nautilus_window_slot_get_filter (self->slot) != GTK_FILTER (self->filter))
    {
        nautilus_window_slot_set_filter (self->slot, GTK_FILTER (self->filter));
    }
    else
    {
        gtk_filter_changed (GTK_FILTER (self->filter), change);
    }
}

static void
clear_filter (NautilusBottomBar *self)
{
    if (*gtk_editable_get_text (GTK_EDITABLE (self->filter_entry)) != '\0')
    {
        gtk_editable_set_text (GTK_EDITABLE (self->filter_entry), "");
    }
}

static void
focus_files (NautilusBottomBar *self)
{
    NautilusFilesView *view = self->slot != NULL ? nautilus_window_slot_get_current_view (self->slot) : NULL;

    if (view != NULL)
    {
        gtk_widget_grab_focus (GTK_WIDGET (view));
    }
}

/* Esc clears, then gives the keys back; Down and Enter go to the files. */
static gboolean
on_filter_key (GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode,
               GdkModifierType        state,
               NautilusBottomBar     *self)
{
    if (keyval == GDK_KEY_Escape)
    {
        if (*gtk_editable_get_text (GTK_EDITABLE (self->filter_entry)) != '\0')
        {
            clear_filter (self);
        }
        focus_files (self);
        return GDK_EVENT_STOP;
    }

    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    {
        focus_files (self);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

gboolean
nautilus_bottom_bar_handle_event (NautilusBottomBar     *self,
                                  GtkEventControllerKey *controller,
                                  guint                  keyval,
                                  GdkModifierType        state)
{
    gunichar c = gdk_keyval_to_unicode (keyval);

    if (!gtk_widget_get_visible (GTK_WIDGET (self)) ||
        (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0)
    {
        return FALSE;
    }

    if (keyval == GDK_KEY_Escape &&
        *gtk_editable_get_text (GTK_EDITABLE (self->filter_entry)) != '\0')
    {
        clear_filter (self);
        return TRUE;
    }

    /* Printable characters only; Space stays Quick Look. */
    if (c == 0 || !g_unichar_isgraph (c))
    {
        return FALSE;
    }

    gtk_widget_grab_focus (self->filter_entry);
    return gtk_event_controller_key_forward (controller, self->filter_entry);
}

/* ---------------------------------------------------------- Views */

static const char *const mode_icons[] =
{
    "view-list-symbolic", "view-list-bullet-symbolic", "view-columns-symbolic", "view-grid-symbolic",
};

/* The views button shows the tab's view. */
static void
sync_mode (NautilusBottomBar *self)
{
    guint mode = self->columns != NULL ? nautilus_columns_get_mode (self->columns) : NAUTILUS_COLUMNS_MODE_LIST;

    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (self->views_button), mode_icons[mode - 1]);
}

static void append_item (GMenu *menu, const char *label, const char *action);

/* The view's own sort and group items, which change with the view. */
static void
sync_sort_menu (NautilusBottomBar *self)
{
    NautilusToolbarMenuSections *sections = self->slot != NULL ?
                                            nautilus_window_slot_get_toolbar_menu_sections (self->slot) : NULL;
    g_autoptr (GMenu) hidden = g_menu_new ();
    g_autoptr (GMenu) groups = g_menu_new ();

    g_menu_remove_all (self->sort_menu);
    gtk_widget_set_sensitive (self->sort_button, sections != NULL);
    if (sections == NULL)
    {
        return;
    }

    g_menu_append_section (self->sort_menu, _("Sort"), sections->sort_section);
    append_item (groups, _("_Next Grouping"), "view.group-by-next");
    append_item (groups, _("No _Groups"), "view.group-by('none')");
    g_menu_append_section (self->sort_menu, _("Group"), G_MENU_MODEL (groups));
    append_item (hidden, _("Show _Hidden Files"), "view.show-hidden-files");
    append_item (hidden, _("Fla_t View"), "win.flat-view");
    g_menu_append_section (self->sort_menu, NULL, G_MENU_MODEL (hidden));
}

static void
sync_flat (NautilusBottomBar *self)
{
    self->syncing = TRUE;
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->flat_button),
                                  self->columns != NULL && nautilus_columns_get_flat (self->columns));
    self->syncing = FALSE;
}

static void
on_flat_toggled (GtkToggleButton   *button,
                 NautilusBottomBar *self)
{
    if (!self->syncing && self->columns != NULL)
    {
        nautilus_columns_set_flat (self->columns, gtk_toggle_button_get_active (button));
    }
}

/* ---------------------------------------------------------- Search */

static void
sync_search (NautilusBottomBar *self)
{
    GtkWidget *editor;
    GtkWidget *parent;
    gboolean searching;

    if (self->slot == NULL || !nautilus_bottom_bar_enabled ())
    {
        return;
    }

    searching = nautilus_window_slot_get_search_visible (self->slot);
    editor = GTK_WIDGET (nautilus_window_slot_get_query_editor (self->slot));
    parent = gtk_widget_get_parent (editor);

    /* Kept here, hidden while not searching, as long as the bar is on (the
     * header bar holds it while the bar is off). A query editor without a
     * parent gets realized by the search starting, and GTK then aborts
     * when it's added somewhere. */
    if (parent != self->search_box)
    {
        if (parent != NULL)
        {
            gtk_box_remove (GTK_BOX (parent), editor);
        }
        gtk_widget_set_hexpand (editor, TRUE);
        gtk_box_append (GTK_BOX (self->search_box), editor);
        /* Moving it lost the focus the slot gave it. */
        if (searching)
        {
            gtk_widget_grab_focus (editor);
        }
    }

    gtk_stack_set_visible_child (GTK_STACK (self->stack), searching ? self->search_box : self->filter_entry);
    if (searching)
    {
        clear_filter (self);
    }
}

static void
on_location (NautilusBottomBar *self)
{
    /* A filter is for the folder it was typed in. */
    clear_filter (self);
}

static void
on_setting (NautilusBottomBar *self)
{
    gboolean on = nautilus_bottom_bar_enabled ();

    gtk_widget_set_visible (GTK_WIDGET (self), on);
    if (!on)
    {
        clear_filter (self);
    }
    sync_search (self);
}

/* ---------------------------------------------------------- Building */

static GtkWidget *
icon_toggle (const char *icon_name,
             const char *tooltip)
{
    GtkWidget *button = gtk_toggle_button_new ();

    gtk_button_set_icon_name (GTK_BUTTON (button), icon_name);
    gtk_widget_set_tooltip_text (button, tooltip);
    gtk_widget_set_focus_on_click (button, FALSE);
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);

    return button;
}

static gboolean
keys_in_menus (void)
{
    return g_settings_get_boolean (nautilus_flier_settings (), "menu-shortcuts");
}

/* An item, with its shortcut next to it while "menu-shortcuts" is on. */
static void
append_item (GMenu      *menu,
             const char *label,
             const char *action)
{
    g_autoptr (GMenuItem) item = g_menu_item_new (label, action);
    g_autofree char *accel = keys_in_menus () ? nautilus_shortcuts_get_accel (action) : NULL;

    if (accel != NULL)
    {
        g_menu_item_set_attribute (item, "accel", "s", accel);
    }
    g_menu_append_item (menu, item);
}

/* The views of the tab, then what goes with them. */
static GtkWidget *
build_views_popover (void)
{
    g_autoptr (GMenu) menu = g_menu_new ();
    g_autoptr (GMenu) views = g_menu_new ();
    g_autoptr (GMenu) zoom = g_menu_new ();
    g_autoptr (GMenu) density = g_menu_new ();
    g_autoptr (GMenu) more = g_menu_new ();
    g_autoptr (GMenu) panels = g_menu_new ();
    g_autoptr (GMenuItem) item = NULL;
    GtkWidget *popover;
    GtkWidget *zoom_button;

    append_item (views, _("_List"), "win.view-mode(uint32 1)");
    append_item (views, _("_Tree"), "win.view-mode(uint32 2)");
    append_item (views, _("_Columns"), "win.view-mode(uint32 3)");
    append_item (views, _("_Grid"), "win.view-mode(uint32 4)");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (views));

    item = g_menu_item_new (NULL, NULL);
    g_menu_item_set_attribute (item, "custom", "s", "zoom-out");
    g_menu_append_item (zoom, item);
    g_clear_object (&item);
    item = g_menu_item_new (NULL, NULL);
    g_menu_item_set_attribute (item, "custom", "s", "zoom-in");
    g_menu_append_item (zoom, item);
    g_clear_object (&item);
    item = g_menu_item_new_section (_("Icon Size"), G_MENU_MODEL (zoom));
    g_menu_item_set_attribute (item, "display-hint", "s", "inline-buttons");
    g_menu_append_item (menu, item);
    g_clear_object (&item);

    /* Same setting as Preferences → Organize → Density, for all windows. */
    g_menu_append (density, _("Co_mfortable"), "bar.density('comfortable')");
    g_menu_append (density, _("Com_pact"), "bar.density('compact')");
    g_menu_append (density, _("_Dense"), "bar.density('dense')");
    g_menu_append (density, _("_Extra Dense"), "bar.density('extra-dense')");
    g_menu_append_section (menu, _("Density"), G_MENU_MODEL (density));

    append_item (more, _("_Flat View"), "win.flat-view");
    g_menu_append (more, _("_Visible Columns…"), "view.visible-columns");
    g_menu_append (more, _("Captions…"), "view.visible-captions");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (more));

    append_item (panels, _("_Inspector"), "win.inspector-visible");
    append_item (panels, _("_Bottom Bar"), "win.bottom-bar");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (panels));

    popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    zoom_button = gtk_button_new_from_icon_name ("zoom-out-symbolic");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (zoom_button), "view.zoom-out");
    gtk_widget_set_tooltip_text (zoom_button, _("Zoom Out"));
    gtk_widget_add_css_class (zoom_button, "flat");
    gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover), zoom_button, "zoom-out");
    zoom_button = gtk_button_new_from_icon_name ("zoom-in-symbolic");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (zoom_button), "view.zoom-in");
    gtk_widget_set_tooltip_text (zoom_button, _("Zoom In"));
    gtk_widget_add_css_class (zoom_button, "flat");
    gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover), zoom_button, "zoom-in");

    return popover;
}

static GtkWidget *
build_views_button (void)
{
    GtkWidget *button = gtk_menu_button_new ();

    gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), build_views_popover ());
    gtk_widget_set_tooltip_text (button, _("View (Alt+1 to 4)"));
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);

    return button;
}

static void sync_sort_menu (NautilusBottomBar *self);

static void
on_menu_shortcuts (NautilusBottomBar *self)
{
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->views_button), build_views_popover ());
    sync_sort_menu (self);
}

static void
nautilus_bottom_bar_init (NautilusBottomBar *self)
{
    GtkWidget *linked = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkEventController *keys;

    gtk_widget_add_css_class (GTK_WIDGET (self), "bottom-bar");

    self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (self->box, "toolbar");
    gtk_widget_set_parent (self->box, GTK_WIDGET (self));

    self->filter_entry = gtk_search_entry_new ();
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (self->filter_entry), _("Filter"));
    gtk_search_entry_set_search_delay (GTK_SEARCH_ENTRY (self->filter_entry), 0);
    gtk_widget_set_hexpand (self->filter_entry, TRUE);
    g_signal_connect_swapped (self->filter_entry, "changed", G_CALLBACK (on_filter_changed), self);
    keys = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_filter_key), self);
    gtk_widget_add_controller (self->filter_entry, keys);

    self->search_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (self->search_box, TRUE);

    self->stack = gtk_stack_new ();
    gtk_stack_set_hhomogeneous (GTK_STACK (self->stack), FALSE);
    gtk_widget_set_hexpand (self->stack, TRUE);
    gtk_widget_set_valign (self->stack, GTK_ALIGN_CENTER);
    gtk_stack_add_child (GTK_STACK (self->stack), self->filter_entry);
    gtk_stack_add_child (GTK_STACK (self->stack), self->search_box);

    /* Sorting, next to the filter or search it applies to. */
    self->sort_menu = g_menu_new ();
    self->sort_button = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (self->sort_button), "view-sort-descending-symbolic");
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->sort_button), G_MENU_MODEL (self->sort_menu));
    gtk_widget_set_tooltip_text (self->sort_button, _("Sort and Group"));
    gtk_widget_set_valign (self->sort_button, GTK_ALIGN_CENTER);

    gtk_widget_add_css_class (linked, "linked");
    gtk_widget_set_hexpand (linked, TRUE);
    gtk_widget_set_valign (linked, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (linked), self->stack);
    gtk_box_append (GTK_BOX (self->box), linked);
    /* A plain icon like the buttons after it, not a pill joined to the entry. */
    gtk_widget_add_css_class (self->sort_button, "flat");
    gtk_box_append (GTK_BOX (self->box), self->sort_button);

    self->flat_button = icon_toggle ("view-paged-symbolic", _("Flat View: All Files in Subfolders"));
    g_object_set_data (G_OBJECT (self->flat_button), "flier-tooltip-action", (gpointer) "win.flat-view");
    g_signal_connect (self->flat_button, "toggled", G_CALLBACK (on_flat_toggled), self);
    gtk_box_append (GTK_BOX (self->box), self->flat_button);

    self->views_button = build_views_button ();
    gtk_box_append (GTK_BOX (self->box), self->views_button);

    self->filter = gtk_custom_filter_new (matches, self, NULL);

    {
        g_autoptr (GSimpleActionGroup) actions = g_simple_action_group_new ();
        g_autoptr (GAction) density = g_settings_create_action (nautilus_flier_settings (), "density");

        g_action_map_add_action (G_ACTION_MAP (actions), density);
        gtk_widget_insert_action_group (GTK_WIDGET (self), "bar", G_ACTION_GROUP (actions));
    }

    g_signal_connect_object (nautilus_flier_settings (), "changed::menu-shortcuts",
                             G_CALLBACK (on_menu_shortcuts), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::custom-shortcuts",
                             G_CALLBACK (on_menu_shortcuts), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::bottom-bar",
                             G_CALLBACK (on_setting), self, G_CONNECT_SWAPPED);
    gtk_widget_set_visible (GTK_WIDGET (self), nautilus_bottom_bar_enabled ());
}

static void
nautilus_bottom_bar_dispose (GObject *object)
{
    NautilusBottomBar *self = NAUTILUS_BOTTOM_BAR (object);

    g_clear_object (&self->sort_menu);
    if (self->filter != NULL)
    {
        /* Nothing to filter with once we're gone. */
        if (self->slot != NULL && nautilus_window_slot_get_filter (self->slot) == GTK_FILTER (self->filter))
        {
            nautilus_window_slot_set_filter (self->slot, NULL);
        }
        g_clear_object (&self->filter);
    }
    g_clear_pointer (&self->words, g_strfreev);
    g_clear_weak_pointer (&self->slot);
    g_clear_weak_pointer (&self->columns);
    g_clear_pointer (&self->box, gtk_widget_unparent);

    G_OBJECT_CLASS (nautilus_bottom_bar_parent_class)->dispose (object);
}

static void
nautilus_bottom_bar_class_init (NautilusBottomBarClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_bottom_bar_dispose;
    gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass), GTK_TYPE_BIN_LAYOUT);
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "nautilus-bottom-bar");
}

GtkWidget *
nautilus_bottom_bar_new (NautilusWindowSlot *slot)
{
    NautilusBottomBar *self = g_object_new (NAUTILUS_TYPE_BOTTOM_BAR, NULL);

    g_set_weak_pointer (&self->slot, slot);
    g_set_weak_pointer (&self->columns, NAUTILUS_COLUMNS (nautilus_window_slot_get_columns (slot)));

    g_signal_connect_object (slot, "notify::toolbar-menu-sections", G_CALLBACK (sync_sort_menu), self,
                             G_CONNECT_SWAPPED);

    if (self->columns != NULL)
    {
        g_signal_connect_object (self->columns, "notify::mode", G_CALLBACK (sync_mode), self,
                                 G_CONNECT_SWAPPED);
        g_signal_connect_object (self->columns, "notify::flat", G_CALLBACK (sync_flat), self,
                                 G_CONNECT_SWAPPED);
    }
    g_signal_connect_object (slot, "notify::search-visible", G_CALLBACK (sync_search), self,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (slot, "notify::location", G_CALLBACK (on_location), self,
                             G_CONNECT_SWAPPED);

    sync_mode (self);
    sync_flat (self);
    sync_sort_menu (self);
    sync_search (self);
    nautilus_shortcuts_add_tooltips (GTK_WIDGET (self));

    return GTK_WIDGET (self);
}

void
nautilus_bottom_bar_set_filter_text (NautilusBottomBar *self,
                                     const char        *text)
{
    gtk_editable_set_text (GTK_EDITABLE (self->filter_entry), text);
}
