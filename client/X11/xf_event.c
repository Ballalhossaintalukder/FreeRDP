/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * X11 Event Handling
 *
 * Copyright 2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2023 HP Development Company, L.P.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <string.h>
#include <math.h>

#include <winpr/assert.h>
#include <winpr/path.h>

#include <freerdp/log.h>
#include <freerdp/locale/keyboard.h>

#include "xf_rail.h"
#include "xf_window.h"
#include "xf_cliprdr.h"
#include "xf_disp.h"
#include "xf_input.h"
#include "xf_gfx.h"
#include "xf_graphics.h"
#include "xf_utils.h"

#include "xf_debug.h"
#include "xf_event.h"

#define TAG CLIENT_TAG("x11")

#define CLAMP_COORDINATES(x, y) \
	do                          \
	{                           \
		if ((x) < 0)            \
			(x) = 0;            \
		if ((y) < 0)            \
			(y) = 0;            \
	} while (0)

const char* x11_event_string(int event)
{
	switch (event)
	{
		case KeyPress:
			return "KeyPress";

		case KeyRelease:
			return "KeyRelease";

		case ButtonPress:
			return "ButtonPress";

		case ButtonRelease:
			return "ButtonRelease";

		case MotionNotify:
			return "MotionNotify";

		case EnterNotify:
			return "EnterNotify";

		case LeaveNotify:
			return "LeaveNotify";

		case FocusIn:
			return "FocusIn";

		case FocusOut:
			return "FocusOut";

		case KeymapNotify:
			return "KeymapNotify";

		case Expose:
			return "Expose";

		case GraphicsExpose:
			return "GraphicsExpose";

		case NoExpose:
			return "NoExpose";

		case VisibilityNotify:
			return "VisibilityNotify";

		case CreateNotify:
			return "CreateNotify";

		case DestroyNotify:
			return "DestroyNotify";

		case UnmapNotify:
			return "UnmapNotify";

		case MapNotify:
			return "MapNotify";

		case MapRequest:
			return "MapRequest";

		case ReparentNotify:
			return "ReparentNotify";

		case ConfigureNotify:
			return "ConfigureNotify";

		case ConfigureRequest:
			return "ConfigureRequest";

		case GravityNotify:
			return "GravityNotify";

		case ResizeRequest:
			return "ResizeRequest";

		case CirculateNotify:
			return "CirculateNotify";

		case CirculateRequest:
			return "CirculateRequest";

		case PropertyNotify:
			return "PropertyNotify";

		case SelectionClear:
			return "SelectionClear";

		case SelectionRequest:
			return "SelectionRequest";

		case SelectionNotify:
			return "SelectionNotify";

		case ColormapNotify:
			return "ColormapNotify";

		case ClientMessage:
			return "ClientMessage";

		case MappingNotify:
			return "MappingNotify";

		case GenericEvent:
			return "GenericEvent";

		default:
			return "UNKNOWN";
	}
}

static BOOL xf_action_script_append(xfContext* xfc, const char* buffer, size_t size,
                                    WINPR_ATTR_UNUSED void* user, const char* what, const char* arg)
{
	WINPR_ASSERT(xfc);
	WINPR_UNUSED(what);
	WINPR_UNUSED(arg);

	if (buffer || (size == 0))
		return TRUE;

	if (!ArrayList_Append(xfc->xevents, buffer))
	{
		ArrayList_Clear(xfc->xevents);
		return FALSE;
	}
	return TRUE;
}

BOOL xf_event_action_script_init(xfContext* xfc)
{
	WINPR_ASSERT(xfc);

	xf_event_action_script_free(xfc);

	xfc->xevents = ArrayList_New(TRUE);

	if (!xfc->xevents)
		return FALSE;

	wObject* obj = ArrayList_Object(xfc->xevents);
	WINPR_ASSERT(obj);
	obj->fnObjectNew = winpr_ObjectStringClone;
	obj->fnObjectFree = winpr_ObjectStringFree;

	return run_action_script(xfc, "xevent", NULL, xf_action_script_append, NULL);
}

void xf_event_action_script_free(xfContext* xfc)
{
	if (xfc->xevents)
	{
		ArrayList_Free(xfc->xevents);
		xfc->xevents = NULL;
	}
}

