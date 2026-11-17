/*
 * nautilus-jump.c: the jump menu under the location entry
 *
 * Typing a name in the path bar (Ctrl+E, or Ctrl+L and a name instead of a
 * path) lists the folders of that name anywhere in the index, each with
 * its path to click into, the commands and the settings that match. Tab
 * goes through All, Folders, Commands and Settings. A path ("/…", "~…")
 * hides the menu and works as before.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-jump.h"

#include <glib/gi18n.h>

#include "nautilus-application.h"
#include "nautilus-bookmark.h"
#include "nautilus-bookmark-list.h"
#include "nautilus-global-preferences.h"
#include "nautilus-labels.h"
#include "nautilus-localsearch-utilities.h"
#include "nautilus-shortcuts.h"

typedef enum
{
    SCOPE_ALL,
    SCOPE_FOLDERS,
    SCOPE_COMMANDS,
    SCOPE_SETTINGS,
    N_SCOPES
} Scope;

typedef enum
{
    ROW_FOLDER = 1,
    ROW_COMMAND,
    ROW_SETTING,
    ROW_NONE,
} RowKind;

enum
{
    SCHEMA_FLIER,
    SCHEMA_NAUTILUS,
    SCHEMA_CHOOSER,
};

typedef struct
{
    const char *title;
    const char *words;   /* more to find it by */
    int schema;
    const char *key;     /* NULL: opens the Preferences */
    const char *choices; /* for a string: "value=Label|…" */
} Setting;

#define SPEED N_("local-only=This Device|always=All|never=Never")
#define DENSITY N_("comfortable=Comfortable|compact=Compact|dense=Dense|extra-dense=Extra")

static const Setting settings[] =
{
    { N_("Show Hidden Files"), "dot files", SCHEMA_CHOOSER, "show-hidden", NULL },
    { N_("Folders Before Files"), "sort first", SCHEMA_CHOOSER, "sort-directories-first", NULL },
    { N_("Click to Open"), "single double mouse", SCHEMA_NAUTILUS, "click-policy", N_("single=Single|double=Double") },
    { N_("Density"), "rows icons size compact", SCHEMA_FLIER, "density", DENSITY },
    { N_("Sidebar Density"), "rows size compact", SCHEMA_FLIER, "sidebar-density", DENSITY },
    { N_("Inspector"), "details panel info", SCHEMA_FLIER, "inspector-visible", NULL },
    { N_("Inspector Density"), "details compact", SCHEMA_FLIER, "inspector-density",
      N_("comfortable=Comfortable|compact=Compact|dense=Dense") },
    { N_("Inspector Side"), "details dock panel", SCHEMA_FLIER, "inspector-side",
      N_("left=Left|right=Right|top=Top|bottom=Bottom|window=Window") },
    { N_("Bottom Bar"), "filter status", SCHEMA_FLIER, "bottom-bar", NULL },
    { N_("Columns View"), "miller", SCHEMA_FLIER, "columns-view", NULL },
    { N_("Tree View for New Tabs"), "expand list", SCHEMA_FLIER, "tree-view", NULL },
    { N_("Ratings"), "stars", SCHEMA_FLIER, "ratings", NULL },
    { N_("Tags"), "labels", SCHEMA_FLIER, "tags", NULL },
    { N_("Ratings and Tags on Grid Items"), "stars tiles", SCHEMA_FLIER, "marks-on-tiles", NULL },
    { N_("Stars in Columns"), "ratings miller", SCHEMA_FLIER, "columns-stars", NULL },
    { N_("Folder Colors"), "colours", SCHEMA_FLIER, "folder-colors", NULL },
    { N_("Folder Color Scheme"), "colours palette", SCHEMA_FLIER, "folder-color-scheme",
      N_("pastel=Pastel|vibrant=Vibrant|earth=Earth|ocean=Ocean|mono=Mono") },
    { N_("Window Opacity"), "transparent transparency blur", SCHEMA_FLIER, "window-opacity", NULL },
    { N_("Built-in Quick Look"), "preview", SCHEMA_FLIER, "own-quick-look", NULL },
    { N_("Quick Look Names"), "preview", SCHEMA_FLIER, "quick-look-names", NULL },
    { N_("Quick Look Ratings and Tags"), "preview stars", SCHEMA_FLIER, "quick-look-marks", NULL },
    { N_("Thumbnails"), "previews images", SCHEMA_NAUTILUS, "show-image-thumbnails", SPEED },
    { N_("Folder Item Counts"), "count size", SCHEMA_NAUTILUS, "show-directory-item-counts", SPEED },
    { N_("Search in Subfolders"), "recursive", SCHEMA_NAUTILUS, "recursive-search", SPEED },
    { N_("Dates"), "time format detailed", SCHEMA_NAUTILUS, "date-time-format",
      N_("simple=Simple|detailed=Detailed") },
    { N_("Sidebar Groups"), "sections fold", SCHEMA_FLIER, "sidebar-groups", NULL },
    { N_("Storage Usage in the Sidebar"), "disk drive space", SCHEMA_FLIER, "sidebar-storage-usage", NULL },
    { N_("Progress Panel"), "copy move", SCHEMA_FLIER, "progress-popup", NULL },
    { N_("Progress Panel in Its Own Window"), "copy move popup", SCHEMA_FLIER, "progress-popup-window", NULL },
    { N_("Copy One After Another"), "queue move", SCHEMA_FLIER, "queue-transfers", NULL },
    { N_("Delete Permanently in Menus"), "trash", SCHEMA_NAUTILUS, "show-delete-permanently", NULL },
    { N_("Create Link in Menus"), "symlink", SCHEMA_NAUTILUS, "show-create-link", NULL },
    { N_("Open Folders While Dragging"), "drag drop hover", SCHEMA_NAUTILUS, "open-folder-on-dnd-hover", NULL },
    { N_("Shortcuts in Menus"), "keys", SCHEMA_FLIER, "menu-shortcuts", NULL },
    { N_("Shortcut Hints"), "keys", SCHEMA_FLIER, "shortcut-hints", NULL },
    { N_("Run in the Background"), "startup start", SCHEMA_FLIER, "run-in-background", NULL },
    { N_("All Preferences"), "settings options", 0, NULL, NULL },
};

