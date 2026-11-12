/*
 * nautilus-inspector.c: a panel with a preview and the details of the
 * selection
 *
 * One item: its picture, video, song, text or thumbnail, its name, stars,
 * tags and details. Several: all of them on Quick Look's canvas, with what
 * they are and how big. Nothing selected: the folder itself.
 *
 * It does nothing while hidden, and waits for a selection to settle (a
 * rubber band changes it many times a second) before updating.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-inspector.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "nautilus-file.h"
#include "nautilus-image.h"
#include "nautilus-labels.h"
#include "nautilus-labels-ui.h"
#include "nautilus-ql-canvas.h"
#include "nautilus-text-preview.h"
#include "nautilus-window-slot.h"

#define SETTLE_MS 90
#define TEXT_LIMIT (64 * 1024)

struct _NautilusInspector
{
    GtkWidget parent_instance;

    GtkWidget *content;        /* the scrolled column: name, preview, details */

    NautilusWindowSlot *slot;  /* weak */
    gulong selection_id;
    gulong location_id;
    guint update_id;

    GList *files;              /* shown */
    gboolean is_folder;        /* showing the slot's folder */

    GtkWidget *header;         /* grip to drag the panel, pop-out button */
    GtkWidget *grip;
    GtkWidget *pop_button;
    GtkWidget *scroller;
    GtkStack *preview;
    NautilusQlCanvas *canvas;
    GtkWidget *image;
    NautilusTextPreview *text;
    GtkVideo *video;

    GtkLabel *name;
    GtkLabel *summary;
    GtkWidget *rating_box;
    GtkWidget *rating_row;
    GtkWidget *tags_row;
    GtkWidget *stars[5];
    GtkWidget *tags_box;
    AdwPreferencesGroup *details;
    GList *detail_rows;
};

G_DEFINE_TYPE (NautilusInspector, nautilus_inspector, GTK_TYPE_WIDGET)

static void schedule_update (NautilusInspector *self);

/* ------------------------------------------------------------ Details */

static void
clear_details (NautilusInspector *self)
{
    for (GList *l = self->detail_rows; l != NULL; l = l->next)
    {
        adw_preferences_group_remove (self->details, l->data);
    }
    g_clear_pointer (&self->detail_rows, g_list_free);
}

static void
add_detail (NautilusInspector *self,
            const char        *title,
            const char        *value)
{
    GtkWidget *row;
    GtkWidget *box;
    GtkWidget *key;
    GtkWidget *val;

    if (value == NULL || *value == '\0' || g_strcmp0 (value, "—") == 0)
    {
        return;
    }

    /* One line each: the name on the left, the value on the right (on
     * more lines only when it is long, like a path). */
    row = gtk_list_box_row_new ();
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_widget_add_css_class (row, "inspector-detail");
    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    key = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (key), 0);
    gtk_widget_set_valign (key, GTK_ALIGN_START);
    gtk_widget_add_css_class (key, "inspector-detail-key");
    val = gtk_label_new (value);
    gtk_label_set_xalign (GTK_LABEL (val), 1);
    gtk_label_set_justify (GTK_LABEL (val), GTK_JUSTIFY_RIGHT);
    gtk_label_set_wrap (GTK_LABEL (val), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (val), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable (GTK_LABEL (val), TRUE);
    gtk_widget_set_focusable (val, FALSE);
    gtk_widget_set_hexpand (val, TRUE);
    gtk_widget_add_css_class (val, "inspector-detail-value");
    gtk_box_append (GTK_BOX (box), key);
    gtk_box_append (GTK_BOX (box), val);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    adw_preferences_group_add (self->details, row);
    self->detail_rows = g_list_append (self->detail_rows, row);
}

/* A detail line whose value is a widget (the stars, the tags). */
static GtkWidget *
widget_line (const char *title,
             GtkWidget  *value)
{
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *key = gtk_label_new (title);

    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_widget_add_css_class (row, "inspector-detail");
    gtk_widget_add_css_class (row, "inspector-widget-line");
    gtk_label_set_xalign (GTK_LABEL (key), 0);
    gtk_widget_add_css_class (key, "inspector-detail-key");
    gtk_widget_set_hexpand (value, TRUE);
    gtk_widget_set_halign (value, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (box), key);
    gtk_box_append (GTK_BOX (box), value);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);

    return row;
}

