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

#include <adwaita.h>
#include <glib/gi18n.h>

#include "nautilus-application.h"
#include "nautilus-bookmark.h"
#include "nautilus-bookmark-list.h"
#include "nautilus-global-preferences.h"
#include "nautilus-labels.h"
#include "nautilus-localsearch-utilities.h"
#include "nautilus-file.h"
#include "nautilus-shortcuts.h"
#include "nautilus-window.h"

typedef enum
{
    SCOPE_ALL,
    SCOPE_FOLDERS,
    SCOPE_FILES,
    SCOPE_COMMANDS,
    SCOPE_SETTINGS,
    N_SCOPES
} Scope;

typedef enum
{
    ROW_FOLDER = 1,
    ROW_COMMAND,
    ROW_SETTING,
    ROW_FILE,
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
    const char *section;
    const char *words;   /* more to find it by */
    int schema;
    const char *key;     /* NULL: opens the Preferences */
    const char *choices; /* for a string: "value=Label|…" */
} Setting;

#define SPEED N_("local-only=This Device|always=All|never=Never")
#define DENSITY N_("comfortable=Comfortable|compact=Compact|dense=Dense|extra-dense=Extra")

static const Setting settings[] =
{
    { N_("Show Hidden Files"), N_("View"), "dot files", SCHEMA_CHOOSER, "show-hidden", NULL },
    { N_("Folders Before Files"), N_("View"), "sort first", SCHEMA_CHOOSER, "sort-directories-first", NULL },
    { N_("Click to Open"), N_("Files"), "single double mouse", SCHEMA_NAUTILUS, "click-policy", N_("single=Single|double=Double") },
    { N_("Density"), N_("View"), "rows icons size compact", SCHEMA_FLIER, "density", DENSITY },
    { N_("Sidebar Density"), N_("Sidebar"), "rows size compact", SCHEMA_FLIER, "sidebar-density", DENSITY },
    { N_("Inspector"), N_("Inspector"), "details panel info", SCHEMA_FLIER, "inspector-visible", NULL },
    { N_("Inspector Density"), N_("Inspector"), "details compact", SCHEMA_FLIER, "inspector-density",
      N_("comfortable=Comfortable|compact=Compact|dense=Dense") },
    { N_("Inspector Side"), N_("Inspector"), "details dock panel", SCHEMA_FLIER, "inspector-side",
      N_("left=Left|right=Right|top=Top|bottom=Bottom|window=Window") },
    { N_("Bottom Bar"), N_("View"), "filter status", SCHEMA_FLIER, "bottom-bar", NULL },
    { N_("Panel Buttons"), N_("View"), "sidebar inspector header toggle", SCHEMA_FLIER, "panel-buttons", NULL },
    { N_("Columns View"), N_("View"), "miller", SCHEMA_FLIER, "columns-view", NULL },
    { N_("Tree View for New Tabs"), N_("View"), "expand list", SCHEMA_FLIER, "tree-view", NULL },
    { N_("Ratings"), N_("Rate and Tag"), "stars", SCHEMA_FLIER, "ratings", NULL },
    { N_("Tags"), N_("Rate and Tag"), "labels", SCHEMA_FLIER, "tags", NULL },
    { N_("Ratings and Tags on Grid Items"), N_("Rate and Tag"), "stars tiles", SCHEMA_FLIER, "marks-on-tiles", NULL },
    { N_("Stars in Columns"), N_("Rate and Tag"), "ratings miller", SCHEMA_FLIER, "columns-stars", NULL },
    { N_("Folder Colors"), N_("Appearance"), "colours", SCHEMA_FLIER, "folder-colors", NULL },
    { N_("Folder Color Scheme"), N_("Appearance"), "colours palette", SCHEMA_FLIER, "folder-color-scheme",
      N_("pastel=Pastel|vibrant=Vibrant|earth=Earth|ocean=Ocean|mono=Mono") },
    { N_("Window Opacity"), N_("Appearance"), "transparent transparency blur", SCHEMA_FLIER, "window-opacity", NULL },
    { N_("Alternating Row Colors"), N_("Appearance"), "zebra stripes rows list striped", SCHEMA_FLIER, "alternate-rows", NULL },
    { N_("Framed Panels"), N_("Appearance"), "theme cards rounded tiles style look", SCHEMA_FLIER, "framed-panels", NULL },
    { N_("Color Theme"), N_("Appearance"), "colour theme palette look style dark", SCHEMA_FLIER, "color-theme",
      N_("mako=Mako|uniform=Uniform|classic=Classic|graphite=Graphite|deep-sea=Deep Sea|synthwave=Synthwave|matcha=Matcha") },
    { N_("Built-in Quick Look"), N_("Quick Look"), "preview", SCHEMA_FLIER, "own-quick-look", NULL },
    { N_("Quick Look Names"), N_("Quick Look"), "preview", SCHEMA_FLIER, "quick-look-names", NULL },
    { N_("Quick Look Ratings and Tags"), N_("Quick Look"), "preview stars", SCHEMA_FLIER, "quick-look-marks", NULL },
    { N_("Thumbnails"), N_("Files"), "previews images", SCHEMA_NAUTILUS, "show-image-thumbnails", SPEED },
    { N_("Folder Item Counts"), N_("Files"), "count size", SCHEMA_NAUTILUS, "show-directory-item-counts", SPEED },
    { N_("Search in Subfolders"), N_("Search"), "recursive", SCHEMA_NAUTILUS, "recursive-search", SPEED },
    { N_("Dates"), N_("Files"), "time format detailed", SCHEMA_NAUTILUS, "date-time-format",
      N_("simple=Simple|detailed=Detailed") },
    { N_("Sidebar Groups"), N_("Sidebar"), "sections fold", SCHEMA_FLIER, "sidebar-groups", NULL },
    { N_("Storage Usage in the Sidebar"), N_("Sidebar"), "disk drive space", SCHEMA_FLIER, "sidebar-storage-usage", NULL },
    { N_("Progress Panel"), N_("Copying"), "copy move", SCHEMA_FLIER, "progress-popup", NULL },
    { N_("Progress Panel in Its Own Window"), N_("Copying"), "copy move popup", SCHEMA_FLIER, "progress-popup-window", NULL },
    { N_("Copy One After Another"), N_("Copying"), "queue move", SCHEMA_FLIER, "queue-transfers", NULL },
    { N_("Delete Permanently in Menus"), N_("Menus"), "trash", SCHEMA_NAUTILUS, "show-delete-permanently", NULL },
    { N_("Create Link in Menus"), N_("Menus"), "symlink", SCHEMA_NAUTILUS, "show-create-link", NULL },
    { N_("Open Folders While Dragging"), N_("Files"), "drag drop hover", SCHEMA_NAUTILUS, "open-folder-on-dnd-hover", NULL },
    { N_("Shortcuts in Menus"), N_("Keyboard"), "keys", SCHEMA_FLIER, "menu-shortcuts", NULL },
    { N_("Shortcut Hints"), N_("Keyboard"), "keys", SCHEMA_FLIER, "shortcut-hints", NULL },
    { N_("Run in the Background"), N_("Startup"), "startup start", SCHEMA_FLIER, "run-in-background", NULL },
    { N_("All Preferences"), N_("Opens the Preferences window"), "settings options", 0, NULL, NULL },
};

