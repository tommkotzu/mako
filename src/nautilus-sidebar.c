/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Authors : Mr Jamie McCracken (jamiemcc at blueyonder dot co dot uk)
 *           Cosimo Cecchi <cosimoc@gnome.org>
 *           Federico Mena Quintero <federico@gnome.org>
 *           Carlos Soriano <csoriano@gnome.org>
 */
/* This code is originally from Nautilus, was repurposed as a public widget for
 * the file chooser in GTK3, became private with GTK4 and thus reimported into
 * nautilus with the GTK4 port in 7bf5ab0c73d5027dbe754df34243054a83ffae1b
 */
#include "nautilus-sidebar.h"

#include "config.h"
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "nautilus-enum-types.h"

#include <gio/gio.h>
#ifdef HAVE_CLOUDPROVIDERS
#include <cloudproviders.h>
#endif

#include "gdk/gdkkeysyms.h"
#include "nautilus-application.h"
#include "nautilus-bookmark.h"
#include "nautilus-bookmark-list.h"
#include "nautilus-dnd.h"
#include "nautilus-dbus-launcher.h"
#include "nautilus-file.h"
#include "nautilus-file-operations.h"
#include "nautilus-file-utilities.h"
#include "nautilus-global-preferences.h"
#include "nautilus-labels.h"
#include "nautilus-labels-ui.h"
#include "nautilus-properties.h"
#include "nautilus-scheme.h"
#include "nautilus-sidebar-row.h"
#include "nautilus-trash-monitor.h"
#include "nautilus-ui-utilities.h"
#include "nautilus-window-slot.h"
#include "nautilus-window.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#pragma GCC diagnostic ignored "-Wshadow"

/*< private >
 * NautilusSidebar:
 *
 * NautilusSidebar is a widget that displays a list of frequently-used places in the
 * file system:  the user’s home directory, the user’s bookmarks, and volumes and drives.
 * This widget is used as a sidebar in GtkFileChooser and may be used by file managers
 * and similar programs.
 *
 * The places sidebar displays drives and volumes, and will automatically mount
 * or unmount them when the user selects them.
 *
 * Applications can hook to various signals in the places sidebar to customize
 * its behavior.  For example, they can add extra commands to the context menu
 * of the sidebar.
 *
 * While bookmarks are completely in control of the user, the places sidebar also
 * allows individual applications to provide extra shortcut folders that are unique
 * to each application.  For example, a Paint program may want to add a shortcut
 * for a Clipart folder.  You can do this with nautilus_sidebar_add_shortcut().
 *
 * To make use of the places sidebar, an application at least needs to connect
 * to the NautilusSidebar::open-location signal.  This is emitted when the
 * user selects in the sidebar a location to open.  The application should also
 * call nautilus_sidebar_set_location() when it changes the currently-viewed
 * location.
 *
 * # CSS nodes
 *
 * NautilusSidebar uses a single CSS node with name placessidebar and style
 * class .sidebar.
 *
 * Among the children of the places sidebar, the following style classes can
 * be used:
 * - .sidebar-new-bookmark-row for the 'Add new bookmark' row
 * - .sidebar-placeholder-row for a row that is a placeholder
 * - .has-open-popup when a popup is open for a row
 */

/* These are used when a destination-side DND operation is taking place.
 * Normally, when a common drag action is taking place, the state will be
 * DROP_STATE_NEW_BOOKMARK_ARMED, however, if the client of NautilusSidebar
 * wants to show hints about the valid targets, we sill set it as
 * DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT, so the sidebar will show drop hints
 * until the client says otherwise
 */
typedef enum
{
    DROP_STATE_NORMAL,
    DROP_STATE_NEW_BOOKMARK_ARMED,
    DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT,
} DropState;

struct _NautilusSidebar
{
    GtkWidget parent;

    NautilusWindowSlot *window_slot;
    GSignalGroup *slot_signal_group;

    GtkWidget *swin;
    GtkWidget *list_box;
    GtkWidget *new_bookmark_row;
    GtkWidget *stepped_row;     /* weak: where Ctrl+Up/Down last went */
    GtkWidget *color_row;       /* weak: the row a color menu is for */

    NautilusBookmarkList *bookmark_list;

    GActionGroup *row_actions;

#ifdef HAVE_CLOUDPROVIDERS
    CloudProvidersCollector *cloud_manager;
    GList *unready_accounts;
#endif

    GVolumeMonitor *volume_monitor;
    GtkSettings *gtk_settings;
    GFile *current_location;

    GtkWidget *rename_popover;
    GtkWidget *rename_entry;
    GtkWidget *rename_button;
    GtkWidget *rename_error;
    char *rename_uri;

    GtkWidget *trash_row;
    gboolean show_trash;

    /* DND */
    gboolean dragging_over;
    GtkWidget *drag_row;
    int drag_row_height;
    int drag_row_x;
    int drag_row_y;
    GtkWidget *row_placeholder;
    DropState drop_state;
    guint hover_timer_id;
    graphene_point_t hover_start_point;
    GtkListBoxRow *hover_row;

    GtkWidget *popover;
    NautilusSidebarRow *context_row;

    GDBusProxy *hostnamed_proxy;
    GCancellable *hostnamed_cancellable;
    char *hostname;

    NautilusOpenFlags open_flags;

    guint show_desktop           : 1;
};

struct _NautilusSidebarClass
{
    GtkWidgetClass parent_class;

    GdkDragAction (* drag_action_requested)  (NautilusSidebar *sidebar,
                                              GFile           *dest_file,
                                              GSList          *source_file_list);
    GdkDragAction (* drag_action_ask)  (NautilusSidebar *sidebar,
                                        GdkDragAction    actions);
    void (* drag_perform_drop)      (NautilusSidebar *sidebar,
                                     GFile           *dest_file,
                                     GList           *source_file_list,
                                     GdkDragAction    action);

    void (* mount)                  (NautilusSidebar *sidebar,
                                     GMountOperation *mount_operation);
    void (* unmount)                (NautilusSidebar *sidebar,
                                     GMountOperation *unmount_operation);
};

enum
{
    DRAG_ACTION_REQUESTED,
    DRAG_ACTION_ASK,
    DRAG_PERFORM_DROP,
    MOUNT,
    UNMOUNT,
    LAST_SIGNAL
};

enum
{
    PROP_LOCATION = 1,
    PROP_OPEN_FLAGS,
    PROP_WINDOW_SLOT,
    NUM_PROPERTIES
};

/* Names for themed icons */
#define ICON_NAME_HOME     "user-home-symbolic"
#define ICON_NAME_DESKTOP  "user-desktop-symbolic"
#define ICON_NAME_NETWORK_VIEW  "network-computer-symbolic"

static guint places_sidebar_signals[LAST_SIGNAL] = { 0 };
static GParamSpec *properties[NUM_PROPERTIES] = { NULL, };

static gboolean eject_or_unmount_bookmark (NautilusSidebarRow *row);
static gboolean eject_or_unmount_selection (NautilusSidebar *sidebar);
static void  check_unmount_and_eject (GMount   *mount,
                                      GVolume  *volume,
                                      GDrive   *drive,
                                      gboolean *show_unmount,
                                      gboolean *show_eject);
static void on_row_pressed (GtkGestureClick    *gesture,
                            int                 n_press,
                            double              x,
                            double              y,
                            NautilusSidebarRow *row);
static void on_row_released (GtkGestureClick    *gesture,
                             int                 n_press,
                             double              x,
                             double              y,
                             NautilusSidebarRow *row);
static void on_row_dragged (GtkGestureDrag     *gesture,
                            double              x,
                            double              y,
                            NautilusSidebarRow *row);

static void popup_menu_cb (NautilusSidebarRow *row);
static void long_press_cb (GtkGesture      *gesture,
                           double           x,
                           double           y,
                           NautilusSidebar *sidebar);
static void stop_drop_feedback (NautilusSidebar *sidebar);
static GMountOperation * get_mount_operation (NautilusSidebar *sidebar);
static GMountOperation * get_unmount_operation (NautilusSidebar *sidebar);


G_DEFINE_TYPE (NautilusSidebar, nautilus_sidebar, GTK_TYPE_WIDGET);

static void
call_open_location (NautilusSidebar    *self,
                    GFile              *location,
                    NautilusWindowSlot *preferred_slot,
                    NautilusOpenFlags   open_flags)
{
    if ((open_flags & self->open_flags) == 0)
    {
        open_flags = NAUTILUS_OPEN_FLAG_NORMAL;
    }

    if (open_flags == NAUTILUS_OPEN_FLAG_NEW_TAB)
    {
        open_flags |= NAUTILUS_OPEN_FLAG_DONT_MAKE_ACTIVE;
    }

    if (open_flags & (NAUTILUS_OPEN_FLAG_NEW_WINDOW | NAUTILUS_OPEN_FLAG_NEW_TAB))
    {
        nautilus_application_open_location_full (NAUTILUS_APPLICATION (g_application_get_default ()),
                                                 location, open_flags, NULL, NULL);
    }
    else
    {
        if (!preferred_slot)
        {
            nautilus_window_slot_open_location_full (self->window_slot, location, NULL);
        }
        else
        {
            nautilus_window_slot_open_location_full (preferred_slot, location, NULL);
        }

        GtkWidget *ancestor = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_OVERLAY_SPLIT_VIEW);
        AdwOverlaySplitView *split_view = ADW_OVERLAY_SPLIT_VIEW (ancestor);

        g_assert (split_view != NULL);

        if (adw_overlay_split_view_get_collapsed (split_view))
        {
            adw_overlay_split_view_set_show_sidebar (split_view, FALSE);
        }
    }
}

static void
show_error_message (NautilusSidebar *self,
                    const char      *primary,
                    const char      *secondary)
{
    nautilus_show_ok_dialog (primary,
                             secondary,
                             GTK_WIDGET (self));
}

static void
emit_mount_operation (NautilusSidebar *sidebar,
                      GMountOperation *mount_op)
{
    g_signal_emit (sidebar, places_sidebar_signals[MOUNT], 0, mount_op);
}

static void
emit_unmount_operation (NautilusSidebar *sidebar,
                        GMountOperation *mount_op)
{
    g_signal_emit (sidebar, places_sidebar_signals[UNMOUNT], 0, mount_op);
}

static GdkDragAction
emit_drag_action_requested (NautilusSidebar *sidebar,
                            NautilusFile    *dest_file,
                            GSList          *source_file_list)
{
    GdkDragAction ret_action = 0;

    g_signal_emit (sidebar, places_sidebar_signals[DRAG_ACTION_REQUESTED], 0,
                   dest_file, source_file_list, &ret_action);

    return ret_action;
}

static void
emit_drag_perform_drop (NautilusSidebar *sidebar,
                        GFile           *dest_file,
                        GSList          *source_file_list,
                        GdkDragAction    action)
{
    g_signal_emit (sidebar, places_sidebar_signals[DRAG_PERFORM_DROP], 0,
                   dest_file, source_file_list, action);
}
/* ------------------------------------------------------------ Groups */

static void update_places (NautilusSidebar *sidebar);
static gboolean should_show_recent (NautilusSidebar *sidebar);
static char *get_home_directory_uri (void);

/* With the "sidebar-groups" setting, each section gets a header that folds
 * it away, like File Flier's sidebar, and Recent and Starred get groups of
 * their own. Tags are a section either way. */

#define MAX_RECENT_FOLDERS 10

static gboolean
groups_on (void)
{
    return g_settings_get_boolean (nautilus_flier_settings (), "sidebar-groups");
}

static const char *
group_id (NautilusSidebarSectionType section)
{
    switch (section)
    {
        case NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS: return "places";
        case NAUTILUS_SIDEBAR_SECTION_RECENT: return "recent";
        case NAUTILUS_SIDEBAR_SECTION_BOOKMARKS: return "bookmarks";
        case NAUTILUS_SIDEBAR_SECTION_TAGS: return "tags";
        case NAUTILUS_SIDEBAR_SECTION_CLOUD: return "cloud";
        case NAUTILUS_SIDEBAR_SECTION_MOUNTS: return "devices";
        default: return NULL;
    }
}

static const char *
group_title (NautilusSidebarSectionType section)
{
    switch (section)
    {
        case NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS: return _("Places");
        case NAUTILUS_SIDEBAR_SECTION_RECENT: return _("Recent");
        case NAUTILUS_SIDEBAR_SECTION_BOOKMARKS: return _("Bookmarks");
        case NAUTILUS_SIDEBAR_SECTION_TAGS: return _("Tags");
        case NAUTILUS_SIDEBAR_SECTION_CLOUD: return _("Cloud");
        case NAUTILUS_SIDEBAR_SECTION_MOUNTS: return _("Storage");
        default: return "";
    }
}

static const char *
group_icon (NautilusSidebarSectionType section)
{
    switch (section)
    {
        case NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS: return "folder-symbolic";
        case NAUTILUS_SIDEBAR_SECTION_RECENT: return "document-open-recent-symbolic";
        case NAUTILUS_SIDEBAR_SECTION_BOOKMARKS: return "user-bookmarks-symbolic";
        case NAUTILUS_SIDEBAR_SECTION_TAGS: return "media-record-symbolic";
        case NAUTILUS_SIDEBAR_SECTION_CLOUD: return "folder-remote-symbolic";
        case NAUTILUS_SIDEBAR_SECTION_MOUNTS: return "drive-harddisk-symbolic";
        default: return NULL;
    }
}

static const NautilusSidebarSectionType all_groups[] =
{
    NAUTILUS_SIDEBAR_SECTION_BOOKMARKS, NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS,
    NAUTILUS_SIDEBAR_SECTION_CLOUD, NAUTILUS_SIDEBAR_SECTION_MOUNTS,
    NAUTILUS_SIDEBAR_SECTION_RECENT, NAUTILUS_SIDEBAR_SECTION_TAGS,
};

/* The groups in the user's order (sidebar-group-order); ones it doesn't
 * name go after, in File Flier's order. Returns a new array of ids. */
static GPtrArray *
group_order (void)
{
    g_auto (GStrv) order = g_settings_get_strv (nautilus_flier_settings (), "sidebar-group-order");
    GPtrArray *ids = g_ptr_array_new ();

    for (guint i = 0; order[i] != NULL; i++)
    {
        for (guint g = 0; g < G_N_ELEMENTS (all_groups); g++)
        {
            const char *id = group_id (all_groups[g]);

            if (g_str_equal (order[i], id) && !g_ptr_array_find_with_equal_func (ids, id, g_str_equal, NULL))
            {
                g_ptr_array_add (ids, (gpointer) id);
            }
        }
    }
    for (guint g = 0; g < G_N_ELEMENTS (all_groups); g++)
    {
        const char *id = group_id (all_groups[g]);

        if (!g_ptr_array_find_with_equal_func (ids, id, g_str_equal, NULL))
        {
            g_ptr_array_add (ids, (gpointer) id);
        }
    }

    return ids;
}

static int
group_rank (NautilusSidebarSectionType section)
{
    g_autoptr (GPtrArray) ids = group_order ();
    guint index;

    return g_ptr_array_find_with_equal_func (ids, group_id (section), g_str_equal, &index) ? (int) index : 99;
}

/* Puts group @moved before (or, @after, behind) group @target. */
static void
move_group (const char *moved,
            const char *target,
            gboolean    after)
{
    g_autoptr (GPtrArray) ids = group_order ();
    guint from, to;

    if (g_strcmp0 (moved, target) == 0 ||
        !g_ptr_array_find_with_equal_func (ids, moved, g_str_equal, &from) ||
        !g_ptr_array_find_with_equal_func (ids, target, g_str_equal, NULL))
    {
        return;
    }
    g_ptr_array_remove_index (ids, from);
    g_ptr_array_find_with_equal_func (ids, target, g_str_equal, &to);
    g_ptr_array_insert (ids, after ? (int) to + 1 : (int) to, (gpointer) moved);
    g_ptr_array_add (ids, NULL);
    g_settings_set_strv (nautilus_flier_settings (), "sidebar-group-order", (const char * const *) ids->pdata);
}

static int compare_group_ids (const char *a, const char *b, gpointer user_data);

/* Moves group @id one place up or down among the groups that are shown. */
static void
step_group (NautilusSidebar *sidebar,
            const char      *id,
            int              step)
{
    g_autoptr (GPtrArray) shown = g_ptr_array_new_with_free_func (g_free);

    for (GtkWidget *child = gtk_widget_get_first_child (sidebar->list_box); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        NautilusSidebarRowType place_type;
        NautilusSidebarSectionType section;

        if (NAUTILUS_IS_SIDEBAR_ROW (child))
        {
            g_object_get (child, "place-type", &place_type, "section-type", &section, NULL);
            if (place_type == NAUTILUS_SIDEBAR_ROW_GROUP_HEADER)
            {
                g_ptr_array_add (shown, g_strdup (group_id (section)));
            }
        }
    }

    /* The children are in insertion order: sort them by rank. */
    g_ptr_array_sort_values_with_data (shown, (GCompareDataFunc) compare_group_ids, NULL);

    guint at;

    if (g_ptr_array_find_with_equal_func (shown, id, g_str_equal, &at) &&
        (int) at + step >= 0 && at + step < shown->len)
    {
        move_group (id, g_ptr_array_index (shown, at + step), step > 0);
    }
}

static NautilusSidebarSectionType
group_from_id (const char *id)
{
    for (guint g = 0; g < G_N_ELEMENTS (all_groups); g++)
    {
        if (g_strcmp0 (group_id (all_groups[g]), id) == 0)
        {
            return all_groups[g];
        }
    }
    return NAUTILUS_SIDEBAR_SECTION_INVALID;
}

static int
compare_group_ids (const char *a,
                   const char *b,
                   gpointer    user_data)
{
    return group_rank (group_from_id (a)) - group_rank (group_from_id (b));
}

static gboolean
group_collapsed (NautilusSidebarSectionType section)
{
    g_auto (GStrv) collapsed = g_settings_get_strv (nautilus_flier_settings (), "sidebar-collapsed");
    const char *id = group_id (section);

    return id != NULL && g_strv_contains ((const char * const *) collapsed, id);
}

static void
toggle_group (NautilusSidebarSectionType section)
{
    g_auto (GStrv) collapsed = g_settings_get_strv (nautilus_flier_settings (), "sidebar-collapsed");
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
    const char *id = group_id (section);
    gboolean was = FALSE;

    for (guint i = 0; collapsed[i] != NULL; i++)
    {
        if (g_strcmp0 (collapsed[i], id) == 0)
        {
            was = TRUE;
        }
        else
        {
            g_strv_builder_add (builder, collapsed[i]);
        }
    }
    if (!was && id != NULL)
    {
        g_strv_builder_add (builder, id);
    }

    g_auto (GStrv) result = g_strv_builder_end (builder);
    g_settings_set_strv (nautilus_flier_settings (), "sidebar-collapsed", (const char * const *) result);
}

static gboolean
group_hidden (NautilusSidebarSectionType section)
{
    g_auto (GStrv) hidden = g_settings_get_strv (nautilus_flier_settings (), "sidebar-hidden-groups");
    const char *id = group_id (section);

    return id != NULL && g_strv_contains ((const char * const *) hidden, id);
}

/* "sections.ID": one boolean action per group, true while it is shown, so
 * the right-click menus can switch groups on and off. */
static void
on_section_toggled (GSimpleAction *action,
                    GVariant      *state,
                    gpointer       user_data)
{
    GSettings *flier = nautilus_flier_settings ();
    g_auto (GStrv) hidden = g_settings_get_strv (flier, "sidebar-hidden-groups");
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
    const char *id = g_action_get_name (G_ACTION (action));

    for (guint i = 0; hidden[i] != NULL; i++)
    {
        if (!g_str_equal (hidden[i], id))
        {
            g_strv_builder_add (builder, hidden[i]);
        }
    }
    if (!g_variant_get_boolean (state))
    {
        g_strv_builder_add (builder, id);
    }

    g_auto (GStrv) result = g_strv_builder_end (builder);
    g_settings_set_strv (flier, "sidebar-hidden-groups", (const char * const *) result);
}

static void
sync_section_actions (GActionMap *map)
{
    for (guint g = 0; g < G_N_ELEMENTS (all_groups); g++)
    {
        GAction *action = g_action_map_lookup_action (map, group_id (all_groups[g]));

        if (action != NULL)
        {
            g_simple_action_set_state (G_SIMPLE_ACTION (action),
                                       g_variant_new_boolean (!group_hidden (all_groups[g])));
        }
    }
}

static void
install_section_actions (GtkWidget *sidebar)
{
    g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();

    for (guint g = 0; g < G_N_ELEMENTS (all_groups); g++)
    {
        g_autoptr (GSimpleAction) action = g_simple_action_new_stateful (group_id (all_groups[g]), NULL,
                                                                         g_variant_new_boolean (TRUE));

        g_signal_connect (action, "change-state", G_CALLBACK (on_section_toggled), NULL);
        g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (action));
    }
    sync_section_actions (G_ACTION_MAP (group));
    {
        g_autoptr (GAction) density = g_settings_create_action (nautilus_flier_settings (), "sidebar-density");

        g_action_map_add_action (G_ACTION_MAP (group), density);
    }
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-hidden-groups",
                             G_CALLBACK (sync_section_actions), group, G_CONNECT_SWAPPED);
    gtk_widget_insert_action_group (sidebar, "sections", G_ACTION_GROUP (group));
}

/* The groups as check items, in the sidebar's order. */
static GMenu *
sections_menu_new (void)
{
    GMenu *menu = g_menu_new ();
    g_autoptr (GPtrArray) ids = group_order ();

    for (guint i = 0; i < ids->len; i++)
    {
        for (guint g = 0; g < G_N_ELEMENTS (all_groups); g++)
        {
            if (g_str_equal (group_id (all_groups[g]), g_ptr_array_index (ids, i)))
            {
                g_autofree char *action = g_strdup_printf ("sections.%s", group_id (all_groups[g]));

                g_menu_append (menu, all_groups[g] == NAUTILUS_SIDEBAR_SECTION_RECENT ? _("Recent Folders")
                                                                                       : group_title (all_groups[g]),
                               action);
            }
        }
    }
    return menu;
}

