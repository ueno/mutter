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
#include "meta-wayland-tablet-tool.h"

static void meta_wayland_tablet_set_focus          (MetaWaylandTablet  *tablet,
                                                    MetaWaylandSurface *surface);

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static void
move_resources (struct wl_list *destination, struct wl_list *source)
{
  wl_list_insert_list (destination, source);
  wl_list_init (source);
}

static void
move_resources_for_client (struct wl_list *destination,
			   struct wl_list *source,
			   struct wl_client *client)
{
  struct wl_resource *resource, *tmp;
  wl_resource_for_each_safe (resource, tmp, source)
    {
      if (wl_resource_get_client (resource) == client)
        {
          wl_list_remove (wl_resource_get_link (resource));
          wl_list_insert (destination, wl_resource_get_link (resource));
        }
    }
}

static void
tablet_handle_focus_surface_destroy (struct wl_listener *listener,
                                     void               *data)
{
  MetaWaylandTablet *tablet;

  tablet = wl_container_of (listener, tablet, focus_surface_destroy_listener);
  meta_wayland_tablet_set_focus (tablet, NULL);
}

MetaWaylandTablet *
meta_wayland_tablet_new (ClutterInputDevice       *device,
                         MetaWaylandTabletManager *manager)
{
  MetaWaylandTablet *tablet;

  tablet = g_slice_new0 (MetaWaylandTablet);
  wl_list_init (&tablet->resource_list);
  wl_list_init (&tablet->focus_resource_list);
  tablet->device = device;
  tablet->manager = manager;

  tablet->focus_surface_destroy_listener.notify = tablet_handle_focus_surface_destroy;

  tablet->cursor_renderer = meta_cursor_renderer_new ();
  meta_cursor_renderer_set_cursor (tablet->cursor_renderer, NULL);

  return tablet;
}

void
meta_wayland_tablet_free (MetaWaylandTablet *tablet)
{
  struct wl_resource *resource, *next;

  meta_wayland_tablet_set_focus (tablet, NULL);
  g_hash_table_destroy (tablet->tools);

  wl_resource_for_each_safe (resource, next, &tablet->resource_list)
    {
      wl_tablet_send_removed (resource);
      wl_resource_destroy (resource);
    }

  g_object_unref (tablet->cursor_renderer);
  g_slice_free (MetaWaylandTablet, tablet);
}

static struct wl_resource *
meta_wayland_tablet_ensure_tool_resource_for_client (MetaWaylandTablet *tablet,
                                                     struct wl_client  *client)
{
  struct wl_resource *tool_resource;

  tool_resource = meta_wayland_tablet_tool_lookup_resource (tablet->current_tool,
                                                            client);
  if (!tool_resource)
    {
      meta_wayland_tablet_manager_notify_tool (tablet->manager, tablet,
                                               tablet->current_tool, client);
      tool_resource = meta_wayland_tablet_tool_lookup_resource (tablet->current_tool,
                                                                client);
    }

  return tool_resource;
}

static void
meta_wayland_tablet_set_focus (MetaWaylandTablet  *tablet,
                               MetaWaylandSurface *surface)
{
  guint32 _time;

  if (tablet->manager->wl_display == NULL)
    return;

  if (tablet->focus_surface == surface)
    return;

  _time = clutter_get_current_event_time ();

  if (tablet->focus_surface != NULL)
    {
      struct wl_resource *resource;
      struct wl_list *l;

      l = &tablet->focus_resource_list;
      if (!wl_list_empty (l))
        {
          wl_resource_for_each (resource, l)
            {
              wl_tablet_send_proximity_out (resource, _time);
            }

          move_resources (&tablet->resource_list, &tablet->focus_resource_list);
        }

      wl_list_remove (&tablet->focus_surface_destroy_listener.link);
      tablet->focus_surface = NULL;
    }

  if (surface != NULL)
    {
      struct wl_resource *resource, *tool_resource;
      struct wl_client *client;
      struct wl_list *l;

      tablet->focus_surface = surface;
      client = wl_resource_get_client (tablet->focus_surface->resource);
      wl_resource_add_destroy_listener (tablet->focus_surface->resource,
                                        &tablet->focus_surface_destroy_listener);

      move_resources_for_client (&tablet->focus_resource_list,
                                 &tablet->resource_list, client);

      tool_resource = meta_wayland_tablet_ensure_tool_resource_for_client (tablet, client);

      l = &tablet->focus_resource_list;
      if (!wl_list_empty (l))
        {
          struct wl_client *client = wl_resource_get_client (tablet->focus_surface->resource);
          struct wl_display *display = wl_client_get_display (client);

          tablet->proximity_serial = wl_display_next_serial (display);

          wl_resource_for_each (resource, l)
            {
              wl_tablet_send_proximity_in (resource, tablet->proximity_serial, _time,
                                           tool_resource, tablet->focus_surface->resource);
            }
        }
    }
}

