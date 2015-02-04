/*
 * Wayland Support
 *
 * Copyright (C) 2015 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#define _GNU_SOURCE

#include "config.h"

#include <glib.h>

#include <wayland-server.h>
#include "wayland-tablet-server-protocol.h"

#include "meta-surface-actor-wayland.h"
#include "meta-wayland-private.h"
#include "meta-wayland-tablet.h"

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

MetaWaylandTablet *
meta_wayland_tablet_new (ClutterInputDevice       *device,
                         MetaWaylandTabletManager *manager)
{
  MetaWaylandTablet *tablet;

  tablet = g_slice_new0 (MetaWaylandTablet);
  wl_list_init (&tablet->resource_list);
  tablet->device = device;
  tablet->manager = manager;

  tablet->cursor_renderer = meta_cursor_renderer_new ();
  meta_cursor_renderer_set_cursor (tablet->cursor_renderer, NULL);

  return tablet;
}

void
meta_wayland_tablet_free (MetaWaylandTablet *tablet)
{
  struct wl_resource *resource, *next;

  wl_resource_for_each_safe (resource, next, &tablet->resource_list)
    {
      wl_tablet_send_removed (resource);
      wl_resource_destroy (resource);
    }

  g_object_unref (tablet->cursor_renderer);
  g_slice_free (MetaWaylandTablet, tablet);
}

void
meta_wayland_tablet_update (MetaWaylandTablet  *tablet,
                            const ClutterEvent *event)
{
}

static void
handle_motion_event (MetaWaylandTablet  *tablet,
                     const ClutterEvent *event)
{
}

static void
handle_button_event (MetaWaylandTablet  *tablet,
                     const ClutterEvent *event)
{
}

gboolean
meta_wayland_tablet_handle_event (MetaWaylandTablet  *tablet,
                                  const ClutterEvent *event)
{
  switch (event->type)
    {
    case CLUTTER_MOTION:
      handle_motion_event (tablet, event);
      break;
    case CLUTTER_BUTTON_PRESS:
    case CLUTTER_BUTTON_RELEASE:
      handle_button_event (tablet, event);
      break;
    default:
      return FALSE;
    }
  return TRUE;
}

static void
tablet_release (struct wl_client   *client,
                struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
tablet_set_cursor (struct wl_client   *client,
                   struct wl_resource *resource,
                   uint32_t            serial,
                   struct wl_resource *surface_resource,
                   int32_t             hotspot_x,
                   int32_t             hotspot_y)
{
}

static const struct wl_tablet_interface tablet_interface = {
  tablet_release,
  tablet_set_cursor
};

struct wl_resource *
meta_wayland_tablet_create_new_resource (MetaWaylandTablet  *tablet,
                                         struct wl_client   *client,
                                         struct wl_resource *seat_resource,
                                         uint32_t            id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_tablet_interface,
                                 wl_resource_get_version (seat_resource), id);
  wl_resource_set_implementation (resource, &tablet_interface,
                                  tablet, unbind_resource);
  wl_resource_set_user_data (resource, tablet);
  wl_list_insert (&tablet->resource_list, wl_resource_get_link (resource));

  return resource;
}

void
meta_wayland_tablet_update_cursor_position (MetaWaylandTablet *tablet,
                                            int                new_x,
                                            int                new_y)
{
  meta_cursor_renderer_set_position (tablet->cursor_renderer, new_x, new_y);
}

struct wl_resource *
meta_wayland_tablet_lookup_resource (MetaWaylandTablet *tablet,
                                     struct wl_client  *client)
{
  struct wl_resource *resource;

  resource = wl_resource_find_for_client (&tablet->resource_list, client);

  if (!resource)
    resource = wl_resource_find_for_client (&tablet->focus_resource_list, client);

  return resource;
}