static void
append_sections_section (GMenu *menu)
{
    g_autoptr (GMenu) sections = sections_menu_new ();
    g_autoptr (GMenu) density = g_menu_new ();
    g_autoptr (GMenu) last = g_menu_new ();

    g_menu_append_section (menu, _("Show in Sidebar"), G_MENU_MODEL (sections));
    g_menu_append (density, _("Comfortable"), "sections.sidebar-density('comfortable')");
    g_menu_append (density, _("Compact"), "sections.sidebar-density('compact')");
    g_menu_append (density, _("Dense"), "sections.sidebar-density('dense')");
    g_menu_append (density, _("Extra Dense"), "sections.sidebar-density('extra-dense')");
    g_menu_append_submenu (last, _("Sidebar _Density"), G_MENU_MODEL (density));
    g_menu_append_section (menu, NULL, G_MENU_MODEL (last));
}

static gboolean path_is_home_dir (const char *path);

static gboolean
place_hidden (const char *id)
{
    g_auto (GStrv) hidden = g_settings_get_strv (nautilus_flier_settings (), "sidebar-hidden-places");

    return g_strv_contains ((const char * const *) hidden, id);
}

/* The user folders Places holds, after Home. */
static const struct
{
    const char *id;
    GUserDirectory directory;
    const char *icon;
} user_folders[] =
{
    { "downloads", G_USER_DIRECTORY_DOWNLOAD, "folder-download-symbolic" },
    { "documents", G_USER_DIRECTORY_DOCUMENTS, "folder-documents-symbolic" },
    { "pictures", G_USER_DIRECTORY_PICTURES, "folder-pictures-symbolic" },
    { "videos", G_USER_DIRECTORY_VIDEOS, "folder-videos-symbolic" },
    { "music", G_USER_DIRECTORY_MUSIC, "folder-music-symbolic" },
};

/* The uri of a user folder Places shows, or NULL. */
static char *
user_folder_uri (guint i)
{
    const char *path = g_get_user_special_dir (user_folders[i].directory);

    if (path == NULL || path_is_home_dir (path) || place_hidden (user_folders[i].id))
    {
        return NULL;
    }
    return g_filename_to_uri (path, NULL, NULL);
}

static gboolean
is_user_folder_uri (const char *uri)
{
    for (guint i = 0; i < G_N_ELEMENTS (user_folders); i++)
    {
        g_autofree char *folder = user_folder_uri (i);

        if (folder != NULL && g_strcmp0 (folder, uri) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
list_box_filter_func (GtkListBoxRow *row,
                      gpointer       user_data)
{
    NautilusSidebarSectionType section;
    NautilusSidebarRowType place_type;

    if (!NAUTILUS_IS_SIDEBAR_ROW (row))
    {
        return TRUE;
    }
    g_object_get (row, "section-type", &section, "place-type", &place_type, NULL);

    /* Switched off in Preferences: not shown at all. */
    if (group_hidden (section))
    {
        return FALSE;
    }
    if (!groups_on ())
    {
        return TRUE;
    }

    return place_type == NAUTILUS_SIDEBAR_ROW_GROUP_HEADER || !group_collapsed (section);
}

static void
update_group_arrows (NautilusSidebar *sidebar)
{
    for (GtkWidget *child = gtk_widget_get_first_child (sidebar->list_box); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        NautilusSidebarSectionType section;
        NautilusSidebarRowType place_type;

        if (!NAUTILUS_IS_SIDEBAR_ROW (child))
        {
            continue;
        }
        g_object_get (child, "section-type", &section, "place-type", &place_type, NULL);
        if (place_type == NAUTILUS_SIDEBAR_ROW_GROUP_HEADER)
        {
            g_autoptr (GIcon) arrow = g_themed_icon_new (group_collapsed (section) ? "pan-end-symbolic"
                                                                                : "pan-down-symbolic");

            /* The property, which also shows the icon (the setter doesn't). */
            g_object_set (child, "end-icon", arrow, NULL);
        }
    }
}

static void
on_collapsed_changed (NautilusSidebar *sidebar)
{
    update_group_arrows (sidebar);
    gtk_list_box_invalidate_filter (GTK_LIST_BOX (sidebar->list_box));
}

/* Colors for the tag dots: one rule per tag row. */
static void
update_tag_css (GString *css)
{
    static GtkCssProvider *provider = NULL;

    if (provider == NULL)
    {
        provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string (provider, css->str);
}

static void
on_recent_changed (NautilusSidebar *sidebar)
{
    if (groups_on ())
    {
        update_places (sidebar);
    }
}

static void
on_tags_changed (NautilusSidebar *sidebar)
{
    update_places (sidebar);
}

/* Dragging a group's header onto another moves the group there. */
#define GROUP_DRAG_PREFIX "nautilus-sidebar-group:"

static GdkContentProvider *
on_group_drag_prepare (GtkDragSource *source,
                       double         x,
                       double         y,
                       GtkWidget     *header)
{
    NautilusSidebarSectionType section;
    g_autofree char *text = NULL;

    g_object_get (header, "section-type", &section, NULL);
    text = g_strconcat (GROUP_DRAG_PREFIX, group_id (section), NULL);
    return gdk_content_provider_new_typed (G_TYPE_STRING, text);
}

static void
on_group_drag_begin (GtkDragSource *source,
                     GdkDrag       *drag,
                     GtkWidget     *header)
{
    g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (header);

    gtk_drag_source_set_icon (source, paintable, 12, 12);
}

static gboolean
on_group_drop (GtkDropTarget *target,
               const GValue  *value,
               double         x,
               double         y,
               GtkWidget     *header)
{
    const char *text = G_VALUE_HOLDS_STRING (value) ? g_value_get_string (value) : NULL;
    NautilusSidebarSectionType section;

    if (text == NULL || !g_str_has_prefix (text, GROUP_DRAG_PREFIX))
    {
        return FALSE;
    }
    g_object_get (header, "section-type", &section, NULL);

    const char *moved = text + strlen (GROUP_DRAG_PREFIX);
    /* Down the list: behind the target; up: before it. */
    gboolean after = group_rank (group_from_id (moved)) < group_rank (section);

    move_group (moved, group_id (section), after);
    return TRUE;
}

static void
on_group_move (GSimpleAction *action,
               GVariant      *parameter,
               gpointer       user_data)
{
    GtkWidget *header = user_data;
    NautilusSidebarSectionType section;
    g_autoptr (NautilusSidebar) sidebar = NULL;

    g_object_get (header, "section-type", &section, "sidebar", &sidebar, NULL);
    step_group (sidebar, group_id (section), g_str_equal (g_action_get_name (G_ACTION (action)), "down") ? 1 : -1);
}

static gboolean
unparent_later (gpointer popover)
{
    gtk_widget_unparent (popover);
    g_object_unref (popover);
    return G_SOURCE_REMOVE;
}

static void
unparent_popover_later (GtkWidget *popover)
{
    /* Later: the item's action runs after the menu closes. */
    g_idle_add (unparent_later, g_object_ref (popover));
}

/* ------------------------------------------------------- Row colors */

/* Any row can have its icon colored from its right-click menu: a slot of
 * the folder color scheme, kept in sidebar-colors by row key. A tag row
 * recolors the tag itself. */

static char *
row_color_key (GtkWidget *row)
{
    NautilusSidebarRowType place_type;
    NautilusSidebarSectionType section;
    g_autofree char *uri = NULL;
    g_autofree char *label = NULL;

    g_object_get (row, "place-type", &place_type, "section-type", &section,
                  "uri", &uri, "label", &label, NULL);
    if (place_type == NAUTILUS_SIDEBAR_ROW_GROUP_HEADER)
    {
        return g_strconcat ("group:", group_id (section), NULL);
    }
    if (uri != NULL)
    {
        return g_steal_pointer (&uri);
    }
    return label != NULL ? g_strconcat ("name:", label, NULL) : NULL;
}

static int
row_color_slot (GVariant  *colors,
                GtkWidget *row)
{
    g_autofree char *key = row_color_key (row);
    int slot;

    if (key == NULL || !g_variant_lookup (colors, key, "i", &slot))
    {
        return -1;
    }
    return slot;
}

/* One rule per slot in the current scheme, and the slot's class on each row. */
static void
update_row_colors (NautilusSidebar *sidebar)
{
    static GtkCssProvider *provider = NULL;
    g_autoptr (GVariant) colors = g_settings_get_value (nautilus_flier_settings (), "sidebar-colors");
    g_autoptr (GString) css = g_string_new (NULL);

    if (provider == NULL)
    {
        provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    for (int i = 0; i < NAUTILUS_FOLDER_COLOR_SLOTS; i++)
    {
        GdkRGBA rgba;
        g_autofree char *color = NULL;

        nautilus_labels_get_slot_color (i, &rgba);
        color = gdk_rgba_to_string (&rgba);
        g_string_append_printf (css, ".navigation-sidebar row.sidebar-color-%d box > image:first-child "
                                     "{ color: %s; opacity: 1; }\n", i, color);
    }
    gtk_css_provider_load_from_string (provider, css->str);

    for (GtkWidget *child = gtk_widget_get_first_child (sidebar->list_box); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        NautilusSidebarRowType place_type;
        int slot;

        if (!NAUTILUS_IS_SIDEBAR_ROW (child))
        {
            continue;
        }
        g_object_get (child, "place-type", &place_type, NULL);
        slot = place_type == NAUTILUS_SIDEBAR_ROW_TAG ? -1 : row_color_slot (colors, child);
        for (int i = 0; i < NAUTILUS_FOLDER_COLOR_SLOTS; i++)
        {
            g_autofree char *class = g_strdup_printf ("sidebar-color-%d", i);

            if (i == slot)
            {
                gtk_widget_add_css_class (child, class);
            }
            else
            {
                gtk_widget_remove_css_class (child, class);
            }
        }
    }
}

static void
on_row_color (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
    NautilusSidebar *sidebar = user_data;
    GtkWidget *row = sidebar->color_row;
    int slot = g_variant_get_int32 (parameter);
    NautilusSidebarRowType place_type;

    if (row == NULL)
    {
        return;
    }
    g_object_get (row, "place-type", &place_type, NULL);

    if (place_type == NAUTILUS_SIDEBAR_ROW_TAG)
    {
        g_autofree char *label = NULL;
        GdkRGBA rgba = { 0.6, 0.6, 0.6, 1 };

        g_object_get (row, "label", &label, NULL);
        if (slot >= 0)
        {
            nautilus_labels_get_slot_color (slot, &rgba);
        }
        nautilus_labels_set_tag_color (label, &rgba);
        return;
    }

    g_autofree char *key = row_color_key (row);
    g_autoptr (GVariant) colors = g_settings_get_value (nautilus_flier_settings (), "sidebar-colors");
    GVariantBuilder builder;
    GVariantIter iter;
    const char *k;
    int v;

    if (key == NULL)
    {
        return;
    }
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{si}"));
    g_variant_iter_init (&iter, colors);
    while (g_variant_iter_next (&iter, "{&si}", &k, &v))
    {
        if (!g_str_equal (k, key))
        {
            g_variant_builder_add (&builder, "{si}", k, v);
        }
    }
    if (slot >= 0)
    {
        g_variant_builder_add (&builder, "{si}", key, slot);
    }
    g_settings_set_value (nautilus_flier_settings (), "sidebar-colors", g_variant_builder_end (&builder));
}

/* Adds the swatches to a menu made from @menu; call before and after. */
static void
append_color_section (GMenu *menu)
{
    g_autoptr (GMenu) section = g_menu_new ();
    g_autoptr (GMenuItem) item = g_menu_item_new (NULL, NULL);

    g_menu_item_set_attribute (item, "custom", "s", "sidebar-color");
    g_menu_append_item (section, item);
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
}

static void
add_color_swatches (GtkWidget *popover,
                    GtkWidget *row)
{
    NautilusSidebar *sidebar = NAUTILUS_PLACES_SIDEBAR (gtk_widget_get_ancestor (row, NAUTILUS_TYPE_PLACES_SIDEBAR));

    if (sidebar != NULL)
    {
        g_set_weak_pointer (&sidebar->color_row, row);
    }
    gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover),
                                nautilus_color_row_new ("row-color.set", FALSE), "sidebar-color");
}

static gboolean
popup_when_built (gpointer data)
{
    GtkWidget *popover = data;

    if (gtk_widget_get_parent (popover) != NULL)
    {
        gtk_popover_popup (GTK_POPOVER (popover));
    }
    g_object_unref (popover);
    return G_SOURCE_REMOVE;
}

/* Pops @popover up at (@x, @y) in @parent: below the pointer in the upper
 * half of @parent and above it in the lower half, as menus there do. */
static void
popup_at_point (GtkWidget *popover,
                GtkWidget *parent,
                double     x,
                double     y)
{
    gtk_widget_set_parent (popover, parent);
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_widget_set_halign (popover, GTK_ALIGN_START);
    gtk_popover_set_position (GTK_POPOVER (popover),
                              y > gtk_widget_get_height (parent) / 2 ? GTK_POS_TOP : GTK_POS_BOTTOM);
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &(GdkRectangle) { (int) x, (int) y, 1, 1 });
    g_signal_connect (popover, "closed", G_CALLBACK (unparent_popover_later), NULL);
    /* A menu shows its section titles and separators from an idle, and
     * popped up before that it is measured without them: too small, so
     * it scrolls. */
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, popup_when_built, g_object_ref (popover), NULL);
}

/* Pops @popover up at (@x, @y) in @row, parented to the sidebar: rows get
 * rebuilt (a tag recolored, a group switched off) while their menu is
 * still open, and a popover must not go down with them. */
static void
popup_on_sidebar (GtkWidget *popover,
                  GtkWidget *row,
                  double     x,
                  double     y)
{
    GtkWidget *sidebar = gtk_widget_get_ancestor (row, NAUTILUS_TYPE_PLACES_SIDEBAR);
    graphene_point_t point = GRAPHENE_POINT_INIT (x, y);

    if (sidebar == NULL)
    {
        sidebar = row;
    }
    if (sidebar != row && !gtk_widget_compute_point (row, sidebar, &GRAPHENE_POINT_INIT (x, y), &point))
    {
        point = GRAPHENE_POINT_INIT (0, 0);
    }
    popup_at_point (popover, sidebar, point.x, point.y);
}

static void
show_tag_menu (GtkWidget *row,
               double     x,
               double     y)
{
    g_autoptr (GMenu) menu = g_menu_new ();
    GtkWidget *popover;

    append_color_section (menu);
    popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    add_color_swatches (popover, row);
    popup_on_sidebar (popover, row, x, y);
}

static void
show_group_menu (GtkWidget *header,
                 double     x,
                 double     y)
{
    static const GActionEntry entries[] =
    {
        { .name = "up", .activate = on_group_move },
        { .name = "down", .activate = on_group_move },
    };
    g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
    g_autoptr (GMenu) menu = g_menu_new ();
    GtkWidget *popover;

    g_action_map_add_action_entries (G_ACTION_MAP (group), entries, G_N_ELEMENTS (entries), header);
    gtk_widget_insert_action_group (header, "group", G_ACTION_GROUP (group));
    g_menu_append (menu, _("Move _Up"), "group.up");
    g_menu_append (menu, _("Move _Down"), "group.down");
    append_color_section (menu);
    append_sections_section (menu);

    popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    gtk_widget_insert_action_group (popover, "group", G_ACTION_GROUP (group));
    add_color_swatches (popover, header);
    popup_on_sidebar (popover, header, x, y);
}

static void
on_order_changed (NautilusSidebar *sidebar)
{
    gtk_list_box_invalidate_filter (GTK_LIST_BOX (sidebar->list_box));
    gtk_list_box_invalidate_sort (GTK_LIST_BOX (sidebar->list_box));
    gtk_list_box_invalidate_headers (GTK_LIST_BOX (sidebar->list_box));
}

static void
on_tag_row_activated (NautilusSidebar    *sidebar,
                      NautilusSidebarRow *row)
{
    g_autofree char *label = NULL;

    g_object_get (row, "label", &label, NULL);
    gtk_widget_activate_action (GTK_WIDGET (sidebar), "win.show-tag", "s", label);
}

static GtkWidget *add_place (NautilusSidebar *sidebar, NautilusSidebarRowType place_type,
                             NautilusSidebarSectionType section_type, const char *name,
                             GIcon *start_icon, GIcon *end_icon, const char *uri, GDrive *drive,
                             GVolume *volume, GMount *mount,
#ifdef HAVE_CLOUDPROVIDERS
                             CloudProvidersAccount *cloud_provider_account,
#else
                             gpointer *cloud_provider_account,
#endif
                             const int index, const char *tooltip);

static int
compare_recent_newest (GtkRecentInfo *a,
                       GtkRecentInfo *b)
{
    GDateTime *ta = gtk_recent_info_get_modified (a);
    GDateTime *tb = gtk_recent_info_get_modified (b);

    return -g_date_time_compare (ta, tb);
}

/* Folders of recently used files, newest first. */
static void
add_recent_folders (NautilusSidebar *sidebar)
{
    GList *items = gtk_recent_manager_get_items (gtk_recent_manager_get_default ());
    g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr (GPtrArray) sorted = g_ptr_array_new ();
    g_autoptr (GIcon) icon = g_themed_icon_new ("folder-symbolic");
    g_autofree char *home = get_home_directory_uri ();
    int n = 0;

    for (GList *l = items; l != NULL; l = l->next)
    {
        g_ptr_array_add (sorted, l->data);
    }
    g_ptr_array_sort_values (sorted, (GCompareFunc) compare_recent_newest);

    for (guint i = 0; i < sorted->len && n < MAX_RECENT_FOLDERS; i++)
    {
        GtkRecentInfo *info = g_ptr_array_index (sorted, i);
        g_autoptr (GFile) file = NULL;
        g_autoptr (GFile) parent = NULL;
        g_autofree char *uri = NULL;
        g_autofree char *name = NULL;
        g_autofree char *path = NULL;

        if (!gtk_recent_info_is_local (info) || !gtk_recent_info_exists (info))
        {
            continue;
        }
        file = g_file_new_for_uri (gtk_recent_info_get_uri (info));
        parent = g_file_get_parent (file);
        if (parent == NULL)
        {
            continue;
        }
        uri = g_file_get_uri (parent);
        if (g_hash_table_contains (seen, uri) || g_strcmp0 (uri, home) == 0)
        {
            continue;
        }
        g_hash_table_add (seen, g_strdup (uri));
        name = g_file_get_basename (parent);
        path = g_file_get_path (parent);
        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN, NAUTILUS_SIDEBAR_SECTION_RECENT,
                   name, icon, NULL, uri, NULL, NULL, NULL, NULL, ++n, path);
    }

    g_list_free_full (items, (GDestroyNotify) gtk_recent_info_unref);
}

/* The tags, a colored dot each; opening one lists its files. */
static void
add_tags (NautilusSidebar *sidebar)
{
    g_autoptr (GString) css = g_string_new (NULL);
    g_autoptr (GIcon) dot = g_themed_icon_new ("media-record-symbolic");
    guint n = nautilus_labels_get_n_tags ();

    for (guint i = 0; i < n; i++)
    {
        const char *tag = nautilus_labels_get_tag_name (i);
        g_autofree char *tooltip = g_strdup_printf (_("Files Tagged “%s”"), tag);
        g_autofree char *class = g_strdup_printf ("sidebar-tag-%u", i);
        g_autofree char *color = NULL;
        GdkRGBA rgba;
        GtkWidget *row;

        row = add_place (sidebar, NAUTILUS_SIDEBAR_ROW_TAG, NAUTILUS_SIDEBAR_SECTION_TAGS,
                         tag, dot, NULL, NULL, NULL, NULL, NULL, NULL, (int) i, tooltip);
        gtk_widget_add_css_class (row, class);
        nautilus_labels_get_tag_color (tag, &rgba);
        color = gdk_rgba_to_string (&rgba);
        g_string_append_printf (css, "row.%s box > image:first-child { color: %s; }\n", class, color);
    }
    update_tag_css (css);
}

static void
add_flier_rows (NautilusSidebar *sidebar)
{
    gboolean present[NAUTILUS_SIDEBAR_N_SECTION_TYPES] = { FALSE };

    if (groups_on () && should_show_recent (sidebar))
    {
        add_recent_folders (sidebar);
    }
    add_tags (sidebar);

    if (!groups_on ())
    {
        return;
    }

    for (GtkWidget *child = gtk_widget_get_first_child (sidebar->list_box); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        NautilusSidebarSectionType section;
        NautilusSidebarRowType place_type;

        if (NAUTILUS_IS_SIDEBAR_ROW (child))
        {
            g_object_get (child, "section-type", &section, "place-type", &place_type, NULL);
            /* The hidden new-bookmark row alone doesn't make a group. */
            if (place_type != NAUTILUS_SIDEBAR_ROW_NEW_BOOKMARK && section < NAUTILUS_SIDEBAR_N_SECTION_TYPES)
            {
                present[section] = TRUE;
            }
        }
    }

    for (int section = NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS; section < NAUTILUS_SIDEBAR_N_SECTION_TYPES;
         section++)
    {
        if (present[section])
        {
            g_autoptr (GIcon) icon = g_themed_icon_new (group_icon (section));
            GtkWidget *header = add_place (sidebar, NAUTILUS_SIDEBAR_ROW_GROUP_HEADER, section,
                                           group_title (section), icon, NULL, NULL, NULL, NULL, NULL,
                                           NULL, 0, NULL);

            gtk_widget_add_css_class (header, "sidebar-group-header");

            GtkDragSource *drag = gtk_drag_source_new ();
            GtkDropTarget *drop = gtk_drop_target_new (G_TYPE_STRING, GDK_ACTION_MOVE);

            gtk_drag_source_set_actions (drag, GDK_ACTION_MOVE);
            g_signal_connect (drag, "prepare", G_CALLBACK (on_group_drag_prepare), header);
            g_signal_connect (drag, "drag-begin", G_CALLBACK (on_group_drag_begin), header);
            gtk_widget_add_controller (header, GTK_EVENT_CONTROLLER (drag));
            g_signal_connect (drop, "drop", G_CALLBACK (on_group_drop), header);
            gtk_widget_add_controller (header, GTK_EVENT_CONTROLLER (drop));
            gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (header), FALSE);
        }
    }
    update_group_arrows (sidebar);
}

