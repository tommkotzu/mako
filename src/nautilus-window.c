/*
 *  Nautilus
 *
 *  Copyright (C) 1999, 2000, 2004 Red Hat, Inc.
 *  Copyright (C) 1999, 2000, 2001 Eazel, Inc.
 *
 *  Nautilus is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  Nautilus is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *  Authors: Elliot Lee <sopwith@redhat.com>
 *           John Sullivan <sullivan@eazel.com>
 *           Alexander Larsson <alexl@redhat.com>
 */

/* nautilus-window.c: Implementation of the main window object */
#define G_LOG_DOMAIN "nautilus-window"

#include <config.h>

#include "nautilus-window.h"

#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <math.h>
#include <sys/time.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include "nautilus-application.h"
#include "nautilus-bookmark.h"
#include "nautilus-bookmark-list.h"
#include "nautilus-clipboard.h"
#include "nautilus-dnd.h"
#include "nautilus-enums.h"
#include "nautilus-file.h"
#include "nautilus-file-operations.h"
#include "nautilus-file-undo-manager.h"
#include "nautilus-file-undo-operations.h"
#include "nautilus-file-utilities.h"
#include "nautilus-global-preferences.h"
#include "nautilus-metadata.h"
#include "nautilus-network-address-bar.h"
#include "nautilus-mime-actions.h"
#include "nautilus-module.h"
#include "nautilus-progress-indicator.h"
#include "nautilus-scheme.h"
#include "nautilus-shortcut-manager.h"
#include "nautilus-sidebar.h"
#include "nautilus-tile-layout.h"
#include "nautilus-signaller.h"
#include "nautilus-toolbar.h"
#include "nautilus-bottom-bar.h"
#include "nautilus-files-view.h"
#include "nautilus-inspector.h"
#include "nautilus-columns.h"
#include "nautilus-labels.h"
#include "nautilus-quick-look.h"
#include "nautilus-shortcuts.h"
#include "nautilus-trash-monitor.h"
#include "nautilus-ui-utilities.h"
#include "nautilus-window-slot.h"

static void nautilus_window_initialize_actions (NautilusWindow *window);
static void nautilus_window_back_or_forward (NautilusWindow *window,
                                             gboolean        back,
                                             guint           distance);
static void nautilus_window_sync_location_widgets (NautilusWindow *window);
static void update_cursor (NautilusWindow *window);
static void
set_active_slot (NautilusWindow     *window,
                 NautilusWindowSlot *new_slot);
static void set_active_tile (NautilusWindow *window,
                             NautilusTile   *tile);
static void schedule_tile_collapse (NautilusWindow *window,
                                    NautilusTile   *tile);
static AdwTabView *take_pending_tab_drop_view (void);
static NautilusTile *create_tile (NautilusWindow *window);
static AdwTabPage *dropped_page;

/* Sanity check: highest mouse button value I could find was 14. 5 is our
 * lower threshold (well-documented to be the one of the button events for the
 * scrollwheel), so it's hardcoded in the functions below. However, if you have
 * a button that registers higher and want to map it, file a bug and
 * we'll move the bar. Makes you wonder why the X guys don't have
 * defined values for these like the XKB stuff, huh?
 */

struct _NautilusWindow
{
    AdwApplicationWindow parent_instance;

    GMenuModel *undo_redo_section;

    /* The active tile's tab view and bar. Every tile has its own. */
    AdwTabView *tab_view;
    AdwTabBar *tab_bar;
    AdwTabPage *menu_page;
    NautilusTileLayout *tile_layout;
    GtkPaned *inspector_paned;
    GtkWidget *inspector;
    GtkWidget *mode_columns;    /* weak; the active slot's, for win.view-mode */
    gboolean inspector_placed;
    guint inspector_save_id;
    GtkWidget *inspector_overlay;  /* over the files, for the drop highlight */
    GtkWidget *inspector_drop;
    GtkWidget *inspector_drop_label;
    GtkWidget *inspector_window;   /* the popped-out inspector, or NULL */
    int inspector_docked_side;     /* where it goes back to */
    double inspector_drag_x, inspector_drag_y;
    int inspector_drag_target;     /* InspectorSide, or -1 */
    GMenuModel *tab_menu_model;
    gboolean tab_bar_inverted;

    GList *slots;
    NautilusWindowSlot *active_slot; /* weak reference */

    GtkWidget *split_view;

    /* Side Pane */
    NautilusSidebar *places_sidebar;
    GVolume *selected_volume;     /* the selected volume in the sidebar popup callback */
    GFile *selected_file;     /* the selected file in the sidebar popup callback */

    /* Notifications */
    AdwToastOverlay *toast_overlay;
    AdwToast *last_undo_toast;

    /* Toolbar */
    GtkWidget *toolbar;
    gboolean temporary_navigation_bar;

    GtkWidget *network_address_bar;

    guint sidebar_width_handler_id;

    GQueue *tab_data_queue;

    /* Pad controller which holds a reference to the window. Kept around to
     * break reference-counting cycles during finalization. */
    GtkPadController *pad_controller;
};

enum
{
    LOCATIONS_CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (NautilusWindow, nautilus_window, ADW_TYPE_APPLICATION_WINDOW);

enum
{
    PROP_0,
    PROP_ACTIVE_SLOT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static const GtkPadActionEntry pad_actions[] =
{
    { GTK_PAD_ACTION_BUTTON, 0, -1, N_("Home"), "go-home" },
    { GTK_PAD_ACTION_BUTTON, 1, -1, N_("New tab"), "new-tab" },
    { GTK_PAD_ACTION_BUTTON, 2, -1, N_("Close current view"), "close-current-view" },
    /* Button number sequence continues in window-slot.c */
};

#ifdef HAVE_MALLOC_TRIM
static guint malloc_trim_idle_id = 0;

static gboolean
malloc_trim_idle_cb (gpointer user_data)
{
    malloc_trim (8 * 1024 * 1024);
    malloc_trim_idle_id = 0;

    return FALSE;
}
#endif

static AdwTabPage *
get_current_page (NautilusWindow *window)
{
    /* The tab right-clicked, while its menu is up. With tiles it can be in
     * another tile's view, which doesn't take it as a parent for new tabs. */
    if (window->menu_page != NULL)
    {
        for (int i = adw_tab_view_get_n_pages (window->tab_view) - 1; i >= 0; i--)
        {
            if (adw_tab_view_get_nth_page (window->tab_view, i) == window->menu_page)
            {
                return window->menu_page;
            }
        }
    }

    return adw_tab_view_get_selected_page (window->tab_view);
}

static void
action_close_current_view (GSimpleAction *action,
                           GVariant      *state,
                           gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);

    if (window->slots == NULL || window->slots->next == NULL)
    {
        nautilus_window_close (window);
        return;
    }

    adw_tab_view_close_page (window->tab_view, page);
}

static void
action_close_other_tabs (GSimpleAction *action,
                         GVariant      *parameters,
                         gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);

    adw_tab_view_close_other_pages (window->tab_view, page);
}

static void
action_go_home (GSimpleAction *action,
                GVariant      *state,
                gpointer       user_data)
{
    NautilusWindow *window;
    GFile *home;

    window = NAUTILUS_WINDOW (user_data);
    home = g_file_new_for_path (g_get_home_dir ());

    nautilus_window_open_location_full (window, home, 0, NULL);

    g_object_unref (home);
}

static void
action_new_tab (GSimpleAction *action,
                GVariant      *state,
                gpointer       user_data)
{
    nautilus_window_new_tab (NAUTILUS_WINDOW (user_data));
}

static void
action_tab_move_left (GSimpleAction *action,
                      GVariant      *state,
                      gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);

    adw_tab_view_reorder_backward (window->tab_view, page);
}

static void
action_tab_move_right (GSimpleAction *action,
                       GVariant      *state,
                       gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);

    adw_tab_view_reorder_forward (window->tab_view, page);
}

static void
action_go_to_tab (GSimpleAction *action,
                  GVariant      *value,
                  gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);
    gint16 num;

    num = g_variant_get_int32 (value);
    if (num < adw_tab_view_get_n_pages (window->tab_view))
    {
        AdwTabPage *page = adw_tab_view_get_nth_page (window->tab_view, num);

        adw_tab_view_set_selected_page (window->tab_view, page);
    }
}

static void
action_redo (GSimpleAction *action,
             GVariant      *state,
             gpointer       user_data)
{
    NautilusWindow *window = user_data;

    nautilus_file_undo_manager_redo (GTK_WINDOW (window), NULL);
}

static void
action_undo (GSimpleAction *action,
             GVariant      *state,
             gpointer       user_data)
{
    NautilusWindow *window = user_data;

    nautilus_file_undo_manager_undo (GTK_WINDOW (window), NULL);
}

static void
action_show_current_location_menu (GSimpleAction *action,
                                   GVariant      *state,
                                   gpointer       user_data)
{
    NautilusWindow *self = NAUTILUS_WINDOW (user_data);

    nautilus_toolbar_show_current_location_menu (NAUTILUS_TOOLBAR (self->toolbar));
}

static void
on_location_changed (NautilusWindow *window)
{
    nautilus_window_sync_location_widgets (window);
}

static void
on_slot_location_changed (NautilusWindowSlot *slot,
                          GParamSpec         *pspec,
                          NautilusWindow     *window)
{
    if (window->active_slot == slot)
    {
        on_location_changed (window);
    }

    g_signal_emit (window, signals[LOCATIONS_CHANGED], 0);
}

static void
tab_view_setup_menu_cb (AdwTabView     *tab_view,
                        AdwTabPage     *page,
                        NautilusWindow *window)
{
    GAction *move_tab_left_action;
    GAction *move_tab_right_action;
    GAction *restore_tab_action;
    int position, n_pages;
    gboolean menu_is_closed = (page == NULL);

    /* Weak: the tile and its tabs can go before the menu says it closed. */
    g_set_weak_pointer (&window->menu_page, page);

    if (!menu_is_closed)
    {
        position = adw_tab_view_get_page_position (tab_view, page);
        n_pages = adw_tab_view_get_n_pages (tab_view);
    }

    move_tab_left_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                       "tab-move-left");
    move_tab_right_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                        "tab-move-right");
    restore_tab_action = g_action_map_lookup_action (G_ACTION_MAP (window),
                                                     "restore-tab");

    /* Re-enable all of the actions if the menu is closed */
    g_simple_action_set_enabled (G_SIMPLE_ACTION (move_tab_left_action),
                                 menu_is_closed || position > 0);
    g_simple_action_set_enabled (G_SIMPLE_ACTION (move_tab_right_action),
                                 menu_is_closed || position < n_pages - 1);
    g_simple_action_set_enabled (G_SIMPLE_ACTION (restore_tab_action),
                                 menu_is_closed || g_queue_get_length (window->tab_data_queue) > 0);
}

static void
tab_view_notify_selected_page_cb (AdwTabView     *tab_view,
                                  GParamSpec     *pspec,
                                  NautilusWindow *window)
{
    AdwTabPage *page = adw_tab_view_get_selected_page (tab_view);

    /* Other tiles keep their own selection; an empty tile has none. */
    if (tab_view != window->tab_view || page == NULL)
    {
        return;
    }

    NautilusWindowSlot *slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));

    g_return_if_fail (slot != NULL);

    set_active_slot (window, slot);
}

static void
disconnect_slot (NautilusWindow     *window,
                 NautilusWindowSlot *slot)
{
    g_signal_handlers_disconnect_by_data (slot, window);
}

static void
on_update_page_tooltip (NautilusWindowSlot *slot,
                        GParamSpec         *pspec,
                        AdwTabPage         *page)
{
    GFile *location = nautilus_window_slot_get_location (slot);
    g_autofree char *escaped_name = NULL;

    if (location == NULL)
    {
        return;
    }

    /* Set the tooltip on the label's parent (the tab label hbox),
     * so it covers all of the tab label.
     */

    if (g_file_has_uri_scheme (location, SCHEME_SEARCH))
    {
        escaped_name = g_markup_escape_text (nautilus_window_slot_get_title (slot), -1);
    }
    else
    {
        g_autofree gchar *location_name = g_file_get_parse_name (location);
        escaped_name = g_markup_escape_text (location_name, -1);
    }

    adw_tab_page_set_tooltip (page, escaped_name);
}

static NautilusWindowSlot *
nautilus_window_create_and_init_slot (NautilusWindow *window)
{
    g_autoptr (NautilusWindowSlot) slot = nautilus_window_slot_new (NAUTILUS_MODE_BROWSE);

    g_signal_connect_swapped (slot, "notify::allow-stop",
                              G_CALLBACK (update_cursor), window);
    g_signal_connect (slot, "notify::location",
                      G_CALLBACK (on_slot_location_changed), window);

    AdwTabPage *current = get_current_page (window);
    AdwTabPage *page = adw_tab_view_add_page (window->tab_view, GTK_WIDGET (slot), current);

    g_object_bind_property (slot, "allow-stop",
                            page, "loading",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (slot, "title",
                            page, "title",
                            G_BINDING_SYNC_CREATE);
    g_signal_connect (slot, "notify::title", G_CALLBACK (on_update_page_tooltip), page);

    /* Assure that AdwTabView has added slot to slot list */
    g_warn_if_fail (g_list_find (window->slots, slot) != NULL);

    return slot;
}

static NautilusWindowSlot *
get_slot_with_open_location (NautilusWindow *self,
                             GFile          *location)
{
    for (GList *l = self->slots; l != NULL; l = l->next)
    {
        NautilusWindowSlot *slot = l->data;
        GFile *slot_location = nautilus_window_slot_get_location (slot);

        if (slot_location != NULL &&
            g_file_equal (location, slot_location))
        {
            return slot;
        }
    }

    return NULL;
}

void
nautilus_window_open_location_full (NautilusWindow    *window,
                                    GFile             *location,
                                    NautilusOpenFlags  flags,
                                    NautilusFileList  *selection)
{
    /* Assert that we are not managing new windows */
    g_warn_if_fail ((flags & NAUTILUS_OPEN_FLAG_NEW_WINDOW) == 0);

    NautilusWindowSlot *target_slot = (flags & NAUTILUS_OPEN_FLAG_REUSE_EXISTING) != 0
                                      ? get_slot_with_open_location (window, location)
                                      : window->active_slot;

    if (target_slot == NULL || (flags & NAUTILUS_OPEN_FLAG_NEW_TAB) != 0)
    {
        target_slot = nautilus_window_create_and_init_slot (window);
    }

    /* Make the opened location the one active if we weren't asked for the
     * opposite, since it's the most usual use case */
    if ((flags & NAUTILUS_OPEN_FLAG_DONT_MAKE_ACTIVE) == 0)
    {
        gtk_window_present (GTK_WINDOW (window));
        set_active_slot (window, target_slot);
    }

    nautilus_window_slot_open_location_full (target_slot, location, selection);
}

static gboolean
nautilus_window_grab_focus (GtkWidget *widget)
{
    NautilusWindow *self = NAUTILUS_WINDOW (widget);

    if (self->active_slot != NULL)
    {
        return gtk_widget_grab_focus (GTK_WIDGET (self->active_slot));
    }

    return GTK_WIDGET_CLASS (nautilus_window_parent_class)->grab_focus (widget);
}

static void
remove_slot_from_window (NautilusWindowSlot *slot,
                         NautilusWindow     *window)
{
    g_return_if_fail (NAUTILUS_IS_WINDOW_SLOT (slot));
    g_return_if_fail (NAUTILUS_WINDOW (window));

    g_debug ("Removing slot %p", slot);

    disconnect_slot (window, slot);
    window->slots = g_list_remove (window->slots, slot);
    g_signal_emit (window, signals[LOCATIONS_CHANGED], 0);
}

void
nautilus_window_new_tab (NautilusWindow *window)
{
    NautilusWindowSlot *current_slot;
    AdwTabPage *page;
    GFile *location;

    page = get_current_page (window);
    current_slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));
    location = nautilus_window_slot_get_location (current_slot);

    if (location != NULL)
    {
        if (g_file_has_uri_scheme (location, SCHEME_SEARCH))
        {
            location = g_file_new_for_path (g_get_home_dir ());
        }
        else
        {
            g_object_ref (location);
        }

        nautilus_window_open_location_full (window, location,
                                            NAUTILUS_OPEN_FLAG_NEW_TAB,
                                            NULL);
        g_object_unref (location);
    }
}

