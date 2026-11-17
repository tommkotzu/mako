/*
 * nautilus-ql-canvas.c: the zoomable canvas of Quick Look
 *
 * Several files are laid out in a grid with about the canvas's shape, each
 * a thumbnail (NautilusImage, so thumbnails are shared with the views).
 * Children are allocated at one size and scaled by a transform, so zooming
 * costs no relayout and no reloading. Pictures looked at closely, and a
 * picture shown alone, are loaded in full with glycin.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nautilus-ql-canvas.h"

#include <math.h>
#include <string.h>

#include <adwaita.h>
#include <glycin.h>
#include <glycin-gtk4.h>

#include "nautilus-file.h"
#include "nautilus-image.h"
#include "nautilus-labels.h"
#include "nautilus-text-preview.h"

/* Several items are laid out bento style: in rows of one height each,
 * every picture at its own shape, each row filling the width. PICTURE is
 * the row height aimed for (in canvas units, before zoom); TILE the size
 * the zoom thresholds (marks) are measured against. */
#define TILE 200.0
#define GAP 6.0
#define PICTURE 168.0
#define NAME_ROOM 26.0
/* Row widths tried when fitting the canvas's shape. */
#define WIDTH_STEPS 240
/* How much a picture's cell may differ from its shape (the picture then
 * covers it, cropped), so the whole fills the canvas's shape. */
#define MAX_CROP 0.12
#define MARGIN 16.0
#define MAX_ITEMS 400
/* Full pictures kept at once on a canvas of several. */
#define MAX_FULL 6
/* Load the full picture once a thumbnail would show bigger than this. */
#define FULL_FROM_PX 380.0

typedef struct
{
    NautilusFile *file;
    GtkWidget *image;          /* NautilusImage, the thumbnail */
    GdkTexture *full;          /* the whole picture, once loaded */
    GCancellable *loading;
    gboolean is_picture;
    gboolean full_failed;
    PangoLayout *name;
    gulong changed_id;
    char *text;                /* the start of a text file, drawn as a page */
    PangoLayout *text_layout;
    GCancellable *text_loading;
    GtkWidget *canvas;
} Item;

/* How much of a text file a page shows. */
#define TEXT_PAGE_BYTES 3000
#define TEXT_PAGE_LINES 48

struct _NautilusQlCanvas
{
    GtkWidget parent_instance;

    GPtrArray *items;

    double zoom;
    double pan_x, pan_y;
    gboolean at_fit;

    double pointer_x, pointer_y;
    gboolean pointer_in;
    int hovered;

    double drag_pan_x, drag_pan_y;
    gboolean dragging;
    double pinch_zoom;

    gboolean show_names;
    gboolean show_marks;

    /* The bento layout: each item's picture in canvas units, its row, and
     * the size of the whole. */
    GArray *cells;
    double content_w, content_h;

    guint full_idle;
};

G_DEFINE_TYPE (NautilusQlCanvas, nautilus_ql_canvas, GTK_TYPE_WIDGET)

enum
{
    PROP_0,
    PROP_SHOW_NAMES,
    PROP_SHOW_MARKS,
    N_PROPS
};

