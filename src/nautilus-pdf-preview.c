/*
 * nautilus-pdf-preview.c: the pages of a PDF, to scroll through and zoom
 *
 * Pages are rendered by Poppler, opened at runtime so that it isn't a
 * build or package dependency: without it, PDFs show their thumbnail as
 * before. Rendering happens off the main thread, one page at a time (a
 * Poppler document mustn't be used by two threads at once). Each page
 * keeps one picture of the whole page, up to a size; zoomed in past
 * that, the visible part of the page gets a sharp picture of its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "nautilus-pdf-preview"

#include "nautilus-pdf-preview.h"

#include <cairo.h>
#include <glib/gi18n.h>
#include <gmodule.h>

#include "nautilus-file.h"
#include "nautilus-labels.h"

#define PAD 16.0
#define GAP 12.0
#define ZOOM_MIN 0.05
#define ZOOM_MAX 12.0
/* 96 dpi */
#define ACTUAL_SIZE (96.0 / 72.0)
/* The most pixels one picture holds. */
#define MAX_PIXELS (12 * 1000 * 1000)
/* After zooming, before sharpening. */
#define SETTLE_MS 140

/* ------------------------------------------------------------ Poppler */

typedef struct _PopplerDocument PopplerDocument;
typedef struct _PopplerPage PopplerPage;

static struct
{
    gboolean tried;
    gboolean ok;
    PopplerDocument *(*document_new_from_file) (const char  *uri,
                                                const char  *password,
                                                GError     **error);
    int (*document_get_n_pages) (PopplerDocument *document);
    PopplerPage *(*document_get_page) (PopplerDocument *document,
                                       int              index);
    void (*page_get_size) (PopplerPage *page,
                           double      *width,
                           double      *height);
    void (*page_render) (PopplerPage *page,
                         cairo_t     *cr);
} poppler;