typedef struct
{
    GFile *file;
    char *name;
    const char *icon;
    gboolean place;
} Folder;

struct _NautilusJump
{
    GtkWidget *entry;
    GtkWidget *anchor;
    GtkWidget *popover;
    GtkWidget *scopes[N_SCOPES];
    GtkListBox *list;
    GtkScrolledWindow *scroller;
    NautilusWindowSlot *slot;
    Scope scope;
    char *query;

    /* Folders from the index for found_for. */
    GPtrArray *found;
    char *found_for;
    TrackerSparqlStatement *statement;
    GCancellable *cancellable;
    guint search_timeout;
    guint size_idle;

    /* The folders in the current folder, read once per folder. */
    GFile *here;
    GPtrArray *here_folders;
};

static void rebuild (NautilusJump *jump);

static GSettings *
schema_settings (int schema)
{
    switch (schema)
    {
        case SCHEMA_NAUTILUS:
        {
            return nautilus_preferences;
        }

        case SCHEMA_CHOOSER:
        {
            return gtk_filechooser_preferences;
        }

        default:
        {
            return nautilus_flier_settings ();
        }
    }
}

static void
folder_free (Folder *folder)
{
    g_object_unref (folder->file);
    g_free (folder->name);
    g_free (folder);
}

static Folder *
folder_new (GFile      *file,
            const char *name,
            const char *icon,
            gboolean    place)
{
    Folder *folder = g_new0 (Folder, 1);

    folder->file = g_object_ref (file);
    folder->name = name != NULL ? g_strdup (name) : g_file_get_basename (file);
    folder->icon = icon;
    folder->place = place;
    return folder;
}

gboolean
nautilus_jump_is_path (const char *text)
{
    while (text != NULL && g_ascii_isspace (*text))
    {
        text++;
    }

    return text != NULL &&
           (text[0] == '/' || text[0] == '~' || text[0] == '.' || text[0] == '\\' ||
            strstr (text, "://") != NULL || g_str_has_prefix (text, "file:"));
}

/* Every word typed starts a word of @haystack. */
static gboolean
matches (const char *query,
         const char *haystack)
{
    g_auto (GStrv) words = g_str_tokenize_and_fold (query, NULL, NULL);

    for (guint i = 0; words[i] != NULL; i++)
    {
        if (!g_str_match_string (words[i], haystack, TRUE))
        {
            return FALSE;
        }
    }

    return TRUE;
}

/* --------------------------------------------------------- Folders */

static GPtrArray *
places (void)
{
    static const GUserDirectory dirs[] =
    {
        G_USER_DIRECTORY_DESKTOP, G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_DOWNLOAD,
        G_USER_DIRECTORY_MUSIC, G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_VIDEOS,
    };
    GPtrArray *result = g_ptr_array_new_with_free_func ((GDestroyNotify) folder_free);
    g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());
    NautilusBookmarkList *bookmarks = nautilus_application_get_bookmarks (nautilus_application_get_default ());
    GList *recent = gtk_recent_manager_get_items (gtk_recent_manager_get_default ());
    guint recent_folders = 0;

    g_ptr_array_add (result, folder_new (home, _("Home"), "user-home-symbolic", TRUE));
    for (guint i = 0; i < G_N_ELEMENTS (dirs); i++)
    {
        const char *path = g_get_user_special_dir (dirs[i]);

        if (path != NULL && g_strcmp0 (path, g_get_home_dir ()) != 0)
        {
            g_autoptr (GFile) file = g_file_new_for_path (path);

            g_ptr_array_add (result, folder_new (file, NULL, "folder-symbolic", TRUE));
        }
    }

    for (GList *l = nautilus_bookmark_list_get_all (bookmarks); l != NULL; l = l->next)
    {
        NautilusBookmark *bookmark = l->data;

        g_ptr_array_add (result, folder_new (nautilus_bookmark_get_location (bookmark),
                                             nautilus_bookmark_get_name (bookmark),
                                             "user-bookmarks-symbolic", TRUE));
    }

    /* Folders of recently used files, newest first. */
    for (GList *l = recent; l != NULL; l = l->next)
    {
        for (GList *m = l->next; m != NULL; m = m->next)
        {
            if (g_date_time_compare (gtk_recent_info_get_modified (m->data),
                                     gtk_recent_info_get_modified (l->data)) > 0)
            {
                gpointer swap = l->data;

                l->data = m->data;
                m->data = swap;
            }
        }
    }
    for (GList *l = recent; l != NULL && recent_folders < 6; l = l->next)
    {
        const char *uri = gtk_recent_info_get_uri (l->data);
        g_autoptr (GFile) file = NULL;
        g_autoptr (GFile) parent = NULL;
        gboolean seen = FALSE;

        if (!g_str_has_prefix (uri, "file://"))
        {
            continue;
        }

        file = g_file_new_for_uri (uri);
        parent = g_file_get_parent (file);
        for (guint i = 0; parent != NULL && i < result->len; i++)
        {
            seen = seen || g_file_equal (((Folder *) result->pdata[i])->file, parent);
        }
        if (parent == NULL || seen)
        {
            continue;
        }

        g_ptr_array_add (result, folder_new (parent, NULL, "document-open-recent-symbolic", TRUE));
        recent_folders++;
    }
    g_list_free_full (recent, (GDestroyNotify) gtk_recent_info_unref);

    return result;
}

