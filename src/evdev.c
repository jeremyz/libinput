/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2013 Jonas Ådahl
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>
#include <mtdev.h>
#include <assert.h>

#include "libinput.h"
#include "evdev.h"
#include "libinput-private.h"

#define DEFAULT_AXIS_STEP_DISTANCE li_fixed_from_int(10)

void
evdev_device_led_update(struct evdev_device *device, enum libinput_led leds)
{
	static const struct {
		enum libinput_led weston;
		int evdev;
	} map[] = {
		{ LIBINPUT_LED_NUM_LOCK, LED_NUML },
		{ LIBINPUT_LED_CAPS_LOCK, LED_CAPSL },
		{ LIBINPUT_LED_SCROLL_LOCK, LED_SCROLLL },
	};
	struct input_event ev[ARRAY_LENGTH(map) + 1];
	unsigned int i;

	if (!device->caps & EVDEV_KEYBOARD)
		return;

	memset(ev, 0, sizeof(ev));
	for (i = 0; i < ARRAY_LENGTH(map); i++) {
		ev[i].type = EV_LED;
		ev[i].code = map[i].evdev;
		ev[i].value = !!(leds & map[i].weston);
	}
	ev[i].type = EV_SYN;
	ev[i].code = SYN_REPORT;

	i = write(device->fd, ev, sizeof ev);
	(void)i; /* no, we really don't care about the return value */
}

static void
transform_absolute(struct evdev_device *device, int32_t *x, int32_t *y)
{
	if (!device->abs.apply_calibration) {
		*x = device->abs.x;
		*y = device->abs.y;
		return;
	} else {
		*x = device->abs.x * device->abs.calibration[0] +
			device->abs.y * device->abs.calibration[1] +
			device->abs.calibration[2];

		*y = device->abs.x * device->abs.calibration[3] +
			device->abs.y * device->abs.calibration[4] +
			device->abs.calibration[5];
	}
}

static void
evdev_flush_pending_event(struct evdev_device *device, uint32_t time)
{
	int32_t cx, cy;
	int slot;
	struct libinput_device *base = &device->base;

	slot = device->mt.slot;

	switch (device->pending_event) {
	case EVDEV_NONE:
		return;
	case EVDEV_RELATIVE_MOTION:
		pointer_notify_motion(base,
				      time,
				      device->rel.dx,
				      device->rel.dy);
		device->rel.dx = 0;
		device->rel.dy = 0;
		goto handled;
	case EVDEV_ABSOLUTE_MT_DOWN:
		touch_notify_touch(base,
				   time,
				   slot,
				   li_fixed_from_int(device->mt.slots[slot].x),
				   li_fixed_from_int(device->mt.slots[slot].y),
				   LIBINPUT_TOUCH_TYPE_DOWN);
		goto handled;
	case EVDEV_ABSOLUTE_MT_MOTION:
		touch_notify_touch(base,
				   time,
				   slot,
				   li_fixed_from_int(device->mt.slots[slot].x),
				   li_fixed_from_int(device->mt.slots[slot].y),
				   LIBINPUT_TOUCH_TYPE_MOTION);
		goto handled;
	case EVDEV_ABSOLUTE_MT_UP:
		touch_notify_touch(base,
				   time,
				   slot,
				   0, 0,
				   LIBINPUT_TOUCH_TYPE_UP);
		goto handled;
	case EVDEV_ABSOLUTE_TOUCH_DOWN:
		transform_absolute(device, &cx, &cy);
		touch_notify_touch(base,
				   time,
				   slot,
				   li_fixed_from_int(cx),
				   li_fixed_from_int(cy),
				   LIBINPUT_TOUCH_TYPE_DOWN);
		goto handled;
	case EVDEV_ABSOLUTE_MOTION:
		transform_absolute(device, &cx, &cy);
		if (device->caps & EVDEV_TOUCH) {
			touch_notify_touch(base,
					   time,
					   slot,
					   li_fixed_from_int(cx),
					   li_fixed_from_int(cy),
					   LIBINPUT_TOUCH_TYPE_DOWN);
		} else {
			pointer_notify_motion_absolute(base,
						       time,
						       li_fixed_from_int(cx),
						       li_fixed_from_int(cy));
		}
		goto handled;
	case EVDEV_ABSOLUTE_TOUCH_UP:
		touch_notify_touch(base,
				   time,
				   0, 0, 0, LIBINPUT_TOUCH_TYPE_UP);
		goto handled;
	}

	assert(0 && "Unknown pending event type");

handled:
	device->pending_event = EVDEV_NONE;
}