typedef struct
{
    GFile *file;
    char *name;
    const char *icon;
    gboolean place;
    gboolean visited;
    gboolean is_file;
} Folder;

struct _NautilusJump
{
    GtkWidget *entry;
    GtkWidget *anchor;
    GtkWidget *popover;
    GtkWidget *scopes[N_SCOPES];
    GtkWidget *counts[N_SCOPES];
    GtkListBox *list;
    GtkScrolledWindow *scroller;
    NautilusWindowSlot *slot;
    Scope scope;
    char *query;

    /* Folders from the index for found_for. */
    GPtrArray *found;
    char *found_for;
    TrackerSparqlStatement *statement;
    TrackerSparqlStatement *statement_files;
    /* Files from the index for files_for. */
    GPtrArray *found_files;
    char *files_for;
    /* What was typed when the menu last closed, and on which tab. */
    char *last_query;
    Scope last_scope;
    GCancellable *cancellable;
    guint search_timeout;
    guint size_idle;

    /* The folders in the current folder, read once per folder. */
    GFile *here;
    GPtrArray *here_folders;
};

static void rebuild (NautilusJump *jump);
static gboolean matches (const char *query, const char *haystack);

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
    folder->icon = icon;
    folder->name = name != NULL ? g_strdup (name) : g_file_get_basename (file);
    if (name == NULL)
    {
        g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());

        if (g_file_equal (file, home))
        {
            g_free (folder->name);
            folder->name = g_strdup (_("Home"));
            folder->icon = "user-home-symbolic";
        }
    }
    folder->place = place;
    return folder;
}

/* ------------------------------------------------------------ Visits */

/* How often and how lately each folder was opened, to rank the ones in
 * use first: ~/.local/share/mako/jump-visits, "count<TAB>time<TAB>uri". */
typedef struct
{
    guint count;
    gint64 last;
} Visit;

static GHashTable *visits;
static guint visits_save;

static char *
visits_path (void)
{
    return g_build_filename (g_get_user_data_dir (), "mako", "jump-visits", NULL);
}

static GHashTable *
get_visits (void)
{
    g_autofree char *path = NULL;
    g_autofree char *text = NULL;

    if (visits != NULL)
    {
        return visits;
    }

    visits = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    path = visits_path ();
    if (g_file_get_contents (path, &text, NULL, NULL))
    {
        g_auto (GStrv) lines = g_strsplit (text, "\n", -1);

        for (guint i = 0; lines[i] != NULL; i++)
        {
            g_auto (GStrv) fields = g_strsplit (lines[i], "\t", 3);

            if (g_strv_length (fields) == 3)
            {
                Visit *visit = g_new0 (Visit, 1);

                visit->count = (guint) g_ascii_strtoull (fields[0], NULL, 10);
                visit->last = g_ascii_strtoll (fields[1], NULL, 10);
                g_hash_table_replace (visits, g_strdup (fields[2]), visit);
            }
        }
    }

    return visits;
}

static double
visit_weight (const Visit *visit)
{
    gint64 days = (g_get_real_time () / G_USEC_PER_SEC - visit->last) / (24 * 3600);
    double recent = days < 4 ? 1.0 : days < 14 ? 0.6 : days < 90 ? 0.3 : 0.1;

    return visit->count * recent;
}

static double
frecency (GFile *file)
{
    g_autofree char *uri = g_file_get_uri (file);
    Visit *visit = g_hash_table_lookup (get_visits (), uri);

    return visit != NULL ? visit_weight (visit) : 0;
}

static guint
visit_count (GFile *file)
{
    g_autofree char *uri = g_file_get_uri (file);
    Visit *visit = g_hash_table_lookup (get_visits (), uri);

    return visit != NULL ? visit->count : 0;
}

static gboolean
save_visits (gpointer user_data)
{
    g_autoptr (GString) out = g_string_new (NULL);
    g_autofree char *path = visits_path ();
    g_autofree char *dir = g_path_get_dirname (path);
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    visits_save = 0;
    g_hash_table_iter_init (&iter, get_visits ());
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        Visit *visit = value;

        g_string_append_printf (out, "%u\t%" G_GINT64_FORMAT "\t%s\n", visit->count, visit->last, (char *) key);
    }
    g_mkdir_with_parents (dir, 0700);
    g_file_set_contents (path, out->str, out->len, NULL);

    return G_SOURCE_REMOVE;
}

void
nautilus_jump_note_visit (GFile *location)
{
    g_autofree char *uri = NULL;
    Visit *visit;

    if (location == NULL)
    {
        return;
    }
    uri = g_file_get_uri (location);
    /* Folders, not searches, Recent, Starred or the Trash. */
    if (g_str_has_prefix (uri, "x-nautilus-") || g_str_has_prefix (uri, "recent:") ||
        g_str_has_prefix (uri, "starred:") || g_str_has_prefix (uri, "trash:") ||
        g_str_has_prefix (uri, "other-locations:"))
    {
        return;
    }

    visit = g_hash_table_lookup (get_visits (), uri);
    if (visit == NULL)
    {
        visit = g_new0 (Visit, 1);
        g_hash_table_replace (visits, g_strdup (uri), visit);
    }
    visit->count++;
    visit->last = g_get_real_time () / G_USEC_PER_SEC;

    /* At most the 800 most used. */
    if (g_hash_table_size (visits) > 1000)
    {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_iter_init (&iter, visits);
        while (g_hash_table_iter_next (&iter, &key, &value))
        {
            if (visit_weight (value) < 1.0 && value != visit)
            {
                g_hash_table_iter_remove (&iter);
            }
        }
    }

    if (visits_save == 0)
    {
        visits_save = g_timeout_add_seconds (2, save_visits, NULL);
    }
}

/* The folders opened most and lately, best first. */
static GPtrArray *
visited_folders (void)
{
    GPtrArray *result = g_ptr_array_new_with_free_func ((GDestroyNotify) folder_free);
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_hash_table_iter_init (&iter, get_visits ());
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        g_autoptr (GFile) file = g_file_new_for_uri (key);
        Folder *folder = folder_new (file, NULL, "folder-symbolic", FALSE);

        folder->visited = TRUE;
        g_ptr_array_add (result, folder);
    }

    return result;
}

/* ---------------------------------------------------------- Matching */

/* "dl" for Downloads, "ql" for Quick Look: the letters in order, the
 * first starting a word. */