static gboolean
poppler_load (void)
{
    GModule *module;

    if (poppler.tried)
    {
        return poppler.ok;
    }
    poppler.tried = TRUE;

    module = g_module_open ("libpoppler-glib.so.8", G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (module == NULL)
    {
        g_debug ("No Poppler: %s", g_module_error ());
        return FALSE;
    }

    poppler.ok = g_module_symbol (module, "poppler_document_new_from_file",
                                  (gpointer *) &poppler.document_new_from_file) &&
                 g_module_symbol (module, "poppler_document_get_n_pages",
                                  (gpointer *) &poppler.document_get_n_pages) &&
                 g_module_symbol (module, "poppler_document_get_page",
                                  (gpointer *) &poppler.document_get_page) &&
                 g_module_symbol (module, "poppler_page_get_size",
                                  (gpointer *) &poppler.page_get_size) &&
                 g_module_symbol (module, "poppler_page_render",
                                  (gpointer *) &poppler.page_render);
    if (poppler.ok)
    {
        g_module_make_resident (module);
    }
    else
    {
        g_module_close (module);
    }

    return poppler.ok;
}

gboolean
nautilus_pdf_preview_handles (const char *mime_type)
{
    return g_strcmp0 (mime_type, "application/pdf") == 0 && poppler_load ();
}

/* -------------------------------------------------------------- Pages */

typedef struct
{
    double width;              /* in points */
    double height;
    PopplerPage *page;
    GdkTexture *texture;       /* the whole page */
    double texture_scale;      /* its pixels per point */
    double x;                  /* where it is laid out, in the view's */
    double y;                  /* pixels, at the current zoom */
    double row_y;              /* the top of its row */
} Page;

typedef struct
{
    int index;
    graphene_rect_t area;      /* in points */
    double scale;
    GdkTexture *texture;
} Detail;

#define PDF_TYPE_PAGES (pdf_pages_get_type ())
G_DECLARE_FINAL_TYPE (PdfPages, pdf_pages, PDF, PAGES, GtkWidget)

struct _PdfPages
{
    GtkWidget parent_instance;

    GtkAdjustment *hadjustment;
    GtkAdjustment *vadjustment;
    GtkScrollablePolicy hscroll_policy;
    GtkScrollablePolicy vscroll_policy;

    guint generation;
    GObject *document;         /* PopplerDocument */
    GArray *pages;             /* of Page */
    double widest;             /* in points */
    /* Pages side by side; the widest and tallest row, in points. */
    gboolean dual;
    double row_width;
    double row_height;
    /* A page to bring into view on the next layout. */
    int pending_page;

    double zoom;               /* logical pixels per point */
    gboolean fit;
    double content_width;
    double content_height;

    /* Kept in place over the next layout. */
    gboolean anchored;
    int anchor_page;
    double anchor_x;           /* in the page, in points */
    double anchor_y;
    double anchor_at_x;        /* in the view */
    double anchor_at_y;

    Detail detail;
    gboolean rendering;
    guint settle_id;
    gint64 zoomed_at;

    double pointer_x;
    double pointer_y;
    double zoom_gesture_start;
    double drag_start_x;
    double drag_start_y;
};

enum
{
    PROP_0,
    PROP_HADJUSTMENT,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY,
};

G_DEFINE_TYPE_WITH_CODE (PdfPages, pdf_pages, GTK_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

static void pump (PdfPages *self);

static void
page_clear (Page *page)
{
    g_clear_object (&page->page);
    g_clear_object (&page->texture);
}

static guint
per_row (PdfPages *self)
{
    return self->dual ? 2 : 1;
}

/* The widest and tallest row, for fitting a whole one in view. */
static void
measure (PdfPages *self)
{
    guint cols = per_row (self);

    self->row_width = 0;
    self->row_height = 0;
    for (guint i = 0; i < self->pages->len; i += cols)
    {
        double width = 0;
        double height = 0;

        for (guint j = i; j < MIN (i + cols, self->pages->len); j++)
        {
            Page *page = &g_array_index (self->pages, Page, j);

            width += page->width;
            height = MAX (height, page->height);
        }
        self->row_width = MAX (self->row_width, width);
        self->row_height = MAX (self->row_height, height);
    }
}

/* Places the pages in rows, centred, and sizes the content. Fitted, each
 * row has a view's height to itself, so no other page peeks in. */
static void
layout (PdfPages *self,
        double    view_width,
        double    view_height)
{
    guint cols = per_row (self);
    double y = PAD;
    double pitch = self->fit ? MAX (view_height, ceil (self->row_height * self->zoom) + 2 * PAD) : 0;

    self->content_width = MAX (view_width, ceil (self->row_width * self->zoom + (cols - 1) * GAP + 2 * PAD));
    for (guint i = 0; i < self->pages->len; i += cols)
    {
        guint end = MIN (i + cols, self->pages->len);
        double width = (end - i - 1) * GAP;
        double height = 0;
        double x;

        for (guint j = i; j < end; j++)
        {
            Page *page = &g_array_index (self->pages, Page, j);

            width += page->width * self->zoom;
            height = MAX (height, page->height * self->zoom);
        }
        x = floor ((self->content_width - width) / 2);
        for (guint j = i; j < end; j++)
        {
            Page *page = &g_array_index (self->pages, Page, j);

            page->x = x;
            if (pitch > 0)
            {
                double slot = (i / cols) * pitch;

                page->y = slot + floor ((pitch - page->height * self->zoom) / 2);
                page->row_y = slot + PAD;
            }
            else
            {
                page->y = y + floor ((height - page->height * self->zoom) / 2);
                page->row_y = y;
            }
            x += page->width * self->zoom + GAP;
        }
        y += height + GAP;
    }
    if (pitch > 0)
    {
        self->content_height = ((self->pages->len + cols - 1) / cols) * pitch;
    }
    else
    {
        self->content_height = self->pages->len > 0 ? y - GAP + PAD : 0;
    }
}



/* The first page of the row at the top of the view. */
static int
current_page (PdfPages *self)
{
    double value = self->vadjustment != NULL ? gtk_adjustment_get_value (self->vadjustment) : 0;
    guint cols = per_row (self);
    int result = 0;

    for (guint i = 0; i < self->pages->len; i += cols)
    {
        Page *page = &g_array_index (self->pages, Page, i);

        /* The row whose top is in the upper half of the view. */
        if (page->row_y - PAD <= value + 1 ||
            (self->vadjustment != NULL &&
             page->row_y < value + gtk_adjustment_get_page_size (self->vadjustment) / 2 &&
             value + gtk_adjustment_get_page_size (self->vadjustment) >= gtk_adjustment_get_upper (self->vadjustment) - 1))
        {
            result = i;
        }
    }
    return result;
}

/* A whole row of pages in view. */
static double
fit_zoom (PdfPages *self,
          double    width,
          double    height)
{
    double zoom;

    if (self->pages->len == 0 || self->row_width <= 0)
    {
        return 1;
    }

    zoom = MIN ((width - 2 * PAD - (per_row (self) - 1) * GAP) / self->row_width,
                (height - 2 * PAD) / self->row_height);
    return CLAMP (zoom, ZOOM_MIN, ZOOM_MAX);
}

static double
pixel_scale (PdfPages *self)
{
    return self->zoom * gtk_widget_get_scale_factor (GTK_WIDGET (self));
}

/* The page under a point of the view, and where in it. */
/* The page under a point of the view, and where in it. */
static void
anchor_at (PdfPages *self,
           double    x,
           double    y)
{
    double cx = x + gtk_adjustment_get_value (self->hadjustment);
    double cy = y + gtk_adjustment_get_value (self->vadjustment);
    double nearest = G_MAXDOUBLE;
    guint best = 0;
    Page *page;

    if (self->pages->len == 0)
    {
        return;
    }

    for (guint i = 0; i < self->pages->len; i++)
    {
        Page *p = &g_array_index (self->pages, Page, i);
        double dx = cx < p->x ? p->x - cx : MAX (0, cx - p->x - p->width * self->zoom);
        double dy = cy < p->y ? p->y - cy : MAX (0, cy - p->y - p->height * self->zoom);

        if (dx + dy < nearest)
        {
            nearest = dx + dy;
            best = i;
        }
    }

    page = &g_array_index (self->pages, Page, best);
    self->anchored = TRUE;
    self->anchor_page = best;
    self->anchor_x = (cx - page->x) / self->zoom;
    self->anchor_y = (cy - page->y) / self->zoom;
    self->anchor_at_x = x;
    self->anchor_at_y = y;
}

static void
set_zoom (PdfPages *self,
          double    zoom,
          double    at_x,
          double    at_y)
{
    zoom = CLAMP (zoom, ZOOM_MIN, ZOOM_MAX);
    if (G_APPROX_VALUE (zoom, self->zoom, 1e-6))
    {
        return;
    }

    anchor_at (self, at_x, at_y);
    self->zoom = zoom;
    self->fit = FALSE;
    self->zoomed_at = g_get_monotonic_time ();
    gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
configure (GtkAdjustment *adjustment,
           double         value,
           double         upper,
           double         page)
{
    upper = MAX (upper, page);
    gtk_adjustment_configure (adjustment,
                              CLAMP (value, 0, upper - page),
                              0, upper,
                              page * 0.1, page * 0.9, page);
}

static void
pdf_pages_size_allocate (GtkWidget *widget,
                         int        width,
                         int        height,
                         int        baseline)
{
    PdfPages *self = PDF_PAGES (widget);
    double hvalue = self->hadjustment != NULL ? gtk_adjustment_get_value (self->hadjustment) : 0;
    double vvalue = self->vadjustment != NULL ? gtk_adjustment_get_value (self->vadjustment) : 0;

    if (self->fit)
    {
        self->zoom = fit_zoom (self, width, height);
    }

    layout (self, width, height);

    if (self->pending_page >= 0 && self->pending_page < (int) self->pages->len)
    {
        vvalue = g_array_index (self->pages, Page, self->pending_page).row_y - PAD;
        self->anchored = FALSE;
    }
    else if (self->anchored && self->anchor_page < (int) self->pages->len)
    {
        Page *page = &g_array_index (self->pages, Page, self->anchor_page);

        hvalue = page->x + self->anchor_x * self->zoom - self->anchor_at_x;
        vvalue = page->y + self->anchor_y * self->zoom - self->anchor_at_y;
    }
    self->anchored = FALSE;
    self->pending_page = -1;

    if (self->hadjustment != NULL)
    {
        configure (self->hadjustment, hvalue, self->content_width, width);
    }
    if (self->vadjustment != NULL)
    {
        configure (self->vadjustment, vvalue, self->content_height, height);
    }
}

static void
pdf_pages_snapshot (GtkWidget   *widget,
                    GtkSnapshot *snapshot)
{
    PdfPages *self = PDF_PAGES (widget);
    double height = gtk_widget_get_height (widget);
    double hvalue = gtk_adjustment_get_value (self->hadjustment);
    double vvalue = gtk_adjustment_get_value (self->vadjustment);
    GdkRGBA paper = { 1, 1, 1, 1 };
    GdkRGBA shade = { 0, 0, 0, 0.25 };

    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (-hvalue, -vvalue));

    for (guint i = 0; i < self->pages->len; i++)
    {
        Page *page = &g_array_index (self->pages, Page, i);
        double h = page->height * self->zoom;
        graphene_rect_t rect;
        GskRoundedRect outline;

        if (page->y > vvalue + height)
        {
            break;
        }
        if (page->y + h < vvalue)
        {
            continue;
        }

        rect = GRAPHENE_RECT_INIT (page->x, page->y, page->width * self->zoom, h);
        gsk_rounded_rect_init_from_rect (&outline, &rect, 0);
        gtk_snapshot_append_outset_shadow (snapshot, &outline, &shade, 0, 1, 0, 4);
        gtk_snapshot_append_color (snapshot, &paper, &rect);
        if (page->texture != NULL)
        {
            gtk_snapshot_append_scaled_texture (snapshot, page->texture,
                                                page->texture_scale > pixel_scale (self) * 1.01 ?
                                                GSK_SCALING_FILTER_TRILINEAR : GSK_SCALING_FILTER_LINEAR,
                                                &rect);
        }
        if (self->detail.texture != NULL && self->detail.index == (int) i)
        {
            graphene_rect_t area = self->detail.area;

            gtk_snapshot_push_clip (snapshot, &rect);
            gtk_snapshot_append_scaled_texture (snapshot, self->detail.texture,
                                                GSK_SCALING_FILTER_LINEAR,
                                                &GRAPHENE_RECT_INIT (rect.origin.x + area.origin.x * self->zoom,
                                                                     rect.origin.y + area.origin.y * self->zoom,
                                                                     area.size.width * self->zoom,
                                                                     area.size.height * self->zoom));
            gtk_snapshot_pop (snapshot);
        }
    }

    gtk_snapshot_restore (snapshot);
    pump (self);
}

/* ---------------------------------------------------------- Rendering */

typedef struct
{
    GWeakRef self;             /* PdfPages */
    guint generation;
    gboolean detail;
    int index;
    PopplerPage *page;
    double scale;
    graphene_rect_t area;      /* in points */
} RenderJob;

static void
render_job_free (RenderJob *job)
{
    g_weak_ref_clear (&job->self);
    g_clear_object (&job->page);
    g_free (job);
}

static void
render_thread (GTask        *task,
               gpointer      source,
               gpointer      task_data,
               GCancellable *cancellable)
{
    RenderJob *job = task_data;
    int width = MAX (1, (int) ceil (job->area.size.width * job->scale));
    int height = MAX (1, (int) ceil (job->area.size.height * job->scale));
    cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create (surface);
    g_autoptr (GBytes) bytes = NULL;
    GdkTexture *texture;

    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);
    cairo_scale (cr, job->scale, job->scale);
    cairo_translate (cr, -job->area.origin.x, -job->area.origin.y);
    poppler.page_render (job->page, cr);
    cairo_destroy (cr);
    cairo_surface_flush (surface);

    bytes = g_bytes_new_with_free_func (cairo_image_surface_get_data (surface),
                                        (gsize) cairo_image_surface_get_stride (surface) * height,
                                        (GDestroyNotify) cairo_surface_destroy, surface);
    texture = gdk_memory_texture_new (width, height, GDK_MEMORY_DEFAULT, bytes,
                                      cairo_image_surface_get_stride (surface));
    g_task_return_pointer (task, texture, g_object_unref);
}

static void
on_rendered (GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
    RenderJob *job = g_task_get_task_data (G_TASK (result));
    g_autoptr (GdkTexture) texture = g_task_propagate_pointer (G_TASK (result), NULL);
    g_autoptr (PdfPages) self = g_weak_ref_get (&job->self);

    if (self == NULL)
    {
        return;
    }

    self->rendering = FALSE;
    if (job->generation != self->generation || job->index >= (int) self->pages->len)
    {
        pump (self);
        return;
    }

    if (job->detail)
    {
        g_clear_object (&self->detail.texture);
        self->detail.index = job->index;
        self->detail.area = job->area;
        self->detail.scale = job->scale;
        self->detail.texture = g_steal_pointer (&texture);
    }
    else
    {
        Page *page = &g_array_index (self->pages, Page, job->index);

        g_set_object (&page->texture, texture);
        page->texture_scale = job->scale;
    }
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
start_render (PdfPages              *self,
              int                    index,
              gboolean               detail,
              double                 scale,
              const graphene_rect_t *area)
{
    Page *page = &g_array_index (self->pages, Page, index);
    RenderJob *job = g_new0 (RenderJob, 1);
    g_autoptr (GTask) task = g_task_new (NULL, NULL, on_rendered, NULL);

    g_weak_ref_init (&job->self, self);
    job->generation = self->generation;
    job->detail = detail;
    job->index = index;
    job->page = g_object_ref ((gpointer) page->page);
    job->scale = scale;
    job->area = *area;

    self->rendering = TRUE;
    g_task_set_task_data (task, job, (GDestroyNotify) render_job_free);
    g_task_run_in_thread (task, render_thread);
}

static gboolean
on_settled (gpointer user_data)
{
    PdfPages *self = user_data;

    self->settle_id = 0;
    pump (self);
    return G_SOURCE_REMOVE;
}

/* The scale a whole page is rendered at: as sharp as shown, up to a size. */
static double
whole_scale (PdfPages *self,
             Page     *page)
{
    double scale = pixel_scale (self);
    double cap = sqrt (MAX_PIXELS / (page->width * page->height));

    return MIN (scale, cap);
}

/* Starts the next picture needed, if none is underway. */
static void
pump (PdfPages *self)
{
    double vvalue, hvalue, height, width;
    gboolean settled;
    int first = -1, last = -1;

    if (self->rendering || self->pages->len == 0 ||
        self->hadjustment == NULL || self->vadjustment == NULL)
    {
        return;
    }

    hvalue = gtk_adjustment_get_value (self->hadjustment);
    vvalue = gtk_adjustment_get_value (self->vadjustment);
    width = gtk_widget_get_width (GTK_WIDGET (self));
    height = gtk_widget_get_height (GTK_WIDGET (self));
    if (width <= 0 || height <= 0)
    {
        return;
    }

    for (guint i = 0; i < self->pages->len; i++)
    {
        Page *page = &g_array_index (self->pages, Page, i);
        double h = page->height * self->zoom;

        if (page->y + h >= vvalue && page->y <= vvalue + height)
        {
            if (first < 0)
            {
                first = i;
            }
            last = i;
        }
    }
    if (first < 0)
    {
        return;
    }

    settled = g_get_monotonic_time () - self->zoomed_at >= SETTLE_MS * 1000;

    /* Visible pages without any picture first, then ones that are blurry,
     * once zooming has settled. The pages next to the view come after. */
    for (int pass = 0; pass < 3; pass++)
    {
        int from = pass < 2 ? first : MAX (0, first - 1);
        int to = pass < 2 ? last : MIN ((int) self->pages->len - 1, last + 1);

        for (int i = from; i <= to; i++)
        {
            Page *page = &g_array_index (self->pages, Page, i);
            double scale = whole_scale (self, page);
            gboolean needed;

            if (pass == 0 || pass == 2)
            {
                needed = page->texture == NULL;
            }
            else
            {
                needed = settled && fabs (page->texture_scale - scale) > scale * 0.02;
            }

            if (needed)
            {
                start_render (self, i, FALSE, scale,
                              &GRAPHENE_RECT_INIT (0, 0, page->width, page->height));
                return;
            }
        }
    }

    /* Zoomed in past the whole-page pictures: the part of the page in view. */
    if (settled)
    {
        for (int i = first; i <= last; i++)
        {
            Page *page = &g_array_index (self->pages, Page, i);
            double scale = pixel_scale (self);
            double top = page->y;
            double left = page->x;
            graphene_rect_t view, area;

            if (page->texture_scale >= scale * 0.98)
            {
                continue;
            }

            view = GRAPHENE_RECT_INIT ((hvalue - left) / self->zoom, (vvalue - top) / self->zoom,
                                       width / self->zoom, height / self->zoom);
            if (!graphene_rect_intersection (&view,
                                             &GRAPHENE_RECT_INIT (0, 0, page->width, page->height),
                                             &area))
            {
                continue;
            }

            if (self->detail.texture != NULL && self->detail.index == i &&
                fabs (self->detail.scale - scale) <= scale * 0.02 &&
                graphene_rect_contains_rect (&self->detail.area, &area))
            {
                continue;
            }

            /* Some margin, for scrolling a little. */
            graphene_rect_inset (&view, -view.size.width / 4, -view.size.height / 4);
            graphene_rect_intersection (&view,
                                        &GRAPHENE_RECT_INIT (0, 0, page->width, page->height),
                                        &area);
            start_render (self, i, TRUE, scale, &area);
            return;
        }
    }
    else if (self->settle_id == 0)
    {
        self->settle_id = g_timeout_add (SETTLE_MS, on_settled, self);
    }

    /* Pages far from view let go of their pictures. */
    for (int i = 0; i < (int) self->pages->len; i++)
    {
        if (i < first - 3 || i > last + 3)
        {
            g_clear_object (&g_array_index (self->pages, Page, i).texture);
        }
    }
}

/* ------------------------------------------------------------ Loading */

typedef struct
{
    GObject *document;
    GArray *pages;
} Loaded;

static void
loaded_free (Loaded *loaded)
{
    g_clear_object (&loaded->document);
    g_clear_pointer (&loaded->pages, g_array_unref);
    g_free (loaded);
}

static void
load_thread (GTask        *task,
             gpointer      source,
             gpointer      task_data,
             GCancellable *cancellable)
{
    const char *uri = task_data;
    GError *error = NULL;
    PopplerDocument *document = poppler.document_new_from_file (uri, NULL, &error);
    Loaded *loaded;
    int n;

    if (document == NULL)
    {
        g_task_return_error (task, error);
        return;
    }

    loaded = g_new0 (Loaded, 1);
    loaded->document = G_OBJECT (document);
    loaded->pages = g_array_new (FALSE, TRUE, sizeof (Page));
    g_array_set_clear_func (loaded->pages, (GDestroyNotify) page_clear);

    n = poppler.document_get_n_pages (document);
    for (int i = 0; i < n; i++)
    {
        Page page = { 0 };

        page.page = poppler.document_get_page (document, i);
        if (page.page == NULL)
        {
            continue;
        }
        poppler.page_get_size (page.page, &page.width, &page.height);
        page.width = MAX (page.width, 1);
        page.height = MAX (page.height, 1);
        g_array_append_val (loaded->pages, page);
    }

    g_task_return_pointer (task, loaded, (GDestroyNotify) loaded_free);
}

/* -------------------------------------------------------------- Input */

static gboolean
on_scroll (GtkEventControllerScroll *controller,
           double                    dx,
           double                    dy,
           PdfPages                 *self)
{
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));

    if ((state & GDK_CONTROL_MASK) == 0)
    {
        return GDK_EVENT_PROPAGATE;
    }

    set_zoom (self, self->zoom * pow (1.1, -dy), self->pointer_x, self->pointer_y);
    return GDK_EVENT_STOP;
}