static void
evdev_process_touch_button(struct evdev_device *device, int time, int value)
{
	if (device->pending_event != EVDEV_NONE &&
	    device->pending_event != EVDEV_ABSOLUTE_MOTION)
		evdev_flush_pending_event(device, time);

	device->pending_event = (value ?
				 EVDEV_ABSOLUTE_TOUCH_DOWN :
				 EVDEV_ABSOLUTE_TOUCH_UP);
}

static inline void
evdev_process_key(struct evdev_device *device, struct input_event *e, int time)
{
	/* ignore kernel key repeat */
	if (e->value == 2)
		return;

	if (e->code == BTN_TOUCH) {
		if (!device->is_mt)
			evdev_process_touch_button(device, time, e->value);
		return;
	}

	evdev_flush_pending_event(device, time);

	switch (e->code) {
	case BTN_LEFT:
	case BTN_RIGHT:
	case BTN_MIDDLE:
	case BTN_SIDE:
	case BTN_EXTRA:
	case BTN_FORWARD:
	case BTN_BACK:
	case BTN_TASK:
		pointer_notify_button(
			&device->base,
			time,
			e->code,
			e->value ? LIBINPUT_POINTER_BUTTON_STATE_PRESSED :
				   LIBINPUT_POINTER_BUTTON_STATE_RELEASED);
		break;

	default:
		keyboard_notify_key(
			&device->base,
			time,
			e->code,
			e->value ? LIBINPUT_KEYBOARD_KEY_STATE_PRESSED :
				   LIBINPUT_KEYBOARD_KEY_STATE_RELEASED);
		break;
	}
}

static void
evdev_process_touch(struct evdev_device *device,
		    struct input_event *e,
		    uint32_t time)
{
	int screen_width;
	int screen_height;

	device->base.device_interface->get_current_screen_dimensions(
		&screen_width,
		&screen_height,
		device->base.device_interface_data);

	switch (e->code) {
	case ABS_MT_SLOT:
		evdev_flush_pending_event(device, time);
		device->mt.slot = e->value;
		break;
	case ABS_MT_TRACKING_ID:
		if (device->pending_event != EVDEV_NONE &&
		    device->pending_event != EVDEV_ABSOLUTE_MT_MOTION)
			evdev_flush_pending_event(device, time);
		if (e->value >= 0)
			device->pending_event = EVDEV_ABSOLUTE_MT_DOWN;
		else
			device->pending_event = EVDEV_ABSOLUTE_MT_UP;
		break;
	case ABS_MT_POSITION_X:
		device->mt.slots[device->mt.slot].x =
			(e->value - device->abs.min_x) * screen_width /
			(device->abs.max_x - device->abs.min_x);
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MT_MOTION;
		break;
	case ABS_MT_POSITION_Y:
		device->mt.slots[device->mt.slot].y =
			(e->value - device->abs.min_y) * screen_height /
			(device->abs.max_y - device->abs.min_y);
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MT_MOTION;
		break;
	}
}

static inline void
evdev_process_absolute_motion(struct evdev_device *device,
			      struct input_event *e)
{
	int screen_width;
	int screen_height;

	device->base.device_interface->get_current_screen_dimensions(
		&screen_width,
		&screen_height,
		device->base.device_interface_data);

	switch (e->code) {
	case ABS_X:
		device->abs.x =
			(e->value - device->abs.min_x) * screen_width /
			(device->abs.max_x - device->abs.min_x);
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MOTION;
		break;
	case ABS_Y:
		device->abs.y =
			(e->value - device->abs.min_y) * screen_height /
			(device->abs.max_y - device->abs.min_y);
		if (device->pending_event == EVDEV_NONE)
			device->pending_event = EVDEV_ABSOLUTE_MOTION;
		break;
	}
}

static inline void
evdev_process_relative(struct evdev_device *device,
		       struct input_event *e, uint32_t time)
{
	struct libinput_device *base = &device->base;

