/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001 Havoc Pennington, Anders Carlsson
 * Copyright (C) 2002, 2003 Red Hat, Inc.
 * Copyright (C) 2003 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "window-x11.h"
#include "window-x11-private.h"

#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xlibint.h> /* For display->resource_mask */

#ifdef HAVE_SHAPE
#include <X11/extensions/shape.h>
#endif

#include <X11/extensions/Xcomposite.h>
#include "core.h"

#include <meta/common.h>
#include <meta/errors.h>
#include <meta/prefs.h>
#include <meta/meta-cursor-tracker.h>

#include "frame.h"
#include "window-private.h"
#include "window-props.h"
#include "xprops.h"

struct _MetaWindowX11Class
{
  MetaWindowClass parent_class;
};

G_DEFINE_TYPE_WITH_PRIVATE (MetaWindowX11, meta_window_x11, META_TYPE_WINDOW)

static void
meta_window_x11_init (MetaWindowX11 *window_x11)
{
  window_x11->priv = meta_window_x11_get_instance_private (window_x11);
}

static void
send_icccm_message (MetaWindow *window,
                    Atom        atom,
                    guint32     timestamp)
{
  /* This comment and code are from twm, copyright
   * Open Group, Evans & Sutherland, etc.
   */

  /*
   * ICCCM Client Messages - Section 4.2.8 of the ICCCM dictates that all
   * client messages will have the following form:
   *
   *     event type	ClientMessage
   *     message type	_XA_WM_PROTOCOLS
   *     window		tmp->w
   *     format		32
   *     data[0]		message atom
   *     data[1]		time stamp
   */

  XClientMessageEvent ev;

  ev.type = ClientMessage;
  ev.window = window->xwindow;
  ev.message_type = window->display->atom_WM_PROTOCOLS;
  ev.format = 32;
  ev.data.l[0] = atom;
  ev.data.l[1] = timestamp;

  meta_error_trap_push (window->display);
  XSendEvent (window->display->xdisplay,
              window->xwindow, False, 0, (XEvent*) &ev);
  meta_error_trap_pop (window->display);
}

static Window
read_client_leader (MetaDisplay *display,
                    Window       xwindow)
{
  Window retval = None;

  meta_prop_get_window (display, xwindow,
                        display->atom_WM_CLIENT_LEADER,
                        &retval);

  return retval;
}

typedef struct
{
  Window leader;
} ClientLeaderData;

static gboolean
find_client_leader_func (MetaWindow *ancestor,
                         void       *data)
{
  ClientLeaderData *d;

  d = data;

  d->leader = read_client_leader (ancestor->display,
                                  ancestor->xwindow);

  /* keep going if no client leader found */
  return d->leader == None;
}

static void
update_sm_hints (MetaWindow *window)
{
  Window leader;

  window->xclient_leader = None;
  window->sm_client_id = NULL;

  /* If not on the current window, we can get the client
   * leader from transient parents. If we find a client
   * leader, we read the SM_CLIENT_ID from it.
   */
  leader = read_client_leader (window->display, window->xwindow);
  if (leader == None)
    {
      ClientLeaderData d;
      d.leader = None;
      meta_window_foreach_ancestor (window, find_client_leader_func,
                                    &d);
      leader = d.leader;
    }

  if (leader != None)
    {
      char *str;

      window->xclient_leader = leader;

      if (meta_prop_get_latin1_string (window->display, leader,
                                       window->display->atom_SM_CLIENT_ID,
                                       &str))
        {
          window->sm_client_id = g_strdup (str);
          meta_XFree (str);
        }
    }
  else
    {
      meta_verbose ("Didn't find a client leader for %s\n", window->desc);

      if (!meta_prefs_get_disable_workarounds ())
        {
          /* Some broken apps (kdelibs fault?) set SM_CLIENT_ID on the app
           * instead of the client leader
           */
          char *str;

          str = NULL;
          if (meta_prop_get_latin1_string (window->display, window->xwindow,
                                           window->display->atom_SM_CLIENT_ID,
                                           &str))
            {
              if (window->sm_client_id == NULL) /* first time through */
                meta_warning ("Window %s sets SM_CLIENT_ID on itself, instead of on the WM_CLIENT_LEADER window as specified in the ICCCM.\n",
                              window->desc);

              window->sm_client_id = g_strdup (str);
              meta_XFree (str);
            }
        }
    }

  meta_verbose ("Window %s client leader: 0x%lx SM_CLIENT_ID: '%s'\n",
                window->desc, window->xclient_leader,
                window->sm_client_id ? window->sm_client_id : "none");
}

static void
send_configure_notify (MetaWindow *window)
{
  XEvent event;

  /* from twm */

  event.type = ConfigureNotify;
  event.xconfigure.display = window->display->xdisplay;
  event.xconfigure.event = window->xwindow;
  event.xconfigure.window = window->xwindow;
  event.xconfigure.x = window->rect.x - window->border_width;
  event.xconfigure.y = window->rect.y - window->border_width;
  if (window->frame)
    {
      if (window->withdrawn)
        {
          MetaFrameBorders borders;
          /* We reparent the client window and put it to the position
           * where the visible top-left of the frame window currently is.
           */

          meta_frame_calc_borders (window->frame, &borders);

          event.xconfigure.x = window->frame->rect.x + borders.invisible.left;
          event.xconfigure.y = window->frame->rect.y + borders.invisible.top;
        }
      else
        {
          /* Need to be in root window coordinates */
          event.xconfigure.x += window->frame->rect.x;
          event.xconfigure.y += window->frame->rect.y;
        }
    }
  event.xconfigure.width = window->rect.width;
  event.xconfigure.height = window->rect.height;
  event.xconfigure.border_width = window->border_width; /* requested not actual */
  event.xconfigure.above = None; /* FIXME */
  event.xconfigure.override_redirect = False;

  meta_topic (META_DEBUG_GEOMETRY,
              "Sending synthetic configure notify to %s with x: %d y: %d w: %d h: %d\n",
              window->desc,
              event.xconfigure.x, event.xconfigure.y,
              event.xconfigure.width, event.xconfigure.height);

  meta_error_trap_push (window->display);
  XSendEvent (window->display->xdisplay,
              window->xwindow,
              False, StructureNotifyMask, &event);
  meta_error_trap_pop (window->display);
}

static void
meta_window_x11_manage (MetaWindow *window)
{
  MetaDisplay *display = window->display;

  meta_display_register_x_window (display, &window->xwindow, window);
  meta_window_x11_update_shape_region (window);
  meta_window_x11_update_input_region (window);

  /* assign the window to its group, or create a new group if needed */
  window->group = NULL;
  window->xgroup_leader = None;
  meta_window_compute_group (window);

  meta_window_load_initial_properties (window);

  if (!window->override_redirect)
    update_sm_hints (window); /* must come after transient_for */

  meta_window_x11_update_net_wm_type (window);
}

static void
meta_window_x11_unmanage (MetaWindow *window)
{
  meta_error_trap_push (window->display);

  if (window->withdrawn)
    {
      /* We need to clean off the window's state so it
       * won't be restored if the app maps it again.
       */
      meta_verbose ("Cleaning state from window %s\n", window->desc);
      XDeleteProperty (window->display->xdisplay,
                       window->xwindow,
                       window->display->atom__NET_WM_DESKTOP);
      XDeleteProperty (window->display->xdisplay,
                       window->xwindow,
                       window->display->atom__NET_WM_STATE);
      XDeleteProperty (window->display->xdisplay,
                       window->xwindow,
                       window->display->atom__NET_WM_FULLSCREEN_MONITORS);
      meta_window_x11_set_wm_state (window);
    }
  else
    {
      /* We need to put WM_STATE so that others will understand it on
       * restart.
       */
      if (!window->minimized)
        meta_window_x11_set_wm_state (window);

      /* If we're unmanaging a window that is not withdrawn, then
       * either (a) mutter is exiting, in which case we need to map
       * the window so the next WM will know that it's not Withdrawn,
       * or (b) we want to create a new MetaWindow to replace the
       * current one, which will happen automatically if we re-map
       * the X Window.
       */
      XMapWindow (window->display->xdisplay,
                  window->xwindow);
    }

  meta_display_unregister_x_window (window->display, window->xwindow);

  /* Put back anything we messed up */
  if (window->border_width != 0)
    XSetWindowBorderWidth (window->display->xdisplay,
                           window->xwindow,
                           window->border_width);

  /* No save set */
  XRemoveFromSaveSet (window->display->xdisplay,
                      window->xwindow);

  /* Even though the window is now unmanaged, we can't unselect events. This
   * window might be a window from this process, like a GdkMenu, in
   * which case it will have pointer events and so forth selected
   * for it by GDK. There's no way to disentangle those events from the events
   * we've selected. Even for a window from a different X client,
   * GDK could also have selected events for it for IPC purposes, so we
   * can't unselect in that case either.
   *
   * Similarly, we can't unselected for events on window->user_time_window.
   * It might be our own GDK focus window, or it might be a window that a
   * different client is using for multiple different things:
   * _NET_WM_USER_TIME_WINDOW and IPC, perhaps.
   */

  if (window->user_time_window != None)
    {
      meta_display_unregister_x_window (window->display,
                                        window->user_time_window);
      window->user_time_window = None;
    }

#ifdef HAVE_SHAPE
  if (META_DISPLAY_HAS_SHAPE (window->display))
    XShapeSelectInput (window->display->xdisplay, window->xwindow, NoEventMask);
#endif

  /* The XReparentWindow call in meta_window_destroy_frame() moves the
   * window so we need to send a configure notify; see bug 399552.  (We
   * also do this just in case a window got unmaximized.)
   */
  send_configure_notify (window);

  meta_error_trap_pop (window->display);
}