static void
on_motion (GtkEventControllerMotion *controller,
           double                    x,
           double                    y,
           PdfPages                 *self)
{
    self->pointer_x = x;
    self->pointer_y = y;
}

static void
on_zoom_begin (GtkGesture       *gesture,
               GdkEventSequence *sequence,
               PdfPages         *self)
{
    self->zoom_gesture_start = self->zoom;
}

static void
on_zoom_changed (GtkGestureZoom *gesture,
                 double          scale,
                 PdfPages       *self)
{
    double x, y;

    if (!gtk_gesture_get_bounding_box_center (GTK_GESTURE (gesture), &x, &y))
    {
        x = gtk_widget_get_width (GTK_WIDGET (self)) / 2;
        y = gtk_widget_get_height (GTK_WIDGET (self)) / 2;
    }
    set_zoom (self, self->zoom_gesture_start * scale, x, y);
}

static void
on_drag_begin (GtkGestureDrag *gesture,
               double          x,
               double          y,
               PdfPages       *self)
{
    self->drag_start_x = gtk_adjustment_get_value (self->hadjustment);
    self->drag_start_y = gtk_adjustment_get_value (self->vadjustment);
}

static void
on_drag_update (GtkGestureDrag *gesture,
                double          dx,
                double          dy,
                PdfPages       *self)
{
    gtk_adjustment_set_value (self->hadjustment, self->drag_start_x - dx);
    gtk_adjustment_set_value (self->vadjustment, self->drag_start_y - dy);
}

