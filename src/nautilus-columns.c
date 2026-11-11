/*
 * nautilus-columns.c: Miller columns around a slot's list view
 *
 * Laid out like File Flier's: the folder being looked at stays the real
 * list view, with everything it can do, as the middle column. To its left
 * are the folders above it, each with the way down highlighted; to its
 * right, what's inside the selected folder. The columns sit on one strip
 * that starts at the leftmost folder; when it is wider than the view, it
 * slides left (animated, like Finder) so the right end stays in sight.
 * All columns look alike, so going into a folder reads as the selection
 * moving one column on. Clicking an item in a side column shows
 * it (its folder becomes the middle column, with it selected); a double
 * click goes into it or opens it. Left and Right in the list go up and
 * into the selected folder.
 *
 * Column edges can be dragged: a plain drag sets that column's width (kept
 * per depth for the tab), Alt+drag sets the width of every column (the
 * columns-width setting). A double click on an edge resets that column,
 * Alt+double click resets all of them. With columns-width at 0 the
 * columns share the window width equally instead.
 *
 * The side columns are light: a GtkDirectoryList each, with a few file
 * attributes, no thumbnails and no file monitoring beyond GIO's.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-columns.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "nautilus-file.h"
#include "nautilus-files-view.h"
#include "nautilus-enums.h"
#include "nautilus-icon-info.h"
#include "nautilus-image.h"
#include "nautilus-labels.h"
#include "nautilus-metadata.h"
#include "nautilus-mime-actions.h"
#include "nautilus-list-base.h"
#include "nautilus-list-base-private.h"
#include "nautilus-list-view.h"
#include "nautilus-query.h"
#include "nautilus-view-info.h"
#include "nautilus-window-slot.h"

#define COLUMN_WIDTH 220
#define MIN_COLUMN 120
#define MAX_HANDLES 32
#define HANDLE_WIDTH 16
#define INPUT_IS_OURS_US (1500 * G_TIME_SPAN_MILLISECOND)
#define ATTRIBUTES G_FILE_ATTRIBUTE_STANDARD_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
    G_FILE_ATTRIBUTE_STANDARD_ICON "," \
    "metadata::" NAUTILUS_METADATA_KEY_FOLDER_COLOR "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP

struct _NautilusColumns
{
    GtkWidget parent_instance;

    NautilusWindowSlot *slot;   /* weak */
    GtkWidget *content;

    GPtrArray *parents;         /* the side columns on the left, outermost first */
    GtkWidget *child_column;    /* the column on the right */
    GFile *child_shown;         /* the folder it lists, if it lists one */
    GFile *child_item;          /* what it shows */
    gint64 last_input;          /* when a click or key last went to the columns */

    GFile *shown_location;
    guint update_id;

    /* The leftmost column: the folder last opened from outside the columns
     * (sidebar, double-click, path bar, history). Moving within the columns
     * keeps it, unless you go above it. */
    GFile *root;
    gboolean moving_inside;   /* the next location change is ours */

    /* Columns mode, per tab. New tabs start the way the last one was set. */
    gboolean enabled;
    gboolean tree;
    guint mode;
    gboolean flat;
    double wheel_amount;
    gint64 wheel_quiet_until;

    /* Column widths. column_width 0: share the width equally. */
    int column_width;
    GArray *depth_widths;       /* int per depth from the leftmost column; 0 = column_width */
    GtkWidget *handles[MAX_HANDLES];
    GtkWidget *content_title;   /* the middle column's folder name */
    double scroll;              /* how far the strip is slid left */
    AdwAnimation *scroll_animation;
    GFile *laid_root;           /* the root the strip was last laid out from */
    gboolean dragging;
    GFile *previous_location;
    gboolean select_first;      /* went into a folder: select its first item once loaded */
    gboolean revealing;         /* going to a folder to select a clicked item there */
    int drag_depth;
    int drag_start;             /* the column's width when the drag began */
    double drag_left;           /* the column's left edge then */
    gboolean drag_all;

    /* Going up a level keeps everything where it is on screen: the strip
     * doesn't slide back (unless the folder went off the left edge), the
     * folder's side column covers the middle until the list view has
     * read it again, and the folder you were in comes back on the right
     * from a list read before. */
    gboolean going_up;
    GFile *expected_child;      /* going up: the folder you came from, selected once the list has read */
    gboolean scroll_held;       /* the strip stays at scroll, empty on the right if need be */
    int held_width;
    GtkWidget *cover;
    guint cover_timeout_id;
    guint cover_tick_id;
    GPtrArray *recent_lists;    /* GtkDirectoryList, most recent last */
};

#define RECENT_LISTS 8
#define COVER_TIMEOUT_MS 600

