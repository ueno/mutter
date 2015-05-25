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

#ifndef META_WAYLAND_TEXT_H
#define META_WAYLAND_TEXT_H

#include "meta-wayland-keyboard.h"
#include "meta-wayland-types.h"

struct _MetaWaylandInputMethod
{
  struct wl_resource *resource;

  MetaWaylandTextInput *text_input;

  MetaWaylandKeyboard *keyboard;
  struct wl_listener keyboard_focus_listener;
  MetaWaylandKeyboardGrab grab;
  struct wl_resource *keyboard_resource;

  /* FIXME: We could allow multiple contexts.  */
  MetaWaylandInputMethodContext *context;
};

void meta_wayland_input_method_init (MetaWaylandInputMethod *input_method,
				     MetaWaylandKeyboard *keyboard);

#endif /* META_WAYLAND_TEXT_H */
