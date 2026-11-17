/*
 * nautilus-tile-layout.h: tiles of tabs, split side by side
 *
 * A tile is a tab bar over a tab view. The layout holds a tree of GtkPaned
 * with a tile at every leaf.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define NAUTILUS_TYPE_TILE (nautilus_tile_get_type ())

G_DECLARE_FINAL_TYPE (NautilusTile, nautilus_tile, NAUTILUS, TILE, GtkBox)

NautilusTile *nautilus_tile_new      (GMenuModel   *menu_model);
AdwTabView   *nautilus_tile_get_view (NautilusTile *self);
AdwTabBar    *nautilus_tile_get_bar  (NautilusTile *self);
/* Shown right after the last tab, in the tab bar's row. */
void          nautilus_tile_set_after_tabs (NautilusTile *self,
                                            GtkWidget    *widget);

/* The tile a widget (a tab view, a slot) lives in, or NULL. */
NautilusTile *nautilus_tile_for_widget (GtkWidget *widget);

typedef enum
{
    NAUTILUS_TILE_DROP_CENTER,
    NAUTILUS_TILE_DROP_LEFT,
    NAUTILUS_TILE_DROP_RIGHT,
    NAUTILUS_TILE_DROP_TOP,
    NAUTILUS_TILE_DROP_BOTTOM,
} NautilusTileDrop;

#define NAUTILUS_TYPE_TILE_LAYOUT (nautilus_tile_layout_get_type ())

G_DECLARE_FINAL_TYPE (NautilusTileLayout, nautilus_tile_layout, NAUTILUS, TILE_LAYOUT, AdwBin)

/* Sets the first tile of an empty layout. */
void          nautilus_tile_layout_set_root      (NautilusTileLayout *self,
                                                  NautilusTile       *tile);

/* Puts @tile next to @target, on the side @where names (not CENTER). */
void          nautilus_tile_layout_split         (NautilusTileLayout *self,
                                                  NautilusTile       *target,
                                                  NautilusTile       *tile,
                                                  NautilusTileDrop    where);

/* Removes a tile; the layout must keep at least one. The tile is destroyed. */
void          nautilus_tile_layout_remove        (NautilusTileLayout *self,
                                                  NautilusTile       *tile);

/* Tiles in creation order. Owned by the layout. */
GList        *nautilus_tile_layout_get_tiles     (NautilusTileLayout *self);
guint         nautilus_tile_layout_get_n_tiles   (NautilusTileLayout *self);

void          nautilus_tile_layout_set_active    (NautilusTileLayout *self,
                                                  NautilusTile       *tile);

/* Where a tab dropped at (@x, @y) in @tile's coordinates would go: an edge
 * splits the tile, the middle joins it. Fills @zone for the highlight. */
NautilusTileDrop nautilus_tile_layout_pick_drop  (NautilusTileLayout *self,
                                                  NautilusTile       *tile,
                                                  double              x,
                                                  double              y,
                                                  graphene_rect_t    *zone);

/* Highlights a drop zone, in layout coordinates; NULL hides it. */
void          nautilus_tile_layout_set_drop_zone (NautilusTileLayout    *self,
                                                  const graphene_rect_t *zone);

G_END_DECLS