static GPtrArray *
folders_here (NautilusJump *jump)
{
    GFile *location = jump->slot != NULL ? nautilus_window_slot_get_location (jump->slot) : NULL;
    g_autoptr (GFileEnumerator) children = NULL;

    if (location == NULL || !g_file_is_native (location))
    {
        return NULL;
    }
    if (jump->here != NULL && g_file_equal (jump->here, location))
    {
        return jump->here_folders;
    }

    g_set_object (&jump->here, location);
    g_clear_pointer (&jump->here_folders, g_ptr_array_unref);
    jump->here_folders = g_ptr_array_new_with_free_func ((GDestroyNotify) folder_free);

    children = g_file_enumerate_children (location,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                          G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
                                          G_FILE_QUERY_INFO_NONE, NULL, NULL);
    for (guint n = 0; children != NULL && n < 2000; n++)
    {
        GFileInfo *info = NULL;
        GFile *child = NULL;

        if (!g_file_enumerator_iterate (children, &info, &child, NULL, NULL) || info == NULL)
        {
            break;
        }
        if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY && !g_file_info_get_is_hidden (info))
        {
            g_ptr_array_add (jump->here_folders, folder_new (child, NULL, "folder-symbolic", TRUE));
        }
    }

    return jump->here_folders;
}

/* Lower is better: the name as typed, then starting with it; places,
 * then ordinary folders, then hidden and app-data ones; shallow first. */
static int
folder_score (const Folder *folder,
              const char   *query)
{
    g_autofree char *name = g_utf8_casefold (folder->name, -1);
    g_autofree char *wanted = g_utf8_casefold (query, -1);
    g_autofree char *path = g_file_get_path (folder->file);
    int score;
    int depth = 0;

    g_strstrip (wanted);
    score = g_str_equal (name, wanted) ? 0 : g_str_has_prefix (name, wanted) ? 1000 : 2000;
    if (folder->place)
    {
        score -= 500;
    }
    if (path != NULL)
    {
        if (strstr (path, "/.") != NULL)
        {
            score += 4000;
        }
        if (strstr (path, "/snap/") != NULL || strstr (path, "/node_modules/") != NULL)
        {
            score += 3000;
        }
        for (const char *p = path; *p != '\0'; p++)
        {
            depth += *p == '/';
        }
    }

    return score + depth * 10;
}

static void
on_query_done (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (TrackerSparqlCursor) cursor = NULL;
    NautilusJump *jump = user_data;
    GPtrArray *found;

    cursor = tracker_sparql_statement_execute_finish (TRACKER_SPARQL_STATEMENT (source), result, &error);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        return;
    }
    if (cursor == NULL)
    {
        g_debug ("Jump: folder search failed: %s", error != NULL ? error->message : "");
        return;
    }

    found = g_ptr_array_new_with_free_func ((GDestroyNotify) folder_free);
    while (tracker_sparql_cursor_next (cursor, NULL, NULL))
    {
        g_autoptr (GFile) file = g_file_new_for_uri (tracker_sparql_cursor_get_string (cursor, 0, NULL));

        g_ptr_array_add (found, folder_new (file, NULL, "folder-symbolic", FALSE));
    }
    tracker_sparql_cursor_close (cursor);

    g_clear_pointer (&jump->found, g_ptr_array_unref);
    jump->found = found;
    g_free (jump->found_for);
    jump->found_for = g_strdup (jump->query);
    rebuild (jump);
}

