/*
 * nautilus-tile-layout.c: tiles of tabs, split side by side
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-tile-layout.h"

/* Share of a tile's width/height near each edge that splits instead of joins. */
#define EDGE_FRACTION 0.3

/* ---------------------------------------------------------------- Tile */

struct _NautilusTile
{
    GtkBox parent_instance;

    AdwTabBar *bar;
    AdwTabView *view;
};

G_DEFINE_FINAL_TYPE (NautilusTile, nautilus_tile, GTK_TYPE_BOX)

static void
nautilus_tile_class_init (NautilusTileClass *klass)
{
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "nautilus-tile");
}

static void
nautilus_tile_init (NautilusTile *self)
{
    gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
    /* Its contents follow its rounded corners, with framed panels. */
    gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);

    self->view = ADW_TAB_VIEW (adw_tab_view_new ());
    self->bar = ADW_TAB_BAR (adw_tab_bar_new ());
    adw_tab_bar_set_view (self->bar, self->view);
    /* Tabs at their own width (libadwaita caps it), from the left, rather
     * than stretched over the whole bar. */
    adw_tab_bar_set_expand_tabs (self->bar, FALSE);
    gtk_widget_set_vexpand (GTK_WIDGET (self->view), TRUE);

    gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->bar));
    gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->view));
}

NautilusTile *
nautilus_tile_new (GMenuModel *menu_model)
{
    NautilusTile *self = g_object_new (NAUTILUS_TYPE_TILE, NULL);

    adw_tab_view_set_menu_model (self->view, menu_model);

    return self;
}

AdwTabView *
nautilus_tile_get_view (NautilusTile *self)
{
    g_return_val_if_fail (NAUTILUS_IS_TILE (self), NULL);

    return self->view;
}

AdwTabBar *
nautilus_tile_get_bar (NautilusTile *self)
{
    g_return_val_if_fail (NAUTILUS_IS_TILE (self), NULL);

    return self->bar;
}

NautilusTile *
nautilus_tile_for_widget (GtkWidget *widget)
{
    GtkWidget *ancestor = gtk_widget_get_ancestor (widget, NAUTILUS_TYPE_TILE);

    return ancestor != NULL ? NAUTILUS_TILE (ancestor) : NULL;
}

/* -------------------------------------------------------------- Layout */

struct _NautilusTileLayout
{
    AdwBin parent_instance;

    GList *tiles;

    gboolean has_drop_zone;
    graphene_rect_t drop_zone;
};

G_DEFINE_FINAL_TYPE (NautilusTileLayout, nautilus_tile_layout, ADW_TYPE_BIN)

static void
update_classes (NautilusTileLayout *self)
{
    gboolean split = self->tiles != NULL && self->tiles->next != NULL;

    if (split)
    {
        gtk_widget_add_css_class (GTK_WIDGET (self), "split");
    }
    else
    {
        gtk_widget_remove_css_class (GTK_WIDGET (self), "split");
    }

    /* With one tile, the tab bar hides itself for a single tab, like stock
     * Nautilus. With several, every tile keeps its bar as a handle. */
    for (GList *l = self->tiles; l != NULL; l = l->next)
    {
        adw_tab_bar_set_autohide (nautilus_tile_get_bar (l->data), !split);
    }
}

/* Puts @replacement where @child is, inside @parent (the layout or a GtkPaned). */
static void
replace_child (NautilusTileLayout *self,
               GtkWidget          *parent,
               GtkWidget          *child,
               GtkWidget          *replacement)
{
    if (parent == GTK_WIDGET (self))
    {
        adw_bin_set_child (ADW_BIN (self), replacement);
    }
    else if (gtk_paned_get_start_child (GTK_PANED (parent)) == child)
    {
        gtk_paned_set_start_child (GTK_PANED (parent), replacement);
    }
    else
    {
        gtk_paned_set_end_child (GTK_PANED (parent), replacement);
    }
}

/* Detaches @child from its parent without destroying it. Returns a new ref. */
static GtkWidget *
take_child (GtkWidget *parent,
            GtkWidget *child)
{
    g_object_ref (child);

    if (ADW_IS_BIN (parent))
    {
        adw_bin_set_child (ADW_BIN (parent), NULL);
    }
    else if (gtk_paned_get_start_child (GTK_PANED (parent)) == child)
    {
        gtk_paned_set_start_child (GTK_PANED (parent), NULL);
    }
    else
    {
        gtk_paned_set_end_child (GTK_PANED (parent), NULL);
    }

    return child;
}

