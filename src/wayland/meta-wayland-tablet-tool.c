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
#include "meta-wayland-private.h"
#include "meta-wayland-tablet-tool.h"

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static guint32
input_device_get_axes (ClutterInputDevice *device)
{
  ClutterInputAxis axis;
  guint32 axes = 0, i;

  for (i = 0; i < clutter_input_device_get_n_axes (device); i++)
    {
      axis = clutter_input_device_get_axis (device, i);

      switch (axis)
        {
        case CLUTTER_INPUT_AXIS_PRESSURE:
          axes |= WL_TABLET_TOOL_AXIS_FLAG_PRESSURE;
          break;
        case CLUTTER_INPUT_AXIS_DISTANCE:
          axes |= WL_TABLET_TOOL_AXIS_FLAG_DISTANCE;
          break;
        case CLUTTER_INPUT_AXIS_XTILT:
        case CLUTTER_INPUT_AXIS_YTILT:
          axes |= WL_TABLET_TOOL_AXIS_FLAG_TILT;
          break;
        default:
          break;
        }
    }

  return axes;
}

static enum wl_tablet_tool_type
input_device_tool_get_type (ClutterInputDeviceTool *device_tool)
{
  ClutterInputDeviceToolType tool_type;

  tool_type = clutter_input_device_tool_get_tool_type (device_tool);

  switch (tool_type)
    {
    case CLUTTER_INPUT_DEVICE_TOOL_NONE:
    case CLUTTER_INPUT_DEVICE_TOOL_PEN:
      return WL_TABLET_TOOL_TYPE_PEN;
    case CLUTTER_INPUT_DEVICE_TOOL_ERASER:
      return WL_TABLET_TOOL_TYPE_ERASER;
    case CLUTTER_INPUT_DEVICE_TOOL_BRUSH:
      return WL_TABLET_TOOL_TYPE_BRUSH;
    case CLUTTER_INPUT_DEVICE_TOOL_PENCIL:
      return WL_TABLET_TOOL_TYPE_PENCIL;
    case CLUTTER_INPUT_DEVICE_TOOL_AIRBRUSH:
      return WL_TABLET_TOOL_TYPE_AIRBRUSH;
    case CLUTTER_INPUT_DEVICE_TOOL_FINGER:
      return WL_TABLET_TOOL_TYPE_FINGER;
    case CLUTTER_INPUT_DEVICE_TOOL_MOUSE:
      return WL_TABLET_TOOL_TYPE_MOUSE;
    case CLUTTER_INPUT_DEVICE_TOOL_LENS:
      return WL_TABLET_TOOL_TYPE_LENS;
    }

  g_assert_not_reached ();
  return 0;
}

MetaWaylandTabletTool *
meta_wayland_tablet_tool_new (ClutterInputDevice     *device,
                              ClutterInputDeviceTool *device_tool)
{
  MetaWaylandTabletTool *tool;

  tool = g_slice_new0 (MetaWaylandTabletTool);
  tool->type = input_device_tool_get_type (device_tool);
  tool->serial = clutter_input_device_tool_get_serial (device_tool);
  wl_list_init (&tool->resource_list);
  tool->axes = input_device_get_axes (device);

  return tool;
}

void
meta_wayland_tablet_tool_free (MetaWaylandTabletTool *tool)
{
  struct wl_resource *resource, *next;

  wl_resource_for_each_safe (resource, next, &tool->resource_list)
    {
      wl_tablet_tool_send_removed (resource);
      wl_resource_destroy (resource);
    }

  g_slice_free (MetaWaylandTabletTool, tool);
}

static void
tool_release (struct wl_client   *client,
              struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_tablet_tool_interface tool_interface = {
  tool_release
};

struct wl_resource *
meta_wayland_tablet_tool_create_new_resource (MetaWaylandTabletTool *tool,
                                              struct wl_client      *client,
                                              struct wl_resource    *seat_resource,
                                              uint32_t               id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_tablet_tool_interface,
                                 wl_resource_get_version (seat_resource), id);
  wl_resource_set_implementation (resource, &tool_interface,
                                  tool, unbind_resource);
  wl_resource_set_user_data (resource, tool);
  wl_list_insert (&tool->resource_list, wl_resource_get_link (resource));

  return resource;
}

struct wl_resource *
meta_wayland_tablet_tool_lookup_resource (MetaWaylandTabletTool *tool,
                                          struct wl_client      *client)
{
  return wl_resource_find_for_client (&tool->resource_list, client);
}