static gboolean
search_folders (gpointer user_data)
{
    NautilusJump *jump = user_data;
    g_auto (GStrv) words = g_str_tokenize_and_fold (jump->query, NULL, NULL);
    g_autoptr (GString) match = g_string_new (NULL);

    jump->search_timeout = 0;

    for (guint i = 0; words[i] != NULL; i++)
    {
        /* Letters and digits only: the rest means something to the index. */
        g_autoptr (GString) word = g_string_new (NULL);

        for (const char *p = words[i]; *p != '\0'; p = g_utf8_next_char (p))
        {
            gunichar c = g_utf8_get_char (p);

            if (g_unichar_isalnum (c))
            {
                g_string_append_unichar (word, c);
            }
        }
        if (word->len > 0)
        {
            g_string_append_printf (match, "%s%s*", match->len > 0 ? " " : "", word->str);
        }
    }
    if (match->len == 0)
    {
        return G_SOURCE_REMOVE;
    }

    if (jump->statement == NULL)
    {
        TrackerSparqlConnection *connection = nautilus_localsearch_get_miner_fs_connection (NULL);

        if (connection == NULL)
        {
            return G_SOURCE_REMOVE;
        }
        jump->statement = tracker_sparql_connection_query_statement (
            connection,
            "SELECT DISTINCT ?url WHERE {"
            "  GRAPH tracker:FileSystem {"
            "    ?folder a nfo:Folder ; nie:isStoredAs ?file ."
            "    ?file nie:url ?url ; fts:match ~match ."
            "  }"
            "} LIMIT 300",
            NULL, NULL);
        if (jump->statement == NULL)
        {
            return G_SOURCE_REMOVE;
        }
    }

    g_cancellable_cancel (jump->cancellable);
    g_clear_object (&jump->cancellable);
    jump->cancellable = g_cancellable_new ();
    tracker_sparql_statement_bind_string (jump->statement, "match", match->str);
    tracker_sparql_statement_execute_async (jump->statement, jump->cancellable, on_query_done, jump);

    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------ Rows */

static GtkWindow *
jump_window (NautilusJump *jump)
{
    GtkRoot *root = gtk_widget_get_root (jump->entry);

    return GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL;
}

static void
close_entry (NautilusJump *jump)
{
    nautilus_jump_hide (jump);
    /* The toolbar closes the entry on its "cancel". */
    g_signal_emit_by_name (jump->entry, "cancel");
}

static void
open_folder (NautilusJump *jump,
             GFile        *file,
             gboolean      new_tab)
{
    g_autoptr (GFile) keep = g_object_ref (file);
    NautilusWindowSlot *slot = jump->slot;

    close_entry (jump);
    if (new_tab || slot == NULL)
    {
        nautilus_application_open_location_full (nautilus_application_get_default (), keep,
                                                 NAUTILUS_OPEN_FLAG_NEW_TAB, NULL, NULL);
    }
    else
    {
        nautilus_window_slot_open_location_full (slot, keep, NULL);
    }
}

static void
on_crumb_clicked (GtkButton    *button,
                  NautilusJump *jump)
{
    open_folder (jump, g_object_get_data (G_OBJECT (button), "file"), FALSE);
}

static GtkWidget *
crumb_button (NautilusJump *jump,
              GFile        *file,
              const char   *label)
{
    GtkWidget *button = gtk_button_new_with_label (label);

    gtk_widget_add_css_class (button, "flat");
    gtk_widget_add_css_class (button, "jump-crumb");
    gtk_widget_set_focus_on_click (button, FALSE);
    gtk_widget_set_focusable (button, FALSE);
    g_object_set_data_full (G_OBJECT (button), "file", g_object_ref (file), g_object_unref);
    g_signal_connect (button, "clicked", G_CALLBACK (on_crumb_clicked), jump);
    return button;
}

/* The folders above @file, each a button: "~ › Documents › Work". */
static GtkWidget *
crumbs (NautilusJump *jump,
        GFile        *file)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());
    g_autoptr (GFile) root = g_file_new_for_path ("/");
    g_autoptr (GPtrArray) chain = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr (GFile) at = g_file_get_parent (file);
    gboolean cut = FALSE;

    while (at != NULL)
    {
        GFile *parent = g_file_get_parent (at);

        g_ptr_array_insert (chain, 0, g_object_ref (at));
        if (g_file_equal (at, home))
        {
            g_clear_object (&parent);
        }
        g_object_unref (at);
        at = parent;
    }

    /* The nearest few, after where they start. */
    if (chain->len > 5)
    {
        g_ptr_array_remove_range (chain, 1, chain->len - 4);
        cut = TRUE;
    }

    gtk_widget_add_css_class (box, "jump-crumbs");
    for (guint i = 0; i < chain->len; i++)
    {
        GFile *step = chain->pdata[i];
        g_autofree char *name = NULL;

        if (g_file_equal (step, home))
        {
            name = g_strdup ("~");
        }
        else if (g_file_equal (step, root))
        {
            name = g_strdup ("/");
        }
        else
        {
            name = g_file_get_basename (step);
        }

        if (i > 0)
        {
            GtkWidget *sep = gtk_label_new (cut && i == 1 ? "› … ›" : "›");

            gtk_widget_add_css_class (sep, "jump-crumb-sep");
            gtk_box_append (GTK_BOX (box), sep);
        }
        gtk_box_append (GTK_BOX (box), crumb_button (jump, step, name));
    }

    return box;
}

static GtkWidget *
row_new (RowKind kind)
{
    GtkWidget *row = gtk_list_box_row_new ();

    gtk_widget_set_focusable (row, FALSE);
    gtk_widget_add_css_class (row, "jump-row");
    g_object_set_data (G_OBJECT (row), "kind", GINT_TO_POINTER (kind));
    return row;
}

static GtkWidget *
folder_row (NautilusJump *jump,
            Folder       *folder)
{
    GtkWidget *row = row_new (ROW_FOLDER);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *name = gtk_label_new (folder->name);

    gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name (folder->icon));
    gtk_label_set_xalign (GTK_LABEL (name), 0);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (name, "jump-name");
    gtk_box_append (GTK_BOX (text), name);
    gtk_box_append (GTK_BOX (text), crumbs (jump, folder->file));
    gtk_widget_set_hexpand (text, TRUE);
    gtk_box_append (GTK_BOX (box), text);

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_widget_set_tooltip_text (row, _("Enter opens it, Ctrl+Enter in a new tab"));
    g_object_set_data_full (G_OBJECT (row), "file", g_object_ref (folder->file), g_object_unref);
    return row;
}

static GtkWidget *
command_row (guint i)
{
    GtkWidget *row = row_new (ROW_COMMAND);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new (nautilus_shortcuts_command_title (i));
    GtkWidget *section = gtk_label_new (nautilus_shortcuts_command_section (i));
    g_auto (GStrv) keys = nautilus_shortcuts_command_keys (i);

    gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("system-run-symbolic"));
    gtk_label_set_xalign (GTK_LABEL (title), 0);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (box), title);
    gtk_label_set_xalign (GTK_LABEL (section), 0);
    gtk_widget_set_hexpand (section, TRUE);
    gtk_widget_add_css_class (section, "dim-label");
    gtk_widget_add_css_class (section, "caption");
    gtk_box_append (GTK_BOX (box), section);
    if (keys[0] != NULL)
    {
        gtk_box_append (GTK_BOX (box), gtk_shortcut_label_new (keys[0]));
    }

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    g_object_set_data (G_OBJECT (row), "index", GUINT_TO_POINTER (i));
    return row;
}

static void
on_choice_toggled (GtkToggleButton *button,
                   const Setting   *setting)
{
    GSettings *gsettings = schema_settings (setting->schema);
    const char *value = g_object_get_data (G_OBJECT (button), "value");
    g_autofree char *now = g_settings_get_string (gsettings, setting->key);

    if (gtk_toggle_button_get_active (button) && !g_str_equal (now, value))
    {
        g_settings_set_string (gsettings, setting->key, value);
    }
}

static void
sync_choices (GSettings  *gsettings,
              const char *key,
              GtkWidget  *box)
{
    g_autofree char *now = g_settings_get_string (gsettings, key);

    for (GtkWidget *b = gtk_widget_get_first_child (box); b != NULL; b = gtk_widget_get_next_sibling (b))
    {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b),
                                      g_str_equal (now, g_object_get_data (G_OBJECT (b), "value")));
    }
}