static BOOL action_script_run(xfContext* xfc, const char* buffer, size_t size, void* user,
                              const char* what, const char* arg)
{
	WINPR_UNUSED(xfc);
	WINPR_UNUSED(what);
	WINPR_UNUSED(arg);
	WINPR_ASSERT(user);
	int* pstatus = user;

	if (size == 0)
	{
		WLog_WARN(TAG, "ActionScript xevent: script did not return data");
		return FALSE;
	}

	if (winpr_PathFileExists(buffer))
	{
		char* cmd = NULL;
		size_t cmdlen = 0;
		winpr_asprintf(&cmd, &cmdlen, "%s %s %s", buffer, what, arg);
		if (!cmd)
			return FALSE;

		FILE* fp = popen(cmd, "w");
		free(cmd);
		if (!fp)
		{
			WLog_ERR(TAG, "Failed to execute '%s'", buffer);
			return FALSE;
		}

		*pstatus = pclose(fp);
		if (*pstatus < 0)
		{
			WLog_ERR(TAG, "Command '%s' returned %d", buffer, *pstatus);
			return FALSE;
		}
	}
	else
	{
		WLog_WARN(TAG, "ActionScript xevent: No such file '%s'", buffer);
		return FALSE;
	}

	return TRUE;
}

static BOOL xf_event_execute_action_script(xfContext* xfc, const XEvent* event)
{
	size_t count = 0;
	char* name = NULL;
	BOOL match = FALSE;
	const char* xeventName = NULL;

	if (!xfc->actionScriptExists || !xfc->xevents || !xfc->window)
		return FALSE;

	if (event->type > LASTEvent)
		return FALSE;

	xeventName = x11_event_string(event->type);
	count = ArrayList_Count(xfc->xevents);

	for (size_t index = 0; index < count; index++)
	{
		name = (char*)ArrayList_GetItem(xfc->xevents, index);

		if (_stricmp(name, xeventName) == 0)
		{
			match = TRUE;
			break;
		}
	}

	if (!match)
		return FALSE;

	char command[2048] = { 0 };
	char arg[2048] = { 0 };
	(void)_snprintf(command, sizeof(command), "xevent %s", xeventName);
	(void)_snprintf(arg, sizeof(arg), "%lu", (unsigned long)xfc->window->handle);
	return run_action_script(xfc, command, arg, action_script_run, NULL);
}

void xf_adjust_coordinates_to_screen(xfContext* xfc, UINT32* x, UINT32* y)
{
	if (!xfc || !xfc->common.context.settings || !y || !x)
		return;

	rdpSettings* settings = xfc->common.context.settings;
	INT64 tx = *x;
	INT64 ty = *y;
	if (!xfc->remote_app)
	{
#ifdef WITH_XRENDER

		if (xf_picture_transform_required(xfc))
		{
			const double dw = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
			const double dh = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
			double xScalingFactor = xfc->scaledWidth / dw;
			double yScalingFactor = xfc->scaledHeight / dh;
			tx = (INT64)lround((1.0 * (*x) + xfc->offset_x) * xScalingFactor);
			ty = (INT64)lround((1.0 * (*y) + xfc->offset_y) * yScalingFactor);
		}

#endif
	}

	CLAMP_COORDINATES(tx, ty);
	*x = (UINT32)tx;
	*y = (UINT32)ty;
}

void xf_event_adjust_coordinates(xfContext* xfc, int* x, int* y)
{
	if (!xfc || !xfc->common.context.settings || !y || !x)
		return;

	if (!xfc->remote_app)
	{
#ifdef WITH_XRENDER
		rdpSettings* settings = xfc->common.context.settings;
		if (xf_picture_transform_required(xfc))
		{
			double xScalingFactor = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) /
			                        (double)xfc->scaledWidth;
			double yScalingFactor = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) /
			                        (double)xfc->scaledHeight;
			*x = (int)((*x - xfc->offset_x) * xScalingFactor);
			*y = (int)((*y - xfc->offset_y) * yScalingFactor);
		}

#endif
	}

	CLAMP_COORDINATES(*x, *y);
}

static BOOL xf_event_Expose(xfContext* xfc, const XExposeEvent* event, BOOL app)
{
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	rdpSettings* settings = NULL;

	WINPR_ASSERT(xfc);
	WINPR_ASSERT(event);

	settings = xfc->common.context.settings;
	WINPR_ASSERT(settings);

	if (!app && (freerdp_settings_get_bool(settings, FreeRDP_SmartSizing) ||
	             freerdp_settings_get_bool(settings, FreeRDP_MultiTouchGestures)))
	{
		x = 0;
		y = 0;
		w = WINPR_ASSERTING_INT_CAST(int,
		                             freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth));
		h = WINPR_ASSERTING_INT_CAST(int,
		                             freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight));
	}
	else
	{
		x = event->x;
		y = event->y;
		w = event->width;
		h = event->height;
	}

	if (!app)
	{
		if (xfc->common.context.gdi->gfx)
		{
			xf_OutputExpose(
			    xfc, WINPR_ASSERTING_INT_CAST(uint32_t, x), WINPR_ASSERTING_INT_CAST(uint32_t, y),
			    WINPR_ASSERTING_INT_CAST(uint32_t, w), WINPR_ASSERTING_INT_CAST(uint32_t, h));
			return TRUE;
		}
		xf_draw_screen(xfc, x, y, w, h);
	}
	else
	{
		xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, event->window);
		if (appWindow)
		{
			xf_UpdateWindowArea(xfc, appWindow, x, y, w, h);
		}
	}

	return TRUE;
}