/* ------------------------------------------------------------- Object */

static void
on_adjustment_changed (PdfPages *self)
{
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
set_adjustment (PdfPages       *self,
                GtkAdjustment **slot,
                GtkAdjustment  *adjustment)
{
    if (*slot == adjustment && adjustment != NULL)
    {
        return;
    }
    if (*slot != NULL)
    {
        g_signal_handlers_disconnect_by_func (*slot, on_adjustment_changed, self);
        g_object_unref (*slot);
    }
    if (adjustment == NULL)
    {
        adjustment = gtk_adjustment_new (0, 0, 0, 0, 0, 0);
    }
    *slot = g_object_ref_sink (adjustment);
    g_signal_connect_swapped (adjustment, "value-changed", G_CALLBACK (on_adjustment_changed), self);
    gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
pdf_pages_set_property (GObject      *object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
    PdfPages *self = PDF_PAGES (object);

    switch (prop_id)
    {
        case PROP_HADJUSTMENT:
        {
            set_adjustment (self, &self->hadjustment, g_value_get_object (value));
        }
        break;

        case PROP_VADJUSTMENT:
        {
            set_adjustment (self, &self->vadjustment, g_value_get_object (value));
        }
        break;

        case PROP_HSCROLL_POLICY:
        {
            self->hscroll_policy = g_value_get_enum (value);
        }
        break;

        case PROP_VSCROLL_POLICY:
        {
            self->vscroll_policy = g_value_get_enum (value);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
pdf_pages_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
    PdfPages *self = PDF_PAGES (object);

    switch (prop_id)
    {
        case PROP_HADJUSTMENT:
        {
            g_value_set_object (value, self->hadjustment);
        }
        break;

        case PROP_VADJUSTMENT:
        {
            g_value_set_object (value, self->vadjustment);
        }
        break;

        case PROP_HSCROLL_POLICY:
        {
            g_value_set_enum (value, self->hscroll_policy);
        }
        break;

        case PROP_VSCROLL_POLICY:
        {
            g_value_set_enum (value, self->vscroll_policy);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        }
    }
}

static void
pdf_pages_clear (PdfPages *self)
{
    self->generation++;
    g_array_set_size (self->pages, 0);
    g_clear_object (&self->detail.texture);
    g_clear_object (&self->document);
    self->widest = 0;
    self->row_width = 0;
    self->row_height = 0;
    self->fit = TRUE;
    self->anchored = FALSE;
    self->pending_page = -1;
}

static void
pdf_pages_dispose (GObject *object)
{
    PdfPages *self = PDF_PAGES (object);

    pdf_pages_clear (self);
    g_clear_handle_id (&self->settle_id, g_source_remove);
    if (self->hadjustment != NULL)
    {
        g_signal_handlers_disconnect_by_func (self->hadjustment, on_adjustment_changed, self);
        g_clear_object (&self->hadjustment);
    }
    if (self->vadjustment != NULL)
    {
        g_signal_handlers_disconnect_by_func (self->vadjustment, on_adjustment_changed, self);
        g_clear_object (&self->vadjustment);
    }

    G_OBJECT_CLASS (pdf_pages_parent_class)->dispose (object);
}

static void
pdf_pages_finalize (GObject *object)
{
    PdfPages *self = PDF_PAGES (object);

    g_array_unref (self->pages);

    G_OBJECT_CLASS (pdf_pages_parent_class)->finalize (object);
}

static void
pdf_pages_class_init (PdfPagesClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->set_property = pdf_pages_set_property;
    object_class->get_property = pdf_pages_get_property;
    object_class->dispose = pdf_pages_dispose;
    object_class->finalize = pdf_pages_finalize;
    widget_class->size_allocate = pdf_pages_size_allocate;
    widget_class->snapshot = pdf_pages_snapshot;

    g_object_class_override_property (object_class, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property (object_class, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
}

static void
pdf_pages_init (PdfPages *self)
{
    GtkEventController *scroll;
    GtkGesture *gesture;

    self->pages = g_array_new (FALSE, TRUE, sizeof (Page));
    g_array_set_clear_func (self->pages, (GDestroyNotify) page_clear);
    self->zoom = 1;
    self->fit = TRUE;
    self->pending_page = -1;
    gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);

    scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
    gtk_widget_add_controller (GTK_WIDGET (self), scroll);

    scroll = gtk_event_controller_motion_new ();
    g_signal_connect (scroll, "enter", G_CALLBACK (on_motion), self);
    g_signal_connect (scroll, "motion", G_CALLBACK (on_motion), self);
    gtk_widget_add_controller (GTK_WIDGET (self), scroll);

    gesture = gtk_gesture_zoom_new ();
    g_signal_connect (gesture, "begin", G_CALLBACK (on_zoom_begin), self);
    g_signal_connect (gesture, "scale-changed", G_CALLBACK (on_zoom_changed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (gesture));

    gesture = gtk_gesture_drag_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect (gesture, "drag-begin", G_CALLBACK (on_drag_begin), self);
    g_signal_connect (gesture, "drag-update", G_CALLBACK (on_drag_update), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (gesture));
    gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "grab");
}

/* ------------------------------------------------------------ Preview */

struct _NautilusPdfPreview
{
    GtkWidget parent_instance;

    GtkWidget *stack;
    GtkWidget *scrolled;
    PdfPages *pages;
    GtkWidget *counter;
    GtkWidget *message;
    GCancellable *cancellable;
};

G_DEFINE_TYPE (NautilusPdfPreview, nautilus_pdf_preview, GTK_TYPE_WIDGET)

static void
on_loaded (GObject      *source,
           GAsyncResult *result,
           gpointer      user_data)
{
    g_autoptr (GError) error = NULL;
    Loaded *loaded = g_task_propagate_pointer (G_TASK (result), &error);
    NautilusPdfPreview *self;
    PdfPages *pages;

    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
        g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (result))))
    {
        g_clear_pointer (&loaded, loaded_free);
        return;
    }

    self = NAUTILUS_PDF_PREVIEW (user_data);
    pages = self->pages;
    if (loaded == NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->message), error != NULL ? error->message : "");
        gtk_stack_set_visible_child (GTK_STACK (self->stack), self->message);
        return;
    }

    pdf_pages_clear (pages);
    pages->document = g_steal_pointer (&loaded->document);
    g_array_append_vals (pages->pages, loaded->pages->data, loaded->pages->len);
    /* The pages moved over; don't let the old array unref them. */
    g_array_set_clear_func (loaded->pages, NULL);
    loaded_free (loaded);

    for (guint i = 0; i < pages->pages->len; i++)
    {
        pages->widest = MAX (pages->widest, g_array_index (pages->pages, Page, i).width);
    }
    measure (pages);

    gtk_adjustment_set_value (pages->vadjustment, 0);
    gtk_adjustment_set_value (pages->hadjustment, 0);
    gtk_stack_set_visible_child (GTK_STACK (self->stack), gtk_widget_get_parent (self->scrolled));
    gtk_widget_queue_allocate (GTK_WIDGET (pages));
}

