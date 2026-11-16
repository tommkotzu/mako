/*
 * nautilus-shortcuts.c: every command with its keys, the shortcuts
 * overview (F1), changeable keys and the left-hand layout
 *
 * Nautilus binds its keys in several places (view class bindings, slot and
 * toolbar controllers, application accels). Rather than move them all,
 * this table lists each command with the keys upstream gives it, plus
 * extra keys so everything can be done with the left hand alone. One
 * shortcut controller per window, in the capture phase, handles the extra
 * keys and any keys changed in the overview; a changed command's old keys
 * are swallowed there, before upstream's bindings see them.
 *
 * Changed keys are stored in org.gnome.nautilus.flier custom-shortcuts, as
 * action → accelerators.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-shortcuts.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "nautilus-application.h"
#include "nautilus-bookmark.h"
#include "nautilus-bookmark-list.h"
#include "nautilus-labels.h"
#include "nautilus-quick-look.h"
#include "nautilus-window-slot.h"

typedef struct
{
    const char *section;
    const char *title;
    const char *action;   /* detailed: "view.rate(uint32 3)" */
    const char *defaults; /* upstream's keys, '|'-separated */
    const char *extras;   /* ours: the left-hand ones */
} Command;

#define SECTION_NAVIGATE N_("Navigate")
#define SECTION_TABS N_("Tabs and Tiles")
#define SECTION_OPEN N_("Open")
#define SECTION_EDIT N_("Edit")
#define SECTION_SELECT N_("Select")
#define SECTION_VIEW N_("View")
#define SECTION_ORGANIZE N_("Rate and Tag")
#define SECTION_APP N_("Application")

static const Command commands[] =
{
    { SECTION_NAVIGATE, N_("Back"), "slot.back", "<alt>Left", "<alt>a" },
    { SECTION_NAVIGATE, N_("Forward"), "slot.forward", "<alt>Right", "<alt>d" },
    { SECTION_NAVIGATE, N_("Parent Folder"), "slot.up", "<alt>Up", "<alt>w" },
    { SECTION_NAVIGATE, N_("Home Folder"), "win.go-home", "<alt>Home", "<alt><shift>w" },
    { SECTION_NAVIGATE, N_("Enter Location"), "toolbar.edit-location", "<ctrl>l", "<ctrl>e" },
    { SECTION_NAVIGATE, N_("Search"), "slot.focus-search", "<ctrl>f", NULL },
    { SECTION_NAVIGATE, N_("Search Everywhere"), "slot.search-global", "<ctrl><shift>f", NULL },
    { SECTION_NAVIGATE, N_("Reload"), "slot.reload", "F5|<ctrl>r", NULL },
    { SECTION_NAVIGATE, N_("Bookmark This Folder"), "slot.bookmark-current-directory", "<ctrl>d", NULL },
    { SECTION_NAVIGATE, N_("Next Place in the Sidebar"), "win.sidebar-step(1)", NULL, "<ctrl>Down" },
    { SECTION_NAVIGATE, N_("Previous Place in the Sidebar"), "win.sidebar-step(-1)", NULL, "<ctrl>Up" },
    { SECTION_NAVIGATE, N_("Fold the Sidebar Group"), "win.sidebar-fold(true)", NULL, "<ctrl>Left" },
    { SECTION_NAVIGATE, N_("Unfold the Sidebar Group"), "win.sidebar-fold(false)", NULL, "<ctrl>Right" },
    { SECTION_NAVIGATE, N_("Show Sidebar"), "win.toggle-sidebar", "F9", "<alt>q" },

    { SECTION_TABS, N_("New Tab"), "win.new-tab", "<ctrl>t", NULL },
    { SECTION_TABS, N_("Close Tab"), "win.close-current-view", "<ctrl>w", NULL },
    { SECTION_TABS, N_("Reopen Closed Tab"), "win.restore-tab", "<ctrl><shift>t", NULL },
    { SECTION_TABS, N_("Move Tab Left"), "win.tab-move-left", "<ctrl><shift>Page_Up", NULL },
    { SECTION_TABS, N_("Move Tab Right"), "win.tab-move-right", "<ctrl><shift>Page_Down", NULL },
    { SECTION_TABS, N_("Split Right"), "win.split-right", "F3", NULL },
    { SECTION_TABS, N_("Split Down"), "win.split-down", "<shift>F3", NULL },
    { SECTION_TABS, N_("Next Tab"), "win.tab-next-all", NULL, "<ctrl>Tab|<ctrl>Page_Down" },
    { SECTION_TABS, N_("Previous Tab"), "win.tab-previous-all", NULL, "<ctrl><shift>Tab|<ctrl>Page_Up|<ctrl>ISO_Left_Tab" },
    { SECTION_TABS, N_("Next Tile"), "win.focus-next-tile", "F6", "<alt>t|<ctrl>grave" },
    { SECTION_TABS, N_("Previous Tile"), "win.focus-previous-tile", "<shift>F6", "<alt><shift>t|<ctrl>asciitilde" },
    { SECTION_TABS, N_("New Window"), "app.clone-window", "<ctrl>n", NULL },

    { SECTION_OPEN, N_("Open"), "view.open-with-default-application", "Return|<ctrl>o", "<alt>s" },
    { SECTION_OPEN, N_("Open in New Tab"), "view.open-item-new-tab", "<ctrl>Return", "<alt><shift>s" },
    { SECTION_OPEN, N_("Open in New Window"), "view.open-item-new-window", "<shift>Return", NULL },
    { SECTION_OPEN, N_("Open Item Location"), "view.open-item-location", "<ctrl><alt>o", NULL },
    { SECTION_OPEN, N_("Quick Look"), "view.preview-selection", "space", NULL },
    { SECTION_OPEN, N_("Quick Look in a Window"), "view.preview-selection-window", "<ctrl>space", NULL },
    { SECTION_OPEN, N_("Open in Console"), "view.current-directory-console", "<ctrl>period", NULL },

    { SECTION_EDIT, N_("Copy"), "view.copy", "<ctrl>c", NULL },
    { SECTION_EDIT, N_("Cut"), "view.cut", "<ctrl>x", NULL },
    { SECTION_EDIT, N_("Paste"), "view.paste", "<ctrl>v", NULL },
    { SECTION_EDIT, N_("Undo"), "win.undo", "<ctrl>z", NULL },
    { SECTION_EDIT, N_("Redo"), "win.redo", "<ctrl><shift>z", NULL },
    { SECTION_EDIT, N_("Rename"), "view.rename", "F2", NULL },
    { SECTION_EDIT, N_("New Folder"), "view.new-folder", "<ctrl><shift>n", "<ctrl><shift>d" },
    { SECTION_EDIT, N_("Move to Trash"), "view.move-to-trash", "Delete", "<alt>x" },
    { SECTION_EDIT, N_("Delete Permanently"), "view.delete-permanently-shortcut", "<shift>Delete", "<alt><shift>x" },
    { SECTION_EDIT, N_("Create Link"), "view.create-link", "<ctrl>m", NULL },
    { SECTION_EDIT, N_("Properties"), "view.properties", "<ctrl>i|<alt>Return", "<alt>g" },

    { SECTION_SELECT, N_("Select All"), "view.select-all", "<ctrl>a", NULL },
    { SECTION_SELECT, N_("Invert Selection"), "view.invert-selection", "<ctrl><shift>i", "<ctrl><shift>a" },
    { SECTION_SELECT, N_("Select Items Matching"), "view.select-pattern", "<ctrl>s", NULL },

    { SECTION_VIEW, N_("List View"), "win.view-mode(uint32 1)", "<ctrl>1", "<alt>1" },
    { SECTION_VIEW, N_("Tree View"), "win.view-mode(uint32 2)", NULL, "<alt>2" },
    { SECTION_VIEW, N_("Columns View"), "win.view-mode(uint32 3)", NULL, "<alt>3" },
    { SECTION_VIEW, N_("Grid View"), "win.view-mode(uint32 4)", "<ctrl>2", "<alt>4" },
    { SECTION_VIEW, N_("Flat View"), "win.flat-view", NULL, "<alt>r" },
    { SECTION_VIEW, N_("Show Bottom Bar"), "win.bottom-bar", NULL, "<alt><shift>q" },
    { SECTION_VIEW, N_("Show Shortcut Hints"), "win.shortcut-hints", NULL, NULL },
    { SECTION_VIEW, N_("Sidebar Groups"), "win.sidebar-groups", NULL, NULL },
    { SECTION_VIEW, N_("Next View"), "win.view-step(1)", NULL, NULL },
    { SECTION_VIEW, N_("Previous View"), "win.view-step(-1)", NULL, NULL },
    { SECTION_VIEW, N_("Zoom In"), "view.zoom-in", "<ctrl>plus", "<ctrl><alt>w" },
    { SECTION_VIEW, N_("Zoom Out"), "view.zoom-out", "<ctrl>minus", "<ctrl><alt>s" },
    { SECTION_VIEW, N_("Reset Zoom"), "view.zoom-standard", "<ctrl>0", NULL },
    { SECTION_VIEW, N_("Show Hidden Files"), "view.show-hidden-files", "<ctrl>h", "<alt>v" },
    { SECTION_VIEW, N_("Show Inspector"), "win.inspector-visible", NULL, "<alt>e|F4" },
    { SECTION_VIEW, N_("Move Inspector Clockwise"), "win.inspector-side-next", NULL, "<alt><shift>e|<shift>F4" },
    { SECTION_VIEW, N_("Move Inspector Counterclockwise"), "win.inspector-side-previous", NULL, "<ctrl><shift>F4" },
    { SECTION_VIEW, N_("Inspector on the Right"), "win.inspector-side('right')", NULL, NULL },
    { SECTION_VIEW, N_("Inspector at the Bottom"), "win.inspector-side('bottom')", NULL, NULL },
    { SECTION_VIEW, N_("Inspector on the Left"), "win.inspector-side('left')", NULL, NULL },
    { SECTION_VIEW, N_("Inspector at the Top"), "win.inspector-side('top')", NULL, NULL },
    { SECTION_VIEW, N_("Next Grouping"), "view.group-by-next", NULL, "<ctrl>g" },
    { SECTION_VIEW, N_("Group by Date"), "view.group-by('date')", NULL, NULL },
    { SECTION_VIEW, N_("Group by First Letter"), "view.group-by('name')", NULL, NULL },
    { SECTION_VIEW, N_("Group by Rating"), "view.group-by('rating')", NULL, NULL },
    { SECTION_VIEW, N_("Group by Kind"), "view.group-by('kind')", NULL, NULL },
    { SECTION_VIEW, N_("No Groups"), "view.group-by('none')", NULL, NULL },

    { SECTION_ORGANIZE, N_("Rate 1 Star"), "view.rate(uint32 1)", "<ctrl><alt>1", NULL },
    { SECTION_ORGANIZE, N_("Rate 2 Stars"), "view.rate(uint32 2)", "<ctrl><alt>2", NULL },
    { SECTION_ORGANIZE, N_("Rate 3 Stars"), "view.rate(uint32 3)", "<ctrl><alt>3", NULL },
    { SECTION_ORGANIZE, N_("Rate 4 Stars"), "view.rate(uint32 4)", "<ctrl><alt>4", NULL },
    { SECTION_ORGANIZE, N_("Rate 5 Stars"), "view.rate(uint32 5)", "<ctrl><alt>5", NULL },
    { SECTION_ORGANIZE, N_("Remove Rating"), "view.rate(uint32 0)", "<ctrl><alt>grave|<ctrl><alt>0", NULL },
    { SECTION_ORGANIZE, N_("Tags"), "view.edit-tags", "<ctrl><alt>a", NULL },
    { SECTION_ORGANIZE, N_("Ratings and Tags on Grid Items"), "view.toggle-marks-on-tiles", "<ctrl><shift>r", NULL },

    { SECTION_APP, N_("Commands and Shortcuts"), "win.keyboard-shortcuts", NULL,
      "F1|<ctrl>k|<ctrl><shift>p|<ctrl>question" },
    { SECTION_APP, N_("Help"), "app.help", "<shift>F1", NULL },
    { SECTION_APP, N_("Preferences"), "app.preferences", "<ctrl>comma", NULL },
    { SECTION_APP, N_("Main Menu"), "win.current-location-menu", "F10", NULL },
    { SECTION_APP, N_("Context Menu"), "view.popup-menu", "<shift>F10|Menu", NULL },
    { SECTION_APP, N_("Quit"), "app.quit", "<ctrl>q", NULL },
};