static GtkWidget *
setting_control (const Setting *setting)
{
    GSettings *gsettings = schema_settings (setting->schema);
    g_autoptr (GSettingsSchema) schema = NULL;
    g_autoptr (GSettingsSchemaKey) key = NULL;
    const GVariantType *type;

    if (setting->key == NULL)
    {
        return gtk_image_new_from_icon_name ("go-next-symbolic");
    }

    g_object_get (gsettings, "settings-schema", &schema, NULL);
    key = g_settings_schema_get_key (schema, setting->key);
    type = g_settings_schema_key_get_value_type (key);

    if (g_variant_type_equal (type, G_VARIANT_TYPE_BOOLEAN))
    {
        GtkWidget *toggle = gtk_switch_new ();

        gtk_widget_set_valign (toggle, GTK_ALIGN_CENTER);
        gtk_widget_set_focusable (toggle, FALSE);
        gtk_widget_set_focus_on_click (toggle, FALSE);
        g_settings_bind (gsettings, setting->key, toggle, "active", G_SETTINGS_BIND_DEFAULT);
        return toggle;
    }

    if (g_variant_type_equal (type, G_VARIANT_TYPE_INT32))
    {
        g_autoptr (GVariant) range = g_settings_schema_key_get_range (key);
        g_autoptr (GVariant) limits = NULL;
        const char *how = NULL;
        int low = 0;
        int high = 100;
        GtkWidget *scale;

        g_variant_get (range, "(&sv)", &how, &limits);
        if (g_str_equal (how, "range"))
        {
            g_variant_get (limits, "(ii)", &low, &high);
        }
        scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, low, high, 1);
        gtk_widget_set_size_request (scale, 160, -1);
        gtk_widget_set_focusable (scale, FALSE);
        gtk_widget_set_focus_on_click (scale, FALSE);
        gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
        gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
        g_settings_bind (gsettings, setting->key, gtk_range_get_adjustment (GTK_RANGE (scale)),
                         "value", G_SETTINGS_BIND_DEFAULT);
        return scale;
    }

    /* A string: one button per choice. */
    {
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        g_auto (GStrv) choices = g_strsplit (_(setting->choices), "|", -1);
        g_autofree char *signal = g_strconcat ("changed::", setting->key, NULL);
        GtkWidget *first = NULL;

        gtk_widget_add_css_class (box, "linked");
        gtk_widget_add_css_class (box, "jump-choices");
        gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
        for (guint i = 0; choices[i] != NULL; i++)
        {
            char *eq = strchr (choices[i], '=');
            GtkWidget *button;

            if (eq == NULL)
            {
                continue;
            }
            *eq = '\0';
            button = gtk_toggle_button_new_with_label (eq + 1);
            gtk_widget_set_focusable (button, FALSE);
            gtk_widget_set_focus_on_click (button, FALSE);
            g_object_set_data_full (G_OBJECT (button), "value", g_strdup (choices[i]), g_free);
            if (first == NULL)
            {
                first = button;
            }
            else
            {
                gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (button), GTK_TOGGLE_BUTTON (first));
            }
            gtk_box_append (GTK_BOX (box), button);
        }
        sync_choices (gsettings, setting->key, box);
        for (GtkWidget *b = gtk_widget_get_first_child (box); b != NULL; b = gtk_widget_get_next_sibling (b))
        {
            g_signal_connect (b, "toggled", G_CALLBACK (on_choice_toggled), (gpointer) setting);
        }
        g_signal_connect_object (gsettings, signal, G_CALLBACK (sync_choices), box, 0);
        return box;
    }
}

static GtkWidget *
setting_row (guint i)
{
    GtkWidget *row = row_new (ROW_SETTING);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new (_(settings[i].title));

    gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("emblem-system-symbolic"));
    gtk_label_set_xalign (GTK_LABEL (title), 0);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_box_append (GTK_BOX (box), title);
    gtk_box_append (GTK_BOX (box), setting_control (&settings[i]));

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    g_object_set_data (G_OBJECT (row), "index", GUINT_TO_POINTER (i));
    return row;
}

/* Enter on a setting: flips a switch, takes the next choice. */
static void
change_setting (const Setting *setting)
{
    GSettings *gsettings = schema_settings (setting->schema);
    g_autoptr (GVariant) value = g_settings_get_value (gsettings, setting->key);

    if (g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN))
    {
        g_settings_set_boolean (gsettings, setting->key, !g_variant_get_boolean (value));
    }
    else if (setting->choices != NULL)
    {
        g_auto (GStrv) choices = g_strsplit (setting->choices, "|", -1);
        const char *now = g_variant_get_string (value, NULL);
        guint n = g_strv_length (choices);

        for (guint i = 0; i < n; i++)
        {
            g_auto (GStrv) parts = g_strsplit (choices[i], "=", 2);

            if (g_str_equal (parts[0], now))
            {
                g_auto (GStrv) next = g_strsplit (choices[(i + 1) % n], "=", 2);

                g_settings_set_string (gsettings, setting->key, next[0]);
                return;
            }
        }
    }
}

static gboolean
run_later (gpointer data)
{
    GtkWindow *window = g_object_get_data (G_OBJECT (data), "window");
    guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (data), "index"));

    if (window != NULL)
    {
        nautilus_shortcuts_run (window, index);
    }
    g_object_unref (data);
    return G_SOURCE_REMOVE;
}

