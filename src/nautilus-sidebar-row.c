/*
 * Copyright (C) 2015 Carlos Soriano <csoriano@gnome.org>
 *
 * SPDX-License-Identifier: GPL-2.1-or-later
 */
#include "nautilus-sidebar-row.h"

#include "config.h"
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "nautilus-enum-types.h"
#include "nautilus-file.h"
#include "nautilus-labels.h"

/* For section and place type enums */
#include "nautilus-sidebar.h"

#ifdef HAVE_CLOUDPROVIDERS
#include <cloudproviders.h>
#endif

struct _NautilusSidebarRow
{
    GtkListBoxRow parent_instance;
    GIcon *start_icon;
    GIcon *end_icon;
    GtkWidget *start_icon_widget;
    GtkWidget *end_icon_widget;
    char *label;
    char *tooltip;
    char *eject_tooltip;
    GtkWidget *label_widget;
    gboolean ejectable;
    GtkWidget *eject_button;
    int order_index;
    NautilusSidebarSectionType section_type;
    NautilusSidebarRowType place_type;
    char *uri;
    NautilusFile *file;
    GDrive *drive;
    GVolume *volume;
    GMount *mount;
    GObject *cloud_provider_account;
    NautilusSidebar *sidebar;
    GtkWidget *revealer;
    GtkWidget *busy_spinner;
    GtkWidget *usage_bar;
    GCancellable *usage_cancellable;
    guint usage_timeout_id;
};

#define USAGE_REFRESH 30 /* s */

G_DEFINE_TYPE (NautilusSidebarRow, nautilus_sidebar_row, GTK_TYPE_LIST_BOX_ROW)

enum
{
    PROP_0,
    PROP_START_ICON,
    PROP_END_ICON,
    PROP_LABEL,
    PROP_TOOLTIP,
    PROP_EJECT_TOOLTIP,
    PROP_EJECTABLE,
    PROP_SIDEBAR,
    PROP_ORDER_INDEX,
    PROP_SECTION_TYPE,
    PROP_PLACE_TYPE,
    PROP_URI,
    PROP_NAUTILUS_FILE,
    PROP_DRIVE,
    PROP_VOLUME,
    PROP_MOUNT,
    PROP_CLOUD_PROVIDER_ACCOUNT,
    LAST_PROP
};

static GParamSpec *properties[LAST_PROP];

static void
dummy_callback (NautilusFile *file,
                gpointer      callback_data)
{
    /* No-op. Just a tag for nautilus_file_cancel_call_when_ready() to match. */
}

static void
on_file_changed (NautilusSidebarRow *self)
{
    g_return_if_fail (NAUTILUS_IS_SIDEBAR_ROW (self));
    g_return_if_fail (NAUTILUS_IS_FILE (self->file));
    if (nautilus_file_is_gone (self->file))
    {
        g_signal_handlers_disconnect_by_func (self->file, on_file_changed, self);
        nautilus_file_cancel_call_when_ready (self->file, dummy_callback, self);
        g_clear_object (&self->file);
    }
}

static void
ensure_connected_file (NautilusSidebarRow *self)
{
    if (self->file != NULL)
    {
        return;
    }

    if (self->uri != NULL)
    {
        g_autoptr (NautilusFile) file = nautilus_file_get_by_uri (self->uri);

        /* Self-owned file may be marked as gone, if we are rebuilding the sidebar
         * in response to that file being unmounted. Don't keep it alive. */
        if (file == NULL || nautilus_file_is_gone (file))
        {
            return;
        }

        self->file = g_steal_pointer (&file);
        nautilus_file_call_when_ready (self->file, NAUTILUS_FILE_ATTRIBUTE_MOUNT, dummy_callback, self);
        g_signal_connect_object (self->file, "changed",
                                 G_CALLBACK (on_file_changed), self, G_CONNECT_SWAPPED);
    }
}

#ifdef HAVE_CLOUDPROVIDERS

static void
cloud_row_update (NautilusSidebarRow *self)
{
    CloudProvidersAccount *account;
    GIcon *end_icon;
    int provider_status;

    account = CLOUD_PROVIDERS_ACCOUNT (self->cloud_provider_account);
    provider_status = cloud_providers_account_get_status (account);
    switch (provider_status)
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
            return;
        }
    }

    g_object_set (self,
                  "label", cloud_providers_account_get_name (account),
                  NULL);
    g_object_set (self,
                  "tooltip", cloud_providers_account_get_status_details (account),
                  NULL);
    g_object_set (self,
                  "end-icon", end_icon,
                  NULL);

    if (end_icon != NULL)
    {
        g_object_unref (end_icon);
    }
}