enum
{
    PROP_0,
    PROP_MODE,
    PROP_FLAT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void slide_from (NautilusColumns *self, double from);
static void hold_scroll (NautilusColumns *self, double from);
static void slide (NautilusColumns *self, double from, double to);
static double slide_target (NautilusColumns *self, int width);

G_DEFINE_TYPE (NautilusColumns, nautilus_columns, GTK_TYPE_WIDGET)

static gboolean
columns_on (NautilusColumns *self)
{
    NautilusFilesView *view;

    if (self->slot == NULL || !self->enabled)
    {
        return FALSE;
    }

    view = nautilus_window_slot_get_current_view (self->slot);

    return view != NULL && nautilus_files_view_get_view_id (view) == NAUTILUS_VIEW_LIST_ID &&
           nautilus_window_slot_get_location (self->slot) != NULL &&
           g_file_is_native (nautilus_window_slot_get_location (self->slot));
}

static gboolean
show_hidden (void)
{
    g_autoptr (GSettings) gtk = g_settings_new ("org.gtk.gtk4.Settings.FileChooser");

    return g_settings_get_boolean (gtk, "show-hidden");
}

/* ------------------------------------------------------------ A column */

typedef struct
{
    NautilusColumns *columns;
    GFile *directory;
    char *highlight;      /* name of the child on the way down, if any */
    GtkSingleSelection *selection;
    GtkDirectoryList *list;
    GtkWidget *view;
    int icon_size;        /* the list view's, when the column was made */
} Column;

static void
column_free (Column *column)
{
    GPtrArray *recent = column->columns != NULL ? column->columns->recent_lists : NULL;

    g_signal_handlers_disconnect_by_data (column->list, column);
    /* Kept for a while, read and monitored, so that the folder can come
     * back in a column at once. */
    if (recent != NULL && !gtk_directory_list_is_loading (column->list) &&
        gtk_directory_list_get_error (column->list) == NULL)
    {
        g_ptr_array_add (recent, g_steal_pointer (&column->list));
        if (recent->len > RECENT_LISTS)
        {
            g_ptr_array_remove_index (recent, 0);
        }
    }
    g_object_unref (column->directory);
    g_free (column->highlight);
    g_clear_object (&column->list);
    g_free (column);
}

static GtkDirectoryList *
take_recent_list (NautilusColumns *self,
                  GFile           *directory)
{
    for (guint i = self->recent_lists->len; i > 0; i--)
    {
        GtkDirectoryList *list = g_ptr_array_index (self->recent_lists, i - 1);

        if (g_file_equal (gtk_directory_list_get_file (list), directory))
        {
            return g_ptr_array_steal_index (self->recent_lists, i - 1);
        }
    }
    return NULL;
}

/* Sorted like the list view: by name as people read it ("file-2" before
 * "file-10"), so a folder keeps its order going from a side column to the
 * middle one and back. */
static const char *
collation_key (GFileInfo *info)
{
    static GQuark quark;
    const char *key;

    if (quark == 0)
    {
        quark = g_quark_from_static_string ("nautilus-columns-collation-key");
    }
    key = g_object_get_qdata (G_OBJECT (info), quark);
    if (key == NULL)
    {
        char *new_key = g_utf8_collate_key_for_filename (g_file_info_get_display_name (info), -1);

        g_object_set_qdata_full (G_OBJECT (info), quark, new_key, g_free);
        key = new_key;
    }
    return key;
}

static int
compare_infos (gconstpointer a,
               gconstpointer b,
               gpointer      user_data)
{
    GFileInfo *ia = G_FILE_INFO ((gpointer) a);
    GFileInfo *ib = G_FILE_INFO ((gpointer) b);
    gboolean da = g_file_info_get_file_type (ia) == G_FILE_TYPE_DIRECTORY;
    gboolean db = g_file_info_get_file_type (ib) == G_FILE_TYPE_DIRECTORY;

    if (da != db)
    {
        return da ? -1 : 1;
    }

    return strcmp (collation_key (ia), collation_key (ib));
}

static gboolean
filter_info (gpointer item,
             gpointer user_data)
{
    GFileInfo *info = item;

    return GPOINTER_TO_INT (user_data) ||
           (!g_file_info_get_is_hidden (info) && !g_file_info_get_is_backup (info));
}

static void on_row_pressed (GtkGestureClick *gesture, int n_press, double x, double y, GtkListItem *item);

static void
setup_row (GtkSignalListItemFactory *factory,
           GtkListItem              *item,
           gpointer                  user_data)
{
    /* Like the list view's name cell (spacing, image), so a folder looks
     * the same here as once it is the middle column. */
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    Column *column = user_data;
    GtkWidget *icon = GTK_WIDGET (nautilus_image_new ());
    GtkWidget *label = gtk_label_new (NULL);

    GtkWidget *frame = gtk_center_box_new ();

    nautilus_image_set_size (NAUTILUS_IMAGE (icon), column->icon_size);
    gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
    /* A square the icon size, the thumbnail centred in it. */
    gtk_widget_set_size_request (frame, column->icon_size, column->icon_size);
    gtk_widget_set_valign (frame, GTK_ALIGN_CENTER);
    gtk_center_box_set_center_widget (GTK_CENTER_BOX (frame), icon);

    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_box_append (GTK_BOX (box), frame);
    gtk_box_append (GTK_BOX (box), label);
    gtk_list_item_set_child (item, box);

    GtkGesture *click = gtk_gesture_click_new ();

    g_object_set_data (G_OBJECT (click), "column", column);
    g_signal_connect (click, "pressed", G_CALLBACK (on_row_pressed), item);
    gtk_widget_add_controller (box, GTK_EVENT_CONTROLLER (click));
}

static void
bind_row (GtkSignalListItemFactory *factory,
          GtkListItem              *item,
          gpointer                  user_data)
{
    GFileInfo *info = gtk_list_item_get_item (item);
    GtkWidget *box = gtk_list_item_get_child (item);
    GtkWidget *frame = gtk_widget_get_first_child (box);
    GtkWidget *icon = gtk_center_box_get_center_widget (GTK_CENTER_BOX (frame));
    GtkWidget *label = gtk_widget_get_next_sibling (frame);

    Column *column = user_data;
    GIcon *gicon = g_file_info_get_icon (info);
    g_autoptr (GdkPaintable) paintable = NULL;

    /* The same icons as the files: the theme's, in the folder's color. */
    if (gicon != NULL)
    {
        g_autoptr (NautilusIconInfo) icon_info = nautilus_icon_info_lookup (gicon, column->icon_size,
                                                                           gtk_widget_get_scale_factor (icon));
        const char *color = g_file_info_get_attribute_string (info, "metadata::" NAUTILUS_METADATA_KEY_FOLDER_COLOR);

        paintable = nautilus_icon_info_get_paintable (icon_info);
        if (paintable != NULL && g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY &&
            color != NULL && color[0] >= '0' && color[0] <= '7' && color[1] == '\0')
        {
            GdkPaintable *tinted = nautilus_labels_tint_paintable (paintable, color[0] - '0');

            g_object_unref (paintable);
            paintable = tinted;
        }
    }
    nautilus_image_set_fallback (NAUTILUS_IMAGE (icon), paintable);
    /* Thumbnails for files, made or loaded the way the list view does. */
    if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR &&
        column->icon_size >= NAUTILUS_THUMBNAIL_MINIMUM_ICON_SIZE)
    {
        g_autoptr (GFile) location = g_file_get_child (column->directory, g_file_info_get_name (info));

        nautilus_image_set_source (NAUTILUS_IMAGE (icon), location);
    }
    else
    {
        nautilus_image_set_source (NAUTILUS_IMAGE (icon), NULL);
    }
    gtk_label_set_text (GTK_LABEL (label), g_file_info_get_display_name (info));
}

/* Shows the item: its folder becomes the middle column, with it selected. */
static void
reveal_info (Column    *column,
             GFileInfo *info)
{
    NautilusColumns *self = column->columns;
    g_autoptr (GFile) location = g_file_get_child (column->directory, g_file_info_get_name (info));
    g_autolist (NautilusFile) selection = g_list_prepend (NULL, nautilus_file_get (location));

    if (self->slot == NULL)
    {
        return;
    }

    self->moving_inside = TRUE;
    if (self->shown_location != NULL && g_file_equal (column->directory, self->shown_location))
    {
        nautilus_files_view_set_selection (nautilus_window_slot_get_current_view (self->slot), selection, FALSE);
        return;
    }
    /* The clicked item gets selected there, not the first one. */
    self->revealing = TRUE;
    nautilus_window_slot_open_location_full (self->slot, column->directory, selection);
}

/* Goes into a folder, opens a file. */
static void
open_info (Column    *column,
           GFileInfo *info)
{
    NautilusColumns *self = column->columns;
    g_autoptr (GFile) location = g_file_get_child (column->directory, g_file_info_get_name (info));

    if (self->slot == NULL)
    {
        return;
    }

    self->moving_inside = TRUE;
    if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
        nautilus_window_slot_open_location_full (self->slot, location, NULL);
    }
    else
    {
        g_autolist (NautilusFile) files = g_list_prepend (NULL, nautilus_file_get (location));
        g_autofree char *dir = g_file_get_path (column->directory);

        nautilus_mime_activate_files (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))), self->slot,
                                      files, dir, 0, TRUE);
    }
}

static GFileInfo *
row_info (GtkListItem *item)
{
    return gtk_list_item_get_item (item);
}

static void
on_row_pressed (GtkGestureClick *gesture,
                int              n_press,
                double           x,
                double           y,
                GtkListItem     *item)
{
    Column *column = g_object_get_data (G_OBJECT (gesture), "column");
    GFileInfo *info = row_info (item);

    if (info == NULL || column == NULL)
    {
        return;
    }

    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    if (n_press == 2)
    {
        open_info (column, info);
    }
    else if (n_press == 1)
    {
        reveal_info (column, info);
    }
}

/* Enter on a row: as a double click. */
static void
on_activate (GtkListView *view,
             guint        position,
             Column      *column)
{
    g_autoptr (GFileInfo) info = g_list_model_get_item (gtk_single_selection_get_model (column->selection),
                                                        position);

    if (info != NULL)
    {
        open_info (column, info);
    }
}

/* Highlights the way down once the folder has been read. */
static void
highlight (Column *column)
{
    GListModel *model = gtk_single_selection_get_model (column->selection);
    guint n = g_list_model_get_n_items (model);

    if (column->highlight == NULL)
    {
        return;
    }

    for (guint i = 0; i < n; i++)
    {
        g_autoptr (GFileInfo) info = g_list_model_get_item (model, i);

        if (g_strcmp0 (g_file_info_get_name (info), column->highlight) == 0)
        {
            gtk_single_selection_set_selected (column->selection, i);
            gtk_list_view_scroll_to (GTK_LIST_VIEW (column->view), i, GTK_LIST_SCROLL_NONE, NULL);
            return;
        }
    }
}

static void
on_loading (GtkDirectoryList *list,
            GParamSpec       *pspec,
            Column           *column)
{
    if (!gtk_directory_list_is_loading (list))
    {
        highlight (column);
    }
}