	switch (e->code) {
	case REL_X:
		if (device->pending_event != EVDEV_RELATIVE_MOTION)
			evdev_flush_pending_event(device, time);
		device->rel.dx += li_fixed_from_int(e->value);
		device->pending_event = EVDEV_RELATIVE_MOTION;
		break;
	case REL_Y:
		if (device->pending_event != EVDEV_RELATIVE_MOTION)
			evdev_flush_pending_event(device, time);
		device->rel.dy += li_fixed_from_int(e->value);
		device->pending_event = EVDEV_RELATIVE_MOTION;
		break;
	case REL_WHEEL:
		evdev_flush_pending_event(device, time);
		switch (e->value) {
		case -1:
			/* Scroll down */
		case 1:
			/* Scroll up */
			pointer_notify_axis(
				base,
				time,
				LIBINPUT_POINTER_AXIS_VERTICAL_SCROLL,
				-1 * e->value * DEFAULT_AXIS_STEP_DISTANCE);
			break;
		default:
			break;
		}
		break;
	case REL_HWHEEL:
		evdev_flush_pending_event(device, time);
		switch (e->value) {
		case -1:
			/* Scroll left */
		case 1:
			/* Scroll right */
			pointer_notify_axis(
				base,
				time,
				LIBINPUT_POINTER_AXIS_HORIZONTAL_SCROLL,
				e->value * DEFAULT_AXIS_STEP_DISTANCE);
			break;
		default:
			break;

		}
	}
}

static inline void
evdev_process_absolute(struct evdev_device *device,
		       struct input_event *e,
		       uint32_t time)
{
	if (device->is_mt) {
		evdev_process_touch(device, e, time);
	} else {
		evdev_process_absolute_motion(device, e);
	}
}

static void
fallback_process(struct evdev_dispatch *dispatch,
		 struct evdev_device *device,
		 struct input_event *event,
		 uint32_t time)
{
	switch (event->type) {
	case EV_REL:
		evdev_process_relative(device, event, time);
		break;
	case EV_ABS:
		evdev_process_absolute(device, event, time);
		break;
	case EV_KEY:
		evdev_process_key(device, event, time);
		break;
	case EV_SYN:
		evdev_flush_pending_event(device, time);
		break;
	}
}

static void
fallback_destroy(struct evdev_dispatch *dispatch)
{
	free(dispatch);
}

struct evdev_dispatch_interface fallback_interface = {
	fallback_process,
	fallback_destroy
};

static struct evdev_dispatch *
fallback_dispatch_create(void)
{
	struct evdev_dispatch *dispatch = malloc(sizeof *dispatch);
	if (dispatch == NULL)
		return NULL;

	dispatch->interface = &fallback_interface;

	return dispatch;
}

static void
evdev_process_events(struct evdev_device *device,
		     struct input_event *ev, int count)
{
	struct evdev_dispatch *dispatch = device->dispatch;
	struct input_event *e, *end;
	uint32_t time = 0;

	e = ev;
	end = e + count;
	for (e = ev; e < end; e++) {
		time = e->time.tv_sec * 1000 + e->time.tv_usec / 1000;

		dispatch->interface->process(dispatch, device, e, time);
	}
}

int
evdev_device_dispatch(struct evdev_device *device)
{
	int fd = device->fd;
	struct input_event ev[32];
	int len;

	/* If the compositor is repainting, this function is called only once
	 * per frame and we have to process all the events available on the
	 * fd, otherwise there will be input lag. */
	do {
		if (device->mtdev)
			len = mtdev_get(device->mtdev, fd, ev,
					ARRAY_LENGTH(ev)) *
				sizeof (struct input_event);
		else
			len = read(fd, &ev, sizeof ev);

		if (len < 0 || len % sizeof ev[0] != 0) {
			if (len < 0 && errno != EAGAIN && errno != EINTR) {
				device->base.device_interface->device_lost(
					device->base.device_interface_data);
			}

			return 1;
		}

		evdev_process_events(device, ev, len / sizeof ev[0]);

	} while (len > 0);

	return 1;
}