#endif

static void
nautilus_sidebar_row_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
    NautilusSidebarRow *self = NAUTILUS_SIDEBAR_ROW (object);

    switch (prop_id)
    {
        case PROP_SIDEBAR:
        {
            g_value_set_object (value, self->sidebar);
            break;
        }

        case PROP_START_ICON:
        {
            g_value_set_object (value, self->start_icon);
            break;
        }

        case PROP_END_ICON:
        {
            g_value_set_object (value, self->end_icon);
            break;
        }

        case PROP_LABEL:
        {
            g_value_set_string (value, self->label);
            break;
        }

        case PROP_TOOLTIP:
        {
            g_value_set_string (value, self->tooltip);
            break;
        }

        case PROP_EJECT_TOOLTIP:
        {
            g_value_set_string (value, self->eject_tooltip);
            break;
        }

        case PROP_EJECTABLE:
        {
            g_value_set_boolean (value, self->ejectable);
            break;
        }

        case PROP_ORDER_INDEX:
        {
            g_value_set_int (value, self->order_index);
            break;
        }

        case PROP_SECTION_TYPE:
        {
            g_value_set_enum (value, self->section_type);
            break;
        }

        case PROP_PLACE_TYPE:
        {
            g_value_set_enum (value, self->place_type);
            break;
        }

        case PROP_URI:
        {
            g_value_set_string (value, self->uri);
            break;
        }

        case PROP_NAUTILUS_FILE:
        {
            ensure_connected_file (self);
            g_value_set_object (value, self->file);
            break;
        }

        case PROP_DRIVE:
        {
            g_value_set_object (value, self->drive);
            break;
        }

        case PROP_VOLUME:
        {
            g_value_set_object (value, self->volume);
            break;
        }

        case PROP_MOUNT:
        {
            g_value_set_object (value, self->mount);
            break;
        }

        case PROP_CLOUD_PROVIDER_ACCOUNT:
        {
            g_value_set_object (value, self->cloud_provider_account);
            break;
        }

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_sidebar_row_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
    NautilusSidebarRow *self = NAUTILUS_SIDEBAR_ROW (object);

    switch (prop_id)
    {
        case PROP_SIDEBAR:
        {
            self->sidebar = g_value_get_object (value);
            break;
        }

        case PROP_START_ICON:
        {
            g_clear_object (&self->start_icon);
            object = g_value_get_object (value);
            if (object != NULL)
            {
                self->start_icon = G_ICON (g_object_ref (object));
                gtk_image_set_from_gicon (GTK_IMAGE (self->start_icon_widget), self->start_icon);
            }
            else
            {
                gtk_image_clear (GTK_IMAGE (self->start_icon_widget));
            }
            break;
        }

        case PROP_END_ICON:
        {
            g_clear_object (&self->end_icon);
            object = g_value_get_object (value);
            if (object != NULL)
            {
                self->end_icon = G_ICON (g_object_ref (object));
                gtk_image_set_from_gicon (GTK_IMAGE (self->end_icon_widget), self->end_icon);
                gtk_widget_set_visible (self->end_icon_widget, TRUE);
            }
            else
            {
                gtk_image_clear (GTK_IMAGE (self->end_icon_widget));
                gtk_widget_set_visible (self->end_icon_widget, FALSE);
            }
            break;
        }

        case PROP_LABEL:
        {
            g_free (self->label);
            self->label = g_strdup (g_value_get_string (value));
            gtk_label_set_text (GTK_LABEL (self->label_widget), self->label);
            break;
        }

        case PROP_TOOLTIP:
        {
            g_free (self->tooltip);
            self->tooltip = g_strdup (g_value_get_string (value));
            gtk_widget_set_tooltip_text (GTK_WIDGET (self), self->tooltip);
            break;
        }

        case PROP_EJECT_TOOLTIP:
        {
            g_free (self->eject_tooltip);
            self->eject_tooltip = g_strdup (g_value_get_string (value));
            gtk_widget_set_tooltip_text (GTK_WIDGET (self->eject_button), self->eject_tooltip);
            break;
        }

        case PROP_EJECTABLE:
        {
            self->ejectable = g_value_get_boolean (value);
            if (self->ejectable)
            {
                gtk_widget_set_visible (self->eject_button, TRUE);
            }
            else
            {
                gtk_widget_set_visible (self->eject_button, FALSE);
            }
            break;
        }

        case PROP_ORDER_INDEX:
        {
            self->order_index = g_value_get_int (value);
            break;
        }

        case PROP_SECTION_TYPE:
        {
            self->section_type = g_value_get_enum (value);
            if (self->section_type == NAUTILUS_SIDEBAR_SECTION_DEFAULT_LOCATIONS)
            {
                gtk_label_set_ellipsize (GTK_LABEL (self->label_widget), PANGO_ELLIPSIZE_NONE);
            }
            else
            {
                gtk_label_set_ellipsize (GTK_LABEL (self->label_widget), PANGO_ELLIPSIZE_MIDDLE);
            }
            break;
        }

        case PROP_PLACE_TYPE:
        {
            self->place_type = g_value_get_enum (value);
            break;
        }

        case PROP_URI:
        {
            g_free (self->uri);
            self->uri = g_strdup (g_value_get_string (value));
            ensure_connected_file (self);
            break;
        }

        case PROP_DRIVE:
        {
            g_set_object (&self->drive, g_value_get_object (value));
            break;
        }

        case PROP_VOLUME:
        {
            g_set_object (&self->volume, g_value_get_object (value));
            break;
        }

        case PROP_MOUNT:
        {
            g_set_object (&self->mount, g_value_get_object (value));
            break;
        }

        case PROP_CLOUD_PROVIDER_ACCOUNT:
        {
#ifdef HAVE_CLOUDPROVIDERS
            if (self->cloud_provider_account != NULL)
            {
                g_signal_handlers_disconnect_by_data (self->cloud_provider_account, self);
            }

            self->cloud_provider_account = g_value_dup_object (value);

            if (self->cloud_provider_account != NULL)
            {
                g_signal_connect_swapped (self->cloud_provider_account, "notify::name",
                                          G_CALLBACK (cloud_row_update), self);
                g_signal_connect_swapped (self->cloud_provider_account, "notify::status",
                                          G_CALLBACK (cloud_row_update), self);
                g_signal_connect_swapped (self->cloud_provider_account, "notify::status-details",
                                          G_CALLBACK (cloud_row_update), self);
            }
#endif
            break;
        }

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
on_child_revealed (GObject    *self,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
    /* We need to hide the actual widget because if not the GtkListBoxRow will
     * still allocate the paddings, even if the revealer is not revealed, and
     * therefore the row will be still somewhat visible. */
    if (!gtk_revealer_get_reveal_child (GTK_REVEALER (self)))
    {
        gtk_widget_set_visible (GTK_WIDGET (NAUTILUS_SIDEBAR_ROW (user_data)), FALSE);
    }
}

void
nautilus_sidebar_row_reveal (NautilusSidebarRow *self)
{
    gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), TRUE);
}

