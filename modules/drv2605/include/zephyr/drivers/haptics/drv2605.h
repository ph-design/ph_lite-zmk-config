/*
 * Copyright (c) 2024 Cirrus Logic, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_HAPTICS_DRV2605_H_
#define ZEPHYR_INCLUDE_DRIVERS_HAPTICS_DRV2605_H_

#include <zephyr/drivers/haptics.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRV2605_WAVEFORM_SEQUENCER_MAX 8

enum drv2605_library {
	DRV2605_LIBRARY_EMPTY = 0,
	DRV2605_LIBRARY_TS2200_A,
	DRV2605_LIBRARY_TS2200_B,
	DRV2605_LIBRARY_TS2200_C,
	DRV2605_LIBRARY_TS2200_D,
	DRV2605_LIBRARY_TS2200_E,
	DRV2605_LIBRARY_LRA,
};

enum drv2605_mode {
	DRV2605_MODE_INTERNAL_TRIGGER = 0,
	DRV2605_MODE_EXTERNAL_EDGE_TRIGGER,
	DRV2605_MODE_EXTERNAL_LEVEL_TRIGGER,
	DRV2605_MODE_PWM_ANALOG_INPUT,
	DRV2605_MODE_AUDIO_TO_VIBE,
	DRV2605_MODE_RTP,
	DRV2605_MODE_DIAGNOSTICS,
	DRV2605_MODE_AUTO_CAL,
};

/**
 * @brief DRV2605 haptic driver signal sources
 */
enum drv2605_haptics_source {
	/** The playback source is device ROM */
	DRV2605_HAPTICS_SOURCE_ROM,
	/** The playback source is the RTP buffer */
	DRV2605_HAPTICS_SOURCE_RTP,
	/** The playback source is audio */
	DRV2605_HAPTICS_SOURCE_AUDIO,
	/** The playback source is a PWM signal */
	DRV2605_HAPTICS_SOURCE_PWM,
	/** The playback source is an analog signal */
	DRV2605_HAPTICS_SOURCE_ANALOG,
};

struct drv2605_rom_data {
	enum drv2605_mode trigger;
	enum drv2605_library library;
	uint8_t seq_regs[DRV2605_WAVEFORM_SEQUENCER_MAX];
	uint8_t overdrive_time;
	uint8_t sustain_pos_time;
	uint8_t sustain_neg_time;
	uint8_t brake_time;
};

// RTP 输入格式（CONTROL3.DATA_FORMAT_RTP）。signed 默认：0x00=零、0x7f=+满、负值=刹车
enum drv2605_rtp_format {
	DRV2605_RTP_SIGNED = 0,
	DRV2605_RTP_UNSIGNED = 1,
};

struct drv2605_rtp_data {
	size_t size;
	uint32_t *rtp_hold_us;
	uint8_t *rtp_input;
	enum drv2605_rtp_format format;
};

union drv2605_config_data {
	struct drv2605_rom_data *rom_data;
	struct drv2605_rtp_data *rtp_data;
};

/**
 * @brief Configure the DRV2605 device for a particular signal source
 *
 * @param dev Pointer to the device structure for haptic device instance
 * @param source The type of haptic signal source desired
 * @param config_data Pointer to the configuration data union for the source
 *
 * @retval 0 if successful
 * @retval -ENOTSUP if the signal source is not supported
 * @retval <0 if failed
 */
int drv2605_haptic_config(const struct device *dev, enum drv2605_haptics_source source,
			  const union drv2605_config_data *config_data);

/* Play one effect after configuring internal-trigger ROM mode and clearing the other slots. */
int drv2605_play_rom_effect(const struct device *dev, uint8_t effect);
/* Write an input sample in the active RTP format after configuring RTP mode. */
int drv2605_rtp_set_input(const struct device *dev, uint8_t input);
/* End host-timed RTP and enter standby in a single transaction. */
int drv2605_rtp_finish(const struct device *dev);
/* Return 1 while GO is set, 0 when the operation is complete, or a negative I2C error. */
int drv2605_go_active(const struct device *dev);