static void
update_cursor (NautilusWindow *window)
{
    if (!gtk_widget_get_realized (GTK_WIDGET (window)))
    {
        return;
    }

    if (window->active_slot != NULL &&
        nautilus_window_slot_get_allow_stop (window->active_slot))
    {
        gtk_widget_set_cursor_from_name (GTK_WIDGET (window), "progress");
    }
    else
    {
        gtk_widget_set_cursor (GTK_WIDGET (window), NULL);
    }
}

/* Callback used when the places sidebar needs to know the drag action to suggest */
static GdkDragAction
places_sidebar_drag_action_requested_cb (NautilusSidebar *sidebar,
                                         NautilusFile    *dest_file,
                                         GList           *source_file_list)
{
    return nautilus_dnd_get_preferred_action (dest_file, source_file_list->data);
}
#if 0 && NAUTILUS_DND_NEEDS_GTK4_REIMPLEMENTATION
/* Callback used when the places sidebar needs us to pop up a menu with possible drag actions */
static GdkDragAction
places_sidebar_drag_action_ask_cb (NautilusSidebar *sidebar,
                                   GdkDragAction    actions,
                                   gpointer         user_data)
{
    return nautilus_drag_drop_action_ask (GTK_WIDGET (sidebar), actions);
}
#endif
static GList *
build_uri_list_from_gfile_list (GSList *file_list)
{
    GList *result;
    GSList *l;

    result = NULL;

    for (l = file_list; l; l = l->next)
    {
        GFile *file = l->data;
        char *uri;

        uri = g_file_get_uri (file);
        result = g_list_prepend (result, uri);
    }

    return g_list_reverse (result);
}

/* Callback used when the places sidebar has URIs dropped into it.  We do a normal file operation for them. */
static void
places_sidebar_drag_perform_drop_cb (NautilusSidebar *sidebar,
                                     GFile           *dest_file,
                                     GSList          *source_file_list,
                                     GdkDragAction    action,
                                     gpointer         user_data)
{
    char *dest_uri;
    GList *source_uri_list;

    dest_uri = g_file_get_uri (dest_file);
    source_uri_list = build_uri_list_from_gfile_list (source_file_list);

    nautilus_file_operations_copy_move (source_uri_list, dest_uri, action, GTK_WIDGET (sidebar), NULL, NULL, NULL);

    g_free (dest_uri);
    g_list_free_full (source_uri_list, g_free);
}

static void
action_restore_tab (GSimpleAction *action,
                    GVariant      *state,
                    gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);
    NautilusWindowSlot *slot;
    NautilusNavigationState *data;

    if (g_queue_get_length (window->tab_data_queue) == 0)
    {
        return;
    }

    data = g_queue_pop_head (window->tab_data_queue);

    GFile *location = nautilus_bookmark_get_location (data->current_location_bookmark);

    slot = nautilus_window_create_and_init_slot (window);

    nautilus_window_slot_open_location_full (slot, location, NULL);
    nautilus_window_slot_restore_navigation_state (slot, data);

    free_navigation_state (data);
}

static void
action_toggle_sidebar (GSimpleAction *action,
                       GVariant      *state,
                       gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);
    gboolean revealed;

    revealed = adw_overlay_split_view_get_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (window->split_view));
    adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (window->split_view), !revealed);
}

static void
nautilus_window_set_up_sidebar (NautilusWindow *window)
{
    nautilus_sidebar_set_open_flags (window->places_sidebar,
                                     (NAUTILUS_OPEN_FLAG_NORMAL
                                      | NAUTILUS_OPEN_FLAG_NEW_TAB
                                      | NAUTILUS_OPEN_FLAG_NEW_WINDOW));

    g_signal_connect (window->places_sidebar, "drag-action-requested",
                      G_CALLBACK (places_sidebar_drag_action_requested_cb), window);
#if 0 && NAUTILUS_DND_NEEDS_GTK4_REIMPLEMENTATION
    g_signal_connect (window->places_sidebar, "drag-action-ask",
                      G_CALLBACK (places_sidebar_drag_action_ask_cb), window);
  #endif
    g_signal_connect (window->places_sidebar, "drag-perform-drop",
                      G_CALLBACK (places_sidebar_drag_perform_drop_cb), window);
}

static void
window_slot_close (NautilusWindow     *window,
                   NautilusWindowSlot *slot)
{
    NautilusNavigationState *data;
    AdwTabPage *page;

    g_debug ("Requesting to remove slot %p from window %p", slot, window);
    if (window == NULL || slot == NULL)
    {
        return;
    }

    data = nautilus_window_slot_get_navigation_state (slot);
    if (data != NULL)
    {
        g_queue_push_head (window->tab_data_queue, data);
    }

    remove_slot_from_window (slot, window);

    AdwTabView *view = ADW_TAB_VIEW (gtk_widget_get_ancestor (GTK_WIDGET (slot), ADW_TYPE_TAB_VIEW));

    page = adw_tab_view_get_page (view, GTK_WIDGET (slot));
    /* this will destroy the slot */
    adw_tab_view_close_page (view, page);

    /* If that was the last slot in the window, close the window. */
    if (window->slots == NULL)
    {
        g_debug ("Last slot removed, closing the window");
        nautilus_window_close (window);
    }

#ifdef HAVE_MALLOC_TRIM
    if (malloc_trim_idle_id == 0)
    {
        malloc_trim_idle_id = g_idle_add (malloc_trim_idle_cb, NULL);
    }
#endif
}

static void
nautilus_window_sync_location_widgets (NautilusWindow *window)
{
    /* This function can only be called when there is a slot. */
    g_return_if_fail (window->active_slot != NULL);

    GFile *location = nautilus_window_slot_get_location (window->active_slot);

    if (location != NULL)
    {
        gtk_widget_set_visible (window->network_address_bar,
                                g_file_has_uri_scheme (location, SCHEME_NETWORK_VIEW));
    }
}

static gchar *
toast_undo_deleted_get_label (NautilusFileUndoInfo *undo_info)
{
    g_autoptr (GList) files = nautilus_file_undo_info_trash_get_files (NAUTILUS_FILE_UNDO_INFO_TRASH (undo_info));
    gchar *file_label;
    gchar *label;
    gint length;

    length = g_list_length (files);
    if (length == 1)
    {
        file_label = g_file_get_basename (files->data);
        /* Translators: only one item has been moved to trash and %s is its name. */
        label = g_markup_printf_escaped (_("“%s” moved to trash"), file_label);
        g_free (file_label);
    }
    else
    {
        /* Translators: one or more items might have been moved to trash, and %d
         * is the count. */
        label = g_markup_printf_escaped (ngettext ("%d file moved to trash", "%d files moved to trash", length), length);
    }

    return label;
}

static gchar *
toast_undo_unstar_get_label (NautilusFileUndoInfo *undo_info)
{
    GList *files;
    gchar *label;
    gint length;

    files = nautilus_file_undo_info_starred_get_files (NAUTILUS_FILE_UNDO_INFO_STARRED (undo_info));
    length = g_list_length (files);
    if (length == 1)
    {
        const char *file_label = nautilus_file_get_display_name (files->data);
        /* Translators: one item has been unstarred and %s is its name. */
        label = g_markup_printf_escaped (_("“%s” unstarred"), file_label);
    }
    else
    {
        /* Translators: one or more items have been unstarred, and %d
         * is the count. */
        label = g_markup_printf_escaped (ngettext ("%d file unstarred", "%d files unstarred", length), length);
    }

    return label;
}

static void
update_undo_redo_menu_items (NautilusWindow               *window,
                             NautilusFileUndoInfo         *info,
                             NautilusFileUndoManagerState  undo_state)
{
    gboolean undo_active;
    gboolean redo_active;
    g_autofree gchar *undo_label = NULL;
    g_autofree gchar *redo_label = NULL;
    g_autofree gchar *undo_description = NULL;
    g_autofree gchar *redo_description = NULL;
    gboolean is_undo;
    g_autoptr (GMenu) updated_section = g_menu_new ();
    g_autoptr (GMenuItem) undo_menu_item = NULL;
    g_autoptr (GMenuItem) redo_menu_item = NULL;
    GAction *action;

    /* Look at the last action from the undo manager, and get the text that
     * describes it, e.g. "Undo Create Folder"/"Redo Create Folder"
     */
    undo_active = redo_active = FALSE;
    if (info != NULL && undo_state > NAUTILUS_FILE_UNDO_MANAGER_STATE_NONE)
    {
        is_undo = undo_state == NAUTILUS_FILE_UNDO_MANAGER_STATE_UNDO;

        /* The last action can either be undone/redone. Activate the corresponding
         * menu item and deactivate the other
         */
        undo_active = is_undo;
        redo_active = !is_undo;
        nautilus_file_undo_info_get_strings (info, &undo_label, &undo_description,
                                             &redo_label, &redo_description);
    }

    /* Set the label of the undo and redo menu items, and activate them appropriately
     */
    if (!undo_active || undo_label == NULL)
    {
        g_free (undo_label);
        undo_label = g_strdup (_("_Undo"));
    }
    undo_menu_item = g_menu_item_new (undo_label, "win.undo");
    g_menu_append_item (updated_section, undo_menu_item);
    action = g_action_map_lookup_action (G_ACTION_MAP (window), "undo");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), undo_active);

    if (!redo_active || redo_label == NULL)
    {
        g_free (redo_label);
        redo_label = g_strdup (_("_Redo"));
    }
    redo_menu_item = g_menu_item_new (redo_label, "win.redo");
    g_menu_append_item (updated_section, redo_menu_item);
    action = g_action_map_lookup_action (G_ACTION_MAP (window), "redo");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), redo_active);

    nautilus_gmenu_set_from_model (G_MENU (window->undo_redo_section),
                                   G_MENU_MODEL (updated_section));
}

static void
nautilus_window_on_undo_changed (NautilusFileUndoManager *manager,
                                 NautilusWindow          *window)
{
    NautilusFileUndoInfo *undo_info;
    NautilusFileUndoManagerState state;
    AdwToast *toast;

    undo_info = nautilus_file_undo_manager_get_action ();
    state = nautilus_file_undo_manager_get_state ();

    update_undo_redo_menu_items (window, undo_info, state);

    if (undo_info != NULL &&
        state == NAUTILUS_FILE_UNDO_MANAGER_STATE_UNDO)
    {
        gboolean popup_toast = FALSE;
        g_autofree gchar *label = NULL;

        if (nautilus_file_undo_info_get_op_type (undo_info) == NAUTILUS_FILE_UNDO_OP_MOVE_TO_TRASH)
        {
            g_autoptr (GList) files = NULL;

            files = nautilus_file_undo_info_trash_get_files (NAUTILUS_FILE_UNDO_INFO_TRASH (undo_info));

            /* Don't pop up a notification if user canceled the operation or the focus
             * is not in the this window. This is an easy way to know from which window
             * was the delete operation made */
            if (files != NULL && gtk_window_is_active (GTK_WINDOW (window)))
            {
                popup_toast = TRUE;
                label = toast_undo_deleted_get_label (undo_info);
            }
        }
        else if (nautilus_file_undo_info_get_op_type (undo_info) == NAUTILUS_FILE_UNDO_OP_STARRED)
        {
            if (window->active_slot == NULL)
            {
                return;
            }

            GFile *location = nautilus_window_slot_get_location (window->active_slot);
            /* Don't pop up a notification if the focus is not in the this
             * window. This is an easy way to know from which window was the
             * unstart operation made */
            if (g_file_has_uri_scheme (location, SCHEME_STARRED) &&
                gtk_window_is_active (GTK_WINDOW (window)) &&
                !nautilus_file_undo_info_starred_is_starred (NAUTILUS_FILE_UNDO_INFO_STARRED (undo_info)))
            {
                popup_toast = TRUE;
                label = toast_undo_unstar_get_label (undo_info);
            }
        }

        if (window->last_undo_toast != NULL)
        {
            adw_toast_dismiss (window->last_undo_toast);
            g_clear_weak_pointer (&window->last_undo_toast);
        }

        if (popup_toast)
        {
            toast = adw_toast_new (label);
            adw_toast_set_button_label (toast, _("Undo"));
            adw_toast_set_action_name (toast, "win.undo");
            adw_toast_set_priority (toast, ADW_TOAST_PRIORITY_HIGH);
            g_set_weak_pointer (&window->last_undo_toast, toast);
            adw_toast_overlay_add_toast (window->toast_overlay, toast);
        }
    }
}

void
nautilus_window_show_operation_notification (NautilusWindow *window,
                                             gchar          *main_label,
                                             GFile          *folder_to_open,
                                             gboolean        was_quick)
{
    gboolean is_current_location;
    AdwToast *toast;

    if (window->active_slot == NULL || !gtk_window_is_active (GTK_WINDOW (window)))
    {
        return;
    }

    is_current_location = g_file_equal (folder_to_open,
                                        nautilus_window_slot_get_location (window->active_slot));

    if (is_current_location && was_quick)
    {
        return;
    }

    toast = adw_toast_new (main_label);
    adw_toast_set_priority (toast, ADW_TOAST_PRIORITY_HIGH);
    adw_toast_set_use_markup (toast, FALSE);

    if (!is_current_location)
    {
        GVariant *target;

        target = g_variant_new_take_string (g_file_get_uri (folder_to_open));

        adw_toast_set_button_label (toast, _("Open Folder"));
        adw_toast_set_action_name (toast, "slot.open-location");
        adw_toast_set_action_target_value (toast, target);
    }

    adw_toast_overlay_add_toast (window->toast_overlay, toast);
}

static gboolean
tab_view_close_page_cb (AdwTabView     *view,
                        AdwTabPage     *page,
                        NautilusWindow *window)
{
    NautilusWindowSlot *slot;

    slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));

    window_slot_close (window, slot);

    return GDK_EVENT_PROPAGATE;
}

static void
tab_view_page_detached_cb (AdwTabView     *tab_view,
                           AdwTabPage     *page,
                           gint            position,
                           NautilusWindow *window)
{
    NautilusWindowSlot *slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));

    /* If the tab has been moved to another window, we need to remove the slot
     * from the current window here. Otherwise, if the tab has been closed, then
     * we have*/
    if (g_list_find (window->slots, slot))
    {
        remove_slot_from_window (slot, window);
    }

    if (adw_tab_view_get_n_pages (tab_view) == 0)
    {
        schedule_tile_collapse (window, nautilus_tile_for_widget (GTK_WIDGET (tab_view)));
    }
}

