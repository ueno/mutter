/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
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
 * Written by:
 *     Daiki Ueno <dueno@src.gnome.org>
 */

/* The file is based on src/text-backend.c from Weston */

#include "config.h"

#include <string.h>
#include "meta-wayland-private.h"
#include "meta-wayland-input-method.h"
#include "input-method-server-protocol.h"
#include "text-server-protocol.h"

struct _MetaWaylandTextInput
{
  struct wl_resource *resource;
  MetaWaylandInputMethod *input_method;
  MetaWaylandSurface *surface;
};

struct _MetaWaylandInputMethodContext
{
  struct wl_resource *resource;

  MetaWaylandTextInput *text_input;
  MetaWaylandInputMethod *input_method;

  struct wl_resource *keyboard_resource;
};

static void
input_method_context_create (MetaWaylandTextInput *text_input,
                             MetaWaylandInputMethod *input_method);

static void
input_method_context_end_keyboard_grab (MetaWaylandInputMethodContext *context)
{
  MetaWaylandKeyboardGrab *grab;
  MetaWaylandKeyboard *keyboard;

  grab = &context->input_method->grab;
  keyboard = grab->keyboard;
  if (!keyboard)
    return;

  if (keyboard->grab == grab)
    meta_wayland_keyboard_end_grab (keyboard);

  context->input_method->keyboard_resource = NULL;
}

static void
deactivate_input_method (MetaWaylandInputMethod *input_method)
{
  MetaWaylandTextInput *text_input = input_method->text_input;

  if (input_method->context && input_method->resource)
    {
      input_method_context_end_keyboard_grab (input_method->context);
      wl_input_method_send_deactivate (input_method->resource,
                                       input_method->context->resource);
    }

  input_method->text_input = NULL;
  input_method->context = NULL;

  if (text_input && text_input->resource)
    wl_text_input_send_leave (text_input->resource);
}

static void
text_input_activate (struct wl_client *client,
                     struct wl_resource *resource,
                     struct wl_resource *seat_resource,
                     struct wl_resource *surface_resource)
{
  MetaWaylandTextInput *text_input = wl_resource_get_user_data (resource);
  MetaWaylandSeat *seat = wl_resource_get_user_data (seat_resource);
  MetaWaylandInputMethod *input_method = &seat->input_method;

  if (input_method->text_input == text_input)
    return;

  if (input_method->text_input)
    deactivate_input_method (input_method);

  input_method->text_input = text_input;
  text_input->input_method = input_method;

  text_input->surface = wl_resource_get_user_data (surface_resource);
  input_method_context_create (text_input, input_method);

  wl_text_input_send_enter (resource, surface_resource);
}

static void
text_input_deactivate (struct wl_client *client,
                       struct wl_resource *resource,
                       struct wl_resource *seat_resource)
{
  MetaWaylandSeat *seat = wl_resource_get_user_data (seat_resource);
  MetaWaylandInputMethod *input_method = &seat->input_method;

  if (input_method->text_input)
    deactivate_input_method (input_method);
}

static void
text_input_show_input_panel (struct wl_client *client,
                             struct wl_resource *resource)
{
  /* We don't implement input_panel stuff.  */
}

static void
text_input_hide_input_panel (struct wl_client *client,
                             struct wl_resource *resource)
{
  /* We don't implement input_panel stuff.  */
}

static void
text_input_reset (struct wl_client *client,
                  struct wl_resource *resource)
{
  MetaWaylandTextInput *text_input = wl_resource_get_user_data (resource);
  MetaWaylandInputMethod *input_method = text_input->input_method;

  if (!input_method->context)
    return;

  wl_input_method_context_send_reset (input_method->context->resource);
}

static void
text_input_set_surrounding_text (struct wl_client *client,
                                 struct wl_resource *resource,
                                 const char *text,
                                 uint32_t cursor,
                                 uint32_t anchor)
{
  MetaWaylandTextInput *text_input = wl_resource_get_user_data (resource);
  MetaWaylandInputMethod *input_method = text_input->input_method;

  if (!input_method->context)
    return;

  wl_input_method_context_send_surrounding_text (input_method->context->resource,
                                                 text,
                                                 cursor,
                                                 anchor);
}