static void
add_attribute (NautilusInspector *self,
               NautilusFile      *file,
               const char        *title,
               const char        *attribute)
{
    g_autofree char *value = nautilus_file_get_string_attribute (file, attribute);

    add_detail (self, title, value);
}

static void
show_file_details (NautilusInspector *self,
                   NautilusFile      *file)
{
    add_attribute (self, file, _("Kind"), "detailed_type");
    add_attribute (self, file, nautilus_file_is_directory (file) ? _("Contents") : _("Size"), "size");

    int width = 0, height = 0;

    nautilus_ql_canvas_get_picture_size (self->canvas, &width, &height);
    if (width > 0 && height > 0)
    {
        g_autofree char *dimensions = g_strdup_printf ("%d × %d", width, height);

        add_detail (self, _("Dimensions"), dimensions);
    }

    add_attribute (self, file, _("Modified"), "date_modified_full");
    add_attribute (self, file, _("Created"), "date_created_full");
    add_attribute (self, file, _("Accessed"), "date_accessed_full");
    add_attribute (self, file, _("Location"), "where");
    if (nautilus_file_is_symbolic_link (file))
    {
        add_attribute (self, file, _("Link To"), "link_target");
    }
}

static void
show_selection_details (NautilusInspector *self)
{
    guint folders = 0, others = 0;
    goffset total = 0;

    for (GList *l = self->files; l != NULL; l = l->next)
    {
        if (nautilus_file_is_directory (l->data))
        {
            folders++;
        }
        else
        {
            others++;
            total += nautilus_file_get_size (l->data);
        }
    }

    if (folders > 0)
    {
        g_autofree char *value = g_strdup_printf ("%u", folders);

        add_detail (self, _("Folders"), value);
    }
    if (others > 0)
    {
        g_autofree char *value = g_strdup_printf ("%u", others);
        g_autofree char *size = g_format_size (total);

        add_detail (self, _("Files"), value);
        add_detail (self, _("Total Size"), size);
    }
}

/* ------------------------------------------------------ Rating, tags */

static void
update_marks (NautilusInspector *self)
{
    GSettings *flier = nautilus_flier_settings ();
    gboolean writable = self->files != NULL && !self->is_folder;
    guint rating = 0;
    GtkWidget *child;

    for (GList *l = self->files; l != NULL && writable; l = l->next)
    {
        writable = nautilus_file_can_write (l->data);
    }

    /* The stars show a rating all the items share. */
    if (self->files != NULL)
    {
        rating = nautilus_file_get_rating (self->files->data);
        for (GList *l = self->files->next; l != NULL; l = l->next)
        {
            if (nautilus_file_get_rating (l->data) != rating)
            {
                rating = 0;
                break;
            }
        }
    }

    gtk_widget_set_visible (self->rating_row, writable && g_settings_get_boolean (flier, "ratings"));
    for (guint i = 0; i < 5; i++)
    {
        gtk_button_set_icon_name (GTK_BUTTON (self->stars[i]),
                                  i < rating ? "starred-symbolic" : "non-starred-symbolic");
        if (i < rating)
        {
            gtk_widget_add_css_class (self->stars[i], "rated");
        }
        else
        {
            gtk_widget_remove_css_class (self->stars[i], "rated");
        }
    }

    while ((child = gtk_widget_get_first_child (self->tags_box)) != NULL)
    {
        gtk_box_remove (GTK_BOX (self->tags_box), child);
    }

    gboolean tags_on = writable && g_settings_get_boolean (flier, "tags");
    const char * const *tags = self->files != NULL && self->files->next == NULL
                               ? nautilus_file_get_tags (self->files->data) : NULL;

    for (guint i = 0; tags_on && tags != NULL && tags[i] != NULL; i++)
    {
        GtkWidget *tag = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *label = gtk_label_new (tags[i]);

        gtk_widget_add_css_class (tag, "inspector-tag");
        gtk_box_append (GTK_BOX (tag), nautilus_tag_dot_new (tags[i]));
        gtk_box_append (GTK_BOX (tag), label);
        gtk_box_append (GTK_BOX (self->tags_box), tag);
    }

    if (tags_on)
    {
        GtkWidget *edit = gtk_button_new_with_label (tags != NULL && tags[0] != NULL ? _("Edit…") : _("Add…"));

        gtk_widget_add_css_class (edit, "flat");
        gtk_widget_add_css_class (edit, "inspector-tags-edit");
        gtk_actionable_set_action_name (GTK_ACTIONABLE (edit), "view.edit-tags");
        gtk_box_append (GTK_BOX (self->tags_box), edit);
    }
    gtk_widget_set_visible (self->tags_row, tags_on);
}