static void
tab_view_page_attached_cb (AdwTabView     *tab_view,
                           AdwTabPage     *page,
                           gint            position,
                           NautilusWindow *window)
{
    NautilusWindowSlot *slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));

    window->slots = g_list_prepend (window->slots, slot);

    if (page == dropped_page)
    {
        g_clear_weak_pointer (&dropped_page);
        set_active_tile (window, nautilus_tile_for_widget (GTK_WIDGET (tab_view)));
        adw_tab_view_set_selected_page (tab_view, page);
        set_active_slot (window, slot);
    }

    g_signal_emit (window, signals[LOCATIONS_CHANGED], 0);
}

static AdwTabView *
tab_view_create_window_cb (AdwTabView     *tab_view,
                           NautilusWindow *window)
{
    NautilusApplication *app;
    NautilusWindow *new_window;

    /* libadwaita asks for a new window after a drop outside its tab bars;
     * a tab dropped on a tile goes to that tile instead. */
    AdwTabView *tile_view = take_pending_tab_drop_view ();
    if (tile_view != NULL)
    {
        return tile_view;
    }

    app = NAUTILUS_APPLICATION (g_application_get_default ());
    new_window = nautilus_application_create_window (app, NULL);
    gtk_window_set_display (GTK_WINDOW (new_window),
                            gtk_widget_get_display (GTK_WIDGET (tab_view)));

    gtk_window_present (GTK_WINDOW (new_window));

    return new_window->tab_view;
}

static void
action_tab_move_new_window (GSimpleAction *action,
                            GVariant      *parameter,
                            gpointer       user_data)
{
    NautilusWindow *window = user_data;
    AdwTabPage *page = get_current_page (window);
    AdwTabView *new_view = tab_view_create_window_cb (window->tab_view, window);

    adw_tab_view_transfer_page (window->tab_view, page, new_view, 0);
}

static void
setup_tab_view (NautilusWindow *window,
                AdwTabView     *tab_view)
{
    g_signal_connect (tab_view, "close-page",
                      G_CALLBACK (tab_view_close_page_cb),
                      window);
    g_signal_connect (tab_view, "setup-menu",
                      G_CALLBACK (tab_view_setup_menu_cb),
                      window);
    g_signal_connect (tab_view, "notify::selected-page",
                      G_CALLBACK (tab_view_notify_selected_page_cb),
                      window);
    g_signal_connect (tab_view, "create-window",
                      G_CALLBACK (tab_view_create_window_cb),
                      window);
    g_signal_connect (tab_view, "page-attached",
                      G_CALLBACK (tab_view_page_attached_cb),
                      window);
    g_signal_connect (tab_view, "page-detached",
                      G_CALLBACK (tab_view_page_detached_cb),
                      window);
}

/* ------------------------------------------------------------------ Tiles
 *
 * The window's content is a layout of tiles, each a tab bar over a tab view.
 * window->tab_view is the active tile's view: new tabs and tab actions go
 * there, and the toolbar follows its selected slot.
 */

static AdwTabPage *dropped_page = NULL; /* weak: select it once it lands */

/* A tab dropped on a tile, waiting for libadwaita's create-window request
 * (see take_pending_tab_drop_view()). */
static struct
{
    NautilusWindow *window; /* weak */
    NautilusTile *tile;     /* weak */
    NautilusTileDrop where;
} pending_tab_drop;

static GdkDragAction extra_drag_value_cb (AdwTabBar    *self,
                                          AdwTabPage   *page,
                                          const GValue *value,
                                          gpointer      user_data);
static gboolean extra_drag_drop_cb (AdwTabBar    *self,
                                    AdwTabPage   *page,
                                    const GValue *value,
                                    gpointer      user_data);

/* Makes @tile the one new tabs and tab actions go to. */
static void
set_active_tile (NautilusWindow *window,
                 NautilusTile   *tile)
{
    if (tile == NULL || window->tab_view == nautilus_tile_get_view (tile))
    {
        return;
    }

    window->tab_view = nautilus_tile_get_view (tile);
    window->tab_bar = nautilus_tile_get_bar (tile);
    nautilus_tile_layout_set_active (window->tile_layout, tile);
}

/* Like set_active_tile(), and the tile's selected tab becomes the active slot. */
static void
activate_tile (NautilusWindow *window,
               NautilusTile   *tile)
{
    if (tile == NULL || window->tab_view == nautilus_tile_get_view (tile))
    {
        return;
    }

    set_active_tile (window, tile);

    AdwTabPage *page = adw_tab_view_get_selected_page (window->tab_view);
    if (page != NULL)
    {
        set_active_slot (window, NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page)));
    }
}

static void
on_tile_focus_enter (GtkEventControllerFocus *controller,
                     NautilusWindow          *window)
{
    activate_tile (window, NAUTILUS_TILE (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller))));
}

static void
on_tile_pressed (GtkGestureClick *gesture,
                 int              n_press,
                 double           x,
                 double           y,
                 NautilusWindow  *window)
{
    activate_tile (window, NAUTILUS_TILE (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture))));
}

typedef struct
{
    NautilusWindow *window;
    NautilusTile *tile;
} TileCollapse;

/* Removes a tile whose last tab was closed or dragged away. Waits while a
 * tab drag is running, since a cancelled drag brings the tab back. */
static gboolean
collapse_tile_cb (gpointer user_data)
{
    TileCollapse *collapse = user_data;
    NautilusWindow *window = collapse->window;
    NautilusTile *tile = collapse->tile;
    AdwTabView *view = nautilus_tile_get_view (tile);

    if (adw_tab_view_get_is_transferring_page (view))
    {
        return G_SOURCE_CONTINUE;
    }

    g_object_set_data (G_OBJECT (tile), "nautilus-collapse-pending", NULL);

    if (window->tile_layout != NULL &&
        adw_tab_view_get_n_pages (view) == 0 &&
        nautilus_tile_layout_get_n_tiles (window->tile_layout) > 1 &&
        g_list_find (nautilus_tile_layout_get_tiles (window->tile_layout), tile) != NULL)
    {
        gboolean was_active = (window->tab_view == view);

        nautilus_tile_layout_remove (window->tile_layout, tile);

        if (was_active)
        {
            window->tab_view = NULL;
            activate_tile (window, nautilus_tile_layout_get_tiles (window->tile_layout)->data);
            if (window->active_slot != NULL)
            {
                gtk_widget_grab_focus (GTK_WIDGET (window->active_slot));
            }
        }
    }

    g_object_unref (window);
    g_object_unref (tile);
    g_free (collapse);

    return G_SOURCE_REMOVE;
}

static void
schedule_tile_collapse (NautilusWindow *window,
                        NautilusTile   *tile)
{
    if (tile == NULL || g_object_get_data (G_OBJECT (tile), "nautilus-collapse-pending") != NULL)
    {
        return;
    }

    TileCollapse *collapse = g_new0 (TileCollapse, 1);

    collapse->window = g_object_ref (window);
    collapse->tile = g_object_ref (tile);
    g_object_set_data (G_OBJECT (tile), "nautilus-collapse-pending", GINT_TO_POINTER (TRUE));
    g_timeout_add (150, collapse_tile_cb, collapse);
}

/* The dragged tab, read straight from the in-process drag (a drop target's
 * own value only arrives asynchronously). */
static AdwTabPage *
drop_dup_page (GtkDropTarget *target)
{
    GdkDrop *drop = gtk_drop_target_get_current_drop (target);
    GdkDrag *drag = drop != NULL ? gdk_drop_get_drag (drop) : NULL;
    g_auto (GValue) value = G_VALUE_INIT;

    if (drag == NULL)
    {
        return NULL;
    }

    g_value_init (&value, ADW_TYPE_TAB_PAGE);
    if (!gdk_content_provider_get_value (gdk_drag_get_content (drag), &value, NULL))
    {
        return NULL;
    }

    return g_value_dup_object (&value);
}

static gboolean
tile_drop_pick (GtkDropTarget    *target,
                NautilusWindow   *window,
                double            x,
                double            y,
                NautilusTile    **tile,
                NautilusTileDrop *where)
{
    GtkWidget *view = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
    g_autoptr (AdwTabPage) page = drop_dup_page (target);
    graphene_point_t point;
    graphene_rect_t zone;

    *tile = nautilus_tile_for_widget (view);

    if (page == NULL || *tile == NULL ||
        !gtk_widget_compute_point (view, GTK_WIDGET (*tile), &GRAPHENE_POINT_INIT (x, y), &point))
    {
        nautilus_tile_layout_set_drop_zone (window->tile_layout, NULL);
        return FALSE;
    }

    *where = nautilus_tile_layout_pick_drop (window->tile_layout, *tile, point.x, point.y, &zone);
    nautilus_tile_layout_set_drop_zone (window->tile_layout, &zone);

    return TRUE;
}

static GdkDragAction
tile_drop_enter_cb (GtkDropTarget  *target,
                    double          x,
                    double          y,
                    NautilusWindow *window)
{
    NautilusTile *tile;
    NautilusTileDrop where;

    /* A new drag: forget a drop that never got its create-window request. */
    g_clear_weak_pointer (&pending_tab_drop.window);
    g_clear_weak_pointer (&pending_tab_drop.tile);

    return tile_drop_pick (target, window, x, y, &tile, &where) ? GDK_ACTION_MOVE : 0;
}

static GdkDragAction
tile_drop_motion_cb (GtkDropTarget  *target,
                     double          x,
                     double          y,
                     NautilusWindow *window)
{
    NautilusTile *tile;
    NautilusTileDrop where;

    return tile_drop_pick (target, window, x, y, &tile, &where) ? GDK_ACTION_MOVE : 0;
}

static void
tile_drop_leave_cb (GtkDropTarget  *target,
                    NautilusWindow *window)
{
    nautilus_tile_layout_set_drop_zone (window->tile_layout, NULL);
}

static gboolean
tile_drop_cb (GtkDropTarget  *target,
              const GValue   *value,
              double          x,
              double          y,
              NautilusWindow *window)
{
    g_autoptr (AdwTabPage) page = drop_dup_page (target);
    NautilusTile *tile;
    NautilusTileDrop where;

    if (page == NULL || !tile_drop_pick (target, window, x, y, &tile, &where))
    {
        return FALSE;
    }

    nautilus_tile_layout_set_drop_zone (window->tile_layout, NULL);

    /* libadwaita detached the page when the drag began. After this drop it
     * asks for a new window to put it in; we hand it the tile instead. */
    g_set_weak_pointer (&pending_tab_drop.window, window);
    g_set_weak_pointer (&pending_tab_drop.tile, tile);
    pending_tab_drop.where = where;
    g_set_weak_pointer (&dropped_page, page);

    return TRUE;
}

static AdwTabView *
take_pending_tab_drop_view (void)
{
    NautilusWindow *window = pending_tab_drop.window;
    NautilusTile *tile = pending_tab_drop.tile;
    NautilusTileDrop where = pending_tab_drop.where;

    g_clear_weak_pointer (&pending_tab_drop.window);
    g_clear_weak_pointer (&pending_tab_drop.tile);

    if (window == NULL || tile == NULL)
    {
        return NULL;
    }

    if (where == NAUTILUS_TILE_DROP_CENTER)
    {
        return nautilus_tile_get_view (tile);
    }

    NautilusTile *new_tile = create_tile (window);

    nautilus_tile_layout_split (window->tile_layout, tile, new_tile, where);

    return nautilus_tile_get_view (new_tile);
}

static NautilusTile *
create_tile (NautilusWindow *window)
{
    NautilusTile *tile = nautilus_tile_new (window->tab_menu_model);
    AdwTabView *view = nautilus_tile_get_view (tile);
    AdwTabBar *bar = nautilus_tile_get_bar (tile);

    setup_tab_view (window, view);
    adw_tab_bar_set_inverted (bar, window->tab_bar_inverted);

    /* Only allow tab DnD in Wayland.  We are using a hack in list-base to
     * get the preferred action which we can not replicate here because we
     * don't have access to the GtkDropTarget to check the actions on the GdkDrag.
     * See: https://gitlab.gnome.org/GNOME/gtk/-/merge_requests/4982
     */
#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY (gtk_widget_get_display (GTK_WIDGET (window))))
    {
        adw_tab_bar_setup_extra_drop_target (bar,
                                             GDK_ACTION_COPY | GDK_ACTION_MOVE,
                                             (GType [3]) {GDK_TYPE_TEXTURE, GDK_TYPE_FILE_LIST, G_TYPE_STRING}, 3);
        adw_tab_bar_set_extra_drag_preload (bar, TRUE);
        g_signal_connect (bar, "extra-drag-value", G_CALLBACK (extra_drag_value_cb), NULL);
        g_signal_connect (bar, "extra-drag-drop", G_CALLBACK (extra_drag_drop_cb), NULL);
    }
#endif

    /* While a tab is dragged, AdwTabView makes its pages untargetable, so
     * the target sits on the view. Capture phase: run before the tab bar's
     * own target on the view, which would only add the tab to this tile. */
    GtkDropTarget *drop = gtk_drop_target_new (ADW_TYPE_TAB_PAGE, GDK_ACTION_MOVE);

    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drop), GTK_PHASE_CAPTURE);
    g_signal_connect (drop, "enter", G_CALLBACK (tile_drop_enter_cb), window);
    g_signal_connect (drop, "motion", G_CALLBACK (tile_drop_motion_cb), window);
    g_signal_connect (drop, "leave", G_CALLBACK (tile_drop_leave_cb), window);
    g_signal_connect (drop, "drop", G_CALLBACK (tile_drop_cb), window);
    gtk_widget_add_controller (GTK_WIDGET (view), GTK_EVENT_CONTROLLER (drop));

    /* Focus or a click anywhere in a tile makes it the active one. */
    GtkEventController *focus = gtk_event_controller_focus_new ();
    GtkGesture *click = gtk_gesture_click_new ();

    g_signal_connect (focus, "enter", G_CALLBACK (on_tile_focus_enter), window);
    gtk_widget_add_controller (GTK_WIDGET (tile), focus);
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
    g_signal_connect (click, "pressed", G_CALLBACK (on_tile_pressed), window);
    gtk_widget_add_controller (GTK_WIDGET (tile), GTK_EVENT_CONTROLLER (click));

    return tile;
}

/* Opens the current folder in a new tile beside or below the active one. */
static void
split_active_tile (NautilusWindow   *window,
                   NautilusTileDrop  where)
{
    NautilusTile *current = nautilus_tile_for_widget (GTK_WIDGET (window->tab_view));
    GFile *location = window->active_slot != NULL
                      ? nautilus_window_slot_get_location (window->active_slot)
                      : NULL;
    g_autoptr (GFile) target = NULL;

    if (current == NULL)
    {
        return;
    }

    if (location == NULL || g_file_has_uri_scheme (location, SCHEME_SEARCH))
    {
        target = g_file_new_for_path (g_get_home_dir ());
    }
    else
    {
        target = g_object_ref (location);
    }

    NautilusTile *tile = create_tile (window);

    nautilus_tile_layout_split (window->tile_layout, current, tile, where);
    set_active_tile (window, tile);
    nautilus_window_open_location_full (window, target, NAUTILUS_OPEN_FLAG_NEW_TAB, NULL);
}

static void
action_split_right (GSimpleAction *action,
                    GVariant      *state,
                    gpointer       user_data)
{
    split_active_tile (NAUTILUS_WINDOW (user_data), NAUTILUS_TILE_DROP_RIGHT);
}

static void
action_split_down (GSimpleAction *action,
                   GVariant      *state,
                   gpointer       user_data)
{
    split_active_tile (NAUTILUS_WINDOW (user_data), NAUTILUS_TILE_DROP_BOTTOM);
}