/* Things that aren't commands, shown for reference. */
static const struct
{
    const char *title;
    const char *keys;
} basics[] =
{
    { N_("Rate an item with the mouse"), N_("Hold Alt and click its stars") },
    { N_("Properties"), N_("Alt+Double-click") },
    { N_("Open folder in a new tab"), N_("Ctrl+Double-click, Middle-click") },
    { N_("Quick Look: rate, clear"), "1–5  `" },
    { N_("Quick Look: zoom in, out, fit, real size"), "W  S  Q  Z" },
    { N_("Quick Look: previous, next"), "A  D" },
    { N_("Quick Look: play, back, ahead, mute"), "K  J  L  M" },
    { N_("Quick Look: frame back, ahead"), "Shift+A  Shift+D  ,  ." },
    { N_("Quick Look: names, marks, fullscreen"), "N  T  Tab" },
};

/* ----------------------------------------------------- Effective keys */

static GStrv
split_keys (const char *keys)
{
    return keys != NULL && *keys != '\0' ? g_strsplit (keys, "|", -1) : g_new0 (char *, 1);
}

/* The keys a command has now: changed ones, or upstream's and ours. */
static GStrv
command_keys (const Command *command,
              gboolean      *changed)
{
    g_autoptr (GVariant) custom = g_settings_get_value (nautilus_flier_settings (), "custom-shortcuts");
    g_autoptr (GVariant) value = g_variant_lookup_value (custom, command->action, G_VARIANT_TYPE_STRING_ARRAY);

    if (changed != NULL)
    {
        *changed = value != NULL;
    }

    if (value != NULL)
    {
        return g_variant_dup_strv (value, NULL);
    }

    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
    g_auto (GStrv) defaults = split_keys (command->defaults);
    g_auto (GStrv) extras = split_keys (command->extras);

    g_strv_builder_addv (builder, (const char **) defaults);
    g_strv_builder_addv (builder, (const char **) extras);

    return g_strv_builder_end (builder);
}

static void
set_command_keys (const Command      *command,
                  const char * const *keys)
{
    GSettings *settings = nautilus_flier_settings ();
    g_autoptr (GVariant) custom = g_settings_get_value (settings, "custom-shortcuts");
    GVariantBuilder builder;
    GVariantIter iter;
    const char *action;
    GVariant *value;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sas}"));
    g_variant_iter_init (&iter, custom);
    while (g_variant_iter_next (&iter, "{&s@as}", &action, &value))
    {
        if (g_strcmp0 (action, command->action) != 0)
        {
            g_variant_builder_add (&builder, "{s@as}", action, value);
        }
        g_variant_unref (value);
    }

    if (keys != NULL)
    {
        g_variant_builder_add (&builder, "{s^as}", command->action, keys);
    }

    g_settings_set_value (settings, "custom-shortcuts", g_variant_builder_end (&builder));
}

