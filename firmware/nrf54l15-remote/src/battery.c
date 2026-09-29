/*
 * Battery level from the ADC channel in /zephyr,user:
 *
 *   zephyr,user {
 *       io-channels = <&adc N>;
 *       battery-divider-ratio = <2>;      optional, default 1
 *       battery-enable = <&regulator>;    optional divider power switch
 *   };
 *
 * Without io-channels the Battery Service keeps reporting 100 %.
 */
#include "battery.h"

#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery_level.h"

LOG_MODULE_REGISTER(battery, CONFIG_APP_LOG_LEVEL);

#define USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(USER_NODE, io_channels)

#include <zephyr/drivers/adc.h>

#define DIVIDER_RATIO DT_PROP_OR(USER_NODE, battery_divider_ratio, 1)
#define HAS_ENABLE    DT_NODE_HAS_PROP(USER_NODE, battery_enable)
/* Time for the divider and ADC input to settle after switching it on. */
#define SETTLE_MS     50

#if HAS_ENABLE
#include <zephyr/drivers/regulator.h>
static const struct device *const enable_dev = DEVICE_DT_GET(DT_PHANDLE(USER_NODE, battery_enable));
#endif

static const struct adc_dt_spec adc = ADC_DT_SPEC_GET_BY_IDX(USER_NODE, 0);
static bool adc_ok;

#if defined(CONFIG_APP_BATTERY_CURVE_LIPO)
#define CURVE     battery_curve_lipo
#define CURVE_LEN battery_curve_lipo_len
#else
static const struct battery_point linear_curve[] = {
	{CONFIG_APP_BATTERY_FULL_MV, 100},
	{CONFIG_APP_BATTERY_EMPTY_MV, 0},
};
#define CURVE     linear_curve
#define CURVE_LEN ARRAY_SIZE(linear_curve)
#endif

static void read_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(read_work, read_work_handler);
static void periodic_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(periodic_work, periodic_work_handler);

static void power_divider(bool on)
{
#if HAS_ENABLE
	int err = on ? regulator_enable(enable_dev) : regulator_disable(enable_dev);

	if (err) {
		LOG_WRN("battery divider %s failed: %d", on ? "enable" : "disable", err);
	}
#else
	ARG_UNUSED(on);
#endif
}

static void read_work_handler(struct k_work *work)
{
	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int err = adc_sequence_init_dt(&adc, &seq);

	if (!err) {
		err = adc_read_dt(&adc, &seq);
	}
	power_divider(false);
	if (err) {
		LOG_WRN("battery ADC read failed: %d", err);
		return;
	}

	int32_t mv = raw;

	err = adc_raw_to_millivolts_dt(&adc, &mv);
	if (err || mv < 0) {
		LOG_WRN("battery ADC conversion failed: %d", err);
		return;
	}
	mv *= DIVIDER_RATIO;

	uint8_t pct = battery_level_pct((uint16_t)MIN(mv, UINT16_MAX), CURVE, CURVE_LEN);

	LOG_INF("battery %d mV, %u%%", mv, pct);
	bt_bas_set_battery_level(pct);
}

static void periodic_work_handler(struct k_work *work)
{
	battery_measure();
	k_work_reschedule(&periodic_work, K_SECONDS(CONFIG_APP_BATTERY_INTERVAL_SECONDS));
}

int battery_init(void)
{
	if (!adc_is_ready_dt(&adc)) {
		LOG_ERR("battery ADC not ready");
		return -ENODEV;
	}
	int err = adc_channel_setup_dt(&adc);

	if (err) {
		LOG_ERR("battery ADC channel setup failed: %d", err);
		return err;
	}
	adc_ok = true;
	k_work_reschedule(&periodic_work, K_NO_WAIT);
	return 0;
}

void battery_measure(void)
{
	if (!adc_ok || k_work_delayable_is_pending(&read_work)) {
		return;
	}
	power_divider(true);
	k_work_reschedule(&read_work, K_MSEC(SETTLE_MS));
}

#else /* no battery ADC described */

int battery_init(void)
{
	LOG_INF("no battery ADC in devicetree; reporting a fixed level");
	return 0;
}

void battery_measure(void)
{
}

#endif