static void
meta_window_x11_ping (MetaWindow *window,
                      guint32     serial)
{
  MetaDisplay *display = window->display;

  send_icccm_message (window, display->atom__NET_WM_PING, serial);
}

static void
meta_window_x11_delete (MetaWindow *window,
                        guint32     timestamp)
{
  meta_error_trap_push (window->display);
  if (window->delete_window)
    {
      meta_topic (META_DEBUG_WINDOW_OPS,
                  "Deleting %s with delete_window request\n",
                  window->desc);
      send_icccm_message (window, window->display->atom_WM_DELETE_WINDOW, timestamp);
    }
  else
    {
      meta_topic (META_DEBUG_WINDOW_OPS,
                  "Deleting %s with explicit kill\n",
                  window->desc);
      XKillClient (window->display->xdisplay, window->xwindow);
    }
  meta_error_trap_pop (window->display);
}

static void
meta_window_x11_kill (MetaWindow *window)
{
  meta_topic (META_DEBUG_WINDOW_OPS,
              "Killing %s brutally\n",
              window->desc);

  if (!meta_window_is_remote (window) &&
      window->net_wm_pid > 0)
    {
      meta_topic (META_DEBUG_WINDOW_OPS,
                  "Killing %s with kill()\n",
                  window->desc);

      if (kill (window->net_wm_pid, 9) < 0)
        meta_topic (META_DEBUG_WINDOW_OPS,
                    "Failed to signal %s: %s\n",
                    window->desc, strerror (errno));
    }

  meta_topic (META_DEBUG_WINDOW_OPS,
              "Disconnecting %s with XKillClient()\n",
              window->desc);

  meta_error_trap_push (window->display);
  XKillClient (window->display->xdisplay, window->xwindow);
  meta_error_trap_pop (window->display);
}

static void
request_take_focus (MetaWindow *window,
                    guint32     timestamp)
{
  MetaDisplay *display = window->display;

  meta_topic (META_DEBUG_FOCUS, "WM_TAKE_FOCUS(%s, %u)\n",
              window->desc, timestamp);

  send_icccm_message (window, display->atom_WM_TAKE_FOCUS, timestamp);
}

static void
meta_window_x11_focus (MetaWindow *window,
                       guint32     timestamp)
{
  /* For output-only or shaded windows, focus the frame.
   * This seems to result in the client window getting key events
   * though, so I don't know if it's icccm-compliant.
   *
   * Still, we have to do this or keynav breaks for these windows.
   */
  if (window->frame &&
      (window->shaded ||
       !(window->input || window->take_focus)))
    {
      if (window->frame)
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Focusing frame of %s\n", window->desc);
          meta_display_set_input_focus_window (window->display,
                                               window,
                                               TRUE,
                                               timestamp);
        }
    }
  else
    {
      if (window->input)
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Setting input focus on %s since input = true\n",
                      window->desc);
          meta_display_set_input_focus_window (window->display,
                                               window,
                                               FALSE,
                                               timestamp);
        }

      if (window->take_focus)
        {
          meta_topic (META_DEBUG_FOCUS,
                      "Sending WM_TAKE_FOCUS to %s since take_focus = true\n",
                      window->desc);

          if (!window->input)
            {
              /* The "Globally Active Input" window case, where the window
               * doesn't want us to call XSetInputFocus on it, but does
               * want us to send a WM_TAKE_FOCUS.
               *
               * Normally, we want to just leave the focus undisturbed until
               * the window respnds to WM_TAKE_FOCUS, but if we're unmanaging
               * the current focus window we *need* to move the focus away, so
               * we focus the no_focus_window now (and set
               * display->focus_window to that) before sending WM_TAKE_FOCUS.
               */
              if (window->display->focus_window != NULL &&
                  window->display->focus_window->unmanaging)
                meta_display_focus_the_no_focus_window (window->display,
                                                        window->screen,
                                                        timestamp);
            }

          request_take_focus (window, timestamp);
        }
    }
}

static void
update_net_frame_extents (MetaWindow *window)
{
  unsigned long data[4];
  MetaFrameBorders borders;

  meta_frame_calc_borders (window->frame, &borders);
  /* Left */
  data[0] = borders.visible.left;
  /* Right */
  data[1] = borders.visible.right;
  /* Top */
  data[2] = borders.visible.top;
  /* Bottom */
  data[3] = borders.visible.bottom;

  meta_topic (META_DEBUG_GEOMETRY,
              "Setting _NET_FRAME_EXTENTS on managed window 0x%lx "
 "to left = %lu, right = %lu, top = %lu, bottom = %lu\n",
              window->xwindow, data[0], data[1], data[2], data[3]);

  meta_error_trap_push (window->display);
  XChangeProperty (window->display->xdisplay, window->xwindow,
                   window->display->atom__NET_FRAME_EXTENTS,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 4);
  meta_error_trap_pop (window->display);
}

#ifdef HAVE_XSYNC
static gboolean
sync_request_timeout (gpointer data)
{
  MetaWindow *window = data;

  window->sync_request_timeout_id = 0;

  /* We have now waited for more than a second for the
   * application to respond to the sync request
   */
  window->disable_sync = TRUE;

  /* Reset the wait serial, so we don't continue freezing
   * window updates
   */
  window->sync_request_wait_serial = 0;
  meta_compositor_set_updates_frozen (window->display->compositor, window,
                                      meta_window_updates_are_frozen (window));

  if (window == window->display->grab_window &&
      meta_grab_op_is_resizing (window->display->grab_op))
    {
      meta_window_update_resize (window,
                                 window->display->grab_last_user_action_was_snap,
                                 window->display->grab_latest_motion_x,
                                 window->display->grab_latest_motion_y,
                                 TRUE);
    }

  return FALSE;
}

static void
send_sync_request (MetaWindow *window)
{
  XClientMessageEvent ev;
  gint64 wait_serial;

  /* For the old style of _NET_WM_SYNC_REQUEST_COUNTER, we just have to
   * increase the value, but for the new "extended" style we need to
   * pick an even (unfrozen) value sufficiently ahead of the last serial
   * that we received from the client; the same code still works
   * for the old style. The increment of 240 is specified by the EWMH
   * and is (1 second) * (60fps) * (an increment of 4 per frame).
   */
  wait_serial = window->sync_request_serial + 240;

  window->sync_request_wait_serial = wait_serial;

  ev.type = ClientMessage;
  ev.window = window->xwindow;
  ev.message_type = window->display->atom_WM_PROTOCOLS;
  ev.format = 32;
  ev.data.l[0] = window->display->atom__NET_WM_SYNC_REQUEST;
  /* FIXME: meta_display_get_current_time() is bad, but since calls
   * come from meta_window_move_resize_internal (which in turn come
   * from all over), I'm not sure what we can do to fix it.  Do we
   * want to use _roundtrip, though?
   */
  ev.data.l[1] = meta_display_get_current_time (window->display);
  ev.data.l[2] = wait_serial & G_GUINT64_CONSTANT(0xffffffff);
  ev.data.l[3] = wait_serial >> 32;
  ev.data.l[4] = window->extended_sync_request_counter ? 1 : 0;

  /* We don't need to trap errors here as we are already
   * inside an error_trap_push()/pop() pair.
   */
  XSendEvent (window->display->xdisplay,
	      window->xwindow, False, 0, (XEvent*) &ev);

  /* We give the window 1 sec to respond to _NET_WM_SYNC_REQUEST;
   * if this time expires, we consider the window unresponsive
   * and resize it unsynchonized.
   */
  window->sync_request_timeout_id = g_timeout_add (1000,
                                                   sync_request_timeout,
                                                   window);

  meta_compositor_set_updates_frozen (window->display->compositor, window,
                                      meta_window_updates_are_frozen (window));
}
#endif

