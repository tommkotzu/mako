/*
 * nautilus-quick-look.c: the built-in Quick Look, a card over the window
 *
 * Space shows the selection: one picture to zoom into, a video or song to
 * play, a text file with line numbers, or a big thumbnail and the details
 * of anything else. Several selected items are laid out on a canvas to
 * zoom around, rate and tag. Everything works from the left hand:
 *
 *   Space, Esc        close (Esc first goes back from an item to the canvas)
 *   1-5, `            rate the item under the pointer (or all), clear
 *   W, S / + -, Q     zoom in, out, fit   (also scroll and pinch)
 *   A, D / arrows     previous, next item
 *   K  J L  E F  M    play/pause, back/ahead 5 s, next/previous frame, mute
 *   N, T              names, ratings and tags on the canvas
 *   Enter, Ctrl+O     open
 *   Tab               fullscreen
 *
 * It lives in the window rather than in a window of its own, so it opens
 * and closes as fast as we like, not at the speed of the shell's animation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "nautilus-quick-look"

#include "nautilus-quick-look.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "nautilus-file.h"
#include "nautilus-files-view.h"
#include "nautilus-image.h"
#include "nautilus-labels.h"
#include "nautilus-labels-ui.h"
#include "nautilus-mime-actions.h"
#include "nautilus-ql-canvas.h"
#include "nautilus-text-preview.h"
#include "nautilus-view-info.h"

/* Text files are shown up to this much. */
#define TEXT_LIMIT (512 * 1024)
#define FRAME_USEC (G_USEC_PER_SEC / 30)

#define NAUTILUS_TYPE_QUICK_LOOK (nautilus_quick_look_get_type ())
G_DECLARE_FINAL_TYPE (NautilusQuickLook, nautilus_quick_look, NAUTILUS, QUICK_LOOK, AdwBin)

struct _NautilusQuickLook
{
    AdwBin parent_instance;

    GtkWidget *card;
    AdwAnimation *fade;
    gboolean shown;            /* open, or opening */
    gboolean made_fullscreen;  /* the window, by Tab */

    NautilusFilesView *view;   /* weak */
    GList *files;              /* the selection shown */
    NautilusFile *alone;       /* one of several, looked at alone */

    AdwWindowTitle *title;
    GtkWidget *rating_box;
    GtkWidget *stars[5];
    GtkWidget *canvas_buttons;
    GtkWidget *open_button;
    GtkStack *stack;

    NautilusQlCanvas *canvas;

    GtkWidget *media_picture;
    GtkWidget *media_cover;
    GtkMediaControls *media_controls;
    GtkMediaStream *media;

    NautilusTextPreview *text;

    GtkWidget *info_image;
    GtkLabel *info_name;
    GtkLabel *info_details;
    GtkLabel *keys_hint;
};

G_DEFINE_TYPE (NautilusQuickLook, nautilus_quick_look, ADW_TYPE_BIN)

/* Twice as fast as the shell's dialog animation. */
#define FADE_IN_MS 80
#define FADE_OUT_MS 60

static void ql_hide (NautilusQuickLook *self);
static gboolean ql_is_fullscreen (NautilusQuickLook *self);
static void ql_set_fullscreen (NautilusQuickLook *self,
                               gboolean           fullscreen);

static NautilusQuickLook *quick_look = NULL;

static void show_current (NautilusQuickLook *self);

/* The one item shown alone, if any. */
static NautilusFile *
shown_file (NautilusQuickLook *self)
{
    if (self->alone != NULL)
    {
        return self->alone;
    }

    return self->files != NULL && self->files->next == NULL ? self->files->data : NULL;
}

/* ---------------------------------------------------------- Details */

static char *
file_details (NautilusFile *file,
              int           width,
              int           height)
{
    g_autoptr (GPtrArray) parts = g_ptr_array_new_with_free_func (g_free);
    g_autofree char *type = nautilus_file_get_string_attribute (file, "type");
    g_autofree char *size = nautilus_file_get_string_attribute (file, "size");
    g_autofree char *date = nautilus_file_get_string_attribute (file, "date_modified");

    if (type != NULL && *type != '\0')
    {
        g_ptr_array_add (parts, g_steal_pointer (&type));
    }
    if (size != NULL && *size != '\0' && g_strcmp0 (size, "—") != 0)
    {
        g_ptr_array_add (parts, g_steal_pointer (&size));
    }
    if (width > 0 && height > 0)
    {
        g_ptr_array_add (parts, g_strdup_printf ("%d × %d", width, height));
    }
    if (date != NULL && *date != '\0' && g_strcmp0 (date, "—") != 0)
    {
        g_ptr_array_add (parts, g_steal_pointer (&date));
    }
    g_ptr_array_add (parts, NULL);

    return g_strjoinv ("  ·  ", (char **) parts->pdata);
}