/* The size of the list view's icons, so the columns match it. */
static int
list_icon_size (NautilusColumns *self)
{
    NautilusFilesView *view = self->slot != NULL ? nautilus_window_slot_get_current_view (self->slot) : NULL;

    if (view == NULL)
    {
        return NAUTILUS_LIST_ICON_SIZE_SMALL;
    }

    switch (nautilus_list_base_get_zoom_level (nautilus_files_view_get_private_list_base (view)))
    {
        case NAUTILUS_LIST_ZOOM_LEVEL_MEDIUM:
        {
            return nautilus_density_icon_size (NAUTILUS_LIST_ICON_SIZE_MEDIUM);
        }

        case NAUTILUS_LIST_ZOOM_LEVEL_LARGE:
        {
            return nautilus_density_icon_size (NAUTILUS_LIST_ICON_SIZE_LARGE);
        }

        default:
        {
            return nautilus_density_icon_size (NAUTILUS_LIST_ICON_SIZE_SMALL);
        }
    }
}

static char *
column_title (GFile *directory)
{
    g_autofree char *basename = g_file_get_basename (directory);
    g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());

    if (g_file_equal (directory, home))
    {
        return g_strdup (_("Home"));
    }
    return g_strdup (basename != NULL && g_utf8_validate (basename, -1, NULL) ? basename : "?");
}

static GtkWidget *
column_new (NautilusColumns *self,
            GFile           *directory,
            const char      *highlight_name)
{
    Column *column = g_new0 (Column, 1);
    GtkSorter *sorter = GTK_SORTER (gtk_custom_sorter_new (compare_infos, NULL, NULL));
    GtkFilter *filter = GTK_FILTER (gtk_custom_filter_new (filter_info, GINT_TO_POINTER (show_hidden ()), NULL));
    GtkSortListModel *sorted;
    GtkFilterListModel *filtered;
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    GtkWidget *view;
    GtkWidget *scroller = gtk_scrolled_window_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    g_autofree char *title_text = column_title (directory);
    GtkWidget *title = gtk_label_new (title_text);

    column->columns = self;
    column->directory = g_object_ref (directory);
    column->highlight = g_strdup (highlight_name);
    column->list = take_recent_list (self, directory);
    if (column->list == NULL)
    {
        column->list = gtk_directory_list_new (ATTRIBUTES, directory);
        gtk_directory_list_set_monitored (column->list, TRUE);
    }
    g_object_ref (column->list);

    filtered = gtk_filter_list_model_new (G_LIST_MODEL (column->list), filter);
    sorted = gtk_sort_list_model_new (G_LIST_MODEL (filtered), sorter);
    column->selection = gtk_single_selection_new (G_LIST_MODEL (sorted));
    gtk_single_selection_set_autoselect (column->selection, FALSE);
    gtk_single_selection_set_can_unselect (column->selection, TRUE);
    gtk_single_selection_set_selected (column->selection, GTK_INVALID_LIST_POSITION);
    g_signal_connect (column->list, "notify::loading", G_CALLBACK (on_loading), column);

    column->icon_size = list_icon_size (self);
    g_signal_connect (factory, "setup", G_CALLBACK (setup_row), column);
    g_signal_connect (factory, "bind", G_CALLBACK (bind_row), column);
    view = gtk_list_view_new (GTK_SELECTION_MODEL (column->selection), factory);
    column->view = view;
    g_signal_connect (view, "activate", G_CALLBACK (on_activate), column);
    gtk_widget_add_css_class (view, "navigation-sidebar");
    gtk_widget_add_css_class (view, "miller-column");

    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign (GTK_LABEL (title), 0);
    gtk_widget_add_css_class (title, "heading");
    gtk_widget_add_css_class (title, "miller-column-title");

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scroller, TRUE);

    gtk_box_append (GTK_BOX (box), title);
    gtk_box_append (GTK_BOX (box), scroller);
    gtk_widget_add_css_class (box, "miller-column-box");
    g_object_set_data_full (G_OBJECT (box), "column", column, (GDestroyNotify) column_free);

    /* Already read: no loading to wait for. */
    if (!gtk_directory_list_is_loading (column->list))
    {
        highlight (column);
    }

    return box;
}

/* ------------------------------------------------------------ Updating */

static void
clear_box (GtkWidget *box)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child (box)) != NULL)
    {
        gtk_box_remove (GTK_BOX (box), child);
    }
}

static void
clear_parents (NautilusColumns *self,
               guint            from)
{
    while (self->parents->len > from)
    {
        gtk_widget_unparent (g_ptr_array_steal_index (self->parents, self->parents->len - 1));
    }
}

static Column *
column_of (GtkWidget *box)
{
    return g_object_get_data (G_OBJECT (box), "column");
}

static void
remove_cover (NautilusColumns *self)
{
    g_clear_handle_id (&self->cover_timeout_id, g_source_remove);
    if (self->cover_tick_id != 0)
    {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->cover_tick_id);
        self->cover_tick_id = 0;
    }
    g_clear_pointer (&self->cover, gtk_widget_unparent);
}

static gboolean
on_cover_timeout (gpointer user_data)
{
    NautilusColumns *self = user_data;

    self->cover_timeout_id = 0;
    remove_cover (self);
    return G_SOURCE_REMOVE;
}

/* At the next frame, when the list view lays out what it read. */
static gboolean
on_cover_tick (GtkWidget     *widget,
               GdkFrameClock *clock,
               gpointer       user_data)
{
    NautilusColumns *self = NAUTILUS_COLUMNS (widget);

    self->cover_tick_id = 0;
    remove_cover (self);
    return G_SOURCE_REMOVE;
}

static void
on_content_loading (NautilusColumns *self)
{
    if (self->cover != NULL && self->cover_tick_id == 0 &&
        NAUTILUS_IS_FILES_VIEW (self->content) &&
        !nautilus_files_view_get_loading (NAUTILUS_FILES_VIEW (self->content)))
    {
        self->cover_tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_cover_tick, NULL, NULL);
    }
}

/* The side column of the folder now in the middle stays on top of it. */
static void
set_cover (NautilusColumns *self,
           GtkWidget       *box)
{
    remove_cover (self);
    self->cover = box;
    gtk_widget_set_can_target (box, FALSE);
    gtk_widget_insert_before (box, GTK_WIDGET (self), NULL);
    self->cover_timeout_id = g_timeout_add (COVER_TIMEOUT_MS, on_cover_timeout, self);
}