static void
text_input_set_content_type (struct wl_client *client,
                             struct wl_resource *resource,
                             uint32_t hint,
                             uint32_t purpose)
{
  MetaWaylandTextInput *text_input = wl_resource_get_user_data (resource);
  MetaWaylandInputMethod *input_method = text_input->input_method;

  if (!input_method->context)
    return;

  wl_input_method_context_send_content_type (input_method->context->resource,
                                             hint, purpose);
}

static void
text_input_set_cursor_rectangle (struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t x,
                                 int32_t y,
                                 int32_t width,
                                 int32_t height)
{
  /* We don't implement input_panel stuff.  */
}

static void
text_input_set_preferred_language (struct wl_client *client,
                                   struct wl_resource *resource,
                                   const char *language)
{
  MetaWaylandTextInput *text_input = wl_resource_get_user_data (resource);
  MetaWaylandInputMethod *input_method = text_input->input_method;

  if (!input_method->context)
    return;

  wl_input_method_context_send_preferred_language (input_method->context->resource,
                                                   language);
}

static void
text_input_commit_state (struct wl_client *client,
                         struct wl_resource *resource,
                         uint32_t serial)
{
  MetaWaylandTextInput *text_input = wl_resource_get_user_data (resource);
  MetaWaylandInputMethod *input_method = text_input->input_method;

  if (!input_method->context)
    return;

  wl_input_method_context_send_commit_state (input_method->context->resource,
                                             serial);
}

static void
text_input_invoke_action (struct wl_client *client,
                          struct wl_resource *resource,
                          uint32_t button,
                          uint32_t index)
{
  MetaWaylandTextInput *text_input = wl_resource_get_user_data (resource);
  MetaWaylandInputMethod *input_method = text_input->input_method;

  if (!input_method->context)
    return;

  wl_input_method_context_send_invoke_action (input_method->context->resource,
                                              button, index);
}

static const struct wl_text_input_interface
text_input_interface = {
  text_input_activate,
  text_input_deactivate,
  text_input_show_input_panel,
  text_input_hide_input_panel,
  text_input_reset,
  text_input_set_surrounding_text,
  text_input_set_content_type,
  text_input_set_cursor_rectangle,
  text_input_set_preferred_language,
  text_input_commit_state,
  text_input_invoke_action
};

static void
destroy_text_input (struct wl_resource *resource)
{
  MetaWaylandTextInput *text_input = wl_resource_get_user_data (resource);

  if (text_input->input_method)
    deactivate_input_method (text_input->input_method);

  g_free (text_input);
}

static void
text_input_manager_create_text_input (struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t id)
{
  MetaWaylandTextInput *text_input;

  text_input = g_new0 (MetaWaylandTextInput, 1);
  text_input->resource = wl_resource_create (client,
                                             &wl_text_input_interface,
                                             1,
                                             id);
  wl_resource_set_implementation (text_input->resource,
                                  &text_input_interface,
                                  text_input,
                                  destroy_text_input);
};

static const struct wl_text_input_manager_interface
text_input_manager_interface = {
  text_input_manager_create_text_input
};

static void
handle_keyboard_focus (struct wl_listener *listener, void *data)
{
  MetaWaylandKeyboard *keyboard = data;
  MetaWaylandInputMethod *input_method =
    wl_container_of (listener, input_method, keyboard_focus_listener);
  MetaWaylandSurface *surface = keyboard->focus_surface;

  if (!input_method->text_input)
    return;

  if (!surface || input_method->text_input->surface != surface)
    deactivate_input_method (input_method);
}

static void
input_method_context_destroy (struct wl_client *client,
                              struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
input_method_context_commit_string (struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t serial,
                                    const char *text)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_commit_string (context->text_input->resource,
                                      serial, text);
}

static void
input_method_context_preedit_string (struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t serial,
                                     const char *text,
                                     const char *commit)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_preedit_string (context->text_input->resource,
                                       serial, text, commit);
}

static void
input_method_context_preedit_styling (struct wl_client *client,
                                      struct wl_resource *resource,
                                      uint32_t index,
                                      uint32_t length,
                                      uint32_t style)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_preedit_styling (context->text_input->resource,
                                        index, length, style);
}

static void
input_method_context_preedit_cursor (struct wl_client *client,
                                     struct wl_resource *resource,
                                     int32_t cursor)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_preedit_cursor (context->text_input->resource,
                                       cursor);
}