static gboolean
fuzzy (const char *query,
       const char *name)
{
    g_autofree char *folded_query = g_utf8_casefold (query, -1);
    g_autofree char *folded = g_utf8_casefold (name, -1);
    g_autoptr (GString) q = g_string_new (NULL);

    for (const char *p = folded_query; *p != '\0'; p++)
    {
        if (!g_ascii_isspace (*p))
        {
            g_string_append_c (q, *p);
        }
    }
    if (q->len < 2)
    {
        return FALSE;
    }

    for (const char *start = folded; *start != '\0'; start++)
    {
        gboolean word_start = start == folded || !g_ascii_isalnum (start[-1]);
        const char *at = start;
        gsize matched = 0;

        if (!word_start || *start != q->str[0])
        {
            continue;
        }
        for (; *at != '\0' && matched < q->len; at++)
        {
            if (*at == q->str[matched])
            {
                matched++;
            }
        }
        if (matched == q->len)
        {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
matches_name (const char *query,
              const char *name)
{
    return matches (query, name) || fuzzy (query, name);
}

/* --------------------------------------------------------- Drives */

/* A short name for the drive @file is on, if not the computer's own:
 * "USB Stick", "Network". */
static char *
drive_label (GFile *file)
{
    g_autofree char *path = g_file_get_path (file);
    g_autofree char *scheme = g_file_get_uri_scheme (file);
    const char *rest = NULL;

    if (path == NULL)
    {
        return g_strcmp0 (scheme, "file") == 0 ? NULL : g_strdup (_("Network"));
    }
    if (strstr (path, "/gvfs/") != NULL)
    {
        return g_strdup (_("Network"));
    }
    if (g_str_has_prefix (path, "/media/"))
    {
        rest = path + strlen ("/media/");
    }
    else if (g_str_has_prefix (path, "/run/media/"))
    {
        rest = path + strlen ("/run/media/");
    }
    if (rest != NULL)
    {
        /* /media/USER/NAME/… */
        const char *name = strchr (rest, '/');

        if (name != NULL)
        {
            const char *end = strchr (name + 1, '/');

            return end != NULL ? g_strndup (name + 1, end - name - 1) : g_strdup (name + 1);
        }
    }
    if (g_str_has_prefix (path, "/mnt/"))
    {
        const char *end = strchr (path + 5, '/');

        return end != NULL ? g_strndup (path + 5, end - path - 5) : g_strdup (path + 5);
    }

    return NULL;
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
/* Lower is better: the name as typed, then starting with it, then
 * containing it, then only its letters in order; opened often and lately,
 * places, then ordinary folders, then hidden and app-data ones; shallow
 * first. */
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
    score = g_str_equal (name, wanted) ? 0
            : g_str_has_prefix (name, wanted) ? 1000
            : strstr (name, wanted) != NULL || matches (query, folder->name) ? 2000 : 3000;
    if (folder->place)
    {
        score -= 500;
    }
    score -= (int) MIN (frecency (folder->file) * 60, 900);
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



static GPtrArray *
read_results (TrackerSparqlStatement *statement,
              GAsyncResult           *result,
              gboolean                files,
              gboolean               *cancelled)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (TrackerSparqlCursor) cursor = NULL;
    GPtrArray *found;

    cursor = tracker_sparql_statement_execute_finish (statement, result, &error);
    *cancelled = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if (cursor == NULL)
    {
        if (!*cancelled)
        {
            g_debug ("Jump: search failed: %s", error != NULL ? error->message : "");
        }
        return NULL;
    }

    found = g_ptr_array_new_with_free_func ((GDestroyNotify) folder_free);
    while (tracker_sparql_cursor_next (cursor, NULL, NULL))
    {
        g_autoptr (GFile) file = g_file_new_for_uri (tracker_sparql_cursor_get_string (cursor, 0, NULL));
        Folder *folder = folder_new (file, NULL, "folder-symbolic", FALSE);

        folder->is_file = files;
        g_ptr_array_add (found, folder);
    }
    tracker_sparql_cursor_close (cursor);

    return found;
}

static void
on_query_done (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
    gboolean cancelled = FALSE;
    GPtrArray *found = read_results (TRACKER_SPARQL_STATEMENT (source), result, FALSE, &cancelled);
    NautilusJump *jump = user_data;

    if (cancelled)
    {
        return;
    }
    g_clear_pointer (&jump->found, g_ptr_array_unref);
    jump->found = found;
    g_free (jump->found_for);
    jump->found_for = g_strdup (jump->query);
    rebuild (jump);
}

static void
on_files_done (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
    gboolean cancelled = FALSE;
    GPtrArray *found = read_results (TRACKER_SPARQL_STATEMENT (source), result, TRUE, &cancelled);
    NautilusJump *jump = user_data;

    if (cancelled)
    {
        return;
    }
    g_clear_pointer (&jump->found_files, g_ptr_array_unref);
    jump->found_files = found;
    g_free (jump->files_for);
    jump->files_for = g_strdup (jump->query);
    rebuild (jump);
}

static TrackerSparqlStatement *
statement_for (const char *sparql)
{
    TrackerSparqlConnection *connection = nautilus_localsearch_get_miner_fs_connection (NULL);

    return connection != NULL ? tracker_sparql_connection_query_statement (connection, sparql, NULL, NULL) : NULL;
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
        jump->statement = statement_for ("SELECT DISTINCT ?url WHERE {"
                                         "  GRAPH tracker:FileSystem {"
                                         "    ?folder a nfo:Folder ; nie:isStoredAs ?file ."
                                         "    ?file nie:url ?url ; fts:match ~match ."
                                         "  }"
                                         "} LIMIT 300");
    }
    if (jump->statement_files == NULL)
    {
        jump->statement_files = statement_for ("SELECT DISTINCT ?url WHERE {"
                                               "  GRAPH tracker:FileSystem {"
                                               "    ?file a nfo:FileDataObject ; nie:url ?url ; fts:match ~match ."
                                               "    FILTER NOT EXISTS { ?folder a nfo:Folder ; nie:isStoredAs ?file }"
                                               "  }"
                                               "} LIMIT 300");
    }
    if (jump->statement == NULL || jump->statement_files == NULL)
    {
        return G_SOURCE_REMOVE;
    }

    g_cancellable_cancel (jump->cancellable);
    g_clear_object (&jump->cancellable);
    jump->cancellable = g_cancellable_new ();
    tracker_sparql_statement_bind_string (jump->statement, "match", match->str);
    tracker_sparql_statement_execute_async (jump->statement, jump->cancellable, on_query_done, jump);
    tracker_sparql_statement_bind_string (jump->statement_files, "match", match->str);
    tracker_sparql_statement_execute_async (jump->statement_files, jump->cancellable, on_files_done, jump);

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
row_new (RowKind kind)
{
    GtkWidget *row = gtk_list_box_row_new ();

    gtk_widget_set_focusable (row, FALSE);
    gtk_widget_add_css_class (row, "jump-row");
    g_object_set_data (G_OBJECT (row), "kind", GINT_TO_POINTER (kind));
    return row;
}

/* @text, with where @query (or its first word) is in it in the accent
 * color, as markup. */
static char *
highlight (const char *text,
           const char *query)
{
    g_autofree char *lower = g_utf8_strdown (text, -1);
    g_autofree char *whole = g_utf8_strdown (query != NULL ? query : "", -1);
    g_auto (GStrv) words = NULL;
    const char *tries[2];
    AdwStyleManager *style = adw_style_manager_get_default ();
    GdkRGBA rgba;

    g_strstrip (whole);
    words = g_strsplit (whole, " ", 2);
    tries[0] = whole;
    tries[1] = words[0];

    /* Byte offsets of the lowered text only fit if it kept its length. */
    if (strlen (lower) != strlen (text))
    {
        return g_markup_escape_text (text, -1);
    }

    for (guint t = 0; t < 2; t++)
    {
        const char *at = tries[t] != NULL && *tries[t] != '\0' ? strstr (lower, tries[t]) : NULL;

        if (at != NULL)
        {
            gsize start = at - lower;
            gsize len = strlen (tries[t]);
            g_autofree char *before = g_markup_escape_text (text, start);
            g_autofree char *match = g_markup_escape_text (text + start, len);
            g_autofree char *after = g_markup_escape_text (text + start + len, -1);

            /* As in the design: with the purple accent its violet,
             * #8b7cf6, which the selected row is tinted with too. */
            if (adw_style_manager_get_accent_color (style) == ADW_ACCENT_COLOR_PURPLE)
            {
                gdk_rgba_parse (&rgba, "#8b7cf6");
            }
            else
            {
                g_autoptr (GdkRGBA) accent = adw_style_manager_get_accent_color_rgba (style);

                rgba = *accent;
            }
            return g_strdup_printf ("%s<span foreground=\"#%02x%02x%02x\" weight=\"bold\">%s</span>%s",
                                    before, (int) (rgba.red * 255), (int) (rgba.green * 255),
                                    (int) (rgba.blue * 255), match, after);
        }
    }

    return g_markup_escape_text (text, -1);
}

static GtkWidget *
markup_label (const char *markup,
              const char *css_class)
{
    GtkWidget *label = gtk_label_new (NULL);

    gtk_label_set_markup (GTK_LABEL (label), markup);
    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    if (css_class != NULL)
    {
        gtk_widget_add_css_class (label, css_class);
    }
    return label;
}

/* Title over a dim line. */
static GtkWidget *
two_lines (const char *title_markup,
           const char *sub)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    g_autofree char *escaped = g_markup_escape_text (sub, -1);

    gtk_box_append (GTK_BOX (box), markup_label (title_markup, NULL));
    gtk_box_append (GTK_BOX (box), markup_label (escaped, "jump-sub"));
    gtk_widget_set_hexpand (box, TRUE);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    return box;
}

static GtkWidget *
row_icon (const char *name,
          gboolean    dim)
{
    GtkWidget *icon = gtk_image_new_from_icon_name (name);

    gtk_image_set_pixel_size (GTK_IMAGE (icon), dim ? 16 : 20);
    gtk_widget_set_size_request (icon, 22, -1);
    if (dim)
    {
        gtk_widget_add_css_class (icon, "jump-dim-icon");
    }
    return icon;
}

/* The path as one line: "Home / Documents / Downloads", every folder above
 * a button that goes there, the folder itself in bold. */
static GtkWidget *
path_line (NautilusJump *jump,
           Folder       *folder,
           const char   *query)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    g_autoptr (GFile) home = g_file_new_for_path (g_get_home_dir ());
    g_autoptr (GPtrArray) chain = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr (GFile) at = g_file_get_parent (folder->file);
    g_autofree char *last = highlight (folder->name, query);
    gboolean cut = FALSE;
    GtkWidget *name;

    if (g_file_equal (folder->file, home))
    {
        at = NULL;
    }
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

    /* Where it starts and the nearest few. */
    if (chain->len > 5)
    {
        g_ptr_array_remove_range (chain, 1, chain->len - 4);
        cut = TRUE;
    }

    gtk_widget_add_css_class (box, "jump-path");
    for (guint i = 0; i < chain->len; i++)
    {
        GFile *step = chain->pdata[i];
        g_autofree char *label = NULL;
        g_autofree char *path = g_file_get_path (step);
        gboolean root = path != NULL && g_str_equal (path, "/");

        label = g_file_equal (step, home) ? g_strdup (_("Home"))
                : root ? g_strdup (_("Computer")) : g_file_get_basename (step);
        gtk_box_append (GTK_BOX (box), crumb_button (jump, step, label));
        gtk_box_append (GTK_BOX (box), markup_label ("/", "jump-crumb-sep"));
        if (cut && i == 0)
        {
            gtk_box_append (GTK_BOX (box), markup_label ("…", "jump-crumb-sep"));
            gtk_box_append (GTK_BOX (box), markup_label ("/", "jump-crumb-sep"));
        }
    }
    name = markup_label (last, "jump-last");
    gtk_box_append (GTK_BOX (box), name);
    gtk_widget_set_hexpand (box, TRUE);

    return box;
}

static GdkContentProvider *
on_row_drag_prepare (GtkDragSource *source,
                     double         x,
                     double         y,
                     GtkWidget     *row)
{
    GFile *file = g_object_get_data (G_OBJECT (row), "file");
    GSList *list = g_slist_prepend (NULL, file);
    GdkFileList *files = gdk_file_list_new_from_list (list);
    GdkContentProvider *provider = gdk_content_provider_new_typed (GDK_TYPE_FILE_LIST, files);

    g_slist_free (list);
    g_boxed_free (GDK_TYPE_FILE_LIST, files);
    return provider;
}

/* A row dragged out drops its folder or file, as from the files. */
static void
add_drag (GtkWidget *row)
{
    GtkDragSource *source = gtk_drag_source_new ();

    gtk_drag_source_set_actions (source, GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
    g_signal_connect (source, "prepare", G_CALLBACK (on_row_drag_prepare), row);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (source));
}

static void
add_suffix (GtkWidget *box,
            GFile     *file,
            guint      opened)
{
    g_autofree char *drive = drive_label (file);

    if (drive != NULL)
    {
        GtkWidget *chip = gtk_label_new (drive);

        gtk_widget_add_css_class (chip, "jump-drive");
        gtk_widget_set_valign (chip, GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (box), chip);
    }
    if (opened > 1)
    {
        g_autofree char *text = g_strdup_printf (_("opened %u×"), opened);
        GtkWidget *label = gtk_label_new (text);

        gtk_widget_add_css_class (label, "jump-opened");
        gtk_box_append (GTK_BOX (box), label);
    }
}

static GtkWidget *
folder_row (NautilusJump *jump,
            Folder       *folder,
            const char   *query)
{
    GtkWidget *row = row_new (ROW_FOLDER);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);

    gtk_box_append (GTK_BOX (box), row_icon (g_str_has_prefix (folder->icon, "user-home") ? "user-home" : "folder",
                                             FALSE));
    gtk_box_append (GTK_BOX (box), path_line (jump, folder, query));
    add_suffix (box, folder->file, visit_count (folder->file));

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_widget_set_tooltip_text (row, _("Enter opens it, Ctrl+Enter in a new tab, Shift+Enter in a split, "
                                        "Alt+Enter copies its path. Drag it to drop it elsewhere."));
    g_object_set_data_full (G_OBJECT (row), "file", g_object_ref (folder->file), g_object_unref);
    add_drag (row);
    return row;
}