static void
update_parents (NautilusColumns *self)
{
    GFile *location = nautilus_window_slot_get_location (self->slot);
    g_autoptr (GPtrArray) chain = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr (GFile) child = g_object_ref (location);
    GFile *parent;
    guint keep = 0;

    if (self->shown_location != NULL && g_file_equal (self->shown_location, location))
    {
        return;
    }
    g_set_object (&self->shown_location, location);
    {
        g_autofree char *title = column_title (location);

        gtk_label_set_text (GTK_LABEL (self->content_title), title);
    }

    /* Up to the leftmost column's folder. */
    while (self->root != NULL && !g_file_equal (child, self->root) &&
           (parent = g_file_get_parent (child)) != NULL)
    {
        g_ptr_array_insert (chain, 0, g_object_ref (child));
        g_set_object (&child, parent);
        g_object_unref (parent);
    }

    /* chain: from the top's child down to the location; child: the top.
     * Columns that stay the same are kept, with where they were scrolled. */
    GFile *dir = child;

    for (guint i = 0; i < chain->len; i++)
    {
        g_autofree char *name = g_file_get_basename (g_ptr_array_index (chain, i));
        Column *column = i < self->parents->len ? column_of (g_ptr_array_index (self->parents, i)) : NULL;

        if (column != NULL && keep == i && g_file_equal (column->directory, dir) &&
            g_strcmp0 (column->highlight, name) == 0)
        {
            keep++;
        }
        else
        {
            if (keep == i)
            {
                clear_parents (self, i);
            }

            GtkWidget *box = column_new (self, dir, name);

            gtk_widget_add_css_class (box, "miller-parent");
            gtk_widget_insert_before (box, GTK_WIDGET (self),
                                      self->content != NULL ? self->content : self->child_column);
            g_ptr_array_add (self->parents, box);
        }
        dir = g_ptr_array_index (chain, i);
    }
    remove_cover (self);
    if (self->going_up && self->parents->len > chain->len &&
        NAUTILUS_IS_FILES_VIEW (self->content))
    {
        GtkWidget *box = g_ptr_array_index (self->parents, chain->len);

        if (g_file_equal (column_of (box)->directory, location))
        {
            g_ptr_array_remove_index (self->parents, chain->len);
            set_cover (self, box);
        }
    }
    clear_parents (self, chain->len);
    gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
update_child (NautilusColumns *self)
{
    GList *selection = nautilus_window_slot_get_selection (self->slot);
    NautilusFile *file = selection != NULL && selection->next == NULL ? selection->data : NULL;
    g_autoptr (GFile) item = file != NULL ? nautilus_file_get_location (file) : NULL;
    GFile *location = nautilus_window_slot_get_location (self->slot);

    /* Right after a new location, the selection can still be the old
     * folder's: no child column for it. */
    if (item != NULL && location != NULL && !g_file_has_parent (item, location))
    {
        g_clear_object (&item);
        file = NULL;
    }

    gboolean folder = file != NULL && nautilus_file_is_directory (file);

    /* Going up, the list view selects the folder you came from once it has
     * read the folder: show it on the right now, as it was on screen. */
    if (item == NULL && self->expected_child != NULL && location != NULL &&
        g_file_has_parent (self->expected_child, location))
    {
        item = g_object_ref (self->expected_child);
        folder = TRUE;
    }
    else if (item != NULL)
    {
        g_clear_object (&self->expected_child);
    }

    if ((item == NULL && self->child_item == NULL) ||
        (item != NULL && self->child_item != NULL && g_file_equal (item, self->child_item)))
    {
        return;
    }

    g_set_object (&self->child_item, item);
    g_clear_object (&self->child_shown);
    if (folder)
    {
        self->child_shown = g_object_ref (item);
    }
    clear_box (self->child_column);
    if (folder)
    {
        GtkWidget *column = column_new (self, item, NULL);

        gtk_widget_set_hexpand (column, TRUE);
        gtk_box_append (GTK_BOX (self->child_column), column);
    }
}

static NautilusFilesView *files_view (NautilusColumns *self);

static void
select_first_item (NautilusColumns *self)
{
    NautilusFilesView *view = files_view (self);
    NautilusListBase *base;
    GListModel *model;

    if (!self->select_first || view == NULL || !columns_on (self) ||
        nautilus_files_view_get_loading (view))
    {
        return;
    }
    self->select_first = FALSE;
    base = nautilus_files_view_get_private_list_base (view);
    model = base != NULL ? G_LIST_MODEL (nautilus_list_base_get_model (base)) : NULL;
    if (model == NULL || g_list_model_get_n_items (model) == 0 ||
        nautilus_window_slot_get_selection (self->slot) != NULL)
    {
        return;
    }
    nautilus_list_base_set_cursor (base, 0, TRUE, TRUE);
}

static gboolean
update (gpointer user_data)
{
    NautilusColumns *self = user_data;

    self->update_id = 0;

    if (!columns_on (self))
    {
        gtk_widget_remove_css_class (GTK_WIDGET (self), "columns-on");
        gtk_widget_set_visible (self->child_column, FALSE);
        g_clear_object (&self->shown_location);
        g_clear_object (&self->child_shown);
        g_clear_object (&self->child_item);
        clear_parents (self, 0);
        clear_box (self->child_column);
        gtk_widget_queue_allocate (GTK_WIDGET (self));
        return G_SOURCE_REMOVE;
    }

    double from = self->scroll;
    GFile *location = nautilus_window_slot_get_location (self->slot);
    gboolean new_location = self->shown_location == NULL || location == NULL ||
                            !g_file_equal (self->shown_location, location);
    gboolean same_root = self->laid_root != NULL && self->root != NULL &&
                         g_file_equal (self->laid_root, self->root);

    gtk_widget_add_css_class (GTK_WIDGET (self), "columns-on");
    gtk_widget_set_visible (self->child_column, TRUE);
    update_parents (self);
    select_first_item (self);
    update_child (self);
    g_set_object (&self->laid_root, self->root);
    /* Within the same strip, slide; a new one starts where it belongs.
     * Going up, the strip stays, unless the folder is off the left edge. */
    if (same_root && new_location && self->going_up)
    {
        hold_scroll (self, from);
    }
    else if (same_root && self->scroll_held && !new_location)
    {
        /* Still held: only as far as needed to show a column on the right. */
        int width = gtk_widget_get_width (GTK_WIDGET (self));

        if (width > 0)
        {
            slide (self, from, MAX (from, slide_target (self, width)));
        }
    }
    else if (same_root)
    {
        slide_from (self, from);
    }
    else
    {
        self->scroll_held = FALSE;
        adw_animation_reset (self->scroll_animation);
    }
    self->going_up = FALSE;

    return G_SOURCE_REMOVE;
}

static void
on_location_for_root (NautilusColumns *self)
{
    GFile *location = self->slot != NULL ? nautilus_window_slot_get_location (self->slot) : NULL;
    /* Ours: from the side columns, or a double click, Enter or Backspace in
     * the middle one. The sidebar, path bar and so on start a new chain. */
    gboolean inside = self->moving_inside ||
                      g_get_monotonic_time () - self->last_input < INPUT_IS_OURS_US;

    self->moving_inside = FALSE;
    self->last_input = 0;
    if (location == NULL)
    {
        return;
    }

    /* Going into a folder within the columns: like Finder, its first item
     * gets selected, so the highlight is there without pressing Down. */
    self->select_first = inside && !self->revealing && self->previous_location != NULL &&
                         g_file_has_parent (location, self->previous_location);
    self->going_up = inside && self->previous_location != NULL &&
                     g_file_has_prefix (self->previous_location, location);
    g_clear_object (&self->expected_child);
    if (self->going_up)
    {
        g_autoptr (GFile) step = g_object_ref (self->previous_location);

        while (step != NULL && !g_file_has_parent (step, location))
        {
            GFile *up = g_file_get_parent (step);

            g_object_unref (step);
            step = up;
        }
        self->expected_child = g_steal_pointer (&step);
    }
    self->revealing = FALSE;
    g_set_object (&self->previous_location, location);

    if (!inside || self->root == NULL ||
        !(g_file_equal (location, self->root) || g_file_has_prefix (location, self->root)))
    {
        g_set_object (&self->root, location);
    }
}

static void
schedule_update (NautilusColumns *self)
{
    if (self->update_id == 0)
    {
        self->update_id = g_timeout_add (60, update, self);
    }
}

/* A new location lays the columns out at once: waiting would show the new
 * folder's files for a moment under the old columns. */
static void
update_now (NautilusColumns *self)
{
    g_clear_handle_id (&self->update_id, g_source_remove);
    update (self);
}

static NautilusFilesView *
files_view (NautilusColumns *self)
{
    return self->slot != NULL ? nautilus_window_slot_get_current_view (self->slot) : NULL;
}

static guint
compute_mode (NautilusColumns *self)
{
    NautilusFilesView *view = files_view (self);

    if (view != NULL && nautilus_files_view_get_view_id (view) == NAUTILUS_VIEW_GRID_ID)
    {
        return NAUTILUS_COLUMNS_MODE_GRID;
    }

    if (self->enabled)
    {
        return NAUTILUS_COLUMNS_MODE_COLUMNS;
    }

    return self->tree ? NAUTILUS_COLUMNS_MODE_TREE : NAUTILUS_COLUMNS_MODE_LIST;
}

gboolean
nautilus_columns_get_names_only (NautilusColumns *self)
{
    return self->enabled;
}

gboolean
nautilus_columns_get_tree (NautilusColumns *self)
{
    return self->tree && !self->enabled;
}

static void
on_view_changed (NautilusColumns *self)
{
    NautilusFilesView *view = files_view (self);
    guint mode;

    /* Columns and tree go with the list: back from the grid is the plain list. */
    if (view != NULL && nautilus_files_view_get_view_id (view) == NAUTILUS_VIEW_GRID_ID)
    {
        self->enabled = FALSE;
        self->tree = FALSE;
    }

    mode = compute_mode (self);
    if (mode != self->mode)
    {
        self->mode = mode;
        g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_MODE]);
    }

    schedule_update (self);
}