static void
list_box_header_func (GtkListBoxRow *row,
                      GtkListBoxRow *before,
                      gpointer       user_data)
{
    NautilusSidebarSectionType row_section_type;
    NautilusSidebarSectionType before_section_type;
    GtkWidget *separator;

    gtk_list_box_row_set_header (row, NULL);

    g_object_get (row, "section-type", &row_section_type, NULL);
    if (before)
    {
        g_object_get (before, "section-type", &before_section_type, NULL);
    }
    else
    {
        before_section_type = NAUTILUS_SIDEBAR_SECTION_INVALID;
    }

    if (before && before_section_type != row_section_type)
    {
        separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_list_box_row_set_header (row, separator);
    }
}

static GtkWidget *
add_place (NautilusSidebar           *sidebar,
           NautilusSidebarRowType     place_type,
           NautilusSidebarSectionType section_type,
           const char                *name,
           GIcon                     *start_icon,
           GIcon                     *end_icon,
           const char                *uri,
           GDrive                    *drive,
           GVolume                   *volume,
           GMount                    *mount,
#ifdef HAVE_CLOUDPROVIDERS
           CloudProvidersAccount     *cloud_provider_account,
#else
           gpointer                  *cloud_provider_account,
#endif
           const int                  index,
           const char                *tooltip)
{
    gboolean show_eject, show_unmount;
    gboolean show_eject_button;
    GtkWidget *row;
    GtkWidget *eject_button;
    GtkGesture *gesture;
    char *eject_tooltip;

    check_unmount_and_eject (mount, volume, drive,
                             &show_unmount, &show_eject);

    if (show_unmount || show_eject)
    {
        g_assert (place_type != NAUTILUS_SIDEBAR_ROW_BOOKMARK);
    }

    show_eject_button = (show_unmount || show_eject);
    if (mount != NULL && volume == NULL && drive == NULL)
    {
        eject_tooltip = _("Disconnect");
    }
    else if (show_eject)
    {
        eject_tooltip = _("Eject");
    }
    else
    {
        eject_tooltip = _("Unmount");
    }

    row = g_object_new (NAUTILUS_TYPE_SIDEBAR_ROW,
                        "sidebar", sidebar,
                        "start-icon", start_icon,
                        "end-icon", end_icon,
                        "label", name,
                        "tooltip", tooltip,
                        "ejectable", show_eject_button,
                        "eject-tooltip", eject_tooltip,
                        "order-index", index,
                        "section-type", section_type,
                        "place-type", place_type,
                        "uri", uri,
                        "drive", drive,
                        "volume", volume,
                        "mount", mount,
#ifdef HAVE_CLOUDPROVIDERS
                        "cloud-provider-account", cloud_provider_account,
#endif
                        NULL);

    eject_button = nautilus_sidebar_row_get_eject_button (NAUTILUS_SIDEBAR_ROW (row));

    g_signal_connect_swapped (eject_button, "clicked",
                              G_CALLBACK (eject_or_unmount_bookmark), row);

    gesture = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), 0);
    g_signal_connect (gesture, "pressed",
                      G_CALLBACK (on_row_pressed), row);
    g_signal_connect (gesture, "released",
                      G_CALLBACK (on_row_released), row);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (gesture));

    gesture = gtk_gesture_drag_new ();
    g_signal_connect (gesture, "drag-update",
                      G_CALLBACK (on_row_dragged), row);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (gesture));

    gtk_list_box_insert (GTK_LIST_BOX (sidebar->list_box), GTK_WIDGET (row), -1);

    return row;
}

static gboolean
recent_files_setting_is_enabled (NautilusSidebar *sidebar)
{
    GtkSettings *settings;
    gboolean enabled;

    settings = gtk_widget_get_settings (GTK_WIDGET (sidebar));
    g_object_get (settings, "gtk-recent-files-enabled", &enabled, NULL);

    return enabled;
}

static gboolean
recent_scheme_is_supported (void)
{
    const char * const *supported;

    supported = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());
    if (supported != NULL)
    {
        return g_strv_contains (supported, SCHEME_RECENT);
    }

    return FALSE;
}

static gboolean
should_show_recent (NautilusSidebar *sidebar)
{
    return recent_files_setting_is_enabled (sidebar) && recent_scheme_is_supported ();
}

static gboolean
path_is_home_dir (const char *path)
{
    GFile *home_dir;
    GFile *location;
    const char *home_path;
    gboolean res;

    home_path = g_get_home_dir ();
    if (!home_path)
    {
        return FALSE;
    }

    home_dir = g_file_new_for_path (home_path);
    location = g_file_new_for_path (path);
    res = g_file_equal (home_dir, location);

    g_object_unref (home_dir);
    g_object_unref (location);

    return res;
}

static char *
get_home_directory_uri (void)
{
    const char *home;

    home = g_get_home_dir ();
    if (!home)
    {
        return NULL;
    }

    return g_filename_to_uri (home, NULL, NULL);
}

static char *
get_desktop_directory_uri (void)
{
    const char *name;

    name = g_get_user_special_dir (G_USER_DIRECTORY_DESKTOP);

    /* "To disable a directory, point it to the homedir."
     * See http://freedesktop.org/wiki/Software/xdg-user-dirs
     */
    if (path_is_home_dir (name))
    {
        return NULL;
    }

    return g_filename_to_uri (name, NULL, NULL);
}

typedef struct
{
    NautilusSidebar *sidebar;
    int index;
    gboolean is_native;
} BookmarkQueryClosure;

static void
update_trash_icon (NautilusSidebar *sidebar)
{
    if (sidebar->trash_row)
    {
        GIcon *icon;

        icon = nautilus_trash_monitor_get_symbolic_icon ();
        nautilus_sidebar_row_set_start_icon (NAUTILUS_SIDEBAR_ROW (sidebar->trash_row), icon);
        g_object_unref (icon);
    }
}

#ifdef HAVE_CLOUDPROVIDERS

static gboolean
create_cloud_provider_account_row (NautilusSidebar       *sidebar,
                                   CloudProvidersAccount *account)
{
    GIcon *end_icon;
    GIcon *start_icon;
    const char *mount_path;
    const char *name;
    char *mount_uri;
    char *tooltip;
    guint provider_account_status;

    start_icon = cloud_providers_account_get_icon (account);
    name = cloud_providers_account_get_name (account);
    provider_account_status = cloud_providers_account_get_status (account);
    mount_path = cloud_providers_account_get_path (account);
    if (start_icon != NULL
        && name != NULL
        && provider_account_status != CLOUD_PROVIDERS_ACCOUNT_STATUS_INVALID
        && mount_path != NULL)
    {
        switch (provider_account_status)
        {
            case CLOUD_PROVIDERS_ACCOUNT_STATUS_IDLE:
            {
                end_icon = NULL;
                break;
            }

            case CLOUD_PROVIDERS_ACCOUNT_STATUS_SYNCING:
            {
                end_icon = g_themed_icon_new ("emblem-synchronizing-symbolic");
                break;
            }

            case CLOUD_PROVIDERS_ACCOUNT_STATUS_ERROR:
            {
                end_icon = g_themed_icon_new ("dialog-warning-symbolic");
                break;
            }

            default:
            {
                return FALSE;
            }
        }

        mount_uri = g_strconcat ("file://", mount_path, NULL);

        /* translators: %s is the name of a cloud provider for files */
        tooltip = g_strdup_printf (_("Open %s"), name);

        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                   NAUTILUS_SIDEBAR_SECTION_CLOUD,
                   name, start_icon, end_icon, mount_uri,
                   NULL, NULL, NULL, account, 0,
                   tooltip);

        g_free (tooltip);
        g_free (mount_uri);
        g_clear_object (&end_icon);
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

static void
on_account_updated (GObject    *object,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
    CloudProvidersAccount *account = CLOUD_PROVIDERS_ACCOUNT (object);
    NautilusSidebar *sidebar = NAUTILUS_PLACES_SIDEBAR (user_data);

    if (create_cloud_provider_account_row (sidebar, account))
    {
        g_signal_handlers_disconnect_by_data (account, sidebar);
        sidebar->unready_accounts = g_list_remove (sidebar->unready_accounts, account);
        g_object_unref (account);
    }
}

#endif

static void
update_places (NautilusSidebar *sidebar)
{
    GList *mounts, *l, *ll;
    GMount *mount;
    GList *drives;
    GDrive *drive;
    GList *volumes;
    GVolume *volume;
    GList *bookmarks;
    int index;
    char *original_uri, *name, *identifier;
    GtkListBoxRow *selected;
    char *home_uri;
    GIcon *start_icon;
    GFile *root;
    char *tooltip;
    GList *network_mounts, *network_volumes;
    GIcon *new_bookmark_icon;
    GtkWidget *child;
#ifdef HAVE_CLOUDPROVIDERS
    GList *cloud_providers;
    GList *cloud_providers_accounts;
    CloudProvidersAccount *cloud_provider_account;
    CloudProvidersProvider *cloud_provider;
#endif

    /* Places inside a group sit a little further in than its header. */
    if (groups_on ())
    {
        gtk_widget_add_css_class (sidebar->list_box, "sidebar-grouped");
    }
    else
    {
        gtk_widget_remove_css_class (sidebar->list_box, "sidebar-grouped");
    }

    /* save original selection */
    selected = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));
    if (selected)
    {
        g_object_get (selected, "uri", &original_uri, NULL);
    }
    else
    {
        original_uri = NULL;
    }

    /* Reset drag state, just in case we update the places while dragging or
     * ending a drag */
    stop_drop_feedback (sidebar);
    while ((child = gtk_widget_get_first_child (GTK_WIDGET (sidebar->list_box))))
    {
        gtk_list_box_remove (GTK_LIST_BOX (sidebar->list_box), child);
    }

    network_mounts = network_volumes = NULL;

    /* add built-in places */

    /* home folder */
    home_uri = get_home_directory_uri ();
    if (!place_hidden ("home"))
    {
        start_icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_HOME);
        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                   NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS,
                   _("Home"), start_icon, NULL, home_uri,
                   NULL, NULL, NULL, NULL, 0,
                   _("Open Personal Folder"));
        g_object_unref (start_icon);
    }
    g_free (home_uri);

    /* Downloads, Documents, Pictures, Videos and Music */
    for (guint i = 0; i < G_N_ELEMENTS (user_folders); i++)
    {
        g_autofree char *folder_uri = user_folder_uri (i);
        g_autofree char *path = NULL;
        g_autofree char *basename = NULL;

        if (folder_uri == NULL)
        {
            continue;
        }
        path = g_filename_from_uri (folder_uri, NULL, NULL);
        basename = g_filename_display_basename (path);
        start_icon = g_themed_icon_new_with_default_fallbacks (user_folders[i].icon);
        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                   NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS,
                   basename, start_icon, NULL, folder_uri,
                   NULL, NULL, NULL, NULL, 0, path);
        g_object_unref (start_icon);
    }

    if (should_show_recent (sidebar) && !place_hidden ("recent"))
    {
        start_icon = g_themed_icon_new_with_default_fallbacks ("document-open-recent-symbolic");
        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                   groups_on () ? NAUTILUS_SIDEBAR_SECTION_RECENT : NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS,
                   groups_on () ? _("Recent Files") : _("Recent"), start_icon, NULL, SCHEME_RECENT ":///",
                   NULL, NULL, NULL, NULL, 0,
                   _("Recent Files"));
        g_object_unref (start_icon);
    }

    if (!place_hidden ("starred"))
    {
        start_icon = g_themed_icon_new_with_default_fallbacks ("starred-symbolic");
        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                   NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS,
                   _("Starred"), start_icon, NULL, SCHEME_STARRED ":///",
                   NULL, NULL, NULL, NULL, 0,
                   _("Starred Files"));
        g_object_unref (start_icon);
    }

    /* desktop */
    if (sidebar->show_desktop)
    {
        char *mount_uri = get_desktop_directory_uri ();
        if (mount_uri)
        {
            start_icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_DESKTOP);
            add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                       NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS,
                       _("Desktop"), start_icon, NULL, mount_uri,
                       NULL, NULL, NULL, NULL, 0,
                       _("Open the contents of your desktop in a folder"));
            g_object_unref (start_icon);
            g_free (mount_uri);
        }
    }

    /* Network view */
    if (!place_hidden ("network"))
    {
        start_icon = g_themed_icon_new_with_default_fallbacks (ICON_NAME_NETWORK_VIEW);
        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                   NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS,
                   _("Network"), start_icon, NULL, SCHEME_NETWORK_VIEW ":///",
                   NULL, NULL, NULL, NULL, 0,
                   _("Open Network Locations"));
        g_object_unref (start_icon);
    }

    /* Trash */
    if (sidebar->show_trash && !place_hidden ("trash"))
    {
        start_icon = nautilus_trash_monitor_get_symbolic_icon ();
        sidebar->trash_row = add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                                        NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS,
                                        _("Trash"), start_icon, NULL, SCHEME_TRASH ":///",
                                        NULL, NULL, NULL, NULL, 0,
                                        _("Open Trash"));
        g_object_add_weak_pointer (G_OBJECT (sidebar->trash_row),
                                   (gpointer *) &sidebar->trash_row);
        g_object_unref (start_icon);
    }

    /* Cloud providers */
#ifdef HAVE_CLOUDPROVIDERS
    cloud_providers = cloud_providers_collector_get_providers (sidebar->cloud_manager);
    for (l = sidebar->unready_accounts; l != NULL; l = l->next)
    {
        g_signal_handlers_disconnect_by_data (l->data, sidebar);
    }
    g_list_free_full (sidebar->unready_accounts, g_object_unref);
    sidebar->unready_accounts = NULL;
    for (l = cloud_providers; l != NULL; l = l->next)
    {
        cloud_provider = CLOUD_PROVIDERS_PROVIDER (l->data);
        g_signal_connect_swapped (cloud_provider, "accounts-changed",
                                  G_CALLBACK (update_places), sidebar);
        cloud_providers_accounts = cloud_providers_provider_get_accounts (cloud_provider);
        for (ll = cloud_providers_accounts; ll != NULL; ll = ll->next)
        {
            cloud_provider_account = CLOUD_PROVIDERS_ACCOUNT (ll->data);
            if (!create_cloud_provider_account_row (sidebar, cloud_provider_account))
            {
                g_signal_connect (cloud_provider_account, "notify::name",
                                  G_CALLBACK (on_account_updated), sidebar);
                g_signal_connect (cloud_provider_account, "notify::status",
                                  G_CALLBACK (on_account_updated), sidebar);
                g_signal_connect (cloud_provider_account, "notify::status-details",
                                  G_CALLBACK (on_account_updated), sidebar);
                g_signal_connect (cloud_provider_account, "notify::path",
                                  G_CALLBACK (on_account_updated), sidebar);
                sidebar->unready_accounts = g_list_append (sidebar->unready_accounts,
                                                           g_object_ref (cloud_provider_account));
                continue;
            }
        }
    }
#endif

    /* go through all connected drives */
    drives = g_volume_monitor_get_connected_drives (sidebar->volume_monitor);

    for (l = drives; l != NULL; l = l->next)
    {
        drive = l->data;

        volumes = g_drive_get_volumes (drive);
        if (volumes != NULL)
        {
            for (ll = volumes; ll != NULL; ll = ll->next)
            {
                volume = ll->data;
                identifier = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_CLASS);

                if (g_strcmp0 (identifier, "network") == 0)
                {
                    g_free (identifier);
                    network_volumes = g_list_prepend (network_volumes, volume);
                    continue;
                }
                g_free (identifier);

                mount = g_volume_get_mount (volume);
                if (mount != NULL)
                {
                    char *mount_uri;

                    /* Show mounted volume in the sidebar */
                    start_icon = g_mount_get_symbolic_icon (mount);
                    root = g_mount_get_default_location (mount);
                    mount_uri = g_file_get_uri (root);
                    name = g_mount_get_name (mount);
                    tooltip = g_file_get_parse_name (root);

                    add_place (sidebar, (is_external_volume (volume) ?
                                         NAUTILUS_SIDEBAR_ROW_EXTERNAL_MOUNT :
                                         NAUTILUS_SIDEBAR_ROW_INTERNAL_MOUNT),
                               NAUTILUS_SIDEBAR_SECTION_MOUNTS,
                               name, start_icon, NULL, mount_uri,
                               drive, volume, mount, NULL, 0, tooltip);
                    g_object_unref (root);
                    g_object_unref (mount);
                    g_object_unref (start_icon);
                    g_free (tooltip);
                    g_free (name);
                    g_free (mount_uri);
                }
                else
                {
                    /* Do show the unmounted volumes in the sidebar;
                     * this is so the user can mount it (in case automounting
                     * is off).
                     *
                     * Also, even if automounting is enabled, this gives a visual
                     * cue that the user should remember to yank out the media if
                     * he just unmounted it.
                     */
                    start_icon = g_volume_get_symbolic_icon (volume);
                    name = g_volume_get_name (volume);
                    tooltip = g_strdup_printf (_("Mount and Open “%s”"), name);

                    add_place (sidebar, (is_external_volume (volume) ?
                                         NAUTILUS_SIDEBAR_ROW_EXTERNAL_MOUNT :
                                         NAUTILUS_SIDEBAR_ROW_INTERNAL_MOUNT),
                               NAUTILUS_SIDEBAR_SECTION_MOUNTS,
                               name, start_icon, NULL, NULL,
                               drive, volume, NULL, NULL, 0, tooltip);
                    g_object_unref (start_icon);
                    g_free (name);
                    g_free (tooltip);
                }
                g_object_unref (volume);
            }
            g_list_free (volumes);
        }
        else
        {
            if (g_drive_is_media_removable (drive) && !g_drive_is_media_check_automatic (drive))
            {
                /* If the drive has no mountable volumes and we cannot detect media change.. we
                 * display the drive in the sidebar so the user can manually poll the drive by
                 * right clicking and selecting "Rescan..."
                 *
                 * This is mainly for drives like floppies where media detection doesn't
                 * work.. but it's also for human beings who like to turn off media detection
                 * in the OS to save battery juice.
                 */
                start_icon = g_drive_get_symbolic_icon (drive);
                name = g_drive_get_name (drive);
                tooltip = g_strdup_printf (_("Mount and Open “%s”"), name);

                add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BUILT_IN,
                           NAUTILUS_SIDEBAR_SECTION_MOUNTS,
                           name, start_icon, NULL, NULL,
                           drive, NULL, NULL, NULL, 0, tooltip);
                g_object_unref (start_icon);
                g_free (tooltip);
                g_free (name);
            }
        }
    }
    g_list_free_full (drives, g_object_unref);

    /* add all network volumes that are not associated with a drive, and
     * loop devices
     */
    volumes = g_volume_monitor_get_volumes (sidebar->volume_monitor);
    for (l = volumes; l != NULL; l = l->next)
    {
        volume = l->data;
        drive = g_volume_get_drive (volume);
        if (drive != NULL)
        {
            g_object_unref (volume);
            g_object_unref (drive);
            continue;
        }

        identifier = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_CLASS);

        if (g_strcmp0 (identifier, "network") == 0)
        {
            g_free (identifier);
            network_volumes = g_list_prepend (network_volumes, volume);
            continue;
        }
        g_free (identifier);

        mount = g_volume_get_mount (volume);
        if (mount != NULL)
        {
            char *mount_uri;

            start_icon = g_mount_get_symbolic_icon (mount);
            root = g_mount_get_default_location (mount);
            mount_uri = g_file_get_uri (root);
            tooltip = g_file_get_parse_name (root);
            name = g_mount_get_name (mount);
            add_place (sidebar, (is_external_volume (volume) ?
                                 NAUTILUS_SIDEBAR_ROW_EXTERNAL_MOUNT :
                                 NAUTILUS_SIDEBAR_ROW_INTERNAL_MOUNT),
                       NAUTILUS_SIDEBAR_SECTION_MOUNTS,
                       name, start_icon, NULL, mount_uri,
                       NULL, volume, mount, NULL, 0, tooltip);
            g_object_unref (mount);
            g_object_unref (root);
            g_object_unref (start_icon);
            g_free (name);
            g_free (tooltip);
            g_free (mount_uri);
        }
        else
        {
            /* see comment above in why we add an icon for an unmounted mountable volume */
            start_icon = g_volume_get_symbolic_icon (volume);
            name = g_volume_get_name (volume);
            add_place (sidebar, (is_external_volume (volume) ?
                                 NAUTILUS_SIDEBAR_ROW_EXTERNAL_MOUNT :
                                 NAUTILUS_SIDEBAR_ROW_INTERNAL_MOUNT),
                       NAUTILUS_SIDEBAR_SECTION_MOUNTS,
                       name, start_icon, NULL, NULL,
                       NULL, volume, NULL, NULL, 0, name);
            g_object_unref (start_icon);
            g_free (name);
        }
        g_object_unref (volume);
    }
    g_list_free (volumes);

    /* add mounts that has no volume (/etc/mtab mounts, ftp, sftp,...) */
    mounts = g_volume_monitor_get_mounts (sidebar->volume_monitor);

    for (l = mounts; l != NULL; l = l->next)
    {
        char *mount_uri;

        mount = l->data;
        if (g_mount_is_shadowed (mount))
        {
            g_object_unref (mount);
            continue;
        }
        volume = g_mount_get_volume (mount);
        if (volume != NULL)
        {
            g_object_unref (volume);
            g_object_unref (mount);
            continue;
        }
        root = g_mount_get_default_location (mount);

        if (!g_file_is_native (root))
        {
            network_mounts = g_list_prepend (network_mounts, mount);
            g_object_unref (root);
            continue;
        }

        start_icon = g_mount_get_symbolic_icon (mount);
        mount_uri = g_file_get_uri (root);
        name = g_mount_get_name (mount);
        tooltip = g_file_get_parse_name (root);
        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_EXTERNAL_MOUNT,
                   NAUTILUS_SIDEBAR_SECTION_MOUNTS,
                   name, start_icon, NULL, mount_uri,
                   NULL, NULL, mount, NULL, 0, tooltip);
        g_object_unref (root);
        g_object_unref (mount);
        g_object_unref (start_icon);
        g_free (name);
        g_free (mount_uri);
        g_free (tooltip);
    }
    g_list_free (mounts);

    /* add bookmarks */
    bookmarks = nautilus_bookmark_list_get_all (sidebar->bookmark_list);
    /* Needs to start from 1 so that bookmark drag placeholder can come first. */
    index = 1;

    for (GList *l = bookmarks; l != NULL; l = l->next)
    {
        GtkWidget *row;

        GFile *location = nautilus_bookmark_get_location (l->data);
        g_autofree char *mount_uri = nautilus_bookmark_get_uri (l->data);

        /* Places has it already. */
        if (is_user_folder_uri (mount_uri))
        {
            continue;
        }

        gboolean is_native = g_file_is_native (location);
        tooltip = is_native ? g_file_get_path (location) : g_uri_unescape_string (mount_uri, NULL);

        row = add_place (sidebar, NAUTILUS_SIDEBAR_ROW_BOOKMARK,
                         NAUTILUS_SIDEBAR_SECTION_BOOKMARKS,
                         nautilus_bookmark_get_name (l->data),
                         NULL,
                         NULL, mount_uri, NULL, NULL, NULL, NULL, index, tooltip);
        g_object_bind_property (l->data, "symbolic-icon", row, "start-icon", G_BINDING_SYNC_CREATE);
        index++;

        g_free (tooltip);
    }

    /* Add new bookmark row */
    new_bookmark_icon = g_themed_icon_new ("bookmark-new-symbolic");
    sidebar->new_bookmark_row = add_place (sidebar, NAUTILUS_SIDEBAR_ROW_NEW_BOOKMARK,
                                           NAUTILUS_SIDEBAR_SECTION_BOOKMARKS,
                                           _("New bookmark"), new_bookmark_icon, NULL, NULL,
                                           NULL, NULL, NULL, NULL, 0,
                                           _("Add a new bookmark"));
    gtk_widget_add_css_class (sidebar->new_bookmark_row, "sidebar-new-bookmark-row");
    g_object_unref (new_bookmark_icon);

    /* network */
    network_volumes = g_list_reverse (network_volumes);
    for (l = network_volumes; l != NULL; l = l->next)
    {
        volume = l->data;
        mount = g_volume_get_mount (volume);

        if (mount != NULL)
        {
            network_mounts = g_list_prepend (network_mounts, mount);
            continue;
        }
        else
        {
            start_icon = g_volume_get_symbolic_icon (volume);
            name = g_volume_get_name (volume);
            tooltip = g_strdup_printf (_("Mount and Open “%s”"), name);

            add_place (sidebar, NAUTILUS_SIDEBAR_ROW_EXTERNAL_MOUNT,
                       NAUTILUS_SIDEBAR_SECTION_MOUNTS,
                       name, start_icon, NULL, NULL,
                       NULL, volume, NULL, NULL, 0, tooltip);
            g_object_unref (start_icon);
            g_free (name);
            g_free (tooltip);
        }
    }

    network_mounts = g_list_reverse (network_mounts);
    for (l = network_mounts; l != NULL; l = l->next)
    {
        char *mount_uri;

        mount = l->data;
        root = g_mount_get_default_location (mount);
        start_icon = g_mount_get_symbolic_icon (mount);
        mount_uri = g_file_get_uri (root);
        name = g_mount_get_name (mount);
        tooltip = g_file_get_parse_name (root);
        add_place (sidebar, NAUTILUS_SIDEBAR_ROW_EXTERNAL_MOUNT,
                   NAUTILUS_SIDEBAR_SECTION_MOUNTS,
                   name, start_icon, NULL, mount_uri,
                   NULL, NULL, mount, NULL, 0, tooltip);
        g_object_unref (root);
        g_object_unref (start_icon);
        g_free (name);
        g_free (mount_uri);
        g_free (tooltip);
    }


    g_list_free_full (network_volumes, g_object_unref);
    g_list_free_full (network_mounts, g_object_unref);

    /* We want this hidden by default, but need to do it after the show_all call */
    nautilus_sidebar_row_hide (NAUTILUS_SIDEBAR_ROW (sidebar->new_bookmark_row), TRUE);

    add_flier_rows (sidebar);
    update_row_colors (sidebar);

    /* restore original selection */
    if (original_uri)
    {
        GFile *restore;

        restore = g_file_new_for_uri (original_uri);
        nautilus_sidebar_set_location (sidebar, restore);
        g_object_unref (restore);
        g_free (original_uri);
    }
}