static void
meta_window_x11_move_resize_internal (MetaWindow                *window,
                                      int                        gravity,
                                      MetaRectangle              requested_rect,
                                      MetaRectangle              constrained_rect,
                                      MetaMoveResizeFlags        flags,
                                      MetaMoveResizeResultFlags *result)
{
  int root_x_nw, root_y_nw;
  int w, h;
  int client_move_x, client_move_y;
  int size_dx, size_dy;
  XWindowChanges values;
  unsigned int mask;
  gboolean need_configure_notify;
  MetaFrameBorders borders;
  gboolean need_move_client = FALSE;
  gboolean need_move_frame = FALSE;
  gboolean need_resize_client = FALSE;
  gboolean need_resize_frame = FALSE;
  gboolean frame_shape_changed = FALSE;
  gboolean configure_frame_first;

  gboolean is_configure_request;

  is_configure_request = (flags & META_IS_CONFIGURE_REQUEST) != 0;

  /* meta_window_constrain() might have maximized the window after placement,
   * changing the borders.
   */
  meta_frame_calc_borders (window->frame, &borders);

  root_x_nw = constrained_rect.x;
  root_y_nw = constrained_rect.y;
  w = constrained_rect.width;
  h = constrained_rect.height;

  if (w != window->rect.width ||
      h != window->rect.height)
    need_resize_client = TRUE;

  window->rect.width = w;
  window->rect.height = h;

  if (window->frame)
    {
      int frame_size_dx, frame_size_dy;
      int new_w, new_h;

      new_w = window->rect.width + borders.total.left + borders.total.right;

      if (window->shaded)
        new_h = borders.total.top;
      else
        new_h = window->rect.height + borders.total.top + borders.total.bottom;

      frame_size_dx = new_w - window->frame->rect.width;
      frame_size_dy = new_h - window->frame->rect.height;

      need_resize_frame = (frame_size_dx != 0 || frame_size_dy != 0);

      window->frame->rect.width = new_w;
      window->frame->rect.height = new_h;

      meta_topic (META_DEBUG_GEOMETRY,
                  "Calculated frame size %dx%d\n",
                  window->frame->rect.width,
                  window->frame->rect.height);
    }

  /* For nice effect, when growing the window we want to move/resize
   * the frame first, when shrinking the window we want to move/resize
   * the client first. If we grow one way and shrink the other,
   * see which way we're moving "more"
   *
   * Mail from Owen subject "Suggestion: Gravity and resizing from the left"
   * http://mail.gnome.org/archives/wm-spec-list/1999-November/msg00088.html
   *
   * An annoying fact you need to know in this code is that StaticGravity
   * does nothing if you _only_ resize or _only_ move the frame;
   * it must move _and_ resize, otherwise you get NorthWestGravity
   * behavior. The move and resize must actually occur, it is not
   * enough to set CWX | CWWidth but pass in the current size/pos.
   */

  if (window->frame)
    {
      int new_x, new_y;
      int frame_pos_dx, frame_pos_dy;

      /* Compute new frame coords */
      new_x = root_x_nw - borders.total.left;
      new_y = root_y_nw - borders.total.top;

      frame_pos_dx = new_x - window->frame->rect.x;
      frame_pos_dy = new_y - window->frame->rect.y;

      need_move_frame = (frame_pos_dx != 0 || frame_pos_dy != 0);

      window->frame->rect.x = new_x;
      window->frame->rect.y = new_y;

      /* If frame will both move and resize, then StaticGravity
       * on the child window will kick in and implicitly move
       * the child with respect to the frame. The implicit
       * move will keep the child in the same place with
       * respect to the root window. If frame only moves
       * or only resizes, then the child will just move along
       * with the frame.
       */

      /* window->rect.x, window->rect.y are relative to frame,
       * remember they are the server coords
       */

      new_x = borders.total.left;
      new_y = borders.total.top;
      client_move_x = new_x;
      client_move_y = new_y;

      if (client_move_x != window->rect.x ||
          client_move_y != window->rect.y)
        need_move_client = TRUE;

      /* This is the final target position, but not necessarily what
       * we pass to XConfigureWindow, due to StaticGravity implicit
       * movement.
       */
      window->rect.x = new_x;
      window->rect.y = new_y;
    }
  else
    {
      if (root_x_nw != window->rect.x ||
          root_y_nw != window->rect.y)
        need_move_client = TRUE;

      window->rect.x = root_x_nw;
      window->rect.y = root_y_nw;

      client_move_x = window->rect.x;
      client_move_y = window->rect.y;
    }

  /* If frame extents have changed, fill in other frame fields and
     change frame's extents property. */
  if (window->frame &&
      (window->frame->child_x != borders.total.left ||
       window->frame->child_y != borders.total.top ||
       window->frame->right_width != borders.total.right ||
       window->frame->bottom_height != borders.total.bottom))
    {
      window->frame->child_x = borders.total.left;
      window->frame->child_y = borders.total.top;
      window->frame->right_width = borders.total.right;
      window->frame->bottom_height = borders.total.bottom;

      update_net_frame_extents (window);
    }

  /* See ICCCM 4.1.5 for when to send ConfigureNotify */

  need_configure_notify = FALSE;

  /* If this is a configure request and we change nothing, then we
   * must send configure notify.
   */
  if  (is_configure_request &&
       !(need_move_client || need_move_frame ||
         need_resize_client || need_resize_frame ||
         window->border_width != 0))
    need_configure_notify = TRUE;

  /* We must send configure notify if we move but don't resize, since
   * the client window may not get a real event
   */
  if ((need_move_client || need_move_frame) &&
      !(need_resize_client || need_resize_frame))
    need_configure_notify = TRUE;

  /* MapRequest events with a PPosition or UPosition hint with a frame
   * are moved by mutter without resizing; send a configure notify
   * in such cases.  See #322840.  (Note that window->constructing is
   * only true iff this call is due to a MapRequest, and when
   * PPosition/UPosition hints aren't set, mutter seems to send a
   * ConfigureNotify anyway due to the above code.)
   */
  if (window->constructing && window->frame &&
      ((window->size_hints.flags & PPosition) ||
       (window->size_hints.flags & USPosition)))
    need_configure_notify = TRUE;

  /* The rest of this function syncs our new size/pos with X as
   * efficiently as possible
   */

  /* Normally, we configure the frame first depending on whether
   * we grow the frame more than we shrink. The idea is to avoid
   * messing up the window contents by having a temporary situation
   * where the frame is smaller than the window. However, if we're
   * cooperating with the client to create an atomic frame upate,
   * and the window is redirected, then we should always update
   * the frame first, since updating the frame will force a new
   * backing pixmap to be allocated, and the old backing pixmap
   * will be left undisturbed for us to paint to the screen until
   * the client finishes redrawing.
   */
  if (window->extended_sync_request_counter)
    {
      configure_frame_first = TRUE;
    }
  else
    {
      size_dx = w - window->rect.width;
      size_dy = h - window->rect.height;

      configure_frame_first = size_dx + size_dy >= 0;
    }

  if (configure_frame_first && window->frame)
    frame_shape_changed = meta_frame_sync_to_window (window->frame,
                                                     gravity,
                                                     need_move_frame, need_resize_frame);

  values.border_width = 0;
  values.x = client_move_x;
  values.y = client_move_y;
  values.width = window->rect.width;
  values.height = window->rect.height;

  mask = 0;
  if (is_configure_request && window->border_width != 0)
    mask |= CWBorderWidth; /* must force to 0 */
  if (need_move_client)
    mask |= (CWX | CWY);
  if (need_resize_client)
    mask |= (CWWidth | CWHeight);

  if (mask != 0)
    {
      {
        int newx, newy;
        meta_window_get_position (window, &newx, &newy);
        meta_topic (META_DEBUG_GEOMETRY,
                    "Syncing new client geometry %d,%d %dx%d, border: %s pos: %s size: %s\n",
                    newx, newy,
                    window->rect.width, window->rect.height,
                    mask & CWBorderWidth ? "true" : "false",
                    need_move_client ? "true" : "false",
                    need_resize_client ? "true" : "false");
      }

      meta_error_trap_push (window->display);

#ifdef HAVE_XSYNC
      if (window == window->display->grab_window &&
          meta_grab_op_is_resizing (window->display->grab_op) &&
          !window->disable_sync &&
          window->sync_request_counter != None &&
          window->sync_request_alarm != None &&
          window->sync_request_timeout_id == 0)
        {
          send_sync_request (window);
        }
#endif

      XConfigureWindow (window->display->xdisplay,
                        window->xwindow,
                        mask,
                        &values);

      meta_error_trap_pop (window->display);
    }

  if (!configure_frame_first && window->frame)
    frame_shape_changed = meta_frame_sync_to_window (window->frame,
                                                     gravity,
                                                     need_move_frame, need_resize_frame);

  if (need_configure_notify)
    send_configure_notify (window);

  if (frame_shape_changed)
    *result |= META_MOVE_RESIZE_RESULT_FRAME_SHAPE_CHANGED;
  if (need_move_client || need_move_frame)
    *result |= META_MOVE_RESIZE_RESULT_MOVED;
  if (need_resize_client || need_resize_frame)
    *result |= META_MOVE_RESIZE_RESULT_RESIZED;
}

static void
meta_window_x11_get_default_skip_hints (MetaWindow *window,
                                        gboolean   *skip_taskbar_out,
                                        gboolean   *skip_pager_out)
{
  MetaWindowX11 *window_x11 = META_WINDOW_X11 (window);
  MetaWindowX11Private *priv = meta_window_x11_get_instance_private (window_x11);

  *skip_taskbar_out = priv->wm_state_skip_taskbar;
  *skip_pager_out = priv->wm_state_skip_pager;
}

static void
meta_window_x11_class_init (MetaWindowX11Class *klass)
{
  MetaWindowClass *window_class = META_WINDOW_CLASS (klass);

  window_class->manage = meta_window_x11_manage;
  window_class->unmanage = meta_window_x11_unmanage;
  window_class->ping = meta_window_x11_ping;
  window_class->delete = meta_window_x11_delete;
  window_class->kill = meta_window_x11_kill;
  window_class->focus = meta_window_x11_focus;
  window_class->move_resize_internal = meta_window_x11_move_resize_internal;
  window_class->get_default_skip_hints = meta_window_x11_get_default_skip_hints;
}

