#include "wake.h"

void wake_init(struct wake *w, bool pressed_at_boot, uint32_t min_replay_ms)
{
	w->state = pressed_at_boot ? WAKE_HELD : WAKE_IDLE;
	w->min_replay_ms = min_replay_ms;
}

bool wake_pending(const struct wake *w)
{
	return w->state != WAKE_IDLE;
}

bool wake_button(struct wake *w, bool pressed, enum wake_action *action)
{
	*action = WAKE_NONE;

	switch (w->state) {
	case WAKE_HELD:
		if (!pressed) {
			w->state = WAKE_RELEASED;
			*action = WAKE_FREEZE;
		}
		return true;
	case WAKE_RELEASED:
		if (pressed) {
			/* Tap to wake, then hold to talk while still reconnecting:
			 * the new hold is what the user wants delivered. */
			w->state = WAKE_HELD;
			*action = WAKE_RESTART;
		}
		return true;
	case WAKE_IDLE:
	default:
		return false;
	}
}

enum wake_action wake_ready(struct wake *w, bool hold_to_talk, uint32_t backlog_ms)
{
	enum wake_action action = WAKE_NONE;

	switch (w->state) {
	case WAKE_HELD:
		action = hold_to_talk ? WAKE_DELIVER_HOLD : WAKE_DELIVER_PRESS;
		break;
	case WAKE_RELEASED:
		action = (hold_to_talk && backlog_ms >= w->min_replay_ms) ? WAKE_DELIVER_REPLAY
									  : WAKE_DISCARD;
		break;
	case WAKE_IDLE:
	default:
		break;
	}
	w->state = WAKE_IDLE;
	return action;
}

enum wake_action wake_timeout(struct wake *w)
{
	if (w->state == WAKE_IDLE) {
		return WAKE_NONE;
	}
	w->state = WAKE_IDLE;
	return WAKE_DISCARD;
}