static BOOL xf_event_VisibilityNotify(xfContext* xfc, const XVisibilityEvent* event, BOOL app)
{
	WINPR_UNUSED(app);
	xfc->unobscured = event->state == VisibilityUnobscured;
	return TRUE;
}

BOOL xf_generic_MotionNotify(xfContext* xfc, int x, int y, Window window, BOOL app)
{
	Window childWindow = None;

	WINPR_ASSERT(xfc);
	WINPR_ASSERT(xfc->common.context.settings);

	if (app)
	{
		/* make sure window exists */
		if (!xf_AppWindowFromX11Window(xfc, window))
			return TRUE;

		/* Translate to desktop coordinates */
		XTranslateCoordinates(xfc->display, window, RootWindowOfScreen(xfc->screen), x, y, &x, &y,
		                      &childWindow);
	}

	xf_event_adjust_coordinates(xfc, &x, &y);
	freerdp_client_send_button_event(&xfc->common, FALSE, PTR_FLAGS_MOVE, x, y);

	if (xfc->fullscreen && !app)
	{
		if (xfc->window)
			XSetInputFocus(xfc->display, xfc->window->handle, RevertToPointerRoot, CurrentTime);
	}

	return TRUE;
}

BOOL xf_generic_RawMotionNotify(xfContext* xfc, int x, int y, WINPR_ATTR_UNUSED Window window,
                                BOOL app)
{
	WINPR_ASSERT(xfc);

	if (app)
	{
		WLog_ERR(TAG, "Relative mouse input is not supported with remoate app mode!");
		return FALSE;
	}

	return freerdp_client_send_button_event(&xfc->common, TRUE, PTR_FLAGS_MOVE, x, y);
}

static BOOL xf_event_MotionNotify(xfContext* xfc, const XMotionEvent* event, BOOL app)
{
	WINPR_ASSERT(xfc);

	if (xfc->window)
		xf_floatbar_set_root_y(xfc->window->floatbar, event->y);

	if (xfc->xi_event ||
	    (xfc->common.mouse_grabbed && freerdp_client_use_relative_mouse_events(&xfc->common)))
		return TRUE;

	return xf_generic_MotionNotify(xfc, event->x, event->y, event->window, app);
}

BOOL xf_generic_ButtonEvent(xfContext* xfc, int x, int y, int button, Window window, BOOL app,
                            BOOL down)
{
	UINT16 flags = 0;
	Window childWindow = None;

	WINPR_ASSERT(xfc);
	if (button < 0)
		return FALSE;

	for (size_t i = 0; i < ARRAYSIZE(xfc->button_map); i++)
	{
		const button_map* cur = &xfc->button_map[i];

		if (cur->button == (UINT32)button)
		{
			flags = cur->flags;
			break;
		}
	}

	if (flags != 0)
	{
		if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL))
		{
			if (down)
				freerdp_client_send_wheel_event(&xfc->common, flags);
		}
		else
		{
			BOOL extended = FALSE;

			if (flags & (PTR_XFLAGS_BUTTON1 | PTR_XFLAGS_BUTTON2))
			{
				extended = TRUE;

				if (down)
					flags |= PTR_XFLAGS_DOWN;
			}
			else if (flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3))
			{
				if (down)
					flags |= PTR_FLAGS_DOWN;
			}

			if (app)
			{
				/* make sure window exists */
				if (!xf_AppWindowFromX11Window(xfc, window))
					return TRUE;

				/* Translate to desktop coordinates */
				XTranslateCoordinates(xfc->display, window, RootWindowOfScreen(xfc->screen), x, y,
				                      &x, &y, &childWindow);
			}

			xf_event_adjust_coordinates(xfc, &x, &y);

			if (extended)
				freerdp_client_send_extended_button_event(&xfc->common, FALSE, flags, x, y);
			else
				freerdp_client_send_button_event(&xfc->common, FALSE, flags, x, y);
		}
	}

	return TRUE;
}

static BOOL xf_grab_mouse(xfContext* xfc)
{
	WINPR_ASSERT(xfc);

	if (!xfc->window)
		return FALSE;

	if (freerdp_settings_get_bool(xfc->common.context.settings, FreeRDP_GrabMouse))
	{
		XGrabPointer(xfc->display, xfc->window->handle, False,
		             ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
		                 EnterWindowMask | LeaveWindowMask,
		             GrabModeAsync, GrabModeAsync, xfc->window->handle, None, CurrentTime);
		xfc->common.mouse_grabbed = TRUE;
	}
	return TRUE;
}

