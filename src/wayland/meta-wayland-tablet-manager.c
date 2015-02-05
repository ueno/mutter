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
#include "meta-wayland-tablet-manager.h"
#include "meta-wayland-tablet.h"

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static void
notify_tablet_added (MetaWaylandTabletManager *manager,
                     struct wl_resource       *client_resource,
                     ClutterInputDevice       *device)
{
  struct wl_resource *resource;
  MetaWaylandTablet *tablet;
  struct wl_client *client;
  guint vid, pid;

  if (sscanf (clutter_input_device_get_vendor_id (device), "%x", &vid) != 1 ||
      sscanf (clutter_input_device_get_product_id (device), "%x", &pid) != 1)
    return;

  tablet = g_hash_table_lookup (manager->tablets, device);

  if (!tablet)
    return;

  client = wl_resource_get_client (client_resource);

  if (meta_wayland_tablet_lookup_resource (tablet, client))
    return;

  resource = meta_wayland_tablet_create_new_resource (tablet, client,
                                                      client_resource, 0);
  if (!resource)
    return;

  wl_tablet_manager_send_device_added (client_resource, resource,
                                       clutter_input_device_get_device_name (device),
                                       vid, pid, 0);
}

static void
broadcast_tablet_added (MetaWaylandTabletManager *manager,
                        ClutterInputDevice       *device)
{
  struct wl_resource *resource;

  wl_resource_for_each(resource, &manager->resource_list)
    {
      notify_tablet_added (manager, resource, device);
    }
}

static void
notify_tablets (MetaWaylandTabletManager *manager,
                struct wl_resource       *client_resource)
{
  ClutterInputDevice *device;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, manager->tablets);

  while (g_hash_table_iter_next (&iter, (gpointer *) &device, NULL))
    notify_tablet_added (manager, client_resource, device);
}

static void
meta_wayland_tablet_manager_device_added (MetaWaylandTabletManager *manager,
                                          ClutterInputDevice       *device)
{
  ClutterInputDeviceType device_type;

  if (clutter_input_device_get_device_mode (device) == CLUTTER_INPUT_MODE_MASTER)
    return;

  device_type = clutter_input_device_get_device_type (device);

  if (device_type == CLUTTER_TABLET_DEVICE || device_type == CLUTTER_PEN_DEVICE ||
      device_type == CLUTTER_ERASER_DEVICE || device_type == CLUTTER_CURSOR_DEVICE)
    {
      MetaWaylandTablet *tablet;

      tablet = meta_wayland_tablet_new (device, manager);
      g_hash_table_insert (manager->tablets, device, tablet);
      broadcast_tablet_added (manager, device);
    }
}

static void
meta_wayland_tablet_manager_device_removed (MetaWaylandTabletManager *manager,
                                            ClutterInputDevice       *device)
{
  if (g_hash_table_lookup (manager->tablets, device))
    g_hash_table_remove (manager->tablets, device);
}

static void
bind_tablet_manager (struct wl_client *client,
                     void             *data,
                     uint32_t          version,
                     uint32_t          id)
{
  MetaWaylandCompositor *compositor = data;
  MetaWaylandTabletManager *tablet_manager = compositor->tablet_manager;
  struct wl_resource *seat_resource =
    wl_resource_find_for_client (&compositor->seat->base_resource_list, client);
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_tablet_manager_interface,
                                 MIN (version, 1), id);
  wl_resource_set_implementation (resource, NULL, tablet_manager, unbind_resource);
  wl_resource_set_user_data (resource, tablet_manager);
  wl_list_insert(&tablet_manager->resource_list,
                 wl_resource_get_link (resource));

  /* Notify the client of the wl_seat object we're associated with */
  wl_tablet_manager_send_seat (resource, seat_resource);

  /* Notify client of all available tablets */
  notify_tablets (tablet_manager, resource);
}

static MetaWaylandTabletManager *
meta_wayland_tablet_manager_new (MetaWaylandCompositor *compositor)
{
  MetaWaylandTabletManager *tablet_manager;
  ClutterDeviceManager *device_manager;
  const GSList *devices, *l;

  tablet_manager = g_slice_new0 (MetaWaylandTabletManager);
  tablet_manager->compositor = compositor;
  tablet_manager->wl_display = compositor->wayland_display;
  tablet_manager->tablets = g_hash_table_new_full (NULL, NULL, NULL,
                                                   (GDestroyNotify) meta_wayland_tablet_free);
  wl_list_init (&tablet_manager->resource_list);

  device_manager = clutter_device_manager_get_default ();
  g_signal_connect_swapped (device_manager, "device-added",
                            G_CALLBACK (meta_wayland_tablet_manager_device_added),
                            tablet_manager);
  g_signal_connect_swapped (device_manager, "device-removed",
                            G_CALLBACK (meta_wayland_tablet_manager_device_removed),
                            tablet_manager);

  devices = clutter_device_manager_peek_devices (device_manager);

  for (l = devices; l; l = l->next)
    meta_wayland_tablet_manager_device_added (tablet_manager, l->data);

  wl_global_create (tablet_manager->wl_display,
                    &wl_tablet_manager_interface, 1,
                    compositor, bind_tablet_manager);

  return tablet_manager;
}

void
meta_wayland_tablet_manager_init (MetaWaylandCompositor *compositor)
{
  compositor->tablet_manager = meta_wayland_tablet_manager_new (compositor);
}

void
meta_wayland_tablet_manager_free (MetaWaylandTabletManager *tablet_manager)
{
  ClutterDeviceManager *device_manager;

  device_manager = clutter_device_manager_get_default ();
  g_signal_handlers_disconnect_by_data (device_manager, tablet_manager);

  g_hash_table_destroy (tablet_manager->tablets);
  g_slice_free (MetaWaylandTabletManager, tablet_manager);
}

static MetaWaylandTablet *
meta_wayland_tablet_manager_lookup_from_event (MetaWaylandTabletManager *manager,
                                               const ClutterEvent       *event)
{
  ClutterInputDevice *device;

  device = clutter_event_get_source_device (event);
  return g_hash_table_lookup (manager->tablets, device);
}

gboolean
meta_wayland_tablet_manager_consumes_event (MetaWaylandTabletManager *manager,
                                            const ClutterEvent       *event)
{
  return meta_wayland_tablet_manager_lookup_from_event (manager, event) != NULL;
}

void
meta_wayland_tablet_manager_update (MetaWaylandTabletManager *manager,
                                    const ClutterEvent       *event)
{
  MetaWaylandTablet *tablet;

  tablet = meta_wayland_tablet_manager_lookup_from_event (manager, event);

  if (tablet)
    meta_wayland_tablet_update (tablet, event);
}

gboolean
meta_wayland_tablet_manager_handle_event (MetaWaylandTabletManager *manager,
                                          const ClutterEvent       *event)
{
  MetaWaylandTablet *tablet;

  tablet = meta_wayland_tablet_manager_lookup_from_event (manager, event);

  if (!tablet)
    return FALSE;

  return meta_wayland_tablet_handle_event (tablet, event);
}

void
meta_wayland_tablet_manager_update_cursor_position (MetaWaylandTabletManager *manager,
                                                    const ClutterEvent       *event)
{
  ClutterInputDevice *device;
  MetaWaylandTablet *tablet;
  gfloat new_x, new_y;

  device = clutter_event_get_source_device (event);

  if (!device)
    return;

  tablet = meta_wayland_tablet_manager_lookup_from_event (manager, event);

  if (!tablet)
    return;

  clutter_event_get_coords (event, &new_x, &new_y);
  meta_wayland_tablet_update_cursor_position (tablet, new_x, new_y);
}