guint
nautilus_columns_get_mode (NautilusColumns *self)
{
    return self->mode;
}

void
nautilus_columns_set_mode (NautilusColumns *self,
                           guint            mode)
{
    NautilusFilesView *view = files_view (self);
    guint view_id = mode == NAUTILUS_COLUMNS_MODE_GRID ? NAUTILUS_VIEW_GRID_ID : NAUTILUS_VIEW_LIST_ID;

    g_return_if_fail (mode >= NAUTILUS_COLUMNS_MODE_LIST && mode <= NAUTILUS_COLUMNS_MODE_GRID);

    if (view == NULL)
    {
        return;
    }

    gboolean was_tree = nautilus_columns_get_tree (self);
    gboolean was_names_only = self->enabled;
    GSettings *flier = nautilus_flier_settings ();

    self->enabled = mode == NAUTILUS_COLUMNS_MODE_COLUMNS;
    self->tree = mode == NAUTILUS_COLUMNS_MODE_TREE;
    /* New tabs start this way. */
    if (g_settings_get_boolean (flier, "columns-view") != self->enabled)
    {
        g_settings_set_boolean (flier, "columns-view", self->enabled);
    }
    if (g_settings_get_boolean (flier, "tree-view") != self->tree)
    {
        g_settings_set_boolean (flier, "tree-view", self->tree);
    }

    g_clear_object (&self->shown_location);
    if (nautilus_files_view_get_view_id (view) != view_id)
    {
        /* Notifies the icon name, which lands in on_view_changed (). The new
         * list view asks for the tree as it sets up. */
        gtk_widget_activate_action (GTK_WIDGET (self->slot), "slot.files-view-mode", "u", view_id);
    }
    else if (was_tree != nautilus_columns_get_tree (self))
    {
        /* The list view takes the tree on when it loads the folder. */
        gtk_widget_activate_action (GTK_WIDGET (self->slot), "slot.reload", NULL);
    }
    on_view_changed (self);

    /* Columns mode shows names only in the list; back out, the others. */
    view = files_view (self);
    if (view != NULL && was_names_only != self->enabled &&
        NAUTILUS_IS_LIST_VIEW (nautilus_files_view_get_private_list_base (view)))
    {
        nautilus_list_view_refresh_visible_columns (NAUTILUS_LIST_VIEW (nautilus_files_view_get_private_list_base (view)));
    }
}

/* ---------------------------------------------------------------- Flat view */

static NautilusQuery *
search_query (NautilusColumns *self)
{
    NautilusFilesView *view = files_view (self);

    if (view == NULL || !nautilus_window_slot_get_search_visible (self->slot))
    {
        return NULL;
    }

    return nautilus_files_view_get_search_query (view);
}

static void
update_flat (NautilusColumns *self)
{
    NautilusQuery *query = search_query (self);
    gboolean flat = query != NULL && nautilus_query_get_flat (query);

    if (flat != self->flat)
    {
        self->flat = flat;
        g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FLAT]);
    }
}

gboolean
nautilus_columns_get_flat (NautilusColumns *self)
{
    return self->flat;
}

void
nautilus_columns_set_flat (NautilusColumns *self,
                           gboolean         flat)
{
    NautilusQuery *current = search_query (self);
    g_autoptr (NautilusQuery) query = NULL;
    g_autoptr (GFile) folder = NULL;

    if (self->slot == NULL || flat == self->flat)
    {
        return;
    }

    if (!flat)
    {
        /* Back to the folder. */
        g_object_set (self->slot, "search-visible", FALSE, NULL);
        return;
    }

    /* A search running already keeps its text, as the filter. */
    if (current != NULL)
    {
        query = nautilus_query_copy (current);
    }
    else
    {
        query = nautilus_query_new ();
        nautilus_query_set_location (query, nautilus_window_slot_get_location (self->slot));
    }

    folder = nautilus_query_get_location (query);
    if (folder == NULL)
    {
        /* Search everywhere has no folder to flatten. */
        return;
    }

    nautilus_query_set_flat (query, TRUE);
    nautilus_window_slot_search (self->slot, query);
    /* The search shows before the view has the query. */
    update_flat (self);
}

/* Every file under Home with @tag, as a flat view. */
void
nautilus_columns_show_tag (NautilusColumns *self,
                           const char      *tag)
{
    g_autoptr (NautilusQuery) query = nautilus_query_new ();
    g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());

    if (self->slot == NULL)
    {
        return;
    }

    nautilus_query_set_location (query, home);
    nautilus_query_set_flat (query, TRUE);
    nautilus_query_set_tag (query, tag);
    nautilus_window_slot_search (self->slot, query);
    update_flat (self);
}

/* Like the wheel: list, columns, grid, then bigger grid items; and back. */
void
nautilus_columns_step (NautilusColumns *self,
                       int              direction)
{
    NautilusFilesView *view = files_view (self);
    guint mode = self->mode;

    if (view == NULL || direction == 0)
    {
        return;
    }

    if (mode == NAUTILUS_COLUMNS_MODE_GRID)
    {
        NautilusListBase *base = nautilus_files_view_get_private_list_base (view);
        NautilusViewInfo info = nautilus_list_base_get_view_info (base);
        int zoom = nautilus_list_base_get_zoom_level (base);

        if (direction > 0 && zoom < info.zoom_level_max)
        {
            gtk_widget_activate_action (GTK_WIDGET (view), "view.zoom-in", NULL);
            return;
        }
        /* Smaller than standard stays with Ctrl+minus. */
        if (direction < 0 && zoom > info.zoom_level_standard)
        {
            gtk_widget_activate_action (GTK_WIDGET (view), "view.zoom-out", NULL);
            return;
        }
    }

    mode = CLAMP ((int) mode + (direction > 0 ? 1 : -1),
                  NAUTILUS_COLUMNS_MODE_LIST, NAUTILUS_COLUMNS_MODE_GRID);
    if (mode != self->mode)
    {
        nautilus_columns_set_mode (self, mode);
    }
}

/* Ctrl+wheel steps through the views; Ctrl+Shift+wheel is left to zoom. */
static gboolean
on_wheel (GtkEventControllerScroll *controller,
          double                    dx,
          double                    dy,
          NautilusColumns          *self)
{
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));
    double threshold;
    gint64 now;

    if ((state & gtk_accelerator_get_default_mod_mask ()) != GDK_CONTROL_MASK || files_view (self) == NULL)
    {
        return GDK_EVENT_PROPAGATE;
    }

    /* A notch is 1; touchpads send pixels. */
    threshold = gtk_event_controller_scroll_get_unit (controller) == GDK_SCROLL_UNIT_WHEEL ? 1.0 : 40.0;
    now = g_get_monotonic_time ();
    if (now < self->wheel_quiet_until)
    {
        return GDK_EVENT_STOP;
    }

    self->wheel_amount += dy;
    if (ABS (self->wheel_amount) >= threshold)
    {
        /* Wheel up: the next view, as in Alt+1 to 4. */
        nautilus_columns_step (self, self->wheel_amount < 0 ? 1 : -1);
        self->wheel_amount = 0;
        /* One step per notch, also for smoothed and fast wheels. */
        self->wheel_quiet_until = now + 220 * G_TIME_SPAN_MILLISECOND;
    }

    return GDK_EVENT_STOP;
}