static GtkWidget *
file_row (NautilusJump *jump,
          Folder       *file,
          const char   *query)
{
    GtkWidget *row = row_new (ROW_FILE);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    g_autofree char *type = g_content_type_guess (file->name, NULL, 0, NULL);
    g_autoptr (GIcon) gicon = g_content_type_get_icon (type);
    GtkWidget *icon = gtk_image_new_from_gicon (gicon);

    gtk_image_set_pixel_size (GTK_IMAGE (icon), 20);
    gtk_widget_set_size_request (icon, 22, -1);
    gtk_box_append (GTK_BOX (box), icon);
    gtk_box_append (GTK_BOX (box), path_line (jump, file, query));
    add_suffix (box, file->file, 0);

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_widget_set_tooltip_text (row, _("Enter opens it, Ctrl+Enter shows it in its folder, Shift+Enter "
                                        "there in a split, Alt+Enter copies its path"));
    g_object_set_data_full (G_OBJECT (row), "file", g_object_ref (file->file), g_object_unref);
    add_drag (row);
    return row;
}

/* Keys are changed over the window, once the menu is closed. */
static void
record (NautilusJump *jump,
        guint         i,
        int           slot)
{
    GtkWindow *window = jump_window (jump);

    close_entry (jump);
    if (window != NULL)
    {
        nautilus_shortcuts_record_key (GTK_WIDGET (window), i, slot);
    }
}

