/* Battery voltage -> percentage, portable for host tests. */
#ifndef BATTERY_LEVEL_H_
#define BATTERY_LEVEL_H_

#include <stddef.h>
#include <stdint.h>

struct battery_point {
	uint16_t mv;
	uint8_t pct;
};

/* Piecewise-linear lookup; `curve` must be sorted by descending mv. */
uint8_t battery_level_pct(uint16_t mv, const struct battery_point *curve, size_t count);

/* Approximate single-cell LiPo discharge curve at light load. */
extern const struct battery_point battery_curve_lipo[];
extern const size_t battery_curve_lipo_len;

#endif /* BATTERY_LEVEL_H_ */