static void
activate_row (NautilusJump  *jump,
              GtkListBoxRow *row,
              gboolean       new_tab)
{
    RowKind kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "kind"));
    guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "index"));

    switch (kind)
    {
        case ROW_FOLDER:
        {
            open_folder (jump, g_object_get_data (G_OBJECT (row), "file"), new_tab);
        }
        break;

        case ROW_COMMAND:
        {
            /* Once the entry has given the focus back, for commands on
             * the files. */
            GObject *data = g_object_new (G_TYPE_OBJECT, NULL);
            GtkWindow *window = jump_window (jump);

            g_object_set_data_full (data, "window", window != NULL ? g_object_ref (window) : NULL,
                                    g_object_unref);
            g_object_set_data (data, "index", GUINT_TO_POINTER (index));
            close_entry (jump);
            g_idle_add (run_later, data);
        }
        break;

        case ROW_SETTING:
        {
            if (settings[index].key == NULL)
            {
                GtkWindow *window = jump_window (jump);

                close_entry (jump);
                if (window != NULL)
                {
                    gtk_widget_activate_action (GTK_WIDGET (window), "app.preferences", NULL);
                }
            }
            else
            {
                change_setting (&settings[index]);
            }
        }
        break;

        default:
        {
        }
        break;
    }
}

static void
on_row_activated (GtkListBox    *list,
                  GtkListBoxRow *row,
                  NautilusJump  *jump)
{
    activate_row (jump, row, FALSE);
}

static void
header_func (GtkListBoxRow *row,
             GtkListBoxRow *before,
             gpointer       user_data)
{
    NautilusJump *jump = user_data;
    RowKind kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "kind"));
    RowKind previous = before != NULL ? GPOINTER_TO_INT (g_object_get_data (G_OBJECT (before), "kind")) : 0;
    const char *title = NULL;
    GtkWidget *label;

    if (jump->scope != SCOPE_ALL || kind == previous || kind == ROW_NONE)
    {
        gtk_list_box_row_set_header (row, NULL);
        return;
    }

    title = kind == ROW_FOLDER ? _("Folders") : kind == ROW_COMMAND ? _("Commands") : _("Settings");
    label = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_widget_add_css_class (label, "jump-heading");
    gtk_list_box_row_set_header (row, label);
}

static void
scroll_to (NautilusJump  *jump,
           GtkListBoxRow *row)
{
    graphene_rect_t bounds;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (jump->scroller);

    if (gtk_widget_compute_bounds (GTK_WIDGET (row), GTK_WIDGET (jump->scroller), &bounds))
    {
        double page = gtk_adjustment_get_page_size (adj);
        double value = gtk_adjustment_get_value (adj);

        if (bounds.origin.y < 0)
        {
            gtk_adjustment_set_value (adj, value + bounds.origin.y - 24);
        }
        else if (bounds.origin.y + bounds.size.height > page)
        {
            gtk_adjustment_set_value (adj, value + bounds.origin.y + bounds.size.height - page + 6);
        }
    }
}

static GtkListBoxRow *
step_row (NautilusJump  *jump,
          GtkListBoxRow *from,
          gboolean       forward)
{
    GtkWidget *child = from == NULL ? gtk_widget_get_first_child (GTK_WIDGET (jump->list))
                                    : forward ? gtk_widget_get_next_sibling (GTK_WIDGET (from))
                                              : gtk_widget_get_prev_sibling (GTK_WIDGET (from));

    for (; child != NULL; child = forward ? gtk_widget_get_next_sibling (child) : gtk_widget_get_prev_sibling (child))
    {
        if (GTK_IS_LIST_BOX_ROW (child) &&
            GPOINTER_TO_INT (g_object_get_data (G_OBJECT (child), "kind")) != ROW_NONE)
        {
            return GTK_LIST_BOX_ROW (child);
        }
    }

    return NULL;
}

static void
add_row (NautilusJump *jump,
         GtkWidget    *row)
{
    gtk_list_box_append (jump->list, row);
}