static BOOL xf_grab_kbd(xfContext* xfc)
{
	WINPR_ASSERT(xfc);

	if (!xfc->window)
		return FALSE;

	XGrabKeyboard(xfc->display, xfc->window->handle, TRUE, GrabModeAsync, GrabModeAsync,
	              CurrentTime);
	return TRUE;
}

static BOOL xf_event_ButtonPress(xfContext* xfc, const XButtonEvent* event, BOOL app)
{
	xf_grab_mouse(xfc);

	if (xfc->xi_event ||
	    (xfc->common.mouse_grabbed && freerdp_client_use_relative_mouse_events(&xfc->common)))
		return TRUE;
	return xf_generic_ButtonEvent(xfc, event->x, event->y,
	                              WINPR_ASSERTING_INT_CAST(int, event->button), event->window, app,
	                              TRUE);
}

static BOOL xf_event_ButtonRelease(xfContext* xfc, const XButtonEvent* event, BOOL app)
{
	xf_grab_mouse(xfc);

	if (xfc->xi_event ||
	    (xfc->common.mouse_grabbed && freerdp_client_use_relative_mouse_events(&xfc->common)))
		return TRUE;
	return xf_generic_ButtonEvent(xfc, event->x, event->y,
	                              WINPR_ASSERTING_INT_CAST(int, event->button), event->window, app,
	                              FALSE);
}

static BOOL xf_event_KeyPress(xfContext* xfc, const XKeyEvent* event, BOOL app)
{
	KeySym keysym = 0;
	char str[256] = { 0 };
	union
	{
		const XKeyEvent* cev;
		XKeyEvent* ev;
	} cnv;
	cnv.cev = event;
	WINPR_UNUSED(app);
	XLookupString(cnv.ev, str, sizeof(str), &keysym, NULL);
	xf_keyboard_key_press(xfc, event, keysym);
	return TRUE;
}

static BOOL xf_event_KeyRelease(xfContext* xfc, const XKeyEvent* event, BOOL app)
{
	KeySym keysym = 0;
	char str[256] = { 0 };
	union
	{
		const XKeyEvent* cev;
		XKeyEvent* ev;
	} cnv;
	cnv.cev = event;

	WINPR_UNUSED(app);
	XLookupString(cnv.ev, str, sizeof(str), &keysym, NULL);
	xf_keyboard_key_release(xfc, event, keysym);
	return TRUE;
}

/* Release a key, but ignore the event in case of autorepeat.
 */
static BOOL xf_event_KeyReleaseOrIgnore(xfContext* xfc, const XKeyEvent* event, BOOL app)
{
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(event);

	if ((event->type == KeyRelease) && XEventsQueued(xfc->display, QueuedAfterReading))
	{
		XEvent nev = { 0 };
		XPeekEvent(xfc->display, &nev);

		if ((nev.type == KeyPress) && (nev.xkey.time == event->time) &&
		    (nev.xkey.keycode == event->keycode))
		{
			/* Key wasn’t actually released */
			return TRUE;
		}
	}

	return xf_event_KeyRelease(xfc, event, app);
}

static BOOL xf_event_FocusIn(xfContext* xfc, const XFocusInEvent* event, BOOL app)
{
	if (event->mode == NotifyGrab)
		return TRUE;

	xfc->focused = TRUE;

	if (xfc->mouse_active && !app)
	{
		xf_grab_mouse(xfc);
		if (!xf_grab_kbd(xfc))
			return FALSE;
	}

	/* Release all keys, should already be done at FocusOut but might be missed
	 * if the WM decided to use an alternate event order */
	if (!app)
		xf_keyboard_release_all_keypress(xfc);
	else
		xf_rail_send_activate(xfc, event->window, TRUE);

	xf_pointer_update_scale(xfc);

	if (app)
	{
		xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, event->window);

		/* Update the server with any window changes that occurred while the window was not focused.
		 */
		if (appWindow)
			xf_rail_adjust_position(xfc, appWindow);
	}

	xf_keyboard_focus_in(xfc);
	return TRUE;
}

static BOOL xf_event_FocusOut(xfContext* xfc, const XFocusOutEvent* event, BOOL app)
{
	if (event->mode == NotifyUngrab)
		return TRUE;

	xfc->focused = FALSE;

	if (event->mode == NotifyWhileGrabbed)
		XUngrabKeyboard(xfc->display, CurrentTime);

	xf_keyboard_release_all_keypress(xfc);
	if (app)
		xf_rail_send_activate(xfc, event->window, FALSE);

	return TRUE;
}

