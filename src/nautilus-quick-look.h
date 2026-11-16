/*
 * nautilus-quick-look.h: the built-in Quick Look
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include "nautilus-types.h"

G_BEGIN_DECLS

/* Whether Quick Look is ours rather than the external previewer's. */
gboolean nautilus_quick_look_enabled    (void);

/* Shows @selection of @view, or closes Quick Look if it's showing. */
void     nautilus_quick_look_toggle     (NautilusFilesView *view,
                                         GList             *selection);
/* Follows a new selection of @view, if Quick Look is showing it. */
/* The same, in a window of its own, as big as the screen allows. */
void     nautilus_quick_look_toggle_window (NautilusFilesView *view,
                                            GList             *selection);
void     nautilus_quick_look_update     (NautilusFilesView *view,
                                         GList             *selection);
gboolean nautilus_quick_look_is_visible (void);
/* Whether it is up in @window, where it takes the keys. */
gboolean nautilus_quick_look_has_keys   (GtkWindow         *window);
/* Gives @window the overlay Quick Look shows in. Once, when it's built. */
void     nautilus_quick_look_attach     (AdwApplicationWindow *window);
void     nautilus_quick_look_close      (void);

G_END_DECLS
