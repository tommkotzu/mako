/*
 * nautilus-progress-popup.c: running file operations, in a card in the corner
 *
 * The card shows by itself once an operation has run for a moment, lists every
 * operation in the order it started (one copied while another runs is added
 * below it), and goes away a little after the last one is done.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-progress-popup.h"

#include <glib/gi18n.h>

#include "nautilus-labels.h"
#include "nautilus-progress-info.h"
#include "nautilus-progress-info-manager.h"
#include "nautilus-progress-info-widget.h"

#define SHOW_AFTER 0.5      /* s an operation runs before the card shows */
#define HIDE_AFTER 2500     /* ms the card stays after everything is done */
#define TICK 250            /* ms */

struct _NautilusProgressPopup
{
    AdwBin parent_instance;

    GtkWidget *title;
    GtkWidget *collapse_button;
    GtkWidget *overall_bar;
    GtkWidget *list_revealer;
    GtkWidget *list;

    GListStore *model;
    NautilusProgressInfoManager *manager;
    guint tick_id;
    gint64 all_done_since;
    gboolean dismissed;
};

G_DEFINE_FINAL_TYPE (NautilusProgressPopup, nautilus_progress_popup, ADW_TYPE_BIN)

static gboolean
is_over (NautilusProgressInfo *info)
{
    return nautilus_progress_info_get_is_finished (info) ||
           nautilus_progress_info_get_is_cancelled (info);
}

static gboolean
in_model (NautilusProgressPopup *self,
          NautilusProgressInfo  *info)
{
    return g_list_store_find (self->model, info, NULL);
}

static void
stop_tick (NautilusProgressPopup *self)
{
    g_clear_handle_id (&self->tick_id, g_source_remove);
}

static void
hide_card (NautilusProgressPopup *self)
{
    gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
    g_list_store_remove_all (self->model);
    self->all_done_since = 0;
}

static gboolean
tick (gpointer data)
{
    NautilusProgressPopup *self = data;
    GList *infos = nautilus_progress_info_manager_get_all_infos (self->manager);
    guint running = 0;
    double progress = 0;

    if (!g_settings_get_boolean (nautilus_flier_settings (), "progress-popup"))
    {
        hide_card (self);
        self->tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    /* Rows in the order operations started; new ones go below. */
    for (GList *l = infos; l != NULL; l = l->next)
    {
        NautilusProgressInfo *info = l->data;

        if (!in_model (self, info) && !is_over (info) &&
            nautilus_progress_info_get_total_elapsed_time (info) >= SHOW_AFTER)
        {
            g_list_store_append (self->model, info);
        }
    }

    /* Rows the manager dropped (cleared after finishing) go too. */
    for (guint i = g_list_model_get_n_items (G_LIST_MODEL (self->model)); i > 0; i--)
    {
        g_autoptr (NautilusProgressInfo) info = g_list_model_get_item (G_LIST_MODEL (self->model), i - 1);

        if (g_list_find (infos, info) == NULL)
        {
            g_list_store_remove (self->model, i - 1);
        }
    }

    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->model));
    for (guint i = 0; i < n; i++)
    {
        g_autoptr (NautilusProgressInfo) info = g_list_model_get_item (G_LIST_MODEL (self->model), i);

        if (!is_over (info))
        {
            double p = nautilus_progress_info_get_progress (info);

            running++;
            progress += MAX (p, 0);
        }
        else
        {
            progress += 1;
        }
    }

    if (n == 0)
    {
        gboolean any_running = FALSE;

        for (GList *l = infos; l != NULL; l = l->next)
        {
            any_running |= !is_over (l->data);
        }
        if (!any_running)
        {
            hide_card (self);
            self->tick_id = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    if (running > 0)
    {
        g_autofree char *title = g_strdup_printf (ngettext ("%u operation", "%u operations", running), running);

        gtk_label_set_text (GTK_LABEL (self->title), title);
        self->all_done_since = 0;
    }
    else
    {
        gtk_label_set_text (GTK_LABEL (self->title), _("All done"));
        if (self->all_done_since == 0)
        {
            self->all_done_since = g_get_monotonic_time ();
        }
        else if (g_get_monotonic_time () - self->all_done_since > HIDE_AFTER * G_TIME_SPAN_MILLISECOND)
        {
            hide_card (self);
            self->dismissed = FALSE;
            self->tick_id = 0;
            return G_SOURCE_REMOVE;
        }
    }
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (self->overall_bar), progress / n);

    if (!self->dismissed)
    {
        gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
    }

    return G_SOURCE_CONTINUE;
}