void
nautilus_sidebar_row_hide (NautilusSidebarRow *self,
                           gboolean            immediate)
{
    guint transition_duration;

    transition_duration = gtk_revealer_get_transition_duration (GTK_REVEALER (self->revealer));
    if (immediate)
    {
        gtk_revealer_set_transition_duration (GTK_REVEALER (self->revealer), 0);
    }

    gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), FALSE);

    gtk_revealer_set_transition_duration (GTK_REVEALER (self->revealer), transition_duration);
}

void
nautilus_sidebar_row_set_start_icon (NautilusSidebarRow *self,
                                     GIcon              *icon)
{
    g_return_if_fail (NAUTILUS_IS_SIDEBAR_ROW (self));

    if (self->start_icon != icon)
    {
        g_set_object (&self->start_icon, icon);
        if (self->start_icon != NULL)
        {
            gtk_image_set_from_gicon (GTK_IMAGE (self->start_icon_widget), self->start_icon);
        }
        else
        {
            gtk_image_clear (GTK_IMAGE (self->start_icon_widget));
        }

        g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_START_ICON]);
    }
}

void
nautilus_sidebar_row_set_end_icon (NautilusSidebarRow *self,
                                   GIcon              *icon)
{
    g_return_if_fail (NAUTILUS_IS_SIDEBAR_ROW (self));

    if (self->end_icon != icon)
    {
        g_set_object (&self->end_icon, icon);
        if (self->end_icon != NULL)
        {
            gtk_image_set_from_gicon (GTK_IMAGE (self->end_icon_widget), self->end_icon);
        }
        else
        if (self->end_icon_widget != NULL)
        {
            gtk_image_clear (GTK_IMAGE (self->end_icon_widget));
        }

        g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_END_ICON]);
    }
}