void
nautilus_sidebar_set_show_trash (NautilusSidebar *sidebar,
                                 gboolean         show_trash)
{
    if (sidebar->show_trash == show_trash)
    {
        return;
    }

    sidebar->show_trash = show_trash;

    if (!sidebar->show_trash)
    {
        g_clear_weak_pointer (&sidebar->trash_row);
    }

    update_places (sidebar);
}

static gboolean
hover_timer (gpointer user_data)
{
    NautilusSidebar *sidebar = user_data;
    gboolean open_folder_on_hover;
    g_autofree gchar *uri = NULL;
    g_autoptr (GFile) location = NULL;

    open_folder_on_hover = g_settings_get_boolean (nautilus_preferences,
                                                   NAUTILUS_PREFERENCES_OPEN_FOLDER_ON_DND_HOVER);
    sidebar->hover_timer_id = 0;

    if (open_folder_on_hover &&
        sidebar->hover_row != NULL &&
        gtk_widget_get_sensitive (GTK_WIDGET (sidebar->hover_row)))
    {
        const gchar *drag_open_exlusion_list[] =
        {
            SCHEME_TRASH ":///",
            SCHEME_STARRED ":///",
            NULL,
        };

        g_object_get (sidebar->hover_row, "uri", &uri, NULL);
        if (uri != NULL && !g_strv_contains (drag_open_exlusion_list, uri))
        {
            location = g_file_new_for_uri (uri);
            call_open_location (sidebar, location, NULL, 0);
        }
    }

    return G_SOURCE_REMOVE;
}

static gboolean
check_valid_drop_target (NautilusSidebar    *sidebar,
                         NautilusSidebarRow *row,
                         const GValue       *value)
{
    NautilusSidebarRowType place_type;
    NautilusSidebarSectionType section_type;
    g_autoptr (NautilusFile) dest_file = NULL;
    gboolean valid = FALSE;
    char *uri;
    int drag_action;

    g_return_val_if_fail (value != NULL, TRUE);

    if (row == NULL)
    {
        return FALSE;
    }

    g_object_get (row,
                  "place-type", &place_type,
                  "section_type", &section_type,
                  "uri", &uri,
                  "file", &dest_file,
                  NULL);

    if (place_type == NAUTILUS_SIDEBAR_ROW_NEW_BOOKMARK)
    {
        g_free (uri);
        return TRUE;
    }

    /* Disallow drops on recent:/// */
    if (place_type == NAUTILUS_SIDEBAR_ROW_BUILT_IN)
    {
        if (g_strcmp0 (uri, SCHEME_RECENT ":///") == 0)
        {
            g_free (uri);
            return FALSE;
        }
    }

    /* Dragging a bookmark? */
    if (G_VALUE_HOLDS (value, NAUTILUS_TYPE_SIDEBAR_ROW))
    {
        /* Don't allow reordering bookmarks into non-bookmark areas */
        valid = section_type == NAUTILUS_SIDEBAR_SECTION_BOOKMARKS;
    }
    else if (G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    {
        /* Dragging a file */
        GSList *file_list = g_value_get_boxed (value);

        if (uri != NULL &&
            file_list != NULL)
        {
            drag_action = emit_drag_action_requested (sidebar, dest_file, file_list);
            valid = drag_action > 0;
        }
        else
        {
            valid = FALSE;
        }
    }
    else
    {
        g_assert_not_reached ();
        valid = TRUE;
    }

    g_free (uri);
    return valid;
}

static void
update_possible_drop_targets (NautilusSidebar *sidebar,
                              const GValue    *value)
{
    GtkWidget *row;

    for (row = gtk_widget_get_first_child (GTK_WIDGET (sidebar->list_box));
         row != NULL;
         row = gtk_widget_get_next_sibling (row))
    {
        gboolean sensitive;

        if (!GTK_IS_LIST_BOX_ROW (row))
        {
            continue;
        }

        sensitive = value == NULL ||
                    check_valid_drop_target (sidebar, NAUTILUS_SIDEBAR_ROW (row), value);
        gtk_widget_set_sensitive (row, sensitive);
    }
}

static void
start_drop_feedback (NautilusSidebar *sidebar,
                     const GValue    *value)
{
    if (value != NULL && G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    {
        GSList *source_list = g_value_get_boxed (value);
        if (g_slist_length (source_list) == 1)
        {
            g_autoptr (NautilusFile) file = NULL;
            file = nautilus_file_get (source_list->data);
            if (nautilus_file_is_directory (file))
            {
                nautilus_sidebar_row_reveal (NAUTILUS_SIDEBAR_ROW (sidebar->new_bookmark_row));
            }
        }
    }
    if (value && !G_VALUE_HOLDS (value, NAUTILUS_TYPE_SIDEBAR_ROW))
    {
        /* If the state is permanent, don't change it. The application controls it. */
        if (sidebar->drop_state != DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT)
        {
            sidebar->drop_state = DROP_STATE_NEW_BOOKMARK_ARMED;
        }
    }

    update_possible_drop_targets (sidebar, value);
}

static void
stop_drop_feedback (NautilusSidebar *sidebar)
{
    update_possible_drop_targets (sidebar, NULL);

    if (sidebar->drop_state != DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT &&
        sidebar->new_bookmark_row != NULL)
    {
        nautilus_sidebar_row_hide (NAUTILUS_SIDEBAR_ROW (sidebar->new_bookmark_row), FALSE);
        sidebar->drop_state = DROP_STATE_NORMAL;
    }

    if (sidebar->drag_row != NULL)
    {
        gtk_widget_set_visible (sidebar->drag_row, TRUE);
        sidebar->drag_row = NULL;
    }

    if (sidebar->row_placeholder != NULL)
    {
        if (gtk_widget_get_parent (sidebar->row_placeholder) != NULL)
        {
            gtk_list_box_remove (GTK_LIST_BOX (sidebar->list_box), sidebar->row_placeholder);
        }
        sidebar->row_placeholder = NULL;
    }

    sidebar->dragging_over = FALSE;
}

static GdkDragAction
drag_motion_callback (GtkDropTarget   *target,
                      double           x,
                      double           y,
                      NautilusSidebar *sidebar)
{
    GdkDragAction action;
    GtkListBoxRow *row;
    NautilusSidebarRowType place_type;
    char *drop_target_uri = NULL;
    int row_index;
    int row_placeholder_index;
    const GValue *value;
    graphene_point_t start;
    graphene_point_t point_in_row;

    sidebar->dragging_over = TRUE;
    action = 0;
    row = gtk_list_box_get_row_at_y (GTK_LIST_BOX (sidebar->list_box), y);

    start = sidebar->hover_start_point;
    if (row != NULL &&
        sidebar->drag_row == NULL &&
        (row != sidebar->hover_row ||
         gtk_drag_check_threshold (GTK_WIDGET (sidebar), start.x, start.y, x, y)))
    {
        g_clear_handle_id (&sidebar->hover_timer_id, g_source_remove);
        g_set_weak_pointer (&sidebar->hover_row, row);
        sidebar->hover_timer_id = g_timeout_add (HOVER_TIMEOUT, hover_timer, sidebar);
        sidebar->hover_start_point.x = x;
        sidebar->hover_start_point.y = y;
    }

    /* Workaround https://gitlab.gnome.org/GNOME/gtk/-/issues/5023 */
    gtk_list_box_drag_unhighlight_row (GTK_LIST_BOX (sidebar->list_box));

    /* Nothing to do if no value yet */
    value = gtk_drop_target_get_value (target);
    if (value == NULL)
    {
        goto out;
    }

    /* Nothing to do if the target is not valid drop destination */
    if (!check_valid_drop_target (sidebar, NAUTILUS_SIDEBAR_ROW (row), value))
    {
        goto out;
    }

    if (G_VALUE_HOLDS (value, NAUTILUS_TYPE_SIDEBAR_ROW))
    {
        /* Dragging bookmarks always moves them to another position in the bookmarks list */
        action = GDK_ACTION_MOVE;
        if (sidebar->row_placeholder == NULL)
        {
            sidebar->row_placeholder = GTK_WIDGET (nautilus_sidebar_row_new_placeholder ());
        }
        else if (GTK_WIDGET (row) == sidebar->row_placeholder)
        {
            goto out;
        }

        if (gtk_widget_get_parent (sidebar->row_placeholder) != NULL)
        {
            gtk_list_box_remove (GTK_LIST_BOX (sidebar->list_box), sidebar->row_placeholder);
        }

        if (row != NULL)
        {
            g_object_get (row, "order-index", &row_index, NULL);
            g_object_get (sidebar->row_placeholder, "order-index", &row_placeholder_index, NULL);
            /* We order the bookmarks sections based on the bookmark index that we
             * set on the row as order-index property, but we have to deal with
             * the placeholder row wanting to be between two consecutive bookmarks,
             * with two consecutive order-index values which is the usual case.
             * For that, in the list box sort func we give priority to the placeholder row,
             * that means that if the index-order is the same as another bookmark
             * the placeholder row goes before. However if we want to show it after
             * the current row, for instance when the cursor is in the lower half
             * of the row, we need to increase the order-index.
             */
            row_placeholder_index = MAX (row_index - 1, 0);


            if (gtk_widget_compute_point (sidebar->list_box, GTK_WIDGET (row),
                                          &GRAPHENE_POINT_INIT (x, y), &point_in_row) &&
                point_in_row.y > sidebar->drag_row_height / 2 && row_index > 0)
            {
                row_placeholder_index++;
            }
        }
        else
        {
            /* If the user is dragging over an area that has no row, place the row
             * placeholder in the last position
             */
            row_placeholder_index = G_MAXINT32;
        }

        g_object_set (sidebar->row_placeholder, "order-index", row_placeholder_index, NULL);

        gtk_list_box_prepend (GTK_LIST_BOX (sidebar->list_box),
                              sidebar->row_placeholder);
    }
    else if (G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    {
        NautilusFile *file;
        gtk_list_box_drag_highlight_row (GTK_LIST_BOX (sidebar->list_box), row);

        g_object_get (row,
                      "place-type", &place_type,
                      "uri", &drop_target_uri,
                      "file", &file,
                      NULL);
        /* URIs are being dragged.  See if the caller wants to handle a
         * file move/copy operation itself, or if we should only try to
         * create bookmarks out of the dragged URIs.
         */
        if (place_type == NAUTILUS_SIDEBAR_ROW_NEW_BOOKMARK)
        {
            action = GDK_ACTION_COPY;
        }
        else
        {
            GSList *file_list = g_value_get_boxed (value);

            /* uri may be NULL for unmounted volumes, for example, so we don't allow drops there */
            if (drop_target_uri != NULL &&
                file_list != NULL)
            {
                GFile *dest_file = g_file_new_for_uri (drop_target_uri);

                action = emit_drag_action_requested (sidebar, file, file_list);

                g_object_unref (dest_file);
            }
        }

        nautilus_file_unref (file);
        g_free (drop_target_uri);
    }
    else
    {
        g_assert_not_reached ();
    }

out:
    start_drop_feedback (sidebar, value);
    return action;
}

/* Reorders the bookmark to the specified position */
static void
reorder_bookmarks (NautilusSidebar    *sidebar,
                   NautilusSidebarRow *row,
                   int                 new_position)
{
    g_autoptr (NautilusFile) file = NULL;

    g_object_get (row, "file", &file, NULL);

    g_autoptr (GFile) location = nautilus_file_get_location (file);

    nautilus_bookmark_list_move_item (sidebar->bookmark_list, location, new_position);
}

/* Creates bookmarks for the specified files at the given position in the bookmarks list */
static void
drop_files_as_bookmarks (NautilusSidebar *sidebar,
                         GSList          *files,
                         int              position)
{
    GSList *l;

    for (l = files; l; l = l->next)
    {
        GFile *f = G_FILE (l->data);
        GFileInfo *info = g_file_query_info (f,
                                             G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             NULL,
                                             NULL);

        if (info)
        {
            if ((g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ||
                 g_file_info_get_file_type (info) == G_FILE_TYPE_MOUNTABLE ||
                 g_file_info_get_file_type (info) == G_FILE_TYPE_SHORTCUT ||
                 g_file_info_get_file_type (info) == G_FILE_TYPE_SYMBOLIC_LINK))
            {
                nautilus_bookmark_list_add (sidebar->bookmark_list, f, position++);
            }

            g_object_unref (info);
        }
    }
}

static gboolean
drag_drop_callback (GtkDropTarget   *target,
                    const GValue    *value,
                    double           x,
                    double           y,
                    NautilusSidebar *sidebar)
{
    int target_order_index;
    NautilusSidebarRowType target_place_type;
    NautilusSidebarSectionType target_section_type;
    g_autofree gchar *target_uri = NULL;
    GtkListBoxRow *target_row;
    gboolean result;

    target_row = gtk_list_box_get_row_at_y (GTK_LIST_BOX (sidebar->list_box), y);
    if (target_row == NULL)
    {
        return FALSE;
    }

    if (!check_valid_drop_target (sidebar, NAUTILUS_SIDEBAR_ROW (target_row), value))
    {
        return FALSE;
    }

    g_object_get (target_row,
                  "place-type", &target_place_type,
                  "section-type", &target_section_type,
                  "order-index", &target_order_index,
                  "uri", &target_uri,
                  NULL);
    result = FALSE;

    if (G_VALUE_HOLDS (value, NAUTILUS_TYPE_SIDEBAR_ROW))
    {
        GtkWidget *source_row;
        /* A bookmark got reordered */
        if (target_section_type != NAUTILUS_SIDEBAR_SECTION_BOOKMARKS)
        {
            goto out;
        }

        source_row = g_value_get_object (value);

        if (sidebar->row_placeholder != NULL)
        {
            g_object_get (sidebar->row_placeholder, "order-index", &target_order_index, NULL);
        }

        reorder_bookmarks (sidebar, NAUTILUS_SIDEBAR_ROW (source_row), target_order_index);
        result = TRUE;
    }
    else if (G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    {
        GSList *file_list = g_value_get_boxed (value);

        if (file_list == NULL)
        {
            stop_drop_feedback (sidebar);

            return FALSE;
        }

        /* Dropping URIs! */
        if (target_place_type == NAUTILUS_SIDEBAR_ROW_NEW_BOOKMARK)
        {
            drop_files_as_bookmarks (sidebar, file_list, target_order_index);
        }
        else
        {
            GFile *dest_file = g_file_new_for_uri (target_uri);
            GdkDragAction actions;

            actions = gdk_drop_get_actions (gtk_drop_target_get_current_drop (target));

          #ifdef GDK_WINDOWING_X11
            if (GDK_IS_X11_DISPLAY (gtk_widget_get_display (GTK_WIDGET (sidebar))))
            {
                /* Temporary workaround until the below GTK MR (or equivalend fix)
                 * is merged.  Without this fix, the preferred action isn't set correctly.
                 * https://gitlab.gnome.org/GNOME/gtk/-/merge_requests/4982 */
                GdkDrag *drag = gdk_drop_get_drag (gtk_drop_target_get_current_drop (target));
                actions = gdk_drag_get_selected_action (drag);
            }
          #endif

            emit_drag_perform_drop (sidebar,
                                    dest_file,
                                    file_list,
                                    actions);

            g_object_unref (dest_file);
        }
        result = TRUE;
    }
    else
    {
        g_assert_not_reached ();
    }

out:
    stop_drop_feedback (sidebar);

    return result;
}

static void
dnd_finished_cb (GdkDrag         *drag,
                 NautilusSidebar *sidebar)
{
    stop_drop_feedback (sidebar);
}

static void
dnd_cancel_cb (GdkDrag             *drag,
               GdkDragCancelReason  reason,
               NautilusSidebar     *sidebar)
{
    stop_drop_feedback (sidebar);
}

/* This functions is called every time the drag source leaves
 * the sidebar widget.
 * The problem is that, we start showing hints for drop when the source
 * start being above the sidebar or when the application request so show
 * drop hints, but at some moment we need to restore to normal
 * state.
 * One could think that here we could simply call stop_drop_feedback,
 * but that's not true, because this function is called also before drag_drop,
 * which needs the data from the drag so we cannot free the drag data here.
 * So now one could think we could just do nothing here, and wait for
 * drag-end or drag-cancel signals and just stop_drop_feedback there. But that
 * is also not true, since when the drag comes from a different widget than the
 * sidebar, when the drag stops the last drag signal we receive is drag-leave.
 * So here what we will do is restore the state of the sidebar as if no drag
 * is being done (and if the application didn't request for permanent hints with
 * nautilus_sidebar_show_drop_hints) and we will free the drag data next time
 * we build new drag data in drag_data_received.
 */
static void
drag_leave_callback (GtkDropTarget *dest,
                     gpointer       user_data)
{
    NautilusSidebar *sidebar = NAUTILUS_PLACES_SIDEBAR (user_data);

    gtk_list_box_drag_unhighlight_row (GTK_LIST_BOX (sidebar->list_box));

    if (sidebar->drop_state != DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT)
    {
        update_possible_drop_targets (sidebar, FALSE);
        nautilus_sidebar_row_hide (NAUTILUS_SIDEBAR_ROW (sidebar->new_bookmark_row), FALSE);
        sidebar->drop_state = DROP_STATE_NORMAL;
    }

    g_clear_handle_id (&sidebar->hover_timer_id, g_source_remove);
    sidebar->dragging_over = FALSE;
}

static void
check_unmount_and_eject (GMount   *mount,
                         GVolume  *volume,
                         GDrive   *drive,
                         gboolean *show_unmount,
                         gboolean *show_eject)
{
    *show_unmount = FALSE;
    *show_eject = FALSE;

    if (drive != NULL)
    {
        *show_eject = g_drive_can_eject (drive);
    }

    if (volume != NULL)
    {
        *show_eject |= g_volume_can_eject (volume);
    }

    if (mount != NULL)
    {
        *show_eject |= g_mount_can_eject (mount);
        *show_unmount = g_mount_can_unmount (mount) && !*show_eject;
    }
}

static void
drive_start_from_bookmark_cb (GObject      *source_object,
                              GAsyncResult *res,
                              gpointer      user_data)
{
    NautilusSidebar *sidebar;
    GError *error;
    char *primary;
    char *name;

    sidebar = NAUTILUS_PLACES_SIDEBAR (user_data);

    error = NULL;
    if (!g_drive_poll_for_media_finish (G_DRIVE (source_object), res, &error))
    {
        if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
            name = g_drive_get_name (G_DRIVE (source_object));
            primary = g_strdup_printf (_("Unable to start “%s”"), name);
            g_free (name);
            show_error_message (sidebar, primary, error->message);
            g_free (primary);
        }
        g_error_free (error);
    }
}

typedef struct
{
    NautilusSidebarRow *row;
    NautilusWindowSlot *window_slot; /* weak reference */
    NautilusOpenFlags open_flags;
} VolumeMountCallbackData;

static void
volume_mount_cb (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
    g_autofree VolumeMountCallbackData *callback_data = user_data;
    NautilusSidebarRow *row = NAUTILUS_SIDEBAR_ROW (callback_data->row);
    NautilusSidebar *sidebar;
    GVolume *volume;
    GError *error;
    char *primary;
    char *name;
    GMount *mount;

    volume = G_VOLUME (source_object);
    g_object_get (row, "sidebar", &sidebar, NULL);

    error = NULL;
    if (!g_volume_mount_finish (volume, result, &error))
    {
        if (error->code != G_IO_ERROR_FAILED_HANDLED &&
            error->code != G_IO_ERROR_ALREADY_MOUNTED)
        {
            name = g_volume_get_name (G_VOLUME (source_object));
            if (g_str_has_prefix (error->message, "Error unlocking"))
            {
                /* Translators: This means that unlocking an encrypted storage
                 * device failed. %s is the name of the device.
                 */
                primary = g_strdup_printf (_("Error unlocking “%s”"), name);
            }
            else
            {
                primary = g_strdup_printf (_("Unable to access “%s”"), name);
            }
            g_free (name);
            show_error_message (sidebar, primary, error->message);
            g_free (primary);
        }
        g_error_free (error);
    }

    nautilus_sidebar_row_set_busy (row, FALSE);

    mount = g_volume_get_mount (volume);
    if (mount != NULL)
    {
        GFile *location;

        location = g_mount_get_default_location (mount);
        call_open_location (sidebar, location, callback_data->window_slot, callback_data->open_flags);

        g_object_unref (G_OBJECT (location));
        g_object_unref (G_OBJECT (mount));
    }

    g_object_unref (row);
    g_object_unref (sidebar);
    g_clear_weak_pointer (&callback_data->window_slot);
}

static void
mount_volume (NautilusSidebarRow *row,
              GVolume            *volume,
              NautilusOpenFlags   open_flags)
{
    NautilusSidebar *sidebar;
    g_autoptr (GMountOperation) mount_op = NULL;
    VolumeMountCallbackData *callback_data = NULL;

    g_object_get (row, "sidebar", &sidebar, NULL);

    mount_op = get_mount_operation (sidebar);
    g_mount_operation_set_password_save (mount_op, G_PASSWORD_SAVE_FOR_SESSION);

    g_object_ref (sidebar);
    callback_data = g_new0 (VolumeMountCallbackData, 1);
    g_set_weak_pointer (&callback_data->window_slot, sidebar->window_slot);
    callback_data->row = g_object_ref (row);
    callback_data->open_flags = open_flags;

    g_volume_mount (volume, 0, mount_op, NULL, volume_mount_cb, callback_data);
}

static void
open_row (NautilusSidebarRow *self,
          NautilusOpenFlags   open_flags)
{
    g_autofree char *uri = NULL;
    g_autoptr (GDrive) drive = NULL;
    g_autoptr (GVolume) volume = NULL;
    g_autoptr (NautilusSidebar) sidebar = NULL;

    g_object_get (self,
                  "sidebar", &sidebar,
                  "uri", &uri,
                  "drive", &drive,
                  "volume", &volume,
                  NULL);

    if (uri != NULL)
    {
        g_autoptr (GFile) location = g_file_new_for_uri (uri);
        call_open_location (sidebar, location, NULL, open_flags);
    }
    else if (volume != NULL)
    {
        nautilus_sidebar_row_set_busy (self, TRUE);
        mount_volume (self, volume, open_flags);
    }
    else if (drive != NULL)
    {
        if (g_drive_can_start (drive) || g_drive_can_start_degraded (drive))
        {
            g_autoptr (GMountOperation) mount_op = get_mount_operation (sidebar);

            nautilus_sidebar_row_set_busy (self, TRUE);
            g_drive_start (drive, G_DRIVE_START_NONE, mount_op, NULL, drive_start_from_bookmark_cb, NULL);
        }
    }
}

/* ------------------------------------------- Ctrl+arrows in the sidebar */

/* Ctrl+Up/Down move a cursor over the places and the group headers that
 * are showing: a place is opened, a header only highlighted, so closed
 * groups can be passed through. Ctrl+Left/Right fold and unfold the group
 * the cursor is in. */

static gboolean
is_header (GtkWidget *row)
{
    NautilusSidebarRowType place_type;

    if (!NAUTILUS_IS_SIDEBAR_ROW (row))
    {
        return FALSE;
    }
    g_object_get (row, "place-type", &place_type, NULL);
    return place_type == NAUTILUS_SIDEBAR_ROW_GROUP_HEADER;
}

static gboolean
is_steppable (GtkWidget *row)
{
    NautilusSidebarRowType place_type;
    g_autofree char *uri = NULL;
    g_autoptr (GVolume) volume = NULL;

    if (!NAUTILUS_IS_SIDEBAR_ROW (row) || !gtk_widget_get_child_visible (row) ||
        !gtk_widget_get_visible (row))
    {
        return FALSE;
    }
    g_object_get (row, "place-type", &place_type, "uri", &uri, "volume", &volume, NULL);

    return place_type == NAUTILUS_SIDEBAR_ROW_GROUP_HEADER ||
           place_type == NAUTILUS_SIDEBAR_ROW_TAG ||
           (place_type != NAUTILUS_SIDEBAR_ROW_NEW_BOOKMARK &&
            place_type != NAUTILUS_SIDEBAR_ROW_BOOKMARK_PLACEHOLDER &&
            (uri != NULL || volume != NULL));
}

static void
set_cursor (NautilusSidebar *sidebar,
            GtkWidget       *row)
{
    if (sidebar->stepped_row != NULL)
    {
        gtk_widget_remove_css_class (sidebar->stepped_row, "sidebar-cursor");
    }
    g_set_weak_pointer (&sidebar->stepped_row, row);
    if (row != NULL && is_header (row))
    {
        gtk_widget_add_css_class (row, "sidebar-cursor");
    }
}

/* Where the cursor is: on a header, or on the place we're at (the one
 * stepped to, rather than its twin in another group). */
static GtkWidget *
cursor_row (NautilusSidebar *sidebar)
{
    GtkListBoxRow *selected = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));
    GtkWidget *stepped = sidebar->stepped_row;

    if (stepped != NULL && gtk_widget_has_css_class (stepped, "sidebar-cursor"))
    {
        return stepped;
    }
    if (stepped != NULL && selected != NULL && stepped != GTK_WIDGET (selected))
    {
        g_autofree char *uri_selected = NULL, *uri_stepped = NULL;

        g_object_get (selected, "uri", &uri_selected, NULL);
        g_object_get (stepped, "uri", &uri_stepped, NULL);
        if (g_strcmp0 (uri_selected, uri_stepped) == 0)
        {
            return stepped;
        }
    }
    return selected != NULL ? GTK_WIDGET (selected) : stepped;
}

static GtkWidget *
header_of (NautilusSidebar            *sidebar,
           NautilusSidebarSectionType  section)
{
    for (GtkWidget *child = gtk_widget_get_first_child (sidebar->list_box); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        NautilusSidebarSectionType s;

        if (is_header (child))
        {
            g_object_get (child, "section-type", &s, NULL);
            if (s == section)
            {
                return child;
            }
        }
    }
    return NULL;
}

void
nautilus_sidebar_step (NautilusSidebar *sidebar,
                       int              step)
{
    GtkListBox *list = GTK_LIST_BOX (sidebar->list_box);
    GtkWidget *current = cursor_row (sidebar);
    GtkListBoxRow *row = NULL;
    int n = 0;

    while (gtk_list_box_get_row_at_index (list, n) != NULL)
    {
        n++;
    }

    int index = current != NULL ? gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (current))
                                : (step > 0 ? -1 : n);

    /* Rows by index are in the sorted order. */
    for (int i = index + (step > 0 ? 1 : -1); i >= 0 && i < n; i += step > 0 ? 1 : -1)
    {
        GtkListBoxRow *candidate = gtk_list_box_get_row_at_index (list, i);

        if (is_steppable (GTK_WIDGET (candidate)))
        {
            row = candidate;
            break;
        }
    }

    if (row == NULL)
    {
        return;
    }

    NautilusSidebarRowType place_type;

    set_cursor (sidebar, GTK_WIDGET (row));
    g_object_get (row, "place-type", &place_type, NULL);
    if (place_type == NAUTILUS_SIDEBAR_ROW_TAG)
    {
        /* Tag rows aren't selectable: mark them like a header. */
        gtk_widget_add_css_class (GTK_WIDGET (row), "sidebar-cursor");
        on_tag_row_activated (sidebar, NAUTILUS_SIDEBAR_ROW (row));
    }
    else if (!is_header (GTK_WIDGET (row)))
    {
        gtk_list_box_select_row (list, row);
        open_row (NAUTILUS_SIDEBAR_ROW (row), 0);
    }
}

