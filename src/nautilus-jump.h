/*
 * nautilus-jump.h: the jump menu under the location entry: folders found
 * anywhere by name, commands and settings, while typing in the path bar
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "nautilus-window-slot.h"

G_BEGIN_DECLS

typedef struct _NautilusJump NautilusJump;

/* Hangs off @entry (the location entry), under the whole of @anchor. */
NautilusJump *nautilus_jump_new      (GtkWidget *entry,
                                      GtkWidget *anchor);
void          nautilus_jump_free     (NautilusJump *jump);

void          nautilus_jump_set_slot (NautilusJump       *jump,
                                      NautilusWindowSlot *slot);

/* Shows the menu for what is typed, or hides it for a path. */
void          nautilus_jump_update   (NautilusJump *jump);
/* Brings back what was typed when the menu last closed, selected. */
void          nautilus_jump_restore  (NautilusJump *jump);
/* Counts a visit to @location, to rank folders in use first. */
void          nautilus_jump_note_visit (GFile *location);
/* Shows the menu on its Commands tab. */
void          nautilus_jump_show_commands (NautilusJump *jump);
void          nautilus_jump_hide     (NautilusJump *jump);
gboolean      nautilus_jump_contains (NautilusJump *jump,
                                      GtkWidget    *widget);

/* Up, Down, Enter and Tab while the menu shows. TRUE if it took the key. */
gboolean      nautilus_jump_key      (NautilusJump    *jump,
                                      guint            keyval,
                                      GdkModifierType  state);

/* A path ("/…", "~…", "./…", "file://…") rather than a name to look up. */
gboolean      nautilus_jump_is_path  (const char *text);

G_END_DECLS
