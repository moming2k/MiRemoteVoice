/* 16 kHz mono capture (PDM mic or test tone) -> ADPCM -> ATVV notifications. */
#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdint.h>

#include <stdbool.h>

/* Called on the audio thread once a released backlog has been sent;
 * `recording` says whether live audio follows. */
typedef void (*audio_backlog_sent_cb_t)(bool recording);

int audio_init(audio_backlog_sent_cb_t on_backlog_sent);
/* Begin a new stream of `frame_size`-byte notifications; AUDIO_START must
 * already have been sent. */
void audio_start(uint8_t codec, uint16_t frame_size);
/* Stop recording/streaming and discard any backlog. */
void audio_stop(void);

/*
 * Wake-press capture (CONFIG_APP_WAKE_CAPTURE): record into a backlog before
 * the host is connected, then send it once AUDIO_START has gone out.
 */
void audio_prebuffer_start(void);
/* Button released before the host was ready: stop recording, keep backlog. */
void audio_prebuffer_freeze(void);
/* After `delay_ms`, send the backlog, then continue live if still
 * recording; the backlog-sent callback runs either way. A freeze before the
 * delay ends turns this into a replay of what was recorded. */
void audio_prebuffer_release(uint32_t delay_ms);
uint32_t audio_backlog_ms(void);

#endif /* AUDIO_H_ */
