/* vim:set et sw=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e2s: */
/*
 * Copyright © Philip Withnall 2015, 2016 <philip@tecnocode.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:dwl-timeline
 * @short_description: Dunfell timeline renderer
 * @stability: Unstable
 * @include: libdunfell-ui/dwl-timeline.h
 *
 * TODO
 *
 * Since: UNRELEASED
 */

#include "config.h"

#include <errno.h>
#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>

#include "dfl-main-context.h"
#include "dfl-source.h"
#include "dfl-thread.h"
#include "dfl-time-sequence.h"
#include "dfl-types.h"
#include "dwl-timeline.h"


static void dwl_timeline_get_property (GObject    *object,
                                       guint       property_id,
                                       GValue     *value,
                                       GParamSpec *pspec);
static void dwl_timeline_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec);
static void dwl_timeline_dispose (GObject *object);
static void dwl_timeline_realize (GtkWidget *widget);
static void dwl_timeline_unrealize (GtkWidget *widget);
static void dwl_timeline_map (GtkWidget *widget);
static void dwl_timeline_unmap (GtkWidget *widget);
static void dwl_timeline_size_allocate (GtkWidget     *widget,
                                        GtkAllocation *allocation);
static gboolean dwl_timeline_draw (GtkWidget *widget,
                                   cairo_t   *cr);
static void dwl_timeline_get_preferred_width (GtkWidget *widget,
                                              gint      *minimum_width,
                                              gint      *natural_width);
static void dwl_timeline_get_preferred_height (GtkWidget *widget,
                                               gint      *minimum_height,
                                               gint      *natural_height);
static gboolean dwl_timeline_scroll_event (GtkWidget      *widget,
                                           GdkEventScroll *event);

static void add_default_css (GtkStyleContext *context);
static void update_cache    (DwlTimeline     *self);

#define ZOOM_MIN 0.001
#define ZOOM_MAX 1000.0

struct _DwlTimeline
{
  GtkWidget parent;

  GdkWindow *event_window;  /* owned */

  GPtrArray/*<owned DflMainContext>*/ *main_contexts;  /* owned */
  GPtrArray/*<owned DflThread>*/ *threads;  /* owned */
  GPtrArray/*<owned DflSource>*/ *sources;  /* owned */

  gfloat zoom;

  /* Cached dimensions. */
  DflTimestamp min_timestamp;
  DflTimestamp max_timestamp;
  DflDuration duration;
};

typedef enum
{
  PROP_ZOOM = 1,
} DwlTimelineProperty;

G_DEFINE_TYPE (DwlTimeline, dwl_timeline, GTK_TYPE_WIDGET)

static void
dwl_timeline_class_init (DwlTimelineClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = dwl_timeline_get_property;
  object_class->set_property = dwl_timeline_set_property;
  object_class->dispose = dwl_timeline_dispose;

  widget_class->realize = dwl_timeline_realize;
  widget_class->unrealize = dwl_timeline_unrealize;
  widget_class->map = dwl_timeline_map;
  widget_class->unmap = dwl_timeline_unmap;
  widget_class->size_allocate = dwl_timeline_size_allocate;
  widget_class->draw = dwl_timeline_draw;
  widget_class->get_preferred_width = dwl_timeline_get_preferred_width;
  widget_class->get_preferred_height = dwl_timeline_get_preferred_height;
  widget_class->scroll_event = dwl_timeline_scroll_event;

  /* TODO: Proper accessibility support. */
  gtk_widget_class_set_accessible_role (widget_class, ATK_ROLE_CHART);
  gtk_widget_class_set_css_name (widget_class, "timeline");

  /**
   * DwlTimeline:zoom:
   *
   * TODO
   *
   * Since: UNRELEASED
   */
  g_object_class_install_property (object_class, PROP_ZOOM,
                                   g_param_spec_float ("zoom", "Zoom",
                                                       "Zoom level.",
                                                       ZOOM_MIN, ZOOM_MAX, 1.0,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));
}

static void
dwl_timeline_init (DwlTimeline *self)
{
  self->zoom = 1.0;

  add_default_css (gtk_widget_get_style_context (GTK_WIDGET (self)));

  gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
  gtk_widget_add_events (GTK_WIDGET (self), GDK_SCROLL_MASK);
}