void
nautilus_pdf_preview_set_file (NautilusPdfPreview *self,
                               NautilusFile       *file)
{
    g_autoptr (GTask) task = NULL;

    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
    pdf_pages_clear (self->pages);
    gtk_widget_queue_allocate (GTK_WIDGET (self->pages));
    gtk_stack_set_visible_child (GTK_STACK (self->stack), gtk_widget_get_parent (self->scrolled));

    if (file == NULL || !poppler_load ())
    {
        return;
    }

    self->cancellable = g_cancellable_new ();
    task = g_task_new (NULL, self->cancellable, on_loaded, self);
    g_task_set_task_data (task, nautilus_file_get_uri (file), g_free);
    g_task_run_in_thread (task, load_thread);
}

static void
zoom_middle (NautilusPdfPreview *self,
             double              zoom)
{
    set_zoom (self->pages, zoom,
              gtk_widget_get_width (GTK_WIDGET (self->pages)) / 2,
              gtk_widget_get_height (GTK_WIDGET (self->pages)) / 2);
}

void
nautilus_pdf_preview_zoom (NautilusPdfPreview *self,
                           double              factor)
{
    zoom_middle (self, self->pages->zoom * factor);
}

void
nautilus_pdf_preview_actual_size (NautilusPdfPreview *self)
{
    zoom_middle (self, ACTUAL_SIZE);
}