static char *
selection_details (GList *files)
{
    guint folders = 0, others = 0;
    goffset total = 0;
    g_autofree char *counts = NULL;

    for (GList *l = files; l != NULL; l = l->next)
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

    if (folders > 0 && others > 0)
    {
        g_autofree char *f = g_strdup_printf (ngettext ("%u folder", "%u folders", folders), folders);
        g_autofree char *o = g_strdup_printf (ngettext ("%u file", "%u files", others), others);

        counts = g_strdup_printf ("%s, %s", f, o);
    }
    else if (folders > 0)
    {
        counts = g_strdup_printf (ngettext ("%u folder", "%u folders", folders), folders);
    }
    else
    {
        counts = g_strdup_printf (ngettext ("%u file", "%u files", others), others);
    }

    if (total > 0)
    {
        g_autofree char *size = g_format_size (total);

        return g_strdup_printf ("%s  ·  %s", counts, size);
    }

    return g_steal_pointer (&counts);
}

static void
update_title (NautilusQuickLook *self)
{
    NautilusFile *file = shown_file (self);

    if (file != NULL)
    {
        int w = 0, h = 0;
        g_autofree char *details = NULL;

        nautilus_ql_canvas_get_picture_size (self->canvas, &w, &h);
        details = file_details (file, w, h);
        adw_window_title_set_title (self->title, nautilus_file_get_display_name (file));
        adw_window_title_set_subtitle (self->title, details);
    }
    else
    {
        guint n = g_list_length (self->files);
        g_autofree char *title = g_strdup_printf (ngettext ("%u Item", "%u Items", n), n);
        g_autofree char *details = selection_details (self->files);

        adw_window_title_set_title (self->title, title);
        adw_window_title_set_subtitle (self->title, details);
    }
}

static void
update_rating (NautilusQuickLook *self)
{
    NautilusFile *file = shown_file (self);
    gboolean ratings = g_settings_get_boolean (nautilus_flier_settings (), "ratings");
    guint rating = file != NULL ? nautilus_file_get_rating (file) : 0;

    gtk_widget_set_visible (self->rating_box, ratings && file != NULL &&
                            nautilus_file_can_write (file));
    gtk_widget_set_visible (self->canvas_buttons, file == NULL);

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
}

/* ------------------------------------------------------------- Rating */

static void
rate (NautilusQuickLook *self,
      guint              stars)
{
    NautilusFile *target = shown_file (self);

    if (!g_settings_get_boolean (nautilus_flier_settings (), "ratings"))
    {
        return;
    }

    if (target == NULL)
    {
        target = nautilus_ql_canvas_get_hovered (self->canvas);
    }

    if (target != NULL)
    {
        nautilus_labels_set_rating (&(GList){ .data = target }, stars);
    }
    else
    {
        /* Nothing under the pointer: the whole selection. */
        nautilus_labels_set_rating (self->files, stars);
    }
}

static void
on_star_clicked (GtkButton         *button,
                 NautilusQuickLook *self)
{
    NautilusFile *file = shown_file (self);
    guint stars = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "stars"));

    if (file != NULL)
    {
        rate (self, stars == nautilus_file_get_rating (file) ? 0 : stars);
    }
}

/* -------------------------------------------------------------- Media */

static void
stop_media (NautilusQuickLook *self)
{
    if (self->media != NULL)
    {
        gtk_media_stream_pause (self->media);
        gtk_media_controls_set_media_stream (self->media_controls, NULL);
        gtk_picture_set_paintable (GTK_PICTURE (self->media_picture), NULL);
        g_clear_object (&self->media);
    }
}

static void
show_media (NautilusQuickLook *self,
            NautilusFile      *file,
            gboolean           audio)
{
    g_autoptr (GFile) location = nautilus_file_get_location (file);
    gboolean muted = self->media != NULL && gtk_media_stream_get_muted (self->media);

    stop_media (self);

    self->media = gtk_media_file_new_for_file (location);
    gtk_media_stream_set_muted (self->media, muted);
    gtk_media_controls_set_media_stream (self->media_controls, self->media);
    gtk_picture_set_paintable (GTK_PICTURE (self->media_picture), GDK_PAINTABLE (self->media));
    gtk_widget_set_visible (self->media_picture, !audio);
    gtk_widget_set_visible (self->media_cover, audio);

    if (audio)
    {
        /* Album art, if there's a thumbnail for it. */
        g_autoptr (GdkPaintable) icon = nautilus_file_get_icon_paintable (file, 256,
                                                                          gtk_widget_get_scale_factor (GTK_WIDGET (self)),
                                                                          NAUTILUS_FILE_ICON_FLAGS_NONE);

        nautilus_image_set_fallback (NAUTILUS_IMAGE (self->media_cover), icon);
        nautilus_image_set_source (NAUTILUS_IMAGE (self->media_cover), location);
    }

    gtk_media_stream_play (self->media);
    gtk_stack_set_visible_child_name (self->stack, "media");
}

