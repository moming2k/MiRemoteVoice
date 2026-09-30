/*
 * Handling of the press that wakes the remote from System OFF, independent of
 * Zephyr so it can be unit-tested on the host.
 *
 * The press resets the SoC, so the remote is not connected yet. While it
 * reconnects the audio is recorded into a backlog; once the host is ready:
 *
 *   button still held         -> DELIVER_HOLD: normal Hold-to-Talk press, the
 *                                backlog is sent first, then live audio
 *   released, enough speech   -> DELIVER_REPLAY: press, send the backlog,
 *                                release
 *   released, only a tap      -> DISCARD: the tap just woke the remote
 *   host without Hold-to-Talk -> DELIVER_PRESS (held) or DISCARD (released)
 *
 * If the host is not ready within the timeout the backlog is discarded.
 */
#ifndef WAKE_H_
#define WAKE_H_

#include <stdbool.h>
#include <stdint.h>

enum wake_action {
	WAKE_NONE,
	WAKE_FREEZE,        /* stop recording, keep the backlog */
	WAKE_RESTART,       /* discard the backlog and record afresh */
	WAKE_DELIVER_HOLD,  /* press now; backlog then live audio */
	WAKE_DELIVER_REPLAY, /* press, backlog, release */
	WAKE_DELIVER_PRESS, /* discard audio, deliver a plain press */
	WAKE_DISCARD,       /* discard audio, deliver nothing */
};

enum wake_state {
	WAKE_IDLE,
	WAKE_HELD,
	WAKE_RELEASED,
};

struct wake {
	enum wake_state state;
	uint32_t min_replay_ms;
};

/* pressed_at_boot: the voice button was down when the firmware started. */
void wake_init(struct wake *w, bool pressed_at_boot, uint32_t min_replay_ms);
bool wake_pending(const struct wake *w);
/* Returns true if the event belongs to the wake press (then the normal state
 * machine must not see it); *action says what to do. */
bool wake_button(struct wake *w, bool pressed, enum wake_action *action);
/* The host is connected, encrypted, has sent GET_CAPS and subscribed. */
enum wake_action wake_ready(struct wake *w, bool hold_to_talk, uint32_t backlog_ms);
enum wake_action wake_timeout(struct wake *w);

#endif /* WAKE_H_ */