static int
evdev_handle_device(struct evdev_device *device)
{
	struct input_absinfo absinfo;
	unsigned long ev_bits[NBITS(EV_MAX)];
	unsigned long abs_bits[NBITS(ABS_MAX)];
	unsigned long rel_bits[NBITS(REL_MAX)];
	unsigned long key_bits[NBITS(KEY_MAX)];
	int has_key, has_abs;
	unsigned int i;

	has_key = 0;
	has_abs = 0;
	device->caps = 0;

	ioctl(device->fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
	if (TEST_BIT(ev_bits, EV_ABS)) {
		has_abs = 1;

		ioctl(device->fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)),
		      abs_bits);

		if (TEST_BIT(abs_bits, ABS_WHEEL) ||
		    TEST_BIT(abs_bits, ABS_GAS) ||
		    TEST_BIT(abs_bits, ABS_BRAKE) ||
		    TEST_BIT(abs_bits, ABS_HAT0X)) {
			/* Device %s is a joystick, ignoring. */
			return 0;
		}

		if (TEST_BIT(abs_bits, ABS_X)) {
			ioctl(device->fd, EVIOCGABS(ABS_X), &absinfo);
			device->abs.min_x = absinfo.minimum;
			device->abs.max_x = absinfo.maximum;
			device->caps |= EVDEV_MOTION_ABS;
		}
		if (TEST_BIT(abs_bits, ABS_Y)) {
			ioctl(device->fd, EVIOCGABS(ABS_Y), &absinfo);
			device->abs.min_y = absinfo.minimum;
			device->abs.max_y = absinfo.maximum;
			device->caps |= EVDEV_MOTION_ABS;
		}
                /* We only handle the slotted Protocol B in weston.
                   Devices with ABS_MT_POSITION_* but not ABS_MT_SLOT
                   require mtdev for conversion. */
		if (TEST_BIT(abs_bits, ABS_MT_POSITION_X) &&
		    TEST_BIT(abs_bits, ABS_MT_POSITION_Y)) {
			ioctl(device->fd, EVIOCGABS(ABS_MT_POSITION_X),
			      &absinfo);
			device->abs.min_x = absinfo.minimum;
			device->abs.max_x = absinfo.maximum;
			ioctl(device->fd, EVIOCGABS(ABS_MT_POSITION_Y),
			      &absinfo);
			device->abs.min_y = absinfo.minimum;
			device->abs.max_y = absinfo.maximum;
			device->is_mt = 1;
			device->caps |= EVDEV_TOUCH;

			if (!TEST_BIT(abs_bits, ABS_MT_SLOT)) {
				device->mtdev = mtdev_new_open(device->fd);
				if (!device->mtdev) {
					/* mtdev required but failed to open. */
					return 0;
				}
				device->mt.slot = device->mtdev->caps.slot.value;
			} else {
				ioctl(device->fd, EVIOCGABS(ABS_MT_SLOT),
				      &absinfo);
				device->mt.slot = absinfo.value;
			}
		}
	}
	if (TEST_BIT(ev_bits, EV_REL)) {
		ioctl(device->fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)),
		      rel_bits);
		if (TEST_BIT(rel_bits, REL_X) || TEST_BIT(rel_bits, REL_Y))
			device->caps |= EVDEV_MOTION_REL;
	}
	if (TEST_BIT(ev_bits, EV_KEY)) {
		has_key = 1;
		ioctl(device->fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)),
		      key_bits);
		if (TEST_BIT(key_bits, BTN_TOOL_FINGER) &&
		    !TEST_BIT(key_bits, BTN_TOOL_PEN) &&
		    has_abs) {
			device->dispatch = evdev_touchpad_create(device);
		}
		for (i = KEY_ESC; i < KEY_MAX; i++) {
			if (i >= BTN_MISC && i < KEY_OK)
				continue;
			if (TEST_BIT(key_bits, i)) {
				device->caps |= EVDEV_KEYBOARD;
				break;
			}
		}
		if (TEST_BIT(key_bits, BTN_TOUCH)) {
			device->caps |= EVDEV_TOUCH;
		}
		for (i = BTN_MISC; i < BTN_JOYSTICK; i++) {
			if (TEST_BIT(key_bits, i)) {
				device->caps |= EVDEV_BUTTON;
				device->caps &= ~EVDEV_TOUCH;
				break;
			}
		}
	}
	if (TEST_BIT(ev_bits, EV_LED)) {
		device->caps |= EVDEV_KEYBOARD;
	}

	/* This rule tries to catch accelerometer devices and opt out. We may
	 * want to adjust the protocol later adding a proper event for dealing
	 * with accelerometers and implement here accordingly */
	if (has_abs && !has_key && !device->is_mt) {
		return 0;
	}

	return 1;
}