static void
on_star_clicked (GtkButton         *button,
                 NautilusInspector *self)
{
    guint stars = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "stars"));
    gboolean all_have_it = TRUE;

    for (GList *l = self->files; l != NULL; l = l->next)
    {
        all_have_it &= nautilus_file_get_rating (l->data) == stars;
    }

    /* Clicking the rating they have clears it. */
    nautilus_labels_set_rating (self->files, all_have_it ? 0 : stars);
}

/* ------------------------------------------------------------ Showing */

static void
stop_video (NautilusInspector *self)
{
    GtkMediaStream *stream = gtk_video_get_media_stream (self->video);

    if (stream != NULL)
    {
        gtk_media_stream_pause (stream);
    }
    gtk_video_set_file (self->video, NULL);
}

static void
show_preview (NautilusInspector *self,
              NautilusFile      *file)
{
    const char *mime = nautilus_file_get_mime_type (file);
    gboolean regular = nautilus_file_is_regular_file (file);
    g_autoptr (GFile) location = nautilus_file_get_location (file);

    nautilus_text_preview_set_file (self->text, NULL);

    if (regular && g_str_has_prefix (mime, "image/"))
    {
        stop_video (self);
        nautilus_ql_canvas_set_files (self->canvas, &(GList){ .data = file });
        gtk_stack_set_visible_child (self->preview, GTK_WIDGET (self->canvas));
    }
    else if (regular && (g_str_has_prefix (mime, "video/") || g_str_has_prefix (mime, "audio/")))
    {
        nautilus_ql_canvas_set_files (self->canvas, NULL);
        gtk_video_set_file (self->video, location);
        gtk_stack_set_visible_child (self->preview, GTK_WIDGET (self->video));
    }
    else if (regular && nautilus_text_preview_handles (mime))
    {
        stop_video (self);
        nautilus_ql_canvas_set_files (self->canvas, NULL);
        nautilus_text_preview_set_file (self->text, file);
        gtk_stack_set_visible_child (self->preview, GTK_WIDGET (self->text));
    }
    else
    {
        g_autoptr (GdkPaintable) icon = nautilus_file_get_icon_paintable (file, 192,
                                                                          gtk_widget_get_scale_factor (GTK_WIDGET (self)),
                                                                          NAUTILUS_FILE_ICON_FLAGS_NONE);

        stop_video (self);
        nautilus_ql_canvas_set_files (self->canvas, NULL);
        nautilus_image_set_fallback (NAUTILUS_IMAGE (self->image), icon);
        nautilus_image_set_source (NAUTILUS_IMAGE (self->image), regular ? location : NULL);
        gtk_stack_set_visible_child (self->preview, self->image);
    }
}

static void
refresh_text (NautilusInspector *self)
{
    clear_details (self);

    if (self->files == NULL)
    {
        gtk_label_set_text (self->name, "");
        gtk_label_set_text (self->summary, "");
        update_marks (self);
        return;
    }

    if (self->files->next == NULL)
    {
        NautilusFile *file = self->files->data;

        gtk_label_set_text (self->name, nautilus_file_get_display_name (file));
        gtk_label_set_text (self->summary, self->is_folder ? _("This Folder") : "");
        gtk_widget_set_visible (GTK_WIDGET (self->summary), self->is_folder);
        show_file_details (self, file);
    }
    else
    {
        guint n = g_list_length (self->files);
        g_autofree char *title = g_strdup_printf (ngettext ("%u Item", "%u Items", n), n);

        gtk_label_set_text (self->name, title);
        gtk_widget_set_visible (GTK_WIDGET (self->summary), FALSE);
        show_selection_details (self);
    }

    update_marks (self);
}

static void
on_file_changed (NautilusInspector *self)
{
    schedule_update (self);
}

