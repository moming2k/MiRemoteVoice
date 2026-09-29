#include "battery_level.h"

const struct battery_point battery_curve_lipo[] = {
	{4200, 100}, {4100, 90}, {4000, 78}, {3900, 65}, {3800, 52}, {3750, 42},
	{3700, 32},  {3650, 22}, {3600, 14}, {3500, 6},  {3400, 2},  {3300, 0},
};
const size_t battery_curve_lipo_len = sizeof(battery_curve_lipo) / sizeof(battery_curve_lipo[0]);

uint8_t battery_level_pct(uint16_t mv, const struct battery_point *curve, size_t count)
{
	if (count == 0) {
		return 0;
	}
	if (mv >= curve[0].mv) {
		return curve[0].pct;
	}
	for (size_t i = 1; i < count; i++) {
		if (mv >= curve[i].mv) {
			const struct battery_point *hi = &curve[i - 1];
			const struct battery_point *lo = &curve[i];
			uint32_t span_mv = hi->mv - lo->mv;
			uint32_t span_pct = hi->pct - lo->pct;

			return (uint8_t)(lo->pct + (span_mv ? (mv - lo->mv) * span_pct / span_mv : 0));
		}
	}
	return curve[count - 1].pct;
}