static void
nautilus_sidebar_row_dispose (GObject *object)
{
    NautilusSidebarRow *self = NAUTILUS_SIDEBAR_ROW (object);

    g_clear_handle_id (&self->usage_timeout_id, g_source_remove);
    g_cancellable_cancel (self->usage_cancellable);
    g_clear_object (&self->usage_cancellable);

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (self), NULL);
    gtk_widget_dispose_template (GTK_WIDGET (self), NAUTILUS_TYPE_SIDEBAR_ROW);

    if (self->file != NULL)
    {
        g_signal_handlers_disconnect_by_func (self->file, on_file_changed, self);
        nautilus_file_cancel_call_when_ready (self->file, dummy_callback, self);
        g_clear_object (&self->file);
    }

    G_OBJECT_CLASS (nautilus_sidebar_row_parent_class)->dispose (object);
}

/* How full a drive in the Storage section is, as a thin bar under its name. */
static void
on_usage_queried (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GFileInfo) info = g_file_query_filesystem_info_finish (G_FILE (source), result, &error);
    NautilusSidebarRow *self;
    guint64 size, free_space;

    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        return;
    }
    self = NAUTILUS_SIDEBAR_ROW (user_data);
    g_clear_object (&self->usage_cancellable);

    size = info != NULL ? g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) : 0;
    free_space = info != NULL ? g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE) : 0;
    if (size == 0 || free_space > size)
    {
        gtk_widget_set_visible (self->usage_bar, FALSE);
        return;
    }

    g_autofree char *free_text = g_format_size (free_space);
    g_autofree char *size_text = g_format_size (size);
    /* Translators: free space on a drive, e.g. "120 GB free of 500 GB" */
    g_autofree char *tooltip = g_strdup_printf (_("%s free of %s"), free_text, size_text);

    gtk_level_bar_set_value (GTK_LEVEL_BAR (self->usage_bar), 1.0 - (double) free_space / size);
    gtk_widget_set_tooltip_text (self->usage_bar, tooltip);
    if (free_space < size / 10)
    {
        gtk_widget_add_css_class (self->usage_bar, "almost-full");
    }
    else
    {
        gtk_widget_remove_css_class (self->usage_bar, "almost-full");
    }
    gtk_widget_set_visible (self->usage_bar, TRUE);
}

static gboolean
usage_wanted (NautilusSidebarRow *self)
{
    g_autoptr (GFile) location = NULL;

    if (self->section_type != NAUTILUS_SIDEBAR_SECTION_MOUNTS || self->uri == NULL ||
        !g_settings_get_boolean (nautilus_flier_settings (), "sidebar-storage-usage"))
    {
        return FALSE;
    }
    /* Network shares can take long to answer; local drives only. */
    location = g_file_new_for_uri (self->uri);
    return g_file_is_native (location);
}

static void
usage_update (NautilusSidebarRow *self)
{
    if (!usage_wanted (self))
    {
        gtk_widget_set_visible (self->usage_bar, FALSE);
        return;
    }
    if (self->usage_cancellable != NULL)
    {
        return;
    }

    g_autoptr (GFile) location = g_file_new_for_uri (self->uri);
    self->usage_cancellable = g_cancellable_new ();
    g_file_query_filesystem_info_async (location,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                        G_PRIORITY_LOW, self->usage_cancellable,
                                        on_usage_queried, self);
}

static gboolean
on_usage_timeout (gpointer data)
{
    usage_update (NAUTILUS_SIDEBAR_ROW (data));
    return G_SOURCE_CONTINUE;
}

static void
on_usage_map (NautilusSidebarRow *self)
{
    usage_update (self);
    if (self->usage_timeout_id == 0 && usage_wanted (self))
    {
        self->usage_timeout_id = g_timeout_add_seconds (USAGE_REFRESH, on_usage_timeout, self);
    }
}