static void
on_wheel_end (NautilusColumns *self)
{
    self->wheel_amount = 0;
}

/* ---------------------------------------------------------------- Keys */

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                NautilusColumns       *self)
{
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
    GtkWidget *focus = root != NULL ? gtk_root_get_focus (root) : NULL;

    if (focus != NULL && gtk_widget_is_ancestor (focus, GTK_WIDGET (self)) &&
        !GTK_IS_EDITABLE (focus))
    {
        self->last_input = g_get_monotonic_time ();
    }
    /* Keys move the selection under it. */
    remove_cover (self);

    if (!columns_on (self) || (state & gtk_accelerator_get_default_mod_mask ()) != 0 ||
        focus == NULL || GTK_IS_EDITABLE (focus) || GTK_IS_TEXT_VIEW (focus) ||
        self->content == NULL || !gtk_widget_is_ancestor (focus, self->content))
    {
        return GDK_EVENT_PROPAGATE;
    }

    if (keyval == GDK_KEY_Left)
    {
        self->moving_inside = TRUE;
        gtk_widget_activate_action (GTK_WIDGET (self->slot), "slot.up", NULL);
        return GDK_EVENT_STOP;
    }

    if (keyval == GDK_KEY_Right && self->child_shown != NULL)
    {
        self->moving_inside = TRUE;
        nautilus_window_slot_open_location_full (self->slot, self->child_shown, NULL);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

/* ------------------------------------------------------------- GObject */

void
nautilus_columns_set_content (NautilusColumns *self,
                              GtkWidget       *content)
{
    if (self->content != NULL)
    {
        gtk_widget_remove_css_class (self->content, "miller-content");
        g_signal_handlers_disconnect_by_data (self->content, self);
        gtk_widget_unparent (self->content);
    }

    self->content = content;

    if (content != NULL)
    {
        gtk_widget_add_css_class (content, "miller-content");
        if (NAUTILUS_IS_FILES_VIEW (content))
        {
            g_signal_connect_swapped (content, "notify::loading", G_CALLBACK (select_first_item), self);
            g_signal_connect_swapped (content, "notify::loading", G_CALLBACK (on_content_loading), self);
        }
        gtk_widget_insert_before (content, GTK_WIDGET (self), self->child_column);
        gtk_widget_set_hexpand (content, TRUE);
    }

    g_clear_object (&self->shown_location);
    remove_cover (self);
    clear_parents (self, 0);
    schedule_update (self);
}

static void
on_any_press (NautilusColumns *self)
{
    self->last_input = g_get_monotonic_time ();
    remove_cover (self);
}

static void
nautilus_columns_measure (GtkWidget      *widget,
                          GtkOrientation  orientation,
                          int             for_size,
                          int            *minimum,
                          int            *natural,
                          int            *minimum_baseline,
                          int            *natural_baseline)
{
    NautilusColumns *self = NAUTILUS_COLUMNS (widget);

    *minimum = *natural = 0;
    if (self->content != NULL && gtk_widget_get_visible (self->content))
    {
        gtk_widget_measure (self->content, orientation, -1, minimum, natural, NULL, NULL);
    }
    if (orientation == GTK_ORIENTATION_VERTICAL)
    {
        for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
             child = gtk_widget_get_next_sibling (child))
        {
            int min = 0, nat = 0;

            if (child != self->content && gtk_widget_get_visible (child))
            {
                gtk_widget_measure (child, orientation, -1, &min, &nat, NULL, NULL);
                *minimum = MAX (*minimum, min);
                *natural = MAX (*natural, nat);
            }
        }
    }
}

static int
width_at (NautilusColumns *self,
          guint            depth)
{
    int w = depth < self->depth_widths->len ? g_array_index (self->depth_widths, int, depth) : 0;

    return w > 0 ? w : self->column_width;
}

/* The strip of columns for a view this wide: every column's width (equal,
 * or each depth's own), and how far it has to slide left to show its end. */
typedef struct
{
    int equal;      /* the width of every column, or 0 for each depth's own */
    int content;
    int child;
    int total;
} Layout;

#define LAYOUT_WIDTH(self, l, depth) ((l)->equal > 0 ? (l)->equal : width_at ((self), (depth)))

static void
compute_layout (NautilusColumns *self,
                int              width,
                Layout          *l)
{
    guint n_parents = self->parents->len;
    int content_min = 0;

    if (self->content != NULL && gtk_widget_get_visible (self->content))
    {
        gtk_widget_measure (self->content, GTK_ORIENTATION_HORIZONTAL, -1, &content_min, NULL, NULL, NULL);
    }
    l->equal = self->column_width > 0 ? 0 : MAX (COLUMN_WIDTH, width / (int) (n_parents + 2));
    l->content = MAX (LAYOUT_WIDTH (self, l, n_parents), content_min);
    l->child = LAYOUT_WIDTH (self, l, n_parents + 1);
    if (l->content + l->child > width)
    {
        l->child = MAX (MIN_COLUMN, width - l->content);
        l->content = MAX (0, MIN (l->content, width - l->child));
    }
    /* The right column only takes room while it lists a folder, so going
     * into a folder doesn't slide the strip for an empty column. */
    l->total = l->content + (self->child_shown != NULL ? l->child : 0);
    for (guint i = 0; i < n_parents; i++)
    {
        l->total += LAYOUT_WIDTH (self, l, i);
    }
    /* Equal widths fill the view. */
    if (l->equal > 0 && l->total < width)
    {
        l->child += width - l->total;
        l->total = width;
    }
}

static double
slide_target (NautilusColumns *self,
              int              width)
{
    Layout l;

    /* Only as far as needed, so the columns on the left stay in sight as
     * long as possible. */
    compute_layout (self, width, &l);
    return MAX (0, l.total - width);
}

static void
on_slide (double           value,
          NautilusColumns *self)
{
    self->scroll = value;
    gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
slide (NautilusColumns *self,
       double           from,
       double           to)
{
    if (G_APPROX_VALUE (from, to, 0.5))
    {
        self->scroll = to;
        return;
    }
    adw_animation_reset (self->scroll_animation);
    adw_timed_animation_set_value_from (ADW_TIMED_ANIMATION (self->scroll_animation), from);
    adw_timed_animation_set_value_to (ADW_TIMED_ANIMATION (self->scroll_animation), to);
    self->scroll = from;
    adw_animation_play (self->scroll_animation);
}

/* Where the middle column starts on the strip. */
static double
content_left (NautilusColumns *self,
              int              width)
{
    Layout l;
    double x = 0;

    compute_layout (self, width, &l);
    for (guint i = 0; i < self->parents->len; i++)
    {
        x += LAYOUT_WIDTH (self, &l, i);
    }
    return x;
}

/* Keeps the strip where it is, sliding only to bring the middle column
 * back from past the left edge. */
static void
hold_scroll (NautilusColumns *self,
             double           from)
{
    int width = gtk_widget_get_width (GTK_WIDGET (self));

    if (width <= 0 || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
        return;
    }
    self->scroll_held = TRUE;
    self->held_width = width;
    slide (self, from, MAX (0, MIN (from, content_left (self, width))));
}

/* Slides the strip from where it was to where it now has to be. */
static void
slide_from (NautilusColumns *self,
            double           from)
{
    int width = gtk_widget_get_width (GTK_WIDGET (self));
    double to;

    self->scroll_held = FALSE;
    if (width <= 0 || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
        return;
    }
    to = slide_target (self, width);
    if (G_APPROX_VALUE (from, to, 0.5))
    {
        return;
    }
    adw_animation_reset (self->scroll_animation);
    adw_timed_animation_set_value_from (ADW_TIMED_ANIMATION (self->scroll_animation), from);
    adw_timed_animation_set_value_to (ADW_TIMED_ANIMATION (self->scroll_animation), to);
    self->scroll = from;
    adw_animation_play (self->scroll_animation);
}

static void
place_handle (NautilusColumns *self,
              guint           *n_handles,
              int              x,
              int              width_of_view,
              int              height,
              guint            depth,
              int              width)
{
    GtkWidget *handle;

    if (*n_handles >= MAX_HANDLES || x <= 0 || x + HANDLE_WIDTH / 2 > width_of_view)
    {
        return;
    }
    handle = self->handles[(*n_handles)++];
    g_object_set_data (G_OBJECT (handle), "depth", GUINT_TO_POINTER (depth));
    g_object_set_data (G_OBJECT (handle), "width", GINT_TO_POINTER (width));
    gtk_widget_set_child_visible (handle, TRUE);
    gtk_widget_size_allocate (handle,
                              &(GtkAllocation) { x - HANDLE_WIDTH / 2, 0, HANDLE_WIDTH, height }, -1);
}

static void
place (GtkWidget *widget,
       int        x,
       int        y,
       int        w,
       int        h,
       int        width_of_view)
{
    gboolean shown = w > 0 && x + w > 0 && x < width_of_view;

    gtk_widget_set_child_visible (widget, shown);
    if (shown)
    {
        gtk_widget_size_allocate (widget, &(GtkAllocation) { x, y, w, h }, -1);
    }
}

static void
nautilus_columns_size_allocate (GtkWidget *widget,
                                int        width,
                                int        height,
                                int        baseline)
{
    NautilusColumns *self = NAUTILUS_COLUMNS (widget);
    gboolean has_content = self->content != NULL && gtk_widget_get_visible (self->content);
    gboolean side = gtk_widget_get_visible (self->child_column) && width >= 2 * MIN_COLUMN;
    guint n_parents = self->parents->len;
    guint n_handles = 0;
    Layout l;
    int x, title_h = 0;

    if (!side)
    {
        for (guint i = 0; i < n_parents; i++)
        {
            gtk_widget_set_child_visible (g_ptr_array_index (self->parents, i), FALSE);
        }
        gtk_widget_set_child_visible (self->content_title, FALSE);
        gtk_widget_set_child_visible (self->child_column, FALSE);
        if (self->cover != NULL)
        {
            gtk_widget_set_child_visible (self->cover, FALSE);
        }
        if (has_content)
        {
            gtk_widget_size_allocate (self->content, &(GtkAllocation) { 0, 0, width, height }, -1);
        }
        for (guint i = 0; i < MAX_HANDLES; i++)
        {
            gtk_widget_set_child_visible (self->handles[i], FALSE);
        }
        return;
    }

    compute_layout (self, width, &l);
    if (self->scroll_held && width != self->held_width)
    {
        self->scroll_held = FALSE;
    }
    if (!self->dragging && !self->scroll_held &&
        adw_animation_get_state (self->scroll_animation) != ADW_ANIMATION_PLAYING)
    {
        self->scroll = slide_target (self, width);
    }
    x = - (int) (self->scroll + 0.5);

    for (guint i = 0; i < n_parents; i++)
    {
        int w = LAYOUT_WIDTH (self, &l, i);

        place (g_ptr_array_index (self->parents, i), x, 0, w, height, width);
        x += w;
        place_handle (self, &n_handles, x, width, height, i, w);
    }

    gtk_widget_measure (self->content_title, GTK_ORIENTATION_VERTICAL, l.content, NULL, &title_h, NULL, NULL);
    place (self->content_title, x, 0, l.content, title_h, width);
    if (has_content)
    {
        place (self->content, x, title_h, l.content, height - title_h, width);
    }
    if (self->cover != NULL)
    {
        place (self->cover, x, 0, l.content, height, width);
    }
    x += l.content;
    place_handle (self, &n_handles, x, width, height, n_parents, l.content);

    place (self->child_column, x, 0, l.child, height, width);
    if (l.equal == 0)
    {
        place_handle (self, &n_handles, x + l.child, width, height, n_parents + 1, l.child);
    }

    for (guint i = n_handles; i < MAX_HANDLES; i++)
    {
        gtk_widget_set_child_visible (self->handles[i], FALSE);
    }
}

/* ------------------------------------------------------------ Resizing */

/* The gestures sit on the columns, not on the edge handles: a handle moves
 * with its edge while it is dragged, so offsets measured on it would feed
 * back into themselves and jitter. */
static GtkWidget *
handle_at (NautilusColumns *self,
           double           x,
           double           y)
{
    GtkWidget *picked = gtk_widget_pick (GTK_WIDGET (self), x, y, GTK_PICK_DEFAULT);

    for (guint i = 0; i < MAX_HANDLES; i++)
    {
        if (picked == self->handles[i])
        {
            return picked;
        }
    }
    return NULL;
}

static void
on_drag_begin (GtkGestureDrag  *gesture,
               double           x,
               double           y,
               NautilusColumns *self)
{
    GtkWidget *handle = handle_at (self, x, y);
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
    graphene_point_t edge;

    if (handle == NULL ||
        !gtk_widget_compute_point (handle, GTK_WIDGET (self), &GRAPHENE_POINT_INIT (HANDLE_WIDTH / 2, 0), &edge))
    {
        gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    self->drag_depth = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (handle), "depth"));
    self->drag_start = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (handle), "width"));
    self->drag_all = (state & GDK_ALT_MASK) != 0;
    self->drag_left = edge.x - self->drag_start;
    if (self->column_width <= 0)
    {
        /* Leave equal sharing: every column keeps the width it has now. */
        self->column_width = self->drag_start;
    }
    /* The strip holds still while dragging, so the edge stays under the
     * pointer; it slides into place when the drag ends. */
    self->dragging = TRUE;
}