static void
focus_tile_step (NautilusWindow *window,
                 gboolean        forward)
{
    GList *tiles = nautilus_tile_layout_get_tiles (window->tile_layout);
    GList *link = g_list_find (tiles, nautilus_tile_for_widget (GTK_WIDGET (window->tab_view)));
    GList *to;

    if (forward)
    {
        to = (link != NULL && link->next != NULL) ? link->next : tiles;
    }
    else
    {
        to = (link != NULL && link->prev != NULL) ? link->prev : g_list_last (tiles);
    }

    if (to != NULL && to != link)
    {
        activate_tile (window, to->data);
        if (window->active_slot != NULL)
        {
            gtk_widget_grab_focus (GTK_WIDGET (window->active_slot));
        }
    }
}

/* Ctrl+Tab: through every tab of every tile, in order, wrapping round. */
static void
tab_step_all (NautilusWindow *window,
              gboolean        forward)
{
    GList *tiles = nautilus_tile_layout_get_tiles (window->tile_layout);
    GList *link = g_list_find (tiles, nautilus_tile_for_widget (GTK_WIDGET (window->tab_view)));
    AdwTabPage *page = adw_tab_view_get_selected_page (window->tab_view);
    int n = adw_tab_view_get_n_pages (window->tab_view);
    int pos = page != NULL ? adw_tab_view_get_page_position (window->tab_view, page) : 0;
    GList *to;
    AdwTabView *view;

    if (forward ? pos + 1 < n : pos > 0)
    {
        adw_tab_view_set_selected_page (window->tab_view,
                                        adw_tab_view_get_nth_page (window->tab_view, pos + (forward ? 1 : -1)));
        return;
    }

    if (forward)
    {
        to = (link != NULL && link->next != NULL) ? link->next : tiles;
    }
    else
    {
        to = (link != NULL && link->prev != NULL) ? link->prev : g_list_last (tiles);
    }
    if (to == NULL)
    {
        return;
    }

    view = nautilus_tile_get_view (to->data);
    n = adw_tab_view_get_n_pages (view);
    if (n > 0)
    {
        adw_tab_view_set_selected_page (view, adw_tab_view_get_nth_page (view, forward ? 0 : n - 1));
    }
    if (to != link)
    {
        activate_tile (window, to->data);
    }
    if (window->active_slot != NULL)
    {
        gtk_widget_grab_focus (GTK_WIDGET (window->active_slot));
    }
}

static void
action_sidebar_step (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
    NautilusWindow *window = user_data;

    nautilus_sidebar_step (window->places_sidebar, g_variant_get_int32 (parameter));
}

static void
action_sidebar_fold (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
    NautilusWindow *window = user_data;

    nautilus_sidebar_fold_current (window->places_sidebar, g_variant_get_boolean (parameter));
}

static void
action_tab_next_all (GSimpleAction *action,
                     GVariant      *state,
                     gpointer       user_data)
{
    tab_step_all (NAUTILUS_WINDOW (user_data), TRUE);
}

static void
action_tab_previous_all (GSimpleAction *action,
                         GVariant      *state,
                         gpointer       user_data)
{
    tab_step_all (NAUTILUS_WINDOW (user_data), FALSE);
}

static void
action_focus_next_tile (GSimpleAction *action,
                        GVariant      *state,
                        gpointer       user_data)
{
    focus_tile_step (NAUTILUS_WINDOW (user_data), TRUE);
}

static void
action_focus_previous_tile (GSimpleAction *action,
                            GVariant      *state,
                            gpointer       user_data)
{
    focus_tile_step (NAUTILUS_WINDOW (user_data), FALSE);
}

static GdkDragAction
extra_drag_value_cb (AdwTabBar    *self,
                     AdwTabPage   *page,
                     const GValue *value,
                     gpointer      user_data)
{
    NautilusWindowSlot *slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));
    g_autoptr (NautilusFile) file = nautilus_file_get (nautilus_window_slot_get_location (slot));
    GdkDragAction action = 0;

    if (value != NULL)
    {
        if (G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
        {
            GSList *file_list = g_value_get_boxed (value);
            if (file_list != NULL)
            {
                action = nautilus_dnd_get_preferred_action (file, G_FILE (file_list->data));
            }
        }
        else if (G_VALUE_HOLDS (value, G_TYPE_STRING) || G_VALUE_HOLDS (value, GDK_TYPE_TEXTURE))
        {
            action = GDK_ACTION_COPY;
        }
    }

    return action;
}

static gboolean
extra_drag_drop_cb (AdwTabBar    *self,
                    AdwTabPage   *page,
                    const GValue *value,
                    gpointer      user_data)
{
    NautilusWindowSlot *slot = NAUTILUS_WINDOW_SLOT (adw_tab_page_get_child (page));
    NautilusFilesView *view = nautilus_window_slot_get_current_view (slot);
    GFile *target_location = nautilus_window_slot_get_location (slot);
    GdkDragAction action = adw_tab_bar_get_extra_drag_preferred_action (self);

    return nautilus_dnd_perform_drop (view, value, action, target_location);
}

/* ------------------------------------------------------------ Capture */

/* For the README's pictures: with FLIER_CAPTURE set, win.flier-capture(s)
 * takes "shot:FILE.png", "rec:DIR" / "stop" (frames at FLIER_CAPTURE_FPS,
 * default 15), "page:TITLE" and "scroll:PIXELS" for Preferences,
 * "set:KEY=VALUE" for an org.gnome.nautilus.flier key,
 * "act:ACTION" (run on the files view, so view., slot. and win. all work),
 * "select:NAME,NAME" and "size:W,H". Nothing of this exists otherwise. */

static guint capture_timer;
static char *capture_dir;
static guint capture_frame;
static gint64 capture_start;

static GtkWidget *
capture_find_popover (GtkWidget *widget)
{
    if (GTK_IS_POPOVER (widget) && gtk_widget_get_mapped (widget))
    {
        return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        GtkWidget *found = capture_find_popover (child);

        if (found != NULL)
        {
            return found;
        }
    }
    return NULL;
}

/* For "locate:": whether @widget shows @text, as its label, the start of
 * its tooltip (which may go on with the keys), its icon or its name, or
 * has the CSS class .CLASS. */
static gboolean
capture_shows (GtkWidget  *widget,
               const char *text)
{
    const char *tooltip = gtk_widget_get_tooltip_text (widget);

    if (GTK_IS_LABEL (widget) && g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), text) == 0)
    {
        return TRUE;
    }
    if (tooltip != NULL && g_str_has_prefix (tooltip, text))
    {
        return TRUE;
    }
    if (GTK_IS_BUTTON (widget) && g_strcmp0 (gtk_button_get_icon_name (GTK_BUTTON (widget)), text) == 0)
    {
        return TRUE;
    }
    if (GTK_IS_MENU_BUTTON (widget) && g_strcmp0 (gtk_menu_button_get_icon_name (GTK_MENU_BUTTON (widget)), text) == 0)
    {
        return TRUE;
    }
    if (GTK_IS_IMAGE (widget) && g_strcmp0 (gtk_image_get_icon_name (GTK_IMAGE (widget)), text) == 0)
    {
        return TRUE;
    }
    if (text[0] == '.' && gtk_widget_has_css_class (widget, text + 1))
    {
        return TRUE;
    }
    return g_strcmp0 (gtk_widget_get_name (widget), text) == 0;
}

static const char *capture_scope;

static gboolean
capture_in_scope (GtkWidget *widget)
{
    if (capture_scope == NULL)
    {
        return TRUE;
    }
    for (GtkWidget *w = widget; w != NULL; w = gtk_widget_get_parent (w))
    {
        if (g_strcmp0 (G_OBJECT_TYPE_NAME (w), capture_scope) == 0 ||
            g_strcmp0 (gtk_widget_get_css_name (w), capture_scope) == 0 ||
            gtk_widget_has_css_class (w, capture_scope))
        {
            return TRUE;
        }
    }
    return FALSE;
}

/* The @n-th (from 0) mapped widget under @widget that shows @text. */
static GtkWidget *
capture_locate (GtkWidget  *widget,
                const char *text,
                int        *n)
{
    if (!gtk_widget_get_mapped (widget))
    {
        return NULL;
    }
    if (capture_shows (widget, text) && capture_in_scope (widget) && (*n)-- == 0)
    {
        return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        GtkWidget *found = capture_locate (child, text, n);

        if (found != NULL)
        {
            return found;
        }
    }
    return NULL;
}

/* "locate:[SCOPE>]TEXT[@N]:FILE" writes the centre and size of what shows TEXT, in
 * the window's surface coordinates ("x y width height"), or nothing.
 * Menus are searched first; a popover's place comes from its surface. */
static void
capture_write_location (NautilusWindow *window,
                        const char     *spec)
{
    g_autofree char *text = g_strdup (spec);
    char *file = strrchr (text, ':');
    char *at;
    int n = 0;
    GtkWidget *popover;
    GtkWidget *found = NULL;
    const char *text_start;
    graphene_rect_t bounds;
    double x = 0, y = 0;

    if (file == NULL)
    {
        return;
    }
    *file++ = '\0';
    text_start = text;
    at = strrchr (text, '@');
    if (at != NULL && g_ascii_isdigit (at[1]))
    {
        *at = '\0';
        n = atoi (at + 1);
    }
    /* "SCOPE>TEXT": only what is inside a SCOPE (a type, CSS name or class). */
    {
        char *gt = strchr (text, '>');

        if (gt != NULL)
        {
            *gt = '\0';
            capture_scope = text;
            text_start = gt + 1;
        }
        popover = capture_find_popover (GTK_WIDGET (window));
        if (popover != NULL)
        {
            int m = n;

            found = capture_locate (popover, text_start, &m);
        }
        if (found == NULL)
        {
            found = capture_locate (GTK_WIDGET (window), text_start, &n);
        }
        capture_scope = NULL;
    }
    if (found == NULL || !gtk_widget_compute_bounds (found, GTK_WIDGET (gtk_widget_get_native (found)), &bounds))
    {
        g_file_set_contents (file, "", 0, NULL);
        return;
    }
    /* Up through the popovers to the window. A popup's position is from its
     * parent's surface, which takes in the shadow of a window not maximized;
     * widget coordinates start inside the shadows. */
    for (GtkNative *native = gtk_widget_get_native (found); GTK_IS_POPOVER (native);)
    {
        GdkSurface *surface = gtk_native_get_surface (native);
        GtkNative *parent = gtk_widget_get_native (gtk_widget_get_parent (GTK_WIDGET (native)));
        double sx, sy, px, py;

        gtk_native_get_surface_transform (native, &sx, &sy);
        gtk_native_get_surface_transform (parent, &px, &py);
        x += gdk_popup_get_position_x (GDK_POPUP (surface)) + sx - px;
        y += gdk_popup_get_position_y (GDK_POPUP (surface)) + sy - py;
        native = parent;
    }
    {
        g_autofree char *line = g_strdup_printf ("%.0f %.0f %.0f %.0f\n",
                                                 x + bounds.origin.x + bounds.size.width / 2,
                                                 y + bounds.origin.y + bounds.size.height / 2,
                                                 bounds.size.width, bounds.size.height);

        g_file_set_contents (file, line, -1, NULL);
    }
}

static void
capture_to_png (GtkWidget  *widget,
                const char *path)
{
    GtkNative *window = GTK_NATIVE (widget);
    g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (widget);
    g_autoptr (GtkSnapshot) snapshot = gtk_snapshot_new ();
    g_autoptr (GskRenderNode) node = NULL;
    g_autoptr (GdkTexture) texture = NULL;
    int width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
    const char *scale_env = g_getenv ("FLIER_CAPTURE_SCALE");
    double scale = scale_env != NULL ? g_ascii_strtod (scale_env, NULL)
                                     : gdk_surface_get_scale (gtk_native_get_surface (window));

    gtk_snapshot_scale (snapshot, scale, scale);
    gdk_paintable_snapshot (paintable, snapshot, width, height);
    node = gtk_snapshot_to_node (snapshot);
    if (node == NULL)
    {
        return;
    }
    texture = gsk_renderer_render_texture (gtk_native_get_renderer (window), node,
                                           &GRAPHENE_RECT_INIT (0, 0, width * scale, height * scale));
    if (g_str_has_suffix (path, ".ppm"))
    {
        /* Recording frames: raw RGB, much faster to write than PNG. */
        g_autoptr (GdkTextureDownloader) downloader = gdk_texture_downloader_new (texture);
        gsize stride;
        g_autoptr (GBytes) bytes = NULL;
        g_autofree char *header = NULL;
        g_autoptr (GByteArray) data = g_byte_array_new ();
        int w = gdk_texture_get_width (texture), h = gdk_texture_get_height (texture);

        gdk_texture_downloader_set_format (downloader, GDK_MEMORY_R8G8B8);
        bytes = gdk_texture_downloader_download_bytes (downloader, &stride);
        header = g_strdup_printf ("P6\n%d %d\n255\n", w, h);
        g_byte_array_append (data, (guint8 *) header, strlen (header));
        for (int y = 0; y < h; y++)
        {
            g_byte_array_append (data, (const guint8 *) g_bytes_get_data (bytes, NULL) + y * stride, w * 3);
        }
        g_file_set_contents (path, (const char *) data->data, data->len, NULL);
        return;
    }
    gdk_texture_save_to_png (texture, path);
}

static GtkWidget *
capture_find_page (GtkWidget  *widget,
                   const char *title)
{
    if (ADW_IS_PREFERENCES_PAGE (widget) &&
        (title == NULL ? gtk_widget_get_mapped (widget)
                       : g_strcmp0 (adw_preferences_page_get_title (ADW_PREFERENCES_PAGE (widget)), title) == 0))
    {
        return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        GtkWidget *found = capture_find_page (child, title);

        if (found != NULL)
        {
            return found;
        }
    }
    return NULL;
}

static GtkWidget *
capture_find_scroller (GtkWidget *widget)
{
    if (GTK_IS_SCROLLED_WINDOW (widget))
    {
        return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        GtkWidget *found = capture_find_scroller (child);

        if (found != NULL)
        {
            return found;
        }
    }
    return NULL;
}

static void
capture_mark (const char *label)
{
    g_autofree char *path = NULL;
    g_autofree char *line = NULL;
    g_autoptr (GFileOutputStream) out = NULL;

    if (capture_timer == 0 || capture_dir == NULL)
    {
        return;
    }
    path = g_build_filename (capture_dir, "markers.tsv", NULL);
    line = g_strdup_printf ("%" G_GINT64_FORMAT "\t%s\n", (g_get_monotonic_time () - capture_start) / 1000, label);
    g_autoptr (GFile) file = g_file_new_for_path (path);

    out = g_file_append_to (file, G_FILE_CREATE_NONE, NULL, NULL);
    if (out != NULL)
    {
        g_output_stream_write_all (G_OUTPUT_STREAM (out), line, strlen (line), NULL, NULL, NULL);
    }
}

static GtkWidget *
capture_find_bottom_bar (GtkWidget *widget)
{
    if (NAUTILUS_IS_BOTTOM_BAR (widget) && gtk_widget_get_mapped (widget))
    {
        return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        GtkWidget *found = capture_find_bottom_bar (child);

        if (found != NULL)
        {
            return found;
        }
    }
    return NULL;
}

/* A benchmark: how long from a command until the folder is loaded
 * ("ready") and until that's on screen ("painted"). One JSON line per run
 * goes to FLIER_BENCH_LOG. */
typedef struct
{
    NautilusWindow *window;
    char *name;
    gint64 start, ready;
    guint poll_id;
    gulong paint_id;
    GdkFrameClock *clock;
    gboolean wait_load, seen_load;
} BenchRun;