static void
watch_files (NautilusInspector *self,
             gboolean           watch)
{
    for (GList *l = self->files; l != NULL; l = l->next)
    {
        if (watch)
        {
            g_signal_connect_object (l->data, "changed", G_CALLBACK (on_file_changed), self,
                                     G_CONNECT_SWAPPED);
        }
        else
        {
            g_signal_handlers_disconnect_by_func (l->data, on_file_changed, self);
        }
    }
}

static gboolean
same_files (GList *a,
            GList *b)
{
    for (; a != NULL && b != NULL; a = a->next, b = b->next)
    {
        if (a->data != b->data)
        {
            return FALSE;
        }
    }

    return a == NULL && b == NULL;
}

static gboolean
update (gpointer user_data)
{
    NautilusInspector *self = user_data;
    GList *selection = self->slot != NULL ? nautilus_window_slot_get_selection (self->slot) : NULL;
    g_autolist (NautilusFile) files = NULL;
    gboolean is_folder = FALSE;

    self->update_id = 0;

    if (selection != NULL)
    {
        files = nautilus_file_list_copy (selection);
    }
    else if (self->slot != NULL && nautilus_window_slot_get_location (self->slot) != NULL)
    {
        files = g_list_prepend (NULL, nautilus_file_get (nautilus_window_slot_get_location (self->slot)));
        is_folder = TRUE;
    }

    if (same_files (files, self->files))
    {
        /* Only something about them changed. */
        refresh_text (self);
        return G_SOURCE_REMOVE;
    }

    watch_files (self, FALSE);
    g_clear_list (&self->files, g_object_unref);
    self->files = g_steal_pointer (&files);
    self->is_folder = is_folder;
    watch_files (self, TRUE);

    if (self->files == NULL)
    {
        stop_video (self);
        nautilus_ql_canvas_set_files (self->canvas, NULL);
        gtk_widget_set_visible (GTK_WIDGET (self->preview), FALSE);
    }
    else if (self->files->next == NULL)
    {
        gtk_widget_set_visible (GTK_WIDGET (self->preview), TRUE);
        show_preview (self, self->files->data);
    }
    else
    {
        stop_video (self);
        nautilus_text_preview_set_file (self->text, NULL);
        gtk_widget_set_visible (GTK_WIDGET (self->preview), TRUE);
        nautilus_ql_canvas_set_files (self->canvas, self->files);
        gtk_stack_set_visible_child (self->preview, GTK_WIDGET (self->canvas));
    }

    refresh_text (self);

    return G_SOURCE_REMOVE;
}

static void
schedule_update (NautilusInspector *self)
{
    if (self->update_id == 0 && gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
        self->update_id = g_timeout_add (SETTLE_MS, update, self);
    }
}

static void
disconnect_slot (NautilusInspector *self)
{
    if (self->slot != NULL)
    {
        g_clear_signal_handler (&self->selection_id, self->slot);
        g_clear_signal_handler (&self->location_id, self->slot);
        g_clear_weak_pointer (&self->slot);
    }
}

void
nautilus_inspector_set_slot (NautilusInspector  *self,
                             NautilusWindowSlot *slot)
{
    if (self->slot == slot)
    {
        return;
    }

    disconnect_slot (self);

    if (slot != NULL)
    {
        g_set_weak_pointer (&self->slot, slot);
        self->selection_id = g_signal_connect_swapped (slot, "notify::selection",
                                                       G_CALLBACK (schedule_update), self);
        self->location_id = g_signal_connect_swapped (slot, "notify::location",
                                                      G_CALLBACK (schedule_update), self);
    }

    schedule_update (self);
}

/* ----------------------------------------------------------- Building */

static void
on_map (NautilusInspector *self)
{
    /* Catch up with what changed while hidden. */
    schedule_update (self);
}

static void
on_unmap (NautilusInspector *self)
{
    g_clear_handle_id (&self->update_id, g_source_remove);
    stop_video (self);
}

static void
on_canvas_item_activated (NautilusQlCanvas  *canvas,
                          NautilusFile      *file,
                          NautilusInspector *self)
{
    gtk_widget_activate_action (GTK_WIDGET (self), "view.preview-selection", NULL);
}

/* ----------------------------------------------- Density, and the menu */

