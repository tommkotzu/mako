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
#include <math.h>

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

/* Shared by every window's card and every bottom bar's small bar: whether
 * the card is minimized, and how far the operations are. */
typedef struct
{
    GObject *notifier;  /* "changed" is emitted on it */
    gboolean minimized;
    gboolean active;
    double fraction;
    char *tooltip;
} Dock;

static Dock *
dock_get (void)
{
    static Dock dock;

    if (dock.notifier == NULL)
    {
        dock.notifier = g_object_new (G_TYPE_OBJECT, NULL);
        g_signal_new ("mako-dock-changed", G_TYPE_OBJECT, G_SIGNAL_RUN_LAST, 0,
                      NULL, NULL, NULL, G_TYPE_NONE, 0);
    }
    return &dock;
}

static void
dock_changed (void)
{
    g_signal_emit_by_name (dock_get ()->notifier, "mako-dock-changed");
}

static void
dock_set (gboolean    active,
          double      fraction,
          const char *tooltip)
{
    Dock *dock = dock_get ();

    if (dock->active == active && fabs (dock->fraction - fraction) < 0.001 &&
        g_strcmp0 (dock->tooltip, tooltip) == 0)
    {
        return;
    }
    dock->active = active;
    dock->fraction = fraction;
    g_free (dock->tooltip);
    dock->tooltip = g_strdup (tooltip);
    dock_changed ();
}

static void update_log (GtkWidget *row_box);

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
    /* Minimizing lasts until the operations are over: the next copy
     * shows the card again, instead of only the small bottom bar. */
    dock_get ()->minimized = FALSE;
    dock_set (FALSE, 0, NULL);
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

    for (guint i = 0; i < n; i++)
    {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->list), i);

        if (row != NULL)
        {
            update_log (gtk_list_box_row_get_child (row));
        }
    }

    {
        g_autofree char *tip = running > 0
                               ? g_strdup_printf (ngettext ("%u operation, %d %% — click to show",
                                                            "%u operations, %d %% — click to show", running),
                                                  running, (int) round (100 * progress / n))
                               : g_strdup (_("All done"));

        dock_set (running > 0, progress / n, tip);
    }

    gtk_widget_set_visible (GTK_WIDGET (self), !self->dismissed && !dock_get ()->minimized);

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
on_minimize_clicked (NautilusProgressPopup *self)
{
    dock_get ()->minimized = TRUE;
    dock_changed ();
}

/* Minimized or brought back, from this window or another. */
static void
on_dock_changed (NautilusProgressPopup *self)
{
    if (self->tick_id != 0 && g_list_model_get_n_items (G_LIST_MODEL (self->model)) > 0)
    {
        gtk_widget_set_visible (GTK_WIDGET (self), !self->dismissed && !dock_get ()->minimized);
    }
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

/* After the next layout, scroll the log to its newest line. */
static gboolean
scroll_log_to_end (GtkWidget     *scrolled,
                   GdkFrameClock *clock,
                   gpointer       data)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scrolled));

    gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj));
    g_object_set_data (G_OBJECT (scrolled), "stick", NULL);

    return G_SOURCE_REMOVE;
}

static void
stick_log (GtkWidget *scrolled)
{
    if (g_object_get_data (G_OBJECT (scrolled), "stick") == NULL)
    {
        g_object_set_data (G_OBJECT (scrolled), "stick", GINT_TO_POINTER (TRUE));
        gtk_widget_add_tick_callback (scrolled, scroll_log_to_end, NULL, NULL);
    }
}

/* Each row: the operation, and under a Details dropdown its log, small and
 * monospaced like a terminal, with the live progress as the last line. */
static void
update_log (GtkWidget *row_box)
{
    NautilusProgressInfo *info = g_object_get_data (G_OBJECT (row_box), "info");
    GtkWidget *revealer = g_object_get_data (G_OBJECT (row_box), "revealer");
    GtkWidget *label = g_object_get_data (G_OBJECT (row_box), "log");
    GtkWidget *scrolled = g_object_get_data (G_OBJECT (row_box), "scrolled");
    g_autofree char *log = NULL;
    g_autofree char *details = NULL;
    g_autofree char *current = NULL;
    g_autofree char *text = NULL;
    GtkAdjustment *adj;
    gboolean at_end;

    if (!gtk_revealer_get_reveal_child (GTK_REVEALER (revealer)))
    {
        return;
    }

    log = nautilus_progress_info_dup_log (info);
    details = nautilus_progress_info_get_details (info);
    current = nautilus_progress_info_dup_log_current (info);
    if (nautilus_progress_info_get_is_finished (info) || nautilus_progress_info_get_is_cancelled (info))
    {
        text = g_strdup (log);
    }
    else
    {
        text = g_strdup_printf ("%s%s> %s%s%s", log, *log != '\0' ? "\n" : "",
                                current != NULL ? current : "", current != NULL ? "  " : "",
                                details != NULL ? details : "");
    }
    if (g_strcmp0 (gtk_label_get_text (GTK_LABEL (label)), text) == 0)
    {
        return;
    }

    /* Follow the newest line, unless scrolled up to read. */
    adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scrolled));
    at_end = gtk_adjustment_get_value (adj) + gtk_adjustment_get_page_size (adj) >= gtk_adjustment_get_upper (adj) - 2;
    gtk_label_set_text (GTK_LABEL (label), text);
    if (at_end || g_object_get_data (G_OBJECT (row_box), "fresh"))
    {
        g_object_set_data (G_OBJECT (row_box), "fresh", NULL);
        stick_log (scrolled);
    }
}