static void
rebuild (NautilusJump *jump)
{
    const char *query = jump->query != NULL ? jump->query : "";
    gboolean empty = *query == '\0';
    gboolean all = jump->scope == SCOPE_ALL;
    GtkWidget *child;
    guint shown = 0;

    while ((child = gtk_widget_get_first_child (GTK_WIDGET (jump->list))) != NULL)
    {
        gtk_list_box_remove (jump->list, child);
    }

    if (all || jump->scope == SCOPE_FOLDERS)
    {
        g_autoptr (GPtrArray) list = places ();
        g_autoptr (GPtrArray) chosen = g_ptr_array_new ();
        g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_autoptr (GArray) scores = g_array_new (FALSE, FALSE, sizeof (int));
        guint limit = all ? (empty ? 8 : 6) : 60;

        for (guint i = 0; i < list->len; i++)
        {
            Folder *folder = list->pdata[i];

            if (empty || matches (query, folder->name))
            {
                g_ptr_array_add (chosen, folder);
            }
        }
        if (!empty)
        {
            GPtrArray *here = folders_here (jump);

            for (guint i = 0; here != NULL && i < here->len; i++)
            {
                if (matches (query, ((Folder *) here->pdata[i])->name))
                {
                    g_ptr_array_add (chosen, here->pdata[i]);
                }
            }
        }
        if (!empty && jump->found != NULL && g_strcmp0 (jump->found_for, query) == 0)
        {
            for (guint i = 0; i < jump->found->len; i++)
            {
                g_ptr_array_add (chosen, jump->found->pdata[i]);
            }
        }

        /* Best first, each folder once. */
        for (guint i = 0; i < chosen->len; i++)
        {
            int score = empty ? (int) i : folder_score (chosen->pdata[i], query);

            g_array_append_val (scores, score);
        }
        for (guint i = 1; i < chosen->len; i++)
        {
            for (guint j = i; j > 0 && g_array_index (scores, int, j) < g_array_index (scores, int, j - 1); j--)
            {
                int s = g_array_index (scores, int, j);
                gpointer f = chosen->pdata[j];

                g_array_index (scores, int, j) = g_array_index (scores, int, j - 1);
                g_array_index (scores, int, j - 1) = s;
                chosen->pdata[j] = chosen->pdata[j - 1];
                chosen->pdata[j - 1] = f;
            }
        }
        for (guint i = 0, n = 0; i < chosen->len && n < limit; i++)
        {
            Folder *folder = chosen->pdata[i];
            char *uri = g_file_get_uri (folder->file);

            if (!g_hash_table_add (seen, uri))
            {
                continue;
            }
            add_row (jump, folder_row (jump, folder));
            n++;
            shown++;
        }
    }

    if ((all && !empty) || jump->scope == SCOPE_COMMANDS)
    {
        guint limit = all ? 4 : G_MAXUINT;

        for (guint pass = 0; pass < 2; pass++)
        {
            for (guint i = 0; i < nautilus_shortcuts_n_commands () && limit > 0; i++)
            {
                const char *title = nautilus_shortcuts_command_title (i);
                g_autofree char *haystack = g_strdup_printf ("%s %s", title,
                                                             nautilus_shortcuts_command_section (i));
                gboolean prefix = !empty && g_str_match_string (query, title, TRUE);

                if (g_str_equal (nautilus_shortcuts_command_action (i), "toolbar.jump-to") ||
                    !(empty || matches (query, haystack)) ||
                    (pass == 0) != (empty || prefix))
                {
                    continue;
                }
                add_row (jump, command_row (i));
                limit--;
                shown++;
            }
        }
    }

    if ((all && !empty) || jump->scope == SCOPE_SETTINGS)
    {
        guint limit = all ? 4 : G_MAXUINT;

        for (guint pass = 0; pass < 2; pass++)
        {
            for (guint i = 0; i < G_N_ELEMENTS (settings) && limit > 0; i++)
            {
                const char *title = _(settings[i].title);
                g_autofree char *haystack = g_strdup_printf ("%s %s", title, settings[i].words);
                gboolean prefix = !empty && g_str_match_string (query, title, TRUE);

                if (!(empty || matches (query, haystack)) || (pass == 0) != (empty || prefix))
                {
                    continue;
                }
                add_row (jump, setting_row (i));
                limit--;
                shown++;
            }
        }
    }

    if (shown == 0)
    {
        GtkWidget *row = row_new (ROW_NONE);
        gboolean looking = !empty && jump->scope == SCOPE_FOLDERS && g_strcmp0 (jump->found_for, query) != 0;
        GtkWidget *label = gtk_label_new (looking ? _("Looking…") : _("Nothing found"));

        gtk_widget_add_css_class (label, "dim-label");
        gtk_widget_set_margin_top (label, 12);
        gtk_widget_set_margin_bottom (label, 12);
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
        gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
        gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
        add_row (jump, row);
    }

    gtk_list_box_invalidate_headers (jump->list);
    {
        GtkListBoxRow *first = step_row (jump, NULL, TRUE);

        if (first != NULL)
        {
            gtk_list_box_select_row (jump->list, first);
        }
    }
    gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (jump->scroller), 0);
}

/* ----------------------------------------------------------- Scopes */

static void
set_scope (NautilusJump *jump,
           Scope         scope)
{
    jump->scope = scope;
    for (int i = 0; i < N_SCOPES; i++)
    {
        g_signal_handlers_block_matched (jump->scopes[i], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, jump);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (jump->scopes[i]), i == (int) scope);
        g_signal_handlers_unblock_matched (jump->scopes[i], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, jump);
    }
    rebuild (jump);
}

static void
on_scope_toggled (GtkToggleButton *button,
                  NautilusJump    *jump)
{
    if (gtk_toggle_button_get_active (button))
    {
        set_scope (jump, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "scope")));
    }
}

/* ------------------------------------------------------------- API */

NautilusJump *
nautilus_jump_new (GtkWidget *entry,
                   GtkWidget *anchor)
{
    static const char *names[] = { N_("All"), N_("Folders"), N_("Commands"), N_("Settings") };
    NautilusJump *jump = g_new0 (NautilusJump, 1);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *hint = gtk_label_new (_("Tab switches"));

    jump->entry = entry;
    jump->anchor = anchor;

    for (int i = 0; i < N_SCOPES; i++)
    {
        jump->scopes[i] = gtk_toggle_button_new_with_label (_(names[i]));
        gtk_widget_set_focusable (jump->scopes[i], FALSE);
        gtk_widget_set_focus_on_click (jump->scopes[i], FALSE);
        gtk_widget_add_css_class (jump->scopes[i], "flat");
        gtk_widget_add_css_class (jump->scopes[i], "jump-scope");
        g_object_set_data (G_OBJECT (jump->scopes[i]), "scope", GINT_TO_POINTER (i));
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (jump->scopes[i]), i == 0);
        g_signal_connect (jump->scopes[i], "toggled", G_CALLBACK (on_scope_toggled), jump);
        gtk_box_append (GTK_BOX (bar), jump->scopes[i]);
    }
    gtk_widget_set_hexpand (hint, TRUE);
    gtk_label_set_xalign (GTK_LABEL (hint), 1);
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_widget_add_css_class (hint, "caption");
    gtk_box_append (GTK_BOX (bar), hint);
    gtk_widget_add_css_class (bar, "jump-scopes");
    gtk_box_append (GTK_BOX (box), bar);

    jump->list = GTK_LIST_BOX (gtk_list_box_new ());
    gtk_widget_set_focusable (GTK_WIDGET (jump->list), FALSE);
    gtk_list_box_set_selection_mode (jump->list, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (jump->list, TRUE);
    gtk_list_box_set_header_func (jump->list, header_func, jump, NULL);
    gtk_widget_add_css_class (GTK_WIDGET (jump->list), "jump-list");
    g_signal_connect (jump->list, "row-activated", G_CALLBACK (on_row_activated), jump);

    jump->scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
    gtk_scrolled_window_set_policy (jump->scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (jump->scroller, TRUE);
    gtk_scrolled_window_set_max_content_height (jump->scroller, 460);
    gtk_scrolled_window_set_child (jump->scroller, GTK_WIDGET (jump->list));
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (jump->scroller));

    jump->popover = gtk_popover_new ();
    gtk_popover_set_autohide (GTK_POPOVER (jump->popover), FALSE);
    gtk_popover_set_has_arrow (GTK_POPOVER (jump->popover), FALSE);
    gtk_popover_set_position (GTK_POPOVER (jump->popover), GTK_POS_BOTTOM);
    gtk_widget_set_focusable (jump->popover, FALSE);
    gtk_widget_add_css_class (jump->popover, "jump-menu");
    gtk_popover_set_child (GTK_POPOVER (jump->popover), box);
    gtk_widget_set_parent (jump->popover, anchor);

    return jump;
}