static void
on_drag_update (GtkGestureDrag  *gesture,
                double           offset_x,
                double           offset_y,
                NautilusColumns *self)
{
    int view = gtk_widget_get_width (GTK_WIDGET (self));
    /* Where the edge goes: it keeps its distance to the pointer, so it
     * doesn't jump when the press was a little off the line. */
    double edge = CLAMP (self->drag_left + self->drag_start + offset_x, 0, view - HANDLE_WIDTH / 2);
    int w;

    if (!self->dragging || offset_x == 0)
    {
        return;
    }
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    /* Alt can also come in once the drag is going. */
    if (gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture)) & GDK_ALT_MASK)
    {
        self->drag_all = TRUE;
    }

    if (self->drag_all)
    {
        /* Every column from the leftmost one up to this edge has the new
         * width, so the edge is (depth + 1) widths from the strip's start. */
        w = (int) ((edge + self->scroll) / (self->drag_depth + 1) + 0.5);
        self->column_width = MAX (w, MIN_COLUMN);
        g_array_set_size (self->depth_widths, 0);
    }
    else
    {
        w = MAX ((int) (edge - self->drag_left + 0.5), MIN_COLUMN);
        if (self->depth_widths->len <= (guint) self->drag_depth)
        {
            g_array_set_size (self->depth_widths, self->drag_depth + 1);
        }
        g_array_index (self->depth_widths, int, self->drag_depth) = w;
    }
    gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag  *gesture,
             double           offset_x,
             double           offset_y,
             NautilusColumns *self)
{
    if (!self->dragging)
    {
        return;
    }
    self->dragging = FALSE;
    if (self->drag_all && offset_x != 0)
    {
        g_settings_set_int (nautilus_flier_settings (), "columns-width", self->column_width);
    }
    slide_from (self, self->scroll);
}

static void
on_edge_pressed (GtkGestureClick *gesture,
                 int              n_press,
                 double           x,
                 double           y,
                 NautilusColumns *self)
{
    GtkWidget *handle = handle_at (self, x, y);
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
    guint depth;

    if (handle == NULL || n_press != 2)
    {
        return;
    }
    depth = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (handle), "depth"));
    if (state & GDK_ALT_MASK)
    {
        g_settings_reset (nautilus_flier_settings (), "columns-width");
    }
    else if (depth < self->depth_widths->len)
    {
        g_array_index (self->depth_widths, int, depth) = 0;
    }
    gtk_widget_queue_allocate (GTK_WIDGET (self));
    slide_from (self, self->scroll);
}

