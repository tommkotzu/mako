/*
 * nautilus-ql-canvas.h: the zoomable canvas of Quick Look
 *
 * Lays several files out in a grid, or shows one picture, and lets you
 * zoom (scroll, pinch, keys) around the pointer and pan (drag).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "nautilus-types.h"

G_BEGIN_DECLS

#define NAUTILUS_TYPE_QL_CANVAS (nautilus_ql_canvas_get_type ())

G_DECLARE_FINAL_TYPE (NautilusQlCanvas, nautilus_ql_canvas, NAUTILUS, QL_CANVAS, GtkWidget)

/* Signals: "item-activated" (NautilusFile *), an item was double-clicked;
 * "picture-loaded", the picture shown alone is in at full size. */
GtkWidget    *nautilus_ql_canvas_new          (void);

/* One file: shown alone, as a full-size picture if it is one. Several:
 * laid out in a grid, as thumbnails. */
void          nautilus_ql_canvas_set_files    (NautilusQlCanvas *self,
                                               GList            *files);
void          nautilus_ql_canvas_zoom         (NautilusQlCanvas *self,
                                               double            factor);
/* Shows everything; with one picture, at most at its real size. */
void          nautilus_ql_canvas_fit          (NautilusQlCanvas *self);
/* One picture at its real size. */
void          nautilus_ql_canvas_actual_size  (NautilusQlCanvas *self);
void          nautilus_ql_canvas_pan          (NautilusQlCanvas *self,
                                               double            dx,
                                               double            dy);
/* The item under the pointer, or the only one. */
NautilusFile *nautilus_ql_canvas_get_hovered  (NautilusQlCanvas *self);
gboolean      nautilus_ql_canvas_get_at_fit   (NautilusQlCanvas *self);
void          nautilus_ql_canvas_move_hovered (NautilusQlCanvas *self,
                                               GtkDirectionType  direction,
                                               gboolean          spatial);
/* Width x height of the picture shown alone, once loaded; 0 otherwise. */
void          nautilus_ql_canvas_get_picture_size (NautilusQlCanvas *self,
                                                   int              *width,
                                                   int              *height);

G_END_DECLS