void
nautilus_sidebar_fold_current (NautilusSidebar *sidebar,
                               gboolean         fold)
{
    GtkWidget *current = cursor_row (sidebar);
    NautilusSidebarSectionType section;

    if (current == NULL || !groups_on ())
    {
        return;
    }
    g_object_get (current, "section-type", &section, NULL);
    if (group_id (section) == NULL)
    {
        return;
    }
    if (group_collapsed (section) != fold)
    {
        toggle_group (section);
    }
    /* Folded away from under the cursor: it goes to the header. */
    if (fold && !is_header (current))
    {
        set_cursor (sidebar, header_of (sidebar, section));
    }
}

/* Callback used for the "Open" menu items in the context menu */
static void
open_shortcut_cb (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    NautilusSidebar *sidebar = data;
    NautilusOpenFlags flags;

    flags = (NautilusOpenFlags) g_variant_get_int32 (parameter);
    open_row (sidebar->context_row, flags);
}

static void
rename_entry_changed (GtkEntry        *entry,
                      NautilusSidebar *sidebar)
{
    NautilusSidebarRowType type;
    char *name;
    char *uri;
    const char *new_name;
    gboolean found = FALSE;
    GtkWidget *row;

    new_name = gtk_editable_get_text (GTK_EDITABLE (sidebar->rename_entry));

    if (strcmp (new_name, "") == 0)
    {
        gtk_widget_set_sensitive (sidebar->rename_button, FALSE);
        gtk_label_set_label (GTK_LABEL (sidebar->rename_error), "");
        return;
    }

    for (row = gtk_widget_get_first_child (GTK_WIDGET (sidebar->list_box));
         row != NULL && !found;
         row = gtk_widget_get_next_sibling (row))
    {
        if (!GTK_IS_LIST_BOX_ROW (row))
        {
            continue;
        }

        g_object_get (row,
                      "place-type", &type,
                      "uri", &uri,
                      "label", &name,
                      NULL);

        if (type == NAUTILUS_SIDEBAR_ROW_BOOKMARK &&
            strcmp (uri, sidebar->rename_uri) != 0 &&
            strcmp (new_name, name) == 0)
        {
            found = TRUE;
        }

        g_free (uri);
        g_free (name);
    }

    gtk_widget_set_sensitive (sidebar->rename_button, !found);
    gtk_label_set_label (GTK_LABEL (sidebar->rename_error),
                         found ? _("This name is already taken") : "");
}

static void
do_rename (GtkButton       *button,
           NautilusSidebar *sidebar)
{
    g_autofree char *uri = g_steal_pointer (&sidebar->rename_uri);
    g_autofree char *new_text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (sidebar->rename_entry)));
    g_autoptr (GFile) file = g_file_new_for_uri (uri);
    NautilusBookmark *bookmark = nautilus_bookmark_list_get_bookmark (sidebar->bookmark_list,
                                                                      file);

    if (sidebar->rename_popover)
    {
        gtk_popover_popdown (GTK_POPOVER (sidebar->rename_popover));
    }

    if (bookmark == NULL)
    {
        g_warning ("Tried to rename non-existent bookmark of %s", sidebar->rename_uri);
        return;
    }

    nautilus_bookmark_set_name (bookmark, new_text);
}

static void
on_rename_popover_destroy (GtkWidget       *rename_popover,
                           NautilusSidebar *sidebar)
{
    if (sidebar)
    {
        sidebar->rename_popover = NULL;
        sidebar->rename_entry = NULL;
        sidebar->rename_button = NULL;
        sidebar->rename_error = NULL;
    }
}

static void
create_rename_popover (NautilusSidebar *sidebar)
{
    GtkWidget *popover;
    GtkWidget *grid;
    GtkWidget *label;
    GtkWidget *entry;
    GtkWidget *button;
    GtkWidget *error;
    char *str;

    if (sidebar->rename_popover)
    {
        return;
    }

    popover = gtk_popover_new ();
    gtk_widget_set_parent (popover, GTK_WIDGET (sidebar));
    /* Clean sidebar pointer when its destroyed, most of the times due to its
     * relative_to associated row being destroyed */
    g_signal_connect (popover, "destroy", G_CALLBACK (on_rename_popover_destroy), sidebar);
    gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_RIGHT);
    grid = gtk_grid_new ();
    gtk_popover_set_child (GTK_POPOVER (popover), grid);
    g_object_set (grid,
                  "margin-start", 10,
                  "margin-end", 10,
                  "margin-top", 10,
                  "margin-bottom", 10,
                  "row-spacing", 6,
                  "column-spacing", 6,
                  NULL);
    entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    g_signal_connect (entry, "changed", G_CALLBACK (rename_entry_changed), sidebar);
    str = g_strdup_printf ("<b>%s</b>", _("Name"));
    label = gtk_label_new (str);
    gtk_widget_set_halign (label, GTK_ALIGN_START);
    gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);
    g_free (str);
    button = gtk_button_new_with_mnemonic (_("_Rename"));
    gtk_widget_add_css_class (button, "suggested-action");
    g_signal_connect (button, "clicked", G_CALLBACK (do_rename), sidebar);
    error = gtk_label_new ("");
    gtk_widget_set_halign (error, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 2, 1);
    gtk_grid_attach (GTK_GRID (grid), entry, 0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), button, 1, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), error, 0, 2, 2, 1);
    gtk_popover_set_default_widget (GTK_POPOVER (popover), button);

    sidebar->rename_popover = popover;
    sidebar->rename_entry = entry;
    sidebar->rename_button = button;
    sidebar->rename_error = error;
}

/* Style the row differently while we show a popover for it.
 * Otherwise, the popover is 'pointing to nothing'. Since the
 * main popover and the rename popover interleave their hiding
 * and showing, we have to count to ensure that we don't loose
 * the state before the last popover is gone.
 *
 * This would be nicer as a state, but reusing hover for this
 * interferes with the normal handling of this state, so just
 * use a style class.
 */
static void
update_popover_shadowing (GtkWidget *row,
                          gboolean   shown)
{
    int count;

    count = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "popover-count"));
    count = shown ? count + 1 : count - 1;
    g_object_set_data (G_OBJECT (row), "popover-count", GINT_TO_POINTER (count));

    if (count > 0)
    {
        gtk_widget_add_css_class (row, "has-open-popup");
    }
    else
    {
        gtk_widget_remove_css_class (row, "has-open-popup");
    }
}

static void
set_prelight (NautilusSidebarRow *row)
{
    update_popover_shadowing (GTK_WIDGET (row), TRUE);
}

static void
unset_prelight (NautilusSidebarRow *row)
{
    update_popover_shadowing (GTK_WIDGET (row), FALSE);
}

static void
setup_popover_shadowing (GtkWidget       *popover,
                         NautilusSidebar *sidebar)
{
    g_signal_connect_object (popover, "map",
                             G_CALLBACK (set_prelight), sidebar->context_row,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (popover, "unmap",
                             G_CALLBACK (unset_prelight), sidebar->context_row,
                             G_CONNECT_SWAPPED);
}

static void
_popover_set_pointing_to_widget (GtkPopover *popover,
                                 GtkWidget  *target)
{
    GtkWidget *parent;
    double w, h;
    graphene_point_t target_origin;

    parent = gtk_widget_get_parent (GTK_WIDGET (popover));

    if (!gtk_widget_compute_point (target, parent,
                                   &GRAPHENE_POINT_INIT (0, 0),
                                   &target_origin))
    {
        return;
    }

    w = gtk_widget_get_width (GTK_WIDGET (target));
    h = gtk_widget_get_height (GTK_WIDGET (target));

    gtk_popover_set_pointing_to (popover, &(GdkRectangle){target_origin.x, target_origin.y, w, h});
}

static void
show_rename_popover (NautilusSidebarRow *row)
{
    char *name;
    char *uri;
    NautilusSidebar *sidebar;

    g_object_get (row,
                  "sidebar", &sidebar,
                  "label", &name,
                  "uri", &uri,
                  NULL);

    create_rename_popover (sidebar);

    g_set_str (&sidebar->rename_uri, uri);

    gtk_editable_set_text (GTK_EDITABLE (sidebar->rename_entry), name);

    _popover_set_pointing_to_widget (GTK_POPOVER (sidebar->rename_popover),
                                     GTK_WIDGET (row));

    setup_popover_shadowing (sidebar->rename_popover, sidebar);

    gtk_popover_popup (GTK_POPOVER (sidebar->rename_popover));
    gtk_widget_grab_focus (sidebar->rename_entry);

    g_free (name);
    g_free (uri);
    g_object_unref (sidebar);
}

static void
rename_bookmark (NautilusSidebarRow *row)
{
    NautilusSidebarRowType type;

    g_object_get (row, "place-type", &type, NULL);

    if (type != NAUTILUS_SIDEBAR_ROW_BOOKMARK)
    {
        return;
    }

    show_rename_popover (row);
}

static void
rename_shortcut_cb (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       data)
{
    NautilusSidebar *sidebar = data;

    rename_bookmark (sidebar->context_row);
}

static void
properties_cb (GSimpleAction *action,
               GVariant      *parameter,
               gpointer       data)
{
    NautilusSidebar *sidebar = data;
    GList *list;
    NautilusFile *file;
    g_autofree gchar *uri = NULL;

    g_object_get (sidebar->context_row, "uri", &uri, NULL);
    file = nautilus_file_get_by_uri (uri);
    list = g_list_append (NULL, file);
    nautilus_properties_present_dialog (list, GTK_WIDGET (sidebar));

    nautilus_file_list_free (list);
}

static void
empty_trash_cb (GSimpleAction *action,
                GVariant      *parameter,
                gpointer       data)
{
    NautilusSidebar *sidebar = data;
    nautilus_file_operations_empty_trash (GTK_WIDGET (sidebar), TRUE, NULL);
}

static void
action_history_trash_settings (GSimpleAction *action,
                               GVariant      *parameter,
                               gpointer       data)
{
    NautilusSidebar *self = data;
    GtkWindow *window = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
    const gchar *parameters = "('launch-panel', [<('privacy', [<'usage'>])>], @a{sv} {})";

    nautilus_dbus_launcher_call (nautilus_dbus_launcher_get (),
                                 NAUTILUS_DBUS_LAUNCHER_SETTINGS,
                                 "Activate",
                                 g_variant_new_parsed (parameters),
                                 window);
}

static void
remove_bookmark (NautilusSidebarRow *row)
{
    NautilusSidebarRowType type;
    g_autoptr (NautilusFile) file = NULL;
    NautilusSidebar *sidebar;

    g_object_get (row,
                  "sidebar", &sidebar,
                  "place-type", &type,
                  "file", &file,
                  NULL);

    if (type == NAUTILUS_SIDEBAR_ROW_BOOKMARK)
    {
        g_autoptr (GFile) location = nautilus_file_get_location (file);
        GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (sidebar));

        if (NAUTILUS_IS_WINDOW (root))
        {
            nautilus_window_remove_bookmark (NAUTILUS_WINDOW (root), location);
        }
        else
        {
            nautilus_bookmark_list_remove (sidebar->bookmark_list, location);
        }
    }

    g_object_unref (sidebar);
}

static void
remove_shortcut_cb (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       data)
{
    NautilusSidebar *sidebar = data;

    remove_bookmark (sidebar->context_row);
}

static void
mount_shortcut_cb (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
    NautilusSidebar *sidebar = data;
    GVolume *volume;

    g_object_get (sidebar->context_row,
                  "volume", &volume,
                  NULL);

    if (volume != NULL)
    {
        mount_volume (sidebar->context_row, volume, NAUTILUS_OPEN_FLAG_NORMAL);
    }

    g_object_unref (volume);
}

static GMountOperation *
get_mount_operation (NautilusSidebar *sidebar)
{
    GMountOperation *mount_op;

    mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (sidebar))));

    emit_mount_operation (sidebar, mount_op);

    return mount_op;
}

static GMountOperation *
get_unmount_operation (NautilusSidebar *sidebar)
{
    GMountOperation *mount_op;

    mount_op = gtk_mount_operation_new (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (sidebar))));

    emit_unmount_operation (sidebar, mount_op);

    return mount_op;
}

static void
do_unmount (GMount          *mount,
            NautilusSidebar *sidebar)
{
    if (mount != NULL)
    {
        GMountOperation *mount_op;
        GtkWindow *parent;

        mount_op = get_unmount_operation (sidebar);
        parent = gtk_mount_operation_get_parent (GTK_MOUNT_OPERATION (mount_op));
        nautilus_file_operations_unmount_mount_full (parent, mount, mount_op,
                                                     FALSE, NULL, NULL);
        g_object_unref (mount_op);
    }
}