static int
evdev_configure_device(struct evdev_device *device)
{
	if ((device->caps & (EVDEV_MOTION_ABS | EVDEV_MOTION_REL)) &&
	    (device->caps & EVDEV_BUTTON)) {
		device->base.device_interface->register_capability(
			LIBINPUT_SEAT_CAP_POINTER,
			device->base.device_interface_data);
		device->seat_caps |= EVDEV_SEAT_POINTER;
	}
	if ((device->caps & EVDEV_KEYBOARD)) {
		device->base.device_interface->register_capability(
			LIBINPUT_SEAT_CAP_KEYBOARD,
			device->base.device_interface_data);
		device->seat_caps |= EVDEV_SEAT_KEYBOARD;
	}
	if ((device->caps & EVDEV_TOUCH)) {
		device->base.device_interface->register_capability(
			LIBINPUT_SEAT_CAP_TOUCH,
			device->base.device_interface_data);
		device->seat_caps |= EVDEV_SEAT_TOUCH;
	}

	return 0;
}

LIBINPUT_EXPORT struct libinput_device *
libinput_device_create_evdev(
	const char *devnode,
	int fd,
	const struct libinput_device_interface *device_interface,
	void *user_data)
{
	struct evdev_device *device;
	char devname[256] = "unknown";

	device = zalloc(sizeof *device);
	if (device == NULL)
		return NULL;

	device->base.device_interface = device_interface;
	device->base.device_interface_data = user_data;

	device->seat_caps = 0;
	device->is_mt = 0;
	device->mtdev = NULL;
	device->devnode = strdup(devnode);
	device->mt.slot = -1;
	device->rel.dx = 0;
	device->rel.dy = 0;
	device->dispatch = NULL;
	device->fd = fd;
	device->pending_event = EVDEV_NONE;

	ioctl(device->fd, EVIOCGNAME(sizeof(devname)), devname);
	devname[sizeof(devname) - 1] = '\0';
	device->devname = strdup(devname);

	if (!evdev_handle_device(device)) {
		evdev_device_destroy(device);
		return NULL;
	}

	if (evdev_configure_device(device) == -1)
		goto err;

	/* If the dispatch was not set up use the fallback. */
	if (device->dispatch == NULL)
		device->dispatch = fallback_dispatch_create();
	if (device->dispatch == NULL)
		goto err;

	return &device->base;

err:
	evdev_device_destroy(device);
	return NULL;
}

int
evdev_device_get_keys(struct evdev_device *device, char *keys, size_t size)
{
	memset(keys, 0, size);
	return ioctl(device->fd, EVIOCGKEY(size), keys);
}

void
evdev_device_calibrate(struct evdev_device *device, float calibration[6])
{
	device->abs.apply_calibration = 1;
	memcpy(device->abs.calibration, calibration, sizeof calibration);
}

void
evdev_device_destroy(struct evdev_device *device)
{
	struct evdev_dispatch *dispatch;

	if (device->seat_caps & EVDEV_SEAT_POINTER) {
		device->base.device_interface->unregister_capability(
			LIBINPUT_SEAT_CAP_POINTER,
			device->base.device_interface_data);
	}
	if (device->seat_caps & EVDEV_SEAT_KEYBOARD) {
		device->base.device_interface->unregister_capability(
			LIBINPUT_SEAT_CAP_KEYBOARD,
			device->base.device_interface_data);
	}
	if (device->seat_caps & EVDEV_SEAT_TOUCH) {
		device->base.device_interface->unregister_capability(
			LIBINPUT_SEAT_CAP_TOUCH,
			device->base.device_interface_data);
	}

	dispatch = device->dispatch;
	if (dispatch)
		dispatch->interface->destroy(dispatch);

	if (device->mtdev)
		mtdev_close_delete(device->mtdev);
	close(device->fd);
	free(device->devname);
	free(device->devnode);
	free(device);
}