static void
bench_finish (BenchRun *run)
{
    const char *log = g_getenv ("FLIER_BENCH_LOG");
    gint64 painted = g_get_monotonic_time ();

    g_signal_handler_disconnect (run->clock, run->paint_id);
    if (log != NULL)
    {
        g_autofree char *line = g_strdup_printf ("{\"name\": \"%s\", \"ready_ms\": %.2f, \"painted_ms\": %.2f}\n",
                                                 run->name, (run->ready - run->start) / 1000.0,
                                                 (painted - run->start) / 1000.0);
        g_autoptr (GFile) file = g_file_new_for_path (log);
        g_autoptr (GFileOutputStream) out = g_file_append_to (file, G_FILE_CREATE_NONE, NULL, NULL);

        if (out != NULL)
        {
            g_output_stream_write_all (G_OUTPUT_STREAM (out), line, strlen (line), NULL, NULL, NULL);
        }
    }
    g_free (run->name);
    g_free (run);
}

static void
bench_after_paint (GdkFrameClock *clock,
                   BenchRun      *run)
{
    bench_finish (run);
}

static gboolean
bench_poll (gpointer data)
{
    BenchRun *run = data;
    NautilusWindowSlot *slot = run->window->active_slot;
    NautilusFilesView *view = slot != NULL ? nautilus_window_slot_get_current_view (slot) : NULL;

    gboolean loading = slot == NULL || nautilus_window_slot_get_loading (slot) ||
                       view == NULL || nautilus_files_view_get_loading (view);

    /* A load starts a moment after the command; don't stop before it.
     * Give up waiting for one after a second. */
    run->seen_load |= loading;
    if (loading || (run->wait_load && !run->seen_load &&
                    g_get_monotonic_time () - run->start < G_USEC_PER_SEC))
    {
        return G_SOURCE_CONTINUE;
    }

    run->ready = g_get_monotonic_time ();
    run->poll_id = 0;
    run->clock = gtk_widget_get_frame_clock (GTK_WIDGET (run->window));
    run->paint_id = g_signal_connect (run->clock, "after-paint", G_CALLBACK (bench_after_paint), run);
    gtk_widget_queue_draw (GTK_WIDGET (run->window));
    return G_SOURCE_REMOVE;
}

static void action_flier_capture (GSimpleAction *action, GVariant *parameter, gpointer user_data);

static gboolean
capture_tick (gpointer data)
{
    /* Named by milliseconds since "rec:", so slow frames keep real time. */
    g_autofree char *path = g_strdup_printf ("%s/%08" G_GINT64_FORMAT ".%s", capture_dir,
                                             (g_get_monotonic_time () - capture_start) / 1000,
                                             g_getenv ("FLIER_CAPTURE_RAW") != NULL ? "ppm" : "png");

    capture_frame++;

    capture_to_png (data, path);
    return G_SOURCE_CONTINUE;
}

static void
action_flier_capture (GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       user_data)
{
    NautilusWindow *window = user_data;
    const char *command = g_variant_get_string (parameter, NULL);
    NautilusFilesView *view = window->active_slot != NULL
                              ? nautilus_window_slot_get_current_view (window->active_slot) : NULL;

    if (g_str_has_prefix (command, "key:") || g_str_has_prefix (command, "act:") ||
        g_str_has_prefix (command, "go:") || g_str_has_prefix (command, "select:") ||
        g_str_has_prefix (command, "filter:") || g_str_has_prefix (command, "set:"))
    {
        capture_mark (command);
    }

    if (g_str_has_prefix (command, "mark:"))
    {
        capture_mark (command + 5);
    }
    else if (g_str_has_prefix (command, "bench:") && strchr (command + 6, ':') != NULL)
    {
        /* bench:NAME:COMMAND */
        const char *rest = strchr (command + 6, ':') + 1;
        BenchRun *run = g_new0 (BenchRun, 1);

        run->window = window;
        run->name = g_strndup (command + 6, rest - 1 - (command + 6));
        run->wait_load = g_str_has_prefix (rest, "go:") || strstr (rest, "flat-view") != NULL;
        run->start = g_get_monotonic_time ();
        action_flier_capture (action, g_variant_new_string (rest), user_data);
        run->poll_id = g_timeout_add (1, bench_poll, run);
    }
    else if (g_str_has_prefix (command, "filter:"))
    {
        GtkWidget *bar = capture_find_bottom_bar (GTK_WIDGET (window));

        if (bar != NULL)
        {
            nautilus_bottom_bar_set_filter_text (NAUTILUS_BOTTOM_BAR (bar), command + 7);
        }
    }
    else if (g_str_has_prefix (command, "shot:"))
    {
        capture_to_png (GTK_WIDGET (window), command + 5);
    }
    else if (g_str_has_prefix (command, "locate:"))
    {
        capture_write_location (window, command + 7);
    }
    else if (g_str_has_prefix (command, "popover:"))
    {
        /* The open menu, which has its own surface and isn't in "shot:". */
        GtkWidget *popover = capture_find_popover (GTK_WIDGET (window));

        if (popover != NULL)
        {
            capture_to_png (popover, command + 8);
        }
    }
    else if (g_str_has_prefix (command, "rec:"))
    {
        g_free (capture_dir);
        capture_dir = g_strdup (command + 4);
        capture_frame = 0;
        capture_start = g_get_monotonic_time ();
        g_clear_handle_id (&capture_timer, g_source_remove);
        const char *fps = g_getenv ("FLIER_CAPTURE_FPS");

        capture_timer = g_timeout_add (1000 / (fps != NULL ? MAX (1, atoi (fps)) : 15), capture_tick, window);
    }
    else if (g_str_equal (command, "stop"))
    {
        g_clear_handle_id (&capture_timer, g_source_remove);
    }
    else if ((g_str_has_prefix (command, "act:") || g_str_has_prefix (command, "key:")) && view != NULL)
    {
        g_autofree char *name = NULL;
        g_autoptr (GVariant) target = NULL;

        /* "key:" is "act:" with the hint its key would show. */
        if (command[0] == 'k')
        {
            nautilus_shortcuts_show_hint_for (GTK_WINDOW (window), command + 4);
        }

        if (g_action_parse_detailed_name (command + 4, &name, &target, NULL))
        {
            gtk_widget_activate_action_variant (GTK_WIDGET (view), name, target);
        }
    }
    else if (g_str_has_prefix (command, "select:") && view != NULL)
    {
        g_auto (GStrv) names = g_strsplit (command + 7, ",", -1);
        GFile *location = nautilus_window_slot_get_location (window->active_slot);
        GList *files = NULL;

        for (guint i = 0; names[i] != NULL; i++)
        {
            g_autoptr (GFile) child = g_file_get_child (location, names[i]);

            files = g_list_append (files, nautilus_file_get (child));
        }
        nautilus_files_view_set_selection (view, files, TRUE);
        nautilus_file_list_free (files);
    }
    else if (g_str_has_prefix (command, "set:") && strchr (command, '=') != NULL)
    {
        g_autofree char *key = g_strndup (command + 4, strchr (command, '=') - (command + 4));
        g_autoptr (GSettingsSchema) schema = NULL;
        g_autoptr (GSettingsSchemaKey) schema_key = NULL;
        g_autoptr (GVariant) value = NULL;

        g_object_get (nautilus_flier_settings (), "settings-schema", &schema, NULL);
        if (g_settings_schema_has_key (schema, key))
        {
            schema_key = g_settings_schema_get_key (schema, key);
            value = g_variant_parse (g_settings_schema_key_get_value_type (schema_key),
                                     strchr (command, '=') + 1, NULL, NULL, NULL);
            if (value != NULL)
            {
                g_settings_set_value (nautilus_flier_settings (), key, value);
            }
        }
    }
    else if (g_str_has_prefix (command, "page:"))
    {
        GtkWidget *page = capture_find_page (GTK_WIDGET (window), command + 5);
        GtkWidget *dialog = page != NULL ? gtk_widget_get_ancestor (page, ADW_TYPE_PREFERENCES_DIALOG) : NULL;

        if (dialog != NULL)
        {
            adw_preferences_dialog_set_visible_page (ADW_PREFERENCES_DIALOG (dialog), ADW_PREFERENCES_PAGE (page));
        }
    }
    else if (g_str_has_prefix (command, "scroll:"))
    {
        GtkWidget *page = capture_find_page (GTK_WIDGET (window), NULL);
        GtkWidget *scroller = page != NULL ? capture_find_scroller (page) : NULL;

        if (scroller != NULL)
        {
            GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));

            gtk_adjustment_set_value (adj, gtk_adjustment_get_value (adj) + atof (command + 7));
        }
    }
    else if (g_str_has_prefix (command, "go:") && window->active_slot != NULL)
    {
        g_autoptr (GFile) location = g_file_new_for_path (command + 3);

        nautilus_window_slot_open_location_full (window->active_slot, location, NULL);
    }
    else if (g_str_has_prefix (command, "size:"))
    {
        int w = 0, h = 0;

        if (sscanf (command + 5, "%d,%d", &w, &h) == 2)
        {
            gtk_window_unmaximize (GTK_WINDOW (window));
            gtk_window_set_default_size (GTK_WINDOW (window), w, h);
            gtk_window_present (GTK_WINDOW (window));
        }
    }
}

const GActionEntry win_entries[] =
{
    { .name = "current-location-menu", .activate = action_show_current_location_menu },
    { .name = "new-tab", .activate = action_new_tab },
    { .name = "undo", .activate = action_undo },
    { .name = "redo", .activate = action_redo },
    /* Only accessible by shortcuts */
    { .name = "close-current-view", .activate = action_close_current_view },
    { .name = "close-other-tabs", .activate = action_close_other_tabs },
    { .name = "go-home", .activate = action_go_home },
    { .name = "tab-move-left", .activate = action_tab_move_left },
    { .name = "tab-move-right", .activate = action_tab_move_right },
    { .name = "tab-move-new-window", .activate = action_tab_move_new_window },
    { .name = "go-to-tab", .parameter_type = "i", .state = "0", .change_state = action_go_to_tab },
    { .name = "restore-tab", .activate = action_restore_tab },
    { .name = "split-right", .activate = action_split_right },
    { .name = "split-down", .activate = action_split_down },
    { .name = "focus-next-tile", .activate = action_focus_next_tile },
    { .name = "tab-next-all", .activate = action_tab_next_all },
    { .name = "sidebar-step", .parameter_type = "i", .activate = action_sidebar_step },
    { .name = "sidebar-fold", .parameter_type = "b", .activate = action_sidebar_fold },
    { .name = "tab-previous-all", .activate = action_tab_previous_all },
    { .name = "focus-previous-tile", .activate = action_focus_previous_tile },
    { .name = "toggle-sidebar", .activate = action_toggle_sidebar },
};

static void
nautilus_window_initialize_actions (NautilusWindow *window)
{
    GApplication *app;
    gchar detailed_action[80];
    gchar accel[80];
    gint i;

    g_action_map_add_action_entries (G_ACTION_MAP (window),
                                     win_entries, G_N_ELEMENTS (win_entries),
                                     window);

    if (g_getenv ("FLIER_CAPTURE") != NULL)
    {
        const GActionEntry capture_entry[] =
        {
            { .name = "flier-capture", .activate = action_flier_capture, .parameter_type = "s" },
        };

        g_action_map_add_action_entries (G_ACTION_MAP (window), capture_entry, 1, window);
        if (g_getenv ("FLIER_CAPTURE_PRESET") != NULL)
        {
            /* The look of the README pictures (run with GSETTINGS_BACKEND=memory). */
            GSettings *flier = nautilus_flier_settings ();
            const char *collapsed[] = { "recent", NULL };
            const char *hidden[] = { "devices", "cloud", NULL };

            /* A window behind others gets few frames; without animations
             * dialogs and previews show at once in the recording. Screen
             * recordings (present.sh) keep them, with FLIER_CAPTURE_ANIMATE. */
            if (g_getenv ("FLIER_CAPTURE_ANIMATE") == NULL)
            {
                g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
            }
            /* The icons of the person making the recording (the demo's
             * settings are in memory, with the distribution's default). */
            if (g_getenv ("FLIER_CAPTURE_ICON_THEME") != NULL)
            {
                g_object_set (gtk_settings_get_default (), "gtk-icon-theme-name",
                              g_getenv ("FLIER_CAPTURE_ICON_THEME"), NULL);
            }
            g_settings_set_boolean (flier, "file-flier-imported", TRUE);
            g_settings_set_boolean (flier, "sidebar-groups", TRUE);
            g_settings_set_strv (flier, "sidebar-collapsed", collapsed);
            g_settings_set_strv (flier, "sidebar-hidden-groups", hidden);
        }
    }

#define ACCELS(...) ((const char *[]) { __VA_ARGS__, NULL })

    app = g_application_get_default ();
    nautilus_application_set_accelerator (app, "win.new-tab", "<control>t");
    nautilus_application_set_accelerator (app, "win.close-current-view", "<control>w");

    nautilus_application_set_accelerator (app, "win.undo", "<control>z");
    nautilus_application_set_accelerator (app, "win.redo", "<shift><control>z");
    /* Only accessible by shortcuts */
    nautilus_application_set_accelerator (app, "win.tab-move-left", "<shift><control>Page_Up");
    nautilus_application_set_accelerator (app, "win.tab-move-right", "<shift><control>Page_Down");
    nautilus_application_set_accelerator (app, "win.current-location-menu", "F10");
    nautilus_application_set_accelerator (app, "win.restore-tab", "<shift><control>t");
    nautilus_application_set_accelerator (app, "win.toggle-sidebar", "F9");
    nautilus_application_set_accelerator (app, "win.split-right", "F3");
    nautilus_application_set_accelerator (app, "win.split-down", "<shift>F3");
    nautilus_application_set_accelerator (app, "win.focus-next-tile", "F6");
    nautilus_application_set_accelerator (app, "win.focus-previous-tile", "<shift>F6");

    /* Alt+N for tabs 5 to 9; Alt+1 to 4 switch views. */
    for (i = 4; i < 9; ++i)
    {
        g_snprintf (detailed_action, sizeof (detailed_action), "win.go-to-tab(%i)", i);
        g_snprintf (accel, sizeof (accel), "<alt>%i", i + 1);
        nautilus_application_set_accelerator (app, detailed_action, accel);
    }

    nautilus_window_on_undo_changed (nautilus_file_undo_manager_get (), window);

#undef ACCELS

    /* The sidebar can be hidden at any width, not only when it overlays. */
}

static gboolean
is_layout_reversed (void)
{
    GtkSettings *settings = gtk_settings_get_default ();
    g_autofree char *layout = NULL;
    g_auto (GStrv) parts = NULL;

    if (settings != NULL)
    {
        g_object_get (settings, "gtk-decoration-layout", &layout, NULL);
    }

    if (layout != NULL)
    {
        parts = g_strsplit (layout, ":", 2);
    }

    /* Invalid layout, don't even try */
    if (parts == NULL || g_strv_length (parts) < 2)
    {
        return FALSE;
    }

    g_return_val_if_fail (parts[0] != NULL, FALSE);
    return (g_strrstr (parts[0], "close") != NULL);
}

static void
notify_decoration_layout_cb (NautilusWindow *self)
{
    self->tab_bar_inverted = is_layout_reversed ();

    for (GList *l = nautilus_tile_layout_get_tiles (self->tile_layout); l != NULL; l = l->next)
    {
        adw_tab_bar_set_inverted (nautilus_tile_get_bar (l->data), self->tab_bar_inverted);
    }
}