static BOOL xf_event_MappingNotify(xfContext* xfc, const XMappingEvent* event, BOOL app)
{
	WINPR_UNUSED(app);

	switch (event->request)
	{
		case MappingModifier:
			return xf_keyboard_update_modifier_map(xfc);
		case MappingKeyboard:
			WLog_VRB(TAG, "[%d] MappingKeyboard", event->request);
			return xf_keyboard_init(xfc);
		case MappingPointer:
			WLog_VRB(TAG, "[%d] MappingPointer", event->request);
			xf_button_map_init(xfc);
			return TRUE;
		default:
			WLog_WARN(TAG,
			          "[%d] Unsupported MappingNotify::request, must be one "
			          "of[MappingModifier(%d), MappingKeyboard(%d), MappingPointer(%d)]",
			          event->request, MappingModifier, MappingKeyboard, MappingPointer);
			return FALSE;
	}
}

static BOOL xf_event_ClientMessage(xfContext* xfc, const XClientMessageEvent* event, BOOL app)
{
	if ((event->message_type == xfc->WM_PROTOCOLS) &&
	    ((Atom)event->data.l[0] == xfc->WM_DELETE_WINDOW))
	{
		if (app)
		{
			xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, event->window);

			if (appWindow)
				return xf_rail_send_client_system_command(xfc, appWindow->windowId, SC_CLOSE);

			return TRUE;
		}
		else
		{
			DEBUG_X11("Main window closed");
			return FALSE;
		}
	}

	return TRUE;
}

static BOOL xf_event_EnterNotify(xfContext* xfc, const XEnterWindowEvent* event, BOOL app)
{
	if (!app)
	{
		if (!xfc->window)
			return FALSE;

		xfc->mouse_active = TRUE;

		if (xfc->fullscreen)
			XSetInputFocus(xfc->display, xfc->window->handle, RevertToPointerRoot, CurrentTime);

		if (xfc->focused)
			xf_grab_kbd(xfc);
	}
	else
	{
		xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, event->window);

		/* keep track of which window has focus so that we can apply pointer updates */
		xfc->appWindow = appWindow;
	}

	return TRUE;
}

static BOOL xf_event_LeaveNotify(xfContext* xfc, const XLeaveWindowEvent* event, BOOL app)
{
	if (event->mode == NotifyGrab || event->mode == NotifyUngrab)
		return TRUE;
	if (!app)
	{
		xfc->mouse_active = FALSE;
		XUngrabKeyboard(xfc->display, CurrentTime);
	}
	else
	{
		xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, event->window);

		/* keep track of which window has focus so that we can apply pointer updates */
		if (xfc->appWindow == appWindow)
			xfc->appWindow = NULL;
	}
	return TRUE;
}

static BOOL xf_event_ConfigureNotify(xfContext* xfc, const XConfigureEvent* event, BOOL app)
{
	Window childWindow = None;
	xfAppWindow* appWindow = NULL;

	WINPR_ASSERT(xfc);
	WINPR_ASSERT(event);

	const rdpSettings* settings = xfc->common.context.settings;
	WINPR_ASSERT(settings);

	WLog_DBG(TAG, "x=%" PRId32 ", y=%" PRId32 ", w=%" PRId32 ", h=%" PRId32, event->x, event->y,
	         event->width, event->height);

	if (!app)
	{
		if (!xfc->window)
			return FALSE;

		if (xfc->window->left != event->x)
			xfc->window->left = event->x;

		if (xfc->window->top != event->y)
			xfc->window->top = event->y;

		if (xfc->window->width != event->width || xfc->window->height != event->height)
		{
			xfc->window->width = event->width;
			xfc->window->height = event->height;
#ifdef WITH_XRENDER
			xfc->offset_x = 0;
			xfc->offset_y = 0;

			if (freerdp_settings_get_bool(settings, FreeRDP_SmartSizing) ||
			    freerdp_settings_get_bool(settings, FreeRDP_MultiTouchGestures))
			{
				xfc->scaledWidth = xfc->window->width;
				xfc->scaledHeight = xfc->window->height;
				xf_draw_screen(
				    xfc, 0, 0,
				    WINPR_ASSERTING_INT_CAST(
				        int32_t, freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth)),
				    WINPR_ASSERTING_INT_CAST(
				        int32_t, freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight)));
			}
			else
			{
				xfc->scaledWidth = WINPR_ASSERTING_INT_CAST(
				    int, freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth));
				xfc->scaledHeight = WINPR_ASSERTING_INT_CAST(
				    int, freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight));
			}