void
nautilus_jump_free (NautilusJump *jump)
{
    g_clear_handle_id (&jump->search_timeout, g_source_remove);
    g_clear_handle_id (&jump->size_idle, g_source_remove);
    g_clear_object (&jump->here);
    g_clear_pointer (&jump->here_folders, g_ptr_array_unref);
    g_cancellable_cancel (jump->cancellable);
    g_clear_object (&jump->cancellable);
    g_clear_object (&jump->statement);
    g_clear_pointer (&jump->found, g_ptr_array_unref);
    g_free (jump->found_for);
    g_free (jump->query);
    g_clear_weak_pointer (&jump->slot);
    g_clear_pointer (&jump->popover, gtk_widget_unparent);
    g_free (jump);
}

void
nautilus_jump_set_slot (NautilusJump       *jump,
                        NautilusWindowSlot *slot)
{
    g_set_weak_pointer (&jump->slot, slot);
}

void
nautilus_jump_hide (NautilusJump *jump)
{
    g_clear_handle_id (&jump->search_timeout, g_source_remove);
    if (gtk_widget_get_visible (jump->popover))
    {
        gtk_popover_popdown (GTK_POPOVER (jump->popover));
    }
}

static void
size_popover (NautilusJump *jump)
{
    gtk_widget_set_size_request (gtk_popover_get_child (GTK_POPOVER (jump->popover)),
                                 MAX (gtk_widget_get_width (jump->anchor) - 12, 420), -1);
}

static gboolean
size_later (gpointer user_data)
{
    NautilusJump *jump = user_data;

    jump->size_idle = 0;
    size_popover (jump);
    return G_SOURCE_REMOVE;
}

gboolean
nautilus_jump_contains (NautilusJump *jump,
                        GtkWidget    *widget)
{
    return widget != NULL && (widget == jump->popover || gtk_widget_is_ancestor (widget, jump->popover));
}

void
nautilus_jump_update (NautilusJump *jump)
{
    g_autofree char *text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (jump->entry)));
    gboolean changed;

    g_strstrip (text);
    if (!gtk_widget_get_mapped (jump->entry) || nautilus_jump_is_path (text))
    {
        nautilus_jump_hide (jump);
        return;
    }

    changed = g_strcmp0 (text, jump->query) != 0 || !gtk_widget_get_visible (jump->popover);
    if (!changed)
    {
        return;
    }
    if (!gtk_widget_get_visible (jump->popover))
    {
        jump->scope = SCOPE_ALL;
        for (int i = 0; i < N_SCOPES; i++)
        {
            g_signal_handlers_block_matched (jump->scopes[i], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, jump);
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (jump->scopes[i]), i == 0);
            g_signal_handlers_unblock_matched (jump->scopes[i], G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, jump);
        }
    }

    g_free (jump->query);
    jump->query = g_steal_pointer (&text);

    g_clear_handle_id (&jump->search_timeout, g_source_remove);
    if (*jump->query != '\0')
    {
        jump->search_timeout = g_timeout_add (60, search_folders, jump);
    }

    rebuild (jump);

    if (!gtk_widget_get_visible (jump->popover))
    {
        /* A new folder may have new folders in it. */
        g_clear_object (&jump->here);
        size_popover (jump);
        gtk_popover_popup (GTK_POPOVER (jump->popover));
        /* The entry has only just been shown: its width once laid out. */
        if (jump->size_idle == 0)
        {
            jump->size_idle = g_idle_add (size_later, jump);
        }
    }
}

gboolean
nautilus_jump_key (NautilusJump    *jump,
                   guint            keyval,
                   GdkModifierType  state)
{
    GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();
    GtkListBoxRow *selected;

    if (!gtk_widget_get_visible (jump->popover))
    {
        return FALSE;
    }

    selected = gtk_list_box_get_selected_row (jump->list);
    switch (keyval)
    {
        case GDK_KEY_Down:
        case GDK_KEY_Up:
        case GDK_KEY_Page_Down:
        case GDK_KEY_Page_Up:
        {
            gboolean down = keyval == GDK_KEY_Down || keyval == GDK_KEY_Page_Down;
            guint steps = (keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_Page_Up) ? 8 : 1;
            GtkListBoxRow *row = selected;

            for (guint i = 0; i < steps; i++)
            {
                GtkListBoxRow *next = step_row (jump, row, down);

                if (next == NULL)
                {
                    break;
                }
                row = next;
            }
            if (row != NULL)
            {
                gtk_list_box_select_row (jump->list, row);
                scroll_to (jump, row);
            }
            return TRUE;
        }

        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab:
        {
            gboolean back = keyval == GDK_KEY_ISO_Left_Tab || (mods & GDK_SHIFT_MASK);

            set_scope (jump, (jump->scope + (back ? N_SCOPES - 1 : 1)) % N_SCOPES);
            return TRUE;
        }

        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_ISO_Enter:
        {
            if (selected == NULL)
            {
                return TRUE;
            }
            activate_row (jump, selected, (mods & GDK_CONTROL_MASK) != 0);
            return TRUE;
        }

        default:
        {
        }
        break;
    }

    return FALSE;
}