static void
start_tick (NautilusProgressPopup *self)
{
    if (self->tick_id == 0)
    {
        self->tick_id = g_timeout_add (TICK, tick, self);
    }
}

static void
on_new_progress_info (NautilusProgressPopup *self)
{
    /* A new operation brings a closed card back. */
    self->dismissed = FALSE;
    self->all_done_since = 0;
    start_tick (self);
}

static void
on_close_clicked (NautilusProgressPopup *self)
{
    self->dismissed = TRUE;
    gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
}

static void
on_collapse_clicked (NautilusProgressPopup *self)
{
    gboolean open = !gtk_revealer_get_reveal_child (GTK_REVEALER (self->list_revealer));

    gtk_revealer_set_reveal_child (GTK_REVEALER (self->list_revealer), open);
    gtk_button_set_icon_name (GTK_BUTTON (self->collapse_button), open ? "pan-down-symbolic" : "pan-up-symbolic");
    gtk_widget_set_tooltip_text (self->collapse_button, open ? _("Hide Details") : _("Show Details"));
}

static GtkWidget *
create_row (gpointer  item,
            gpointer  user_data)
{
    return nautilus_progress_info_widget_new (NAUTILUS_PROGRESS_INFO (item));
}

static void
nautilus_progress_popup_dispose (GObject *object)
{
    NautilusProgressPopup *self = NAUTILUS_PROGRESS_POPUP (object);

    stop_tick (self);
    gtk_widget_dispose_template (GTK_WIDGET (self), NAUTILUS_TYPE_PROGRESS_POPUP);
    g_clear_object (&self->model);
    g_clear_object (&self->manager);

    G_OBJECT_CLASS (nautilus_progress_popup_parent_class)->dispose (object);
}

static void
nautilus_progress_popup_class_init (NautilusProgressPopupClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->dispose = nautilus_progress_popup_dispose;

    gtk_widget_class_set_template_from_resource (widget_class,
                                                 "/org/gnome/nautilus/ui/nautilus-progress-popup.ui");
    gtk_widget_class_bind_template_child (widget_class, NautilusProgressPopup, title);
    gtk_widget_class_bind_template_child (widget_class, NautilusProgressPopup, collapse_button);
    gtk_widget_class_bind_template_child (widget_class, NautilusProgressPopup, overall_bar);
    gtk_widget_class_bind_template_child (widget_class, NautilusProgressPopup, list_revealer);
    gtk_widget_class_bind_template_child (widget_class, NautilusProgressPopup, list);
    gtk_widget_class_bind_template_callback (widget_class, on_close_clicked);
    gtk_widget_class_bind_template_callback (widget_class, on_collapse_clicked);
}

static void
nautilus_progress_popup_init (NautilusProgressPopup *self)
{
    gtk_widget_init_template (GTK_WIDGET (self));

    self->model = g_list_store_new (NAUTILUS_TYPE_PROGRESS_INFO);
    gtk_list_box_bind_model (GTK_LIST_BOX (self->list), G_LIST_MODEL (self->model),
                             create_row, NULL, NULL);

    self->manager = nautilus_progress_info_manager_dup_singleton ();
    g_signal_connect_object (self->manager, "new-progress-info",
                             G_CALLBACK (on_new_progress_info), self, G_CONNECT_SWAPPED);

    /* Operations already running when this window opened. */
    if (!nautilus_progress_manager_are_all_infos_finished_or_cancelled (self->manager))
    {
        start_tick (self);
    }
}