static void
on_usage_unmap (NautilusSidebarRow *self)
{
    g_clear_handle_id (&self->usage_timeout_id, g_source_remove);
}

static void
on_usage_setting_changed (NautilusSidebarRow *self)
{
    on_usage_unmap (self);
    if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
        on_usage_map (self);
    }
    else
    {
        usage_update (self);
    }
}

static void
nautilus_sidebar_row_finalize (GObject *object)
{
    NautilusSidebarRow *self = NAUTILUS_SIDEBAR_ROW (object);

    g_clear_object (&self->start_icon);
    g_clear_object (&self->end_icon);
    g_free (self->label);
    self->label = NULL;
    g_free (self->tooltip);
    self->tooltip = NULL;
    g_free (self->eject_tooltip);
    self->eject_tooltip = NULL;
    g_free (self->uri);
    self->uri = NULL;
    g_clear_object (&self->drive);
    g_clear_object (&self->volume);
    g_clear_object (&self->mount);
#ifdef HAVE_CLOUDPROVIDERS
    if (self->cloud_provider_account != NULL)
    {
        g_signal_handlers_disconnect_by_data (self->cloud_provider_account, self);
    }
    g_clear_object (&self->cloud_provider_account);
#endif

    G_OBJECT_CLASS (nautilus_sidebar_row_parent_class)->finalize (object);
}

static void
nautilus_sidebar_row_init (NautilusSidebarRow *self)
{
    gtk_widget_init_template (GTK_WIDGET (self));

    gtk_widget_set_focus_on_click (GTK_WIDGET (self), FALSE);

    self->file = NULL;

    /* No colours by level: the bar is one quiet tone, red when almost full. */
    gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (self->usage_bar), GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (self->usage_bar), GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (self->usage_bar), GTK_LEVEL_BAR_OFFSET_FULL);
    g_signal_connect (self, "map", G_CALLBACK (on_usage_map), NULL);
    g_signal_connect (self, "unmap", G_CALLBACK (on_usage_unmap), NULL);
    g_signal_connect_object (nautilus_flier_settings (), "changed::sidebar-storage-usage",
                             G_CALLBACK (on_usage_setting_changed), self, G_CONNECT_SWAPPED);
}