static gboolean
same_accel (const char *a,
            const char *b)
{
    guint ka, kb;
    GdkModifierType ma, mb;

    return gtk_accelerator_parse (a, &ka, &ma) && gtk_accelerator_parse (b, &kb, &mb) &&
           gdk_keyval_to_lower (ka) == gdk_keyval_to_lower (kb) && ma == mb;
}

static char *
accel_label (const char *accel)
{
    guint key;
    GdkModifierType mods;

    if (!gtk_accelerator_parse (accel, &key, &mods))
    {
        return g_strdup (accel);
    }

    return gtk_accelerator_get_label (key, mods);
}

static gboolean
same_action (const char *a,
             const char *b)
{
    g_autofree char *name_a = NULL, *name_b = NULL;
    g_autoptr (GVariant) target_a = NULL, target_b = NULL;

    if (!g_action_parse_detailed_name (a, &name_a, &target_a, NULL) ||
        !g_action_parse_detailed_name (b, &name_b, &target_b, NULL))
    {
        return FALSE;
    }

    return g_str_equal (name_a, name_b) &&
           ((target_a == NULL && target_b == NULL) ||
            (target_a != NULL && target_b != NULL && g_variant_equal (target_a, target_b)));
}

char *
nautilus_shortcuts_get_accel (const char *detailed_action)
{
    for (guint i = 0; i < G_N_ELEMENTS (commands); i++)
    {
        const Command *command = &commands[i];
        gboolean changed;

        if (!same_action (command->action, detailed_action))
        {
            continue;
        }

        g_auto (GStrv) keys = command_keys (command, &changed);
        g_auto (GStrv) extras = split_keys (command->extras);

        if (!changed && extras[0] != NULL)
        {
            return g_strdup (extras[0]);
        }
        return g_strdup (keys[0]);
    }

    return NULL;
}

static void show_hint (GtkWindow *window, const Command *command, const char *accel);

void
nautilus_shortcuts_show_hint_for (GtkWindow  *window,
                                  const char *detailed_action)
{
    for (guint i = 0; i < G_N_ELEMENTS (commands); i++)
    {
        if (same_action (commands[i].action, detailed_action))
        {
            g_autofree char *accel = nautilus_shortcuts_get_accel (detailed_action);

            show_hint (window, &commands[i], accel);
            return;
        }
    }
}

/* --------------------------------------------------------- Tooltips */

#define TOOLTIP_BASE "flier-tooltip-base"
#define TOOLTIP_BUSY "flier-tooltip-busy"

static char *
widget_action (GtkWidget *widget)
{
    const char *named = g_object_get_data (G_OBJECT (widget), "flier-tooltip-action");

    if (named != NULL)
    {
        return g_strdup (named);
    }
    if (!GTK_IS_ACTIONABLE (widget) || gtk_actionable_get_action_name (GTK_ACTIONABLE (widget)) == NULL)
    {
        return NULL;
    }
    return g_action_print_detailed_name (gtk_actionable_get_action_name (GTK_ACTIONABLE (widget)),
                                         gtk_actionable_get_action_target_value (GTK_ACTIONABLE (widget)));
}

static char *
key_label_for (GtkWidget  *widget,
               const char *action)
{
    g_autofree char *accel = nautilus_shortcuts_get_accel (action);
    guint key;
    GdkModifierType mods;

    if (accel == NULL)
    {
        /* Not in our table: what the application has for it. */
        GtkApplication *app = GTK_APPLICATION (g_application_get_default ());
        g_auto (GStrv) accels = app != NULL ? gtk_application_get_accels_for_action (app, action) : NULL;

        if (accels != NULL && accels[0] != NULL)
        {
            accel = g_strdup (accels[0]);
        }
    }
    if (accel == NULL || !gtk_accelerator_parse (accel, &key, &mods))
    {
        return NULL;
    }
    return gtk_accelerator_get_label (key, mods);
}

static void
update_tooltip (GtkWidget *widget)
{
    const char *base = g_object_get_data (G_OBJECT (widget), TOOLTIP_BASE);
    g_autofree char *action = widget_action (widget);
    g_autofree char *label = action != NULL ? key_label_for (widget, action) : NULL;
    g_autofree char *text = NULL;

    if (base == NULL)
    {
        return;
    }
    text = label != NULL ? g_strdup_printf ("%s (%s)", base, label) : g_strdup (base);

    g_object_set_data (G_OBJECT (widget), TOOLTIP_BUSY, GINT_TO_POINTER (TRUE));
    gtk_widget_set_tooltip_text (widget, text);
    g_object_set_data (G_OBJECT (widget), TOOLTIP_BUSY, NULL);
}

/* The widget's own code changed its tooltip: that's the new base. */
static void
on_tooltip_changed (GtkWidget *widget)
{
    const char *text = gtk_widget_get_tooltip_text (widget);

    if (g_object_get_data (G_OBJECT (widget), TOOLTIP_BUSY) != NULL)
    {
        return;
    }
    g_object_set_data_full (G_OBJECT (widget), TOOLTIP_BASE, g_strdup (text), g_free);
    update_tooltip (widget);
}

static void
add_tooltips (GtkWidget *widget)
{
    for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
         child = gtk_widget_get_next_sibling (child))
    {
        add_tooltips (child);
    }

    const char *text = gtk_widget_get_tooltip_text (widget);
    g_autofree char *action = NULL;

    if (text == NULL || g_object_get_data (G_OBJECT (widget), TOOLTIP_BASE) != NULL)
    {
        return;
    }
    action = widget_action (widget);
    if (action == NULL)
    {
        return;
    }

    g_object_set_data_full (G_OBJECT (widget), TOOLTIP_BASE, g_strdup (text), g_free);
    update_tooltip (widget);
    g_signal_connect (widget, "notify::tooltip-text", G_CALLBACK (on_tooltip_changed), NULL);
    g_signal_connect_object (nautilus_flier_settings (), "changed::custom-shortcuts",
                             G_CALLBACK (update_tooltip), widget, G_CONNECT_SWAPPED);
}

void
nautilus_shortcuts_add_tooltips (GtkWidget *root)
{
    add_tooltips (root);
}

/* --------------------------------------------------------- Running */

static void
run_command (GtkWindow     *window,
             const Command *command)
{
    g_autofree char *name = NULL;
    g_autoptr (GVariant) target = NULL;
    g_autoptr (GtkWidget) slot = NULL;
    g_autoptr (GError) error = NULL;

    if (!g_action_parse_detailed_name (command->action, &name, &target, &error))
    {
        g_warning ("Bad command %s: %s", command->action, error->message);
        return;
    }

    /* From the active slot, which reaches the slot's, view's, window's and
     * application's actions. The toolbar's are in a branch of their own. */
    g_object_get (window, "active-slot", &slot, NULL);

    if (slot != NULL && gtk_widget_activate_action_variant (slot, name, target))
    {
        return;
    }

    for (GtkWidget *w = gtk_widget_get_first_child (GTK_WIDGET (window)); w != NULL;)
    {
        if (gtk_widget_activate_action_variant (w, name, target))
        {
            return;
        }

        /* Depth first. */
        if (gtk_widget_get_first_child (w) != NULL)
        {
            w = gtk_widget_get_first_child (w);
            continue;
        }
        while (w != NULL && gtk_widget_get_next_sibling (w) == NULL)
        {
            w = gtk_widget_get_parent (w);
            if (w == GTK_WIDGET (window))
            {
                w = NULL;
            }
        }
        if (w != NULL)
        {
            w = gtk_widget_get_next_sibling (w);
        }
    }
}

/* ------------------------------------------------------ Hints */

/* Like File Flier's: a small label at the bottom of the window saying what
 * a shortcut just did. */
#define HINT_MS 1400

typedef struct
{
    GtkWidget *revealer;
    GtkLabel *title;
    GtkLabel *keys;
    guint timeout;
} Hint;