static void
unmount_shortcut_cb (GSimpleAction *action,
                     GVariant      *parameter,
                     gpointer       data)
{
    NautilusSidebar *sidebar = data;
    GMount *mount;

    g_object_get (sidebar->context_row,
                  "mount", &mount,
                  NULL);

    do_unmount (mount, sidebar);

    if (mount)
    {
        g_object_unref (mount);
    }
}

static void
drive_stop_cb (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
    NautilusSidebar *sidebar;
    GError *error;
    char *primary;
    char *name;

    sidebar = user_data;

    error = NULL;
    if (!g_drive_stop_finish (G_DRIVE (source_object), res, &error))
    {
        if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
            name = g_drive_get_name (G_DRIVE (source_object));
            primary = g_strdup_printf (_("Unable to stop “%s”"), name);
            g_free (name);
            show_error_message (sidebar, primary, error->message);
            g_free (primary);
        }
        g_error_free (error);
    }

    g_object_unref (sidebar);
}

static void
drive_eject_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
    NautilusSidebar *sidebar;
    GError *error;
    char *primary;
    char *name;

    sidebar = user_data;

    error = NULL;
    if (!g_drive_eject_with_operation_finish (G_DRIVE (source_object), res, &error))
    {
        if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
            name = g_drive_get_name (G_DRIVE (source_object));
            primary = g_strdup_printf (_("Unable to eject “%s”"), name);
            g_free (name);
            show_error_message (sidebar, primary, error->message);
            g_free (primary);
        }
        g_error_free (error);
    }

    g_object_unref (sidebar);
}

static void
volume_eject_cb (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
    NautilusSidebar *sidebar;
    GError *error;
    char *primary;
    char *name;

    sidebar = user_data;

    error = NULL;
    if (!g_volume_eject_with_operation_finish (G_VOLUME (source_object), res, &error))
    {
        if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
            name = g_volume_get_name (G_VOLUME (source_object));
            primary = g_strdup_printf (_("Unable to eject “%s”"), name);
            g_free (name);
            show_error_message (sidebar, primary, error->message);
            g_free (primary);
        }
        g_error_free (error);
    }

    g_object_unref (sidebar);
}

static void
do_eject (GMount          *mount,
          GVolume         *volume,
          GDrive          *drive,
          NautilusSidebar *sidebar)
{
    GMountOperation *mount_op;
    GtkWindow *parent;

    mount_op = get_unmount_operation (sidebar);

    if (mount != NULL)
    {
        parent = gtk_mount_operation_get_parent (GTK_MOUNT_OPERATION (mount_op));
        nautilus_file_operations_unmount_mount_full (parent, mount, mount_op,
                                                     TRUE, NULL, NULL);
    }
    /* This code path is probably never reached since mount always exists,
     * and if it doesn't exists we don't offer a way to eject a volume or
     * drive in the UI. Do it for defensive programming
     */
    else if (volume != NULL)
    {
        g_volume_eject_with_operation (volume, 0, mount_op, NULL, volume_eject_cb,
                                       g_object_ref (sidebar));
    }
    /* This code path is probably never reached since mount always exists,
     * and if it doesn't exists we don't offer a way to eject a volume or
     * drive in the UI. Do it for defensive programming
     */
    else if (drive != NULL)
    {
        if (g_drive_can_stop (drive))
        {
            g_drive_stop (drive, 0, mount_op, NULL, drive_stop_cb,
                          g_object_ref (sidebar));
        }
        else
        {
            g_drive_eject_with_operation (drive, 0, mount_op, NULL, drive_eject_cb,
                                          g_object_ref (sidebar));
        }
    }
    g_object_unref (mount_op);
}

static void
eject_shortcut_cb (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
    NautilusSidebar *sidebar = data;
    GMount *mount;
    GVolume *volume;
    GDrive *drive;

    g_object_get (sidebar->context_row,
                  "mount", &mount,
                  "volume", &volume,
                  "drive", &drive,
                  NULL);

    do_eject (mount, volume, drive, sidebar);

    if (mount)
    {
        g_object_unref (mount);
    }
    if (volume)
    {
        g_object_unref (volume);
    }
    if (drive)
    {
        g_object_unref (drive);
    }
}

static gboolean
eject_or_unmount_bookmark (NautilusSidebarRow *row)
{
    gboolean can_unmount, can_eject;
    GMount *mount;
    GVolume *volume;
    GDrive *drive;
    gboolean ret;
    NautilusSidebar *sidebar;

    g_object_get (row,
                  "sidebar", &sidebar,
                  "mount", &mount,
                  "volume", &volume,
                  "drive", &drive,
                  NULL);
    ret = FALSE;

    check_unmount_and_eject (mount, volume, drive, &can_unmount, &can_eject);
    /* if we can eject, it has priority over unmount */
    if (can_eject)
    {
        do_eject (mount, volume, drive, sidebar);
        ret = TRUE;
    }
    else if (can_unmount)
    {
        do_unmount (mount, sidebar);
        ret = TRUE;
    }

    g_object_unref (sidebar);
    if (mount)
    {
        g_object_unref (mount);
    }
    if (volume)
    {
        g_object_unref (volume);
    }
    if (drive)
    {
        g_object_unref (drive);
    }

    return ret;
}

static gboolean
eject_or_unmount_selection (NautilusSidebar *sidebar)
{
    gboolean ret;
    GtkListBoxRow *row;

    row = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));
    ret = eject_or_unmount_bookmark (NAUTILUS_SIDEBAR_ROW (row));

    return ret;
}

static void
drive_poll_for_media_cb (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
    NautilusSidebar *sidebar;
    GError *error;
    char *primary;
    char *name;

    sidebar = NAUTILUS_PLACES_SIDEBAR (user_data);

    error = NULL;
    if (!g_drive_poll_for_media_finish (G_DRIVE (source_object), res, &error))
    {
        if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
            name = g_drive_get_name (G_DRIVE (source_object));
            primary = g_strdup_printf (_("Unable to poll “%s” for media changes"), name);
            g_free (name);
            show_error_message (sidebar, primary, error->message);
            g_free (primary);
        }
        g_error_free (error);
    }

    g_object_unref (sidebar);
}

static void
rescan_shortcut_cb (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       data)
{
    NautilusSidebar *sidebar = data;
    GDrive *drive;

    g_object_get (sidebar->context_row,
                  "drive", &drive,
                  NULL);

    if (drive != NULL)
    {
        g_drive_poll_for_media (drive, NULL, drive_poll_for_media_cb, g_object_ref (sidebar));
        g_object_unref (drive);
    }
}

static void
drive_start_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
    NautilusSidebar *sidebar;
    GError *error;
    char *primary;
    char *name;

    sidebar = NAUTILUS_PLACES_SIDEBAR (user_data);

    error = NULL;
    if (!g_drive_start_finish (G_DRIVE (source_object), res, &error))
    {
        if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
            name = g_drive_get_name (G_DRIVE (source_object));
            primary = g_strdup_printf (_("Unable to start “%s”"), name);
            g_free (name);
            show_error_message (sidebar, primary, error->message);
            g_free (primary);
        }
        g_error_free (error);
    }

    g_object_unref (sidebar);
}

static void
start_shortcut_cb (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
    NautilusSidebar *sidebar = data;
    GDrive *drive;

    g_object_get (sidebar->context_row,
                  "drive", &drive,
                  NULL);

    if (drive != NULL)
    {
        GMountOperation *mount_op;

        mount_op = get_mount_operation (sidebar);

        g_drive_start (drive, G_DRIVE_START_NONE, mount_op, NULL, drive_start_cb, g_object_ref (sidebar));

        g_object_unref (mount_op);
        g_object_unref (drive);
    }
}

static void
stop_shortcut_cb (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    NautilusSidebar *sidebar = data;
    GDrive *drive;

    g_object_get (sidebar->context_row,
                  "drive", &drive,
                  NULL);

    if (drive != NULL)
    {
        GMountOperation *mount_op;

        mount_op = get_unmount_operation (sidebar);
        g_drive_stop (drive, G_MOUNT_UNMOUNT_NONE, mount_op, NULL, drive_stop_cb,
                      g_object_ref (sidebar));

        g_object_unref (mount_op);
        g_object_unref (drive);
    }
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                NautilusSidebar       *sidebar)
{
    guint modifiers;
    GtkListBoxRow *row;

    row = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));
    if (row)
    {
        modifiers = gtk_accelerator_get_default_mod_mask ();

        if (keyval == GDK_KEY_Return ||
            keyval == GDK_KEY_KP_Enter ||
            keyval == GDK_KEY_ISO_Enter ||
            keyval == GDK_KEY_space)
        {
            NautilusOpenFlags open_flags = NAUTILUS_OPEN_FLAG_NORMAL;

            if ((state & modifiers) == GDK_SHIFT_MASK)
            {
                open_flags = NAUTILUS_OPEN_FLAG_NEW_TAB;
            }
            else if ((state & modifiers) == GDK_CONTROL_MASK)
            {
                open_flags = NAUTILUS_OPEN_FLAG_NEW_WINDOW;
            }

            open_row (NAUTILUS_SIDEBAR_ROW (row), open_flags);

            return TRUE;
        }

        if (keyval == GDK_KEY_Down &&
            (state & modifiers) == GDK_ALT_MASK)
        {
            return eject_or_unmount_selection (sidebar);
        }

        if ((keyval == GDK_KEY_Delete ||
             keyval == GDK_KEY_KP_Delete) &&
            (state & modifiers) == 0)
        {
            remove_bookmark (NAUTILUS_SIDEBAR_ROW (row));
            return TRUE;
        }

        if ((keyval == GDK_KEY_F2) &&
            (state & modifiers) == 0)
        {
            rename_bookmark (NAUTILUS_SIDEBAR_ROW (row));
            return TRUE;
        }

        if ((keyval == GDK_KEY_Menu) ||
            ((keyval == GDK_KEY_F10) &&
             (state & modifiers) == GDK_SHIFT_MASK))
        {
            popup_menu_cb (NAUTILUS_SIDEBAR_ROW (row));
            return TRUE;
        }
    }

    return FALSE;
}

static void
format_cb (GSimpleAction *action,
           GVariant      *variant,
           gpointer       data)
{
    NautilusSidebar *sidebar = data;
    g_autoptr (GVolume) volume = NULL;
    g_autofree gchar *device_identifier = NULL;
    GVariant *parameters;

    g_object_get (sidebar->context_row, "volume", &volume, NULL);
    device_identifier = g_volume_get_identifier (volume,
                                                 G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);

    parameters = g_variant_new_parsed ("(objectpath '/org/gnome/DiskUtility', @aay [], "
                                       "{'options': <{'block-device': <%s>, "
                                       "'format-device': <true> }> })", device_identifier);

    nautilus_dbus_launcher_call (nautilus_dbus_launcher_get (),
                                 NAUTILUS_DBUS_LAUNCHER_DISKS,
                                 "CommandLine",
                                 parameters,
                                 GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (sidebar))));
}

static GActionEntry entries[] =
{
    { .name = "open", .activate = open_shortcut_cb, .parameter_type = "i"},
    { .name = "open-other", .activate = open_shortcut_cb, .parameter_type = "i"},
    { .name = "remove", .activate = remove_shortcut_cb},
    { .name = "rename", .activate = rename_shortcut_cb},
    { .name = "mount", .activate = mount_shortcut_cb},
    { .name = "unmount", .activate = unmount_shortcut_cb},
    { .name = "eject", .activate = eject_shortcut_cb},
    { .name = "rescan", .activate = rescan_shortcut_cb},
    { .name = "start", .activate = start_shortcut_cb},
    { .name = "stop", .activate = stop_shortcut_cb},
    { .name = "properties", .activate = properties_cb},
    { .name = "empty-trash", .activate = empty_trash_cb},
    { .name = "history-trash-settings", .activate = action_history_trash_settings},
    { .name = "format", .activate = format_cb},
};

static gboolean
should_show_format_command (GVolume *volume)
{
    g_autofree gchar *unix_device_id = NULL;
    gboolean disks_available;
    g_autoptr (GFile) activation_root = NULL;

    if (volume == NULL || !G_IS_VOLUME (volume))
    {
        return FALSE;
    }

    /* Don't show format command for MTP and GPhoto2 devices. */
    activation_root = g_volume_get_activation_root (volume);
    if (activation_root != NULL && !g_file_is_native (activation_root))
    {
        return FALSE;
    }

    unix_device_id = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
    disks_available = nautilus_dbus_launcher_is_available (nautilus_dbus_launcher_get (),
                                                           NAUTILUS_DBUS_LAUNCHER_DISKS);

    return unix_device_id != NULL && disks_available;
}

static void
on_row_popover_destroy (GtkWidget       *row_popover,
                        NautilusSidebar *sidebar)
{
    if (sidebar)
    {
        sidebar->popover = NULL;
    }
}

#ifdef HAVE_CLOUDPROVIDERS
static void
build_popup_menu_using_gmenu (NautilusSidebarRow *row)
{
    CloudProvidersAccount *cloud_provider_account;
    NautilusSidebar *sidebar;
    GMenuModel *cloud_provider_menu;
    GActionGroup *cloud_provider_action_group;

    g_object_get (row,
                  "sidebar", &sidebar,
                  "cloud-provider-account", &cloud_provider_account,
                  NULL);

    /* Cloud provider account */
    if (cloud_provider_account)
    {
        GMenu *menu = g_menu_new ();
        GMenuItem *item;
        item = g_menu_item_new (_("_Open"), "row.open");
        g_menu_item_set_action_and_target_value (item, "row.open",
                                                 g_variant_new_int32 (NAUTILUS_OPEN_FLAG_NORMAL));
        g_menu_append_item (menu, item);
        g_object_unref (item);

        if (sidebar->open_flags & NAUTILUS_OPEN_FLAG_NEW_TAB)
        {
            item = g_menu_item_new (_("Open in New _Tab"), "row.open-other");
            g_menu_item_set_action_and_target_value (item, "row.open-other", g_variant_new_int32 (NAUTILUS_OPEN_FLAG_NEW_TAB));
            g_menu_append_item (menu, item);
            g_object_unref (item);
        }
        if (sidebar->open_flags & NAUTILUS_OPEN_FLAG_NEW_WINDOW)
        {
            item = g_menu_item_new (_("Open in New _Window"), "row.open-other");
            g_menu_item_set_action_and_target_value (item, "row.open-other", g_variant_new_int32 (NAUTILUS_OPEN_FLAG_NEW_WINDOW));
            g_menu_append_item (menu, item);
            g_object_unref (item);
        }
        cloud_provider_menu = cloud_providers_account_get_menu_model (cloud_provider_account);
        cloud_provider_action_group = cloud_providers_account_get_action_group (cloud_provider_account);
        if (cloud_provider_menu != NULL && cloud_provider_action_group != NULL)
        {
            g_menu_append_section (menu, NULL, cloud_provider_menu);
            gtk_widget_insert_action_group (GTK_WIDGET (sidebar),
                                            "cloudprovider",
                                            G_ACTION_GROUP (cloud_provider_action_group));
        }
        if (sidebar->popover)
        {
            gtk_widget_unparent (sidebar->popover);
        }

        sidebar->popover = gtk_popover_menu_new_from_model_full (G_MENU_MODEL (menu),
                                                                 GTK_POPOVER_MENU_NESTED);
        g_object_unref (menu);
        gtk_widget_set_parent (sidebar->popover, GTK_WIDGET (sidebar));
        gtk_widget_set_halign (sidebar->popover, GTK_ALIGN_START);
        gtk_popover_set_has_arrow (GTK_POPOVER (sidebar->popover), FALSE);
        g_signal_connect (sidebar->popover, "destroy",
                          G_CALLBACK (on_row_popover_destroy), sidebar);

        setup_popover_shadowing (sidebar->popover, sidebar);

        g_object_unref (sidebar);
        g_object_unref (cloud_provider_account);
    }
}
#endif