enum
{
    ITEM_ACTIVATED,
    PICTURE_LOADED,
    N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

static void schedule_full_loads (NautilusQlCanvas *self);
static void set_hovered (NautilusQlCanvas *self, int hovered);
static void snapshot_text_page (NautilusQlCanvas *self, GtkSnapshot *snapshot, Item *item, const graphene_rect_t *pic);

static void
item_free (Item *item)
{
    g_cancellable_cancel (item->loading);
    g_clear_object (&item->loading);
    g_clear_object (&item->full);
    g_clear_object (&item->name);
    g_cancellable_cancel (item->text_loading);
    g_clear_object (&item->text_loading);
    g_clear_object (&item->text_layout);
    g_free (item->text);
    g_clear_signal_handler (&item->changed_id, item->file);
    if (item->image != NULL)
    {
        gtk_widget_unparent (item->image);
    }
    nautilus_file_unref (item->file);
    g_free (item);
}

static Item *
get_item (NautilusQlCanvas *self,
          guint             i)
{
    return g_ptr_array_index (self->items, i);
}

static gboolean
is_single (NautilusQlCanvas *self)
{
    return self->items->len == 1;
}

/* ----------------------------------------------------------- Geometry */

typedef struct
{
    graphene_rect_t pic;
    int row;
} Cell;

/* An item's width over height. */
static double
item_aspect (NautilusQlCanvas *self,
             guint             i)
{
    Item *item = get_item (self, i);
    double w, h;

    if (item->full != NULL)
    {
        w = gdk_texture_get_width (item->full);
        h = gdk_texture_get_height (item->full);
    }
    else if (item->text != NULL)
    {
        return 0.77;
    }
    else
    {
        int nat_w, nat_h;

        gtk_widget_measure (item->image, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &nat_w, NULL, NULL);
        gtk_widget_measure (item->image, GTK_ORIENTATION_VERTICAL, -1, NULL, &nat_h, NULL, NULL);
        w = MAX (nat_w, 1);
        h = MAX (nat_h, 1);
    }
    /* A panorama or a strip still gets a usable cell. */
    return CLAMP (w / h, 0.3, 4.0);
}

/* Justified rows WIDTH wide: items join a row until it would be wider at
 * the aimed-for height, then the row is scaled to fill the width exactly.
 * The last row grows at most half again as tall, then is centred. */
static double
layout_rows (NautilusQlCanvas *self,
             const double     *aspects,
             double            width,
             Cell             *cells)
{
    guint n = self->items->len;
    double name = self->show_names ? NAME_ROOM : 0;
    double y = 0;
    int row = 0;

    for (guint i = 0; i < n;)
    {
        guint j = i;
        double sum = 0;
        double h, row_w, x;

        while (j < n)
        {
            sum += aspects[j++];
            if (sum * PICTURE + (j - i - 1) * GAP >= width)
            {
                break;
            }
        }
        h = (width - (j - i - 1) * GAP) / sum;
        /* A short last row grows a little to fill the width, then stops
         * and is centred. */
        if (j == n)
        {
            h = MIN (h, PICTURE * 1.5);
        }
        row_w = sum * h + (j - i - 1) * GAP;
        x = (width - row_w) / 2;
        for (guint k = i; k < j; k++)
        {
            cells[k].pic = GRAPHENE_RECT_INIT (x, y, aspects[k] * h, h);
            cells[k].row = row;
            x += aspects[k] * h + GAP;
        }
        y += h + name + GAP;
        row++;
        i = j;
    }
    return y - GAP;
}

/* Rows a bit taller or shorter so the whole takes the canvas's shape:
 * by FACTOR at most MAX_CROP, the widths kept, each picture cropped to its
 * cell. */
static void
stretch_rows (NautilusQlCanvas *self,
              double            target_h)
{
    Cell *cells = (Cell *) self->cells->data;
    guint n = self->cells->len;
    double name = self->show_names ? NAME_ROOM : 0;
    double pictures = 0, other, factor, y = 0;
    int rows = cells[n - 1].row + 1;

    for (guint i = 0; i < n; i++)
    {
        if (i == 0 || cells[i].row != cells[i - 1].row)
        {
            pictures += cells[i].pic.size.height;
        }
    }
    other = self->content_h - pictures;
    factor = CLAMP ((target_h - other) / pictures, 1 - MAX_CROP, 1 + MAX_CROP);

    for (int row = 0, i = 0; row < rows; row++)
    {
        double h = cells[i].pic.size.height * factor;

        for (; i < (int) n && cells[i].row == row; i++)
        {
            cells[i].pic.origin.y = y;
            cells[i].pic.size.height = h;
        }
        y += h + name + GAP;
    }
    self->content_h = y - GAP;
}

/* Lays the items out for the canvas's current size: of the row widths
 * from the widest picture alone to all in one row, the one whose pictures
 * show biggest; then the rows are stretched a little to fill the rest. */
static void
update_layout (NautilusQlCanvas *self)
{
    guint n = self->items->len;
    double avail_w = MAX (gtk_widget_get_width (GTK_WIDGET (self)) - 2 * MARGIN, 1);
    double avail_h = MAX (gtk_widget_get_height (GTK_WIDGET (self)) - 2 * MARGIN, 1);
    g_autofree double *aspects = g_new (double, MAX (n, 1));
    g_autofree Cell *trial = g_new (Cell, MAX (n, 1));
    double sum = 0, widest = 0, best = -1, min_w, max_w;

    g_array_set_size (self->cells, n);
    if (n < 2)
    {
        return;
    }
    for (guint i = 0; i < n; i++)
    {
        aspects[i] = item_aspect (self, i);
        sum += aspects[i];
        widest = MAX (widest, aspects[i]);
    }
    min_w = widest * PICTURE;
    max_w = MAX (sum * PICTURE + (n - 1) * GAP, min_w);
    for (int step = 0; step <= WIDTH_STEPS; step++)
    {
        /* Spread evenly on a log scale: as many tries for few rows as for many. */
        double width = min_w * pow (max_w / min_w, step / (double) WIDTH_STEPS);
        double height = layout_rows (self, aspects, width, trial);
        double fit_w = avail_w / width;
        double fit_h = avail_h / height;
        /* Counting what stretching wins back: too wide fills out at no
         * cost, too tall shrinks by up to MAX_CROP. */
        double fit = MIN (fit_w, fit_h / (1 - MAX_CROP));

        if (fit > best)
        {
            best = fit;
            self->content_w = width;
            self->content_h = height;
            memcpy (self->cells->data, trial, n * sizeof (Cell));
        }
    }
    stretch_rows (self, self->content_w * avail_h / avail_w);
}

static Cell *
get_cell (NautilusQlCanvas *self,
          guint             i)
{
    if (self->cells->len != self->items->len)
    {
        update_layout (self);
    }
    return &g_array_index (self->cells, Cell, i);
}

static void
content_size (NautilusQlCanvas *self,
              double           *width,
              double           *height)
{
    if (is_single (self))
    {
        Item *item = get_item (self, 0);
        int scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));

        if (item->full != NULL)
        {
            *width = gdk_texture_get_width (item->full) / (double) scale;
            *height = gdk_texture_get_height (item->full) / (double) scale;
        }
        else
        {
            *width = *height = PICTURE;
        }
        return;
    }

    if (self->cells->len != self->items->len)
    {
        update_layout (self);
    }
    *width = self->content_w;
    *height = self->content_h;
}

static double
fit_zoom (NautilusQlCanvas *self)
{
    double cw, ch;
    double width = gtk_widget_get_width (GTK_WIDGET (self)) - 2 * MARGIN;
    double height = gtk_widget_get_height (GTK_WIDGET (self)) - 2 * MARGIN;
    double fit;

    content_size (self, &cw, &ch);
    fit = CLAMP (MIN (width / cw, height / ch), 0.02, 2.5);

    /* A picture alone is never blown up past its real size; a thumbnail
     * standing in for it while it loads is. */
    if (is_single (self) && get_item (self, 0)->full != NULL)
    {
        fit = MIN (fit, 1.0);
    }

    return fit;
}

/* The space an item takes on the canvas, its name included, in widget
 * coordinates. */