#endif
		}

		if (freerdp_settings_get_bool(settings, FreeRDP_DynamicResolutionUpdate))
		{
			int alignedWidth = 0;
			int alignedHeight = 0;
			alignedWidth = (xfc->window->width / 2) * 2;
			alignedHeight = (xfc->window->height / 2) * 2;
			/* ask the server to resize using the display channel */
			xf_disp_handle_configureNotify(xfc, alignedWidth, alignedHeight);
		}
	}
	else
	{
		appWindow = xf_AppWindowFromX11Window(xfc, event->window);

		if (appWindow)
		{
			/*
			 * ConfigureNotify coordinates are expressed relative to the window parent.
			 * Translate these to root window coordinates.
			 */
			XTranslateCoordinates(xfc->display, appWindow->handle, RootWindowOfScreen(xfc->screen),
			                      0, 0, &appWindow->x, &appWindow->y, &childWindow);
			appWindow->width = event->width;
			appWindow->height = event->height;

			xf_AppWindowResize(xfc, appWindow);

			/*
			 * Additional checks for not in a local move and not ignoring configure to send
			 * position update to server, also should the window not be focused then do not
			 * send to server yet (i.e. resizing using window decoration).
			 * The server will be updated when the window gets refocused.
			 */
			if (appWindow->decorations)
			{
				/* moving resizing using window decoration */
				xf_rail_adjust_position(xfc, appWindow);
			}
			else
			{
				if ((!event->send_event || appWindow->local_move.state == LMS_NOT_ACTIVE) &&
				    !appWindow->rail_ignore_configure && xfc->focused)
					xf_rail_adjust_position(xfc, appWindow);
			}
		}
	}
	return xf_pointer_update_scale(xfc);
}

static BOOL xf_event_MapNotify(xfContext* xfc, const XMapEvent* event, BOOL app)
{
	WINPR_ASSERT(xfc);
	if (!app)
		gdi_send_suppress_output(xfc->common.context.gdi, FALSE);
	else
	{
		xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, event->window);

		if (appWindow)
		{
			/* local restore event */
			/* This is now handled as part of the PropertyNotify
			 * Doing this here would inhibit the ability to restore a maximized window
			 * that is minimized back to the maximized state
			 */
			// xf_rail_send_client_system_command(xfc, appWindow->windowId, SC_RESTORE);
			appWindow->is_mapped = TRUE;
		}
	}

	return TRUE;
}

static BOOL xf_event_UnmapNotify(xfContext* xfc, const XUnmapEvent* event, BOOL app)
{
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(event);

	if (!app)
		xf_keyboard_release_all_keypress(xfc);

	if (!app)
		gdi_send_suppress_output(xfc->common.context.gdi, TRUE);
	else
	{
		xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, event->window);

		if (appWindow)
			appWindow->is_mapped = FALSE;
	}

	return TRUE;
}

static BOOL xf_event_PropertyNotify(xfContext* xfc, const XPropertyEvent* event, BOOL app)
{
	WINPR_ASSERT(xfc);
	WINPR_ASSERT(event);

	/*
	 * This section handles sending the appropriate commands to the rail server
	 * when the window has been minimized, maximized, restored locally
	 * ie. not using the buttons on the rail window itself
	 */
	if (((event->atom == xfc->NET_WM_STATE) && (event->state != PropertyDelete)) ||
	    ((event->atom == xfc->WM_STATE) && (event->state != PropertyDelete)))
	{
		BOOL status = FALSE;
		BOOL minimized = FALSE;
		BOOL minimizedChanged = FALSE;
		unsigned long nitems = 0;
		unsigned long bytes = 0;
		unsigned char* prop = NULL;
		xfAppWindow* appWindow = NULL;

		if (app)
		{
			appWindow = xf_AppWindowFromX11Window(xfc, event->window);

			if (!appWindow)
				return TRUE;
		}

		if (event->atom == xfc->NET_WM_STATE)
		{
			status = xf_GetWindowProperty(xfc, event->window, xfc->NET_WM_STATE, 12, &nitems,
			                              &bytes, &prop);

			if (status)
			{
				if (appWindow)
				{
					appWindow->maxVert = FALSE;
					appWindow->maxHorz = FALSE;
				}
				for (unsigned long i = 0; i < nitems; i++)
				{
					if ((Atom)((UINT16**)prop)[i] ==
					    Logging_XInternAtom(xfc->log, xfc->display, "_NET_WM_STATE_MAXIMIZED_VERT",
					                        False))
					{
						if (appWindow)
							appWindow->maxVert = TRUE;
					}

					if ((Atom)((UINT16**)prop)[i] ==
					    Logging_XInternAtom(xfc->log, xfc->display, "_NET_WM_STATE_MAXIMIZED_HORZ",
					                        False))
					{
						if (appWindow)
							appWindow->maxHorz = TRUE;
					}
				}

				XFree(prop);
			}
		}

		if (event->atom == xfc->WM_STATE)
		{
			status =
			    xf_GetWindowProperty(xfc, event->window, xfc->WM_STATE, 1, &nitems, &bytes, &prop);

			if (status)
			{
				/* If the window is in the iconic state */
				if (((UINT32)*prop == 3) && !IsGnome())
				{
					minimized = TRUE;
					if (appWindow)
						appWindow->minimized = TRUE;
				}
				else
				{
					minimized = FALSE;
					if (appWindow)
						appWindow->minimized = FALSE;
				}

				minimizedChanged = TRUE;
				XFree(prop);
			}
		}

		if (app)
		{
			WINPR_ASSERT(appWindow);
			if (appWindow->maxVert && appWindow->maxHorz && !appWindow->minimized)
			{
				if (appWindow->rail_state != WINDOW_SHOW_MAXIMIZED)
				{
					appWindow->rail_state = WINDOW_SHOW_MAXIMIZED;
					return xf_rail_send_client_system_command(xfc, appWindow->windowId,
					                                          SC_MAXIMIZE);
				}
			}
			else if (appWindow->minimized)
			{
				if (appWindow->rail_state != WINDOW_SHOW_MINIMIZED)
				{
					appWindow->rail_state = WINDOW_SHOW_MINIMIZED;
					return xf_rail_send_client_system_command(xfc, appWindow->windowId,
					                                          SC_MINIMIZE);
				}
			}
			else
			{
				if (appWindow->rail_state != WINDOW_SHOW && appWindow->rail_state != WINDOW_HIDE)
				{
					appWindow->rail_state = WINDOW_SHOW;
					return xf_rail_send_client_system_command(xfc, appWindow->windowId, SC_RESTORE);
				}
			}
		}
		else if (minimizedChanged)
			gdi_send_suppress_output(xfc->common.context.gdi, minimized);
	}

	return TRUE;
}