static void
input_method_context_delete_surrounding_text (struct wl_client *client,
                                              struct wl_resource *resource,
                                              int32_t index,
                                              uint32_t length)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_delete_surrounding_text (context->text_input->resource,
                                                index, length);
}

static void
input_method_context_cursor_position (struct wl_client *client,
                                      struct wl_resource *resource,
                                      int32_t index,
                                      int32_t anchor)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_cursor_position (context->text_input->resource,
                                        index, anchor);
}

static void
input_method_context_modifiers_map (struct wl_client *client,
                                    struct wl_resource *resource,
                                    struct wl_array *map)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_modifiers_map (context->text_input->resource, map);
}

static void
input_method_context_keysym (struct wl_client *client,
                             struct wl_resource *resource,
                             uint32_t serial,
                             uint32_t time,
                             uint32_t sym,
                             uint32_t state,
                             uint32_t modifiers)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_keysym (context->text_input->resource,
                               serial, time, sym, state, modifiers);
}

static void
unbind_keyboard (struct wl_resource *resource)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  input_method_context_end_keyboard_grab (context);
  context->keyboard_resource = NULL;
}

static void
input_method_context_grab_key (MetaWaylandKeyboardGrab *grab,
                               uint32_t time, uint32_t key, uint32_t state)
{
  MetaWaylandKeyboard *keyboard = grab->keyboard;
  MetaWaylandSeat *seat = wl_container_of (keyboard, seat, keyboard);
  MetaWaylandInputMethod *input_method = &seat->input_method;
  uint32_t serial;

  if (!input_method->keyboard_resource)
    return;

  serial = wl_display_next_serial (keyboard->display);
  wl_keyboard_send_key (input_method->keyboard_resource,
                        serial, time, key, state);
}

static void
input_method_context_grab_modifier (MetaWaylandKeyboardGrab *grab,
                                    uint32_t serial,
                                    uint32_t depressed_mods,
                                    uint32_t latched_mods,
                                    uint32_t locked_mods,
                                    uint32_t group)
{
  MetaWaylandKeyboard *keyboard = grab->keyboard;
  MetaWaylandSeat *seat = wl_container_of (keyboard, seat, keyboard);
  MetaWaylandInputMethod *input_method = &seat->input_method;

  if (!input_method->keyboard_resource)
    return;

  wl_keyboard_send_modifiers (input_method->keyboard_resource,
                              serial, depressed_mods, latched_mods,
                              locked_mods, group);
}

static void
input_method_context_grab_cancel (MetaWaylandKeyboardGrab *grab)
{
  meta_wayland_keyboard_end_grab (grab->keyboard);
}

static const MetaWaylandKeyboardGrabInterface
input_method_context_grab_interface = {
  input_method_context_grab_key,
  input_method_context_grab_modifier,
  input_method_context_grab_cancel
};

static void
input_method_context_grab_keyboard (struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t id)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);
  MetaWaylandKeyboard *keyboard = context->input_method->keyboard;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_keyboard_interface, 1, id);
  wl_resource_set_implementation (cr, NULL, context,
                                  unbind_keyboard);
  context->keyboard_resource = cr;

  wl_keyboard_send_keymap (cr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                           keyboard->xkb_info.keymap_fd,
                           keyboard->xkb_info.keymap_size);

  if (keyboard->grab != &keyboard->default_grab)
    meta_wayland_keyboard_end_grab (keyboard);

  meta_wayland_keyboard_start_grab (keyboard, &context->input_method->grab);
  context->input_method->keyboard_resource = cr;
}

static void
input_method_context_key (struct wl_client *client,
                          struct wl_resource *resource,
                          uint32_t serial,
                          uint32_t time,
                          uint32_t key,
                          uint32_t state)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);
  MetaWaylandKeyboard *keyboard = context->input_method->keyboard;
  MetaWaylandKeyboardGrab *default_grab = &keyboard->default_grab;

  default_grab->interface->key (default_grab, time, key, state);
}

static void
input_method_context_modifiers (struct wl_client *client,
                                struct wl_resource *resource,
                                uint32_t serial,
                                uint32_t mods_depressed,
                                uint32_t mods_latched,
                                uint32_t mods_locked,
                                uint32_t group)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);
  MetaWaylandKeyboard *keyboard = context->input_method->keyboard;
  MetaWaylandKeyboardGrab *default_grab = &keyboard->default_grab;

  default_grab->interface->modifiers (default_grab,
                                      serial, mods_depressed,
                                      mods_latched, mods_locked,
                                      group);
}