static void
nautilus_window_constructed (GObject *self)
{
    NautilusWindow *window;
    NautilusApplication *application;
    GtkSettings *settings;

    window = NAUTILUS_WINDOW (self);

    G_OBJECT_CLASS (nautilus_window_parent_class)->constructed (self);

    application = NAUTILUS_APPLICATION (g_application_get_default ());
    gtk_window_set_application (GTK_WINDOW (window), GTK_APPLICATION (application));

    gtk_window_set_default_size (GTK_WINDOW (window),
                                 NAUTILUS_WINDOW_DEFAULT_WIDTH,
                                 NAUTILUS_WINDOW_DEFAULT_HEIGHT);

    NautilusTile *first_tile = create_tile (window);

    nautilus_tile_layout_set_root (window->tile_layout, first_tile);
    set_active_tile (window, first_tile);

    settings = gtk_settings_get_default ();
    g_signal_connect_object (settings, "notify::gtk-decoration-layout",
                             G_CALLBACK (notify_decoration_layout_cb), window,
                             G_CONNECT_SWAPPED);
    notify_decoration_layout_cb (window);

    nautilus_window_set_up_sidebar (window);


    g_signal_connect_object (nautilus_file_undo_manager_get (), "undo-changed",
                             G_CALLBACK (nautilus_window_on_undo_changed), self,
                             G_CONNECT_AFTER);

    /* Is required that the UI is constructed before initializing the actions, since
     * some actions trigger UI widgets to show/hide. */
    nautilus_window_initialize_actions (window);
}

static void sync_view_mode (NautilusWindow *window);

static void
nautilus_window_dispose (GObject *object)
{
    NautilusWindow *window;
    GList *slots_copy;

    window = NAUTILUS_WINDOW (object);

    g_clear_handle_id (&window->inspector_save_id, g_source_remove);
    if (window->inspector_window != NULL)
    {
        /* The inspector goes with its window. */
        gtk_window_destroy (GTK_WINDOW (window->inspector_window));
        window->inspector_window = NULL;
        window->inspector = NULL;
    }
    if (window->mode_columns != NULL)
    {
        g_signal_handlers_disconnect_by_func (window->mode_columns, sync_view_mode, window);
        g_clear_weak_pointer (&window->mode_columns);
    }
    g_debug ("Destroying window");

    /* close all slots safely */
    slots_copy = g_list_copy (window->slots);
    g_list_foreach (slots_copy, (GFunc) remove_slot_from_window, window);
    g_list_free (slots_copy);

    /* the slots list should now be empty */
    g_warn_if_fail (window->slots == NULL);

    g_clear_weak_pointer (&window->active_slot);

    gtk_widget_dispose_template (GTK_WIDGET (window), NAUTILUS_TYPE_WINDOW);

    G_OBJECT_CLASS (nautilus_window_parent_class)->dispose (object);
}

static void
nautilus_window_finalize (GObject *object)
{
    NautilusWindow *window;

    window = NAUTILUS_WINDOW (object);

    if (window->sidebar_width_handler_id != 0)
    {
        g_source_remove (window->sidebar_width_handler_id);
        window->sidebar_width_handler_id = 0;
    }

    g_clear_object (&window->selected_file);
    g_clear_object (&window->selected_volume);

    g_signal_handlers_disconnect_by_func (nautilus_file_undo_manager_get (),
                                          G_CALLBACK (nautilus_window_on_undo_changed),
                                          window);

    g_queue_free_full (window->tab_data_queue, free_navigation_state);

    /* nautilus_window_close() should have run */
    g_warn_if_fail (window->slots == NULL);

    G_OBJECT_CLASS (nautilus_window_parent_class)->finalize (object);
}

static void
nautilus_window_save_geometry (NautilusWindow *window)
{
    gint width;
    gint height;
    GVariant *initial_size;

    gtk_window_get_default_size (GTK_WINDOW (window), &width, &height);
    initial_size = g_variant_new_parsed ("(%i, %i)", width, height);

    g_settings_set_value (nautilus_window_state,
                          NAUTILUS_WINDOW_STATE_INITIAL_SIZE,
                          initial_size);
}

void
nautilus_window_close (NautilusWindow *window)
{
    g_return_if_fail (NAUTILUS_IS_WINDOW (window));

    nautilus_window_save_geometry (window);
    set_active_slot (window, NULL);

    /* The pad controller hold a reference to the window, creating a cycle.
     * Usually, reference cycles are resolved in dispose(), but GTK removes the
     * controllers in finalize(), so our only option is to manually remove it
     * here before starting the destruction of the window. */
    if (window->pad_controller != NULL)
    {
        gtk_widget_remove_controller (GTK_WIDGET (window),
                                      GTK_EVENT_CONTROLLER (window->pad_controller));
        g_clear_weak_pointer (&window->pad_controller);
    }

    gtk_window_destroy (GTK_WINDOW (window));
}

void
set_active_slot (NautilusWindow     *window,
                 NautilusWindowSlot *new_slot)
{
    NautilusWindowSlot *old_slot = window->active_slot;

    g_return_if_fail (new_slot == NULL ||
                      gtk_widget_is_ancestor (GTK_WIDGET (new_slot), GTK_WIDGET (window)));

    if (old_slot == new_slot)
    {
        return;
    }

    g_debug ("Setting new slot %p as active, old slot inactive %p", new_slot, old_slot);

    /* make old slot inactive if it exists (may be NULL after init, for example) */
    if (old_slot != NULL)
    {
        /* inform slot & view */
        nautilus_window_slot_set_active (old_slot, FALSE);
    }

    g_set_weak_pointer (&window->active_slot, new_slot);

    /* make new slot active, if it exists */
    if (new_slot)
    {
        set_active_tile (window, nautilus_tile_for_widget (GTK_WIDGET (new_slot)));

        AdwTabPage *page = adw_tab_view_get_page (window->tab_view,
                                                  GTK_WIDGET (new_slot));
        adw_tab_view_set_selected_page (window->tab_view, page);

        /* inform slot & view */
        nautilus_window_slot_set_active (new_slot, TRUE);

        on_location_changed (window);
        update_cursor (window);
    }

    g_object_notify_by_pspec (G_OBJECT (window), properties[PROP_ACTIVE_SLOT]);
}

static void
nautilus_window_realize (GtkWidget *widget)
{
    GTK_WIDGET_CLASS (nautilus_window_parent_class)->realize (widget);
    update_cursor (NAUTILUS_WINDOW (widget));
}

static gboolean
nautilus_window_key_bubble (GtkEventControllerKey *controller,
                            unsigned int           keyval,
                            unsigned int           keycode,
                            GdkModifierType        state,
                            gpointer               user_data)
{
    GtkWidget *widget;
    NautilusWindow *window;

    widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
    window = NAUTILUS_WINDOW (widget);
    /* Quick Look's keys stay in Quick Look: no search behind it. */
    if (nautilus_quick_look_has_keys (GTK_WINDOW (window)))
    {
        return GDK_EVENT_PROPAGATE;
    }
    if (window->active_slot != NULL &&
        nautilus_window_slot_handle_event (window->active_slot, controller, keyval, state))
    {
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

/**
 * nautilus_window_show:
 * @widget: GtkWidget
 *
 * Call parent and then show/hide window items
 * base on user prefs.
 */
static void
nautilus_window_show (GtkWidget *widget)
{
    GTK_WIDGET_CLASS (nautilus_window_parent_class)->show (widget);
}

gboolean
nautilus_window_has_open_location (NautilusWindow *self,
                                   GFile          *location)
{
    g_return_val_if_fail (NAUTILUS_IS_WINDOW (self), FALSE);
    g_return_val_if_fail (location != NULL, FALSE);

    return get_slot_with_open_location (self, location) != NULL;
}

GFile *
nautilus_window_get_active_location (NautilusWindow *self)
{
    g_return_val_if_fail (NAUTILUS_IS_WINDOW (self), NULL);
    g_return_val_if_fail (self->active_slot != NULL, NULL);

    GFile *location = nautilus_window_slot_get_location (self->active_slot);

    if (location == NULL)
    {
        location = nautilus_window_slot_get_pending_location (self->active_slot);
    }

    return location;
}

GList *
nautilus_window_get_locations (NautilusWindow *self)
{
    g_return_val_if_fail (NAUTILUS_IS_WINDOW (self), NULL);

    GFileList *locations = NULL;

    for (GList *l = self->slots; l != NULL; l = l->next)
    {
        NautilusWindowSlot *slot = l->data;
        GFile *location = nautilus_window_slot_get_location (slot);

        if (location != NULL)
        {
            locations = g_list_prepend (locations, location);
        }
    }

    return locations;
}

static void
on_is_maximized_changed (GObject    *object,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
    gboolean is_maximized;

    is_maximized = gtk_window_is_maximized (GTK_WINDOW (object));

    g_settings_set_boolean (nautilus_window_state,
                            NAUTILUS_WINDOW_STATE_MAXIMIZED, is_maximized);
}

static gboolean
nautilus_window_close_request (GtkWindow *window)
{
    nautilus_window_close (NAUTILUS_WINDOW (window));

    /* Window got destroyed, don't call other GtkWindow::close-request handlers */
    return FALSE;
}

static void
nautilus_window_back_or_forward (NautilusWindow *window,
                                 gboolean        back,
                                 guint           distance)
{
    if (window->active_slot != NULL)
    {
        nautilus_window_slot_back_or_forward (window->active_slot, back, distance);
    }
}

void
nautilus_window_back_or_forward_in_new_tab (NautilusWindow              *window,
                                            NautilusNavigationDirection  direction)
{
    NautilusNavigationState *state;

    state = nautilus_window_slot_get_navigation_state (window->active_slot);

    /* Manually fix up the back / forward lists and location.
     * This way we don't have to unnecessary load the current location
     * and then load back / forward */
    switch (direction)
    {
        case NAUTILUS_NAVIGATION_DIRECTION_BACK:
        {
            state->forward_list = g_list_prepend (state->forward_list, state->current_location_bookmark);
            state->current_location_bookmark = state->back_list->data;
            state->back_list = state->back_list->next;
            g_clear_object (&state->current_search_query);
        }
        break;

        case NAUTILUS_NAVIGATION_DIRECTION_FORWARD:
        {
            state->back_list = g_list_prepend (state->back_list, state->current_location_bookmark);
            state->current_location_bookmark = state->forward_list->data;
            state->forward_list = state->forward_list->next;
            g_clear_object (&state->current_search_query);
        }
        break;

        default:
        {
            g_assert_not_reached ();
        }
    }

    NautilusWindowSlot *new_slot = nautilus_window_create_and_init_slot (window);

    GFile *location = nautilus_bookmark_get_location (state->current_location_bookmark);
    nautilus_window_slot_open_location_full (new_slot, location, NULL);
    nautilus_window_slot_restore_navigation_state (new_slot, state);

    free_navigation_state (state);
}

static void
on_click_gesture_pressed (GtkGestureClick *gesture,
                          gint             n_press,
                          gdouble          x,
                          gdouble          y,
                          gpointer         user_data)
{
    GtkWidget *widget;
    NautilusWindow *window;
    guint button;

    widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    window = NAUTILUS_WINDOW (widget);
    button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

    if (nautilus_global_preferences_get_use_extra_buttons () &&
        (button == nautilus_global_preferences_get_back_button ()))
    {
        nautilus_window_back_or_forward (window, TRUE, 0);
    }
    else if (nautilus_global_preferences_get_use_extra_buttons () &&
             (button == nautilus_global_preferences_get_forward_button ()))
    {
        nautilus_window_back_or_forward (window, FALSE, 0);
    }
}

/* ---------------------------------------------------------- Inspector
 *
 * Docked beside (or under) the tiles, in a paned that remembers its size.
 */

typedef enum
{
    INSPECTOR_RIGHT,
    INSPECTOR_LEFT,
    INSPECTOR_BOTTOM,
    INSPECTOR_TOP,
    INSPECTOR_WINDOW,
} InspectorSide;

static gboolean
inspector_docked (NautilusWindow *window)
{
    return window->inspector != NULL &&
           gtk_widget_get_parent (window->inspector) == GTK_WIDGET (window->inspector_paned);
}

static gboolean
save_inspector_size (gpointer user_data)
{
    NautilusWindow *window = user_data;
    GtkPaned *paned = window->inspector_paned;
    int total = gtk_orientable_get_orientation (GTK_ORIENTABLE (paned)) == GTK_ORIENTATION_HORIZONTAL
                ? gtk_widget_get_width (GTK_WIDGET (paned))
                : gtk_widget_get_height (GTK_WIDGET (paned));
    int position = gtk_paned_get_position (paned);
    int size = gtk_paned_get_start_child (paned) == window->inspector ? position : total - position;

    window->inspector_save_id = 0;
    if (size > 80 && inspector_docked (window))
    {
        g_settings_set_int (nautilus_flier_settings (), "inspector-size", size);
    }

    return G_SOURCE_REMOVE;
}

static void
on_inspector_position (NautilusWindow *window)
{
    if (window->inspector_placed && inspector_docked (window) &&
        gtk_widget_get_visible (window->inspector))
    {
        g_clear_handle_id (&window->inspector_save_id, g_source_remove);
        window->inspector_save_id = g_timeout_add (400, save_inspector_size, window);
    }
}

/* Puts the divider where the saved size says, once the paned has a size. */
static void
place_inspector (NautilusWindow *window)
{
    GtkPaned *paned = window->inspector_paned;
    int size = g_settings_get_int (nautilus_flier_settings (), "inspector-size");
    int total = gtk_orientable_get_orientation (GTK_ORIENTABLE (paned)) == GTK_ORIENTATION_HORIZONTAL
                ? gtk_widget_get_width (GTK_WIDGET (paned))
                : gtk_widget_get_height (GTK_WIDGET (paned));

    if (total <= 0 || !inspector_docked (window) || !gtk_widget_get_visible (window->inspector))
    {
        return;
    }

    window->inspector_placed = FALSE;
    gtk_paned_set_position (paned, gtk_paned_get_start_child (paned) == window->inspector
                            ? size : MAX (total - size, total / 3));
    window->inspector_placed = TRUE;
}

static void
on_paned_max_position (NautilusWindow *window)
{
    if (!window->inspector_placed)
    {
        place_inspector (window);
    }
}

/* The popped-out inspector forwards its buttons to this window's file view. */
static void
forward_view_action (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       user_data)
{
    g_autofree char *name = g_strconcat ("view.", g_action_get_name (G_ACTION (action)), NULL);

    gtk_widget_activate_action_variant (GTK_WIDGET (user_data), name, parameter);
}

static gboolean
on_inspector_window_close (GtkWindow      *inspector_window,
                           NautilusWindow *window)
{
    /* Closing the floating inspector docks it again. */
    g_settings_set_enum (nautilus_flier_settings (), "inspector-side", window->inspector_docked_side);
    return TRUE;
}

static void
pop_out_inspector (NautilusWindow *window)
{
    static const GActionEntry view_entries[] =
    {
        { .name = "preview-selection", .activate = forward_view_action },
        { .name = "open-with-default-application", .activate = forward_view_action },
        { .name = "edit-tags", .activate = forward_view_action },
    };
    GtkWidget *content;

    if (window->inspector_window == NULL)
    {
        g_autoptr (GSimpleActionGroup) view_actions = g_simple_action_group_new ();
        int width = MAX (g_settings_get_int (nautilus_flier_settings (), "inspector-size"), 300);

        content = adw_toolbar_view_new ();
        window->inspector_window = adw_window_new ();
        gtk_window_set_title (GTK_WINDOW (window->inspector_window), _("Inspector"));
        gtk_window_set_transient_for (GTK_WINDOW (window->inspector_window), GTK_WINDOW (window));
        gtk_window_set_default_size (GTK_WINDOW (window->inspector_window), width,
                                     MAX (gtk_widget_get_height (GTK_WIDGET (window)) * 4 / 5, 480));
        g_action_map_add_action_entries (G_ACTION_MAP (view_actions), view_entries,
                                         G_N_ELEMENTS (view_entries), window);
        gtk_widget_insert_action_group (window->inspector_window, "view", G_ACTION_GROUP (view_actions));
        gtk_widget_insert_action_group (window->inspector_window, "win", G_ACTION_GROUP (window));
        g_signal_connect (window->inspector_window, "close-request",
                          G_CALLBACK (on_inspector_window_close), window);
        adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (content), adw_header_bar_new ());
        adw_window_set_content (ADW_WINDOW (window->inspector_window), content);
    }
    content = adw_window_get_content (ADW_WINDOW (window->inspector_window));

    if (!inspector_docked (window) && gtk_widget_get_parent (window->inspector) != NULL)
    {
        return;
    }
    g_object_ref (window->inspector);
    if (inspector_docked (window))
    {
        if (gtk_paned_get_start_child (window->inspector_paned) == window->inspector)
        {
            gtk_paned_set_start_child (window->inspector_paned, NULL);
        }
        else
        {
            gtk_paned_set_end_child (window->inspector_paned, NULL);
        }
    }
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (content), window->inspector);
    g_object_unref (window->inspector);
    nautilus_inspector_set_detached (NAUTILUS_INSPECTOR (window->inspector), TRUE);
}