static BOOL xf_event_suppress_events(xfContext* xfc, xfAppWindow* appWindow, const XEvent* event)
{
	if (!xfc->remote_app)
		return FALSE;

	switch (appWindow->local_move.state)
	{
		case LMS_NOT_ACTIVE:

			/* No local move in progress, nothing to do */

			/* Prevent Configure from happening during indeterminant state of Horz or Vert Max only
			 */
			if ((event->type == ConfigureNotify) && appWindow->rail_ignore_configure)
			{
				appWindow->rail_ignore_configure = FALSE;
				return TRUE;
			}

			break;

		case LMS_STARTING:

			/* Local move initiated by RDP server, but we have not yet seen any updates from the X
			 * server */
			switch (event->type)
			{
				case ConfigureNotify:
					/* Starting to see move events from the X server. Local move is now in progress.
					 */
					appWindow->local_move.state = LMS_ACTIVE;
					/* Allow these events to be processed during move to keep our state up to date.
					 */
					break;

				case ButtonPress:
				case ButtonRelease:
				case KeyPress:
				case KeyRelease:
				case UnmapNotify:
					/*
					 * A button release event means the X window server did not grab the
					 * mouse before the user released it. In this case we must cancel the
					 * local move. The event will be processed below as normal, below.
					 */
					break;

				case VisibilityNotify:
				case PropertyNotify:
				case Expose:
					/* Allow these events to pass */
					break;

				default:
					/* Eat any other events */
					return TRUE;
			}

			break;

		case LMS_ACTIVE:

			/* Local move is in progress */
			switch (event->type)
			{
				case ConfigureNotify:
				case VisibilityNotify:
				case PropertyNotify:
				case Expose:
				case GravityNotify:
					/* Keep us up to date on position */
					break;

				default:
					/* Any other event terminates move */
					xf_rail_end_local_move(xfc, appWindow);
					break;
			}

			break;

		case LMS_TERMINATING:
			/* Already sent RDP end move to server. Allow events to pass. */
			break;
		default:
			break;
	}

	return FALSE;
}