void
meta_window_x11_set_net_wm_state (MetaWindow *window)
{
  MetaWindowX11 *window_x11 = META_WINDOW_X11 (window);
  MetaWindowX11Private *priv = meta_window_x11_get_instance_private (window_x11);
  int i;
  unsigned long data[13];

  i = 0;
  if (window->shaded)
    {
      data[i] = window->display->atom__NET_WM_STATE_SHADED;
      ++i;
    }
  if (priv->wm_state_modal)
    {
      data[i] = window->display->atom__NET_WM_STATE_MODAL;
      ++i;
    }
  if (window->skip_pager)
    {
      data[i] = window->display->atom__NET_WM_STATE_SKIP_PAGER;
      ++i;
    }
  if (window->skip_taskbar)
    {
      data[i] = window->display->atom__NET_WM_STATE_SKIP_TASKBAR;
      ++i;
    }
  if (window->maximized_horizontally)
    {
      data[i] = window->display->atom__NET_WM_STATE_MAXIMIZED_HORZ;
      ++i;
    }
  if (window->maximized_vertically)
    {
      data[i] = window->display->atom__NET_WM_STATE_MAXIMIZED_VERT;
      ++i;
    }
  if (window->fullscreen)
    {
      data[i] = window->display->atom__NET_WM_STATE_FULLSCREEN;
      ++i;
    }
  if (!meta_window_showing_on_its_workspace (window) || window->shaded)
    {
      data[i] = window->display->atom__NET_WM_STATE_HIDDEN;
      ++i;
    }
  if (window->wm_state_above)
    {
      data[i] = window->display->atom__NET_WM_STATE_ABOVE;
      ++i;
    }
  if (window->wm_state_below)
    {
      data[i] = window->display->atom__NET_WM_STATE_BELOW;
      ++i;
    }
  if (window->wm_state_demands_attention)
    {
      data[i] = window->display->atom__NET_WM_STATE_DEMANDS_ATTENTION;
      ++i;
    }
  if (window->on_all_workspaces_requested)
    {
      data[i] = window->display->atom__NET_WM_STATE_STICKY;
      ++i;
    }
  if (meta_window_appears_focused (window))
    {
      data[i] = window->display->atom__NET_WM_STATE_FOCUSED;
      ++i;
    }

  meta_verbose ("Setting _NET_WM_STATE with %d atoms\n", i);

  meta_error_trap_push (window->display);
  XChangeProperty (window->display->xdisplay, window->xwindow,
                   window->display->atom__NET_WM_STATE,
                   XA_ATOM,
                   32, PropModeReplace, (guchar*) data, i);
  meta_error_trap_pop (window->display);

  if (window->fullscreen)
    {
      if (window->fullscreen_monitors[0] >= 0)
        {
          data[0] = meta_screen_monitor_index_to_xinerama_index (window->screen,
                                                                 window->fullscreen_monitors[0]);
          data[1] = meta_screen_monitor_index_to_xinerama_index (window->screen,
                                                                 window->fullscreen_monitors[1]);
          data[2] = meta_screen_monitor_index_to_xinerama_index (window->screen,
                                                                 window->fullscreen_monitors[2]);
          data[3] = meta_screen_monitor_index_to_xinerama_index (window->screen,
                                                                 window->fullscreen_monitors[3]);

          meta_verbose ("Setting _NET_WM_FULLSCREEN_MONITORS\n");
          meta_error_trap_push (window->display);
          XChangeProperty (window->display->xdisplay,
                           window->xwindow,
                           window->display->atom__NET_WM_FULLSCREEN_MONITORS,
                           XA_CARDINAL, 32, PropModeReplace,
                           (guchar*) data, 4);
          meta_error_trap_pop (window->display);
        }
      else
        {
          meta_verbose ("Clearing _NET_WM_FULLSCREEN_MONITORS\n");
          meta_error_trap_push (window->display);
          XDeleteProperty (window->display->xdisplay,
                           window->xwindow,
                           window->display->atom__NET_WM_FULLSCREEN_MONITORS);
          meta_error_trap_pop (window->display);
        }
    }
}

void
meta_window_x11_update_net_wm_type (MetaWindow *window)
{
  MetaWindowX11 *window_x11 = META_WINDOW_X11 (window);
  MetaWindowX11Private *priv = meta_window_x11_get_instance_private (window_x11);
  int n_atoms;
  Atom *atoms;
  int i;

  priv->type_atom = None;
  n_atoms = 0;
  atoms = NULL;

  meta_prop_get_atom_list (window->display, window->xwindow,
                           window->display->atom__NET_WM_WINDOW_TYPE,
                           &atoms, &n_atoms);

  i = 0;
  while (i < n_atoms)
    {
      /* We break as soon as we find one we recognize,
       * supposed to prefer those near the front of the list
       */
      if (atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_DESKTOP ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_DOCK ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_TOOLBAR ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_MENU ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_UTILITY ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_SPLASH ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_DIALOG ||
          atoms[i] ==
	    window->display->atom__NET_WM_WINDOW_TYPE_DROPDOWN_MENU ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_POPUP_MENU ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_TOOLTIP ||
          atoms[i] ==
	    window->display->atom__NET_WM_WINDOW_TYPE_NOTIFICATION ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_COMBO ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_DND ||
          atoms[i] == window->display->atom__NET_WM_WINDOW_TYPE_NORMAL)
        {
          priv->type_atom = atoms[i];
          break;
        }

      ++i;
    }

  meta_XFree (atoms);

  if (meta_is_verbose ())
    {
      char *str;

      str = NULL;
      if (priv->type_atom != None)
        {
          meta_error_trap_push (window->display);
          str = XGetAtomName (window->display->xdisplay, priv->type_atom);
          meta_error_trap_pop (window->display);
        }

      meta_verbose ("Window %s type atom %s\n", window->desc,
                    str ? str : "(none)");

      if (str)
        meta_XFree (str);
    }

  meta_window_x11_recalc_window_type (window);
}

void
meta_window_x11_update_role (MetaWindow *window)
{
  char *str;

  g_return_if_fail (!window->override_redirect);

  if (window->role)
    g_free (window->role);
  window->role = NULL;

  if (meta_prop_get_latin1_string (window->display, window->xwindow,
                                   window->display->atom_WM_WINDOW_ROLE,
                                   &str))
    {
      window->role = g_strdup (str);
      meta_XFree (str);
    }

  meta_verbose ("Updated role of %s to '%s'\n",
                window->desc, window->role ? window->role : "null");
}

static void
meta_window_set_opaque_region (MetaWindow     *window,
                               cairo_region_t *region)
{
  g_clear_pointer (&window->opaque_region, cairo_region_destroy);

  if (region != NULL)
    window->opaque_region = cairo_region_reference (region);

  meta_compositor_window_shape_changed (window->display->compositor, window);
}

void
meta_window_x11_update_opaque_region (MetaWindow *window)
{
  cairo_region_t *opaque_region = NULL;
  gulong *region = NULL;
  int nitems;

  if (meta_prop_get_cardinal_list (window->display,
                                   window->xwindow,
                                   window->display->atom__NET_WM_OPAQUE_REGION,
                                   &region, &nitems))
    {
      cairo_rectangle_int_t *rects;
      int i, rect_index, nrects;

      if (nitems % 4 != 0)
        {
          meta_verbose ("_NET_WM_OPAQUE_REGION does not have a list of 4-tuples.");
          goto out;
        }

      /* empty region */
      if (nitems == 0)
        goto out;

      nrects = nitems / 4;

      rects = g_new (cairo_rectangle_int_t, nrects);

      rect_index = 0;
      i = 0;
      while (i < nitems)
        {
          cairo_rectangle_int_t *rect = &rects[rect_index];

          rect->x = region[i++];
          rect->y = region[i++];
          rect->width = region[i++];
          rect->height = region[i++];

          rect_index++;
        }

      opaque_region = cairo_region_create_rectangles (rects, nrects);

      g_free (rects);
    }

 out:
  meta_XFree (region);

  meta_window_set_opaque_region (window, opaque_region);
  cairo_region_destroy (opaque_region);
}

static cairo_region_t *
region_create_from_x_rectangles (const XRectangle *rects,
                                 int n_rects)
{
  int i;
  cairo_rectangle_int_t *cairo_rects = g_newa (cairo_rectangle_int_t, n_rects);

  for (i = 0; i < n_rects; i ++)
    {
      cairo_rects[i].x = rects[i].x;
      cairo_rects[i].y = rects[i].y;
      cairo_rects[i].width = rects[i].width;
      cairo_rects[i].height = rects[i].height;
    }

  return cairo_region_create_rectangles (cairo_rects, n_rects);
}

static void
meta_window_set_input_region (MetaWindow     *window,
                              cairo_region_t *region)
{
  g_clear_pointer (&window->input_region, cairo_region_destroy);

  if (region != NULL)
    window->input_region = cairo_region_reference (region);

  meta_compositor_window_shape_changed (window->display->compositor, window);
}

#if 0
/* Print out a region; useful for debugging */
static void
print_region (cairo_region_t *region)
{
  int n_rects;
  int i;

  n_rects = cairo_region_num_rectangles (region);
  g_print ("[");
  for (i = 0; i < n_rects; i++)
    {
      cairo_rectangle_int_t rect;
      cairo_region_get_rectangle (region, i, &rect);
      g_print ("+%d+%dx%dx%d ",
               rect.x, rect.y, rect.width, rect.height);
    }
  g_print ("]\n");
}
#endif

void
meta_window_x11_update_input_region (MetaWindow *window)
{
  cairo_region_t *region = NULL;

  /* Decorated windows don't have an input region, because
     we don't shape the frame to match the client windows
     (so the events are blocked by the frame anyway)
  */
  if (window->decorated)
    {
      if (window->input_region)
        meta_window_set_input_region (window, NULL);
      return;
    }

#ifdef HAVE_SHAPE
  if (META_DISPLAY_HAS_SHAPE (window->display))
    {
      /* Translate the set of XShape rectangles that we
       * get from the X server to a cairo_region. */
      XRectangle *rects = NULL;
      int n_rects, ordering;

      meta_error_trap_push (window->display);
      rects = XShapeGetRectangles (window->display->xdisplay,
                                   window->xwindow,
                                   ShapeInput,
                                   &n_rects,
                                   &ordering);
      meta_error_trap_pop (window->display);

      /* XXX: The x shape extension doesn't provide a way to only test if an
       * input shape has been specified, so we have to query and throw away the
       * rectangles. */
      if (rects)
        {
          if (n_rects > 1 ||
              (n_rects == 1 &&
               (rects[0].x != 0 ||
                rects[0].y != 0 ||
                rects[0].width != window->rect.width ||
                rects[0].height != window->rect.height)))
            region = region_create_from_x_rectangles (rects, n_rects);

          XFree (rects);
        }
    }
#endif /* HAVE_SHAPE */

  if (region != NULL)
    {
      cairo_rectangle_int_t client_area;

      client_area.x = 0;
      client_area.y = 0;
      client_area.width = window->rect.width;
      client_area.height = window->rect.height;

      /* The shape we get back from the client may have coordinates
       * outside of the frame. The X SHAPE Extension requires that
       * the overall shape the client provides never exceeds the
       * "bounding rectangle" of the window -- the shape that the
       * window would have gotten if it was unshaped. In our case,
       * this is simply the client area.
       */
      cairo_region_intersect_rectangle (region, &client_area);
    }

  meta_window_set_input_region (window, region);
  cairo_region_destroy (region);
}