static void
apply_density (NautilusInspector *self)
{
    g_autofree char *density = g_settings_get_string (nautilus_flier_settings (), "inspector-density");
    gboolean comfortable = g_str_equal (density, "comfortable");
    gboolean dense = g_str_equal (density, "dense");

    gtk_widget_remove_css_class (GTK_WIDGET (self), "density-comfortable");
    gtk_widget_remove_css_class (GTK_WIDGET (self), "density-dense");
    if (comfortable || dense)
    {
        gtk_widget_add_css_class (GTK_WIDGET (self), comfortable ? "density-comfortable" : "density-dense");
    }
    gtk_box_set_spacing (GTK_BOX (self->content), comfortable ? 14 : dense ? 3 : 8);
    gtk_widget_set_margin_start (self->content, dense ? 8 : 12);
    gtk_widget_set_margin_end (self->content, dense ? 8 : 12);
    /* As big as the panel allows: all the height the rest doesn't need. */
    gtk_widget_set_vexpand (GTK_WIDGET (self->preview), TRUE);
    /* At least this tall, whatever the density. */
    gtk_widget_set_size_request (GTK_WIDGET (self->preview), -1, 240);
}

static gboolean
unparent_later (gpointer popover)
{
    gtk_widget_unparent (popover);
    g_object_unref (popover);
    return G_SOURCE_REMOVE;
}

/* Not at once: the menu closes before it runs the chosen item's action,
 * which it finds through its parent. */
static void
on_menu_closed (GtkWidget *popover)
{
    g_idle_add (unparent_later, g_object_ref (popover));
}

static void
on_context_pressed (GtkGestureClick   *click,
                    int                n_press,
                    double             x,
                    double             y,
                    NautilusInspector *self)
{
    g_autoptr (GMenu) menu = g_menu_new ();
    g_autoptr (GMenu) density = g_menu_new ();
    g_autoptr (GMenu) show = g_menu_new ();
    g_autoptr (GMenu) side = g_menu_new ();
    g_autoptr (GMenu) pop = g_menu_new ();
    GtkWidget *popover;

    gtk_gesture_set_state (GTK_GESTURE (click), GTK_EVENT_SEQUENCE_CLAIMED);

    g_menu_append (density, _("Comfortable"), "inspector.inspector-density::comfortable");
    g_menu_append (density, _("Compact"), "inspector.inspector-density::compact");
    g_menu_append (density, _("Dense"), "inspector.inspector-density::dense");
    g_menu_append_section (menu, _("Density"), G_MENU_MODEL (density));

    g_menu_append (show, _("Show Ratings"), "inspector.ratings");
    g_menu_append (show, _("Show Tags"), "inspector.tags");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (show));

    g_menu_append (side, _("Right"), "win.inspector-side::right");
    g_menu_append (side, _("Left"), "win.inspector-side::left");
    g_menu_append (side, _("Bottom"), "win.inspector-side::bottom");
    g_menu_append (side, _("Top"), "win.inspector-side::top");
    g_menu_append_section (menu, _("Dock"), G_MENU_MODEL (side));

    g_menu_append (pop, _("Pop Out"), "win.inspector-pop-out");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (pop));

    popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    gtk_widget_set_parent (popover, GTK_WIDGET (self));
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_widget_set_halign (popover, GTK_ALIGN_START);
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &(GdkRectangle) { (int) x, (int) y, 1, 1 });
    g_signal_connect (popover, "closed", G_CALLBACK (on_menu_closed), NULL);
    gtk_popover_popup (GTK_POPOVER (popover));
}