static graphene_rect_t
tile_rect (NautilusQlCanvas *self,
           guint             i)
{
    double cw, ch;
    double cx = gtk_widget_get_width (GTK_WIDGET (self)) / 2.0 + self->pan_x;
    double cy = gtk_widget_get_height (GTK_WIDGET (self)) / 2.0 + self->pan_y;
    double z = self->zoom;

    content_size (self, &cw, &ch);

    if (is_single (self))
    {
        return GRAPHENE_RECT_INIT (cx - cw * z / 2, cy - ch * z / 2, cw * z, ch * z);
    }

    Cell *cell = get_cell (self, i);
    double name = self->show_names ? NAME_ROOM : 0;

    return GRAPHENE_RECT_INIT (cx + (cell->pic.origin.x - cw / 2) * z,
                               cy + (cell->pic.origin.y - ch / 2) * z,
                               cell->pic.size.width * z,
                               (cell->pic.size.height + name) * z);
}

/* The box the picture is fitted into, inside its tile. */
static graphene_rect_t
picture_box (NautilusQlCanvas *self,
             guint             i)
{
    graphene_rect_t tile = tile_rect (self, i);
    double z = self->zoom;

    if (is_single (self))
    {
        return tile;
    }

    return GRAPHENE_RECT_INIT (tile.origin.x, tile.origin.y, tile.size.width,
                               get_cell (self, i)->pic.size.height * z);
}

/* Where the picture itself is drawn: its box, fitted to its shape. */
static graphene_rect_t
picture_rect (NautilusQlCanvas *self,
              guint             i)
{
    Item *item = get_item (self, i);
    graphene_rect_t box = picture_box (self, i);
    double w, h;

    if (item->full != NULL)
    {
        w = gdk_texture_get_width (item->full);
        h = gdk_texture_get_height (item->full);
    }
    else
    {
        int nat_w, nat_h;

        gtk_widget_measure (item->image, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &nat_w, NULL, NULL);
        gtk_widget_measure (item->image, GTK_ORIENTATION_VERTICAL, -1, NULL, &nat_h, NULL, NULL);
        w = MAX (nat_w, 1);
        h = MAX (nat_h, 1);
    }

    /* On a canvas of several a picture covers its cell, which may be a
     * little off its shape (cropped by the drawing); anything else fits. */
    double s = !is_single (self) && (item->is_picture || item->full != NULL)
               ? MAX (box.size.width / w, box.size.height / h)
               : MIN (box.size.width / w, box.size.height / h);

    return GRAPHENE_RECT_INIT (box.origin.x + (box.size.width - w * s) / 2,
                               box.origin.y + (box.size.height - h * s) / 2,
                               w * s, h * s);
}

static gboolean
rect_visible (NautilusQlCanvas      *self,
              const graphene_rect_t *r)
{
    graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0,
                                                 gtk_widget_get_width (GTK_WIDGET (self)),
                                                 gtk_widget_get_height (GTK_WIDGET (self)));

    return graphene_rect_intersection (&bounds, r, NULL);
}

static int
item_at (NautilusQlCanvas *self,
         double            x,
         double            y)
{
    graphene_point_t p = GRAPHENE_POINT_INIT (x, y);

    for (guint i = 0; i < self->items->len; i++)
    {
        graphene_rect_t r = tile_rect (self, i);

        if (graphene_rect_contains_point (&r, &p))
        {
            return i;
        }
    }

    return -1;
}

static gboolean
marks_wanted (NautilusQlCanvas *self)
{
    return self->show_marks && !is_single (self) && self->zoom * TILE >= 70;
}

static gboolean
stars_bounds (NautilusQlCanvas *self,
              guint             i,
              graphene_rect_t  *bounds)
{
    Item *item = get_item (self, i);
    graphene_rect_t pic;
    float size;

    if (!marks_wanted (self) ||
        !g_settings_get_boolean (nautilus_flier_settings (), "ratings") ||
        (nautilus_file_get_rating (item->file) == 0 && (int) i != self->hovered))
    {
        return FALSE;
    }

    /* Where it's drawn: the cell, as the picture may be cropped to it. */
    pic = picture_box (self, i);
    size = CLAMP (self->zoom * TILE / 12, 10, 17);
    *bounds = GRAPHENE_RECT_INIT (pic.origin.x + (pic.size.width - 5 * size) / 2,
                                  pic.origin.y + pic.size.height - size - 4,
                                  5 * size, size);
    return TRUE;
}

/* ------------------------------------------------------------ Zooming */

static void
zoom_at (NautilusQlCanvas *self,
         double            x,
         double            y,
         double            factor)
{
    double fit = fit_zoom (self);
    double z = CLAMP (self->zoom * factor, fit * 0.5, MAX (8.0, fit));
    double cx = gtk_widget_get_width (GTK_WIDGET (self)) / 2.0;
    double cy = gtk_widget_get_height (GTK_WIDGET (self)) / 2.0;

    /* Keep the point under the pointer where it is. */
    double qx = (x - cx - self->pan_x) / self->zoom;
    double qy = (y - cy - self->pan_y) / self->zoom;

    self->pan_x = x - cx - qx * z;
    self->pan_y = y - cy - qy * z;
    self->zoom = z;
    self->at_fit = FALSE;

    gtk_widget_queue_allocate (GTK_WIDGET (self));
    schedule_full_loads (self);
}

void
nautilus_ql_canvas_zoom (NautilusQlCanvas *self,
                         double            factor)
{
    double x = gtk_widget_get_width (GTK_WIDGET (self)) / 2.0;
    double y = gtk_widget_get_height (GTK_WIDGET (self)) / 2.0;

    if (self->pointer_in)
    {
        x = self->pointer_x;
        y = self->pointer_y;
    }

    zoom_at (self, x, y, factor);
}

void
nautilus_ql_canvas_fit (NautilusQlCanvas *self)
{
    self->at_fit = TRUE;
    gtk_widget_queue_allocate (GTK_WIDGET (self));
    schedule_full_loads (self);
}

void
nautilus_ql_canvas_actual_size (NautilusQlCanvas *self)
{
    if (!is_single (self) || get_item (self, 0)->full == NULL)
    {
        return;
    }

    self->zoom = 1.0;
    self->pan_x = self->pan_y = 0;
    self->at_fit = FALSE;
    gtk_widget_queue_allocate (GTK_WIDGET (self));
}