static void
hint_free (Hint *hint)
{
    g_clear_handle_id (&hint->timeout, g_source_remove);
    g_free (hint);
}

static gboolean
hint_hide (gpointer user_data)
{
    Hint *hint = user_data;

    hint->timeout = 0;
    gtk_revealer_set_reveal_child (GTK_REVEALER (hint->revealer), FALSE);
    return G_SOURCE_REMOVE;
}

static void
show_hint (GtkWindow     *window,
           const Command *command,
           const char    *accel)
{
    GtkWidget *overlay = g_object_get_data (G_OBJECT (window), "quick-look-overlay");
    Hint *hint = g_object_get_data (G_OBJECT (window), "nautilus-shortcut-hint");
    g_autofree char *keys = accel != NULL ? accel_label (accel) : NULL;

    if (overlay == NULL || !g_settings_get_boolean (nautilus_flier_settings (), "shortcut-hints"))
    {
        return;
    }

    if (hint == NULL)
    {
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);

        hint = g_new0 (Hint, 1);
        hint->title = GTK_LABEL (gtk_label_new (NULL));
        hint->keys = GTK_LABEL (gtk_label_new (NULL));
        gtk_widget_add_css_class (GTK_WIDGET (hint->title), "heading");
        gtk_widget_add_css_class (GTK_WIDGET (hint->keys), "dim-label");
        gtk_widget_add_css_class (GTK_WIDGET (hint->keys), "monospace");
        gtk_box_append (GTK_BOX (box), GTK_WIDGET (hint->title));
        gtk_box_append (GTK_BOX (box), GTK_WIDGET (hint->keys));
        gtk_widget_add_css_class (box, "shortcut-hint");

        hint->revealer = gtk_revealer_new ();
        gtk_revealer_set_transition_type (GTK_REVEALER (hint->revealer), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
        gtk_revealer_set_transition_duration (GTK_REVEALER (hint->revealer), 150);
        gtk_revealer_set_child (GTK_REVEALER (hint->revealer), box);
        gtk_widget_set_halign (hint->revealer, GTK_ALIGN_CENTER);
        gtk_widget_set_valign (hint->revealer, GTK_ALIGN_END);
        gtk_widget_set_margin_bottom (hint->revealer, 64);
        gtk_widget_set_can_target (hint->revealer, FALSE);
        gtk_widget_set_can_focus (hint->revealer, FALSE);
        gtk_overlay_add_overlay (GTK_OVERLAY (overlay), hint->revealer);
        g_object_set_data_full (G_OBJECT (window), "nautilus-shortcut-hint", hint, (GDestroyNotify) hint_free);
    }

    gtk_label_set_text (hint->title, _(command->title));
    gtk_label_set_text (hint->keys, keys != NULL ? keys : "");
    gtk_widget_set_visible (GTK_WIDGET (hint->keys), keys != NULL);
    gtk_revealer_set_reveal_child (GTK_REVEALER (hint->revealer), TRUE);
    g_clear_handle_id (&hint->timeout, g_source_remove);
    hint->timeout = g_timeout_add (HINT_MS, hint_hide, hint);
}

/* Keys that are plain typing or moving about aren't worth a hint. */
static gboolean
worth_a_hint (const Command *command,
              const char    *accel)
{
    guint key;
    GdkModifierType mods;

    if (!gtk_accelerator_parse (accel, &key, &mods))
    {
        return FALSE;
    }

    return !g_str_equal (command->action, "view.preview-selection") &&
           (mods != 0 || (key >= GDK_KEY_F1 && key <= GDK_KEY_F12) || key == GDK_KEY_Delete) &&
           !(mods == GDK_CONTROL_MASK && (key == GDK_KEY_k || key == GDK_KEY_question));
}

/* What a shortcut runs, and by which key. */
typedef struct
{
    const Command *command;
    char *accel;
} Binding;

static void
binding_free (Binding *binding)
{
    g_free (binding->accel);
    g_free (binding);
}

/* ----------------------------------------------------- The controller */

/* Whether a key may act while typing: Alt and function keys aren't typing. */
static gboolean
works_in_text (GtkShortcutTrigger *trigger)
{
    if (GTK_IS_KEYVAL_TRIGGER (trigger))
    {
        guint key = gtk_keyval_trigger_get_keyval (GTK_KEYVAL_TRIGGER (trigger));
        GdkModifierType mods = gtk_keyval_trigger_get_modifiers (GTK_KEYVAL_TRIGGER (trigger));

        return (mods & GDK_ALT_MASK) || (key >= GDK_KEY_F1 && key <= GDK_KEY_F12) ||
               key == GDK_KEY_grave;
    }

    return FALSE;
}

static gboolean
key_allowed (GtkWidget *widget,
             GVariant  *args)
{
    GtkWindow *window = GTK_WINDOW (widget);
    GtkWidget *focus = gtk_window_get_focus (window);

    /* Not while a dialog of the window, or Quick Look, has the keys. */
    if ((ADW_IS_APPLICATION_WINDOW (window) &&
         adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (window)) != NULL) ||
        nautilus_quick_look_has_keys (window))
    {
        return FALSE;
    }

    if (focus == NULL || (!GTK_IS_EDITABLE (focus) && !GTK_IS_TEXT_VIEW (focus)))
    {
        return TRUE;
    }

    return args != NULL && g_variant_get_boolean (args);
}

static gboolean
on_command_key (GtkWidget *widget,
                GVariant  *args,
                gpointer   user_data)
{
    Binding *binding = user_data;

    if (!key_allowed (widget, args))
    {
        return FALSE;
    }

    if (worth_a_hint (binding->command, binding->accel))
    {
        show_hint (GTK_WINDOW (widget), binding->command, binding->accel);
    }
    run_command (GTK_WINDOW (widget), binding->command);
    return TRUE;
}

static gboolean
on_swallowed_key (GtkWidget *widget,
                  GVariant  *args,
                  gpointer   user_data)
{
    /* An old key of a changed command: upstream's binding mustn't see it. */
    return key_allowed (widget, args);
}

/* Upstream's own key, left to upstream's binding: only the hint. */
static gboolean
on_hint_key (GtkWidget *widget,
             GVariant  *args,
             gpointer   user_data)
{
    Binding *binding = user_data;

    if (key_allowed (widget, args))
    {
        show_hint (GTK_WINDOW (widget), binding->command, binding->accel);
    }
    return FALSE;
}

static void
add_shortcut (GtkShortcutController *controller,
              const char            *accel,
              GtkShortcutFunc        func,
              const Command         *command)
{
    GtkShortcutTrigger *trigger = gtk_shortcut_trigger_parse_string (accel);

    if (trigger == NULL)
    {
        g_warning ("Bad shortcut %s for %s", accel, command->action);
        return;
    }

    Binding *binding = g_new0 (Binding, 1);

    binding->command = command;
    binding->accel = g_strdup (accel);

    GtkShortcut *shortcut = gtk_shortcut_new (trigger,
                                              gtk_callback_action_new (func, binding,
                                                                       (GDestroyNotify) binding_free));

    gtk_shortcut_set_arguments (shortcut, g_variant_new_boolean (works_in_text (trigger)));
    gtk_shortcut_controller_add_shortcut (controller, shortcut);
}