static void
on_columns_width_changed (NautilusColumns *self)
{
    self->column_width = g_settings_get_int (nautilus_flier_settings (), "columns-width");
    g_array_set_size (self->depth_widths, 0);
    gtk_widget_queue_allocate (GTK_WIDGET (self));
}

/* "extra-dense" draws the icons smaller: build the columns again. */
static void
on_density_changed (NautilusColumns *self)
{
    clear_parents (self, 0);
    g_clear_object (&self->shown_location);
    g_clear_object (&self->child_item);
    g_clear_object (&self->child_shown);
    clear_box (self->child_column);
    schedule_update (self);
}

static void
nautilus_columns_init (NautilusColumns *self)
{
    GtkEventController *controller;

    gtk_widget_add_css_class (GTK_WIDGET (self), "miller-columns");
    /* The strip slides left past the edge; don't draw it there. */
    gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);

    self->parents = g_ptr_array_new ();
    self->recent_lists = g_ptr_array_new_with_free_func (g_object_unref);

    controller = GTK_EVENT_CONTROLLER (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (controller), 0);
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    g_signal_connect_swapped (controller, "pressed", G_CALLBACK (on_any_press), self);
    gtk_widget_add_controller (GTK_WIDGET (self), controller);

    self->child_column = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (self->child_column, "miller-child");
    gtk_widget_set_visible (self->child_column, FALSE);
    gtk_widget_set_parent (self->child_column, GTK_WIDGET (self));

    self->content_title = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (self->content_title), 0);
    gtk_label_set_ellipsize (GTK_LABEL (self->content_title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class (self->content_title, "heading");
    gtk_widget_add_css_class (self->content_title, "miller-column-title");
    gtk_widget_add_css_class (self->content_title, "miller-content-title");
    gtk_widget_set_child_visible (self->content_title, FALSE);
    gtk_widget_set_parent (self->content_title, GTK_WIDGET (self));

    self->scroll_animation = adw_timed_animation_new (GTK_WIDGET (self), 0, 0, 120,
                                                      adw_callback_animation_target_new ((AdwAnimationTargetFunc) on_slide,
                                                                                         self, NULL));
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->scroll_animation), ADW_EASE_OUT_CUBIC);

    /* Edge handles, after every column so they are on top. */
    for (guint i = 0; i < MAX_HANDLES; i++)
    {
        GtkWidget *handle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

        gtk_widget_add_css_class (handle, "miller-handle");
        gtk_widget_set_cursor_from_name (handle, "col-resize");
        gtk_widget_set_child_visible (handle, FALSE);
        gtk_widget_set_parent (handle, GTK_WIDGET (self));
        self->handles[i] = handle;
    }
    controller = GTK_EVENT_CONTROLLER (gtk_gesture_drag_new ());
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    g_signal_connect (controller, "drag-begin", G_CALLBACK (on_drag_begin), self);
    g_signal_connect (controller, "drag-update", G_CALLBACK (on_drag_update), self);
    g_signal_connect (controller, "drag-end", G_CALLBACK (on_drag_end), self);
    gtk_widget_add_controller (GTK_WIDGET (self), controller);
    controller = GTK_EVENT_CONTROLLER (gtk_gesture_click_new ());
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    g_signal_connect (controller, "pressed", G_CALLBACK (on_edge_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), controller);
    self->depth_widths = g_array_new (FALSE, TRUE, sizeof (int));
    self->column_width = g_settings_get_int (nautilus_flier_settings (), "columns-width");
    g_signal_connect_object (nautilus_flier_settings (), "changed::columns-width",
                             G_CALLBACK (on_columns_width_changed), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::density",
                             G_CALLBACK (on_density_changed), self, G_CONNECT_SWAPPED);

    controller = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    g_signal_connect (controller, "key-pressed", G_CALLBACK (on_key_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), controller);

    controller = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    g_signal_connect (controller, "scroll", G_CALLBACK (on_wheel), self);
    g_signal_connect_swapped (controller, "scroll-end", G_CALLBACK (on_wheel_end), self);
    gtk_widget_add_controller (GTK_WIDGET (self), controller);

    self->enabled = g_settings_get_boolean (nautilus_flier_settings (), "columns-view");
    self->tree = !self->enabled && g_settings_get_boolean (nautilus_flier_settings (), "tree-view");
    self->mode = self->enabled ? NAUTILUS_COLUMNS_MODE_COLUMNS :
                 self->tree ? NAUTILUS_COLUMNS_MODE_TREE : NAUTILUS_COLUMNS_MODE_LIST;
}

static void
nautilus_columns_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
    switch (prop_id)
    {
        case PROP_MODE:
        {
            g_value_set_uint (value, NAUTILUS_COLUMNS (object)->mode);
        }
        break;

        case PROP_FLAT:
        {
            g_value_set_boolean (value, NAUTILUS_COLUMNS (object)->flat);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_columns_dispose (GObject *object)
{
    NautilusColumns *self = NAUTILUS_COLUMNS (object);
    GtkWidget *child;

    g_clear_handle_id (&self->update_id, g_source_remove);
    remove_cover (self);
    g_clear_weak_pointer (&self->slot);
    g_clear_object (&self->shown_location);
    g_clear_object (&self->child_shown);
    g_clear_object (&self->child_item);
    g_clear_object (&self->root);
    g_clear_object (&self->laid_root);
    g_clear_object (&self->previous_location);
    g_clear_object (&self->expected_child);
    if (self->content != NULL)
    {
        g_signal_handlers_disconnect_by_data (self->content, self);
    }
    g_clear_object (&self->scroll_animation);
    g_clear_pointer (&self->parents, g_ptr_array_unref);
    g_clear_pointer (&self->depth_widths, g_array_unref);
    self->content = NULL;
    while ((child = gtk_widget_get_first_child (GTK_WIDGET (object))) != NULL)
    {
        gtk_widget_unparent (child);
    }
    g_clear_pointer (&self->recent_lists, g_ptr_array_unref);

    G_OBJECT_CLASS (nautilus_columns_parent_class)->dispose (object);
}

static void
nautilus_columns_class_init (NautilusColumnsClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_columns_dispose;
    G_OBJECT_CLASS (klass)->get_property = nautilus_columns_get_property;

    properties[PROP_MODE] = g_param_spec_uint ("mode", NULL, NULL,
                                               NAUTILUS_COLUMNS_MODE_LIST, NAUTILUS_COLUMNS_MODE_GRID,
                                               NAUTILUS_COLUMNS_MODE_LIST,
                                               G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    properties[PROP_FLAT] = g_param_spec_boolean ("flat", NULL, NULL, FALSE,
                                                  G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    g_object_class_install_properties (G_OBJECT_CLASS (klass), N_PROPS, properties);
    GTK_WIDGET_CLASS (klass)->measure = nautilus_columns_measure;
    GTK_WIDGET_CLASS (klass)->size_allocate = nautilus_columns_size_allocate;
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "nautilus-columns");
}

GtkWidget *
nautilus_columns_new (NautilusWindowSlot *slot)
{
    NautilusColumns *self = g_object_new (NAUTILUS_TYPE_COLUMNS, NULL);

    g_set_weak_pointer (&self->slot, slot);
    g_signal_connect_object (slot, "notify::location", G_CALLBACK (on_location_for_root), self,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (slot, "notify::location", G_CALLBACK (update_now), self,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (slot, "notify::location", G_CALLBACK (update_flat), self,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (slot, "notify::search-visible", G_CALLBACK (update_flat), self,
                             G_CONNECT_SWAPPED);
    g_signal_connect_object (slot, "notify::selection", G_CALLBACK (schedule_update), self,
                             G_CONNECT_SWAPPED);
    /* Switching between list and grid. */
    g_signal_connect_object (slot, "notify::icon-name", G_CALLBACK (on_view_changed), self,
                             G_CONNECT_SWAPPED);

    return GTK_WIDGET (self);
}