void
nautilus_ql_canvas_pan (NautilusQlCanvas *self,
                        double            dx,
                        double            dy)
{
    self->pan_x += dx;
    self->pan_y += dy;
    self->at_fit = FALSE;
    gtk_widget_queue_allocate (GTK_WIDGET (self));
    schedule_full_loads (self);
}

gboolean
nautilus_ql_canvas_get_at_fit (NautilusQlCanvas *self)
{
    return self->at_fit;
}

/* Moves the highlighted item with the arrow keys: on a grid like the grid
 * view's (SPATIAL), else back (left, up) and forward (right, down) as in a
 * list. */
void
nautilus_ql_canvas_move_hovered (NautilusQlCanvas *self,
                                 GtkDirectionType  direction,
                                 gboolean          spatial)
{
    int n = self->items->len;
    int i = self->hovered;

    if (n < 2)
    {
        return;
    }
    if (i < 0 || i >= n)
    {
        set_hovered (self, 0);
        return;
    }

    if (!spatial)
    {
        i += (direction == GTK_DIR_LEFT || direction == GTK_DIR_UP) ? -1 : 1;
    }
    else
    {
        int row = get_cell (self, i)->row;
        int last_row = get_cell (self, n - 1)->row;

        switch (direction)
        {
            case GTK_DIR_LEFT:
            {
                i -= i > 0 && get_cell (self, i - 1)->row == row ? 1 : 0;
            }
            break;

            case GTK_DIR_RIGHT:
            {
                i += i < n - 1 && get_cell (self, i + 1)->row == row ? 1 : 0;
            }
            break;

            case GTK_DIR_UP:
            case GTK_DIR_DOWN:
            {
                /* The item in the row above or below nearest across. */
                int to = row + (direction == GTK_DIR_UP ? -1 : 1);
                graphene_rect_t *from = &get_cell (self, i)->pic;
                double x = from->origin.x + from->size.width / 2;
                double nearest = G_MAXDOUBLE;

                if (to < 0 || to > last_row)
                {
                    break;
                }
                for (int k = 0; k < n; k++)
                {
                    graphene_rect_t *r = &get_cell (self, k)->pic;
                    double d = fabs (r->origin.x + r->size.width / 2 - x);

                    if (get_cell (self, k)->row == to && d < nearest)
                    {
                        nearest = d;
                        i = k;
                    }
                }
            }
            break;

            default:
            break;
        }
    }

    set_hovered (self, CLAMP (i, 0, n - 1));
}

/* ----------------------------------------------------- Full pictures */

typedef struct
{
    GWeakRef canvas;
    GCancellable *cancellable;
} LoadData;

static void
load_data_free (LoadData *data)
{
    g_weak_ref_clear (&data->canvas);
    g_object_unref (data->cancellable);
    g_free (data);
}

static Item *
item_for_load (NautilusQlCanvas *self,
               GCancellable     *cancellable)
{
    for (guint i = 0; i < self->items->len; i++)
    {
        if (get_item (self, i)->loading == cancellable)
        {
            return get_item (self, i);
        }
    }

    return NULL;
}

static void
on_frame_loaded (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
    LoadData *data = user_data;
    g_autoptr (GError) error = NULL;
    g_autoptr (GlyFrame) frame = gly_image_next_frame_finish (GLY_IMAGE (source), result, &error);
    g_autoptr (NautilusQlCanvas) self = g_weak_ref_get (&data->canvas);
    Item *item = self != NULL ? item_for_load (self, data->cancellable) : NULL;

    if (item != NULL)
    {
        g_clear_object (&item->loading);
        if (frame != NULL)
        {
            item->full = gly_gtk_frame_get_texture (frame);
            if (is_single (self))
            {
                g_signal_emit (self, signals[PICTURE_LOADED], 0);
            }
        }
        else
        {
            item->full_failed = TRUE;
        }
        gtk_widget_queue_allocate (GTK_WIDGET (self));
        schedule_full_loads (self);
    }

    load_data_free (data);
}

static void
on_image_loaded (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
    LoadData *data = user_data;
    g_autoptr (GError) error = NULL;
    g_autoptr (GlyImage) image = gly_loader_load_finish (GLY_LOADER (source), result, &error);

    if (image == NULL)
    {
        g_autoptr (NautilusQlCanvas) self = g_weak_ref_get (&data->canvas);
        Item *item = self != NULL ? item_for_load (self, data->cancellable) : NULL;

        if (item != NULL)
        {
            g_clear_object (&item->loading);
            item->full_failed = TRUE;
        }
        load_data_free (data);
        return;
    }

    gly_image_next_frame_async (image, data->cancellable, on_frame_loaded, data);
}

static void
start_full_load (NautilusQlCanvas *self,
                 Item             *item)
{
    g_autoptr (GFile) location = nautilus_file_get_location (item->file);
    g_autoptr (GlyLoader) loader = gly_loader_new (location);
    LoadData *data = g_new0 (LoadData, 1);

    item->loading = g_cancellable_new ();
    g_weak_ref_init (&data->canvas, self);
    data->cancellable = g_object_ref (item->loading);

    gly_loader_load_async (loader, item->loading, on_image_loaded, data);
}

static gboolean
full_wanted (NautilusQlCanvas *self,
             guint             i)
{
    Item *item = get_item (self, i);
    graphene_rect_t box;

    if (!item->is_picture || item->full_failed)
    {
        return FALSE;
    }
    if (is_single (self))
    {
        return TRUE;
    }

    box = picture_box (self, i);

    return rect_visible (self, &box) &&
           box.size.width * gtk_widget_get_scale_factor (GTK_WIDGET (self)) > FULL_FROM_PX;
}