BOOL xf_event_process(freerdp* instance, const XEvent* event)
{
	BOOL status = TRUE;

	WINPR_ASSERT(instance);
	WINPR_ASSERT(event);

	xfContext* xfc = (xfContext*)instance->context;
	WINPR_ASSERT(xfc);

	rdpSettings* settings = xfc->common.context.settings;
	WINPR_ASSERT(settings);

	if (xfc->remote_app)
	{
		xfAppWindow* appWindow = xf_AppWindowFromX11Window(xfc, event->xany.window);

		if (appWindow)
		{
			/* Update "current" window for cursor change orders */
			xfc->appWindow = appWindow;

			if (xf_event_suppress_events(xfc, appWindow, event))
				return TRUE;
		}
	}

	if (xfc->window)
	{
		xfFloatbar* floatbar = xfc->window->floatbar;
		if (xf_floatbar_check_event(floatbar, event))
		{
			xf_floatbar_event_process(floatbar, event);
			return TRUE;
		}

		if (xf_floatbar_is_locked(floatbar))
		{
			/* Filter input events, floatbar is locked do not forward anything to the session */
			switch (event->type)
			{
				case MotionNotify:
				case ButtonPress:
				case ButtonRelease:
				case KeyPress:
				case KeyRelease:
				case FocusIn:
				case FocusOut:
				case EnterNotify:
				case LeaveNotify:
					return TRUE;
				default:
					break;
			}
		}
	}

	xf_event_execute_action_script(xfc, event);

	if (event->type != MotionNotify)
	{
		DEBUG_X11("%s Event(%d): wnd=0x%08lX", x11_event_string(event->type), event->type,
		          (unsigned long)event->xany.window);
	}

	switch (event->type)
	{
		case Expose:
			status = xf_event_Expose(xfc, &event->xexpose, xfc->remote_app);
			break;

		case VisibilityNotify:
			status = xf_event_VisibilityNotify(xfc, &event->xvisibility, xfc->remote_app);
			break;

		case MotionNotify:
			status = xf_event_MotionNotify(xfc, &event->xmotion, xfc->remote_app);
			break;

		case ButtonPress:
			status = xf_event_ButtonPress(xfc, &event->xbutton, xfc->remote_app);
			break;

		case ButtonRelease:
			status = xf_event_ButtonRelease(xfc, &event->xbutton, xfc->remote_app);
			break;

		case KeyPress:
			status = xf_event_KeyPress(xfc, &event->xkey, xfc->remote_app);
			break;

		case KeyRelease:
			status = xf_event_KeyReleaseOrIgnore(xfc, &event->xkey, xfc->remote_app);
			break;

		case FocusIn:
			status = xf_event_FocusIn(xfc, &event->xfocus, xfc->remote_app);
			break;

		case FocusOut:
			status = xf_event_FocusOut(xfc, &event->xfocus, xfc->remote_app);
			break;

		case EnterNotify:
			status = xf_event_EnterNotify(xfc, &event->xcrossing, xfc->remote_app);
			break;

		case LeaveNotify:
			status = xf_event_LeaveNotify(xfc, &event->xcrossing, xfc->remote_app);
			break;

		case NoExpose:
			break;

		case GraphicsExpose:
			break;

		case ConfigureNotify:
			status = xf_event_ConfigureNotify(xfc, &event->xconfigure, xfc->remote_app);
			break;

		case MapNotify:
			status = xf_event_MapNotify(xfc, &event->xmap, xfc->remote_app);
			break;

		case UnmapNotify:
			status = xf_event_UnmapNotify(xfc, &event->xunmap, xfc->remote_app);
			break;

		case ReparentNotify:
			break;

		case MappingNotify:
			status = xf_event_MappingNotify(xfc, &event->xmapping, xfc->remote_app);
			break;

		case ClientMessage:
			status = xf_event_ClientMessage(xfc, &event->xclient, xfc->remote_app);
			break;

		case PropertyNotify:
			status = xf_event_PropertyNotify(xfc, &event->xproperty, xfc->remote_app);
			break;

		default:
			if (freerdp_settings_get_bool(settings, FreeRDP_SupportDisplayControl))
				xf_disp_handle_xevent(xfc, event);

			break;
	}

	xfWindow* window = xfc->window;
	xfFloatbar* floatbar = NULL;
	if (window)
		floatbar = window->floatbar;

	xf_cliprdr_handle_xevent(xfc, event);
	if (!xf_floatbar_check_event(floatbar, event) && !xf_floatbar_is_locked(floatbar))
		xf_input_handle_event(xfc, event);

	LogDynAndXSync(xfc->log, xfc->display, FALSE);
	return status;
}

BOOL xf_generic_RawButtonEvent(xfContext* xfc, int button, BOOL app, BOOL down)
{
	UINT16 flags = 0;

	if (app || (button < 0))
		return FALSE;

	for (size_t i = 0; i < ARRAYSIZE(xfc->button_map); i++)
	{
		const button_map* cur = &xfc->button_map[i];

		if (cur->button == (UINT32)button)
		{
			flags = cur->flags;
			break;
		}
	}

	if (flags != 0)
	{
		if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL))
		{
			if (down)
				freerdp_client_send_wheel_event(&xfc->common, flags);
		}
		else
		{
			BOOL extended = FALSE;

			if (flags & (PTR_XFLAGS_BUTTON1 | PTR_XFLAGS_BUTTON2))
			{
				extended = TRUE;

				if (down)
					flags |= PTR_XFLAGS_DOWN;
			}
			else if (flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3))
			{
				if (down)
					flags |= PTR_FLAGS_DOWN;
			}

			if (extended)
				freerdp_client_send_extended_button_event(&xfc->common, TRUE, flags, 0, 0);
			else
				freerdp_client_send_button_event(&xfc->common, TRUE, flags, 0, 0);
		}
	}

	return TRUE;
}