static void
on_key_clicked (GtkButton    *button,
                NautilusJump *jump)
{
    record (jump, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "index")),
            GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "slot")));
}

static void
on_key_secondary (GtkGestureClick *gesture,
                  int              n_press,
                  double           x,
                  double           y,
                  NautilusJump    *jump)
{
    GtkWidget *button = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

    /* Right-click removes the key; the rows are made again for it. */
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    nautilus_shortcuts_remove_key (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "index")),
                                   GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "slot")));
}

static void
on_reset_clicked (GtkButton    *button,
                  NautilusJump *jump)
{
    nautilus_shortcuts_reset_keys (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "index")));
}

static GtkWidget *
key_chip (NautilusJump *jump,
          guint         i,
          int           slot,
          const char   *accel)
{
    guint key = 0;
    GdkModifierType mods = 0;
    g_autofree char *text = NULL;
    GtkWidget *button;
    GtkGesture *secondary = gtk_gesture_click_new ();

    if (accel != NULL)
    {
        gtk_accelerator_parse (accel, &key, &mods);
        text = gtk_accelerator_get_label (key, mods);
    }
    button = gtk_button_new_with_label (accel != NULL ? text : _("Add key"));
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_add_css_class (button, "jump-kbd");
    if (accel == NULL)
    {
        gtk_widget_add_css_class (button, "jump-add");
    }
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable (button, FALSE);
    gtk_widget_set_focus_on_click (button, FALSE);
    gtk_widget_set_tooltip_text (button, accel != NULL ? _("Click to change, right-click to remove")
                                                       : _("Click, then press the keys"));
    g_object_set_data (G_OBJECT (button), "index", GUINT_TO_POINTER (i));
    g_object_set_data (G_OBJECT (button), "slot", GINT_TO_POINTER (slot));
    g_signal_connect (button, "clicked", G_CALLBACK (on_key_clicked), jump);
    if (accel != NULL)
    {
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (secondary), GDK_BUTTON_SECONDARY);
        g_signal_connect (secondary, "pressed", G_CALLBACK (on_key_secondary), jump);
        gtk_widget_add_controller (button, GTK_EVENT_CONTROLLER (secondary));
    }
    else
    {
        g_object_unref (secondary);
    }
    return button;
}

static GtkWidget *
command_row (NautilusJump *jump,
             guint         i,
             const char   *query)
{
    GtkWidget *row = row_new (ROW_COMMAND);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *keys_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    g_autofree char *title = highlight (nautilus_shortcuts_command_title (i), query);
    g_auto (GStrv) keys = nautilus_shortcuts_command_keys (i);

    gtk_box_append (GTK_BOX (box), row_icon ("utilities-terminal-symbolic", TRUE));
    gtk_box_append (GTK_BOX (box), two_lines (title, nautilus_shortcuts_command_section (i)));
    for (guint k = 0; keys[k] != NULL && k < 3; k++)
    {
        gtk_box_append (GTK_BOX (keys_box), key_chip (jump, i, (int) k, keys[k]));
    }
    /* On the Commands tab every command can get one more. */
    if (keys[0] == NULL || jump->scope == SCOPE_COMMANDS)
    {
        gtk_box_append (GTK_BOX (keys_box), key_chip (jump, i, -1, NULL));
    }
    if (nautilus_shortcuts_command_changed (i))
    {
        GtkWidget *reset = gtk_button_new_from_icon_name ("edit-undo-symbolic");

        gtk_widget_add_css_class (reset, "flat");
        gtk_widget_add_css_class (reset, "circular");
        gtk_widget_add_css_class (reset, "jump-reset");
        gtk_widget_set_valign (reset, GTK_ALIGN_CENTER);
        gtk_widget_set_focusable (reset, FALSE);
        gtk_widget_set_focus_on_click (reset, FALSE);
        gtk_widget_set_tooltip_text (reset, _("Back to the keys it came with"));
        g_object_set_data (G_OBJECT (reset), "index", GUINT_TO_POINTER (i));
        g_signal_connect (reset, "clicked", G_CALLBACK (on_reset_clicked), jump);
        gtk_box_append (GTK_BOX (keys_box), reset);
    }
    gtk_box_append (GTK_BOX (box), keys_box);

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

static gboolean
on_choices_scroll (GtkEventControllerScroll *controller,
                   double                    dx,
                   double                    dy,
                   gpointer                  user_data)
{
    GtkWidget *box = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
    double step = dy != 0 ? dy : dx;
    GtkWidget *active = NULL;
    GtkWidget *next;

    for (GtkWidget *b = gtk_widget_get_first_child (box); b != NULL; b = gtk_widget_get_next_sibling (b))
    {
        if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (b)))
        {
            active = b;
        }
    }
    if (step == 0)
    {
        return GDK_EVENT_PROPAGATE;
    }
    next = active == NULL ? gtk_widget_get_first_child (box)
           : step > 0 ? gtk_widget_get_next_sibling (active) : gtk_widget_get_prev_sibling (active);
    if (next != NULL)
    {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (next), TRUE);
    }

    /* Not the list under it. */
    return GDK_EVENT_STOP;
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
        {
            /* The wheel steps through the choices. */
            GtkEventController *wheel = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES |
                                                                          GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);

            g_signal_connect (wheel, "scroll", G_CALLBACK (on_choices_scroll), NULL);
            gtk_widget_add_controller (box, wheel);
        }
        return box;
    }
}