static gboolean
update_full_loads (gpointer user_data)
{
    NautilusQlCanvas *self = user_data;
    guint loading = 0, loaded = 0;

    self->full_idle = 0;

    /* Let go of the ones no longer needed first, to keep memory low. */
    for (guint i = 0; i < self->items->len; i++)
    {
        Item *item = get_item (self, i);

        if (!full_wanted (self, i))
        {
            g_cancellable_cancel (item->loading);
            g_clear_object (&item->loading);
            if (item->full != NULL)
            {
                g_clear_object (&item->full);
                gtk_widget_queue_allocate (GTK_WIDGET (self));
            }
        }
        loading += item->loading != NULL;
        loaded += item->full != NULL;
    }

    for (guint i = 0; i < self->items->len && loading < 2 && loading + loaded < MAX_FULL; i++)
    {
        Item *item = get_item (self, i);

        if (item->full == NULL && item->loading == NULL && full_wanted (self, i))
        {
            start_full_load (self, item);
            loading++;
        }
    }

    return G_SOURCE_REMOVE;
}

static void
schedule_full_loads (NautilusQlCanvas *self)
{
    if (self->full_idle == 0)
    {
        /* After a gesture settles, not on every step of it. */
        self->full_idle = g_timeout_add (120, update_full_loads, self);
    }
}

/* ----------------------------------------------------------- Drawing */

static void
nautilus_ql_canvas_size_allocate (GtkWidget *widget,
                                  int        width,
                                  int        height,
                                  int        baseline)
{
    NautilusQlCanvas *self = NAUTILUS_QL_CANVAS (widget);

    update_layout (self);
    if (self->at_fit)
    {
        self->zoom = fit_zoom (self);
        self->pan_x = self->pan_y = 0;
    }

    for (guint i = 0; i < self->items->len; i++)
    {
        Item *item = get_item (self, i);
        int w, h;
        graphene_rect_t pic = picture_rect (self, i);
        GskTransform *transform;

        gtk_widget_measure (item->image, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &w, NULL, NULL);
        gtk_widget_measure (item->image, GTK_ORIENTATION_VERTICAL, -1, NULL, &h, NULL, NULL);
        w = MAX (w, 1);
        h = MAX (h, 1);

        /* Allocated at its own size and scaled, so zooming relayouts nothing. */
        transform = gsk_transform_translate (NULL, &pic.origin);
        transform = gsk_transform_scale (transform, pic.size.width / w, pic.size.height / h);
        gtk_widget_allocate (item->image, w, h, -1, transform);
    }
}

static void
snapshot_marks (NautilusQlCanvas *self,
                GtkSnapshot      *snapshot,
                guint             i,
                const graphene_rect_t *pic)
{
    Item *item = get_item (self, i);
    GSettings *flier = nautilus_flier_settings ();
    graphene_rect_t stars;

    if (stars_bounds (self, i, &stars))
    {
        GskRoundedRect pill;
        graphene_rect_t back;

        graphene_rect_inset_r (&stars, -4, -2, &back);
        gsk_rounded_rect_init_from_rect (&pill, &back, back.size.height / 2);
        gtk_snapshot_push_rounded_clip (snapshot, &pill);
        gtk_snapshot_append_color (snapshot, &(GdkRGBA){ 0, 0, 0, 0.55f }, &back);
        gtk_snapshot_pop (snapshot);
        nautilus_labels_draw_stars (snapshot, &stars, nautilus_file_get_rating (item->file),
                                    &(GdkRGBA){ 1, 1, 1, 0.75f },
                                    gtk_widget_get_scale_factor (GTK_WIDGET (self)));
    }

    const char * const *tags = nautilus_file_get_tags (item->file);

    if (tags != NULL && g_settings_get_boolean (flier, "tags"))
    {
        float dot = CLAMP (self->zoom * TILE / 18, 7, 12);
        guint n = g_strv_length ((char **) tags);
        float x = pic->origin.x + pic->size.width - 4 - dot - (n - 1) * dot * 0.75f;

        for (guint t = 0; t < n; t++, x += dot * 0.75f)
        {
            GdkRGBA color;
            GskRoundedRect circle;
            graphene_rect_t r = GRAPHENE_RECT_INIT (x, pic->origin.y + 4, dot, dot);
            const GdkRGBA ring = { 1, 1, 1, 0.9f };

            nautilus_labels_get_tag_color (tags[t], &color);
            gsk_rounded_rect_init_from_rect (&circle, &r, dot / 2);
            gtk_snapshot_push_rounded_clip (snapshot, &circle);
            gtk_snapshot_append_color (snapshot, &color, &r);
            gtk_snapshot_pop (snapshot);
            gtk_snapshot_append_border (snapshot, &circle, (float[4]) { 1, 1, 1, 1 },
                                        (GdkRGBA[4]) { ring, ring, ring, ring });
        }
    }
}

static void
snapshot_name (NautilusQlCanvas      *self,
               GtkSnapshot           *snapshot,
               Item                  *item,
               const graphene_rect_t *tile)
{
    GdkRGBA fg;
    int text_w, text_h;

    if (item->name == NULL)
    {
        item->name = gtk_widget_create_pango_layout (GTK_WIDGET (self),
                                                     nautilus_file_get_display_name (item->file));
        pango_layout_set_ellipsize (item->name, PANGO_ELLIPSIZE_MIDDLE);
    }

    {
        /* The name shrinks with the pictures, so it fits the room under
         * them at any zoom. */
        PangoFontDescription *font = pango_font_description_copy (
            pango_context_get_font_description (pango_layout_get_context (item->name)));
        double room = NAME_ROOM * self->zoom;

        pango_font_description_set_absolute_size (font, CLAMP (room * 0.55, 7, 14) * PANGO_SCALE);
        pango_layout_set_font_description (item->name, font);
        pango_font_description_free (font);
    }
    pango_layout_set_width (item->name, (int) (tile->size.width * PANGO_SCALE));
    pango_layout_get_pixel_size (item->name, &text_w, &text_h);
    gtk_widget_get_color (GTK_WIDGET (self), &fg);
    fg.alpha *= 0.75f;

    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (snapshot,
                            &GRAPHENE_POINT_INIT (tile->origin.x + (tile->size.width - text_w) / 2,
                                                  tile->origin.y + tile->size.height - text_h - 1));
    gtk_snapshot_append_layout (snapshot, item->name, &fg);
    gtk_snapshot_restore (snapshot);
}