static void
center_paned_once (GtkPaned   *paned,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
    int max = 0;

    g_object_get (paned, "max-position", &max, NULL);
    if (max <= 0)
    {
        return;
    }

    gtk_paned_set_position (paned, max / 2);
    g_signal_handlers_disconnect_by_func (paned, center_paned_once, user_data);
}

void
nautilus_tile_layout_set_root (NautilusTileLayout *self,
                               NautilusTile       *tile)
{
    g_return_if_fail (NAUTILUS_IS_TILE_LAYOUT (self));
    g_return_if_fail (self->tiles == NULL);

    adw_bin_set_child (ADW_BIN (self), GTK_WIDGET (tile));
    self->tiles = g_list_append (self->tiles, tile);
    update_classes (self);
}

void
nautilus_tile_layout_split (NautilusTileLayout *self,
                            NautilusTile       *target,
                            NautilusTile       *tile,
                            NautilusTileDrop    where)
{
    g_return_if_fail (NAUTILUS_IS_TILE_LAYOUT (self));
    g_return_if_fail (g_list_find (self->tiles, target) != NULL);
    g_return_if_fail (where != NAUTILUS_TILE_DROP_CENTER);

    GtkOrientation orientation = (where == NAUTILUS_TILE_DROP_LEFT || where == NAUTILUS_TILE_DROP_RIGHT)
                                 ? GTK_ORIENTATION_HORIZONTAL
                                 : GTK_ORIENTATION_VERTICAL;
    gboolean before = (where == NAUTILUS_TILE_DROP_LEFT || where == NAUTILUS_TILE_DROP_TOP);
    GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (target));
    GtkWidget *paned = gtk_paned_new (orientation);
    g_autoptr (GtkWidget) old = take_child (parent, GTK_WIDGET (target));

    gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
    gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
    replace_child (self, parent, NULL, paned);
    gtk_paned_set_start_child (GTK_PANED (paned), before ? GTK_WIDGET (tile) : old);
    gtk_paned_set_end_child (GTK_PANED (paned), before ? old : GTK_WIDGET (tile));

    /* Split evenly once the paned knows its size. */
    g_signal_connect (paned, "notify::max-position", G_CALLBACK (center_paned_once), NULL);

    self->tiles = g_list_append (self->tiles, tile);
    update_classes (self);
}

void
nautilus_tile_layout_remove (NautilusTileLayout *self,
                             NautilusTile       *tile)
{
    g_return_if_fail (NAUTILUS_IS_TILE_LAYOUT (self));
    g_return_if_fail (g_list_find (self->tiles, tile) != NULL);
    g_return_if_fail (self->tiles->next != NULL);

    GtkWidget *paned = gtk_widget_get_parent (GTK_WIDGET (tile));
    GtkWidget *grandparent = gtk_widget_get_parent (paned);
    GtkWidget *sibling = gtk_paned_get_start_child (GTK_PANED (paned)) == GTK_WIDGET (tile)
                         ? gtk_paned_get_end_child (GTK_PANED (paned))
                         : gtk_paned_get_start_child (GTK_PANED (paned));

    self->tiles = g_list_remove (self->tiles, tile);

    g_autoptr (GtkWidget) kept = take_child (paned, sibling);
    /* Drops the paned, and with it the removed tile. */
    replace_child (self, grandparent, paned, kept);

    update_classes (self);
}

GList *
nautilus_tile_layout_get_tiles (NautilusTileLayout *self)
{
    g_return_val_if_fail (NAUTILUS_IS_TILE_LAYOUT (self), NULL);

    return self->tiles;
}

guint
nautilus_tile_layout_get_n_tiles (NautilusTileLayout *self)
{
    g_return_val_if_fail (NAUTILUS_IS_TILE_LAYOUT (self), 0);

    return g_list_length (self->tiles);
}

void
nautilus_tile_layout_set_active (NautilusTileLayout *self,
                                 NautilusTile       *tile)
{
    g_return_if_fail (NAUTILUS_IS_TILE_LAYOUT (self));

    for (GList *l = self->tiles; l != NULL; l = l->next)
    {
        if (l->data == tile)
        {
            gtk_widget_add_css_class (l->data, "active-tile");
        }
        else
        {
            gtk_widget_remove_css_class (l->data, "active-tile");
        }
    }
}