// 进入校准/诊断模式并触发 GO（非阻塞；GO 自清后结果才有效，见数据手册 Table 4）
int drv2605_calibrate(const struct device *dev);
/* Wait for auto-calibration. Returns -ERANGE for nonconvergence without protection faults. */
int drv2605_calibrate_and_wait(const struct device *dev);
/* Enable fixed-frequency LRA playback at reduced output after a failed calibration. */
int drv2605_enable_lra_fallback(const struct device *dev, uint16_t peak_mv);
/* Select the LRA control mode, peak limit, and amplitude update cadence. */
int drv2605_set_lra_drive(const struct device *dev, bool open_loop, uint16_t peak_mv,
			  bool half_cycle_updates);
int drv2605_diagnose(const struct device *dev);
// 芯片侧初始化（探测/软复位/DT 配置）。有界 I2C，必须在设备 init 之后由线程调用
int drv2605_hw_setup(const struct device *dev);
/* Enter or leave standby while preserving the selected playback configuration. */
int drv2605_set_standby(const struct device *dev, bool standby);
// STATUS 只读一次（diag/over_temp 等标志读后即清）
int drv2605_read_status(const struct device *dev, uint8_t *status);
// 回到内部触发并按 DT 重新下发配置（软复位后调用）
int drv2605_restore_config(const struct device *dev);

// 数据手册 8.5.2 式(4)-(9) 的电压换算上下文
struct drv2605_voltage_params {
	bool lra;
	bool open_loop;
	uint16_t f_lra_hz; // 0 = 未知，跳过频率修正项
	uint16_t sample_us;
	uint16_t drive_us;
	uint16_t idiss_us;
	uint16_t blank_us;
};

uint8_t drv2605_calc_rated_reg(uint16_t mv, const struct drv2605_voltage_params *p);
uint8_t drv2605_calc_clamp_reg(uint16_t mv, const struct drv2605_voltage_params *p);
uint16_t drv2605_decode_rated_mv(uint8_t reg, const struct drv2605_voltage_params *p);
uint16_t drv2605_decode_clamp_mv(uint8_t reg, const struct drv2605_voltage_params *p);

// CONTROL1/2/5 时间字段 -> µs（数据手册 Table 24/25）
static inline uint16_t drv2605_sample_time_us(uint8_t control2)
{
	return (uint16_t)(150U + 50U * ((control2 >> 4) & 0x3U));
}

static inline uint16_t drv2605_drive_time_us(bool lra, uint8_t control1)
{
	uint8_t n = control1 & 0x1fU;

	return lra ? (uint16_t)(n * 100U + 500U) : (uint16_t)(n * 200U + 1000U);
}

static inline uint16_t drv2605_x_time_us(bool lra, uint8_t hi2, uint8_t lo2)
{
	// LRA 的 BLANKING/IDISS 是 4 位字段（高 2 位在 CONTROL5），ERM 是 2 位
	static const uint16_t erm_tab[4] = {45, 75, 150, 225};
	static const uint16_t lra_tab[16] = {15, 25, 50, 75,  90,  105, 120, 135,
					     150, 165, 180, 195, 210, 235, 260, 285};

	if (!lra) {
		return erm_tab[lo2 & 0x3U];
	}
	return lra_tab[((hi2 & 0x3U) << 2) | (lo2 & 0x3U)];
}

static inline uint16_t drv2605_idiss_time_us(bool lra, uint8_t control2, uint8_t control5)
{
	return drv2605_x_time_us(lra, (uint8_t)(control5 & 0x3U), (uint8_t)(control2 & 0x3U));
}

static inline uint16_t drv2605_blank_time_us(bool lra, uint8_t control2, uint8_t control5)
{
	return drv2605_x_time_us(lra, (uint8_t)((control5 >> 2) & 0x3U),
				 (uint8_t)((control2 >> 2) & 0x3U));
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