static void
fill_controller (GtkShortcutController *controller)
{
    GListModel *model = G_LIST_MODEL (controller);

    while (g_list_model_get_n_items (model) > 0)
    {
        g_autoptr (GtkShortcut) shortcut = g_list_model_get_item (model, 0);

        gtk_shortcut_controller_remove_shortcut (controller, shortcut);
    }

    for (guint i = 0; i < G_N_ELEMENTS (commands); i++)
    {
        const Command *command = &commands[i];
        gboolean changed;
        g_auto (GStrv) keys = command_keys (command, &changed);
        g_auto (GStrv) defaults = split_keys (command->defaults);

        if (!changed)
        {
            /* Upstream's keys work as they are; add ours. */
            g_auto (GStrv) extras = split_keys (command->extras);

            for (guint d = 0; defaults[d] != NULL; d++)
            {
                if (worth_a_hint (command, defaults[d]))
                {
                    add_shortcut (controller, defaults[d], on_hint_key, command);
                }
            }
            for (guint k = 0; extras[k] != NULL; k++)
            {
                add_shortcut (controller, extras[k], on_command_key, command);
            }
            continue;
        }

        for (guint k = 0; keys[k] != NULL; k++)
        {
            add_shortcut (controller, keys[k], on_command_key, command);
        }

        for (guint d = 0; defaults[d] != NULL; d++)
        {
            gboolean kept = FALSE;

            for (guint k = 0; keys[k] != NULL && !kept; k++)
            {
                kept = same_accel (defaults[d], keys[k]);
            }
            if (!kept)
            {
                add_shortcut (controller, defaults[d], on_swallowed_key, command);
            }
        }
    }
}

static void
action_keyboard_shortcuts (GSimpleAction *action,
                           GVariant      *parameter,
                           gpointer       user_data)
{
    nautilus_shortcuts_dialog_present (GTK_WINDOW (user_data));
}

void
nautilus_shortcuts_attach (GtkWindow *window)
{
    GtkEventController *controller = gtk_shortcut_controller_new ();
    static const GActionEntry entries[] =
    {
        { .name = "keyboard-shortcuts", .activate = action_keyboard_shortcuts },
    };

    g_action_map_add_action_entries (G_ACTION_MAP (window), entries, G_N_ELEMENTS (entries), window);

    /* Before the focused widget and upstream's bindings. */
    gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
    gtk_event_controller_set_name (controller, "nautilus-shortcuts");
    fill_controller (GTK_SHORTCUT_CONTROLLER (controller));
    gtk_widget_add_controller (GTK_WIDGET (window), controller);

    g_signal_connect_object (nautilus_flier_settings (), "changed::custom-shortcuts",
                             G_CALLBACK (fill_controller), controller, G_CONNECT_SWAPPED);
}

/* ---------------------------------------------------------- Overview */

typedef struct
{
    AdwDialog *dialog;
    GtkWindow *window;
    GtkListBox *list;
    GtkSearchEntry *search;
    AdwToastOverlay *toasts;

    /* Recording a key: for which command, replacing which key (-1: a new one). */
    const Command *recording;
    int recording_slot;
    AdwDialog *recorder;
    guint rebuild_idle;
    guint select_idle;
} Overview;

static void rebuild_rows (Overview *overview);

static void
overview_free (Overview *overview)
{
    g_clear_handle_id (&overview->rebuild_idle, g_source_remove);
    g_clear_handle_id (&overview->select_idle, g_source_remove);
    g_free (overview);
}

static gboolean
rebuild_idle (gpointer user_data)
{
    Overview *overview = user_data;

    overview->rebuild_idle = 0;
    rebuild_rows (overview);
    return G_SOURCE_REMOVE;
}

/* Later: the change may come from a button of the rows being replaced. */
static void
on_custom_changed (GSettings  *settings,
                   const char *key,
                   AdwDialog  *dialog)
{
    Overview *overview = g_object_get_data (G_OBJECT (dialog), "overview");

    if (overview->rebuild_idle == 0)
    {
        overview->rebuild_idle = g_idle_add (rebuild_idle, overview);
    }
}

static void
on_row_activated (GtkListBox    *list,
                  GtkListBoxRow *row,
                  Overview      *overview)
{
    const Command *command = g_object_get_data (G_OBJECT (row), "command");
    GFile *location = g_object_get_data (G_OBJECT (row), "location");
    GtkWindow *window = overview->window;

    if (location != NULL)
    {
        g_autoptr (NautilusWindowSlot) slot = NULL;
        g_autoptr (GFile) keep = g_object_ref (location);

        g_object_get (window, "active-slot", &slot, NULL);
        adw_dialog_close (overview->dialog);
        if (slot != NULL)
        {
            nautilus_window_slot_open_location_full (slot, keep, NULL);
        }
        return;
    }

    if (command == NULL)
    {
        return;
    }

    adw_dialog_close (overview->dialog);
    run_command (window, command);
}

static void
assign_key (Overview   *overview,
            const char *accel)
{
    const Command *command = overview->recording;
    g_auto (GStrv) keys = command_keys (command, NULL);
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();

    /* Taken from any other command that had it. */
    if (accel != NULL)
    {
        for (guint i = 0; i < G_N_ELEMENTS (commands); i++)
        {
            g_auto (GStrv) other = NULL;
            g_autoptr (GStrvBuilder) rest = NULL;
            gboolean had = FALSE;

            if (&commands[i] == command)
            {
                continue;
            }

            other = command_keys (&commands[i], NULL);
            rest = g_strv_builder_new ();
            for (guint k = 0; other[k] != NULL; k++)
            {
                if (same_accel (other[k], accel))
                {
                    had = TRUE;
                }
                else
                {
                    g_strv_builder_add (rest, other[k]);
                }
            }

            if (had)
            {
                g_auto (GStrv) left = g_strv_builder_end (rest);
                g_autofree char *message = g_strdup_printf (_("Taken from “%s”"),
                                                            _(commands[i].title));

                set_command_keys (&commands[i], (const char * const *) left);
                adw_toast_overlay_add_toast (overview->toasts, adw_toast_new (message));
            }
        }
    }

    for (int k = 0; keys[k] != NULL; k++)
    {
        if (k == overview->recording_slot)
        {
            if (accel != NULL)
            {
                g_strv_builder_add (builder, accel);
            }
        }
        else if (accel == NULL || !same_accel (keys[k], accel))
        {
            g_strv_builder_add (builder, keys[k]);
        }
    }
    if (overview->recording_slot < 0 && accel != NULL)
    {
        g_strv_builder_add (builder, accel);
    }

    g_auto (GStrv) result = g_strv_builder_end (builder);

    set_command_keys (command, (const char * const *) result);
}

static gboolean
on_recorder_key (GtkEventControllerKey *controller,
                 guint                  keyval,
                 guint                  keycode,
                 GdkModifierType        state,
                 Overview              *overview)
{
    GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();
    GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (overview->dialog));
    guint key;

    /* What the key is without Shift's effect, as accelerators are written. */
    if (!gdk_display_translate_key (display, keycode, state & ~GDK_SHIFT_MASK,
                                    gtk_event_controller_key_get_group (controller),
                                    &key, NULL, NULL, NULL))
    {
        key = keyval;
    }
    key = gdk_keyval_to_lower (key);

    switch (keyval)
    {
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R:
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R:
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
        case GDK_KEY_Super_L:
        case GDK_KEY_Super_R:
        case GDK_KEY_Meta_L:
        case GDK_KEY_Meta_R:
        case GDK_KEY_ISO_Level3_Shift:
        {
            return GDK_EVENT_STOP;
        }

        case GDK_KEY_Escape:
        {
            if (mods == 0)
            {
                adw_dialog_close (overview->recorder);
                return GDK_EVENT_STOP;
            }
        }
        break;

        case GDK_KEY_BackSpace:
        {
            if (mods == 0)
            {
                /* Removes the key being replaced. */
                if (overview->recording_slot >= 0)
                {
                    assign_key (overview, NULL);
                }
                adw_dialog_close (overview->recorder);
                return GDK_EVENT_STOP;
            }
        }
        break;

        default:
        {
        }
        break;
    }

    g_autofree char *accel = gtk_accelerator_name (key, mods);

    assign_key (overview, accel);
    adw_dialog_close (overview->recorder);

    return GDK_EVENT_STOP;
}