static void
meta_window_set_shape_region (MetaWindow     *window,
                              cairo_region_t *region)
{
  g_clear_pointer (&window->shape_region, cairo_region_destroy);

  if (region != NULL)
    window->shape_region = cairo_region_reference (region);

  meta_compositor_window_shape_changed (window->display->compositor, window);
}

void
meta_window_x11_update_shape_region (MetaWindow *window)
{
  cairo_region_t *region = NULL;

#ifdef HAVE_SHAPE
  if (META_DISPLAY_HAS_SHAPE (window->display))
    {
      /* Translate the set of XShape rectangles that we
       * get from the X server to a cairo_region. */
      XRectangle *rects = NULL;
      int n_rects, ordering;

      int x_bounding, y_bounding, x_clip, y_clip;
      unsigned w_bounding, h_bounding, w_clip, h_clip;
      int bounding_shaped, clip_shaped;

      meta_error_trap_push (window->display);
      XShapeQueryExtents (window->display->xdisplay, window->xwindow,
                          &bounding_shaped, &x_bounding, &y_bounding,
                          &w_bounding, &h_bounding,
                          &clip_shaped, &x_clip, &y_clip,
                          &w_clip, &h_clip);

      if (bounding_shaped)
        {
          rects = XShapeGetRectangles (window->display->xdisplay,
                                       window->xwindow,
                                       ShapeBounding,
                                       &n_rects,
                                       &ordering);
        }
      meta_error_trap_pop (window->display);

      if (rects)
        {
          region = region_create_from_x_rectangles (rects, n_rects);
          XFree (rects);
        }
    }
#endif /* HAVE_SHAPE */

  if (region != NULL)
    {
      cairo_rectangle_int_t client_area;

      client_area.x = 0;
      client_area.y = 0;
      client_area.width = window->rect.width;
      client_area.height = window->rect.height;

      /* The shape we get back from the client may have coordinates
       * outside of the frame. The X SHAPE Extension requires that
       * the overall shape the client provides never exceeds the
       * "bounding rectangle" of the window -- the shape that the
       * window would have gotten if it was unshaped. In our case,
       * this is simply the client area.
       */
      cairo_region_intersect_rectangle (region, &client_area);
    }

  meta_window_set_shape_region (window, region);
  cairo_region_destroy (region);
}

/* Generally meta_window_same_application() is a better idea
 * of "sameness", since it handles the case where multiple apps
 * want to look like the same app or the same app wants to look
 * like multiple apps, but in the case of workarounds for legacy
 * applications (which likely aren't setting the group properly
 * anyways), it may be desirable to check this as well.
 */
static gboolean
meta_window_same_client (MetaWindow *window,
                         MetaWindow *other_window)
{
  int resource_mask = window->display->xdisplay->resource_mask;

  return ((window->xwindow & ~resource_mask) ==
          (other_window->xwindow & ~resource_mask));
}

gboolean
meta_window_x11_configure_request (MetaWindow *window,
                                   XEvent     *event)
{
  /* Note that x, y is the corner of the window border,
   * and width, height is the size of the window inside
   * its border, but that we always deny border requests
   * and give windows a border of 0. But we save the
   * requested border here.
   */
  if (event->xconfigurerequest.value_mask & CWBorderWidth)
    window->border_width = event->xconfigurerequest.border_width;

  meta_window_move_resize_request(window,
                                  event->xconfigurerequest.value_mask,
                                  window->size_hints.win_gravity,
                                  event->xconfigurerequest.x,
                                  event->xconfigurerequest.y,
                                  event->xconfigurerequest.width,
                                  event->xconfigurerequest.height);

  /* Handle stacking. We only handle raises/lowers, mostly because
   * stack.c really can't deal with anything else.  I guess we'll fix
   * that if a client turns up that really requires it. Only a very
   * few clients even require the raise/lower (and in fact all client
   * attempts to deal with stacking order are essentially broken,
   * since they have no idea what other clients are involved or how
   * the stack looks).
   *
   * I'm pretty sure no interesting client uses TopIf, BottomIf, or
   * Opposite anyway, so the only possible missing thing is
   * Above/Below with a sibling set. For now we just pretend there's
   * never a sibling set and always do the full raise/lower instead of
   * the raise-just-above/below-sibling.
   */
  if (event->xconfigurerequest.value_mask & CWStackMode)
    {
      MetaWindow *active_window;
      active_window = window->display->focus_window;
      if (meta_prefs_get_disable_workarounds ())
        {
          meta_topic (META_DEBUG_STACK,
                      "%s sent an xconfigure stacking request; this is "
                      "broken behavior and the request is being ignored.\n",
                      window->desc);
        }
      else if (active_window &&
               !meta_window_same_application (window, active_window) &&
               !meta_window_same_client (window, active_window) &&
               XSERVER_TIME_IS_BEFORE (window->net_wm_user_time,
                                       active_window->net_wm_user_time))
        {
          meta_topic (META_DEBUG_STACK,
                      "Ignoring xconfigure stacking request from %s (with "
                      "user_time %u); currently active application is %s (with "
                      "user_time %u).\n",
                      window->desc,
                      window->net_wm_user_time,
                      active_window->desc,
                      active_window->net_wm_user_time);
          if (event->xconfigurerequest.detail == Above)
            meta_window_set_demands_attention(window);
        }
      else
        {
          switch (event->xconfigurerequest.detail)
            {
            case Above:
              meta_window_raise (window);
              break;
            case Below:
              meta_window_lower (window);
              break;
            case TopIf:
            case BottomIf:
            case Opposite:
              break;
            }
        }
    }

  return TRUE;
}

static gboolean
process_property_notify (MetaWindow     *window,
                         XPropertyEvent *event)
{
  Window xid = window->xwindow;

  if (meta_is_verbose ()) /* avoid looking up the name if we don't have to */
    {
      char *property_name = XGetAtomName (window->display->xdisplay,
                                          event->atom);

      meta_verbose ("Property notify on %s for %s\n",
                    window->desc, property_name);
      XFree (property_name);
    }

  if (event->atom == window->display->atom__NET_WM_USER_TIME &&
      window->user_time_window)
    {
        xid = window->user_time_window;
    }

  meta_window_reload_property_from_xwindow (window, xid, event->atom, FALSE);

  return TRUE;
}

gboolean
meta_window_x11_property_notify (MetaWindow *window,
                                 XEvent     *event)
{
  return process_property_notify (window, &event->xproperty);
}

#define _NET_WM_MOVERESIZE_SIZE_TOPLEFT      0
#define _NET_WM_MOVERESIZE_SIZE_TOP          1
#define _NET_WM_MOVERESIZE_SIZE_TOPRIGHT     2
#define _NET_WM_MOVERESIZE_SIZE_RIGHT        3
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT  4
#define _NET_WM_MOVERESIZE_SIZE_BOTTOM       5
#define _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT   6
#define _NET_WM_MOVERESIZE_SIZE_LEFT         7
#define _NET_WM_MOVERESIZE_MOVE              8
#define _NET_WM_MOVERESIZE_SIZE_KEYBOARD     9
#define _NET_WM_MOVERESIZE_MOVE_KEYBOARD    10
#define _NET_WM_MOVERESIZE_CANCEL           11

static int
query_pressed_buttons (MetaWindow *window)
{
  ClutterModifierType mods;
  int button = 0;

  meta_cursor_tracker_get_pointer (window->screen->cursor_tracker,
                                   NULL, NULL, &mods);

  if (mods & CLUTTER_BUTTON1_MASK)
    button |= 1 << 1;
  if (mods & CLUTTER_BUTTON2_MASK)
    button |= 1 << 2;
  if (mods & CLUTTER_BUTTON3_MASK)
    button |= 1 << 3;

  return button;
}

