/*
 * Copyright © notice goes here
 *
 * License goes here
 */

#include "config.h"

#include <math.h>
#include <stdbool.h>
#include <limits.h>

#include "quirks.h"
#include "evdev-mt-touchpad.h"

#define SCROLL_MM_X 35 /* mm distance to assume fingers are a scroll */
#define SCROLL_MM_Y 25

static inline const char*
thumb_state_to_str(enum tp_thumb_state state)
{
	switch(state){
	CASE_RETURN_STRING(THUMB_STATE_LIVE);
	CASE_RETURN_STRING(THUMB_STATE_JAILED);
	CASE_RETURN_STRING(THUMB_STATE_PINCH);
	CASE_RETURN_STRING(THUMB_STATE_SUPPRESSED);
	CASE_RETURN_STRING(THUMB_STATE_REVIVED);
	CASE_RETURN_STRING(THUMB_STATE_REV_JAILED);
	CASE_RETURN_STRING(THUMB_STATE_DEAD);
	}

	return NULL;
}

static void
tp_thumb_set_state(struct tp_dispatch *tp,
		   struct tp_touch *t,
		   enum tp_thumb_state state)
{
	if (tp->thumb.state != state ||
	    tp->thumb.index != t->index) {
		evdev_log_debug(tp->device, //TODO show changed index
			"thumb state: touch %d, %s → %s\n",
			t->index,
			thumb_state_to_str(tp->thumb.state),
			thumb_state_to_str(state));
	}

	tp->thumb.state = state;
	tp->thumb.index = t->index;
}

void
tp_thumb_reset(struct tp_dispatch *tp)
{
	tp->thumb.state = THUMB_STATE_LIVE;
	tp->thumb.index = UINT_MAX;
	tp->thumb.pinch_eligible = true;
}

static void
tp_thumb_lift(struct tp_dispatch *tp)
{
	tp->thumb.state = THUMB_STATE_LIVE;
	tp->thumb.index = UINT_MAX;
}

static bool
tp_thumb_hw_finger(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	/* Size detection: reliable even at the edge of the touchpad; if size
	 * and shape confirm finger, return true
	 */
	if (tp->thumb.use_size &&
	    !((t->major > tp->thumb.size_threshold) &&
	    (t->minor < (tp->thumb.size_threshold * 0.6))))
		return true;

	/* Pressure detection: not reliable at the edges; only return true if
	 * the touch is above the lower_thumb_line
	 */
	if (tp->thumb.use_pressure &&
	    t->pressure <= tp->thumb.pressure_threshold &&
	    t->point.y < tp->thumb.lower_thumb_line)
		return true;

	/* Otherwise, we either have no hardware to confirm this is a finger,
	 * or hardware is saying we have a thumb.
	 */
	return false;
}

static bool
tp_thumb_needs_jail(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	if (t->point.y < tp->thumb.upper_thumb_line)
		return false;
	if (t->point.y < tp->thumb.lower_thumb_line &&
	    tp_thumb_hw_finger(tp, t))
		return false;
	if (t->speed.exceeded_count >= 10)
		return false;

	return true;
}

bool
tp_thumb_ignored(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	return (tp->thumb.detect_thumbs &&
		tp->thumb.index == t->index &&
		(tp->thumb.state == THUMB_STATE_JAILED ||
		 tp->thumb.state == THUMB_STATE_PINCH ||
		 tp->thumb.state == THUMB_STATE_SUPPRESSED ||
		 tp->thumb.state == THUMB_STATE_REV_JAILED ||
		 tp->thumb.state == THUMB_STATE_DEAD));
}

bool
tp_thumb_tap_ignored(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	return (tp->thumb.detect_thumbs &&
		tp->thumb.index == t->index &&
		(tp->thumb.state == THUMB_STATE_PINCH ||
		 tp->thumb.state == THUMB_STATE_SUPPRESSED ||
		 tp->thumb.state == THUMB_STATE_DEAD));
}

bool
tp_thumb_gesture_ignored(const struct tp_dispatch *tp, const struct tp_touch *t)
{
	return (tp->thumb.detect_thumbs &&
		tp->thumb.index == t->index &&
		(tp->thumb.state == THUMB_STATE_JAILED ||
		 tp->thumb.state == THUMB_STATE_SUPPRESSED ||
		 tp->thumb.state == THUMB_STATE_REV_JAILED ||
		 tp->thumb.state == THUMB_STATE_DEAD));
}