/* Constructs the popover for the sidebar row if needed */
static void
create_row_popover (NautilusSidebar    *sidebar,
                    NautilusSidebarRow *row)
{
    NautilusSidebarRowType type;
    GMenu *menu, *section;
    GMenuItem *item;
    GMount *mount;
    GVolume *volume;
    GDrive *drive;
    GAction *action;
    gboolean show_unmount, show_eject;
    gboolean show_stop;
    g_autofree gchar *uri = NULL;
    g_autoptr (GFile) file = NULL;
    g_autoptr (GFile) trash = NULL;
    gboolean is_trash = FALSE, is_recent = FALSE, show_properties = FALSE;
#ifdef HAVE_CLOUDPROVIDERS
    CloudProvidersAccount *cloud_provider_account;
#endif

    g_object_get (row,
                  "place-type", &type,
                  "drive", &drive,
                  "volume", &volume,
                  "mount", &mount,
                  "uri", &uri,
                  NULL);

    check_unmount_and_eject (mount, volume, drive, &show_unmount, &show_eject);
    if (uri != NULL)
    {
        file = g_file_new_for_uri (uri);
        trash = g_file_new_for_uri (SCHEME_TRASH ":///");
        is_trash = g_file_equal (trash, file);
        is_recent = g_file_has_uri_scheme (file, SCHEME_RECENT);
        show_properties = (g_file_is_native (file) || is_trash || mount != NULL);
    }

#ifdef HAVE_CLOUDPROVIDERS
    g_object_get (row, "cloud-provider-account", &cloud_provider_account, NULL);

    if (cloud_provider_account)
    {
        build_popup_menu_using_gmenu (row);
        return;
    }
#endif

    action = g_action_map_lookup_action (G_ACTION_MAP (sidebar->row_actions), "remove");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), (type == NAUTILUS_SIDEBAR_ROW_BOOKMARK));
    action = g_action_map_lookup_action (G_ACTION_MAP (sidebar->row_actions), "rename");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), (type == NAUTILUS_SIDEBAR_ROW_BOOKMARK));
    action = g_action_map_lookup_action (G_ACTION_MAP (sidebar->row_actions), "open");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), !gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (row)));
    action = g_action_map_lookup_action (G_ACTION_MAP (sidebar->row_actions), "empty-trash");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), !nautilus_trash_monitor_is_empty ());

    menu = g_menu_new ();
    section = g_menu_new ();

    item = g_menu_item_new (_("_Open"), "row.open");
    g_menu_item_set_action_and_target_value (item, "row.open",
                                             g_variant_new_int32 (NAUTILUS_OPEN_FLAG_NORMAL));
    g_menu_append_item (section, item);
    g_object_unref (item);

    if (sidebar->open_flags & NAUTILUS_OPEN_FLAG_NEW_TAB)
    {
        item = g_menu_item_new (_("Open in New _Tab"), "row.open-other");
        g_menu_item_set_action_and_target_value (item, "row.open-other",
                                                 g_variant_new_int32 (NAUTILUS_OPEN_FLAG_NEW_TAB));
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    if (sidebar->open_flags & NAUTILUS_OPEN_FLAG_NEW_WINDOW)
    {
        item = g_menu_item_new (_("Open in New _Window"), "row.open-other");
        g_menu_item_set_action_and_target_value (item, "row.open-other",
                                                 g_variant_new_int32 (NAUTILUS_OPEN_FLAG_NEW_WINDOW));
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
    g_object_unref (section);

    section = g_menu_new ();

    item = g_menu_item_new (_("_Remove from Bookmarks"), "row.remove");
    g_menu_item_set_attribute (item, "hidden-when", "s", "action-disabled");
    g_menu_append_item (section, item);
    g_object_unref (item);

    item = g_menu_item_new (_("_Rename"), "row.rename");
    g_menu_item_set_attribute (item, "hidden-when", "s", "action-disabled");
    g_menu_append_item (section, item);
    g_object_unref (item);

    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
    g_object_unref (section);

    if (is_recent)
    {
        g_autoptr (GMenu) settings_section = g_menu_new ();

        g_menu_insert (settings_section, 0,
                       _("File History _Settings…"), "row.history-trash-settings");

        g_menu_append_section (menu, NULL, G_MENU_MODEL (settings_section));
    }

    if (is_trash)
    {
        g_autoptr (GMenu) settings_section = g_menu_new ();
        g_autoptr (GMenu) empty_section = g_menu_new ();

        g_menu_insert (settings_section, 0, _("Trash _Settings…"), "row.history-trash-settings");

        g_menu_insert (empty_section, 0, _("_Empty Trash…"), "row.empty-trash");

        g_menu_append_section (menu, NULL, G_MENU_MODEL (settings_section));
        g_menu_append_section (menu, NULL, G_MENU_MODEL (empty_section));
    }

    section = g_menu_new ();

    if (volume != NULL && mount == NULL &&
        g_volume_can_mount (volume))
    {
        item = g_menu_item_new (_("_Mount"), "row.mount");
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    show_stop = (drive != NULL && g_drive_can_stop (drive));

    if (show_unmount && !show_stop)
    {
        item = g_menu_item_new (_("_Unmount"), "row.unmount");
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    if (show_eject)
    {
        item = g_menu_item_new (_("_Eject"), "row.eject");
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    if (drive != NULL &&
        g_drive_is_media_removable (drive) &&
        !g_drive_is_media_check_automatic (drive) &&
        g_drive_can_poll_for_media (drive))
    {
        item = g_menu_item_new (_("_Detect Media"), "row.rescan");
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    if (drive != NULL &&
        (g_drive_can_start (drive) || g_drive_can_start_degraded (drive)))
    {
        const guint ss_type = g_drive_get_start_stop_type (drive);
        const char *start_label = _("_Start");

        if (ss_type == G_DRIVE_START_STOP_TYPE_SHUTDOWN)
        {
            start_label = _("_Power On");
        }
        else if (ss_type == G_DRIVE_START_STOP_TYPE_NETWORK)
        {
            start_label = _("_Connect Drive");
        }
        else if (ss_type == G_DRIVE_START_STOP_TYPE_MULTIDISK)
        {
            start_label = _("_Start Multi-disk Device");
        }
        else if (ss_type == G_DRIVE_START_STOP_TYPE_PASSWORD)
        {
            start_label = _("_Unlock Device");
        }

        item = g_menu_item_new (start_label, "row.start");
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    if (show_stop)
    {
        const guint ss_type = g_drive_get_start_stop_type (drive);
        const char *stop_label = _("_Stop");

        if (ss_type == G_DRIVE_START_STOP_TYPE_SHUTDOWN)
        {
            stop_label = _("_Safely Remove Drive");
        }
        else if (ss_type == G_DRIVE_START_STOP_TYPE_NETWORK)
        {
            stop_label = _("_Disconnect Drive");
        }
        else if (ss_type == G_DRIVE_START_STOP_TYPE_MULTIDISK)
        {
            stop_label = _("_Stop Multi-disk Device");
        }
        else if (ss_type == G_DRIVE_START_STOP_TYPE_PASSWORD)
        {
            stop_label = _("_Lock Device");
        }

        item = g_menu_item_new (stop_label, "row.stop");
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    if (should_show_format_command (volume))
    {
        item = g_menu_item_new (_("Format…"), "row.format");
        g_menu_append_item (section, item);
        g_object_unref (item);
    }

    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
    g_object_unref (section);

    if (show_properties)
    {
        section = g_menu_new ();
        item = g_menu_item_new (_("Properties"), "row.properties");
        g_menu_append_item (section, item);
        g_object_unref (item);

        g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
        g_object_unref (section);
    }

    append_color_section (menu);
    {
        g_autoptr (GMenu) sections = sections_menu_new ();
        g_autoptr (GMenu) last = g_menu_new ();

        g_menu_append_submenu (last, _("Sidebar _Sections"), G_MENU_MODEL (sections));
        g_menu_append_section (menu, NULL, G_MENU_MODEL (last));
    }
    sidebar->popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    g_object_unref (menu);
    gtk_widget_set_parent (sidebar->popover, GTK_WIDGET (sidebar));
    add_color_swatches (sidebar->popover, GTK_WIDGET (row));
    gtk_widget_set_halign (sidebar->popover, GTK_ALIGN_START);
    gtk_popover_set_has_arrow (GTK_POPOVER (sidebar->popover), FALSE);
    g_signal_connect (sidebar->popover, "destroy", G_CALLBACK (on_row_popover_destroy), sidebar);

    setup_popover_shadowing (sidebar->popover, sidebar);
}

static void
show_row_popover (NautilusSidebarRow *row,
                  double              x,
                  double              y)
{
    NautilusSidebar *sidebar;
    graphene_point_t p_in_sidebar;

    g_object_get (row, "sidebar", &sidebar, NULL);

    g_clear_pointer (&sidebar->popover, gtk_widget_unparent);

    sidebar->context_row = row;
    create_row_popover (sidebar, row);

    if ((x == -1 && y == -1) ||
        !gtk_widget_compute_point (GTK_WIDGET (row), GTK_WIDGET (sidebar),
                                   &GRAPHENE_POINT_INIT (x, y),
                                   &p_in_sidebar))
    {
        _popover_set_pointing_to_widget (GTK_POPOVER (sidebar->popover), GTK_WIDGET (row));
    }
    else
    {
        gtk_popover_set_pointing_to (GTK_POPOVER (sidebar->popover),
                                     &(GdkRectangle){p_in_sidebar.x, p_in_sidebar.y, 0, 0});
        /* Above the pointer low in the sidebar, as popup_at_point () does. */
        gtk_popover_set_position (GTK_POPOVER (sidebar->popover),
                                  p_in_sidebar.y > gtk_widget_get_height (GTK_WIDGET (sidebar)) / 2
                                  ? GTK_POS_TOP : GTK_POS_BOTTOM);
    }

    gtk_popover_popup (GTK_POPOVER (sidebar->popover));

    g_object_unref (sidebar);
}

static void
on_row_activated (GtkListBox    *list_box,
                  GtkListBoxRow *row,
                  gpointer       user_data)
{
    NautilusSidebarRow *selected_row;

    /* Avoid to open a location if the user is dragging. Changing the location
     * while dragging usually makes clients changing the view of the files, which
     * is confusing while the user has the attention on the drag
     */
    if (NAUTILUS_PLACES_SIDEBAR (user_data)->dragging_over)
    {
        return;
    }

    NautilusSidebarRowType place_type;
    NautilusSidebarSectionType section;

    set_cursor (NAUTILUS_PLACES_SIDEBAR (user_data), NULL);
    g_object_get (row, "place-type", &place_type, "section-type", &section, NULL);
    if (place_type == NAUTILUS_SIDEBAR_ROW_GROUP_HEADER)
    {
        toggle_group (section);
        return;
    }
    if (place_type == NAUTILUS_SIDEBAR_ROW_TAG)
    {
        on_tag_row_activated (NAUTILUS_PLACES_SIDEBAR (user_data), NAUTILUS_SIDEBAR_ROW (row));
        return;
    }

    selected_row = NAUTILUS_SIDEBAR_ROW (gtk_list_box_get_selected_row (list_box));
    open_row (selected_row, 0);
}

static void
on_row_pressed (GtkGestureClick    *gesture,
                int                 n_press,
                double              x,
                double              y,
                NautilusSidebarRow *row)
{
    NautilusSidebar *sidebar;
    NautilusSidebarSectionType section_type;

    g_object_get (row,
                  "sidebar", &sidebar,
                  "section_type", &section_type,
                  NULL);

    if (section_type == NAUTILUS_SIDEBAR_SECTION_BOOKMARKS)
    {
        sidebar->drag_row = GTK_WIDGET (row);
        sidebar->drag_row_x = (int) x;
        sidebar->drag_row_y = (int) y;
    }

    g_object_unref (sidebar);
}

static void
on_row_released (GtkGestureClick    *gesture,
                 int                 n_press,
                 double              x,
                 double              y,
                 NautilusSidebarRow *row)
{
    NautilusSidebar *sidebar;
    NautilusSidebarSectionType section_type;
    guint button, state;

    g_object_get (row,
                  "sidebar", &sidebar,
                  "section_type", &section_type,
                  NULL);

    button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
    state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));

    if (row)
    {
        if (button == 2)
        {
            NautilusOpenFlags open_flags = NAUTILUS_OPEN_FLAG_NORMAL;

            open_flags = (state & GDK_CONTROL_MASK) ?
                         NAUTILUS_OPEN_FLAG_NEW_WINDOW :
                         NAUTILUS_OPEN_FLAG_NEW_TAB;

            open_row (NAUTILUS_SIDEBAR_ROW (row), open_flags);
            gtk_gesture_set_state (GTK_GESTURE (gesture),
                                   GTK_EVENT_SEQUENCE_CLAIMED);
        }
        else if (button == 3)
        {
            NautilusSidebarRowType place_type;

            g_object_get (row, "place-type", &place_type, NULL);
            if (place_type == NAUTILUS_SIDEBAR_ROW_GROUP_HEADER)
            {
                show_group_menu (GTK_WIDGET (row), x, y);
            }
            else if (place_type == NAUTILUS_SIDEBAR_ROW_TAG)
            {
                show_tag_menu (GTK_WIDGET (row), x, y);
            }
            else
            {
                show_row_popover (NAUTILUS_SIDEBAR_ROW (row), x, y);
            }
        }
    }
}

static void
on_row_dragged (GtkGestureDrag     *gesture,
                double              x,
                double              y,
                NautilusSidebarRow *row)
{
    NautilusSidebar *sidebar;

    g_object_get (row, "sidebar", &sidebar, NULL);

    if (sidebar->drag_row == NULL || sidebar->dragging_over)
    {
        g_object_unref (sidebar);
        return;
    }

    if (gtk_drag_check_threshold (GTK_WIDGET (row), 0, 0, x, y))
    {
        double start_x, start_y;
        GdkContentProvider *content;
        GdkSurface *surface;
        GdkDevice *device;
        gint width, height;
        GtkWidget *drag_widget;
        GdkDrag *drag;
        graphene_point_t drag_point;

        gtk_gesture_drag_get_start_point (gesture, &start_x, &start_y);
        if (!gtk_widget_compute_point (GTK_WIDGET (row), GTK_WIDGET (sidebar),
                                       &GRAPHENE_POINT_INIT (start_x, start_y),
                                       &drag_point))
        {
            g_return_if_reached ();
        }

        sidebar->dragging_over = TRUE;

        content = gdk_content_provider_new_typed (NAUTILUS_TYPE_SIDEBAR_ROW, sidebar->drag_row);

        surface = gtk_native_get_surface (gtk_widget_get_native (GTK_WIDGET (sidebar)));
        device = gtk_gesture_get_device (GTK_GESTURE (gesture));

        drag = gdk_drag_begin (surface, device, content, GDK_ACTION_MOVE, drag_point.x, drag_point.y);

        g_object_unref (content);

        gdk_drag_set_hotspot (drag, start_x, start_y);
        g_signal_connect (drag, "dnd-finished", G_CALLBACK (dnd_finished_cb), sidebar);
        g_signal_connect (drag, "cancel", G_CALLBACK (dnd_cancel_cb), sidebar);

        width = gtk_widget_get_width (sidebar->drag_row);
        height = gtk_widget_get_height (sidebar->drag_row);
        gtk_widget_set_visible (sidebar->drag_row, FALSE);

        drag_widget = GTK_WIDGET (nautilus_sidebar_row_clone (NAUTILUS_SIDEBAR_ROW (sidebar->drag_row)));
        sidebar->drag_row_height = height;
        gtk_widget_set_size_request (drag_widget, width, height);
        gtk_widget_set_opacity (drag_widget, 0.8);
        gtk_widget_set_margin_start (drag_widget, 14);

        gtk_drag_icon_set_child (GTK_DRAG_ICON (gtk_drag_icon_get_for_drag (drag)), drag_widget);

        g_object_unref (drag);
    }

    g_object_unref (sidebar);
}

static void
popup_menu_cb (NautilusSidebarRow *row)
{
    show_row_popover (row, -1, -1);
}

static void
long_press_cb (GtkGesture      *gesture,
               double           x,
               double           y,
               NautilusSidebar *sidebar)
{
    GtkWidget *row;

    row = GTK_WIDGET (gtk_list_box_get_row_at_y (GTK_LIST_BOX (sidebar->list_box), y));
    if (NAUTILUS_IS_SIDEBAR_ROW (row))
    {
        popup_menu_cb (NAUTILUS_SIDEBAR_ROW (row));
    }
}

static int
compare_volume (GVolume *a,
                GVolume *b)
{
    if (a == NULL || b == NULL)
    {
        return 0;
    }

    g_autofree gchar *volume_id_1 = g_volume_get_identifier (a, G_DRIVE_IDENTIFIER_KIND_UNIX_DEVICE);
    g_autofree gchar *volume_id_2 = g_volume_get_identifier (b, G_DRIVE_IDENTIFIER_KIND_UNIX_DEVICE);

    if (volume_id_1 == NULL || volume_id_2 == NULL)
    {
        return 0;
    }

    const gchar *loop_device_prefix = "/dev/loop";
    gboolean volume_1_is_loop = g_str_has_prefix (volume_id_1, loop_device_prefix);
    gboolean volume_2_is_loop = g_str_has_prefix (volume_id_2, loop_device_prefix);

    /* Always sort loop devices last */
    if (volume_1_is_loop != volume_2_is_loop)
    {
        return volume_1_is_loop - volume_2_is_loop;
    }

    return (g_strcmp0 (volume_id_1, volume_id_2));
}

static int
list_box_sort_func (GtkListBoxRow *row1,
                    GtkListBoxRow *row2,
                    gpointer       user_data)
{
    NautilusSidebarSectionType section_type_1, section_type_2;
    NautilusSidebarRowType place_type_1, place_type_2;
    g_autofree gchar *label_1 = NULL, *label_2 = NULL;
    int index_1, index_2;

    g_object_get (row1,
                  "label", &label_1,
                  "place-type", &place_type_1,
                  "section-type", &section_type_1,
                  "order-index", &index_1,
                  NULL);
    g_object_get (row2,
                  "label", &label_2,
                  "place-type", &place_type_2,
                  "section-type", &section_type_2,
                  "order-index", &index_2,
                  NULL);

    if (section_type_1 != section_type_2)
    {
        if (groups_on ())
        {
            return group_rank (section_type_1) - group_rank (section_type_2);
        }
        /* Order by section. That means the order in the enum of section types
         * define the actual order of them in the list */
        return section_type_1 - section_type_2;
    }

    /* We order the bookmarks sections based on the bookmark index that we
     * set on the row as an order-index property, but we have to deal with
     * the placeholder row wanted to be between two consecutive bookmarks,
     * with two consecutive order-index values which is the usual case.
     * For that, in the list box sort func we treat bookmark placeholders as
     * having the same as place type as normal bookmarks so that the index is
     * used for ordering.
     */
    if (place_type_1 == NAUTILUS_SIDEBAR_ROW_BOOKMARK_PLACEHOLDER)
    {
        place_type_1 = NAUTILUS_SIDEBAR_ROW_BOOKMARK;
    }
    if (place_type_2 == NAUTILUS_SIDEBAR_ROW_BOOKMARK_PLACEHOLDER)
    {
        place_type_2 = NAUTILUS_SIDEBAR_ROW_BOOKMARK;
    }

    if (place_type_1 != place_type_2)
    {
        return place_type_1 - place_type_2;
    }

    if (section_type_1 == NAUTILUS_SIDEBAR_SECTION_MOUNTS)
    {
        g_autoptr (GVolume) volume_1 = NULL, volume_2 = NULL;

        g_object_get (row1, "volume", &volume_1, NULL);
        g_object_get (row2, "volume", &volume_2, NULL);

        int retval = compare_volume (volume_1, volume_2);

        if (retval != 0)
        {
            return retval;
        }

        return g_utf8_collate (label_1, label_2);
    }

    return index_1 - index_2;
}

static void
update_hostname (NautilusSidebar *sidebar)
{
    GVariant *variant;
    gsize len;
    const char *hostname;

    if (sidebar->hostnamed_proxy == NULL)
    {
        return;
    }

    variant = g_dbus_proxy_get_cached_property (sidebar->hostnamed_proxy,
                                                "PrettyHostname");
    if (variant == NULL)
    {
        return;
    }

    hostname = g_variant_get_string (variant, &len);
    if (len > 0 &&
        g_set_str (&sidebar->hostname, hostname))
    {
        update_places (sidebar);
    }

    g_variant_unref (variant);
}

static void
hostname_proxy_new_cb (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
    NautilusSidebar *sidebar = user_data;
    GError *error = NULL;
    GDBusProxy *proxy;

    proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_error_free (error);
        return;
    }

    sidebar->hostnamed_proxy = proxy;
    g_clear_object (&sidebar->hostnamed_cancellable);

    if (error != NULL)
    {
        g_debug ("Failed to create D-Bus proxy: %s", error->message);
        g_error_free (error);
        return;
    }

    g_signal_connect_swapped (sidebar->hostnamed_proxy,
                              "g-properties-changed",
                              G_CALLBACK (update_hostname),
                              sidebar);
    update_hostname (sidebar);
}

static void
create_volume_monitor (NautilusSidebar *sidebar)
{
    g_assert (sidebar->volume_monitor == NULL);

    sidebar->volume_monitor = g_volume_monitor_get ();

    g_signal_connect_object (sidebar->volume_monitor, "volume_added",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (sidebar->volume_monitor, "volume_removed",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (sidebar->volume_monitor, "volume_changed",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (sidebar->volume_monitor, "mount_added",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (sidebar->volume_monitor, "mount_removed",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (sidebar->volume_monitor, "mount_changed",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (sidebar->volume_monitor, "drive_disconnected",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (sidebar->volume_monitor, "drive_connected",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (sidebar->volume_monitor, "drive_changed",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
}

static void
shell_shows_desktop_changed (GtkSettings *settings,
                             GParamSpec  *pspec,
                             gpointer     user_data)
{
    NautilusSidebar *sidebar = user_data;
    gboolean show_desktop;

    g_assert (settings == sidebar->gtk_settings);

    g_object_get (settings, "gtk-shell-shows-desktop", &show_desktop, NULL);

    if (show_desktop != sidebar->show_desktop)
    {
        sidebar->show_desktop = show_desktop;
        update_places (sidebar);
    }
}

static void
update_location (NautilusSidebar *self)
{
    GFile *location = NULL;

    if (self->window_slot != NULL &&
        !nautilus_window_slot_get_search_global (self->window_slot))
    {
        location = nautilus_window_slot_get_location (self->window_slot);
    }

    nautilus_sidebar_set_location (self, location);
}

/* A right click where there is no row: which groups to show. */
static void
on_empty_area_pressed (GtkGestureClick *gesture,
                       int              n_press,
                       double           x,
                       double           y,
                       NautilusSidebar *sidebar)
{
    graphene_point_t point;
    g_autoptr (GMenu) menu = g_menu_new ();
    GtkWidget *popover;

    if (!gtk_widget_compute_point (sidebar->swin, sidebar->list_box, &GRAPHENE_POINT_INIT (x, y), &point) ||
        (point.y >= 0 && gtk_list_box_get_row_at_y (GTK_LIST_BOX (sidebar->list_box), (int) point.y) != NULL))
    {
        return;
    }
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    append_sections_section (menu);
    popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    popup_at_point (popover, sidebar->swin, x, y);
}

static void
nautilus_sidebar_init (NautilusSidebar *sidebar)
{
    GtkDropTarget *target;
    gboolean show_desktop;
    GtkEventController *controller;
    GtkGesture *gesture;

    create_volume_monitor (sidebar);

    sidebar->slot_signal_group = g_signal_group_new (NAUTILUS_TYPE_WINDOW_SLOT);
    g_signal_group_connect_object (sidebar->slot_signal_group, "notify::location",
                                   G_CALLBACK (update_location), sidebar,
                                   G_CONNECT_SWAPPED);
    g_signal_group_connect_object (sidebar->slot_signal_group, "notify::search-global",
                                   G_CALLBACK (update_location), sidebar,
                                   G_CONNECT_SWAPPED);

    sidebar->open_flags = NAUTILUS_OPEN_FLAG_NORMAL;

    NautilusApplication *app = NAUTILUS_APPLICATION (g_application_get_default ());
    sidebar->bookmark_list = nautilus_application_get_bookmarks (app);
    g_signal_connect_object (sidebar->bookmark_list, "changed",
                             G_CALLBACK (update_places), sidebar,
                             G_CONNECT_SWAPPED);

    g_signal_connect_object (nautilus_trash_monitor_get (), "trash-state-changed",
                             G_CALLBACK (update_trash_icon), sidebar,
                             G_CONNECT_SWAPPED);

    sidebar->swin = gtk_scrolled_window_new ();
    gtk_widget_set_parent (sidebar->swin, GTK_WIDGET (sidebar));

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sidebar->swin),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);

    install_section_actions (GTK_WIDGET (sidebar));

    /* list box */
    sidebar->list_box = gtk_list_box_new ();
    gtk_widget_add_css_class (sidebar->list_box, "navigation-sidebar");

    gtk_list_box_set_header_func (GTK_LIST_BOX (sidebar->list_box),
                                  list_box_header_func, sidebar, NULL);
    gtk_list_box_set_sort_func (GTK_LIST_BOX (sidebar->list_box),
                                list_box_sort_func, NULL, NULL);
    gtk_list_box_set_filter_func (GTK_LIST_BOX (sidebar->list_box),
                                  list_box_filter_func, sidebar, NULL);
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-collapsed",
                             G_CALLBACK (on_collapsed_changed), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-hidden-groups",
                             G_CALLBACK (on_order_changed), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-group-order",
                             G_CALLBACK (on_order_changed), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-hidden-places",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-groups",
                             G_CALLBACK (update_places), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_labels_get (), "tags-changed",
                             G_CALLBACK (on_tags_changed), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (gtk_recent_manager_get_default (), "changed",
                             G_CALLBACK (on_recent_changed), sidebar, G_CONNECT_SWAPPED);
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (sidebar->list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (sidebar->list_box), TRUE);
    gtk_list_box_set_tab_behavior (GTK_LIST_BOX (sidebar->list_box), GTK_LIST_TAB_ITEM);

    g_signal_connect (sidebar->list_box, "row-activated",
                      G_CALLBACK (on_row_activated), sidebar);

    controller = gtk_event_controller_key_new ();
    g_signal_connect (controller, "key-pressed",
                      G_CALLBACK (on_key_pressed), sidebar);
    gtk_widget_add_controller (sidebar->list_box, controller);

    gesture = gtk_gesture_long_press_new ();
    gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (gesture), TRUE);
    g_signal_connect (gesture, "pressed",
                      G_CALLBACK (long_press_cb), sidebar);
    gtk_widget_add_controller (GTK_WIDGET (sidebar), GTK_EVENT_CONTROLLER (gesture));

    /* DND support */
    target = gtk_drop_target_new (G_TYPE_INVALID, GDK_ACTION_ALL);
    gtk_drop_target_set_preload (target, TRUE);
    gtk_drop_target_set_gtypes (target, (GType[2]) { NAUTILUS_TYPE_SIDEBAR_ROW, GDK_TYPE_FILE_LIST }, 2);
    g_signal_connect (target, "enter", G_CALLBACK (drag_motion_callback), sidebar);
    g_signal_connect (target, "motion", G_CALLBACK (drag_motion_callback), sidebar);
    g_signal_connect (target, "drop", G_CALLBACK (drag_drop_callback), sidebar);
    g_signal_connect (target, "leave", G_CALLBACK (drag_leave_callback), sidebar);
    gtk_widget_add_controller (sidebar->list_box, GTK_EVENT_CONTROLLER (target));

    sidebar->drag_row = NULL;
    sidebar->row_placeholder = NULL;
    sidebar->dragging_over = FALSE;

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sidebar->swin), sidebar->list_box);
    {
        GtkGesture *empty_click = gtk_gesture_click_new ();

        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (empty_click), GDK_BUTTON_SECONDARY);
        g_signal_connect (empty_click, "pressed", G_CALLBACK (on_empty_area_pressed), sidebar);
        gtk_widget_add_controller (sidebar->swin, GTK_EVENT_CONTROLLER (empty_click));
    }

    sidebar->hostname = g_strdup (_("Device"));
    sidebar->hostnamed_cancellable = g_cancellable_new ();
    g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                              G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                              NULL,
                              "org.freedesktop.hostname1",
                              "/org/freedesktop/hostname1",
                              "org.freedesktop.hostname1",
                              sidebar->hostnamed_cancellable,
                              hostname_proxy_new_cb,
                              sidebar);

    sidebar->drop_state = DROP_STATE_NORMAL;

    /* Don't bother trying to trace this across hierarchy changes... */
    sidebar->gtk_settings = gtk_settings_get_default ();
    g_signal_connect (sidebar->gtk_settings, "notify::gtk-shell-shows-desktop",
                      G_CALLBACK (shell_shows_desktop_changed), sidebar);
    g_object_get (sidebar->gtk_settings, "gtk-shell-shows-desktop", &show_desktop, NULL);
    sidebar->show_desktop = show_desktop;

    /* Cloud providers */
#ifdef HAVE_CLOUDPROVIDERS
    sidebar->cloud_manager = cloud_providers_collector_dup_singleton ();
    g_signal_connect_swapped (sidebar->cloud_manager,
                              "providers-changed",
                              G_CALLBACK (update_places),
                              sidebar);
#endif

    sidebar->show_trash = TRUE;

    /* populate the sidebar */
    update_places (sidebar);

    sidebar->row_actions = G_ACTION_GROUP (g_simple_action_group_new ());
    g_action_map_add_action_entries (G_ACTION_MAP (sidebar->row_actions),
                                     entries, G_N_ELEMENTS (entries),
                                     sidebar);
    gtk_widget_insert_action_group (GTK_WIDGET (sidebar), "row", sidebar->row_actions);
    {
        static const GActionEntry color_entries[] =
        {
            { .name = "set", .activate = on_row_color, .parameter_type = "i" },
        };
        g_autoptr (GSimpleActionGroup) colors = g_simple_action_group_new ();

        g_action_map_add_action_entries (G_ACTION_MAP (colors), color_entries,
                                         G_N_ELEMENTS (color_entries), sidebar);
        gtk_widget_insert_action_group (GTK_WIDGET (sidebar), "row-color", G_ACTION_GROUP (colors));
    }
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-colors",
                             G_CALLBACK (update_row_colors), sidebar, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_labels_get (), "colors-changed",
                             G_CALLBACK (update_row_colors), sidebar, G_CONNECT_SWAPPED);

    gtk_accessible_update_property (GTK_ACCESSIBLE (sidebar),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, _("Sidebar"),
                                    GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                    _("List of common shortcuts, mountpoints, and bookmarks."), -1);
}

static void
nautilus_sidebar_set_property (GObject      *obj,
                               guint         property_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
    NautilusSidebar *sidebar = NAUTILUS_PLACES_SIDEBAR (obj);

    switch (property_id)
    {
        case PROP_LOCATION:
        {
            nautilus_sidebar_set_location (sidebar, g_value_get_object (value));
            break;
        }

        case PROP_OPEN_FLAGS:
        {
            nautilus_sidebar_set_open_flags (sidebar, g_value_get_flags (value));
            break;
        }

        case PROP_WINDOW_SLOT:
        {
            if (g_set_object (&sidebar->window_slot, g_value_get_object (value)))
            {
                g_signal_group_set_target (sidebar->slot_signal_group, sidebar->window_slot);
                update_location (sidebar);
            }
            break;
        }

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, property_id, pspec);
            break;
        }
    }
}

static void
nautilus_sidebar_get_property (GObject    *obj,
                               guint       property_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
    NautilusSidebar *sidebar = NAUTILUS_PLACES_SIDEBAR (obj);

    switch (property_id)
    {
        case PROP_LOCATION:
        {
            g_value_take_object (value, nautilus_sidebar_get_location (sidebar));
            break;
        }

        case PROP_OPEN_FLAGS:
        {
            g_value_set_flags (value, nautilus_sidebar_get_open_flags (sidebar));
            break;
        }

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, property_id, pspec);
            break;
        }
    }
}

static void
nautilus_sidebar_dispose (GObject *object)
{
    NautilusSidebar *sidebar;
#ifdef HAVE_CLOUDPROVIDERS
    GList *l;
#endif

    sidebar = NAUTILUS_PLACES_SIDEBAR (object);
    g_clear_weak_pointer (&sidebar->stepped_row);
    g_clear_weak_pointer (&sidebar->color_row);

    g_clear_object (&sidebar->window_slot);
    g_clear_object (&sidebar->slot_signal_group);

    g_clear_pointer (&sidebar->popover, gtk_widget_unparent);

    if (sidebar->rename_popover)
    {
        gtk_widget_unparent (sidebar->rename_popover);
        sidebar->rename_popover = NULL;
        sidebar->rename_entry = NULL;
        sidebar->rename_button = NULL;
        sidebar->rename_error = NULL;
    }

    if (sidebar->trash_row)
    {
        g_object_remove_weak_pointer (G_OBJECT (sidebar->trash_row),
                                      (gpointer *) &sidebar->trash_row);
        sidebar->trash_row = NULL;
    }

    if (sidebar->volume_monitor != NULL)
    {
        g_signal_handlers_disconnect_by_func (sidebar->volume_monitor,
                                              update_places, sidebar);
        g_clear_object (&sidebar->volume_monitor);
    }

    if (sidebar->hostnamed_cancellable != NULL)
    {
        g_cancellable_cancel (sidebar->hostnamed_cancellable);
        g_clear_object (&sidebar->hostnamed_cancellable);
    }

    g_clear_object (&sidebar->hostnamed_proxy);
    g_free (sidebar->hostname);
    sidebar->hostname = NULL;

    if (sidebar->gtk_settings)
    {
        g_signal_handlers_disconnect_by_func (sidebar->gtk_settings, shell_shows_desktop_changed, sidebar);
        sidebar->gtk_settings = NULL;
    }

    g_clear_object (&sidebar->current_location);
    g_clear_pointer (&sidebar->rename_uri, g_free);

    g_clear_handle_id (&sidebar->hover_timer_id, g_source_remove);

#ifdef HAVE_CLOUDPROVIDERS
    for (l = sidebar->unready_accounts; l != NULL; l = l->next)
    {
        g_signal_handlers_disconnect_by_data (l->data, sidebar);
    }
    g_list_free_full (sidebar->unready_accounts, g_object_unref);
    sidebar->unready_accounts = NULL;
    if (sidebar->cloud_manager)
    {
        g_signal_handlers_disconnect_by_data (sidebar->cloud_manager, sidebar);
        for (l = cloud_providers_collector_get_providers (sidebar->cloud_manager);
             l != NULL; l = l->next)
        {
            g_signal_handlers_disconnect_by_data (l->data, sidebar);
        }
        g_object_unref (sidebar->cloud_manager);
        sidebar->cloud_manager = NULL;
    }
#endif

    G_OBJECT_CLASS (nautilus_sidebar_parent_class)->dispose (object);
}

static void
nautilus_sidebar_finalize (GObject *object)
{
    NautilusSidebar *sidebar = NAUTILUS_PLACES_SIDEBAR (object);

    g_clear_object (&sidebar->row_actions);

    g_clear_pointer (&sidebar->swin, gtk_widget_unparent);

    G_OBJECT_CLASS (nautilus_sidebar_parent_class)->finalize (object);
}

static void
nautilus_sidebar_measure (GtkWidget      *widget,
                          GtkOrientation  orientation,
                          int             for_size,
                          int            *minimum,
                          int            *natural,
                          int            *minimum_baseline,
                          int            *natural_baseline)
{
    NautilusSidebar *sidebar = NAUTILUS_PLACES_SIDEBAR (widget);

    gtk_widget_measure (sidebar->swin,
                        orientation,
                        for_size,
                        minimum, natural,
                        minimum_baseline, natural_baseline);
}

static void
nautilus_sidebar_size_allocate (GtkWidget *widget,
                                int        width,
                                int        height,
                                int        baseline)
{
    NautilusSidebar *sidebar = NAUTILUS_PLACES_SIDEBAR (widget);

    gtk_widget_size_allocate (sidebar->swin,
                              &(GtkAllocation) { 0, 0, width, height },
                              baseline);

    if (sidebar->popover)
    {
        gtk_popover_present (GTK_POPOVER (sidebar->popover));
    }

    if (sidebar->rename_popover)
    {
        gtk_popover_present (GTK_POPOVER (sidebar->rename_popover));
    }
}

static void
nautilus_sidebar_class_init (NautilusSidebarClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (class);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);


    gobject_class->dispose = nautilus_sidebar_dispose;
    gobject_class->finalize = nautilus_sidebar_finalize;
    gobject_class->set_property = nautilus_sidebar_set_property;
    gobject_class->get_property = nautilus_sidebar_get_property;

    widget_class->measure = nautilus_sidebar_measure;
    widget_class->size_allocate = nautilus_sidebar_size_allocate;

    /*
     * NautilusSidebar::drag-action-requested:
     * @sidebar: the object which received the signal.
     * @drop: (type Gdk.Drop): GdkDrop with information about the drag operation
     * @dest_file: (type Gio.File): GFile with the tentative location that is being hovered for a drop
     * @source_file_list: (type GLib.SList) (element-type GFile) (transfer none):
     *   List of GFile that are being dragged
     *
     * When the user starts a drag-and-drop operation and the sidebar needs
     * to ask the application for which drag action to perform, then the
     * sidebar will emit this signal.
     *
     * The application can evaluate the @context for customary actions, or
     * it can check the type of the files indicated by @source_file_list against the
     * possible actions for the destination @dest_file.
     *
     * The drag action to use must be the return value of the signal handler.
     *
     * Returns: The drag action to use, for example, GDK_ACTION_COPY
     * or GDK_ACTION_MOVE, or 0 if no action is allowed here (i.e. drops
     * are not allowed in the specified @dest_file).
     */
    places_sidebar_signals [DRAG_ACTION_REQUESTED] =
        g_signal_new ("drag-action-requested",
                      G_OBJECT_CLASS_TYPE (gobject_class),
                      G_SIGNAL_RUN_LAST,
                      0,
                      NULL, NULL,
                      NULL,
                      GDK_TYPE_DRAG_ACTION, 2,
                      G_TYPE_OBJECT,
                      GDK_TYPE_FILE_LIST);

    /*
     * NautilusSidebar::drag-action-ask:
     * @sidebar: the object which received the signal.
     * @actions: Possible drag actions that need to be asked for.
     *
     * The places sidebar emits this signal when it needs to ask the application
     * to pop up a menu to ask the user for which drag action to perform.
     *
     * Returns: the final drag action that the sidebar should pass to the drag side
     * of the drag-and-drop operation.
     */
    places_sidebar_signals [DRAG_ACTION_ASK] =
        g_signal_new ("drag-action-ask",
                      G_OBJECT_CLASS_TYPE (gobject_class),
                      G_SIGNAL_RUN_LAST,
                      0,
                      NULL, NULL,
                      NULL,
                      GDK_TYPE_DRAG_ACTION, 1,
                      GDK_TYPE_DRAG_ACTION);

    /*
     * NautilusSidebar::drag-perform-drop:
     * @sidebar: the object which received the signal.
     * @dest_file: (type Gio.File): Destination GFile.
     * @source_file_list: (type GLib.SList) (element-type GFile) (transfer none):
     *   GSList of GFile that got dropped.
     * @action: Drop action to perform.
     *
     * The places sidebar emits this signal when the user completes a
     * drag-and-drop operation and one of the sidebar's items is the
     * destination.  This item is in the @dest_file, and the
     * @source_file_list has the list of files that are dropped into it and
     * which should be copied/moved/etc. based on the specified @action.
     */
    places_sidebar_signals [DRAG_PERFORM_DROP] =
        g_signal_new ("drag-perform-drop",
                      G_OBJECT_CLASS_TYPE (gobject_class),
                      G_SIGNAL_RUN_FIRST,
                      0,
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 3,
                      G_TYPE_OBJECT,
                      GDK_TYPE_FILE_LIST,
                      GDK_TYPE_DRAG_ACTION);

    /*
     * NautilusSidebar::mount:
     * @sidebar: the object which received the signal.
     * @mount_operation: the GMountOperation that is going to start.
     *
     * The places sidebar emits this signal when it starts a new operation
     * because the user clicked on some location that needs mounting.
     * In this way the application using the NautilusSidebar can track the
     * progress of the operation and, for example, show a notification.
     */
    places_sidebar_signals [MOUNT] =
        g_signal_new ("mount",
                      G_OBJECT_CLASS_TYPE (gobject_class),
                      G_SIGNAL_RUN_FIRST,
                      0,
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_MOUNT_OPERATION);
    /*
     * NautilusSidebar::unmount:
     * @sidebar: the object which received the signal.
     * @mount_operation: the GMountOperation that is going to start.
     *
     * The places sidebar emits this signal when it starts a new operation
     * because the user for example ejected some drive or unmounted a mount.
     * In this way the application using the NautilusSidebar can track the
     * progress of the operation and, for example, show a notification.
     */
    places_sidebar_signals [UNMOUNT] =
        g_signal_new ("unmount",
                      G_OBJECT_CLASS_TYPE (gobject_class),
                      G_SIGNAL_RUN_FIRST,
                      0,
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_MOUNT_OPERATION);

    properties[PROP_LOCATION] =
        g_param_spec_object ("location",
                             "Location to Select",
                             "The location to highlight in the sidebar",
                             G_TYPE_FILE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
    properties[PROP_OPEN_FLAGS] =
        g_param_spec_flags ("open-flags",
                            "Open Flags",
                            "Modes in which the calling app can open locations selected in the sidebar",
                            NAUTILUS_TYPE_OPEN_FLAGS,
                            NAUTILUS_OPEN_FLAG_NORMAL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
    properties[PROP_WINDOW_SLOT] =
        g_param_spec_object ("window-slot", NULL, NULL,
                             NAUTILUS_TYPE_WINDOW_SLOT,
                             G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (gobject_class, NUM_PROPERTIES, properties);

    gtk_widget_class_set_css_name (widget_class, "placessidebar");
    gtk_widget_class_set_accessible_role (widget_class, GTK_ACCESSIBLE_ROLE_LIST);
}

/*
 * nautilus_sidebar_new:
 *
 * Creates a new NautilusSidebar widget.
 *
 * The application should connect to at least the
 * NautilusSidebar::open-location signal to be notified
 * when the user makes a selection in the sidebar.
 *
 * Returns: a newly created NautilusSidebar
 */
GtkWidget *
nautilus_sidebar_new (void)
{
    return GTK_WIDGET (g_object_new (nautilus_sidebar_get_type (), NULL));
}

/*
 * nautilus_sidebar_set_open_flags:
 * @sidebar: a places sidebar
 * @flags: Bitmask of modes in which the calling application can open locations
 *
 * Sets the way in which the calling application can open new locations from
 * the places sidebar.  For example, some applications only open locations
 * “directly” into their main view, while others may support opening locations
 * in a new notebook tab or a new window.
 *
 * This function is used to tell the places @sidebar about the ways in which the
 * application can open new locations, so that the sidebar can display (or not)
 * the “Open in new tab” and “Open in new window” menu items as appropriate.
 *
 * When the NautilusSidebar::open-location signal is emitted, its flags
 * argument will be set to one of the @flags that was passed in
 * nautilus_sidebar_set_open_flags().
 *
 * Passing 0 for @flags will cause NAUTILUS_OPEN_FLAG_NORMAL to always be sent
 * to callbacks for the “open-location” signal.
 */
void
nautilus_sidebar_set_open_flags (NautilusSidebar   *sidebar,
                                 NautilusOpenFlags  flags)
{
    g_return_if_fail (NAUTILUS_IS_PLACES_SIDEBAR (sidebar));

    if (sidebar->open_flags != flags)
    {
        sidebar->open_flags = flags;
        g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_OPEN_FLAGS]);
    }
}

/*
 * nautilus_sidebar_get_open_flags:
 * @sidebar: a NautilusSidebar
 *
 * Gets the open flags.
 *
 * Returns: the NautilusOpenFlags of @sidebar
 */
NautilusOpenFlags
nautilus_sidebar_get_open_flags (NautilusSidebar *sidebar)
{
    g_return_val_if_fail (NAUTILUS_IS_PLACES_SIDEBAR (sidebar), 0);

    return sidebar->open_flags;
}

/*
 * nautilus_sidebar_set_location:
 * @sidebar: a places sidebar
 * @location: (nullable): location to select, or %NULL for no current path
 *
 * Sets the location that is being shown in the widgets surrounding the
 * @sidebar, for example, in a folder view in a file manager.  In turn, the
 * @sidebar will highlight that location if it is being shown in the list of
 * places, or it will unhighlight everything if the @location is not among the
 * places in the list.
 */
void
nautilus_sidebar_set_location (NautilusSidebar *sidebar,
                               GFile           *location)
{
    GtkWidget *row;
    char *row_uri;
    char *uri;
    gboolean found = FALSE;

    g_return_if_fail (NAUTILUS_IS_PLACES_SIDEBAR (sidebar));

    gtk_list_box_unselect_all (GTK_LIST_BOX (sidebar->list_box));

    g_set_object (&sidebar->current_location, location);

    if (location == NULL)
    {
        goto out;
    }

    uri = g_file_get_uri (location);

    for (row = gtk_widget_get_first_child (GTK_WIDGET (sidebar->list_box));
         row != NULL && !found;
         row = gtk_widget_get_next_sibling (row))
    {
        if (!GTK_IS_LIST_BOX_ROW (row))
        {
            continue;
        }

        g_object_get (row, "uri", &row_uri, NULL);
        if (row_uri != NULL && g_strcmp0 (row_uri, uri) == 0)
        {
            gtk_list_box_select_row (GTK_LIST_BOX (sidebar->list_box),
                                     GTK_LIST_BOX_ROW (row));
            found = TRUE;
        }

        g_free (row_uri);
    }

    g_free (uri);

out:
    g_object_notify_by_pspec (G_OBJECT (sidebar), properties[PROP_LOCATION]);
}

/*
 * nautilus_sidebar_get_location:
 * @sidebar: a places sidebar
 *
 * Gets the currently selected location in the @sidebar. This can be %NULL when
 * nothing is selected, for example, when nautilus_sidebar_set_location() has
 * been called with a location that is not among the sidebar’s list of places to
 * show.
 *
 * You can use this function to get the selection in the @sidebar.
 *
 * Returns: (nullable) (transfer full): a GFile with the selected location, or
 * %NULL if nothing is visually selected.
 */
GFile *
nautilus_sidebar_get_location (NautilusSidebar *sidebar)
{
    GtkListBoxRow *selected;
    GFile *file;

    g_return_val_if_fail (sidebar != NULL, NULL);

    file = NULL;
    selected = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));

    if (selected)
    {
        char *uri;

        g_object_get (selected, "uri", &uri, NULL);
        file = g_file_new_for_uri (uri);
        g_free (uri);
    }

    return file;
}