gboolean
meta_window_x11_client_message (MetaWindow *window,
                                XEvent     *event)
{
  MetaWindowX11 *window_x11 = META_WINDOW_X11 (window);
  MetaWindowX11Private *priv = meta_window_x11_get_instance_private (window_x11);
  MetaDisplay *display;

  display = window->display;

  if (window->override_redirect)
    {
      /* Don't warn here: we could warn on any of the messages below,
       * but we might also receive other client messages that are
       * part of protocols we don't know anything about. So, silently
       * ignoring is simplest.
       */
      return FALSE;
    }

  if (event->xclient.message_type ==
      display->atom__NET_CLOSE_WINDOW)
    {
      guint32 timestamp;

      if (event->xclient.data.l[0] != 0)
	timestamp = event->xclient.data.l[0];
      else
        {
          meta_warning ("Receiving a NET_CLOSE_WINDOW message for %s without "
                        "a timestamp!  This means some buggy (outdated) "
                        "application is on the loose!\n",
                        window->desc);
          timestamp = meta_display_get_current_time (window->display);
        }

      meta_window_delete (window, timestamp);

      return TRUE;
    }
  else if (event->xclient.message_type ==
           display->atom__NET_WM_DESKTOP)
    {
      int space;
      MetaWorkspace *workspace;

      space = event->xclient.data.l[0];

      meta_verbose ("Request to move %s to workspace %d\n",
                    window->desc, space);

      workspace =
        meta_screen_get_workspace_by_index (window->screen,
                                            space);

      if (workspace)
        {
          if (window->on_all_workspaces_requested)
            meta_window_unstick (window);
          meta_window_change_workspace (window, workspace);
        }
      else if (space == (int) 0xFFFFFFFF)
        {
          meta_window_stick (window);
        }
      else
        {
          meta_verbose ("No such workspace %d for screen\n", space);
        }

      meta_verbose ("Window %s now on_all_workspaces = %d\n",
                    window->desc, window->on_all_workspaces);

      return TRUE;
    }
  else if (event->xclient.message_type ==
           display->atom__NET_WM_STATE)
    {
      gulong action;
      Atom first;
      Atom second;

      action = event->xclient.data.l[0];
      first = event->xclient.data.l[1];
      second = event->xclient.data.l[2];

      if (meta_is_verbose ())
        {
          char *str1;
          char *str2;

          meta_error_trap_push_with_return (display);
          str1 = XGetAtomName (display->xdisplay, first);
          if (meta_error_trap_pop_with_return (display) != Success)
            str1 = NULL;

          meta_error_trap_push_with_return (display);
          str2 = XGetAtomName (display->xdisplay, second);
          if (meta_error_trap_pop_with_return (display) != Success)
            str2 = NULL;

          meta_verbose ("Request to change _NET_WM_STATE action %lu atom1: %s atom2: %s\n",
                        action,
                        str1 ? str1 : "(unknown)",
                        str2 ? str2 : "(unknown)");

          meta_XFree (str1);
          meta_XFree (str2);
        }

      if (first == display->atom__NET_WM_STATE_SHADED ||
          second == display->atom__NET_WM_STATE_SHADED)
        {
          gboolean shade;
          guint32 timestamp;

          /* Stupid protocol has no timestamp; of course, shading
           * sucks anyway so who really cares that we're forced to do
           * a roundtrip here?
           */
          timestamp = meta_display_get_current_time_roundtrip (window->display);

          shade = (action == _NET_WM_STATE_ADD ||
                   (action == _NET_WM_STATE_TOGGLE && !window->shaded));
          if (shade && window->has_shade_func)
            meta_window_shade (window, timestamp);
          else
            meta_window_unshade (window, timestamp);
        }

      if (first == display->atom__NET_WM_STATE_FULLSCREEN ||
          second == display->atom__NET_WM_STATE_FULLSCREEN)
        {
          gboolean make_fullscreen;

          make_fullscreen = (action == _NET_WM_STATE_ADD ||
                             (action == _NET_WM_STATE_TOGGLE && !window->fullscreen));
          if (make_fullscreen && window->has_fullscreen_func)
            meta_window_make_fullscreen (window);
          else
            meta_window_unmake_fullscreen (window);
        }

      if (first == display->atom__NET_WM_STATE_MAXIMIZED_HORZ ||
          second == display->atom__NET_WM_STATE_MAXIMIZED_HORZ ||
          first == display->atom__NET_WM_STATE_MAXIMIZED_VERT ||
          second == display->atom__NET_WM_STATE_MAXIMIZED_VERT)
        {
          gboolean max;
          MetaMaximizeFlags directions = 0;

          max = (action == _NET_WM_STATE_ADD ||
                 (action == _NET_WM_STATE_TOGGLE &&
                  !window->maximized_horizontally));

          if (first == display->atom__NET_WM_STATE_MAXIMIZED_HORZ ||
              second == display->atom__NET_WM_STATE_MAXIMIZED_HORZ)
            directions |= META_MAXIMIZE_HORIZONTAL;

          if (first == display->atom__NET_WM_STATE_MAXIMIZED_VERT ||
              second == display->atom__NET_WM_STATE_MAXIMIZED_VERT)
            directions |= META_MAXIMIZE_VERTICAL;

          if (max && window->has_maximize_func)
            {
              if (meta_prefs_get_raise_on_click ())
                meta_window_raise (window);
              meta_window_maximize (window, directions);
            }
          else
            {
              if (meta_prefs_get_raise_on_click ())
                meta_window_raise (window);
              meta_window_unmaximize (window, directions);
            }
        }

      if (first == display->atom__NET_WM_STATE_MODAL ||
          second == display->atom__NET_WM_STATE_MODAL)
        {
          priv->wm_state_modal =
            (action == _NET_WM_STATE_ADD) ||
            (action == _NET_WM_STATE_TOGGLE && !priv->wm_state_modal);

          meta_window_x11_recalc_window_type (window);
          meta_window_queue(window, META_QUEUE_MOVE_RESIZE);
        }

      if (first == display->atom__NET_WM_STATE_SKIP_PAGER ||
          second == display->atom__NET_WM_STATE_SKIP_PAGER)
        {
          priv->wm_state_skip_pager =
            (action == _NET_WM_STATE_ADD) ||
            (action == _NET_WM_STATE_TOGGLE && !window->skip_pager);

          meta_window_recalc_features (window);
          meta_window_x11_set_net_wm_state (window);
        }

      if (first == display->atom__NET_WM_STATE_SKIP_TASKBAR ||
          second == display->atom__NET_WM_STATE_SKIP_TASKBAR)
        {
          priv->wm_state_skip_taskbar =
            (action == _NET_WM_STATE_ADD) ||
            (action == _NET_WM_STATE_TOGGLE && !window->skip_taskbar);

          meta_window_recalc_features (window);
          meta_window_x11_set_net_wm_state (window);
        }

      if (first == display->atom__NET_WM_STATE_ABOVE ||
          second == display->atom__NET_WM_STATE_ABOVE)
        {
          if ((action == _NET_WM_STATE_ADD) ||
              (action == _NET_WM_STATE_TOGGLE && !window->wm_state_demands_attention))
            meta_window_make_above (window);
          else
            meta_window_unmake_above (window);
        }

      if (first == display->atom__NET_WM_STATE_BELOW ||
          second == display->atom__NET_WM_STATE_BELOW)
        {
          window->wm_state_below =
            (action == _NET_WM_STATE_ADD) ||
            (action == _NET_WM_STATE_TOGGLE && !window->wm_state_below);

          meta_window_update_layer (window);
          meta_window_x11_set_net_wm_state (window);
        }

      if (first == display->atom__NET_WM_STATE_DEMANDS_ATTENTION ||
          second == display->atom__NET_WM_STATE_DEMANDS_ATTENTION)
        {
          if ((action == _NET_WM_STATE_ADD) ||
              (action == _NET_WM_STATE_TOGGLE && !window->wm_state_demands_attention))
            meta_window_set_demands_attention (window);
          else
            meta_window_unset_demands_attention (window);
        }

       if (first == display->atom__NET_WM_STATE_STICKY ||
          second == display->atom__NET_WM_STATE_STICKY)
        {
          if ((action == _NET_WM_STATE_ADD) ||
              (action == _NET_WM_STATE_TOGGLE && !window->on_all_workspaces_requested))
            meta_window_stick (window);
          else
            meta_window_unstick (window);
        }

      return TRUE;
    }
  else if (event->xclient.message_type ==
           display->atom_WM_CHANGE_STATE)
    {
      meta_verbose ("WM_CHANGE_STATE client message, state: %ld\n",
                    event->xclient.data.l[0]);
      if (event->xclient.data.l[0] == IconicState &&
          window->has_minimize_func)
        meta_window_minimize (window);

      return TRUE;
    }
  else if (event->xclient.message_type ==
           display->atom__NET_WM_MOVERESIZE)
    {
      int x_root;
      int y_root;
      int action;
      MetaGrabOp op;
      int button;
      guint32 timestamp;

      /* _NET_WM_MOVERESIZE messages are almost certainly going to come from
       * clients when users click on the fake "frame" that the client has,
       * thus we should also treat such messages as though it were a
       * "frame action".
       */
      gboolean const frame_action = TRUE;

      x_root = event->xclient.data.l[0];
      y_root = event->xclient.data.l[1];
      action = event->xclient.data.l[2];
      button = event->xclient.data.l[3];

      /* FIXME: What a braindead protocol; no timestamp?!? */
      timestamp = meta_display_get_current_time_roundtrip (display);
      meta_topic (META_DEBUG_WINDOW_OPS,
                  "Received _NET_WM_MOVERESIZE message on %s, %d,%d action = %d, button %d\n",
                  window->desc,
                  x_root, y_root, action, button);

      op = META_GRAB_OP_NONE;
      switch (action)
        {
        case _NET_WM_MOVERESIZE_SIZE_TOPLEFT:
          op = META_GRAB_OP_RESIZING_NW;
          break;
        case _NET_WM_MOVERESIZE_SIZE_TOP:
          op = META_GRAB_OP_RESIZING_N;
          break;
        case _NET_WM_MOVERESIZE_SIZE_TOPRIGHT:
          op = META_GRAB_OP_RESIZING_NE;
          break;
        case _NET_WM_MOVERESIZE_SIZE_RIGHT:
          op = META_GRAB_OP_RESIZING_E;
          break;
        case _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT:
          op = META_GRAB_OP_RESIZING_SE;
          break;
        case _NET_WM_MOVERESIZE_SIZE_BOTTOM:
          op = META_GRAB_OP_RESIZING_S;
          break;
        case _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT:
          op = META_GRAB_OP_RESIZING_SW;
          break;
        case _NET_WM_MOVERESIZE_SIZE_LEFT:
          op = META_GRAB_OP_RESIZING_W;
          break;
        case _NET_WM_MOVERESIZE_MOVE:
          op = META_GRAB_OP_MOVING;
          break;
        case _NET_WM_MOVERESIZE_SIZE_KEYBOARD:
          op = META_GRAB_OP_KEYBOARD_RESIZING_UNKNOWN;
          break;
        case _NET_WM_MOVERESIZE_MOVE_KEYBOARD:
          op = META_GRAB_OP_KEYBOARD_MOVING;
          break;
        case _NET_WM_MOVERESIZE_CANCEL:
          /* handled below */
          break;
        default:
          break;
        }

      if (action == _NET_WM_MOVERESIZE_CANCEL)
        {
          meta_display_end_grab_op (window->display, timestamp);
        }
      else if (op != META_GRAB_OP_NONE &&
          ((window->has_move_func && op == META_GRAB_OP_KEYBOARD_MOVING) ||
           (window->has_resize_func && op == META_GRAB_OP_KEYBOARD_RESIZING_UNKNOWN)))
        {
          meta_window_begin_grab_op (window, op, frame_action, timestamp);
        }
      else if (op != META_GRAB_OP_NONE &&
               ((window->has_move_func && op == META_GRAB_OP_MOVING) ||
               (window->has_resize_func &&
                (op != META_GRAB_OP_MOVING &&
                 op != META_GRAB_OP_KEYBOARD_MOVING))))
        {
          int button_mask;

          meta_topic (META_DEBUG_WINDOW_OPS,
                      "Beginning move/resize with button = %d\n", button);
          meta_display_begin_grab_op (window->display,
                                      window->screen,
                                      window,
                                      op,
                                      FALSE,
                                      frame_action,
                                      button, 0,
                                      timestamp,
                                      x_root,
                                      y_root);

          button_mask = query_pressed_buttons (window);

          if (button == 0)
            {
              /*
               * the button SHOULD already be included in the message
               */
              if ((button_mask & (1 << 1)) != 0)
                button = 1;
              else if ((button_mask & (1 << 2)) != 0)
                button = 2;
              else if ((button_mask & (1 << 3)) != 0)
                button = 3;

              if (button != 0)
                window->display->grab_button = button;
              else
                meta_display_end_grab_op (window->display,
                                          timestamp);
            }
          else
            {
              /* There is a potential race here. If the user presses and
               * releases their mouse button very fast, it's possible for
               * both the ButtonPress and ButtonRelease to be sent to the
               * client before it can get a chance to send _NET_WM_MOVERESIZE
               * to us. When that happens, we'll become stuck in a grab
               * state, as we haven't received a ButtonRelease to cancel the
               * grab.
               *
               * We can solve this by querying after we take the explicit
               * pointer grab -- if the button isn't pressed, we cancel the
               * drag immediately.
               */

              if ((button_mask & (1 << button)) == 0)
                meta_display_end_grab_op (window->display, timestamp);
            }
        }

      return TRUE;
    }
  else if (event->xclient.message_type ==
           display->atom__NET_MOVERESIZE_WINDOW)
    {
      int gravity;
      guint value_mask;

      gravity = (event->xclient.data.l[0] & 0xff);
      value_mask = (event->xclient.data.l[0] & 0xf00) >> 8;
      /* source = (event->xclient.data.l[0] & 0xf000) >> 12; */

      if (gravity == 0)
        gravity = window->size_hints.win_gravity;

      meta_window_move_resize_request(window,
                                      value_mask,
                                      gravity,
                                      event->xclient.data.l[1],  /* x */
                                      event->xclient.data.l[2],  /* y */
                                      event->xclient.data.l[3],  /* width */
                                      event->xclient.data.l[4]); /* height */
    }
  else if (event->xclient.message_type ==
           display->atom__NET_ACTIVE_WINDOW)
    {
      MetaClientType source_indication;
      guint32        timestamp;

      meta_verbose ("_NET_ACTIVE_WINDOW request for window '%s', activating\n",
                    window->desc);

      source_indication = event->xclient.data.l[0];
      timestamp = event->xclient.data.l[1];

      if (source_indication > META_CLIENT_TYPE_MAX_RECOGNIZED)
        source_indication = META_CLIENT_TYPE_UNKNOWN;

      if (timestamp == 0)
        {
          /* Client using older EWMH _NET_ACTIVE_WINDOW without a timestamp */
          meta_warning ("Buggy client sent a _NET_ACTIVE_WINDOW message with a "
                        "timestamp of 0 for %s\n",
                        window->desc);
          timestamp = meta_display_get_current_time (display);
        }

      meta_window_activate_full (window, timestamp, source_indication, NULL);
      return TRUE;
    }
  else if (event->xclient.message_type ==
           display->atom__NET_WM_FULLSCREEN_MONITORS)
    {
      gulong top, bottom, left, right;

      meta_verbose ("_NET_WM_FULLSCREEN_MONITORS request for window '%s'\n",
                    window->desc);

      top = meta_screen_xinerama_index_to_monitor_index (window->screen,
                                                         event->xclient.data.l[0]);
      bottom = meta_screen_xinerama_index_to_monitor_index (window->screen,
                                                            event->xclient.data.l[1]);
      left = meta_screen_xinerama_index_to_monitor_index (window->screen,
                                                          event->xclient.data.l[2]);
      right = meta_screen_xinerama_index_to_monitor_index (window->screen,
                                                           event->xclient.data.l[3]);
      /* source_indication = event->xclient.data.l[4]; */

      meta_window_update_fullscreen_monitors (window, top, bottom, left, right);
    }

  return FALSE;
}