void
nautilus_pdf_preview_fit (NautilusPdfPreview *self)
{
    PdfPages *pages = self->pages;

    anchor_at (pages, gtk_widget_get_width (GTK_WIDGET (pages)) / 2,
               gtk_widget_get_height (GTK_WIDGET (pages)) / 2);
    pages->fit = TRUE;
    /* The page in view, from its top. */
    pages->pending_page = current_page (pages);
    pages->zoomed_at = g_get_monotonic_time ();
    gtk_widget_queue_allocate (GTK_WIDGET (pages));
}

void
nautilus_pdf_preview_scroll (NautilusPdfPreview *self,
                             double              dx,
                             double              dy)
{
    GtkAdjustment *h = self->pages->hadjustment;
    GtkAdjustment *v = self->pages->vadjustment;

    gtk_adjustment_set_value (h, gtk_adjustment_get_value (h) + dx);
    gtk_adjustment_set_value (v, gtk_adjustment_get_value (v) + dy);
}

void
nautilus_pdf_preview_page (NautilusPdfPreview *self,
                           int                 pages)
{
    if (pages != 0)
    {
        nautilus_pdf_preview_step (self, pages);
    }
}

gboolean
nautilus_pdf_preview_step (NautilusPdfPreview *self,
                           int                 rows)
{
    PdfPages *pages = self->pages;
    GtkAdjustment *v = pages->vadjustment;
    double value = gtk_adjustment_get_value (v);
    guint cols = per_row (pages);
    int n_rows = (pages->pages->len + cols - 1) / cols;
    int row = current_page (pages) / cols;
    double top;
    int target;

    if (n_rows == 0)
    {
        return FALSE;
    }

    /* Partway into a row, back goes to its top first. */
    top = g_array_index (pages->pages, Page, row * cols).row_y - PAD;
    target = rows < 0 && value > top + 1 ? row + rows + 1 : row + rows;
    if (target < 0 || target >= n_rows)
    {
        return FALSE;
    }

    gtk_adjustment_set_value (v, g_array_index (pages->pages, Page, target * cols).row_y - PAD);
    return !G_APPROX_VALUE (gtk_adjustment_get_value (v), value, 0.5);
}