static GtkWidget *
setting_row (guint       i,
             const char *query)
{
    GtkWidget *row = row_new (ROW_SETTING);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    g_autofree char *title = highlight (_(settings[i].title), query);

    gtk_box_append (GTK_BOX (box), row_icon ("emblem-system-symbolic", TRUE));
    gtk_box_append (GTK_BOX (box), two_lines (title, _(settings[i].section)));
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
copy_path (NautilusJump *jump,
           GFile        *file)
{
    g_autofree char *path = g_file_get_parse_name (file);
    g_autofree char *message = g_strdup_printf (_("Copied “%s”"), path);
    GtkWindow *window = jump_window (jump);

    gdk_clipboard_set_text (gtk_widget_get_clipboard (jump->entry), path);
    close_entry (jump);
    if (NAUTILUS_IS_WINDOW (window))
    {
        nautilus_window_show_toast (NAUTILUS_WINDOW (window), adw_toast_new (message));
    }
}

/* Opens @folder in the active tile, or in a new tile beside it, with
 * @select selected in it. */
static void
show_in (NautilusJump *jump,
         GFile        *folder,
         GFile        *select,
         gboolean      split)
{
    GtkWindow *window = jump_window (jump);
    g_autoptr (GFile) keep = g_object_ref (folder);
    g_autoptr (NautilusWindowSlot) slot = NULL;
    GList *selection = NULL;

    close_entry (jump);
    if (window == NULL)
    {
        return;
    }
    if (split)
    {
        gtk_widget_activate_action (GTK_WIDGET (window), "win.split-right", NULL);
    }
    g_object_get (window, "active-slot", &slot, NULL);
    if (slot == NULL)
    {
        return;
    }
    if (select != NULL)
    {
        selection = g_list_prepend (NULL, nautilus_file_get (select));
    }
    nautilus_window_slot_open_location_full (slot, keep, selection);
    nautilus_file_list_free (selection);
}

static void
open_file (NautilusJump *jump,
           GFile        *file)
{
    g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);
    GtkWindow *window = jump_window (jump);

    close_entry (jump);
    gtk_file_launcher_launch (launcher, window, NULL, NULL, NULL);
}