void
tp_thumb_suppress(struct tp_dispatch *tp, struct tp_touch *t)
{
	if(tp->thumb.state == THUMB_STATE_LIVE ||
	   tp->thumb.state == THUMB_STATE_JAILED ||
	   tp->thumb.state == THUMB_STATE_PINCH ||
	   tp->thumb.index != t->index) {
		tp_thumb_set_state(tp, t, THUMB_STATE_SUPPRESSED);
		return;
	}

	tp_thumb_set_state(tp, t, THUMB_STATE_DEAD);
}

static void
tp_thumb_pinch(struct tp_dispatch *tp, struct tp_touch *t)
{
	if(tp->thumb.state == THUMB_STATE_LIVE ||
	   tp->thumb.state == THUMB_STATE_JAILED ||
	   tp->thumb.index != t->index) // TODO: investigate this
		tp_thumb_set_state(tp, t, THUMB_STATE_PINCH);
	else
		tp_thumb_suppress(tp, t);
}

static void
tp_thumb_revive(struct tp_dispatch *tp, struct tp_touch *t)
{
	if((tp->thumb.state != THUMB_STATE_SUPPRESSED &&
	    tp->thumb.state != THUMB_STATE_PINCH) ||
	   (tp->thumb.index != t->index))
		return;

	if(tp_thumb_needs_jail(tp, t))
		tp_thumb_set_state(tp, t, THUMB_STATE_REV_JAILED);
	else
		tp_thumb_set_state(tp, t, THUMB_STATE_REVIVED);
}

void
tp_thumb_update(struct tp_dispatch *tp, struct tp_touch *t)
{
	if (!tp->thumb.detect_thumbs)
		return;
//	printf("Update: thumb index %d\n",tp->thumb.index);
	/* Once any active touch exceeds the speed threshold, don't
	 * try to detect pinches until all touches lift.
	 */
	if (t->speed.exceeded_count >= 10 &&
	    tp->thumb.pinch_eligible &&
	    tp->gesture.state == GESTURE_STATE_NONE) {
		tp->thumb.pinch_eligible = false;
		if(tp->thumb.state == THUMB_STATE_PINCH)
			tp->thumb.state = THUMB_STATE_SUPPRESSED; //TODO log
//		printf("Setting pinch ineligible\n");
	}

	/* Handle the thumb lifting off the touchpad */
	if (t->state == TOUCH_END && t->index == tp->thumb.index) {
//		printf("Lifting thumb\n");
		tp_thumb_lift(tp);
		return;
	}

	/* If this touch is not the only one, thumb updates happen by context
	 * instead of here
	 */
	if (tp->nfingers_down > 1)
		return;

	/* If we arrived here by other fingers lifting off, revive current touch
	 * if appropriate
	 */
	tp_thumb_revive(tp, t);

	/* First new touch below the lower_thumb_line, or below the upper_thumb_
	 * line if hardware can't verify it's a finger, starts as JAILED.
	 */
	if (t->state == TOUCH_BEGIN && tp_thumb_needs_jail(tp, t)) {
		tp_thumb_set_state(tp, t, THUMB_STATE_JAILED);
		return;
	}

	/* If a touch breaks the speed threshold, or leaves the thumb area
	 * (upper or lower, depending on HW detection), it "escapes" jail.
	 */
	if (tp->thumb.state == THUMB_STATE_JAILED &&
	    !(tp_thumb_needs_jail(tp, t)))
		tp_thumb_set_state(tp, t, THUMB_STATE_LIVE);
	if (tp->thumb.state == THUMB_STATE_REV_JAILED &&
	    !(tp_thumb_needs_jail(tp, t)))
		tp_thumb_set_state(tp, t, THUMB_STATE_REVIVED);
}