gboolean
nautilus_pdf_preview_fits (NautilusPdfPreview *self)
{
    GtkAdjustment *v = self->pages->vadjustment;

    return self->pages->row_height * self->pages->zoom + 2 * PAD <= gtk_adjustment_get_page_size (v) + 1;
}

static void
update_counter (NautilusPdfPreview *self)
{
    guint n = self->pages->pages->len;
    int page = current_page (self->pages);
    g_autofree char *text = NULL;

    gtk_widget_set_visible (self->counter, n > 1);
    if (n <= 1)
    {
        return;
    }
    if (self->pages->dual && (guint) page + 1 < n)
    {
        text = g_strdup_printf (_("%d–%d of %u"), page + 1, page + 2, n);
    }
    else
    {
        text = g_strdup_printf (_("%d of %u"), page + 1, n);
    }
    gtk_label_set_text (GTK_LABEL (self->counter), text);
}

static void
on_dual_changed (NautilusPdfPreview *self)
{
    PdfPages *pages = self->pages;
    int page = current_page (pages);

    pages->dual = g_settings_get_boolean (nautilus_flier_settings (), "quick-look-pdf-dual");
    measure (pages);
    pages->fit = TRUE;
    pages->pending_page = page;
    gtk_widget_queue_allocate (GTK_WIDGET (pages));
}