static void
nautilus_ql_canvas_snapshot (GtkWidget   *widget,
                             GtkSnapshot *snapshot)
{
    NautilusQlCanvas *self = NAUTILUS_QL_CANVAS (widget);
    gboolean names = self->show_names && !is_single (self) && self->zoom * NAME_ROOM >= 12;
    graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, gtk_widget_get_width (widget),
                                                 gtk_widget_get_height (widget));

    gtk_snapshot_push_clip (snapshot, &bounds);

    for (guint i = 0; i < self->items->len; i++)
    {
        Item *item = get_item (self, i);
        graphene_rect_t tile = tile_rect (self, i);
        graphene_rect_t pic, box;

        if (!rect_visible (self, &tile))
        {
            continue;
        }

        pic = picture_rect (self, i);
        box = is_single (self) ? pic : picture_box (self, i);
        gtk_snapshot_push_clip (snapshot, &box);

        if (item->full != NULL)
        {
            /* Smooth when shrunk, crisp pixels when blown up. */
            GskScalingFilter filter = pic.size.width * gtk_widget_get_scale_factor (widget) >
                                      gdk_texture_get_width (item->full) * 1.5
                                      ? GSK_SCALING_FILTER_NEAREST
                                      : GSK_SCALING_FILTER_TRILINEAR;

            gtk_snapshot_append_scaled_texture (snapshot, item->full, filter, &pic);
        }
        else if (item->text != NULL)
        {
            snapshot_text_page (self, snapshot, item, &pic);
        }
        else
        {
            gtk_widget_snapshot_child (widget, item->image, snapshot);
        }
        gtk_snapshot_pop (snapshot);

        if ((int) i == self->hovered && !is_single (self))
        {
            g_autoptr (GdkRGBA) accent = adw_style_manager_get_accent_color_rgba (adw_style_manager_get_default ());
            GskRoundedRect outline;
            graphene_rect_t r;

            graphene_rect_inset_r (&box, -3, -3, &r);
            gsk_rounded_rect_init_from_rect (&outline, &r, 5);
            gtk_snapshot_append_border (snapshot, &outline, (float[4]) { 2, 2, 2, 2 },
                                        (GdkRGBA[4]) { *accent, *accent, *accent, *accent });
        }

        if (marks_wanted (self))
        {
            snapshot_marks (self, snapshot, i, &box);
        }

        if (names)
        {
            snapshot_name (self, snapshot, item, &tile);
        }
    }

    gtk_snapshot_pop (snapshot);
}

/* ------------------------------------------------------------- Events */