static void
emit_proximity_in (MetaWaylandTablet  *tablet,
                   struct wl_resource *resource)
{
  struct wl_resource *tool_resource;
  struct wl_client *client;
  guint32 _time;

  if (!tablet->focus_surface || !tablet->current_tool)
    return;

  _time = clutter_get_current_event_time ();
  client = wl_resource_get_client (resource);
  tool_resource = meta_wayland_tablet_ensure_tool_resource_for_client (tablet, client);

  wl_tablet_send_proximity_in (resource, tablet->proximity_serial, _time,
                               tool_resource, tablet->focus_surface->resource);
}

static void
sync_focus_surface (MetaWaylandTablet *tablet)
{
  MetaDisplay *display = meta_get_display ();

  switch (display->event_route)
    {
    case META_EVENT_ROUTE_WINDOW_OP:
    case META_EVENT_ROUTE_COMPOSITOR_GRAB:
    case META_EVENT_ROUTE_FRAME_BUTTON:
      /* The compositor has a grab, so remove our focus */
      meta_wayland_tablet_set_focus (tablet, NULL);
      break;

    case META_EVENT_ROUTE_NORMAL:
    case META_EVENT_ROUTE_WAYLAND_POPUP:
      meta_wayland_tablet_set_focus (tablet, tablet->current);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
repick_for_event (MetaWaylandTablet  *tablet,
                  const ClutterEvent *for_event)
{
  ClutterActor *actor = NULL;

  actor = clutter_event_get_source (for_event);

  if (META_IS_SURFACE_ACTOR_WAYLAND (actor))
    tablet->current = meta_surface_actor_wayland_get_surface (META_SURFACE_ACTOR_WAYLAND (actor));
  else
    tablet->current = NULL;

  sync_focus_surface (tablet);
}

static MetaWaylandTabletTool *
meta_wayland_tablet_ensure_tool (MetaWaylandTablet      *tablet,
                                 ClutterInputDeviceTool *device_tool)
{
  MetaWaylandTabletTool *tool;

  tool = g_hash_table_lookup (tablet->tools, device_tool);

  if (!tool)
    {
      tool = meta_wayland_tablet_tool_new (tablet->device, device_tool);
      g_hash_table_insert (tablet->tools, device_tool, tool);
    }

  return tool;
}

static void
meta_wayland_tablet_account_button (MetaWaylandTablet  *tablet,
                                    const ClutterEvent *event)
{
  if (event->type == CLUTTER_BUTTON_PRESS)
    tablet->buttons = g_slist_append (tablet->buttons,
                                      GUINT_TO_POINTER (event->button.button));
  else
    tablet->buttons = g_slist_remove (tablet->buttons,
                                      GUINT_TO_POINTER (event->button.button));
}

void
meta_wayland_tablet_update (MetaWaylandTablet  *tablet,
                            const ClutterEvent *event)
{
  switch (event->type)
    {
    case CLUTTER_BUTTON_PRESS:
    case CLUTTER_BUTTON_RELEASE:
      meta_wayland_tablet_account_button (tablet, event);
      break;
    case CLUTTER_MOTION:
      if (!tablet->buttons)
        repick_for_event (tablet, event);
      break;
    case CLUTTER_PROXIMITY_IN:
      {
        ClutterInputDeviceTool *tool;

        tool = clutter_event_get_device_tool (event);
        tablet->current_tool = meta_wayland_tablet_ensure_tool (tablet, tool);
        break;
      }
    case CLUTTER_PROXIMITY_OUT:
      tablet->current_tool = NULL;
      break;
    default:
      break;
    }
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
    case CLUTTER_PROXIMITY_IN:
      /* We don't have much info here to make anything useful out of it,
       * wait until the first motion event so we have both coordinates
       * and tool.
       */
      break;
    case CLUTTER_PROXIMITY_OUT:
      meta_wayland_tablet_set_focus (tablet, NULL);
      break;
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

  if (tablet->focus_surface &&
      wl_resource_get_client (tablet->focus_surface->resource) == client)
    {
      wl_list_insert (&tablet->focus_resource_list, wl_resource_get_link (resource));
      emit_proximity_in (tablet, resource);
    }
  else
    {
      wl_list_insert (&tablet->resource_list, wl_resource_get_link (resource));
    }

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