static void
seek_by (NautilusQuickLook *self,
         gint64             usec)
{
    gint64 to;

    if (self->media == NULL || !gtk_media_stream_is_seekable (self->media))
    {
        return;
    }

    to = CLAMP (gtk_media_stream_get_timestamp (self->media) + usec, 0,
                MAX (gtk_media_stream_get_duration (self->media), 0));
    gtk_media_stream_seek (self->media, to);
}

static void
step_frame (NautilusQuickLook *self,
            int                frames)
{
    if (self->media != NULL)
    {
        gtk_media_stream_pause (self->media);
        seek_by (self, frames * FRAME_USEC);
    }
}

/* --------------------------------------------------------------- Info */

static void
show_info (NautilusQuickLook *self,
           NautilusFile      *file)
{
    g_autoptr (GFile) location = nautilus_file_get_location (file);
    g_autoptr (GdkPaintable) icon = nautilus_file_get_icon_paintable (file, 256,
                                                                      gtk_widget_get_scale_factor (GTK_WIDGET (self)),
                                                                      NAUTILUS_FILE_ICON_FLAGS_NONE);
    g_autofree char *details = file_details (file, 0, 0);

    /* Documents, fonts and the like have thumbnails worth seeing big. */
    nautilus_image_set_fallback (NAUTILUS_IMAGE (self->info_image), icon);
    nautilus_image_set_source (NAUTILUS_IMAGE (self->info_image),
                               nautilus_file_is_regular_file (file) ? location : NULL);
    gtk_label_set_text (self->info_name, nautilus_file_get_display_name (file));
    gtk_label_set_text (self->info_details, details);
    gtk_stack_set_visible_child_name (self->stack, "info");
}

/* ------------------------------------------------------------ Showing */

/* The keys that work for what's shown, at the bottom of the card. */
static void
update_keys_hint (NautilusQuickLook *self)
{
    const char *page = gtk_stack_get_visible_child_name (self->stack);
    const char *text;

    if (g_strcmp0 (page, "media") == 0)
    {
        text = _("K play · J/L −5/+5 s · Shift+A/D or ,/. frame · M mute · A/D previous/next · Tab full screen · Space close");
    }
    else if (shown_file (self) == NULL)
    {
        text = _("W/S zoom · Q fit · Z actual size · arrows move · N names · T marks · Enter open · Tab full screen · Space close");
    }
    else if (g_strcmp0 (page, "canvas") == 0)
    {
        text = _("W/S zoom · Q fit · Z actual size · A/D previous/next · 1–5 rate · Enter open · Tab full screen · Space close");
    }
    else
    {
        text = _("A/D previous/next · 1–5 rate · Enter open · Ctrl+O open · Tab full screen · Space close");
    }
    gtk_label_set_text (self->keys_hint, text);
}

static void
show_current (NautilusQuickLook *self)
{
    NautilusFile *file = shown_file (self);

    nautilus_text_preview_set_file (self->text, NULL);

    if (file == NULL)
    {
        stop_media (self);
        nautilus_ql_canvas_set_files (self->canvas, self->files);
        gtk_stack_set_visible_child_name (self->stack, "canvas");
    }
    else
    {
        const char *mime = nautilus_file_get_mime_type (file);
        gboolean regular = nautilus_file_is_regular_file (file);

        if (regular && g_str_has_prefix (mime, "image/"))
        {
            stop_media (self);
            nautilus_ql_canvas_set_files (self->canvas, &(GList){ .data = file });
            gtk_stack_set_visible_child_name (self->stack, "canvas");
        }
        else if (regular && (g_str_has_prefix (mime, "video/") || g_str_has_prefix (mime, "audio/")))
        {
            show_media (self, file, g_str_has_prefix (mime, "audio/"));
        }
        else if (regular && nautilus_text_preview_handles (mime))
        {
            stop_media (self);
            nautilus_text_preview_set_file (self->text, file);
            gtk_stack_set_visible_child_name (self->stack, "text");
        }
        else
        {
            stop_media (self);
            show_info (self, file);
        }
    }

    update_title (self);
    update_rating (self);
    update_keys_hint (self);
}