static void
input_method_context_language (struct wl_client *client,
                               struct wl_resource *resource,
                               uint32_t serial,
                               const char *language)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_language (context->text_input->resource,
                                 serial, language);
}

static void
input_method_context_text_direction (struct wl_client *client,
                                     struct wl_resource *resource,
                                     uint32_t serial,
                                     uint32_t direction)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->text_input)
    wl_text_input_send_text_direction (context->text_input->resource,
                                       serial, direction);
}

static const struct wl_input_method_context_interface
input_method_context_interface = {
  input_method_context_destroy,
  input_method_context_commit_string,
  input_method_context_preedit_string,
  input_method_context_preedit_styling,
  input_method_context_preedit_cursor,
  input_method_context_delete_surrounding_text,
  input_method_context_cursor_position,
  input_method_context_modifiers_map,
  input_method_context_keysym,
  input_method_context_grab_keyboard,
  input_method_context_key,
  input_method_context_modifiers,
  input_method_context_language,
  input_method_context_text_direction
};

static void
destroy_input_method_context (struct wl_resource *resource)
{
  MetaWaylandInputMethodContext *context = wl_resource_get_user_data (resource);

  if (context->keyboard_resource)
    wl_resource_destroy (context->keyboard_resource);

  if (context->input_method && context->input_method->context == context)
    context->input_method->context = NULL;

  g_free (context);
}

static void
input_method_context_create (MetaWaylandTextInput *text_input,
                             MetaWaylandInputMethod *input_method)
{
  MetaWaylandInputMethodContext *context;
  struct wl_client *client;

  if (!input_method->resource)
    return;

  context = g_new0 (MetaWaylandInputMethodContext, 1);

  client = wl_resource_get_client (input_method->resource);
  context->resource = wl_resource_create (client,
                                          &wl_input_method_context_interface,
                                          1, 0);
  wl_resource_set_implementation (context->resource,
                                  &input_method_context_interface,
                                  context, destroy_input_method_context);

  context->text_input = text_input;
  context->input_method = input_method;
  input_method->context = context;

  wl_input_method_send_activate (input_method->resource, context->resource);
}

static void
unbind_input_method (struct wl_resource *resource)
{
  MetaWaylandInputMethod *input_method = wl_resource_get_user_data (resource);

  input_method->resource = NULL;
  input_method->context = NULL;
}

static void
bind_input_method (struct wl_client *client,
                   void *data,
                   uint32_t version,
                   uint32_t id)
{
  MetaWaylandInputMethod *input_method = data;
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_input_method_interface, 1, id);

  if (input_method->resource)
    {
      wl_resource_post_error (resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                              "interface object already bound");
      return;
    }

  /* FIXME: should check client is a privileged client.  In Weston,
     such clients can be spawned with weston_client_launch(). */

  wl_resource_set_implementation (resource, NULL, input_method,
                                  unbind_input_method);
  input_method->resource = resource;
}

static void
bind_text_input_manager (struct wl_client *client,
                         void *data,
                         uint32_t version,
                         uint32_t id)
{
  struct wl_resource *resource;

  /* No checking for duplicate binding necessary.  */
  resource = wl_resource_create (client, &wl_text_input_manager_interface,
                                 1, id);
  if (resource)
    wl_resource_set_implementation (resource,
                                    &text_input_manager_interface,
                                    NULL, NULL);
}

void
meta_wayland_input_method_init (MetaWaylandInputMethod *input_method,
                                MetaWaylandKeyboard *keyboard)
{
  memset (input_method, 0, sizeof (MetaWaylandInputMethod));
  input_method->grab.interface = &input_method_context_grab_interface;
  input_method->keyboard = keyboard;
  input_method->keyboard_focus_listener.notify = handle_keyboard_focus;
  wl_signal_add (&keyboard->focus_signal,
                 &input_method->keyboard_focus_listener);

  if (!wl_global_create (keyboard->display,
                        &wl_input_method_interface, 1,
                        input_method, bind_input_method))
    g_error ("Failed to register a global input-method object");

  if (!wl_global_create (keyboard->display,
                         &wl_text_input_manager_interface, 1,
                         NULL, bind_text_input_manager))
    g_error ("Failed to register a global text-input-manager object");
}