static void
nautilus_sidebar_row_class_init (NautilusSidebarRowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->get_property = nautilus_sidebar_row_get_property;
    object_class->set_property = nautilus_sidebar_row_set_property;
    object_class->dispose = nautilus_sidebar_row_dispose;
    object_class->finalize = nautilus_sidebar_row_finalize;

    properties [PROP_SIDEBAR] =
        g_param_spec_object ("sidebar",
                             "Sidebar",
                             "Sidebar",
                             NAUTILUS_TYPE_PLACES_SIDEBAR,
                             (G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_START_ICON] =
        g_param_spec_object ("start-icon",
                             "start-icon",
                             "The start icon.",
                             G_TYPE_ICON,
                             (G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_END_ICON] =
        g_param_spec_object ("end-icon",
                             "end-icon",
                             "The end icon.",
                             G_TYPE_ICON,
                             (G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_LABEL] =
        g_param_spec_string ("label",
                             "label",
                             "The label text.",
                             NULL,
                             (G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_TOOLTIP] =
        g_param_spec_string ("tooltip",
                             "Tooltip",
                             "Tooltip",
                             NULL,
                             (G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_EJECT_TOOLTIP] =
        g_param_spec_string ("eject-tooltip",
                             "Eject Tooltip",
                             "Eject Tooltip",
                             NULL,
                             (G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_EJECTABLE] =
        g_param_spec_boolean ("ejectable",
                              "Ejectable",
                              "Ejectable",
                              FALSE,
                              (G_PARAM_READWRITE |
                               G_PARAM_STATIC_STRINGS));

    properties [PROP_ORDER_INDEX] =
        g_param_spec_int ("order-index",
                          "OrderIndex",
                          "Order Index",
                          0, G_MAXINT, 0,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));

    properties [PROP_SECTION_TYPE] =
        g_param_spec_enum ("section-type",
                           "section type",
                           "The section type.",
                           NAUTILUS_TYPE_SIDEBAR_SECTION_TYPE,
                           NAUTILUS_SIDEBAR_SECTION_INVALID,
                           (G_PARAM_READWRITE |
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_CONSTRUCT_ONLY));

    properties [PROP_PLACE_TYPE] =
        g_param_spec_enum ("place-type",
                           "place type",
                           "The place type.",
                           NAUTILUS_TYPE_SIDEBAR_ROW_TYPE,
                           NAUTILUS_SIDEBAR_ROW_INVALID,
                           (G_PARAM_READWRITE |
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_CONSTRUCT_ONLY));

    properties [PROP_URI] =
        g_param_spec_string ("uri",
                             "Uri",
                             "Uri",
                             NULL,
                             (G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_NAUTILUS_FILE] =
        g_param_spec_object ("file",
                             "File",
                             "Nautilus File",
                             NAUTILUS_TYPE_FILE,
                             (G_PARAM_READABLE |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_DRIVE] =
        g_param_spec_object ("drive",
                             "Drive",
                             "Drive",
                             G_TYPE_DRIVE,
                             (G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_VOLUME] =
        g_param_spec_object ("volume",
                             "Volume",
                             "Volume",
                             G_TYPE_VOLUME,
                             (G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_MOUNT] =
        g_param_spec_object ("mount",
                             "Mount",
                             "Mount",
                             G_TYPE_MOUNT,
                             (G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));

    properties [PROP_CLOUD_PROVIDER_ACCOUNT] =
        g_param_spec_object ("cloud-provider-account",
                             "CloudProvidersAccount",
                             "CloudProvidersAccount",
                             G_TYPE_OBJECT,
                             (G_PARAM_READWRITE |
                              G_PARAM_STATIC_STRINGS));

    g_object_class_install_properties (object_class, LAST_PROP, properties);

    gtk_widget_class_set_template_from_resource (widget_class,
                                                 "/org/gnome/nautilus/ui/nautilus-sidebar-row.ui");

    gtk_widget_class_bind_template_child (widget_class, NautilusSidebarRow, start_icon_widget);
    gtk_widget_class_bind_template_child (widget_class, NautilusSidebarRow, end_icon_widget);
    gtk_widget_class_bind_template_child (widget_class, NautilusSidebarRow, label_widget);
    gtk_widget_class_bind_template_child (widget_class, NautilusSidebarRow, eject_button);
    gtk_widget_class_bind_template_child (widget_class, NautilusSidebarRow, revealer);
    gtk_widget_class_bind_template_child (widget_class, NautilusSidebarRow, busy_spinner);
    gtk_widget_class_bind_template_child (widget_class, NautilusSidebarRow, usage_bar);

    gtk_widget_class_bind_template_callback (widget_class, on_child_revealed);
    gtk_widget_class_set_css_name (widget_class, "row");
}

NautilusSidebarRow *
nautilus_sidebar_row_new_placeholder (void)
{
    NautilusSidebarRow *row = g_object_new (NAUTILUS_TYPE_SIDEBAR_ROW, NULL);
    g_object_ref_sink (row);

    row->section_type = NAUTILUS_SIDEBAR_SECTION_BOOKMARKS;
    row->place_type = NAUTILUS_SIDEBAR_ROW_BOOKMARK_PLACEHOLDER;
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), NULL);

    gtk_widget_add_css_class (GTK_WIDGET (row), "sidebar-placeholder-row");

    return row;
}

NautilusSidebarRow *
nautilus_sidebar_row_clone (NautilusSidebarRow *self)
{
    return g_object_new (NAUTILUS_TYPE_SIDEBAR_ROW,
                         "sidebar", self->sidebar,
                         "start-icon", self->start_icon,
                         "end-icon", self->end_icon,
                         "label", self->label,
                         "tooltip", self->tooltip,
                         "eject-tooltip", self->eject_tooltip,
                         "ejectable", self->ejectable,
                         "order-index", self->order_index,
                         "section-type", self->section_type,
                         "place-type", self->place_type,
                         "uri", self->uri,
                         "drive", self->drive,
                         "volume", self->volume,
                         "mount", self->mount,
                         "cloud-provider-account", self->cloud_provider_account,
                         NULL);
}

GtkWidget *
nautilus_sidebar_row_get_eject_button (NautilusSidebarRow *self)
{
    return self->eject_button;
}

void
nautilus_sidebar_row_set_busy (NautilusSidebarRow *row,
                               gboolean            is_busy)
{
    g_return_if_fail (NAUTILUS_IS_SIDEBAR_ROW (row));

    gtk_widget_set_visible (row->busy_spinner, is_busy);
}