static void
on_item_activated (NautilusQlCanvas  *canvas,
                   NautilusFile      *file,
                   NautilusQuickLook *self)
{
    g_set_object (&self->alone, file);
    show_current (self);
}

static void
on_file_changed (NautilusQuickLook *self)
{
    update_rating (self);
    update_title (self);
}

static void
watch_files (NautilusQuickLook *self,
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

static void
set_files (NautilusQuickLook *self,
           GList             *files)
{
    watch_files (self, FALSE);
    g_clear_object (&self->alone);
    g_clear_list (&self->files, g_object_unref);
    self->files = nautilus_file_list_copy (files);
    watch_files (self, TRUE);
    show_current (self);
}

/* ---------------------------------------------------------- Actions */

static void
open_shown (NautilusQuickLook *self)
{
    NautilusFile *file = shown_file (self);

    if (self->view == NULL)
    {
        return;
    }

    if (file != NULL)
    {
        nautilus_files_view_activate_file (self->view, file, 0);
    }
    else
    {
        nautilus_files_view_activate_selection (self->view, FALSE);
    }
    ql_hide (self);
}

static void
move_selection (NautilusQuickLook *self,
                GtkDirectionType   direction)
{
    if (self->view != NULL && self->alone == NULL && shown_file (self) != NULL)
    {
        nautilus_files_view_preview_selection_event (self->view, direction);
    }
}

/* The arrows on the canvas of several files: from item to item while it
 * shows them all (in two dimensions when opened from the grid view), around
 * it when zoomed in. */
static void
move_on_canvas (NautilusQuickLook *self,
                GtkDirectionType   direction,
                double             dx,
                double             dy)
{
    if (!nautilus_ql_canvas_get_at_fit (self->canvas))
    {
        nautilus_ql_canvas_pan (self->canvas, dx, dy);
        return;
    }

    gboolean spatial = self->view == NULL ||
                       nautilus_files_view_get_view_id (self->view) == NAUTILUS_VIEW_GRID_ID;

    nautilus_ql_canvas_move_hovered (self->canvas, direction, spatial);
}

static void
toggle_setting (const char *key)
{
    GSettings *settings = nautilus_flier_settings ();

    g_settings_set_boolean (settings, key, !g_settings_get_boolean (settings, key));
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                NautilusQuickLook     *self)
{
    GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();
    gboolean on_text = g_strcmp0 (gtk_stack_get_visible_child_name (self->stack), "text") == 0;
    guint key = gdk_keyval_to_lower (keyval);

    if (mods == GDK_CONTROL_MASK && key == GDK_KEY_o)
    {
        open_shown (self);
        return GDK_EVENT_STOP;
    }

    if ((mods & ~GDK_SHIFT_MASK) != 0)
    {
        return GDK_EVENT_PROPAGATE;
    }

    /* Frame by frame: Shift+A/D (or Shift+arrows), comma and period. */
    if (self->media != NULL)
    {
        int frames = 0;

        if (mods == GDK_SHIFT_MASK && (key == GDK_KEY_a || key == GDK_KEY_Left))
        {
            frames = -1;
        }
        else if (mods == GDK_SHIFT_MASK && (key == GDK_KEY_d || key == GDK_KEY_Right))
        {
            frames = 1;
        }
        else if (keyval == GDK_KEY_comma || keyval == GDK_KEY_less)
        {
            frames = -1;
        }
        else if (keyval == GDK_KEY_period || keyval == GDK_KEY_greater)
        {
            frames = 1;
        }
        if (frames != 0)
        {
            step_frame (self, frames);
            return GDK_EVENT_STOP;
        }
    }

    switch (key)
    {
        case GDK_KEY_space:
        {
            ql_hide (self);
        }
        break;

        case GDK_KEY_Escape:
        {
            if (self->alone != NULL)
            {
                g_clear_object (&self->alone);
                show_current (self);
            }
            else if (ql_is_fullscreen (self))
            {
                ql_set_fullscreen (self, FALSE);
            }
            else
            {
                ql_hide (self);
            }
        }
        break;

        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        {
            open_shown (self);
        }
        break;

        case GDK_KEY_Tab:
        case GDK_KEY_F11:
        {
            ql_set_fullscreen (self, !ql_is_fullscreen (self));
        }
        break;

        case GDK_KEY_1:
        case GDK_KEY_2:
        case GDK_KEY_3:
        case GDK_KEY_4:
        case GDK_KEY_5:
        {
            rate (self, key - GDK_KEY_0);
        }
        break;

        case GDK_KEY_grave:
        case GDK_KEY_0:
        {
            rate (self, 0);
        }
        break;

        /* Zoom */
        case GDK_KEY_w:
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:
        {
            nautilus_ql_canvas_zoom (self->canvas, 1.25);
        }
        break;

        case GDK_KEY_s:
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract:
        {
            nautilus_ql_canvas_zoom (self->canvas, 0.8);
        }
        break;

        case GDK_KEY_q:
        {
            nautilus_ql_canvas_fit (self->canvas);
        }
        break;

        case GDK_KEY_z:
        {
            nautilus_ql_canvas_actual_size (self->canvas);
        }
        break;

        /* Moving between items, or around the canvas */
        case GDK_KEY_a:
        case GDK_KEY_Left:
        {
            if (shown_file (self) != NULL)
            {
                move_selection (self, GTK_DIR_LEFT);
            }
            else
            {
                move_on_canvas (self, GTK_DIR_LEFT, 80, 0);
            }
        }
        break;

        case GDK_KEY_d:
        case GDK_KEY_Right:
        {
            if (shown_file (self) != NULL)
            {
                move_selection (self, GTK_DIR_RIGHT);
            }
            else
            {
                move_on_canvas (self, GTK_DIR_RIGHT, -80, 0);
            }
        }
        break;

        case GDK_KEY_Up:
        case GDK_KEY_Down:
        {
            if (on_text)
            {
                return GDK_EVENT_PROPAGATE;
            }
            if (shown_file (self) != NULL)
            {
                move_selection (self, key == GDK_KEY_Up ? GTK_DIR_UP : GTK_DIR_DOWN);
            }
            else
            {
                move_on_canvas (self, key == GDK_KEY_Up ? GTK_DIR_UP : GTK_DIR_DOWN,
                                0, key == GDK_KEY_Up ? 80 : -80);
            }
        }
        break;

        /* Playing */
        case GDK_KEY_k:
        {
            if (self->media != NULL)
            {
                gtk_media_stream_set_playing (self->media, !gtk_media_stream_get_playing (self->media));
            }
        }
        break;

        case GDK_KEY_j:
        {
            seek_by (self, -5 * G_USEC_PER_SEC);
        }
        break;

        case GDK_KEY_l:
        {
            seek_by (self, 5 * G_USEC_PER_SEC);
        }
        break;

        case GDK_KEY_e:
        {
            step_frame (self, 1);
        }
        break;

        case GDK_KEY_f:
        {
            step_frame (self, -1);
        }
        break;

        case GDK_KEY_m:
        {
            if (self->media != NULL)
            {
                gtk_media_stream_set_muted (self->media, !gtk_media_stream_get_muted (self->media));
            }
        }
        break;

        /* The canvas */
        case GDK_KEY_n:
        {
            toggle_setting ("quick-look-names");
        }
        break;

        case GDK_KEY_t:
        {
            toggle_setting ("quick-look-marks");
        }
        break;

        default:
        {
            /* Typing mustn't reach the files behind (and start a search),
             * except into the text preview. */
            if (!on_text && gdk_keyval_to_unicode (keyval) != 0)
            {
                return GDK_EVENT_STOP;
            }
            return GDK_EVENT_PROPAGATE;
        }
    }

    return GDK_EVENT_STOP;
}

/* ------------------------------------------------- Showing, hiding */

static gboolean
ql_is_fullscreen (NautilusQuickLook *self)
{
    return gtk_widget_has_css_class (GTK_WIDGET (self), "fullscreen");
}

static void
ql_set_fullscreen (NautilusQuickLook *self,
                   gboolean           fullscreen)
{
    GtkWindow *window = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));

    if (fullscreen)
    {
        gtk_widget_add_css_class (GTK_WIDGET (self), "fullscreen");
        if (window != NULL && !gtk_window_is_fullscreen (window))
        {
            self->made_fullscreen = TRUE;
            gtk_window_fullscreen (window);
        }
    }
    else
    {
        gtk_widget_remove_css_class (GTK_WIDGET (self), "fullscreen");
        if (window != NULL && self->made_fullscreen)
        {
            gtk_window_unfullscreen (window);
        }
        self->made_fullscreen = FALSE;
    }
}