static void
activate_row (NautilusJump    *jump,
              GtkListBoxRow   *row,
              GdkModifierType  mods)
{
    RowKind kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "kind"));
    guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), "index"));
    GFile *file = g_object_get_data (G_OBJECT (row), "file");

    if ((kind == ROW_FOLDER || kind == ROW_FILE) && (mods & GDK_ALT_MASK))
    {
        copy_path (jump, file);
        return;
    }

    switch (kind)
    {
        case ROW_FOLDER:
        {
            if (mods & GDK_SHIFT_MASK)
            {
                show_in (jump, file, NULL, TRUE);
            }
            else
            {
                open_folder (jump, file, (mods & GDK_CONTROL_MASK) != 0);
            }
        }
        break;

        case ROW_FILE:
        {
            if (mods & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
            {
                g_autoptr (GFile) parent = g_file_get_parent (file);

                if (parent != NULL)
                {
                    show_in (jump, parent, file, (mods & GDK_SHIFT_MASK) != 0);
                }
            }
            else
            {
                open_file (jump, file);
            }
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
    activate_row (jump, row, 0);
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

/* The folders for @query, best first, each once; borrowed from @list,
 * the current folder's and the index's. */
static void
sort_by_score (GPtrArray  *chosen,
               const char *query,
               gboolean    empty)
{
    g_autoptr (GArray) scores = g_array_new (FALSE, FALSE, sizeof (int));

    for (guint i = 0; i < chosen->len; i++)
    {
        Folder *folder = chosen->pdata[i];
        /* Nothing typed: the folders in use, then the places. */
        int score = !empty ? folder_score (folder, query)
                    : folder->visited ? -(int) (frecency (folder->file) * 100) : 100000 + (int) i;

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
}

/* Each once, in order. */
static GPtrArray *
unique (GPtrArray *chosen)
{
    g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *result = g_ptr_array_new ();

    for (guint i = 0; i < chosen->len; i++)
    {
        Folder *folder = chosen->pdata[i];

        if (g_hash_table_add (seen, g_file_get_uri (folder->file)))
        {
            g_ptr_array_add (result, folder);
        }
    }

    return result;
}

/* The folders for @query, best first, each once; borrowed from @list,
 * @visited, the current folder's and the index's. */
static GPtrArray *
pick_folders (NautilusJump *jump,
              const char   *query,
              GPtrArray    *list,
              GPtrArray    *visited)
{
    gboolean empty = *query == '\0';
    g_autoptr (GPtrArray) chosen = g_ptr_array_new ();

    for (guint i = 0; i < visited->len; i++)
    {
        Folder *folder = visited->pdata[i];

        if (empty ? frecency (folder->file) >= 1.0 : matches_name (query, folder->name))
        {
            g_ptr_array_add (chosen, folder);
        }
    }
    for (guint i = 0; i < list->len; i++)
    {
        Folder *folder = list->pdata[i];

        if (empty || matches_name (query, folder->name))
        {
            g_ptr_array_add (chosen, folder);
        }
    }
    if (!empty)
    {
        GPtrArray *here = folders_here (jump);

        for (guint i = 0; here != NULL && i < here->len; i++)
        {
            if (matches_name (query, ((Folder *) here->pdata[i])->name))
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

    sort_by_score (chosen, query, empty);
    return unique (chosen);
}

static GPtrArray *
pick_files (NautilusJump *jump,
            const char   *query)
{
    g_autoptr (GPtrArray) chosen = g_ptr_array_new ();

    if (*query != '\0' && jump->found_files != NULL && g_strcmp0 (jump->files_for, query) == 0)
    {
        for (guint i = 0; i < jump->found_files->len; i++)
        {
            g_ptr_array_add (chosen, jump->found_files->pdata[i]);
        }
    }
    sort_by_score (chosen, query, FALSE);
    return unique (chosen);
}

/* Indexes of the commands (@commands) or settings that match, those whose
 * title starts with what is typed first. */
static GArray *
pick (const char *query,
      gboolean    commands)
{
    gboolean empty = *query == '\0';
    guint n = commands ? nautilus_shortcuts_n_commands () : G_N_ELEMENTS (settings);
    GArray *result = g_array_new (FALSE, FALSE, sizeof (guint));

    /* Titles starting with what was typed, then any word matching, then
     * titles with its letters in order ("ql" for Quick Look). */
    for (guint pass = 0; pass < 3; pass++)
    {
        for (guint i = 0; i < n; i++)
        {
            const char *title = commands ? nautilus_shortcuts_command_title (i) : _(settings[i].title);
            g_autofree char *haystack = g_strdup_printf ("%s %s", title,
                                                         commands ? nautilus_shortcuts_command_section (i)
                                                                  : settings[i].words);
            gboolean prefix = !empty && g_str_match_string (query, title, TRUE);
            gboolean word = empty || matches (query, haystack);
            guint wanted = empty || prefix ? 0 : word ? 1 : fuzzy (query, title) ? 2 : 3;

            if ((commands && g_str_has_prefix (nautilus_shortcuts_command_action (i), "toolbar.jump-")) ||
                wanted != pass)
            {
                continue;
            }
            g_array_append_val (result, i);
        }
    }

    return result;
}

static void
heading_row (NautilusJump *jump,
             const char   *title,
             guint         more)
{
    GtkWidget *row = row_new (ROW_NONE);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    g_autofree char *upper = g_utf8_strup (title, -1);
    GtkWidget *label = gtk_label_new (upper);

    gtk_widget_remove_css_class (row, "jump-row");
    gtk_widget_add_css_class (row, "jump-heading-row");
    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_widget_add_css_class (label, "jump-heading");
    gtk_box_append (GTK_BOX (box), label);
    if (more > 0)
    {
        g_autofree char *text = g_strdup_printf (_("%u more · Tab"), more);
        GtkWidget *rest = gtk_label_new (text);

        gtk_widget_add_css_class (rest, "jump-heading");
        gtk_box_append (GTK_BOX (box), rest);
    }
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
    add_row (jump, row);
}

static void
set_count (NautilusJump *jump,
           Scope         scope,
           guint         count,
           gboolean      show)
{
    g_autofree char *text = g_strdup_printf ("%u", count);

    gtk_label_set_text (GTK_LABEL (jump->counts[scope]), text);
    gtk_widget_set_visible (jump->counts[scope], show);
}

static void
rebuild (NautilusJump *jump)
{
    const char *query = jump->query != NULL ? jump->query : "";
    gboolean empty = *query == '\0';
    gboolean all = jump->scope == SCOPE_ALL;
    g_autoptr (GPtrArray) place_list = places ();
    g_autoptr (GPtrArray) visited = visited_folders ();
    g_autoptr (GPtrArray) folders = pick_folders (jump, query, place_list, visited);
    g_autoptr (GPtrArray) files = pick_files (jump, query);
    g_autoptr (GArray) commands = pick (query, TRUE);
    g_autoptr (GArray) found_settings = pick (query, FALSE);
    GtkWidget *child;
    guint shown = 0;
    guint limit;

    while ((child = gtk_widget_get_first_child (GTK_WIDGET (jump->list))) != NULL)
    {
        gtk_list_box_remove (jump->list, child);
    }

    set_count (jump, SCOPE_ALL, folders->len + files->len + commands->len + found_settings->len, !empty);
    set_count (jump, SCOPE_FOLDERS, folders->len, !empty);
    set_count (jump, SCOPE_FILES, files->len, !empty);
    set_count (jump, SCOPE_COMMANDS, commands->len, !empty);
    set_count (jump, SCOPE_SETTINGS, found_settings->len, !empty);

    if ((all || jump->scope == SCOPE_FOLDERS) && folders->len > 0)
    {
        limit = all ? 5 : 60;
        heading_row (jump, empty ? _("Recent Folders and Places") : _("Folders"),
                     all && folders->len > limit ? folders->len - limit : 0);
        for (guint i = 0; i < folders->len && i < limit; i++, shown++)
        {
            add_row (jump, folder_row (jump, folders->pdata[i], query));
        }
    }

    if ((all || jump->scope == SCOPE_FILES) && files->len > 0)
    {
        limit = all ? 5 : 60;
        heading_row (jump, _("Files"), all && files->len > limit ? files->len - limit : 0);
        for (guint i = 0; i < files->len && i < limit; i++, shown++)
        {
            add_row (jump, file_row (jump, files->pdata[i], query));
        }
    }

    if ((all || jump->scope == SCOPE_COMMANDS) && commands->len > 0)
    {
        limit = all ? (empty ? 4 : 5) : G_MAXUINT;
        heading_row (jump, _("Commands"), all && commands->len > limit ? commands->len - limit : 0);
        for (guint i = 0; i < commands->len && i < limit; i++, shown++)
        {
            add_row (jump, command_row (jump, g_array_index (commands, guint, i), query));
        }
    }

    if ((all || jump->scope == SCOPE_SETTINGS) && found_settings->len > 0)
    {
        limit = all ? (empty ? 3 : 5) : G_MAXUINT;
        heading_row (jump, _("Settings"), all && found_settings->len > limit ? found_settings->len - limit : 0);
        for (guint i = 0; i < found_settings->len && i < limit; i++, shown++)
        {
            add_row (jump, setting_row (g_array_index (found_settings, guint, i), query));
        }
    }

    if (shown == 0)
    {
        GtkWidget *row = row_new (ROW_NONE);
        gboolean looking = !empty &&
                           ((jump->scope == SCOPE_FOLDERS && g_strcmp0 (jump->found_for, query) != 0) ||
                            (jump->scope == SCOPE_FILES && g_strcmp0 (jump->files_for, query) != 0));
        g_autofree char *text = empty ? g_strdup (_("Type a name"))
                                : looking ? g_strdup (_("Looking…"))
                                : g_strdup_printf (_("Nothing called “%s”."), query);
        GtkWidget *label = gtk_label_new (text);

        gtk_widget_add_css_class (label, "dim-label");
        gtk_label_set_xalign (GTK_LABEL (label), 0);
        gtk_widget_set_margin_top (label, 8);
        gtk_widget_set_margin_bottom (label, 8);
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
        gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
        gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
        add_row (jump, row);
    }

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

static gboolean
rebuild_later (gpointer popover)
{
    NautilusJump *jump = g_object_get_data (G_OBJECT (popover), "jump");

    if (jump != NULL && gtk_widget_get_visible (popover))
    {
        rebuild (jump);
    }
    g_object_unref (popover);
    return G_SOURCE_REMOVE;
}

/* Not at once: the button that changed them is in a row being replaced. */
static void
on_keys_changed (GtkWidget *popover)
{
    g_idle_add (rebuild_later, g_object_ref (popover));
}

static void
follow_accent (AdwStyleManager *style,
               GParamSpec      *pspec,
               GtkWidget       *popover)
{
    if (adw_style_manager_get_accent_color (style) == ADW_ACCENT_COLOR_PURPLE)
    {
        gtk_widget_add_css_class (popover, "jump-violet");
    }
    else
    {
        gtk_widget_remove_css_class (popover, "jump-violet");
    }
}

NautilusJump *
nautilus_jump_new (GtkWidget *entry,
                   GtkWidget *anchor)
{
    static const char *names[] = { N_("All"), N_("Folders"), N_("Files"), N_("Commands"), N_("Settings") };
    NautilusJump *jump = g_new0 (NautilusJump, 1);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *hint = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
    static const char *hints[][2] =
    {
        { "↑ ↓", N_("choose") },
        { N_("Enter"), N_("open") },
        { N_("Ctrl+Enter"), N_("new tab") },
        { N_("Shift+Enter"), N_("split") },
        { N_("Alt+Enter"), N_("copy path") },
        { N_("Tab"), N_("next group") },
    };

    jump->entry = entry;
    jump->anchor = anchor;

    for (int i = 0; i < N_SCOPES; i++)
    {
        GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

        gtk_box_append (GTK_BOX (content), gtk_label_new (_(names[i])));
        jump->counts[i] = gtk_label_new (NULL);
        gtk_widget_add_css_class (jump->counts[i], "jump-count");
        gtk_widget_set_visible (jump->counts[i], FALSE);
        gtk_box_append (GTK_BOX (content), jump->counts[i]);
        jump->scopes[i] = gtk_toggle_button_new ();
        gtk_button_set_child (GTK_BUTTON (jump->scopes[i]), content);
        gtk_widget_set_focusable (jump->scopes[i], FALSE);
        gtk_widget_set_focus_on_click (jump->scopes[i], FALSE);
        gtk_widget_add_css_class (jump->scopes[i], "flat");
        gtk_widget_add_css_class (jump->scopes[i], "jump-scope");
        g_object_set_data (G_OBJECT (jump->scopes[i]), "scope", GINT_TO_POINTER (i));
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (jump->scopes[i]), i == 0);
        g_signal_connect (jump->scopes[i], "toggled", G_CALLBACK (on_scope_toggled), jump);
        gtk_box_append (GTK_BOX (bar), jump->scopes[i]);
    }
    gtk_widget_add_css_class (bar, "jump-scopes");
    gtk_box_append (GTK_BOX (box), bar);

    jump->list = GTK_LIST_BOX (gtk_list_box_new ());
    gtk_widget_set_focusable (GTK_WIDGET (jump->list), FALSE);
    gtk_list_box_set_selection_mode (jump->list, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (jump->list, TRUE);
    gtk_widget_add_css_class (GTK_WIDGET (jump->list), "jump-list");
    g_signal_connect (jump->list, "row-activated", G_CALLBACK (on_row_activated), jump);

    jump->scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
    gtk_scrolled_window_set_policy (jump->scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (jump->scroller, TRUE);
    gtk_scrolled_window_set_max_content_height (jump->scroller, 460);
    gtk_scrolled_window_set_child (jump->scroller, GTK_WIDGET (jump->list));
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (jump->scroller));

    for (guint i = 0; i < G_N_ELEMENTS (hints); i++)
    {
        GtkWidget *pair = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
        GtkWidget *what = gtk_label_new (_(hints[i][1]));
        g_auto (GStrv) keys = g_strsplit (_(hints[i][0]), " ", -1);

        for (guint k = 0; keys[k] != NULL; k++)
        {
            GtkWidget *kbd = gtk_label_new (keys[k]);

            gtk_widget_add_css_class (kbd, "jump-kbd");
            gtk_box_append (GTK_BOX (pair), kbd);
        }
        gtk_widget_add_css_class (what, "jump-hint-text");
        gtk_box_append (GTK_BOX (pair), what);
        gtk_box_append (GTK_BOX (hint), pair);
    }
    gtk_widget_add_css_class (hint, "jump-hint");
    gtk_box_append (GTK_BOX (box), hint);

    jump->popover = gtk_popover_new ();
    gtk_popover_set_autohide (GTK_POPOVER (jump->popover), FALSE);
    gtk_popover_set_has_arrow (GTK_POPOVER (jump->popover), FALSE);
    gtk_popover_set_position (GTK_POPOVER (jump->popover), GTK_POS_BOTTOM);
    /* A little apart from the path bar. */
    gtk_popover_set_offset (GTK_POPOVER (jump->popover), 0, 8);
    gtk_widget_set_focusable (jump->popover, FALSE);
    gtk_widget_add_css_class (jump->popover, "jump-menu");
    g_signal_connect_object (adw_style_manager_get_default (), "notify::accent-color",
                             G_CALLBACK (follow_accent), jump->popover, 0);
    follow_accent (adw_style_manager_get_default (), NULL, jump->popover);
    gtk_popover_set_child (GTK_POPOVER (jump->popover), box);
    gtk_widget_set_parent (jump->popover, anchor);
    g_signal_connect_object (nautilus_flier_settings (), "changed::custom-shortcuts",
                             G_CALLBACK (on_keys_changed), jump->popover, G_CONNECT_SWAPPED);
    g_object_set_data (G_OBJECT (jump->popover), "jump", jump);

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
    g_clear_object (&jump->statement_files);
    g_clear_pointer (&jump->found, g_ptr_array_unref);
    g_clear_pointer (&jump->found_files, g_ptr_array_unref);
    g_free (jump->files_for);
    g_free (jump->last_query);
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
        /* For next time. */
        if (jump->query != NULL && *jump->query != '\0')
        {
            g_free (jump->last_query);
            jump->last_query = g_strdup (jump->query);
            jump->last_scope = jump->scope;
        }
        gtk_popover_popdown (GTK_POPOVER (jump->popover));
    }
}

static void
size_popover (NautilusJump *jump)
{
    gtk_widget_set_size_request (gtk_popover_get_child (GTK_POPOVER (jump->popover)),
                                 MAX (gtk_widget_get_width (jump->anchor) + 48, 560), -1);
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

void
nautilus_jump_restore (NautilusJump *jump)
{
    Scope scope = jump->last_scope;

    if (jump->last_query == NULL)
    {
        return;
    }
    /* Selected, so typing starts afresh. */
    gtk_editable_set_text (GTK_EDITABLE (jump->entry), jump->last_query);
    gtk_editable_select_region (GTK_EDITABLE (jump->entry), 0, -1);
    nautilus_jump_update (jump);
    if (gtk_widget_get_visible (jump->popover) && scope != SCOPE_ALL)
    {
        set_scope (jump, scope);
    }
}

void
nautilus_jump_show_commands (NautilusJump *jump)
{
    nautilus_jump_update (jump);
    if (gtk_widget_get_visible (jump->popover))
    {
        set_scope (jump, SCOPE_COMMANDS);
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
            activate_row (jump, selected, mods);
            return TRUE;
        }

        default:
        {
        }
        break;
    }

    return FALSE;
}
