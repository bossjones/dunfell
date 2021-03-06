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

#ifndef DFL_EVENT_SEQUENCE_H
#define DFL_EVENT_SEQUENCE_H

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include "event.h"

G_BEGIN_DECLS

/**
 * DflEventSequence:
 *
 * All the fields in this structure are private.
 *
 * Since: 0.1.0
 */
#define DFL_TYPE_EVENT_SEQUENCE dfl_event_sequence_get_type ()
G_DECLARE_FINAL_TYPE (DflEventSequence, dfl_event_sequence, DFL, EVENT_SEQUENCE, GObject)

DflEventSequence *dfl_event_sequence_new (const DflEvent **events,
                                          guint            n_events,
                                          DflTimestamp     initial_timestamp);

/**
 * DflEventWalker:
 * @sequence: a #DflEventSequence
 * @event: (transfer none): the current event
 * @user_data: user data passed in to dfl_event_sequence_add_walker()
 *
 * Callback from dfl_event_sequence_walk() emitted for each event in the
 * sequence, in order. The @user_data is as passed in to
 * dfl_event_sequence_add_walker().
 *
 * The sequence must not be modified while walking over it.
 *
 * Since: 0.1.0
 */
typedef void (*DflEventWalker) (DflEventSequence *sequence,
                                DflEvent         *event,
                                gpointer          user_data);

void  dfl_event_sequence_start_walker_group (DflEventSequence *self);
void  dfl_event_sequence_end_walker_group   (DflEventSequence *self,
                                             const gchar      *event_type,
                                             DflId             id);

guint dfl_event_sequence_add_walker    (DflEventSequence *self,
                                        const gchar      *event_type,
                                        DflId             id,
                                        DflEventWalker    walker,
                                        gpointer          user_data,
                                        GDestroyNotify    destroy_user_data);
void  dfl_event_sequence_remove_walker (DflEventSequence *self,
                                        guint             walker_id);

void  dfl_event_sequence_walk          (DflEventSequence *self);

G_END_DECLS

#endif /* !DFL_EVENT_SEQUENCE_H */