NautilusTileDrop
nautilus_tile_layout_pick_drop (NautilusTileLayout *self,
                                NautilusTile       *tile,
                                double              x,
                                double              y,
                                graphene_rect_t    *zone)
{
    graphene_rect_t b;

    g_return_val_if_fail (NAUTILUS_IS_TILE_LAYOUT (self), NAUTILUS_TILE_DROP_CENTER);

    if (!gtk_widget_compute_bounds (GTK_WIDGET (tile), GTK_WIDGET (self), &b))
    {
        graphene_rect_init (&b, 0, 0, 0, 0);
    }

    double fx = b.size.width > 0 ? x / b.size.width : 0.5;
    double fy = b.size.height > 0 ? y / b.size.height : 0.5;
    double d_left = fx, d_right = 1 - fx, d_top = fy, d_bottom = 1 - fy;
    double nearest = MIN (MIN (d_left, d_right), MIN (d_top, d_bottom));
    NautilusTileDrop where = NAUTILUS_TILE_DROP_CENTER;

    if (nearest < EDGE_FRACTION)
    {
        where = nearest == d_left ? NAUTILUS_TILE_DROP_LEFT :
                nearest == d_right ? NAUTILUS_TILE_DROP_RIGHT :
                nearest == d_top ? NAUTILUS_TILE_DROP_TOP :
                NAUTILUS_TILE_DROP_BOTTOM;
    }

    *zone = b;
    switch (where)
    {
        case NAUTILUS_TILE_DROP_LEFT:
        {
            zone->size.width /= 2;
        }
        break;

        case NAUTILUS_TILE_DROP_RIGHT:
        {
            zone->size.width /= 2;
            zone->origin.x += zone->size.width;
        }
        break;

        case NAUTILUS_TILE_DROP_TOP:
        {
            zone->size.height /= 2;
        }
        break;

        case NAUTILUS_TILE_DROP_BOTTOM:
        {
            zone->size.height /= 2;
            zone->origin.y += zone->size.height;
        }
        break;

        case NAUTILUS_TILE_DROP_CENTER:
        default:
        {
        }
        break;
    }

    return where;
}

void
nautilus_tile_layout_set_drop_zone (NautilusTileLayout    *self,
                                    const graphene_rect_t *zone)
{
    g_return_if_fail (NAUTILUS_IS_TILE_LAYOUT (self));

    if (!self->has_drop_zone && zone == NULL)
    {
        return;
    }

    self->has_drop_zone = (zone != NULL);
    if (zone != NULL)
    {
        self->drop_zone = *zone;
    }

    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
nautilus_tile_layout_snapshot (GtkWidget   *widget,
                               GtkSnapshot *snapshot)
{
    NautilusTileLayout *self = NAUTILUS_TILE_LAYOUT (widget);

    GTK_WIDGET_CLASS (nautilus_tile_layout_parent_class)->snapshot (widget, snapshot);

    if (!self->has_drop_zone)
    {
        return;
    }

    g_autoptr (GdkRGBA) accent_ptr = adw_style_manager_get_accent_color_rgba (adw_style_manager_get_default ());
    GdkRGBA accent = *accent_ptr;
    GdkRGBA fill = accent;
    GskRoundedRect outline;
    graphene_rect_t r;
    float border = 2;

    graphene_rect_inset_r (&self->drop_zone, 6, 6, &r);
    gsk_rounded_rect_init_from_rect (&outline, &r, 10);

    fill.alpha = 0.18;
    gtk_snapshot_push_rounded_clip (snapshot, &outline);
    gtk_snapshot_append_color (snapshot, &fill, &r);
    gtk_snapshot_pop (snapshot);

    accent.alpha = 0.7;
    gtk_snapshot_append_border (snapshot, &outline,
                                (float[4]) { border, border, border, border },
                                (GdkRGBA[4]) { accent, accent, accent, accent });
}

static void
nautilus_tile_layout_dispose (GObject *object)
{
    NautilusTileLayout *self = NAUTILUS_TILE_LAYOUT (object);

    g_clear_pointer (&self->tiles, g_list_free);

    G_OBJECT_CLASS (nautilus_tile_layout_parent_class)->dispose (object);
}

static void
nautilus_tile_layout_class_init (NautilusTileLayoutClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_tile_layout_dispose;
    GTK_WIDGET_CLASS (klass)->snapshot = nautilus_tile_layout_snapshot;
    gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (klass), "nautilus-tile-layout");
}

static void
nautilus_tile_layout_init (NautilusTileLayout *self)
{
}