static void
record_key (Overview      *overview,
            const Command *command,
            int            slot)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *title = gtk_label_new (_(command->title));
    GtkWidget *hint = gtk_label_new (slot >= 0
                                     ? _("Press the new keys. Esc cancels, Backspace removes the key.")
                                     : _("Press the keys to add. Esc cancels."));
    GtkWidget *toolbar = adw_toolbar_view_new ();
    GtkEventController *keys = gtk_event_controller_key_new ();

    overview->recording = command;
    overview->recording_slot = slot;
    overview->recorder = adw_dialog_new ();
    adw_dialog_set_title (overview->recorder, _("New Shortcut"));
    adw_dialog_set_content_width (overview->recorder, 360);

    gtk_widget_add_css_class (title, "title-3");
    gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
    gtk_label_set_justify (GTK_LABEL (hint), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_widget_set_margin_start (box, 24);
    gtk_widget_set_margin_end (box, 24);
    gtk_widget_set_margin_top (box, 12);
    gtk_widget_set_margin_bottom (box, 24);
    gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("preferences-desktop-keyboard-shortcuts-symbolic"));
    gtk_box_append (GTK_BOX (box), title);
    gtk_box_append (GTK_BOX (box), hint);
    gtk_image_set_pixel_size (GTK_IMAGE (gtk_widget_get_first_child (box)), 48);

    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
    adw_dialog_set_child (overview->recorder, toolbar);

    /* Every key, before the dialog's own (Esc would just close it). */
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_recorder_key), overview);
    gtk_widget_add_controller (GTK_WIDGET (overview->recorder), keys);

    adw_dialog_present (overview->recorder, GTK_WIDGET (overview->dialog));
    gtk_widget_grab_focus (GTK_WIDGET (overview->recorder));
}

static void
on_key_clicked (GtkButton *button,
                Overview  *overview)
{
    record_key (overview,
                g_object_get_data (G_OBJECT (button), "command"),
                GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "slot")));
}

static void
on_key_secondary (GtkGestureClick *gesture,
                  int              n_press,
                  double           x,
                  double           y,
                  Overview        *overview)
{
    GtkWidget *button = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

    /* Right-click removes a key. */
    overview->recording = g_object_get_data (G_OBJECT (button), "command");
    overview->recording_slot = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "slot"));
    assign_key (overview, NULL);
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_reset_clicked (GtkButton *button,
                  Overview  *overview)
{
    set_command_keys (g_object_get_data (G_OBJECT (button), "command"), NULL);
}

static void
on_reset_all (Overview *overview)
{
    g_settings_reset (nautilus_flier_settings (), "custom-shortcuts");
    adw_toast_overlay_add_toast (overview->toasts, adw_toast_new (_("All shortcuts reset")));
}

static GtkWidget *
key_button (Overview      *overview,
            const Command *command,
            const char    *accel,
            int            slot)
{
    GtkWidget *button = gtk_button_new ();
    GtkWidget *label = gtk_shortcut_label_new (accel);
    GtkGesture *secondary = gtk_gesture_click_new ();

    gtk_button_set_child (GTK_BUTTON (button), label);
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_add_css_class (button, "shortcut-key");
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (button, _("Change (right-click removes)"));
    g_object_set_data (G_OBJECT (button), "command", (gpointer) command);
    g_object_set_data (G_OBJECT (button), "slot", GINT_TO_POINTER (slot));
    g_signal_connect (button, "clicked", G_CALLBACK (on_key_clicked), overview);

    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (secondary), GDK_BUTTON_SECONDARY);
    g_signal_connect (secondary, "pressed", G_CALLBACK (on_key_secondary), overview);
    gtk_widget_add_controller (button, GTK_EVENT_CONTROLLER (secondary));

    return button;
}

static GtkWidget *
command_row (Overview      *overview,
             const Command *command)
{
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *title = gtk_label_new (_(command->title));
    GtkWidget *button;
    gboolean changed;
    g_auto (GStrv) keys = command_keys (command, &changed);
    g_autoptr (GString) search = g_string_new (_(command->title));

    g_string_append_printf (search, " %s %s", _(command->section), command->action);

    gtk_label_set_xalign (GTK_LABEL (title), 0);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_box_append (GTK_BOX (box), title);

    if (changed)
    {
        GtkWidget *dot = gtk_image_new_from_icon_name ("document-edit-symbolic");

        gtk_widget_add_css_class (dot, "accent");
        gtk_widget_set_tooltip_text (dot, _("Changed"));
        gtk_box_append (GTK_BOX (box), dot);
    }

    for (int k = 0; keys[k] != NULL; k++)
    {
        g_autofree char *label = accel_label (keys[k]);

        g_string_append_printf (search, " %s", label);
        gtk_box_append (GTK_BOX (box), key_button (overview, command, keys[k], k));
    }

    button = gtk_button_new_from_icon_name ("list-add-symbolic");
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_add_css_class (button, "circular");
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (button, _("Add Shortcut"));
    g_object_set_data (G_OBJECT (button), "command", (gpointer) command);
    g_object_set_data (G_OBJECT (button), "slot", GINT_TO_POINTER (-1));
    g_signal_connect (button, "clicked", G_CALLBACK (on_key_clicked), overview);
    gtk_box_append (GTK_BOX (box), button);

    button = gtk_button_new_from_icon_name ("edit-undo-symbolic");
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_add_css_class (button, "circular");
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (button, _("Reset"));
    gtk_widget_set_opacity (button, changed ? 1 : 0);
    gtk_widget_set_sensitive (button, changed);
    g_object_set_data (G_OBJECT (button), "command", (gpointer) command);
    g_signal_connect (button, "clicked", G_CALLBACK (on_reset_clicked), overview);
    gtk_box_append (GTK_BOX (box), button);

    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 6);
    gtk_widget_set_margin_top (box, 3);
    gtk_widget_set_margin_bottom (box, 3);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_widget_set_tooltip_text (row, _("Enter runs it"));

    g_object_set_data (G_OBJECT (row), "command", (gpointer) command);
    g_object_set_data (G_OBJECT (row), "section", (gpointer) _(command->section));
    g_object_set_data_full (G_OBJECT (row), "search", g_string_free (g_steal_pointer (&search), FALSE), g_free);

    return row;
}

static GtkWidget *
basic_row (guint i)
{
    GtkWidget *row = adw_action_row_new ();
    GtkWidget *keys = gtk_label_new (_(basics[i].keys));
    g_autofree char *search = g_strdup_printf ("%s %s", _(basics[i].title), _(basics[i].keys));

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _(basics[i].title));
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
    gtk_widget_add_css_class (keys, "dim-label");
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), keys);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);

    g_object_set_data (G_OBJECT (row), "section", (gpointer) _("Mouse and Quick Look"));
    g_object_set_data_full (G_OBJECT (row), "search", g_steal_pointer (&search), g_free);

    return row;
}

/* ---------------------------------------------------- Go To rows */

static GtkWidget *
place_row (GFile      *location,
           const char *name,
           const char *icon_name,
           guint       order)
{
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title = gtk_label_new (name);
    g_autofree char *path = g_file_get_parse_name (location);
    GtkWidget *where = gtk_label_new (path);
    g_autofree char *search = g_strdup_printf ("%s %s", name, path);

    gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name (icon_name));
    gtk_label_set_xalign (GTK_LABEL (title), 0);
    gtk_box_append (GTK_BOX (box), title);
    gtk_label_set_xalign (GTK_LABEL (where), 1);
    gtk_label_set_ellipsize (GTK_LABEL (where), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand (where, TRUE);
    gtk_widget_add_css_class (where, "dim-label");
    gtk_box_append (GTK_BOX (box), where);

    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 9);
    gtk_widget_set_margin_bottom (box, 9);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);

    g_object_set_data_full (G_OBJECT (row), "location", g_object_ref (location), g_object_unref);
    g_object_set_data (G_OBJECT (row), "section", (gpointer) _("Go To"));
    g_object_set_data (G_OBJECT (row), "order", GUINT_TO_POINTER (order));
    g_object_set_data_full (G_OBJECT (row), "search", g_steal_pointer (&search), g_free);

    return row;
}