static void
set_wm_state_on_xwindow (MetaDisplay *display,
                         Window       xwindow,
                         int          state)
{
  unsigned long data[2];

  /* Mutter doesn't use icon windows, so data[1] should be None
   * according to the ICCCM 2.0 Section 4.1.3.1.
   */
  data[0] = state;
  data[1] = None;

  meta_error_trap_push (display);
  XChangeProperty (display->xdisplay, xwindow,
                   display->atom_WM_STATE,
                   display->atom_WM_STATE,
                   32, PropModeReplace, (guchar*) data, 2);
  meta_error_trap_pop (display);
}

void
meta_window_x11_set_wm_state (MetaWindow *window)
{
  int state;

  if (window->withdrawn)
    state = WithdrawnState;
  else if (window->iconic)
    state = IconicState;
  else
    state = NormalState;

  set_wm_state_on_xwindow (window->display, window->xwindow, state);
}

/* The MUTTER_WM_CLASS_FILTER environment variable is designed for
 * performance and regression testing environments where we want to do
 * tests with only a limited set of windows and ignore all other windows
 *
 * When it is set to a comma separated list of WM_CLASS class names, all
 * windows not matching the list will be ignored.
 *
 * Returns TRUE if window has been filtered out and should be ignored.
 */
static gboolean
maybe_filter_xwindow (MetaDisplay       *display,
                      Window             xwindow,
                      gboolean           must_be_viewable,
                      XWindowAttributes *attrs)
{
  static char **filter_wm_classes = NULL;
  static gboolean initialized = FALSE;
  XClassHint class_hint;
  gboolean filtered;
  Status success;
  int i;

  if (!initialized)
    {
      const char *filter_string = g_getenv ("MUTTER_WM_CLASS_FILTER");
      if (filter_string)
        filter_wm_classes = g_strsplit (filter_string, ",", -1);
      initialized = TRUE;
    }

  if (!filter_wm_classes || !filter_wm_classes[0])
    return FALSE;

  filtered = TRUE;

  meta_error_trap_push (display);
  success = XGetClassHint (display->xdisplay, xwindow, &class_hint);

  if (success)
    {
      for (i = 0; filter_wm_classes[i]; i++)
        {
          if (strcmp (class_hint.res_class, filter_wm_classes[i]) == 0)
            {
              filtered = FALSE;
              break;
            }
        }

      XFree (class_hint.res_name);
      XFree (class_hint.res_class);
    }

  if (filtered)
    {
      /* We want to try and get the window managed by the next WM that come along,
       * so we need to make sure that windows that are requested to be mapped while
       * Mutter is running (!must_be_viewable), or windows already viewable at startup
       * get a non-withdrawn WM_STATE property. Previously unmapped windows are left
       * with whatever WM_STATE property they had.
       */
      if (!must_be_viewable || attrs->map_state == IsViewable)
        {
          gulong old_state;

          if (!meta_prop_get_cardinal_with_atom_type (display, xwindow,
                                                      display->atom_WM_STATE,
                                                      display->atom_WM_STATE,
                                                      &old_state))
            old_state = WithdrawnState;

          if (old_state == WithdrawnState)
            set_wm_state_on_xwindow (display, xwindow, NormalState);
        }

      /* Make sure filtered windows are hidden from view */
      XUnmapWindow (display->xdisplay, xwindow);
    }

  meta_error_trap_pop (display);

  return filtered;
}

static gboolean
is_our_xwindow (MetaDisplay       *display,
                MetaScreen        *screen,
                Window             xwindow,
                XWindowAttributes *attrs)
{
  if (xwindow == screen->no_focus_window)
    return TRUE;

  if (xwindow == screen->wm_sn_selection_window)
    return TRUE;

  if (xwindow == screen->wm_cm_selection_window)
    return TRUE;

  if (xwindow == screen->guard_window)
    return TRUE;

  if (xwindow == XCompositeGetOverlayWindow (display->xdisplay, screen->xroot))
    return TRUE;

  /* Any windows created via meta_create_offscreen_window */
  if (attrs->override_redirect && attrs->x == -100 && attrs->y == -100 && attrs->width == 1 && attrs->height == 1)
    return TRUE;

  return FALSE;
}

#ifdef WITH_VERBOSE_MODE
static const char*
wm_state_to_string (int state)
{
  switch (state)
    {
    case NormalState:
      return "NormalState";
    case IconicState:
      return "IconicState";
    case WithdrawnState:
      return "WithdrawnState";
    }

  return "Unknown";
}
#endif