static void
set_hovered (NautilusQlCanvas *self,
             int               hovered)
{
    if (self->hovered != hovered)
    {
        self->hovered = hovered;
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
on_motion (GtkEventControllerMotion *motion,
           double                    x,
           double                    y,
           NautilusQlCanvas         *self)
{
    self->pointer_x = x;
    self->pointer_y = y;
    self->pointer_in = TRUE;
    set_hovered (self, is_single (self) ? 0 : item_at (self, x, y));
}

static void
on_leave (GtkEventControllerMotion *motion,
          NautilusQlCanvas         *self)
{
    self->pointer_in = FALSE;
    set_hovered (self, is_single (self) ? 0 : -1);
}

static gboolean
on_scroll (GtkEventControllerScroll *scroll,
           double                    dx,
           double                    dy,
           NautilusQlCanvas         *self)
{
    GdkScrollUnit unit = gtk_event_controller_scroll_get_unit (scroll);
    double factor = unit == GDK_SCROLL_UNIT_WHEEL ? pow (1.2, -dy) : exp (-dy * 0.01);

    nautilus_ql_canvas_zoom (self, factor);

    return GDK_EVENT_STOP;
}

static void
on_pinch_begin (GtkGesture       *gesture,
                GdkEventSequence *sequence,
                NautilusQlCanvas *self)
{
    self->pinch_zoom = self->zoom;
}

static void
on_pinch_changed (GtkGestureZoom   *gesture,
                  double            scale,
                  NautilusQlCanvas *self)
{
    double x, y;

    gtk_gesture_get_bounding_box_center (GTK_GESTURE (gesture), &x, &y);
    zoom_at (self, x, y, self->pinch_zoom * scale / self->zoom);
}

static void
on_drag_begin (GtkGestureDrag   *gesture,
               double            x,
               double            y,
               NautilusQlCanvas *self)
{
    self->drag_pan_x = self->pan_x;
    self->drag_pan_y = self->pan_y;
    self->dragging = FALSE;
}

static void
on_drag_update (GtkGestureDrag   *gesture,
                double            dx,
                double            dy,
                NautilusQlCanvas *self)
{
    if (!self->dragging)
    {
        if (!gtk_drag_check_threshold (GTK_WIDGET (self), 0, 0, (int) dx, (int) dy))
        {
            return;
        }
        self->dragging = TRUE;
        gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "grabbing");
    }

    self->pan_x = self->drag_pan_x + dx;
    self->pan_y = self->drag_pan_y + dy;
    self->at_fit = FALSE;
    gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag   *gesture,
             double            dx,
             double            dy,
             NautilusQlCanvas *self)
{
    gtk_widget_set_cursor (GTK_WIDGET (self), NULL);
    self->dragging = FALSE;
    schedule_full_loads (self);
}

static void
on_pressed (GtkGestureClick  *gesture,
            int               n_press,
            double            x,
            double            y,
            NautilusQlCanvas *self)
{
    int i = item_at (self, x, y);
    graphene_rect_t stars;

    if (n_press == 1 && i >= 0 && !is_single (self) && stars_bounds (self, i, &stars) &&
        y >= stars.origin.y - 2 && y <= stars.origin.y + stars.size.height + 2)
    {
        NautilusFile *file = get_item (self, i)->file;
        guint star = nautilus_labels_star_at (&stars, x);

        if (star > 0)
        {
            nautilus_labels_set_rating (&(GList){ .data = file },
                                        star == nautilus_file_get_rating (file) ? 0 : star);
            gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            return;
        }
    }

    if (n_press == 2)
    {
        gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

        if (i >= 0 && !is_single (self))
        {
            g_signal_emit (self, signals[ITEM_ACTIVATED], 0, get_item (self, i)->file);
        }
        else if (is_single (self) && self->at_fit)
        {
            /* Double-click a picture: real size there, then back. */
            double z = 1.0;

            zoom_at (self, x, y, z / self->zoom);
        }
        else
        {
            nautilus_ql_canvas_fit (self);
        }
    }
}

static void
on_file_changed (NautilusQlCanvas *self)
{
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ------------------------------------------------------- Text pages */

static gboolean
text_enough (const char *chunk,
             goffset     size,
             gpointer    user_data)
{
    return size < TEXT_PAGE_BYTES;
}

static void
on_text_loaded (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *contents = NULL;
    gsize length = 0;

    if (!g_file_load_partial_contents_finish (G_FILE (source), result, &contents, &length, NULL, &error) &&
        g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        return;
    }

    Item *item = user_data;
    g_autofree char *valid = g_utf8_make_valid (contents != NULL ? contents : "", MIN (length, TEXT_PAGE_BYTES));
    char *p = valid;

    g_clear_object (&item->text_loading);
    if (contents == NULL || memchr (contents, '\0', MIN (length, TEXT_PAGE_BYTES)) != NULL)
    {
        return;   /* not text after all */
    }
    for (int lines = 0; *p != '\0' && lines < TEXT_PAGE_LINES; p++)
    {
        lines += *p == '\n';
        if (*p == '\t')
        {
            *p = ' ';
        }
    }
    *p = '\0';
    item->text = g_steal_pointer (&valid);
    gtk_widget_queue_draw (item->canvas);
}

/* A text file as a page: white, with its first lines. */
static void
snapshot_text_page (NautilusQlCanvas      *self,
                    GtkSnapshot           *snapshot,
                    Item                  *item,
                    const graphene_rect_t *pic)
{
    double z = pic->size.height / PICTURE;
    float w = pic->size.height * 0.77f;
    graphene_rect_t page = GRAPHENE_RECT_INIT (pic->origin.x + (pic->size.width - w) / 2, pic->origin.y,
                                               w, pic->size.height);
    GskRoundedRect rounded;
    GdkRGBA ink = { 0.18f, 0.18f, 0.2f, 1 };

    if (item->text_layout == NULL)
    {
        PangoFontDescription *font = pango_font_description_from_string ("Monospace 5");

        item->text_layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), item->text);
        pango_layout_set_font_description (item->text_layout, font);
        pango_layout_set_width (item->text_layout, (int) ((PICTURE * 0.77 - 16) * PANGO_SCALE));
        pango_layout_set_wrap (item->text_layout, PANGO_WRAP_WORD_CHAR);
        pango_font_description_free (font);
    }

    gsk_rounded_rect_init_from_rect (&rounded, &page, 3 * z);
    gtk_snapshot_append_outset_shadow (snapshot, &rounded, &(GdkRGBA){ 0, 0, 0, 0.25f }, 0, 1, 0, 4 * z);
    gtk_snapshot_push_rounded_clip (snapshot, &rounded);
    gtk_snapshot_append_color (snapshot, &(GdkRGBA){ 0.98f, 0.98f, 0.97f, 1 }, &page);
    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (page.origin.x + 8 * z, page.origin.y + 8 * z));
    gtk_snapshot_scale (snapshot, z, z);
    gtk_snapshot_append_layout (snapshot, item->text_layout, &ink);
    gtk_snapshot_restore (snapshot);
    gtk_snapshot_pop (snapshot);
}

/* -------------------------------------------------------------- API */

static gboolean
is_single_list (GList *files)
{
    return files != NULL && files->next == NULL;
}

void
nautilus_ql_canvas_set_files (NautilusQlCanvas *self,
                              GList            *files)
{
    g_ptr_array_set_size (self->items, 0);
    g_array_set_size (self->cells, 0);
    self->hovered = -1;

    for (GList *l = files; l != NULL && self->items->len < MAX_ITEMS; l = l->next)
    {
        Item *item = g_new0 (Item, 1);
        g_autoptr (GFile) location = nautilus_file_get_location (l->data);
        const char *mime = nautilus_file_get_mime_type (l->data);
        g_autoptr (GdkPaintable) icon = NULL;

        item->file = nautilus_file_ref (l->data);
        item->is_picture = g_str_has_prefix (mime, "image/") && nautilus_file_is_regular_file (l->data);

        /* Folders and the like: their icon, at the size of a thumbnail. */
        icon = nautilus_file_get_icon_paintable (item->file, PICTURE,
                                                 gtk_widget_get_scale_factor (GTK_WIDGET (self)),
                                                 NAUTILUS_FILE_ICON_FLAGS_NONE);
        item->image = GTK_WIDGET (nautilus_image_new ());
        nautilus_image_set_size (NAUTILUS_IMAGE (item->image), PICTURE);
        nautilus_image_set_fallback (NAUTILUS_IMAGE (item->image), icon);
        if (nautilus_file_is_regular_file (item->file))
        {
            nautilus_image_set_source (NAUTILUS_IMAGE (item->image), location);
        }
        gtk_widget_set_parent (item->image, GTK_WIDGET (self));

        item->canvas = GTK_WIDGET (self);
        if (!is_single_list (files) && nautilus_file_is_regular_file (item->file) &&
            nautilus_text_preview_handles (mime))
        {
            item->text_loading = g_cancellable_new ();
            g_file_load_partial_contents_async (location, item->text_loading, text_enough,
                                                on_text_loaded, item);
        }

        item->changed_id = g_signal_connect_swapped (item->file, "changed",
                                                     G_CALLBACK (on_file_changed), self);

        g_ptr_array_add (self->items, item);
    }

    if (is_single (self))
    {
        self->hovered = 0;
    }

    nautilus_ql_canvas_fit (self);
    update_full_loads (self);
}