static void
set_opacity (double             value,
             NautilusQuickLook *self)
{
    gtk_widget_set_opacity (GTK_WIDGET (self), value);
}

static void
on_fade_done (NautilusQuickLook *self)
{
    if (self->shown)
    {
        return;
    }

    /* Let go of everything; the card stays, to come back quickly. */
    gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
    stop_media (self);
    nautilus_text_preview_set_file (self->text, NULL);
    watch_files (self, FALSE);
    g_clear_object (&self->alone);
    g_clear_list (&self->files, g_object_unref);
    nautilus_ql_canvas_set_files (self->canvas, NULL);
}

static void
fade_to (NautilusQuickLook *self,
         double             to,
         guint              ms)
{
    adw_timed_animation_set_value_from (ADW_TIMED_ANIMATION (self->fade),
                                        gtk_widget_get_opacity (GTK_WIDGET (self)));
    adw_timed_animation_set_value_to (ADW_TIMED_ANIMATION (self->fade), to);
    adw_timed_animation_set_duration (ADW_TIMED_ANIMATION (self->fade), ms);
    adw_animation_play (self->fade);
}

static void
ql_show (NautilusQuickLook *self)
{
    if (!self->shown)
    {
        self->shown = TRUE;
        if (!gtk_widget_get_visible (GTK_WIDGET (self)))
        {
            gtk_widget_set_opacity (GTK_WIDGET (self), 0);
            gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
        }
        fade_to (self, 1, FADE_IN_MS);
    }
    gtk_widget_grab_focus (GTK_WIDGET (self));
}

