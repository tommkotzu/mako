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
    GtkWidget *stars[5];
    GtkWidget *tags_box;
    AdwPreferencesGroup *details;
    GList *detail_rows;
    GtkWidget *open_button;
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

    if (value == NULL || *value == '\0' || g_strcmp0 (value, "—") == 0)
    {
        return;
    }

    row = adw_action_row_new ();
    gtk_widget_add_css_class (row, "property");
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value);
    adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
    adw_preferences_group_add (self->details, row);
    self->detail_rows = g_list_append (self->detail_rows, row);
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

    gtk_widget_set_visible (self->rating_box, writable && g_settings_get_boolean (flier, "ratings"));
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
        GtkWidget *edit = gtk_button_new_with_mnemonic (_("_Tags…"));

        gtk_widget_add_css_class (edit, "flat");
        gtk_actionable_set_action_name (GTK_ACTIONABLE (edit), "view.edit-tags");
        gtk_box_append (GTK_BOX (self->tags_box), edit);
    }
    gtk_widget_set_visible (self->tags_box, tags_on);
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
        gtk_widget_set_visible (self->open_button, FALSE);
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

    gtk_widget_set_visible (self->open_button, !self->is_folder);
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
on_open_clicked (NautilusInspector *self)
{
    gtk_widget_activate_action (GTK_WIDGET (self), "view.open-with-default-application", NULL);
}

static void
on_canvas_item_activated (NautilusQlCanvas  *canvas,
                          NautilusFile      *file,
                          NautilusInspector *self)
{
    gtk_widget_activate_action (GTK_WIDGET (self), "view.preview-selection", NULL);
}

static void
nautilus_inspector_init (NautilusInspector *self)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);

    gtk_widget_add_css_class (GTK_WIDGET (self), "inspector");
    gtk_orientable_set_orientation (GTK_ORIENTABLE (gtk_widget_get_layout_manager (GTK_WIDGET (self))),
                                    GTK_ORIENTATION_VERTICAL);

    /* The row at the bottom: a grip to drag the panel to another side (or
     * out of the window), the Open button, and a button to pop it out or
     * dock it again. */
    self->header = gtk_center_box_new ();
    gtk_widget_add_css_class (self->header, "inspector-header");
    gtk_widget_set_cursor_from_name (self->header, "grab");
    gtk_widget_set_tooltip_text (self->header, _("Drag to move the inspector to another side, or out of the window"));
    self->grip = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (self->grip, "inspector-grip");
    gtk_widget_set_valign (self->grip, GTK_ALIGN_CENTER);
    gtk_center_box_set_start_widget (GTK_CENTER_BOX (self->header), self->grip);
    self->pop_button = gtk_button_new_from_icon_name ("window-new-symbolic");
    gtk_widget_add_css_class (self->pop_button, "flat");
    gtk_widget_add_css_class (self->pop_button, "circular");
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
    gtk_widget_set_size_request (GTK_WIDGET (self->preview), -1, 240);
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
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->name));

    self->summary = GTK_LABEL (gtk_label_new (NULL));
    gtk_widget_add_css_class (GTK_WIDGET (self->summary), "dim-label");
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->summary));

    /* Stars */
    self->rating_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign (self->rating_box, GTK_ALIGN_CENTER);
    for (guint i = 0; i < 5; i++)
    {
        self->stars[i] = gtk_button_new_from_icon_name ("non-starred-symbolic");
        gtk_widget_add_css_class (self->stars[i], "flat");
        gtk_widget_add_css_class (self->stars[i], "star");
        g_object_set_data (G_OBJECT (self->stars[i]), "stars", GUINT_TO_POINTER (i + 1));
        g_signal_connect (self->stars[i], "clicked", G_CALLBACK (on_star_clicked), self);
        gtk_box_append (GTK_BOX (self->rating_box), self->stars[i]);
    }
    gtk_box_append (GTK_BOX (box), self->rating_box);

    /* Tags */
    self->tags_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (self->tags_box, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), self->tags_box);

    /* Details */
    self->details = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->details));

    /* Open, in the row at the bottom. */
    self->open_button = gtk_button_new_with_mnemonic (_("_Open"));
    gtk_widget_add_css_class (self->open_button, "pill");
    gtk_widget_add_css_class (self->open_button, "inspector-open");
    gtk_widget_set_cursor_from_name (self->open_button, "default");
    gtk_widget_set_valign (self->open_button, GTK_ALIGN_CENTER);
    g_signal_connect_swapped (self->open_button, "clicked", G_CALLBACK (on_open_clicked), self);
    gtk_center_box_set_center_widget (GTK_CENTER_BOX (self->header), self->open_button);

    self->scroller = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), box);
    gtk_widget_set_vexpand (self->scroller, TRUE);
    gtk_widget_set_parent (self->scroller, GTK_WIDGET (self));
    gtk_widget_set_parent (self->header, GTK_WIDGET (self));

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