MetaWindow *
meta_window_x11_new (MetaDisplay       *display,
                     Window             xwindow,
                     gboolean           must_be_viewable,
                     MetaCompEffect     effect)
{
  MetaScreen *screen = display->screen;
  XWindowAttributes attrs;
  gulong existing_wm_state;
  MetaWindow *window = NULL;
  gulong event_mask;

  meta_verbose ("Attempting to manage 0x%lx\n", xwindow);

  if (meta_display_xwindow_is_a_no_focus_window (display, xwindow))
    {
      meta_verbose ("Not managing no_focus_window 0x%lx\n",
                    xwindow);
      return NULL;
    }

  meta_error_trap_push (display); /* Push a trap over all of window
                                   * creation, to reduce XSync() calls
                                   */
  /*
   * This function executes without any server grabs held. This means that
   * the window could have already gone away, or could go away at any point,
   * so we must be careful with X error handling.
   */

  if (!XGetWindowAttributes (display->xdisplay, xwindow, &attrs))
    {
      meta_verbose ("Failed to get attributes for window 0x%lx\n",
                    xwindow);
      goto error;
    }

  if (attrs.root != screen->xroot)
    {
      meta_verbose ("Not on our screen\n");
      goto error;
    }

  if (is_our_xwindow (display, screen, xwindow, &attrs))
    {
      meta_verbose ("Not managing our own windows\n");
      goto error;
    }

  if (maybe_filter_xwindow (display, xwindow, must_be_viewable, &attrs))
    {
      meta_verbose ("Not managing filtered window\n");
      goto error;
    }

  existing_wm_state = WithdrawnState;
  if (must_be_viewable && attrs.map_state != IsViewable)
    {
      /* Only manage if WM_STATE is IconicState or NormalState */
      gulong state;

      /* WM_STATE isn't a cardinal, it's type WM_STATE, but is an int */
      if (!(meta_prop_get_cardinal_with_atom_type (display, xwindow,
                                                   display->atom_WM_STATE,
                                                   display->atom_WM_STATE,
                                                   &state) &&
            (state == IconicState || state == NormalState)))
        {
          meta_verbose ("Deciding not to manage unmapped or unviewable window 0x%lx\n", xwindow);
          goto error;
        }

      existing_wm_state = state;
      meta_verbose ("WM_STATE of %lx = %s\n", xwindow,
                    wm_state_to_string (existing_wm_state));
    }

  meta_error_trap_push_with_return (display);

  /*
   * XAddToSaveSet can only be called on windows created by a different
   * client.  with Mutter we want to be able to create manageable windows
   * from within the process (such as a dummy desktop window). As we do not
   * want this call failing to prevent the window from being managed, we
   * call this before creating the return-checked error trap.
   */
  XAddToSaveSet (display->xdisplay, xwindow);

  meta_error_trap_push_with_return (display);

  event_mask = PropertyChangeMask;
  if (attrs.override_redirect)
    event_mask |= StructureNotifyMask;

  /* If the window is from this client (a menu, say) we need to augment
   * the event mask, not replace it. For windows from other clients,
   * attrs.your_event_mask will be empty at this point.
   */
  XSelectInput (display->xdisplay, xwindow, attrs.your_event_mask | event_mask);

  {
    unsigned char mask_bits[XIMaskLen (XI_LASTEVENT)] = { 0 };
    XIEventMask mask = { XIAllMasterDevices, sizeof (mask_bits), mask_bits };

    meta_core_add_old_event_mask (display->xdisplay, xwindow, &mask);

    XISetMask (mask.mask, XI_Enter);
    XISetMask (mask.mask, XI_Leave);
    XISetMask (mask.mask, XI_FocusIn);
    XISetMask (mask.mask, XI_FocusOut);

    XISelectEvents (display->xdisplay, xwindow, &mask, 1);
  }

#ifdef HAVE_SHAPE
  if (META_DISPLAY_HAS_SHAPE (display))
    XShapeSelectInput (display->xdisplay, xwindow, ShapeNotifyMask);
#endif

  /* Get rid of any borders */
  if (attrs.border_width != 0)
    XSetWindowBorderWidth (display->xdisplay, xwindow, 0);

  /* Get rid of weird gravities */
  if (attrs.win_gravity != NorthWestGravity)
    {
      XSetWindowAttributes set_attrs;

      set_attrs.win_gravity = NorthWestGravity;

      XChangeWindowAttributes (display->xdisplay,
                               xwindow,
                               CWWinGravity,
                               &set_attrs);
    }

  if (meta_error_trap_pop_with_return (display) != Success)
    {
      meta_verbose ("Window 0x%lx disappeared just as we tried to manage it\n",
                    xwindow);
      goto error;
    }

  window = _meta_window_shared_new (display,
                                    screen,
                                    META_WINDOW_CLIENT_TYPE_X11,
                                    NULL,
                                    xwindow,
                                    existing_wm_state,
                                    effect,
                                    &attrs);
  meta_window_set_surface_mapped (window, TRUE);

  meta_error_trap_pop (display); /* pop the XSync()-reducing trap */
  return window;

error:
  meta_error_trap_pop (display);
  return NULL;
}

void
meta_window_x11_recalc_window_type (MetaWindow *window)
{
  MetaWindowX11 *window_x11 = META_WINDOW_X11 (window);
  MetaWindowX11Private *priv = meta_window_x11_get_instance_private (window_x11);
  MetaWindowType type;

  if (priv->type_atom != None)
    {
      if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_DESKTOP)
        type = META_WINDOW_DESKTOP;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_DOCK)
        type = META_WINDOW_DOCK;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_TOOLBAR)
        type = META_WINDOW_TOOLBAR;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_MENU)
        type = META_WINDOW_MENU;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_UTILITY)
        type = META_WINDOW_UTILITY;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_SPLASH)
        type = META_WINDOW_SPLASHSCREEN;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_DIALOG)
        type = META_WINDOW_DIALOG;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_NORMAL)
        type = META_WINDOW_NORMAL;
      /* The below are *typically* override-redirect windows, but the spec does
       * not disallow using them for managed windows.
       */
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_DROPDOWN_MENU)
        type = META_WINDOW_DROPDOWN_MENU;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_POPUP_MENU)
        type = META_WINDOW_POPUP_MENU;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_TOOLTIP)
        type = META_WINDOW_TOOLTIP;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_NOTIFICATION)
        type = META_WINDOW_NOTIFICATION;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_COMBO)
        type = META_WINDOW_COMBO;
      else if (priv->type_atom  == window->display->atom__NET_WM_WINDOW_TYPE_DND)
        type = META_WINDOW_DND;
      else
        {
          char *atom_name;

          /*
           * Fallback on a normal type, and print warning. Don't abort.
           */
          type = META_WINDOW_NORMAL;

          meta_error_trap_push (window->display);
          atom_name = XGetAtomName (window->display->xdisplay,
                                    priv->type_atom);
          meta_error_trap_pop (window->display);

          meta_warning ("Unrecognized type atom [%s] set for %s \n",
                        atom_name ? atom_name : "unknown",
                        window->desc);

          if (atom_name)
            XFree (atom_name);
        }
    }
  else if (window->transient_for != NULL)
    {
      type = META_WINDOW_DIALOG;
    }
  else
    {
      type = META_WINDOW_NORMAL;
    }

  if (type == META_WINDOW_DIALOG && priv->wm_state_modal)
    type = META_WINDOW_MODAL_DIALOG;

  /* We don't want to allow override-redirect windows to have decorated-window
   * types since that's just confusing.
   */
  if (window->override_redirect)
    {
      switch (window->type)
        {
        /* Decorated types */
        case META_WINDOW_NORMAL:
        case META_WINDOW_DIALOG:
        case META_WINDOW_MODAL_DIALOG:
        case META_WINDOW_MENU:
        case META_WINDOW_UTILITY:
          type = META_WINDOW_OVERRIDE_OTHER;
          break;
        /* Undecorated types, normally not override-redirect */
        case META_WINDOW_DESKTOP:
        case META_WINDOW_DOCK:
        case META_WINDOW_TOOLBAR:
        case META_WINDOW_SPLASHSCREEN:
        /* Undecorated types, normally override-redirect types */
        case META_WINDOW_DROPDOWN_MENU:
        case META_WINDOW_POPUP_MENU:
        case META_WINDOW_TOOLTIP:
        case META_WINDOW_NOTIFICATION:
        case META_WINDOW_COMBO:
        case META_WINDOW_DND:
        /* To complete enum */
        case META_WINDOW_OVERRIDE_OTHER:
          break;
        }
    }

  meta_verbose ("Calculated type %u for %s, old type %u\n",
                type, window->desc, type);
  meta_window_set_type (window, type);
}

/**
 * meta_window_x11_configure_notify: (skip)
 * @window: a #MetaWindow
 * @event: a #XConfigureEvent
 *
 * This is used to notify us of an unrequested configuration
 * (only applicable to override redirect windows)
 */
void
meta_window_x11_configure_notify (MetaWindow      *window,
                                  XConfigureEvent *event)
{
  g_assert (window->override_redirect);
  g_assert (window->frame == NULL);

  window->rect.x = event->x;
  window->rect.y = event->y;
  window->rect.width = event->width;
  window->rect.height = event->height;
  meta_window_update_monitor (window);

  /* Whether an override-redirect window is considered fullscreen depends
   * on its geometry.
   */
  if (window->override_redirect)
    meta_screen_queue_check_fullscreen (window->screen);

  if (!event->override_redirect && !event->send_event)
    meta_warning ("Unhandled change of windows override redirect status\n");

  meta_compositor_sync_window_geometry (window->display->compositor, window, FALSE);
}