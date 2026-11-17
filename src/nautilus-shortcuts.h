/*
 * nautilus-shortcuts.h: every command with its keys, the shortcuts
 * jump menu's Commands (F1), changeable keys and the left-hand layout
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Adds the keys of the commands to @window (a NautilusWindow): the
 * left-hand ones and any changed in the overview. */
void nautilus_shortcuts_attach         (GtkWindow *window);


/* The key to show next to @detailed_action in menus ("<alt>1"), the
 * left-hand one if it has one, or NULL. */
char *nautilus_shortcuts_get_accel     (const char *detailed_action);

/* Adds the key to the tooltips of the buttons in @root that run a command
 * ("Show Sidebar (Alt+B)"), and keeps them right when keys change. A
 * widget without an action can name one in its "flier-tooltip-action" data. */
void  nautilus_shortcuts_add_tooltips  (GtkWidget  *root);

/* Shows the hint for @detailed_action as if its key was pressed. */
void  nautilus_shortcuts_show_hint_for (GtkWindow  *window,
                                        const char *detailed_action);

/* The commands, for the jump menu. */
guint        nautilus_shortcuts_n_commands      (void);
const char  *nautilus_shortcuts_command_title   (guint i);
const char  *nautilus_shortcuts_command_section (guint i);
const char  *nautilus_shortcuts_command_action  (guint i);
GStrv        nautilus_shortcuts_command_keys    (guint i);
void         nautilus_shortcuts_run             (GtkWindow *window,
                                                 guint      i);

/* Changing a command's keys, from the jump menu: records a new key for
 * @slot (-1 adds one) in a small dialog over @window. */
void         nautilus_shortcuts_record_key      (GtkWindow *window,
                                                 guint      i,
                                                 int        slot);
void         nautilus_shortcuts_remove_key      (guint      i,
                                                 int        slot);
void         nautilus_shortcuts_reset_keys      (guint      i);
gboolean     nautilus_shortcuts_command_changed (guint      i);

G_END_DECLS