static gint
by_modified (gconstpointer a,
             gconstpointer b)
{
    GDateTime *ta = gtk_recent_info_get_modified ((GtkRecentInfo *) a);
    GDateTime *tb = gtk_recent_info_get_modified ((GtkRecentInfo *) b);

    return -g_date_time_compare (ta, tb);
}

/* Home and the standard folders, bookmarks, then folders of recent files. */
static void
append_places (Overview *overview,
               guint    *order)
{
    static const GUserDirectory dirs[] =
    {
        G_USER_DIRECTORY_DESKTOP, G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_DOWNLOAD,
        G_USER_DIRECTORY_MUSIC, G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_VIDEOS,
    };
    g_autoptr (GHashTable) seen = g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
                                                         g_object_unref, NULL);
    g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());
    NautilusBookmarkList *bookmarks = nautilus_application_get_bookmarks (nautilus_application_get_default ());
    GList *recent = gtk_recent_manager_get_items (gtk_recent_manager_get_default ());
    guint recent_folders = 0;

#define ADD_PLACE(file, name, icon) \
        G_STMT_START { \
            if (!g_hash_table_contains (seen, (file))) \
            { \
                g_hash_table_add (seen, g_object_ref (file)); \
                gtk_list_box_append (overview->list, place_row ((file), (name), (icon), (*order)++)); \
            } \
        } G_STMT_END

    ADD_PLACE (home, _("Home"), "user-home-symbolic");
    for (guint i = 0; i < G_N_ELEMENTS (dirs); i++)
    {
        const char *path = g_get_user_special_dir (dirs[i]);

        if (path != NULL && g_strcmp0 (path, g_get_home_dir ()) != 0)
        {
            g_autoptr (GFile) file = g_file_new_for_path (path);
            g_autofree char *name = g_file_get_basename (file);

            ADD_PLACE (file, name, "folder-symbolic");
        }
    }

    for (GList *l = nautilus_bookmark_list_get_all (bookmarks); l != NULL; l = l->next)
    {
        NautilusBookmark *bookmark = l->data;

        ADD_PLACE (nautilus_bookmark_get_location (bookmark), nautilus_bookmark_get_name (bookmark),
                   "user-bookmarks-symbolic");
    }

    recent = g_list_sort (recent, by_modified);
    for (GList *l = recent; l != NULL && recent_folders < 8; l = l->next)
    {
        const char *uri = gtk_recent_info_get_uri (l->data);
        g_autoptr (GFile) file = NULL;
        g_autoptr (GFile) parent = NULL;
        g_autofree char *name = NULL;

        if (!g_str_has_prefix (uri, "file://"))
        {
            continue;
        }

        file = g_file_new_for_uri (uri);
        parent = g_file_get_parent (file);
        if (parent == NULL || g_hash_table_contains (seen, parent))
        {
            continue;
        }

        name = g_file_get_basename (parent);
        ADD_PLACE (parent, name, "document-open-recent-symbolic");
        recent_folders++;
    }
    g_list_free_full (recent, (GDestroyNotify) gtk_recent_info_unref);

#undef ADD_PLACE
}

static void
rebuild_rows (Overview *overview)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child (GTK_WIDGET (overview->list))) != NULL)
    {
        gtk_list_box_remove (overview->list, child);
    }

    guint order = 0;

    for (guint i = 0; i < G_N_ELEMENTS (commands); i++)
    {
        GtkWidget *row = command_row (overview, &commands[i]);

        g_object_set_data (G_OBJECT (row), "order", GUINT_TO_POINTER (order++));
        gtk_list_box_append (overview->list, row);
    }
    append_places (overview, &order);
    for (guint i = 0; i < G_N_ELEMENTS (basics); i++)
    {
        GtkWidget *row = basic_row (i);

        g_object_set_data (G_OBJECT (row), "order", GUINT_TO_POINTER (order++));
        gtk_list_box_append (overview->list, row);
    }

    gtk_list_box_invalidate_filter (overview->list);
    gtk_list_box_invalidate_sort (overview->list);
}

static gboolean
searching (Overview *overview)
{
    const char *query = gtk_editable_get_text (GTK_EDITABLE (overview->search));

    return query != NULL && *query != '\0';
}

static gboolean
runnable (GtkListBoxRow *row)
{
    return g_object_get_data (G_OBJECT (row), "command") != NULL ||
           g_object_get_data (G_OBJECT (row), "location") != NULL;
}

/* While searching: names that start with what was typed first, then
 * names with a word that does, then the rest (matching the key, say). */
static guint
rank (GtkListBoxRow *row,
      const char    *query)
{
    const char *search = g_object_get_data (G_OBJECT (row), "search");
    g_autofree char *folded_query = g_utf8_casefold (query, -1);
    g_autofree char *folded = g_utf8_casefold (search, -1);
    guint score = 3;

    if (g_str_has_prefix (folded, folded_query))
    {
        score = 0;
    }
    else if (g_str_match_string (query, search, TRUE))
    {
        score = 1;
    }

    /* Things to run before the mouse list. */
    return score * 2 + (runnable (row) ? 0 : 1);
}

static int
sort_rows (GtkListBoxRow *a,
           GtkListBoxRow *b,
           gpointer       user_data)
{
    Overview *overview = user_data;
    guint oa = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (a), "order"));
    guint ob = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (b), "order"));

    if (searching (overview))
    {
        const char *query = gtk_editable_get_text (GTK_EDITABLE (overview->search));
        guint ra = rank (a, query);
        guint rb = rank (b, query);

        if (ra != rb)
        {
            return ra < rb ? -1 : 1;
        }
    }

    return oa < ob ? -1 : (oa > ob ? 1 : 0);
}

static gboolean
filter_row (GtkListBoxRow *row,
            gpointer       user_data)
{
    Overview *overview = user_data;
    const char *query = gtk_editable_get_text (GTK_EDITABLE (overview->search));
    const char *text = g_object_get_data (G_OBJECT (row), "search");
    g_auto (GStrv) words = NULL;

    if (query == NULL || *query == '\0')
    {
        return TRUE;
    }

    /* Every word typed matches the start of a word of the row. */
    words = g_str_tokenize_and_fold (query, NULL, NULL);
    for (guint i = 0; words[i] != NULL; i++)
    {
        if (!g_str_match_string (words[i], text, TRUE))
        {
            return FALSE;
        }
    }

    return TRUE;
}

static void
header_row (GtkListBoxRow *row,
            GtkListBoxRow *before,
            gpointer       user_data)
{
    Overview *overview = user_data;

    /* Ranked by match while searching; sections would repeat. */
    if (searching (overview))
    {
        gtk_list_box_row_set_header (row, NULL);
        return;
    }

    const char *section = g_object_get_data (G_OBJECT (row), "section");
    const char *previous = before != NULL ? g_object_get_data (G_OBJECT (before), "section") : NULL;

    if (g_strcmp0 (section, previous) == 0)
    {
        gtk_list_box_row_set_header (row, NULL);
        return;
    }

    GtkWidget *label = gtk_label_new (section);

    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_widget_add_css_class (label, "heading");
    gtk_widget_set_margin_start (label, 12);
    gtk_widget_set_margin_top (label, before != NULL ? 18 : 6);
    gtk_widget_set_margin_bottom (label, 6);
    gtk_list_box_row_set_header (row, label);
}