static void
dock_inspector (NautilusWindow *window)
{
    if (window->inspector_window == NULL)
    {
        return;
    }
    g_object_ref (window->inspector);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (adw_window_get_content (ADW_WINDOW (window->inspector_window))),
                                  NULL);
    gtk_window_destroy (GTK_WINDOW (window->inspector_window));
    window->inspector_window = NULL;
    nautilus_inspector_set_detached (NAUTILUS_INSPECTOR (window->inspector), FALSE);
    /* Floating; arrange_inspector() puts it into the paned and drops this ref. */
}

static void
arrange_inspector (NautilusWindow *window)
{
    GSettings *flier = nautilus_flier_settings ();
    InspectorSide side = g_settings_get_enum (flier, "inspector-side");
    gboolean visible = g_settings_get_boolean (flier, "inspector-visible");
    GtkPaned *paned = window->inspector_paned;
    GtkWidget *layout = GTK_WIDGET (window->tile_layout);
    gboolean first = side == INSPECTOR_LEFT || side == INSPECTOR_TOP;
    gboolean was_floating = window->inspector_window != NULL;
    GtkWidget *start;
    GtkWidget *end;

    if (window->inspector == NULL)
    {
        return;
    }

    if (side == INSPECTOR_WINDOW)
    {
        pop_out_inspector (window);
        gtk_widget_set_visible (window->inspector, TRUE);
        gtk_widget_set_visible (window->inspector_window, visible);
        if (visible)
        {
            gtk_window_present (GTK_WINDOW (window->inspector_window));
        }
        return;
    }

    window->inspector_docked_side = side;
    if (was_floating)
    {
        dock_inspector (window);
    }
    else
    {
        g_object_ref (window->inspector);
    }

    start = first ? window->inspector : layout;
    end = first ? layout : window->inspector;
    g_object_ref (layout);
    gtk_paned_set_start_child (paned, NULL);
    gtk_paned_set_end_child (paned, NULL);
    gtk_orientable_set_orientation (GTK_ORIENTABLE (paned),
                                    side == INSPECTOR_BOTTOM || side == INSPECTOR_TOP
                                    ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child (paned, start);
    gtk_paned_set_end_child (paned, end);
    gtk_paned_set_resize_start_child (paned, start == layout);
    gtk_paned_set_resize_end_child (paned, end == layout);
    gtk_paned_set_shrink_start_child (paned, FALSE);
    gtk_paned_set_shrink_end_child (paned, FALSE);
    g_object_unref (layout);
    g_object_unref (window->inspector);

    gtk_widget_set_visible (window->inspector, visible);
    window->inspector_placed = FALSE;
    place_inspector (window);
}

static void
action_inspector_pop_out (GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       user_data)
{
    NautilusWindow *window = user_data;
    GSettings *flier = nautilus_flier_settings ();

    if (g_settings_get_enum (flier, "inspector-side") == INSPECTOR_WINDOW)
    {
        g_settings_set_enum (flier, "inspector-side", window->inspector_docked_side);
    }
    else
    {
        g_settings_set_enum (flier, "inspector-side", INSPECTOR_WINDOW);
    }
    g_settings_set_boolean (flier, "inspector-visible", TRUE);
}

/* Dragging the inspector's grip: the side nearest the pointer gets a
 * highlight, and letting go out of the window pops the inspector out. */
static int
inspector_drop_target (NautilusWindow *window,
                       double          x,
                       double          y)
{
    GtkWidget *overlay = window->inspector_overlay;
    double w = gtk_widget_get_width (overlay), h = gtk_widget_get_height (overlay);
    graphene_point_t in_window;
    double left, right, top, bottom, best;
    int side;

    if (gtk_widget_compute_point (overlay, GTK_WIDGET (window), &GRAPHENE_POINT_INIT (x, y), &in_window) &&
        (in_window.x < -8 || in_window.y < -8 ||
         in_window.x > gtk_widget_get_width (GTK_WIDGET (window)) + 8 ||
         in_window.y > gtk_widget_get_height (GTK_WIDGET (window)) + 8))
    {
        return INSPECTOR_WINDOW;
    }

    left = x; right = w - x; top = y; bottom = h - y;
    best = left; side = INSPECTOR_LEFT;
    if (right < best) { best = right; side = INSPECTOR_RIGHT; }
    if (top < best) { best = top; side = INSPECTOR_TOP; }
    if (bottom < best) { side = INSPECTOR_BOTTOM; }
    return side;
}

static void
show_inspector_drop (NautilusWindow *window,
                     int             target)
{
    GtkWidget *drop = window->inspector_drop;
    double w = gtk_widget_get_width (window->inspector_overlay);
    double h = gtk_widget_get_height (window->inspector_overlay);
    int size = CLAMP (g_settings_get_int (nautilus_flier_settings (), "inspector-size"), 200, 480);
    gboolean vertical = target == INSPECTOR_TOP || target == INSPECTOR_BOTTOM;

    window->inspector_drag_target = target;
    if (target < 0)
    {
        gtk_widget_set_visible (drop, FALSE);
        return;
    }

    gtk_widget_set_halign (drop, target == INSPECTOR_LEFT ? GTK_ALIGN_START
                           : target == INSPECTOR_RIGHT ? GTK_ALIGN_END
                           : target == INSPECTOR_WINDOW ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
    gtk_widget_set_valign (drop, target == INSPECTOR_TOP ? GTK_ALIGN_START
                           : target == INSPECTOR_BOTTOM ? GTK_ALIGN_END
                           : target == INSPECTOR_WINDOW ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
    if (target == INSPECTOR_WINDOW)
    {
        gtk_widget_set_size_request (drop, 260, 120);
    }
    else
    {
        gtk_widget_set_size_request (drop, vertical ? -1 : MIN (size, (int) w / 2),
                                     vertical ? MIN (size, (int) h / 2) : -1);
    }
    gtk_label_set_label (GTK_LABEL (window->inspector_drop_label),
                         target == INSPECTOR_WINDOW ? _("Release to Pop Out") : "");
    gtk_widget_set_visible (drop, TRUE);
}

static void
on_inspector_drag_begin (GtkGestureDrag *gesture,
                         double          x,
                         double          y,
                         NautilusWindow *window)
{
    GtkWidget *grip = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    graphene_point_t point;

    window->inspector_drag_target = -1;
    if (!gtk_widget_compute_point (grip, window->inspector_overlay, &GRAPHENE_POINT_INIT (x, y), &point))
    {
        gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    window->inspector_drag_x = point.x;
    window->inspector_drag_y = point.y;
}

static void
on_inspector_drag_update (GtkGestureDrag *gesture,
                          double          dx,
                          double          dy,
                          NautilusWindow *window)
{
    if (window->inspector_drag_target < 0 && hypot (dx, dy) < 12)
    {
        return;
    }
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_widget_set_cursor_from_name (window->inspector_overlay, "grabbing");
    show_inspector_drop (window, inspector_drop_target (window, window->inspector_drag_x + dx,
                                                        window->inspector_drag_y + dy));
}

static void
on_inspector_drag_end (GtkGestureDrag *gesture,
                       double          dx,
                       double          dy,
                       NautilusWindow *window)
{
    int target = window->inspector_drag_target;

    show_inspector_drop (window, -1);
    gtk_widget_set_cursor (window->inspector_overlay, NULL);
    if (target >= 0)
    {
        g_settings_set_enum (nautilus_flier_settings (), "inspector-side", target);
    }
}

static void
on_inspector_drag_cancel (GtkGesture       *gesture,
                          GdkEventSequence *sequence,
                          NautilusWindow   *window)
{
    show_inspector_drop (window, -1);
    gtk_widget_set_cursor (window->inspector_overlay, NULL);
}

static void
on_active_slot_for_inspector (NautilusWindow *window)
{
    if (window->inspector == NULL)
    {
        return;
    }
    nautilus_inspector_set_slot (NAUTILUS_INSPECTOR (window->inspector), window->active_slot);
}

/* Around the window: right, bottom, left, top. */
static void
move_inspector (gboolean clockwise)
{
    static const InspectorSide order[] = { INSPECTOR_RIGHT, INSPECTOR_BOTTOM, INSPECTOR_LEFT, INSPECTOR_TOP };
    GSettings *flier = nautilus_flier_settings ();
    InspectorSide side = g_settings_get_enum (flier, "inspector-side");
    guint at = 0;

    if (side == INSPECTOR_WINDOW)
    {
        g_settings_set_enum (flier, "inspector-side", INSPECTOR_RIGHT);
        g_settings_set_boolean (flier, "inspector-visible", TRUE);
        return;
    }

    for (guint i = 0; i < G_N_ELEMENTS (order); i++)
    {
        if (order[i] == side)
        {
            at = i;
        }
    }

    at = (at + (clockwise ? 1 : G_N_ELEMENTS (order) - 1)) % G_N_ELEMENTS (order);
    g_settings_set_enum (flier, "inspector-side", order[at]);
    g_settings_set_boolean (flier, "inspector-visible", TRUE);
}

static void
action_inspector_side_next (GSimpleAction *action,
                            GVariant      *parameter,
                            gpointer       user_data)
{
    move_inspector (TRUE);
}

static void
action_inspector_side_previous (GSimpleAction *action,
                                GVariant      *parameter,
                                gpointer       user_data)
{
    move_inspector (FALSE);
}

static void
sync_view_mode (NautilusWindow *window)
{
    GAction *action = g_action_map_lookup_action (G_ACTION_MAP (window), "view-mode");
    guint mode = window->mode_columns != NULL ?
                 nautilus_columns_get_mode (NAUTILUS_COLUMNS (window->mode_columns)) :
                 NAUTILUS_COLUMNS_MODE_LIST;

    g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_uint32 (mode));

    action = g_action_map_lookup_action (G_ACTION_MAP (window), "flat-view");
    g_simple_action_set_state (G_SIMPLE_ACTION (action),
                               g_variant_new_boolean (window->mode_columns != NULL &&
                                                      nautilus_columns_get_flat (NAUTILUS_COLUMNS (window->mode_columns))));
}

static void
on_active_slot_for_view_mode (NautilusWindow *window)
{
    GtkWidget *columns = window->active_slot != NULL ?
                         nautilus_window_slot_get_columns (window->active_slot) : NULL;

    if (columns == window->mode_columns)
    {
        return;
    }

    if (window->mode_columns != NULL)
    {
        g_signal_handlers_disconnect_by_func (window->mode_columns, sync_view_mode, window);
    }
    g_set_weak_pointer (&window->mode_columns, columns);
    if (columns != NULL)
    {
        g_signal_connect_object (columns, "notify::mode", G_CALLBACK (sync_view_mode), window,
                                 G_CONNECT_SWAPPED);
        g_signal_connect_object (columns, "notify::flat", G_CALLBACK (sync_view_mode), window,
                                 G_CONNECT_SWAPPED);
    }
    sync_view_mode (window);
}

static void
action_view_mode (GSimpleAction *action,
                  GVariant      *value,
                  gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);

    if (window->mode_columns != NULL)
    {
        nautilus_columns_set_mode (NAUTILUS_COLUMNS (window->mode_columns),
                                   CLAMP (g_variant_get_uint32 (value),
                                          NAUTILUS_COLUMNS_MODE_LIST, NAUTILUS_COLUMNS_MODE_GRID));
    }
}

static void
action_flat_view (GSimpleAction *action,
                  GVariant      *value,
                  gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);

    if (window->mode_columns != NULL)
    {
        nautilus_columns_set_flat (NAUTILUS_COLUMNS (window->mode_columns), g_variant_get_boolean (value));
    }
}

static void
action_show_tag (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
    NautilusWindow *window = user_data;
    NautilusColumns *columns = window->active_slot != NULL ?
                               NAUTILUS_COLUMNS (nautilus_window_slot_get_columns (window->active_slot)) : NULL;

    if (columns != NULL)
    {
        nautilus_columns_show_tag (columns, g_variant_get_string (parameter, NULL));
    }
}

static void
action_view_step (GSimpleAction *action,
                  GVariant      *value,
                  gpointer       user_data)
{
    NautilusWindow *window = NAUTILUS_WINDOW (user_data);

    if (window->mode_columns != NULL)
    {
        nautilus_columns_step (NAUTILUS_COLUMNS (window->mode_columns), g_variant_get_int32 (value));
    }
}

static void
setup_inspector (NautilusWindow *window)
{
    GtkWidget *layout = GTK_WIDGET (window->tile_layout);
    GtkWidget *parent = gtk_widget_get_parent (layout);
    g_autoptr (GAction) visible = g_settings_create_action (nautilus_flier_settings (), "inspector-visible");
    g_autoptr (GAction) side = g_settings_create_action (nautilus_flier_settings (), "inspector-side");
    g_autoptr (GAction) bottom_bar = g_settings_create_action (nautilus_flier_settings (), "bottom-bar");
    g_autoptr (GAction) hints = g_settings_create_action (nautilus_flier_settings (), "shortcut-hints");
    g_autoptr (GAction) groups = g_settings_create_action (nautilus_flier_settings (), "sidebar-groups");
    static const GActionEntry entries[] =
    {
        { .name = "inspector-side-next", .activate = action_inspector_side_next },
        { .name = "inspector-side-previous", .activate = action_inspector_side_previous },
        { .name = "inspector-pop-out", .activate = action_inspector_pop_out },
        { .name = "view-mode", .parameter_type = "u", .state = "uint32 1", .change_state = action_view_mode },
        { .name = "view-step", .parameter_type = "i", .activate = action_view_step },
        { .name = "show-tag", .parameter_type = "s", .activate = action_show_tag },
        { .name = "flat-view", .state = "false", .change_state = action_flat_view },
    };

    GtkGesture *drag;

    window->inspector = nautilus_inspector_new ();
    window->inspector_docked_side = INSPECTOR_RIGHT;
    window->inspector_drag_target = -1;
    window->inspector_paned = GTK_PANED (gtk_paned_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_paned_set_wide_handle (window->inspector_paned, FALSE);
    gtk_widget_add_css_class (GTK_WIDGET (window->inspector_paned), "inspector-paned");

    /* Where a dragged inspector would land. */
    window->inspector_overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (window->inspector_overlay), GTK_WIDGET (window->inspector_paned));
    window->inspector_drop = adw_bin_new ();
    window->inspector_drop_label = gtk_label_new (NULL);
    adw_bin_set_child (ADW_BIN (window->inspector_drop), window->inspector_drop_label);
    gtk_widget_add_css_class (window->inspector_drop, "inspector-drop");
    gtk_widget_set_can_target (window->inspector_drop, FALSE);
    gtk_widget_set_visible (window->inspector_drop, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (window->inspector_overlay), window->inspector_drop);

    drag = gtk_gesture_drag_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
    g_signal_connect (drag, "drag-begin", G_CALLBACK (on_inspector_drag_begin), window);
    g_signal_connect (drag, "drag-update", G_CALLBACK (on_inspector_drag_update), window);
    g_signal_connect (drag, "drag-end", G_CALLBACK (on_inspector_drag_end), window);
    g_signal_connect (drag, "cancel", G_CALLBACK (on_inspector_drag_cancel), window);
    gtk_widget_add_controller (nautilus_inspector_get_grip (NAUTILUS_INSPECTOR (window->inspector)),
                               GTK_EVENT_CONTROLLER (drag));

    /* In the tiles' place in the template. */
    g_object_ref (layout);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (parent), window->inspector_overlay);
    gtk_paned_set_start_child (window->inspector_paned, layout);
    g_object_unref (layout);
    arrange_inspector (window);

    g_action_map_add_action (G_ACTION_MAP (window), visible);
    g_action_map_add_action (G_ACTION_MAP (window), side);
    g_action_map_add_action (G_ACTION_MAP (window), bottom_bar);
    g_action_map_add_action (G_ACTION_MAP (window), hints);
    g_action_map_add_action (G_ACTION_MAP (window), groups);
    g_action_map_add_action_entries (G_ACTION_MAP (window), entries, G_N_ELEMENTS (entries), window);
    g_signal_connect (window, "notify::active-slot", G_CALLBACK (on_active_slot_for_view_mode), NULL);

    g_signal_connect_object (nautilus_flier_settings (), "changed::inspector-visible",
                             G_CALLBACK (arrange_inspector), window, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::inspector-side",
                             G_CALLBACK (arrange_inspector), window, G_CONNECT_SWAPPED);
    g_signal_connect_swapped (window->inspector_paned, "notify::position",
                              G_CALLBACK (on_inspector_position), window);
    g_signal_connect_swapped (window->inspector_paned, "notify::max-position",
                              G_CALLBACK (on_paned_max_position), window);
    g_signal_connect (window, "notify::active-slot",
                      G_CALLBACK (on_active_slot_for_inspector), NULL);
}