NautilusFile *
nautilus_ql_canvas_get_hovered (NautilusQlCanvas *self)
{
    if (self->hovered < 0 || (guint) self->hovered >= self->items->len)
    {
        return NULL;
    }

    return get_item (self, self->hovered)->file;
}

void
nautilus_ql_canvas_get_picture_size (NautilusQlCanvas *self,
                                     int              *width,
                                     int              *height)
{
    Item *item = is_single (self) ? get_item (self, 0) : NULL;

    *width = item != NULL && item->full != NULL ? gdk_texture_get_width (item->full) : 0;
    *height = item != NULL && item->full != NULL ? gdk_texture_get_height (item->full) : 0;
}

GtkWidget *
nautilus_ql_canvas_new (void)
{
    return g_object_new (NAUTILUS_TYPE_QL_CANVAS, NULL);
}

/* ----------------------------------------------------------- GObject */

static void
nautilus_ql_canvas_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
    NautilusQlCanvas *self = NAUTILUS_QL_CANVAS (object);

    switch (prop_id)
    {
        case PROP_SHOW_NAMES:
        {
            g_value_set_boolean (value, self->show_names);
        }
        break;

        case PROP_SHOW_MARKS:
        {
            g_value_set_boolean (value, self->show_marks);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_ql_canvas_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
    NautilusQlCanvas *self = NAUTILUS_QL_CANVAS (object);

    switch (prop_id)
    {
        case PROP_SHOW_NAMES:
        {
            self->show_names = g_value_get_boolean (value);
            /* The cells change height with it. */
            gtk_widget_queue_allocate (GTK_WIDGET (self));
            gtk_widget_queue_draw (GTK_WIDGET (self));
        }
        break;

        case PROP_SHOW_MARKS:
        {
            self->show_marks = g_value_get_boolean (value);
            gtk_widget_queue_draw (GTK_WIDGET (self));
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
nautilus_ql_canvas_dispose (GObject *object)
{
    NautilusQlCanvas *self = NAUTILUS_QL_CANVAS (object);

    g_clear_handle_id (&self->full_idle, g_source_remove);
    g_clear_pointer (&self->items, g_ptr_array_unref);
    g_clear_pointer (&self->cells, g_array_unref);

    G_OBJECT_CLASS (nautilus_ql_canvas_parent_class)->dispose (object);
}

static void
nautilus_ql_canvas_init (NautilusQlCanvas *self)
{
    GtkEventController *controller;
    GtkGesture *gesture;

    self->items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
    self->cells = g_array_new (FALSE, TRUE, sizeof (Cell));
    self->zoom = 1.0;
    self->at_fit = TRUE;
    self->hovered = -1;
    self->show_names = TRUE;
    self->show_marks = TRUE;

    gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

    controller = gtk_event_controller_motion_new ();
    g_signal_connect (controller, "motion", G_CALLBACK (on_motion), self);
    g_signal_connect (controller, "enter", G_CALLBACK (on_motion), self);
    g_signal_connect (controller, "leave", G_CALLBACK (on_leave), self);
    gtk_widget_add_controller (GTK_WIDGET (self), controller);

    controller = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect (controller, "scroll", G_CALLBACK (on_scroll), self);
    gtk_widget_add_controller (GTK_WIDGET (self), controller);

    gesture = gtk_gesture_zoom_new ();
    g_signal_connect (gesture, "begin", G_CALLBACK (on_pinch_begin), self);
    g_signal_connect (gesture, "scale-changed", G_CALLBACK (on_pinch_changed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (gesture));

    gesture = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect (gesture, "pressed", G_CALLBACK (on_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (gesture));

    gesture = gtk_gesture_drag_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect (gesture, "drag-begin", G_CALLBACK (on_drag_begin), self);
    g_signal_connect (gesture, "drag-update", G_CALLBACK (on_drag_update), self);
    g_signal_connect (gesture, "drag-end", G_CALLBACK (on_drag_end), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (gesture));

    g_signal_connect_object (nautilus_labels_get (), "tags-changed",
                             G_CALLBACK (gtk_widget_queue_draw), self, G_CONNECT_SWAPPED);
}

static void
nautilus_ql_canvas_class_init (NautilusQlCanvasClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->get_property = nautilus_ql_canvas_get_property;
    object_class->set_property = nautilus_ql_canvas_set_property;
    object_class->dispose = nautilus_ql_canvas_dispose;
    widget_class->size_allocate = nautilus_ql_canvas_size_allocate;
    widget_class->snapshot = nautilus_ql_canvas_snapshot;

    properties[PROP_SHOW_NAMES] = g_param_spec_boolean ("show-names", NULL, NULL, TRUE,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_SHOW_MARKS] = g_param_spec_boolean ("show-marks", NULL, NULL, TRUE,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties (object_class, N_PROPS, properties);

    signals[ITEM_ACTIVATED] = g_signal_new ("item-activated", G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1, NAUTILUS_TYPE_FILE);
    signals[PICTURE_LOADED] = g_signal_new ("picture-loaded", G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);

    gtk_widget_class_set_css_name (widget_class, "nautilus-ql-canvas");
}