static void
ql_hide (NautilusQuickLook *self)
{
    if (!self->shown)
    {
        return;
    }

    self->shown = FALSE;
    stop_media (self);
    if (ql_is_fullscreen (self))
    {
        ql_set_fullscreen (self, FALSE);
    }
    fade_to (self, 0, FADE_OUT_MS);

    /* The keys go back to the files. */
    if (self->view != NULL)
    {
        gtk_widget_grab_focus (GTK_WIDGET (self->view));
    }
}

/* A click on the dimmed window around the card closes. */
static void
on_backdrop_pressed (GtkGestureClick   *gesture,
                     int                n_press,
                     double             x,
                     double             y,
                     NautilusQuickLook *self)
{
    if (gtk_widget_pick (GTK_WIDGET (self), x, y, GTK_PICK_DEFAULT) == GTK_WIDGET (self))
    {
        gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        ql_hide (self);
    }
}

/* --------------------------------------------------------- Building */

static GtkWidget *
build_header (NautilusQuickLook *self)
{
    GtkWidget *header = adw_header_bar_new ();
    GtkWidget *button;

    self->title = ADW_WINDOW_TITLE (adw_window_title_new ("", ""));
    adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (self->title));
    adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
    adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

    button = gtk_button_new_from_icon_name ("window-close-symbolic");
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_add_css_class (button, "circular");
    gtk_widget_set_tooltip_text (button, _("Close (Space)"));
    gtk_widget_set_focus_on_click (button, FALSE);
    g_signal_connect_swapped (button, "clicked", G_CALLBACK (ql_hide), self);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), button);

    /* Stars of the item shown alone. */
    self->rating_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (self->rating_box, "quick-look-rating");
    for (guint i = 0; i < 5; i++)
    {
        g_autofree char *tooltip = g_strdup_printf (ngettext ("%u Star (%u)", "%u Stars (%u)", i + 1), i + 1, i + 1);

        self->stars[i] = gtk_button_new_from_icon_name ("non-starred-symbolic");
        gtk_widget_add_css_class (self->stars[i], "flat");
        gtk_widget_add_css_class (self->stars[i], "star");
        gtk_widget_set_tooltip_text (self->stars[i], tooltip);
        gtk_widget_set_focus_on_click (self->stars[i], FALSE);
        g_object_set_data (G_OBJECT (self->stars[i]), "stars", GUINT_TO_POINTER (i + 1));
        g_signal_connect (self->stars[i], "clicked", G_CALLBACK (on_star_clicked), self);
        gtk_box_append (GTK_BOX (self->rating_box), self->stars[i]);
    }
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->rating_box);

    /* Switches of the canvas. */
    self->canvas_buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

    button = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (button), "text-x-generic-symbolic");
    gtk_widget_set_tooltip_text (button, _("Show Names (N)"));
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_set_focus_on_click (button, FALSE);
    g_settings_bind (nautilus_flier_settings (), "quick-look-names", button, "active", G_SETTINGS_BIND_DEFAULT);
    gtk_box_append (GTK_BOX (self->canvas_buttons), button);

    button = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (button), "starred-symbolic");
    gtk_widget_set_tooltip_text (button, _("Show Ratings and Tags (T)"));
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_set_focus_on_click (button, FALSE);
    g_settings_bind (nautilus_flier_settings (), "quick-look-marks", button, "active", G_SETTINGS_BIND_DEFAULT);
    gtk_box_append (GTK_BOX (self->canvas_buttons), button);

    button = gtk_button_new_from_icon_name ("zoom-fit-best-symbolic");
    gtk_widget_set_tooltip_text (button, _("Show Everything (Q)"));
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_set_focus_on_click (button, FALSE);
    g_signal_connect_swapped (button, "clicked", G_CALLBACK (nautilus_ql_canvas_fit), self->canvas);
    gtk_box_append (GTK_BOX (self->canvas_buttons), button);

    adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->canvas_buttons);

    self->open_button = gtk_button_new_with_mnemonic (_("_Open"));
    gtk_widget_add_css_class (self->open_button, "suggested-action");
    gtk_widget_set_tooltip_text (self->open_button, _("Open (Enter)"));
    gtk_widget_set_focus_on_click (self->open_button, FALSE);
    g_signal_connect_swapped (self->open_button, "clicked", G_CALLBACK (open_shown), self);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->open_button);

    return header;
}