static void
setup_menu (NautilusInspector *self)
{
    GSimpleActionGroup *group = g_simple_action_group_new ();
    GtkGesture *click = gtk_gesture_click_new ();
    const char *keys[] = { "inspector-density", "ratings", "tags" };

    /* Settings actions, named after their keys. */
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    {
        g_autoptr (GAction) action = g_settings_create_action (nautilus_flier_settings (), keys[i]);

        g_action_map_add_action (G_ACTION_MAP (group), action);
    }
    gtk_widget_insert_action_group (GTK_WIDGET (self), "inspector", G_ACTION_GROUP (group));
    g_object_unref (group);

    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
    g_signal_connect (click, "pressed", G_CALLBACK (on_context_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

    g_signal_connect_object (nautilus_flier_settings (), "changed::inspector-density",
                             G_CALLBACK (apply_density), self, G_CONNECT_SWAPPED);
    apply_density (self);
}

static void
nautilus_inspector_init (NautilusInspector *self)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);

    self->content = box;
    gtk_widget_add_css_class (GTK_WIDGET (self), "inspector");
    gtk_orientable_set_orientation (GTK_ORIENTABLE (gtk_widget_get_layout_manager (GTK_WIDGET (self))),
                                    GTK_ORIENTATION_VERTICAL);

    /* The row at the bottom: a grip in the middle to drag the panel to
     * another side (or out of the window), showing while the pointer is on
     * the row, and a quiet button to pop it out or dock it again. */
    self->header = gtk_center_box_new ();
    gtk_widget_add_css_class (self->header, "inspector-header");
    gtk_widget_set_cursor_from_name (self->header, "grab");
    gtk_widget_set_tooltip_text (self->header, _("Drag to move the inspector to another side, or out of the window"));
    self->grip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (self->grip, "inspector-grip");
    gtk_widget_set_valign (self->grip, GTK_ALIGN_CENTER);
    gtk_center_box_set_center_widget (GTK_CENTER_BOX (self->header), self->grip);
    self->pop_button = gtk_button_new_from_icon_name ("window-new-symbolic");
    gtk_widget_add_css_class (self->pop_button, "flat");
    gtk_widget_add_css_class (self->pop_button, "circular");
    gtk_widget_add_css_class (self->pop_button, "inspector-pop");
    gtk_widget_set_cursor_from_name (self->pop_button, "default");
    gtk_widget_set_tooltip_text (self->pop_button, _("Pop Out"));
    gtk_actionable_set_action_name (GTK_ACTIONABLE (self->pop_button), "win.inspector-pop-out");
    gtk_center_box_set_end_widget (GTK_CENTER_BOX (self->header), self->pop_button);

    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 12);
    gtk_widget_set_margin_bottom (box, 12);

    /* Preview */
    self->preview = GTK_STACK (gtk_stack_new ());
    gtk_widget_add_css_class (GTK_WIDGET (self->preview), "card");
    gtk_widget_add_css_class (GTK_WIDGET (self->preview), "inspector-preview");
    gtk_widget_set_overflow (GTK_WIDGET (self->preview), GTK_OVERFLOW_HIDDEN);
    gtk_stack_set_vhomogeneous (self->preview, TRUE);

    self->canvas = NAUTILUS_QL_CANVAS (nautilus_ql_canvas_new ());
    g_settings_bind (nautilus_flier_settings (), "quick-look-names", self->canvas, "show-names",
                     G_SETTINGS_BIND_GET);
    g_settings_bind (nautilus_flier_settings (), "quick-look-marks", self->canvas, "show-marks",
                     G_SETTINGS_BIND_GET);
    g_signal_connect (self->canvas, "item-activated", G_CALLBACK (on_canvas_item_activated), self);
    g_signal_connect_swapped (self->canvas, "picture-loaded", G_CALLBACK (refresh_text), self);
    gtk_stack_add_child (self->preview, GTK_WIDGET (self->canvas));

    self->image = GTK_WIDGET (nautilus_image_new ());
    nautilus_image_set_size (NAUTILUS_IMAGE (self->image), 192);
    gtk_widget_set_halign (self->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (self->image, GTK_ALIGN_CENTER);
    gtk_stack_add_child (self->preview, self->image);

    self->text = NAUTILUS_TEXT_PREVIEW (nautilus_text_preview_new (TEXT_LIMIT));
    gtk_stack_add_child (self->preview, GTK_WIDGET (self->text));

    self->video = GTK_VIDEO (gtk_video_new ());
    gtk_video_set_autoplay (self->video, FALSE);
    gtk_stack_add_child (self->preview, GTK_WIDGET (self->video));

    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->preview));

    /* Name */
    self->name = GTK_LABEL (gtk_label_new (NULL));
    gtk_label_set_wrap (self->name, TRUE);
    gtk_label_set_wrap_mode (self->name, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_justify (self->name, GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable (self->name, TRUE);
    gtk_widget_add_css_class (GTK_WIDGET (self->name), "title-4");
    gtk_widget_add_css_class (GTK_WIDGET (self->name), "inspector-name");
    /* Selectable with the mouse, but not focused (and so all selected)
     * when a menu closes over it. */
    gtk_widget_set_focusable (GTK_WIDGET (self->name), FALSE);
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->name));

    self->summary = GTK_LABEL (gtk_label_new (NULL));
    gtk_widget_add_css_class (GTK_WIDGET (self->summary), "dim-label");
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->summary));

    /* The name and summary go above the preview. */
    gtk_box_reorder_child_after (GTK_BOX (box), GTK_WIDGET (self->preview), GTK_WIDGET (self->summary));

    /* Stars */
    self->rating_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    for (guint i = 0; i < 5; i++)
    {
        self->stars[i] = gtk_button_new_from_icon_name ("non-starred-symbolic");
        gtk_widget_add_css_class (self->stars[i], "flat");
        gtk_widget_add_css_class (self->stars[i], "star");
        g_object_set_data (G_OBJECT (self->stars[i]), "stars", GUINT_TO_POINTER (i + 1));
        g_signal_connect (self->stars[i], "clicked", G_CALLBACK (on_star_clicked), self);
        gtk_box_append (GTK_BOX (self->rating_box), self->stars[i]);
    }

    /* Tags */
    self->tags_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

    /* Details, with the stars and tags as their first lines (the other
     * lines are added after them for each file). */
    self->details = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    self->rating_row = widget_line (_("Rating"), self->rating_box);
    adw_preferences_group_add (self->details, self->rating_row);
    self->tags_row = widget_line (_("Tags"), self->tags_box);
    adw_preferences_group_add (self->details, self->tags_row);
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->details));


    self->scroller = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), box);
    gtk_widget_set_vexpand (self->scroller, TRUE);
    gtk_widget_set_parent (self->scroller, GTK_WIDGET (self));
    gtk_widget_set_parent (self->header, GTK_WIDGET (self));

    setup_menu (self);
    g_signal_connect (self, "map", G_CALLBACK (on_map), NULL);
    g_signal_connect (self, "unmap", G_CALLBACK (on_unmap), NULL);
    g_signal_connect_object (nautilus_flier_settings (), "changed::ratings",
                             G_CALLBACK (schedule_update), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::tags",
                             G_CALLBACK (schedule_update), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_labels_get (), "tags-changed",
                             G_CALLBACK (schedule_update), self, G_CONNECT_SWAPPED);
}

