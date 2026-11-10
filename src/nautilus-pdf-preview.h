/*
 * nautilus-pdf-preview.h: the pages of a PDF, to scroll through and zoom
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "nautilus-types.h"

G_BEGIN_DECLS

#define NAUTILUS_TYPE_PDF_PREVIEW (nautilus_pdf_preview_get_type ())

G_DECLARE_FINAL_TYPE (NautilusPdfPreview, nautilus_pdf_preview, NAUTILUS, PDF_PREVIEW, GtkWidget)

GtkWidget *nautilus_pdf_preview_new         (void);
void       nautilus_pdf_preview_set_file    (NautilusPdfPreview *self,
                                             NautilusFile       *file);

/* Whether a file of @mime_type can be shown; false without Poppler. */
gboolean   nautilus_pdf_preview_handles     (const char         *mime_type);

/* Zooming keeps the middle of the view in place. */
void       nautilus_pdf_preview_zoom        (NautilusPdfPreview *self,
                                             double              factor);
void       nautilus_pdf_preview_fit         (NautilusPdfPreview *self);
void       nautilus_pdf_preview_actual_size (NautilusPdfPreview *self);
void       nautilus_pdf_preview_scroll      (NautilusPdfPreview *self,
                                             double              dx,
                                             double              dy);
void       nautilus_pdf_preview_page        (NautilusPdfPreview *self,
                                             int                 pages);
gboolean   nautilus_pdf_preview_is_wider    (NautilusPdfPreview *self);

G_END_DECLS