/* Holding Alt (alone) shows the stars of the tile under the pointer. Key
 * presses and releases, not "modifiers": that reports the state from before
 * the key, so it would show them on release and hide them on press. */
static gboolean
is_alt (guint keyval)
{
    return keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R || keyval == GDK_KEY_ISO_Level3_Shift;
}

static gboolean
on_alt_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               user_data)
{
    if (is_alt (keyval))
    {
        nautilus_labels_set_alt_held ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK)) == 0);
    }
    else
    {
        /* Alt plus another key is a shortcut, not looking at stars. */
        nautilus_labels_set_alt_held (FALSE);
    }
    return GDK_EVENT_PROPAGATE;
}

static void
on_alt_released (GtkEventControllerKey *controller,
                 guint                  keyval,
                 guint                  keycode,
                 GdkModifierType        state,
                 gpointer               user_data)
{
    if (is_alt (keyval))
    {
        nautilus_labels_set_alt_held (FALSE);
    }
}

static void
on_window_active_changed (GtkWindow *window)
{
    if (!gtk_window_is_active (window))
    {
        nautilus_labels_set_alt_held (FALSE);
    }
}

/* Density: a style class on the window, so every view in it follows;
 * "density" for the files, "sidebar-density" for the sidebar. */
static void
update_density (NautilusWindow *window)
{
    static const char *nicks[] = { NULL, "compact", "dense", "extra-dense" };
    static const char *keys[] = { "density", "sidebar-density" };
    static const char *prefixes[] = { "density-", "sidebar-density-" };

    for (guint k = 0; k < G_N_ELEMENTS (keys); k++)
    {
        int density = g_settings_get_enum (nautilus_flier_settings (), keys[k]);

        for (guint i = 1; i < G_N_ELEMENTS (nicks); i++)
        {
            g_autofree char *class = g_strconcat (prefixes[k], nicks[i], NULL);

            if ((int) i == density)
            {
                gtk_widget_add_css_class (GTK_WIDGET (window), class);
            }
            else
            {
                gtk_widget_remove_css_class (GTK_WIDGET (window), class);
            }
        }
    }
}

/* The sidebar shows and hides at once, like the inspector. Its split view
 * keeps the animation to itself, so it's caught when it first ends
 * ("done" is emitted even when it's skipped) and its spring stiffened so
 * much (the same damping ratio, 100 times the speed) that it settles
 * within a frame. */
#define SIDEBAR_SPEEDUP 100.0

static gboolean
on_animation_done (GSignalInvocationHint *hint,
                   guint                  n_values,
                   const GValue          *values,
                   gpointer               user_data)
{
    AdwAnimation *animation = g_value_get_object (&values[0]);
    GtkWidget *widget = adw_animation_get_widget (animation);
    static GQuark tuned;

    if (tuned == 0)
    {
        tuned = g_quark_from_static_string ("nautilus-sidebar-tuned");
    }
    if (!ADW_IS_OVERLAY_SPLIT_VIEW (widget) ||
        g_object_get_qdata (G_OBJECT (animation), tuned) != NULL)
    {
        return TRUE;
    }
    g_object_set_qdata (G_OBJECT (animation), tuned, GINT_TO_POINTER (1));

    if (ADW_IS_SPRING_ANIMATION (animation))
    {
        AdwSpringParams *params = adw_spring_animation_get_spring_params (ADW_SPRING_ANIMATION (animation));
        double mass = adw_spring_params_get_mass (params);
        double stiffness = adw_spring_params_get_stiffness (params) * SIDEBAR_SPEEDUP * SIDEBAR_SPEEDUP;
        g_autoptr (AdwSpringParams) faster = adw_spring_params_new (adw_spring_params_get_damping_ratio (params),
                                                                    mass, stiffness);

        adw_spring_animation_set_spring_params (ADW_SPRING_ANIMATION (animation), faster);
    }
    else if (ADW_IS_TIMED_ANIMATION (animation))
    {
        AdwTimedAnimation *timed = ADW_TIMED_ANIMATION (animation);

        adw_timed_animation_set_duration (timed, adw_timed_animation_get_duration (timed) / SIDEBAR_SPEEDUP);
    }
    return TRUE;
}

static void
speed_up_sidebar (NautilusWindow *window)
{
    static gboolean hooked;
    AdwOverlaySplitView *split_view = ADW_OVERLAY_SPLIT_VIEW (window->split_view);
    gboolean shown = adw_overlay_split_view_get_show_sidebar (split_view);

    if (!hooked)
    {
        g_type_ensure (ADW_TYPE_ANIMATION);
        g_signal_add_emission_hook (g_signal_lookup ("done", ADW_TYPE_ANIMATION), 0,
                                    on_animation_done, NULL, NULL);
        hooked = TRUE;
    }
    /* Not mapped yet: these finish at once, and so meet the hook. */
    adw_overlay_split_view_set_show_sidebar (split_view, !shown);
    adw_overlay_split_view_set_show_sidebar (split_view, shown);
}

static void
nautilus_window_init (NautilusWindow *window)
{
    GtkWindowGroup *window_group;
    GtkEventController *controller;
    GtkPadController *pad_controller;

    g_type_ensure (NAUTILUS_TYPE_NETWORK_ADDRESS_BAR);
    g_type_ensure (NAUTILUS_TYPE_TOOLBAR);
    g_type_ensure (NAUTILUS_TYPE_TILE_LAYOUT);
    g_type_ensure (NAUTILUS_TYPE_PLACES_SIDEBAR);
    g_type_ensure (NAUTILUS_TYPE_PROGRESS_INDICATOR);
    g_type_ensure (NAUTILUS_TYPE_SHORTCUT_MANAGER);
    gtk_widget_init_template (GTK_WIDGET (window));
    speed_up_sidebar (window);

    g_signal_connect (window, "notify::maximized",
                      G_CALLBACK (on_is_maximized_changed), NULL);

    window->slots = NULL;
    window->active_slot = NULL;

    gtk_widget_add_css_class (GTK_WIDGET (window), "nautilus-window");

    window_group = gtk_window_group_new ();
    gtk_window_group_add_window (window_group, GTK_WINDOW (window));
    g_object_unref (window_group);

    window->tab_data_queue = g_queue_new ();

    /* Attention: this creates a reference cycle: the pad controller owns a
     * reference to the window (as an action group) and the window (as a widget)
     * owns a reference to the pad controller. To break this, we must remove
     * the controller from the window before destroying the window. But we need
     * to know the controller is still alive before trying to remove it, so a
     * weak reference is added. */
    pad_controller = gtk_pad_controller_new (G_ACTION_GROUP (window), NULL);
    g_set_weak_pointer (&window->pad_controller, pad_controller);
    gtk_pad_controller_set_action_entries (window->pad_controller,
                                           pad_actions, G_N_ELEMENTS (pad_actions));
    gtk_widget_add_controller (GTK_WIDGET (window),
                               GTK_EVENT_CONTROLLER (window->pad_controller));

    controller = GTK_EVENT_CONTROLLER (gtk_gesture_click_new ());
    gtk_widget_add_controller (GTK_WIDGET (window), controller);
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (controller), 0);
    g_signal_connect (controller, "pressed",
                      G_CALLBACK (on_click_gesture_pressed), NULL);

    controller = gtk_event_controller_key_new ();
    gtk_widget_add_controller (GTK_WIDGET (window), controller);
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_BUBBLE);
    g_signal_connect (controller, "key-pressed",
                      G_CALLBACK (nautilus_window_key_bubble), NULL);

    controller = gtk_event_controller_key_new ();
    gtk_widget_add_controller (GTK_WIDGET (window), controller);
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    g_signal_connect (controller, "key-pressed",
                      G_CALLBACK (on_alt_pressed), NULL);
    g_signal_connect (controller, "key-released",
                      G_CALLBACK (on_alt_released), NULL);
    g_signal_connect (window, "notify::is-active",
                      G_CALLBACK (on_window_active_changed), NULL);

    nautilus_quick_look_attach (ADW_APPLICATION_WINDOW (window));
    nautilus_shortcuts_attach (GTK_WINDOW (window));
    setup_inspector (window);

    update_density (window);
    g_signal_connect_object (nautilus_flier_settings (), "changed::density",
                             G_CALLBACK (update_density), window, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-density",
                             G_CALLBACK (update_density), window, G_CONNECT_SWAPPED);
}

static void
nautilus_window_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    NautilusWindow *self = NAUTILUS_WINDOW (object);

    switch (prop_id)
    {
        case PROP_ACTIVE_SLOT:
        {
            g_value_set_object (value, G_OBJECT (self->active_slot));
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_window_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    NautilusWindow *self = NAUTILUS_WINDOW (object);

    switch (prop_id)
    {
        case PROP_ACTIVE_SLOT:
        {
            set_active_slot (self, NAUTILUS_WINDOW_SLOT (g_value_get_object (value)));
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_window_class_init (NautilusWindowClass *class)
{
    GObjectClass *oclass = G_OBJECT_CLASS (class);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS (class);
    GtkWindowClass *winclass = GTK_WINDOW_CLASS (class);

    oclass->dispose = nautilus_window_dispose;
    oclass->finalize = nautilus_window_finalize;
    oclass->constructed = nautilus_window_constructed;
    oclass->get_property = nautilus_window_get_property;
    oclass->set_property = nautilus_window_set_property;

    wclass->show = nautilus_window_show;
    wclass->realize = nautilus_window_realize;
    wclass->grab_focus = nautilus_window_grab_focus;

    winclass->close_request = nautilus_window_close_request;

    properties[PROP_ACTIVE_SLOT] =
        g_param_spec_object ("active-slot",
                             NULL, NULL,
                             NAUTILUS_TYPE_WINDOW_SLOT,
                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (oclass, N_PROPS, properties);

    gtk_widget_class_set_template_from_resource (wclass,
                                                 "/org/gnome/nautilus/ui/nautilus-window.ui");
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, undo_redo_section);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, toolbar);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, split_view);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, places_sidebar);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, toast_overlay);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, tile_layout);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, tab_menu_model);
    gtk_widget_class_bind_template_child (wclass, NautilusWindow, network_address_bar);

    signals[LOCATIONS_CHANGED] =
        g_signal_new ("locations-changed",
                      G_TYPE_FROM_CLASS (class),
                      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                      0,
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
}

NautilusWindow *
nautilus_window_new (void)
{
    return g_object_new (NAUTILUS_TYPE_WINDOW,
                         "icon-name", APPLICATION_ID,
                         NULL);
}

void
nautilus_window_show_about_dialog (NautilusWindow *window)
{
    AdwDialog *dialog;
    g_autofree gchar *module_names = nautilus_module_get_installed_module_names ();
    g_autofree gchar *debug_info = NULL;

    const gchar *designers[] =
    {
        _("The GNOME Project"),
        NULL
    };
    const gchar *developers[] =
    {
        _("The GNOME Project"),
        NULL
    };
    const gchar *documenters[] =
    {
        _("The GNOME Project"),
        "Sun Microsystems",
        NULL
    };

    if (module_names == NULL)
    {
        debug_info = g_strdup (_("No plugins currently installed."));
    }
    else
    {
        debug_info = g_strconcat (_("Currently installed plugins:"), "\n\n",
                                  module_names, "\n\n",
                                  _("For bug testing only, the following command can be used:"), "\n"
                                  "NAUTILUS_DISABLE_PLUGINS=TRUE nautilus", NULL);
    }

    dialog = adw_about_dialog_new_from_appdata ("/org/gnome/nautilus/appdata", NULL);

    adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dialog), VERSION);
    adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (dialog), "org.gnome.Nautilus");
    adw_about_dialog_set_debug_info (ADW_ABOUT_DIALOG (dialog), debug_info);
    adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog), "© 1999 The Files Authors");
    adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (dialog), developers);
    adw_about_dialog_set_designers (ADW_ABOUT_DIALOG (dialog), designers);
    adw_about_dialog_set_documenters (ADW_ABOUT_DIALOG (dialog), documenters);
    adw_about_dialog_set_support_url (ADW_ABOUT_DIALOG (dialog), "https://discourse.gnome.org/tag/nautilus");
    /* Translators should localize the following string which will be displayed at the bottom of
     * the about box to give credit to the translator(s). */
    adw_about_dialog_set_translator_credits (ADW_ABOUT_DIALOG (dialog), _("translator-credits"));

    adw_dialog_present (dialog, window ? GTK_WIDGET (window) : NULL);
}

void
nautilus_window_search (NautilusWindow *window,
                        NautilusQuery  *query)
{
    if (window->active_slot != NULL)
    {
        nautilus_window_slot_search (window->active_slot, query);
    }
    else
    {
        g_warning ("Trying search on a slot but no active slot present");
    }
}