gboolean
nautilus_pdf_preview_is_wider (NautilusPdfPreview *self)
{
    GtkAdjustment *h = self->pages->hadjustment;

    return gtk_adjustment_get_upper (h) > gtk_adjustment_get_page_size (h) + 1;
}

static void
nautilus_pdf_preview_dispose (GObject *object)
{
    NautilusPdfPreview *self = NAUTILUS_PDF_PREVIEW (object);

    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
    g_clear_pointer (&self->stack, gtk_widget_unparent);

    G_OBJECT_CLASS (nautilus_pdf_preview_parent_class)->dispose (object);
}

static void
nautilus_pdf_preview_class_init (NautilusPdfPreviewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->dispose = nautilus_pdf_preview_dispose;
    gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
nautilus_pdf_preview_init (NautilusPdfPreview *self)
{
    GtkWidget *overlay;

    self->stack = gtk_stack_new ();
    gtk_widget_set_parent (self->stack, GTK_WIDGET (self));

    self->pages = g_object_new (PDF_TYPE_PAGES, NULL);
    self->scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scrolled), GTK_WIDGET (self->pages));
    overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (overlay), self->scrolled);
    gtk_stack_add_child (GTK_STACK (self->stack), overlay);

    /* Which page, at the bottom. */
    self->counter = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->counter, "pdf-page-counter");
    gtk_widget_set_halign (self->counter, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (self->counter, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom (self->counter, 12);
    gtk_widget_set_can_target (self->counter, FALSE);
    gtk_widget_set_visible (self->counter, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->counter);
    g_signal_connect_object (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled)),
                             "value-changed", G_CALLBACK (update_counter), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled)),
                             "changed", G_CALLBACK (update_counter), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (nautilus_flier_settings (), "changed::quick-look-pdf-dual",
                             G_CALLBACK (on_dual_changed), self, G_CONNECT_SWAPPED);
    self->pages->dual = g_settings_get_boolean (nautilus_flier_settings (), "quick-look-pdf-dual");

    self->message = gtk_label_new (NULL);
    gtk_label_set_wrap (GTK_LABEL (self->message), TRUE);
    gtk_widget_add_css_class (self->message, "dim-label");
    gtk_stack_add_child (GTK_STACK (self->stack), self->message);
}

GtkWidget *
nautilus_pdf_preview_new (void)
{
    return g_object_new (NAUTILUS_TYPE_PDF_PREVIEW, NULL);
}