static GtkWidget *
build_media_page (NautilusQuickLook *self)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    self->media_picture = gtk_picture_new ();
    gtk_picture_set_content_fit (GTK_PICTURE (self->media_picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_vexpand (self->media_picture, TRUE);
    gtk_box_append (GTK_BOX (box), self->media_picture);

    self->media_cover = GTK_WIDGET (nautilus_image_new ());
    nautilus_image_set_size (NAUTILUS_IMAGE (self->media_cover), 256);
    gtk_widget_set_halign (self->media_cover, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (self->media_cover, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand (self->media_cover, TRUE);
    gtk_box_append (GTK_BOX (box), self->media_cover);

    self->media_controls = GTK_MEDIA_CONTROLS (gtk_media_controls_new (NULL));
    gtk_widget_add_css_class (GTK_WIDGET (self->media_controls), "toolbar");
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->media_controls));

    return box;
}

static GtkWidget *
build_info_page (NautilusQuickLook *self)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);

    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start (box, 24);
    gtk_widget_set_margin_end (box, 24);

    self->info_image = GTK_WIDGET (nautilus_image_new ());
    nautilus_image_set_size (NAUTILUS_IMAGE (self->info_image), 384);
    gtk_widget_set_halign (self->info_image, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), self->info_image);

    self->info_name = GTK_LABEL (gtk_label_new (NULL));
    gtk_label_set_wrap (self->info_name, TRUE);
    gtk_label_set_justify (self->info_name, GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (GTK_WIDGET (self->info_name), "title-2");
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->info_name));

    self->info_details = GTK_LABEL (gtk_label_new (NULL));
    gtk_label_set_wrap (self->info_details, TRUE);
    gtk_label_set_justify (self->info_details, GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (GTK_WIDGET (self->info_details), "dim-label");
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->info_details));

    return box;
}

static void
nautilus_quick_look_init (NautilusQuickLook *self)
{
    GtkWidget *toolbar = adw_toolbar_view_new ();
    GtkEventController *keys;

    GtkGesture *click;

    gtk_widget_add_css_class (GTK_WIDGET (self), "quick-look");
    gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
    gtk_widget_set_visible (GTK_WIDGET (self), FALSE);

    self->canvas = NAUTILUS_QL_CANVAS (nautilus_ql_canvas_new ());
    g_signal_connect (self->canvas, "item-activated", G_CALLBACK (on_item_activated), self);
    g_signal_connect_swapped (self->canvas, "picture-loaded", G_CALLBACK (update_title), self);
    g_settings_bind (nautilus_flier_settings (), "quick-look-names", self->canvas, "show-names",
                     G_SETTINGS_BIND_GET);
    g_settings_bind (nautilus_flier_settings (), "quick-look-marks", self->canvas, "show-marks",
                     G_SETTINGS_BIND_GET);

    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), build_header (self));

    self->keys_hint = GTK_LABEL (gtk_label_new (NULL));
    gtk_label_set_wrap (self->keys_hint, TRUE);
    gtk_label_set_justify (self->keys_hint, GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (GTK_WIDGET (self->keys_hint), "quick-look-keys");
    gtk_widget_add_css_class (GTK_WIDGET (self->keys_hint), "dim-label");
    gtk_widget_add_css_class (GTK_WIDGET (self->keys_hint), "caption");
    adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (self->keys_hint));

    self->stack = GTK_STACK (gtk_stack_new ());
    gtk_stack_add_named (self->stack, GTK_WIDGET (self->canvas), "canvas");
    gtk_stack_add_named (self->stack, build_media_page (self), "media");
    self->text = NAUTILUS_TEXT_PREVIEW (nautilus_text_preview_new (TEXT_LIMIT));
    gtk_stack_add_named (self->stack, GTK_WIDGET (self->text), "text");
    gtk_stack_add_named (self->stack, build_info_page (self), "info");
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (self->stack));
    self->card = toolbar;
    gtk_widget_add_css_class (self->card, "quick-look-card");
    gtk_widget_set_overflow (self->card, GTK_OVERFLOW_HIDDEN);
    adw_bin_set_child (ADW_BIN (self), toolbar);

    click = gtk_gesture_click_new ();
    g_signal_connect (click, "pressed", G_CALLBACK (on_backdrop_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

    self->fade = adw_timed_animation_new (GTK_WIDGET (self), 0, 1, FADE_IN_MS,
                                          adw_callback_animation_target_new ((AdwAnimationTargetFunc) set_opacity,
                                                                             self, NULL));
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->fade), ADW_EASE_OUT_CUBIC);
    g_signal_connect_swapped (self->fade, "done", G_CALLBACK (on_fade_done), self);

    keys = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);

    g_signal_connect_object (nautilus_flier_settings (), "changed::ratings",
                             G_CALLBACK (update_rating), self, G_CONNECT_SWAPPED);
}