char *
nautilus_sidebar_get_location_title (NautilusSidebar *sidebar)
{
    GtkListBoxRow *selected;
    char *title;

    g_return_val_if_fail (sidebar != NULL, NULL);

    title = NULL;
    selected = gtk_list_box_get_selected_row (GTK_LIST_BOX (sidebar->list_box));

    if (selected)
    {
        g_object_get (selected, "label", &title, NULL);
    }

    return title;
}

/*
 * nautilus_sidebar_get_nth_bookmark:
 * @sidebar: a places sidebar
 * @n: index of the bookmark to query
 *
 * This function queries the bookmarks added by the user to the places sidebar,
 * and returns one of them.  This function is used by GtkFileChooser to implement
 * the “Alt-1”, “Alt-2”, etc. shortcuts, which activate the corresponding bookmark.
 *
 * Returns: (nullable) (transfer full): The bookmark specified by the index @n, or
 * %NULL if no such index exist.  Note that the indices start at 0, even though
 * the file chooser starts them with the keyboard shortcut "Alt-1".
 */
GFile *
nautilus_sidebar_get_nth_bookmark (NautilusSidebar *sidebar,
                                   int              n)
{
    GtkWidget *row;
    int k;
    GFile *file;

    g_return_val_if_fail (NAUTILUS_IS_PLACES_SIDEBAR (sidebar), NULL);

    file = NULL;
    k = 0;
    for (row = gtk_widget_get_first_child (GTK_WIDGET (sidebar->list_box));
         row != NULL;
         row = gtk_widget_get_next_sibling (row))
    {
        NautilusSidebarRowType place_type;
        char *uri;

        if (!GTK_IS_LIST_BOX_ROW (row))
        {
            continue;
        }

        g_object_get (row,
                      "place-type", &place_type,
                      "uri", &uri,
                      NULL);
        if (place_type == NAUTILUS_SIDEBAR_ROW_BOOKMARK)
        {
            if (k == n)
            {
                file = g_file_new_for_uri (uri);
                g_free (uri);
                break;
            }
            k++;
        }
        g_free (uri);
    }

    return file;
}

/*
 * nautilus_sidebar_set_drop_targets_visible:
 * @sidebar: a places sidebar.
 * @visible: whether to show the valid targets or not.
 *
 * Make the NautilusSidebar show drop targets, so it can show the available
 * drop targets and a "new bookmark" row. This improves the Drag-and-Drop
 * experience of the user and allows applications to show all available
 * drop targets at once.
 *
 * This needs to be called when the application is aware of an ongoing drag
 * that might target the sidebar. The drop-targets-visible state will be unset
 * automatically if the drag finishes in the NautilusSidebar. You only need
 * to unset the state when the drag ends on some other widget on your application.
 */
void
nautilus_sidebar_set_drop_targets_visible (NautilusSidebar *sidebar,
                                           gboolean         visible)
{
    g_return_if_fail (NAUTILUS_IS_PLACES_SIDEBAR (sidebar));

    if (visible)
    {
        sidebar->drop_state = DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT;
        start_drop_feedback (sidebar, NULL);
    }
    else
    {
        if (sidebar->drop_state == DROP_STATE_NEW_BOOKMARK_ARMED_PERMANENT ||
            sidebar->drop_state == DROP_STATE_NEW_BOOKMARK_ARMED)
        {
            if (!sidebar->dragging_over)
            {
                sidebar->drop_state = DROP_STATE_NORMAL;
                stop_drop_feedback (sidebar);
            }
            else
            {
                /* In case this is called while we are dragging we need to mark the
                 * drop state as no permanent so the leave timeout can do its job.
                 * This will only happen in applications that call this in a wrong
                 * time */
                sidebar->drop_state = DROP_STATE_NEW_BOOKMARK_ARMED;
            }
        }
    }
}