static void
nautilus_inspector_dispose (GObject *object)
{
    NautilusInspector *self = NAUTILUS_INSPECTOR (object);

    g_clear_handle_id (&self->update_id, g_source_remove);
    disconnect_slot (self);
    watch_files (self, FALSE);
    g_clear_list (&self->files, g_object_unref);
    g_clear_pointer (&self->detail_rows, g_list_free);
    if (self->scroller != NULL)
    {
        gtk_widget_unparent (self->scroller);
        self->scroller = NULL;
    }
    if (self->header != NULL)
    {
        gtk_widget_unparent (self->header);
        self->header = NULL;
    }

    G_OBJECT_CLASS (nautilus_inspector_parent_class)->dispose (object);
}

static void
nautilus_inspector_class_init (NautilusInspectorClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_inspector_dispose;
    gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass), GTK_TYPE_BOX_LAYOUT);
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "nautilus-inspector");
}

GtkWidget *
nautilus_inspector_new (void)
{
    return g_object_new (NAUTILUS_TYPE_INSPECTOR, NULL);
}

GtkWidget *
nautilus_inspector_get_grip (NautilusInspector *self)
{
    g_return_val_if_fail (NAUTILUS_IS_INSPECTOR (self), NULL);

    return self->header;
}

void
nautilus_inspector_set_detached (NautilusInspector *self,
                                 gboolean           detached)
{
    g_return_if_fail (NAUTILUS_IS_INSPECTOR (self));

    /* In its own window the panel is moved by the window's title bar. */
    gtk_widget_set_visible (self->grip, !detached);
    gtk_widget_set_cursor_from_name (self->header, detached ? NULL : "grab");
    gtk_widget_set_tooltip_text (self->header, detached ? NULL
                                 : _("Drag to move the inspector to another side, or out of the window"));
    gtk_button_set_icon_name (GTK_BUTTON (self->pop_button),
                              detached ? "sidebar-show-right-symbolic" : "window-new-symbolic");
    gtk_widget_set_tooltip_text (self->pop_button, detached ? _("Dock in the Window") : _("Pop Out"));
}