static void
on_details_toggled (GtkToggleButton *button,
                    GtkWidget       *row_box)
{
    GtkWidget *revealer = g_object_get_data (G_OBJECT (row_box), "revealer");
    gboolean open = gtk_toggle_button_get_active (button);

    gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), open);
    gtk_image_set_from_icon_name (GTK_IMAGE (g_object_get_data (G_OBJECT (row_box), "toggle-icon")),
                                  open ? "pan-down-symbolic" : "pan-end-symbolic");
    g_object_set_data (G_OBJECT (row_box), "fresh", GINT_TO_POINTER (TRUE));
    gtk_label_set_text (GTK_LABEL (g_object_get_data (G_OBJECT (row_box), "log")), "");
    update_log (row_box);
}

static GtkWidget *
create_row (gpointer  item,
            gpointer  user_data)
{
    NautilusProgressInfo *info = NAUTILUS_PROGRESS_INFO (item);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *toggle = gtk_toggle_button_new ();
    GtkWidget *toggle_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *revealer = gtk_revealer_new ();
    GtkWidget *scrolled = gtk_scrolled_window_new ();
    GtkWidget *label = gtk_label_new (NULL);

    gtk_box_append (GTK_BOX (box), nautilus_progress_info_widget_new (info));

    gtk_box_append (GTK_BOX (toggle_content), gtk_image_new_from_icon_name ("pan-end-symbolic"));
    gtk_box_append (GTK_BOX (toggle_content), gtk_label_new (_("Details")));
    gtk_button_set_child (GTK_BUTTON (toggle), toggle_content);
    gtk_widget_set_halign (toggle, GTK_ALIGN_START);
    gtk_widget_add_css_class (toggle, "flat");
    gtk_widget_add_css_class (toggle, "copy-log-toggle");
    gtk_box_append (GTK_BOX (box), toggle);

    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_label_set_yalign (GTK_LABEL (label), 0);
    gtk_label_set_selectable (GTK_LABEL (label), TRUE);
    /* A path longer than the card still shows whole, on a second line. */
    gtk_label_set_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_CHAR);
    gtk_widget_set_focusable (label, FALSE);
    gtk_widget_add_css_class (label, "copy-log");
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), label);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), 200);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_widget_add_css_class (scrolled, "copy-log-frame");
    gtk_revealer_set_child (GTK_REVEALER (revealer), scrolled);
    gtk_revealer_set_transition_type (GTK_REVEALER (revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_box_append (GTK_BOX (box), revealer);

    g_object_set_data_full (G_OBJECT (box), "info", g_object_ref (info), g_object_unref);
    g_object_set_data (G_OBJECT (box), "revealer", revealer);
    g_object_set_data (G_OBJECT (box), "scrolled", scrolled);
    g_object_set_data (G_OBJECT (box), "log", label);
    g_object_set_data (G_OBJECT (box), "toggle-icon", gtk_widget_get_first_child (toggle_content));
    g_signal_connect (toggle, "toggled", G_CALLBACK (on_details_toggled), box);

    return box;
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
    gtk_widget_class_bind_template_callback (widget_class, on_minimize_clicked);
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

    g_signal_connect_object (dock_get ()->notifier, "mako-dock-changed",
                             G_CALLBACK (on_dock_changed), self, G_CONNECT_SWAPPED);

    /* Operations already running when this window opened. */
    if (!nautilus_progress_manager_are_all_infos_finished_or_cancelled (self->manager))
    {
        start_tick (self);
    }
}

/* ------------------------------------------------- The minimized card */

static void
mini_sync (GtkWidget *button)
{
    Dock *dock = dock_get ();
    GtkWidget *bar = gtk_button_get_child (GTK_BUTTON (button));

    gtk_widget_set_visible (button, dock->minimized && dock->active);
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), dock->fraction);
    gtk_widget_set_tooltip_text (button, dock->tooltip);
}

static void
on_mini_clicked (GtkWidget *button)
{
    dock_get ()->minimized = FALSE;
    dock_changed ();
}

GtkWidget *
nautilus_progress_mini_new (void)
{
    GtkWidget *button = gtk_button_new ();
    GtkWidget *bar = gtk_progress_bar_new ();

    gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);
    gtk_button_set_child (GTK_BUTTON (button), bar);
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_add_css_class (button, "progress-mini");
    g_signal_connect (button, "clicked", G_CALLBACK (on_mini_clicked), NULL);
    g_signal_connect_object (dock_get ()->notifier, "mako-dock-changed",
                             G_CALLBACK (mini_sync), button, G_CONNECT_SWAPPED);
    mini_sync (button);

    return button;
}