static void
dwl_timeline_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  DwlTimeline *self = DWL_TIMELINE (object);

  switch ((DwlTimelineProperty) property_id)
    {
    case PROP_ZOOM:
      g_value_set_float (value, self->zoom);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
dwl_timeline_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  DwlTimeline *self = DWL_TIMELINE (object);

  switch ((DwlTimelineProperty) property_id)
    {
    case PROP_ZOOM:
      dwl_timeline_set_zoom (self, g_value_get_float (value));
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
dwl_timeline_dispose (GObject *object)
{
  DwlTimeline *self = DWL_TIMELINE (object);

  g_clear_pointer (&self->sources, g_ptr_array_unref);
  g_clear_pointer (&self->main_contexts, g_ptr_array_unref);
  g_clear_pointer (&self->threads, g_ptr_array_unref);

  /* Chain up to the parent class */
  G_OBJECT_CLASS (dwl_timeline_parent_class)->dispose (object);
}

/**
 * dwl_timeline_new:
 * @threads: (element-type DflThread): TODO
 * @main_contexts: (element-type DflMainContext): TODO
 * @sources: (element-type DflSource): TODO
 *
 * TODO
 *
 * Returns: (transfer full): a new #DwlTimeline
 * Since: UNRELEASED
 */
DwlTimeline *
dwl_timeline_new (GPtrArray *threads,
                  GPtrArray *main_contexts,
                  GPtrArray *sources)
{
  DwlTimeline *timeline = NULL;

  /* TODO: Properties. */
  timeline = g_object_new (DWL_TYPE_TIMELINE, NULL);

  timeline->threads = g_ptr_array_ref (threads);
  timeline->main_contexts = g_ptr_array_ref (main_contexts);
  timeline->sources = g_ptr_array_ref (sources);

  update_cache (timeline);

  return timeline;
}

static void
add_default_css (GtkStyleContext *context)
{
  GtkCssProvider *provider = NULL;
  GError *error = NULL;
  const gchar *css;

  css =
    "timeline.thread_guide { color: #cccccc }\n"
    "timeline.thread { color: rgb(139, 142, 143) }\n"
    "timeline.main_context_dispatch { background-color: red; "
                                     "border: 1px solid black }\n"
    "timeline.source { background-color: blue; "
                      "color: #cccccc }\n";

  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (provider, css, -1, &error);
  g_assert_no_error (error);

  gtk_style_context_add_provider (context, GTK_STYLE_PROVIDER (provider),
                                  GTK_STYLE_PROVIDER_PRIORITY_FALLBACK);

  g_object_unref (provider);
}

#define THREAD_MIN_WIDTH 100 /* pixels */
#define THREAD_NATURAL_WIDTH 140 /* pixels */
#define HEADER_HEIGHT 100 /* pixels */
#define FOOTER_HEIGHT 30 /* pixels */
#define MAIN_CONTEXT_ACQUIRED_WIDTH 3 /* pixels */
#define MAIN_CONTEXT_DISPATCH_WIDTH 10 /* pixels */
#define SOURCE_BORDER_WIDTH 1 /* pixel */
#define SOURCE_OFFSET 20 /* pixels */
#define SOURCE_WIDTH 10 /* pixels */

/* Calculate various values from the data model we have (the threads, main
 * contexts and sources). The calculated values will be used frequently when
 * drawing. */
static void
update_cache (DwlTimeline *self)
{
  guint i;
  DflTimestamp min_timestamp, max_timestamp;

  min_timestamp = G_MAXUINT64;
  max_timestamp = 0;

  for (i = 0; i < self->threads->len; i++)
    {
      DflThread *thread = self->threads->pdata[i];
      min_timestamp = MIN (min_timestamp, dfl_thread_get_new_timestamp (thread));
      max_timestamp = MAX (max_timestamp, dfl_thread_get_free_timestamp (thread));
    }

  g_assert (max_timestamp >= min_timestamp);

  /* Update the cache. */
  self->min_timestamp = min_timestamp;
  self->max_timestamp = max_timestamp;
  self->duration = max_timestamp - min_timestamp;
}

static gint
timestamp_to_pixels (DwlTimeline  *self,
                     DflTimestamp  timestamp)
{
  g_return_val_if_fail (timestamp <= G_MAXINT / self->zoom, G_MAXINT);
  return HEADER_HEIGHT + timestamp * self->zoom;
}

static gint
duration_to_pixels (DwlTimeline *self,
                    DflDuration  duration)
{
  g_return_val_if_fail (duration <= G_MAXINT / self->zoom, G_MAXINT);
  return duration * self->zoom;
}

static void
dwl_timeline_realize (GtkWidget *widget)
{
  DwlTimeline *self = DWL_TIMELINE (widget);
  GdkWindow *parent_window;
  GdkWindowAttr attributes;
  gint attributes_mask;
  GtkAllocation allocation;

  gtk_widget_set_realized (widget, TRUE);
  parent_window = gtk_widget_get_parent_window (widget);
  gtk_widget_set_window (widget, parent_window);
  g_object_ref (parent_window);

  gtk_widget_get_allocation (widget, &allocation);

  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.wclass = GDK_INPUT_ONLY;
  attributes.x = allocation.x;
  attributes.y = allocation.y;
  attributes.width = allocation.width;
  attributes.height = allocation.height;
  attributes.event_mask = gtk_widget_get_events (widget) |
                          GDK_SCROLL_MASK;
  attributes_mask = GDK_WA_X | GDK_WA_Y;

  self->event_window = gdk_window_new (parent_window,
                                       &attributes,
                                       attributes_mask);
  gtk_widget_register_window (widget, self->event_window);
}

static void
dwl_timeline_unrealize (GtkWidget *widget)
{
  DwlTimeline *self = DWL_TIMELINE (widget);

  if (self->event_window != NULL)
    {
      gtk_widget_unregister_window (widget, self->event_window);
      gdk_window_destroy (self->event_window);
      self->event_window = NULL;
    }

  GTK_WIDGET_CLASS (dwl_timeline_parent_class)->unrealize (widget);
}

static void
dwl_timeline_map (GtkWidget *widget)
{
  DwlTimeline *self = DWL_TIMELINE (widget);

  GTK_WIDGET_CLASS (dwl_timeline_parent_class)->map (widget);

  if (self->event_window)
    gdk_window_show (self->event_window);
}

static void
dwl_timeline_unmap (GtkWidget *widget)
{
  DwlTimeline *self = DWL_TIMELINE (widget);

  if (self->event_window)
    gdk_window_hide (self->event_window);

  GTK_WIDGET_CLASS (dwl_timeline_parent_class)->unmap (widget);
}

static void
dwl_timeline_size_allocate (GtkWidget     *widget,
                            GtkAllocation *allocation)
{
  DwlTimeline *self = DWL_TIMELINE (widget);

  gtk_widget_set_allocation (widget, allocation);

  if (gtk_widget_get_realized (widget))
    gdk_window_move_resize (self->event_window,
                            allocation->x,
                            allocation->y,
                            allocation->width,
                            allocation->height);
}

static gboolean
dwl_timeline_draw (GtkWidget *widget,
                   cairo_t   *cr)
{
  DwlTimeline *self = DWL_TIMELINE (widget);
  GtkStyleContext *context;
  gint widget_width;
  guint i, n_threads;
  DflTimestamp min_timestamp, max_timestamp;

  context = gtk_widget_get_style_context (widget);
  widget_width = gtk_widget_get_allocated_width (widget);

  n_threads = self->threads->len;
  min_timestamp = self->min_timestamp;
  max_timestamp = self->max_timestamp;

  /* Draw the threads. */
  for (i = 0; i < n_threads; i++)
    {
      DflThread *thread = self->threads->pdata[i];
      gdouble thread_centre;
      PangoLayout *layout = NULL;
      gchar *text = NULL;
      PangoRectangle layout_rect;

      thread_centre = widget_width / n_threads * (2 * i + 1) / 2;

      /* Guide line for the entire length of the thread. */
      gtk_style_context_add_class (context, "thread_guide");
      gtk_render_line (context, cr,
                       thread_centre,
                       timestamp_to_pixels (self, 0),
                       thread_centre,
                       timestamp_to_pixels (self, max_timestamp - min_timestamp));
      gtk_style_context_remove_class (context, "thread_guide");

      /* Line for the actual live length of the thread, plus its label. */
      gtk_style_context_add_class (context, "thread");
      gtk_render_line (context, cr,
                       thread_centre,
                       timestamp_to_pixels (self, dfl_thread_get_new_timestamp (thread) - min_timestamp),
                       thread_centre,
                       timestamp_to_pixels (self, dfl_thread_get_free_timestamp (thread) - min_timestamp));
      gtk_style_context_remove_class (context, "thread");

      /* Thread label. */
      gtk_style_context_add_class (context, "thread_header");

      text = g_strdup_printf ("Thread %" G_GUINT64_FORMAT, dfl_thread_get_id (thread));
      layout = gtk_widget_create_pango_layout (widget, text);
      g_free (text);

      pango_layout_get_pixel_extents (layout, NULL, &layout_rect);

      gtk_render_layout (context, cr,
                         thread_centre - layout_rect.width / 2,
                         HEADER_HEIGHT / 2 - layout_rect.height / 2,
                         layout);
      g_object_unref (layout);

      gtk_style_context_remove_class (context, "thread_header");
    }

  /* Draw the main contexts on top. */
  for (i = 0; i < self->main_contexts->len; i++)
    {
      DflMainContext *main_context = self->main_contexts->pdata[i];
      DflTimeSequenceIter iter;
      DflTimestamp timestamp;
      DflThreadOwnershipData *data;
      GdkRGBA color;

      /* Iterate through the thread ownership events. */
      gtk_style_context_add_class (context, "main_context");
      cairo_save (cr);

      gtk_style_context_get_color (context, gtk_widget_get_state_flags (widget),
                                   &color);

      cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_width (cr, MAIN_CONTEXT_ACQUIRED_WIDTH);
      cairo_new_path (cr);

      /* TODO: Set the start timestamp according to the clip area */
      dfl_main_context_thread_ownership_iter (main_context, &iter, 0);

      while (dfl_time_sequence_iter_next (&iter, &timestamp, (gpointer *) &data))
        {
          gdouble thread_centre;
          gint timestamp_y;
          guint thread_index;

          /* TODO: This should not be so slow. */
          for (thread_index = 0; thread_index < n_threads; thread_index++)
            {
              if (dfl_thread_get_id (self->threads->pdata[thread_index]) ==
                  data->thread_id)
                break;
            }
          g_assert (thread_index < n_threads);

          thread_centre = widget_width / n_threads * (2 * thread_index + 1) / 2;
          timestamp_y = timestamp_to_pixels (self, timestamp - min_timestamp);

          cairo_line_to (cr,
                         thread_centre + 0.5,
                         timestamp_y + 0.5);
          cairo_line_to (cr,
                         thread_centre + 0.5,
                         timestamp_y +
                         duration_to_pixels (self, data->duration) + 0.5);
        }

      gdk_cairo_set_source_rgba (cr, &color);
      cairo_stroke (cr);

      cairo_restore (cr);
      gtk_style_context_remove_class (context, "main_context");

      /* Iterate through the dispatch events. */
      gtk_style_context_add_class (context, "main_context_dispatch");

      /* TODO: Set the start timestamp according to the clip area */
      dfl_main_context_dispatch_iter (main_context, &iter, 0);

      while (dfl_time_sequence_iter_next (&iter, &timestamp, (gpointer *) &data))
        {
          gdouble thread_centre, dispatch_width, dispatch_height;
          gint timestamp_y;
          guint thread_index;

          /* TODO: This should not be so slow. */
          for (thread_index = 0; thread_index < n_threads; thread_index++)
            {
              if (dfl_thread_get_id (self->threads->pdata[thread_index]) ==
                  data->thread_id)
                break;
            }
          g_assert (thread_index < n_threads);

          thread_centre = widget_width / n_threads * (2 * thread_index + 1) / 2;
          timestamp_y = timestamp_to_pixels (self, timestamp - min_timestamp);

          dispatch_width = MAIN_CONTEXT_DISPATCH_WIDTH;
          dispatch_height = duration_to_pixels (self, data->duration);

          gtk_render_background (context, cr,
                                 thread_centre - dispatch_width / 2.0,
                                 timestamp_y,
                                 dispatch_width,
                                 dispatch_height);
          gtk_render_frame (context, cr,
                            thread_centre - dispatch_width / 2.0,
                            timestamp_y,
                            dispatch_width,
                            dispatch_height);
        }

      gtk_style_context_remove_class (context, "main_context_dispatch");
    }

  /* Draw the sources either side. */
  for (i = 0; i < self->sources->len; i++)
    {
      DflSource *source = self->sources->pdata[i];
      gdouble thread_centre, source_x, source_y;
      guint thread_index;
      GdkRGBA color;

      /* TODO: This should not be so slow. */
      for (thread_index = 0; thread_index < n_threads; thread_index++)
        {
          if (dfl_thread_get_id (self->threads->pdata[thread_index]) ==
              dfl_source_get_new_thread_id (source))
            break;
        }
      g_assert (thread_index < n_threads);

      thread_centre = widget_width / n_threads * (2 * thread_index + 1) / 2;

      /* Source circle. */
      gtk_style_context_add_class (context, "source");
      cairo_save (cr);

      cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
      cairo_set_line_width (cr, SOURCE_BORDER_WIDTH);
      cairo_new_path (cr);

      /* Calculate the centre of the source. */
      source_x = thread_centre - SOURCE_OFFSET;
      source_y = timestamp_to_pixels (self, dfl_source_get_new_timestamp (source) - min_timestamp);

      cairo_arc (cr,
                 source_x,
                 source_y,
                 SOURCE_WIDTH / 2.0,
                 0.0, 2 * M_PI);

      cairo_clip_preserve (cr);
      gtk_render_background (context, cr,
                             source_x - SOURCE_WIDTH / 2.0,
                             source_y - SOURCE_WIDTH / 2.0,
                             SOURCE_WIDTH,
                             SOURCE_WIDTH);

      gtk_style_context_get_color (context, gtk_widget_get_state_flags (widget),
                                   &color);
      gdk_cairo_set_source_rgba (cr, &color);
      cairo_stroke (cr);

      cairo_restore (cr);
      gtk_style_context_remove_class (context, "source");
    }

  return FALSE;
}

static void
dwl_timeline_get_preferred_width (GtkWidget *widget,
                                  gint      *minimum_width,
                                  gint      *natural_width)
{
  DwlTimeline *self = DWL_TIMELINE (widget);
  guint n_threads;

  n_threads = self->threads->len;

  if (minimum_width != NULL)
    *minimum_width = MAX (1, n_threads * THREAD_MIN_WIDTH);
  if (natural_width != NULL)
    *natural_width = MAX (1, n_threads * THREAD_NATURAL_WIDTH);
}

static void
dwl_timeline_get_preferred_height (GtkWidget *widget,
                                   gint      *minimum_height,
                                   gint      *natural_height)
{
  DwlTimeline *self = DWL_TIMELINE (widget);
  gint height;

  /* What’s the maximum height of any of the threads? */
  height = timestamp_to_pixels (self,
                                self->max_timestamp - self->min_timestamp);

  if (height > 0)
    height += FOOTER_HEIGHT;

  if (minimum_height != NULL)
    *minimum_height = MAX (1, height);
  if (natural_height != NULL)
    *natural_height = MAX (1, height);
}

#define SCROLL_SMOOTH_FACTOR_SCALE 2.0 /* pixels per unit zoom factor */

static gboolean
dwl_timeline_scroll_event (GtkWidget      *widget,
                           GdkEventScroll *event)
{
  DwlTimeline *self = DWL_TIMELINE (widget);

  /* If the user is holding down Ctrl, change the zoom level. Otherwise, pass
   * the scroll event through to other widgets. */
  if (event->state & GDK_CONTROL_MASK)
    {
      gdouble factor;
      gdouble delta;
      gfloat old_zoom;

      switch (event->direction)
        {
        case GDK_SCROLL_UP:
          factor = 2.0;
          break;
        case GDK_SCROLL_DOWN:
          factor = 0.5;
          break;
        case GDK_SCROLL_SMOOTH:
          g_assert (gdk_event_get_scroll_deltas ((GdkEvent *) event, NULL,
                                                 &delta));

          /* Process the delta. */
          if (delta == 0.0)
            factor = 1.0;
          else if (delta > 0.0)
            factor = delta / SCROLL_SMOOTH_FACTOR_SCALE;
          else
            factor = SCROLL_SMOOTH_FACTOR_SCALE / -delta;

          break;
        case GDK_SCROLL_LEFT:
        case GDK_SCROLL_RIGHT:
        default:
          factor = 1.0;
          break;
        }

      old_zoom = dwl_timeline_get_zoom (self);
      dwl_timeline_set_zoom (self, old_zoom * factor);

      return GDK_EVENT_STOP;
    }

  return GDK_EVENT_PROPAGATE;
}

/**
 * dwl_timeline_get_zoom:
 * @self: a #DwlTimeline
 *
 * TODO
 *
 * Returns: TODO
 * Since: UNRELEASED
 */
gfloat
dwl_timeline_get_zoom (DwlTimeline *self)
{
  g_return_val_if_fail (DWL_IS_TIMELINE (self), 1.0);

  return self->zoom;
}

/**
 * dwl_timeline_set_zoom:
 * @self: a #DwlTimeline
 * @zoom: new zoom value, between %ZOOM_MIN and %ZOOM_MAX
 *
 * TODO
 *
 * Returns: %TRUE if the zoom level changed, %FALSE otherwise
 * Since: UNRELEASED
 */
gboolean
dwl_timeline_set_zoom (DwlTimeline *self,
                       gfloat       zoom)
{
  gfloat new_zoom;

  g_return_val_if_fail (DWL_IS_TIMELINE (self), FALSE);

  new_zoom = CLAMP (zoom, ZOOM_MIN, ZOOM_MAX);

  if (new_zoom == self->zoom)
    return FALSE;

  g_debug ("%s: Setting zoom to %f", G_STRFUNC, new_zoom);

  self->zoom = new_zoom;
  g_object_notify (G_OBJECT (self), "zoom");
  gtk_widget_queue_resize (GTK_WIDGET (self));

  return TRUE;
}