static GtkListBoxRow *
next_runnable (Overview      *overview,
               GtkListBoxRow *from,
               gboolean       forward)
{
    GtkWidget *child;

    if (from == NULL)
    {
        child = gtk_widget_get_first_child (GTK_WIDGET (overview->list));
    }
    else
    {
        child = forward ? gtk_widget_get_next_sibling (GTK_WIDGET (from))
                        : gtk_widget_get_prev_sibling (GTK_WIDGET (from));
    }

    for (; child != NULL;
         child = forward ? gtk_widget_get_next_sibling (child) : gtk_widget_get_prev_sibling (child))
    {
        if (GTK_IS_LIST_BOX_ROW (child) && gtk_widget_get_child_visible (child) &&
            runnable (GTK_LIST_BOX_ROW (child)))
        {
            return GTK_LIST_BOX_ROW (child);
        }
    }

    return NULL;
}

static void
select_row (Overview      *overview,
            GtkListBoxRow *row)
{
    if (row == NULL)
    {
        gtk_list_box_unselect_all (overview->list);
        return;
    }

    gtk_list_box_select_row (overview->list, row);
    /* Keeps the row in view without taking focus from the entry. */
    {
        GtkWidget *scroller = gtk_widget_get_ancestor (GTK_WIDGET (row), GTK_TYPE_SCROLLED_WINDOW);
        graphene_rect_t bounds;

        if (scroller != NULL &&
            gtk_widget_compute_bounds (GTK_WIDGET (row), scroller, &bounds))
        {
            GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));
            double page = gtk_adjustment_get_page_size (adj);
            double value = gtk_adjustment_get_value (adj);

            if (bounds.origin.y < 0)
            {
                gtk_adjustment_set_value (adj, value + bounds.origin.y - 6);
            }
            else if (bounds.origin.y + bounds.size.height > page)
            {
                gtk_adjustment_set_value (adj, value + bounds.origin.y + bounds.size.height - page + 6);
            }
        }
    }
}

static gboolean
select_first_idle (gpointer user_data)
{
    Overview *overview = user_data;

    overview->select_idle = 0;
    select_row (overview, searching (overview) ? next_runnable (overview, NULL, TRUE) : NULL);
    return G_SOURCE_REMOVE;
}

static void
on_search_changed (GtkSearchEntry *entry,
                   Overview       *overview)
{
    gtk_list_box_invalidate_filter (overview->list);
    gtk_list_box_invalidate_sort (overview->list);
    gtk_list_box_invalidate_headers (overview->list);
    /* Once the rows are sorted and laid out. */
    if (overview->select_idle == 0)
    {
        overview->select_idle = g_idle_add (select_first_idle, overview);
    }
}

static void
on_search_activate (GtkSearchEntry *entry,
                    Overview       *overview)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row (overview->list);

    if (row == NULL)
    {
        row = next_runnable (overview, NULL, TRUE);
    }
    if (row != NULL)
    {
        on_row_activated (overview->list, row, overview);
    }
}

/* Up and Down pick the command Enter runs; Tab goes into the list. */
static gboolean
on_search_key (GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode,
               GdkModifierType        state,
               Overview              *overview)
{
    GtkListBoxRow *selected = gtk_list_box_get_selected_row (overview->list);

    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Up)
    {
        gboolean down = keyval == GDK_KEY_Down;
        GtkListBoxRow *row = selected != NULL ? next_runnable (overview, selected, down)
                                              : next_runnable (overview, NULL, TRUE);

        if (row != NULL)
        {
            select_row (overview, row);
        }
        return GDK_EVENT_STOP;
    }

    if (keyval == GDK_KEY_Tab)
    {
        GtkListBoxRow *row = selected != NULL ? selected : next_runnable (overview, NULL, TRUE);

        if (row != NULL)
        {
            gtk_widget_grab_focus (GTK_WIDGET (row));
            return GDK_EVENT_STOP;
        }
    }

    return GDK_EVENT_PROPAGATE;
}

/* Esc closes at once, also with text typed; F1 again too. */
static gboolean
on_dialog_key (GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode,
               GdkModifierType        state,
               Overview              *overview)
{
    if ((state & gtk_accelerator_get_default_mod_mask ()) == 0 &&
        (keyval == GDK_KEY_Escape || keyval == GDK_KEY_F1))
    {
        adw_dialog_close (overview->dialog);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

void
nautilus_shortcuts_dialog_present (GtkWindow *window)
{
    Overview *overview = g_new0 (Overview, 1);
    GtkWidget *toolbar = adw_toolbar_view_new ();
    GtkWidget *header = adw_header_bar_new ();
    GtkWidget *scroller = gtk_scrolled_window_new ();
    GtkWidget *clamp = adw_clamp_new ();
    GtkWidget *reset = gtk_button_new_with_mnemonic (_("_Reset All"));
    GtkEventController *keys = gtk_event_controller_key_new ();

    overview->window = window;
    overview->dialog = adw_dialog_new ();
    adw_dialog_set_title (overview->dialog, _("Commands"));
    adw_dialog_set_content_width (overview->dialog, 640);
    adw_dialog_set_content_height (overview->dialog, 720);

    overview->search = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
    gtk_search_entry_set_placeholder_text (overview->search, _("Run a command, go to a folder, find a key"));
    gtk_widget_set_hexpand (GTK_WIDGET (overview->search), TRUE);
    gtk_widget_set_size_request (GTK_WIDGET (overview->search), 320, -1);
    g_signal_connect (overview->search, "search-changed", G_CALLBACK (on_search_changed), overview);
    g_signal_connect (overview->search, "activate", G_CALLBACK (on_search_activate), overview);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_search_key), overview);
    gtk_widget_add_controller (GTK_WIDGET (overview->search), keys);
    adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (overview->search));

    gtk_widget_set_tooltip_text (reset, _("Back to the keys Files comes with"));
    g_signal_connect_swapped (reset, "clicked", G_CALLBACK (on_reset_all), overview);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), reset);

    overview->list = GTK_LIST_BOX (gtk_list_box_new ());
    gtk_list_box_set_selection_mode (overview->list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class (GTK_WIDGET (overview->list), "boxed-list-separate");
    gtk_widget_add_css_class (GTK_WIDGET (overview->list), "shortcuts-overview");
    gtk_list_box_set_filter_func (overview->list, filter_row, overview, NULL);
    gtk_list_box_set_header_func (overview->list, header_row, overview, NULL);
    gtk_list_box_set_sort_func (overview->list, sort_rows, overview, NULL);
    g_signal_connect (overview->list, "row-activated", G_CALLBACK (on_row_activated), overview);

    gtk_widget_set_margin_start (GTK_WIDGET (overview->list), 12);
    gtk_widget_set_margin_end (GTK_WIDGET (overview->list), 12);
    gtk_widget_set_margin_bottom (GTK_WIDGET (overview->list), 18);
    adw_clamp_set_child (ADW_CLAMP (clamp), GTK_WIDGET (overview->list));
    adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 720);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), clamp);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scroller, TRUE);

    overview->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
    adw_toast_overlay_set_child (overview->toasts, scroller);

    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (overview->toasts));
    adw_dialog_set_child (overview->dialog, toolbar);

    keys = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (on_dialog_key), overview);
    gtk_widget_add_controller (GTK_WIDGET (overview->dialog), keys);

    /* Typing anywhere searches. */
    gtk_search_entry_set_key_capture_widget (overview->search, GTK_WIDGET (overview->dialog));

    rebuild_rows (overview);
    g_object_set_data_full (G_OBJECT (overview->dialog), "overview", overview, (GDestroyNotify) overview_free);
    g_signal_connect_object (nautilus_flier_settings (), "changed::custom-shortcuts",
                             G_CALLBACK (on_custom_changed), overview->dialog, 0);

    adw_dialog_present (overview->dialog, GTK_WIDGET (window));
    gtk_widget_grab_focus (GTK_WIDGET (overview->search));
}