static void
nautilus_quick_look_dispose (GObject *object)
{
    NautilusQuickLook *self = NAUTILUS_QUICK_LOOK (object);

    stop_media (self);
    watch_files (self, FALSE);
    g_clear_object (&self->alone);
    g_clear_list (&self->files, g_object_unref);
    g_clear_weak_pointer (&self->view);
    g_clear_object (&self->fade);

    G_OBJECT_CLASS (nautilus_quick_look_parent_class)->dispose (object);
}

static void
nautilus_quick_look_class_init (NautilusQuickLookClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_quick_look_dispose;
}

/* --------------------------------------------------------------- API */

gboolean
nautilus_quick_look_enabled (void)
{
    return g_settings_get_boolean (nautilus_flier_settings (), "own-quick-look");
}

gboolean
nautilus_quick_look_is_visible (void)
{
    return quick_look != NULL && quick_look->shown;
}

gboolean
nautilus_quick_look_has_keys (GtkWindow *window)
{
    return nautilus_quick_look_is_visible () &&
           gtk_widget_get_root (GTK_WIDGET (quick_look)) == GTK_ROOT (window);
}

void
nautilus_quick_look_close (void)
{
    if (quick_look != NULL)
    {
        ql_hide (quick_look);
    }
}

void
nautilus_quick_look_attach (AdwApplicationWindow *window)
{
    GtkWidget *content = adw_application_window_get_content (window);
    GtkWidget *overlay = gtk_overlay_new ();

    g_object_ref (content);
    adw_application_window_set_content (window, overlay);
    gtk_overlay_set_child (GTK_OVERLAY (overlay), content);
    g_object_unref (content);

    g_object_set_data (G_OBJECT (window), "quick-look-overlay", overlay);
}

void
nautilus_quick_look_toggle (NautilusFilesView *view,
                            GList             *selection)
{
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (view));
    GtkWidget *overlay = root != NULL ? g_object_get_data (G_OBJECT (root), "quick-look-overlay") : NULL;

    if (nautilus_quick_look_is_visible ())
    {
        nautilus_quick_look_close ();
        return;
    }

    if (selection == NULL || overlay == NULL)
    {
        return;
    }

    if (quick_look == NULL)
    {
        quick_look = g_object_new (NAUTILUS_TYPE_QUICK_LOOK, NULL);
        g_object_add_weak_pointer (G_OBJECT (quick_look), (gpointer *) &quick_look);
    }

    /* Over the window it was opened from, going away with it. */
    if (gtk_widget_get_parent (GTK_WIDGET (quick_look)) != overlay)
    {
        if (gtk_widget_get_parent (GTK_WIDGET (quick_look)) != NULL)
        {
            g_object_ref (quick_look);
            gtk_overlay_remove_overlay (GTK_OVERLAY (gtk_widget_get_parent (GTK_WIDGET (quick_look))),
                                        GTK_WIDGET (quick_look));
            gtk_overlay_add_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (quick_look));
            g_object_unref (quick_look);
        }
        else
        {
            gtk_overlay_add_overlay (GTK_OVERLAY (overlay), GTK_WIDGET (quick_look));
        }
    }
    g_set_weak_pointer (&quick_look->view, view);

    set_files (quick_look, selection);
    ql_show (quick_look);
}

void
nautilus_quick_look_update (NautilusFilesView *view,
                            GList             *selection)
{
    if (!nautilus_quick_look_is_visible () || quick_look->view != view || selection == NULL)
    {
        return;
    }

    /* The same items again (a file of them changed): nothing to redo. */
    GList *a = selection, *b = quick_look->files;

    for (; a != NULL && b != NULL && a->data == b->data; a = a->next, b = b->next)
    {
    }
    if (a == NULL && b == NULL)
    {
        return;
    }

    set_files (quick_look, selection);
}