void
tp_thumb_update_by_context(struct tp_dispatch *tp)
{
	struct tp_touch *t;
	struct tp_touch *first = NULL,
			*second = NULL,
			*newest = NULL;
	struct device_coords distance;
	struct phys_coords mm;
	unsigned int speed_exceeded_count = 0;

//printf("Before: thumb index %d  ",tp->thumb.index);

	/* Get the first and second bottom-most touches, the max speed exceeded
	 * count overall, and the newest touch (or one of them, if more).
	 */
	tp_for_each_touch(tp, t) {
		if (t->state == TOUCH_NONE ||
		    t->state == TOUCH_HOVERING)
			continue;

		if (t->state == TOUCH_BEGIN)
			newest = t;

		speed_exceeded_count = max(speed_exceeded_count,
		                           t->speed.exceeded_count);

		if (!first) {
			first = t;
			continue;
		}

		if (t->point.y > first->point.y) {
			second = first;
			first = t;
			continue;
		}

		if (!second || t->point.y > second->point.y ) {
			second = t;
		}
	}

	if (!first || !second) {
//		printf("NoF/S: thumb index %d\n",tp->thumb.index);
		return;
	}

	assert(first);
	assert(second);

	distance.x = abs(first->point.x - second->point.x);
	distance.y = abs(first->point.y - second->point.y);
	mm = evdev_device_unit_delta_to_mm(tp->device, &distance);

	/* Speed-based thumb detection: if an existing touch is moving, and
	 * a new touch arrives, mark it as a thumb if it doesn't qualify as a
	 * 2-finger scroll.
	 */
	if (newest &&
	    tp->nfingers_down == 2 &&
	    speed_exceeded_count > 5 &&
	    (tp->scroll.method != LIBINPUT_CONFIG_SCROLL_2FG ||
	     (mm.x > SCROLL_MM_X && mm.y > SCROLL_MM_Y))) {
		evdev_log_debug(tp->device,
				"touch %d is speed-based thumb\n",
				newest->index);
		tp_thumb_suppress(tp, newest);
//		printf("Speed: thumb index %d\n",tp->thumb.index);
		return;
	}

	/* Position-based thumb detection: When a new touch arrives, check the
	 * two lowest touches. If they qualify for 2-finger scrolling, clear
	 * thumb status. If not, mark the lower touch (based on pinch_eligible)
	 * as either PINCH or SUPPRESSED.
	 */
	if (mm.y > SCROLL_MM_Y) {
		if (tp->thumb.pinch_eligible)
			tp_thumb_pinch(tp, first);
		else
			tp_thumb_suppress(tp, first);
	} else {
		tp_thumb_lift(tp);
	}

//	printf("Ending: thumb index %d\n",tp->thumb.index);
}

void
tp_init_thumb(struct tp_dispatch *tp)
{
	struct evdev_device *device = tp->device;
	double w = 0.0, h = 0.0;
	struct device_coords edges;
	struct phys_coords mm = { 0.0, 0.0 };
	uint32_t threshold;
	struct quirks_context *quirks;
	struct quirks *q;

	if (!tp->buttons.is_clickpad)
		return;

	/* if the touchpad is less than 50mm high, skip thumb detection.
	 * it's too small to meaningfully interact with a thumb on the
	 * touchpad */
	evdev_device_get_size(device, &w, &h);
	if (h < 50)
		return;

	tp->thumb.detect_thumbs = true;
	tp->thumb.use_pressure = false;
	tp->thumb.pressure_threshold = INT_MAX;

	/* detect thumbs by pressure in the bottom 15mm, detect thumbs by
	 * lingering in the bottom 8mm */
	mm.y = h * 0.85;
	edges = evdev_device_mm_to_units(device, &mm);
	tp->thumb.upper_thumb_line = edges.y;

	mm.y = h * 0.92;
	edges = evdev_device_mm_to_units(device, &mm);
	tp->thumb.lower_thumb_line = edges.y;

	quirks = evdev_libinput_context(device)->quirks;
	q = quirks_fetch_for_device(quirks, device->udev_device);

	if (libevdev_has_event_code(device->evdev, EV_ABS, ABS_MT_PRESSURE)) {
		if (quirks_get_uint32(q,
				      QUIRK_ATTR_THUMB_PRESSURE_THRESHOLD,
				      &threshold)) {
			tp->thumb.use_pressure = true;
			tp->thumb.pressure_threshold = threshold;
		}
	}

	if (libevdev_has_event_code(device->evdev, EV_ABS, ABS_MT_TOUCH_MAJOR)) {
		if (quirks_get_uint32(q,
				      QUIRK_ATTR_THUMB_SIZE_THRESHOLD,
				      &threshold)) {
			tp->thumb.use_size = true;
			tp->thumb.size_threshold = threshold;
		}
	}

	quirks_unref(q);

	tp_thumb_reset(tp);

	evdev_log_debug(device,
			"thumb: enabled thumb detection%s%s\n",
			tp->thumb.use_pressure ? " (+pressure)" : "",
			tp->thumb.use_size ? " (+size)" : "");
}
