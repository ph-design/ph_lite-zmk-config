/*
 * Copyright 2024 Cirrus Logic, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DRV2605 Datasheet: https://www.ti.com/lit/gpn/drv2605
 */

#define DT_DRV_COMPAT ti_drv2605

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/haptics.h>
#include <zephyr/drivers/haptics/drv2605.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(DRV2605, CONFIG_HAPTICS_LOG_LEVEL);

#define DRV2605_REG_STATUS         0x0
#define DRV2605_DEVICE_ID          GENMASK(7, 5)
#define DRV2605_DEVICE_ID_DRV2605  0x3
#define DRV2605_DEVICE_ID_DRV2605L 0x7
#define DRV2605_DIAG_RESULT        BIT(3)
#define DRV2605_FB_STS             BIT(2)
#define DRV2605_OVER_TEMP          BIT(1)
#define DRV2605_OC_DETECT          BIT(0)

#define DRV2605_REG_MODE  0x1
#define DRV2605_DEV_RESET BIT(7)
#define DRV2605_STANDBY   BIT(6)
#define DRV2605_MODE      GENMASK(2, 0)

#define DRV2605_REG_RT_PLAYBACK_INPUT 0x2

#define DRV2605_REG_UNNAMED 0x3
#define DRV2605_HI_Z_OUTPUT BIT(4)
#define DRV2605_LIBRARY_SEL GENMASK(2, 0)

#define DRV2605_REG_WAVEFORM_SEQUENCER 0x4
#define DRV2605_WAIT                   BIT(7)
#define DRV2605_WAV_FRM_SEQ            GENMASK(6, 0)

#define DRV2605_REG_GO 0xc
#define DRV2605_GO     BIT(0)

#define DRV2605_REG_OVERDRIVE_TIME_OFFSET 0xd

#define DRV2605_REG_SUSTAIN_TIME_OFFSET_POS 0xe

#define DRV2605_REG_SUSTAIN_TIME_OFFSET_NEG 0xf

#define DRV2605_REG_BRAKE_TIME_OFFSET 0x10

#define DRV2605_TIME_STEP_MS 5

#define DRV2605_REG_AUDIO_TO_VIBE_CONTROL 0x11
#define DRV2605_ATH_PEAK_TIME             GENMASK(3, 2)
#define DRV2605_ATH_FILTER                GENMASK(1, 0)

#define DRV2605_REG_AUDIO_TO_VIBE_MIN_INPUT_LEVEL 0x12

#define DRV2605_REG_AUDIO_TO_VIBE_MAX_INPUT_LEVEL 0x13

#define DRV2605_ATH_INPUT_STEP_UV (1800000 / 255)

#define DRV2605_REG_AUDIO_TO_VIBE_MIN_OUTPUT_DRIVE 0x14

#define DRV2605_REG_AUDIO_TO_VIBE_MAX_OUTPUT_DRIVE 0x15

#define DRV2605_ATH_OUTPUT_DRIVE_PCT (100 * 255)

#define DRV2605_REG_RATED_VOLTAGE 0x16

#define DRV2605_REG_OVERDRIVE_CLAMP_VOLTAGE 0x17

#define DRV2605_REG_AUTO_CAL_COMP_RESULT 0x18

#define DRV2605_REG_AUTO_CAL_BACK_EMF_RESULT 0x19

#define DRV2605_REG_FEEDBACK_CONTROL 0x1a
#define DRV2605_N_ERM_LRA            BIT(7)
#define DRV2605_FB_BRAKE_FACTOR      GENMASK(6, 4)
#define DRV2605_LOOP_GAIN            GENMASK(3, 2)
#define DRV2605_BEMF_GAIN            GENMASK(1, 0)

#define DRV2605_ACTUATOR_MODE_ERM 0
#define DRV2605_ACTUATOR_MODE_LRA 1

#define DRV2605_REG_CONTROL1  0x1b
#define DRV2605_STARTUP_BOOST BIT(7)
#define DRV2605_AC_COUPLE     BIT(5)
#define DRV2605_DRIVE_TIME    GENMASK(4, 0)

#define DRV2605_REG_CONTROL2     0x1c
#define DRV2605_BIDIR_INPUT      BIT(7)
#define DRV2605_BRAKE_STABILIZER BIT(6)
#define DRV2605_SAMPLE_TIME      GENMASK(5, 4)
#define DRV2605_BLANKING_TIME    GENMASK(3, 2)
#define DRV2605_IDISS_TIME       GENMASK(1, 0)

#define DRV2605_REG_CONTROL3    0x1d
#define DRV2605_NG_THRESH       GENMASK(7, 6)
#define DRV2605_ERM_OPEN_LOOP   BIT(5)
#define DRV2605_SUPPLY_COMP_DIS BIT(4)
#define DRV2605_DATA_FORMAT_RTP BIT(3)
#define DRV2605_LRA_DRIVE_MODE  BIT(2)
#define DRV2605_N_PWM_ANALOG    BIT(1)
#define DRV2605_LRA_OPEN_LOOP   BIT(0)

#define DRV2605_REG_CONTROL4       0x1e
#define DRV2605_ZERO_CROSSING_TIME GENMASK(7, 6)
#define DRV2605_AUTO_CAL_TIME      GENMASK(5, 4)
#define DRV2605_OTP_STATUS         BIT(2)
#define DRV2605_OTP_PROGRAM        BIT(0)

#define DRV2605_REG_BATT_VOLTAGE_MONITOR 0x21
#define DRV2605_VBAT_STEP_UV             (5600000 / 255)

#define DRV2605_REG_LRA_RESONANCE_PERIOD 0x22

#define DRV2605_POWER_UP_DELAY_US 250

#define DRV2605_REG_CONTROL5            0x1f
#define DRV2605_REG_LRA_OPEN_LOOP_PERIOD 0x20

#define DRV2605_DRIVE_TIME_DEFAULT 0x13 /* 芯片默认（Table 24） */

struct drv2605_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec en_gpio;
	struct gpio_dt_spec in_trig_gpio;
	uint8_t feedback_brake_factor;
	uint8_t loop_gain;
	uint16_t vib_rated_mv;
	uint16_t vib_overdrive_mv;
	uint16_t vib_frequency_hz;
	uint8_t auto_cal_time;
	uint8_t drive_time;
	uint8_t blanking_time;
	uint8_t idiss_time;
	bool actuator_mode;
	bool erm_open_loop;
	bool lra_open_loop;
};

struct drv2605_data {
	const struct device *dev;
	struct k_work rtp_work;
	atomic_t rtp_abort;
	atomic_t configured;
	atomic_t standby;
	const struct drv2605_rtp_data *rtp_data;
	enum drv2605_mode mode;
};

// 数据手册 8.5.2 式(4)-(9) 的定点换算，电压单位 mV
static uint32_t drv2605_isqrt(uint64_t v)
{
	uint64_t res = 0;
	uint64_t bit = (uint64_t)1 << 62;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit != 0) {
		if (v >= res + bit) {
			v -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}

	return (uint32_t)res;
}

// sqrt(1 - num/den)，Q10 定点
static uint32_t drv2605_sqrt1m_q10(uint32_t num, uint32_t den)
{
	if (num >= den) {
		return 1;
	}

	return drv2605_isqrt(((uint64_t)(den - num) << 20) / den);
}

uint8_t drv2605_calc_rated_reg(uint16_t mv, const struct drv2605_voltage_params *p)
{
	uint32_t reg;

	if (p->lra) {
		uint32_t corr_q10 = 1024;

		if (p->f_lra_hz != 0) {
			uint32_t t_us = 4 * p->sample_us + 300;

			corr_q10 = drv2605_sqrt1m_q10(t_us * p->f_lra_hz, 1000000U);
		}
		// 式(5): V(RMS) = 20.58mV * RATED / sqrt(1 - (4*tSAMPLE+300us) * f)
		reg = DIV_ROUND_CLOSEST((uint64_t)mv * 1000U * corr_q10, 20580U * 1024U);
	} else {
		// 式(4): V(AV) = 21.18mV * RATED
		reg = DIV_ROUND_CLOSEST((uint32_t)mv * 1000U, 21180U);
	}

	return (uint8_t)MIN(reg, 255U);
}

uint8_t drv2605_calc_clamp_reg(uint16_t mv, const struct drv2605_voltage_params *p)
{
	uint32_t reg;

	if (p->open_loop) {
		if (p->lra) {
			uint32_t corr_q10 = 1024;

			if (p->f_lra_hz != 0) {
				corr_q10 = drv2605_sqrt1m_q10((uint32_t)p->f_lra_hz * 800U, 1000000U);
			}
			// 式(7): V(RMS) = 21.32mV * OD_CLAMP * sqrt(1 - f*800us)
			reg = DIV_ROUND_CLOSEST((uint64_t)mv * 1000U * 1024U, 21320U * (uint64_t)corr_q10);
		} else {
			// 式(6): V(AV) = 21.59mV * OD_CLAMP
			reg = DIV_ROUND_CLOSEST((uint32_t)mv * 1000U, 21590U);
		}
	} else if (p->lra) {
		// 式(9): 峰值 = 21.22mV * OD_CLAMP
		reg = DIV_ROUND_CLOSEST((uint32_t)mv * 1000U, 21220U);
	} else {
		uint32_t td = p->drive_us > 300 ? p->drive_us - 300 : 1;
		uint32_t sum = p->drive_us + p->idiss_us + p->blank_us;

		// 式(8): 峰值 = 21.64mV * OD_CLAMP * (tD-300us) / (tD+tI+tB)
		reg = DIV_ROUND_CLOSEST((uint64_t)mv * 1000U * sum, 21640U * (uint64_t)td);
	}

	return (uint8_t)MIN(reg, 255U);
}

uint16_t drv2605_decode_rated_mv(uint8_t reg, const struct drv2605_voltage_params *p)
{
	if (p->lra) {
		uint32_t corr_q10 = 1024;

		if (p->f_lra_hz != 0) {
			uint32_t t_us = 4 * p->sample_us + 300;

			corr_q10 = drv2605_sqrt1m_q10(t_us * p->f_lra_hz, 1000000U);
		}

		return (uint16_t)DIV_ROUND_CLOSEST((uint64_t)reg * 20580U * 1024U,
						   1000U * (uint64_t)corr_q10);
	}

	return (uint16_t)DIV_ROUND_CLOSEST((uint32_t)reg * 21180U, 1000U);
}

uint16_t drv2605_decode_clamp_mv(uint8_t reg, const struct drv2605_voltage_params *p)
{
	if (p->open_loop) {
		if (p->lra) {
			uint32_t corr_q10 = 1024;

			if (p->f_lra_hz != 0) {
				corr_q10 = drv2605_sqrt1m_q10((uint32_t)p->f_lra_hz * 800U, 1000000U);
			}

			return (uint16_t)DIV_ROUND_CLOSEST((uint64_t)reg * 21320U * (uint64_t)corr_q10,
							   1000U * 1024U);
		}

		return (uint16_t)DIV_ROUND_CLOSEST((uint32_t)reg * 21590U, 1000U);
	}

	if (p->lra) {
		return (uint16_t)DIV_ROUND_CLOSEST((uint32_t)reg * 21220U, 1000U);
	} else {
		uint32_t td = p->drive_us > 300 ? p->drive_us - 300 : 1;
		uint32_t sum = p->drive_us + p->idiss_us + p->blank_us;

		return (uint16_t)DIV_ROUND_CLOSEST((uint64_t)reg * 21640U * (uint64_t)td,
						   1000U * sum);
	}
}

static inline int drv2605_haptic_config_audio(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;
	int ret;

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL3, DRV2605_N_PWM_ANALOG,
				     DRV2605_N_PWM_ANALOG);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL1, DRV2605_AC_COUPLE,
				     DRV2605_AC_COUPLE);
	if (ret < 0) {
		return ret;
	}

	data->mode = DRV2605_MODE_AUDIO_TO_VIBE;

	return 0;
}

static inline int drv2605_haptic_config_pwm_analog(const struct device *dev, const bool analog)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;
	uint8_t value = 0;
	int ret;

	// N_PWM_ANALOG: 0=PWM 输入、1=模拟输入（Table 26 + 8.5.8.2；Table 2 两行印反，TI 勘误）
	if (analog) {
		value = DRV2605_N_PWM_ANALOG;
	}

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL3, DRV2605_N_PWM_ANALOG,
				     value);
	if (ret < 0) {
		return ret;
	}

	data->mode = DRV2605_MODE_PWM_ANALOG_INPUT;

	return 0;
}

int drv2605_rtp_set_input(const struct device *dev, uint8_t input)
{
	const struct drv2605_config *config = dev->config;
	const struct drv2605_data *data = dev->data;

	if (data->mode != DRV2605_MODE_RTP) {
		return -EINVAL;
	}

	return i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_RT_PLAYBACK_INPUT, input);
}

int drv2605_rtp_finish(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;

	if (data->mode != DRV2605_MODE_RTP) {
		return -EINVAL;
	}

	/* Internal trigger is mode 0. Entering standby also ends any RTP drive. */
	int ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_STANDBY);
	if (!ret) {
		data->mode = DRV2605_MODE_INTERNAL_TRIGGER;
		atomic_set(&data->standby, true);
	}

	return ret;
}

static void drv2605_rtp_work_handler(struct k_work *work)
{
	struct drv2605_data *data = CONTAINER_OF(work, struct drv2605_data, rtp_work);
	const struct drv2605_rtp_data *rtp_data = data->rtp_data;
	const struct drv2605_config *config = data->dev->config;
	int i;

	for (i = 0; i < rtp_data->size; i++) {
		if (atomic_get(&data->rtp_abort)) {
			break;
		}

		i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_RT_PLAYBACK_INPUT,
				      rtp_data->rtp_input[i]);
		k_usleep(rtp_data->rtp_hold_us[i]);
	}
}

static inline int drv2605_haptic_config_rtp(const struct device *dev,
					    const struct drv2605_rtp_data *rtp_data)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;
	int ret;

	data->rtp_data = rtp_data;
	atomic_clear(&data->rtp_abort);

	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_RT_PLAYBACK_INPUT, 0);
	if (ret < 0) {
		return ret;
	}

	// DATA_FORMAT_RTP: 0=signed(默认) 1=unsigned（Table 26）
	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL3, DRV2605_DATA_FORMAT_RTP,
				     rtp_data->format == DRV2605_RTP_UNSIGNED
					     ? DRV2605_DATA_FORMAT_RTP
					     : 0);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_MODE,
				     (uint8_t)DRV2605_MODE_RTP);
	if (ret < 0) {
		return ret;
	}

	data->mode = DRV2605_MODE_RTP;

	return 0;
}

static inline int drv2605_haptic_config_rom(const struct device *dev,
					    const struct drv2605_rom_data *rom_data)
{
	const struct drv2605_config *config = dev->config;
	uint8_t reg_addr = DRV2605_REG_WAVEFORM_SEQUENCER;
	struct drv2605_data *data = dev->data;
	int i, ret;

	switch (rom_data->trigger) {
	case DRV2605_MODE_INTERNAL_TRIGGER:
	case DRV2605_MODE_EXTERNAL_EDGE_TRIGGER:
	case DRV2605_MODE_EXTERNAL_LEVEL_TRIGGER:
		ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_MODE,
					     (uint8_t)rom_data->trigger);
		if (ret < 0) {
			return ret;
		}

		data->mode = rom_data->trigger;
		break;
	default:
		return -EINVAL;
	}

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_UNNAMED, DRV2605_LIBRARY_SEL,
				     (uint8_t)rom_data->library);
	if (ret < 0) {
		return ret;
	}

	// 全部 8 槽都写（未用的槽写 0），否则上一次播放的残留序列会续播
	for (i = 0; i < DRV2605_WAVEFORM_SEQUENCER_MAX; i++) {
		ret = i2c_reg_write_byte_dt(&config->i2c, reg_addr, rom_data->seq_regs[i]);
		if (ret < 0) {
			return ret;
		}

		reg_addr++;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_OVERDRIVE_TIME_OFFSET,
				    rom_data->overdrive_time);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_SUSTAIN_TIME_OFFSET_POS,
				    rom_data->sustain_pos_time);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_SUSTAIN_TIME_OFFSET_NEG,
				    rom_data->sustain_neg_time);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_BRAKE_TIME_OFFSET,
				    rom_data->brake_time);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

int drv2605_haptic_config(const struct device *dev, enum drv2605_haptics_source source,
			  const union drv2605_config_data *config_data)
{
	switch (source) {
	case DRV2605_HAPTICS_SOURCE_ROM:
		return drv2605_haptic_config_rom(dev, config_data->rom_data);
	case DRV2605_HAPTICS_SOURCE_RTP:
		return drv2605_haptic_config_rtp(dev, config_data->rtp_data);
	case DRV2605_HAPTICS_SOURCE_AUDIO:
		return drv2605_haptic_config_audio(dev);
	case DRV2605_HAPTICS_SOURCE_PWM:
		return drv2605_haptic_config_pwm_analog(dev, false);
	case DRV2605_HAPTICS_SOURCE_ANALOG:
		return drv2605_haptic_config_pwm_analog(dev, true);
	default:
		return -ENOTSUP;
	}

	return 0;
}

static inline int drv2605_edge_mode_event(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	int ret;

	ret = gpio_pin_set_dt(&config->in_trig_gpio, 1);
	if (ret < 0) {
		return ret;
	}

	return gpio_pin_set_dt(&config->in_trig_gpio, 0);
}

static int drv2605_stop_output(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;
	uint8_t value;
	int ret;

	switch (data->mode) {
	case DRV2605_MODE_DIAGNOSTICS:
	case DRV2605_MODE_AUTO_CAL:
		ret = i2c_reg_read_byte_dt(&config->i2c, DRV2605_REG_GO, &value);
		if (ret < 0) {
			return ret;
		}

		if (FIELD_GET(DRV2605_GO, value)) {
			LOG_DBG("Playback mode: %d is uninterruptible", data->mode);
			return -EBUSY;
		}

		break;
	case DRV2605_MODE_INTERNAL_TRIGGER:
		return i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_GO, DRV2605_GO, 0);
	case DRV2605_MODE_EXTERNAL_EDGE_TRIGGER:
		return drv2605_edge_mode_event(dev);
	case DRV2605_MODE_EXTERNAL_LEVEL_TRIGGER:
		return gpio_pin_set_dt(&config->in_trig_gpio, 0);
	case DRV2605_MODE_PWM_ANALOG_INPUT:
	case DRV2605_MODE_AUDIO_TO_VIBE:
		ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_MODE,
					     (uint8_t)DRV2605_MODE_INTERNAL_TRIGGER);
		if (ret < 0) {
			return ret;
		}

		return i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_GO, DRV2605_GO, 0);
	case DRV2605_MODE_RTP: {
		struct k_work_sync sync;

		// k_work_cancel 对运行中的 work 无效：先置中止标志让循环退出，
		// 再清 RTP 输入并退出 RTP 模式（手册：0x02=0/退出模式才停止驱动）
		atomic_set(&data->rtp_abort, 1);
		k_work_cancel_sync(&data->rtp_work, &sync);
		ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_RT_PLAYBACK_INPUT, 0);
		if (ret < 0) {
			return ret;
		}

		ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_MODE,
					     (uint8_t)DRV2605_MODE_INTERNAL_TRIGGER);
		if (ret < 0) {
			return ret;
		}

		data->mode = DRV2605_MODE_INTERNAL_TRIGGER;

		return 0;
	}
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int drv2605_start_output(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;

	switch (data->mode) {
	case DRV2605_MODE_DIAGNOSTICS:
	case DRV2605_MODE_AUTO_CAL:
	case DRV2605_MODE_INTERNAL_TRIGGER:
		return i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_GO, DRV2605_GO, DRV2605_GO);
	case DRV2605_MODE_EXTERNAL_EDGE_TRIGGER:
		return drv2605_edge_mode_event(dev);
	case DRV2605_MODE_EXTERNAL_LEVEL_TRIGGER:
		return gpio_pin_set_dt(&config->in_trig_gpio, 1);
	case DRV2605_MODE_AUDIO_TO_VIBE:
	case DRV2605_MODE_PWM_ANALOG_INPUT:
		return i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_MODE,
					      (uint8_t)data->mode);
	case DRV2605_MODE_RTP:
		return k_work_submit(&data->rtp_work);
	default:
		return -ENOTSUP;
	}

	return 0;
}

int drv2605_play_rom_effect(const struct device *dev, uint8_t effect)
{
	const struct drv2605_config *config = dev->config;
	const struct drv2605_data *data = dev->data;

	if (data->mode != DRV2605_MODE_INTERNAL_TRIGGER || effect == 0 ||
	    effect > DRV2605_WAV_FRM_SEQ) {
		return -EINVAL;
	}

	int ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_WAVEFORM_SEQUENCER, effect);

	return ret < 0 ? ret : drv2605_start_output(dev);
}

int drv2605_go_active(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	uint8_t go;
	int ret = i2c_reg_read_byte_dt(&config->i2c, DRV2605_REG_GO, &go);

	return ret < 0 ? ret : !!(go & DRV2605_GO);
}

static int drv2605_write_standby(const struct device *dev, bool standby)
{
	const struct drv2605_config *config = dev->config;

	return i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_STANDBY,
				      standby ? DRV2605_STANDBY : 0);
}

int drv2605_set_standby(const struct device *dev, bool standby)
{
	struct drv2605_data *data = dev->data;
	int ret = drv2605_write_standby(dev, standby);

	if (!ret) {
		atomic_set(&data->standby, standby);
	}

	return ret;
}

#ifdef CONFIG_PM_DEVICE
static int drv2605_pm_action(const struct device *dev, enum pm_device_action action)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
	case PM_DEVICE_ACTION_SUSPEND:
		/* Leave devices that are already idle or awaiting first use alone. */
		if (!atomic_get(&data->configured) || atomic_get(&data->standby)) {
			return 0;
		}

		return drv2605_write_standby(dev, action == PM_DEVICE_ACTION_SUSPEND);
	case PM_DEVICE_ACTION_TURN_OFF:
		if (config->en_gpio.port != NULL) {
			gpio_pin_set_dt(&config->en_gpio, 0);
		}

		break;
	case PM_DEVICE_ACTION_TURN_ON:
		if (config->en_gpio.port != NULL) {
			gpio_pin_set_dt(&config->en_gpio, 1);
		}

		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}
#endif

static int drv2605_hw_config(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_voltage_params vp = {0};
	uint8_t c2, c5, drive_raw;
	uint8_t mask, value;
	int ret;

	// 时间参数按芯片当前实际值算电压公式（CONTROL2/5 复位默认值即手册推荐值）
	ret = i2c_reg_read_byte_dt(&config->i2c, DRV2605_REG_CONTROL2, &c2);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_read_byte_dt(&config->i2c, DRV2605_REG_CONTROL5, &c5);
	if (ret < 0) {
		return ret;
	}

	if (config->actuator_mode == DRV2605_ACTUATOR_MODE_LRA) {
		// CONTROL2/5 分别保存 LRA 时间字段的低/高两位
		mask = DRV2605_BLANKING_TIME | DRV2605_IDISS_TIME;
		value = FIELD_PREP(DRV2605_BLANKING_TIME, config->blanking_time & 0x3) |
			FIELD_PREP(DRV2605_IDISS_TIME, config->idiss_time & 0x3);
		ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL2, mask, value);
		if (ret < 0) {
			return ret;
		}

		value = FIELD_PREP(DRV2605_BLANKING_TIME, config->blanking_time >> 2) |
			FIELD_PREP(DRV2605_IDISS_TIME, config->idiss_time >> 2);
		ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL5, mask, value);
		if (ret < 0) {
			return ret;
		}
	}

	drive_raw = config->drive_time;
	if (drive_raw == 0) {
		drive_raw = DRV2605_DRIVE_TIME_DEFAULT;
		if (config->actuator_mode == DRV2605_ACTUATOR_MODE_LRA &&
		    config->vib_frequency_hz != 0) {
			// 初始猜测 = LRA 半周期：n = round(5000/f) - 5（8.5.1.1）
			uint32_t n = DIV_ROUND_CLOSEST(5000U, (uint32_t)config->vib_frequency_hz);

			drive_raw = (uint8_t)CLAMP(n > 5 ? n - 5 : 0, 0, 31);
		}
	}

	vp.lra = config->actuator_mode == DRV2605_ACTUATOR_MODE_LRA;
	vp.open_loop = vp.lra ? config->lra_open_loop : config->erm_open_loop;
	vp.f_lra_hz = config->vib_frequency_hz;
	vp.sample_us = drv2605_sample_time_us(c2);
	vp.drive_us = drv2605_drive_time_us(vp.lra, drive_raw);
	vp.idiss_us = drv2605_idiss_time_us(vp.lra, c2, c5);
	vp.blank_us = drv2605_blank_time_us(vp.lra, c2, c5);

	value = FIELD_PREP(DRV2605_N_ERM_LRA, config->actuator_mode) |
		FIELD_PREP(DRV2605_FB_BRAKE_FACTOR, config->feedback_brake_factor) |
		FIELD_PREP(DRV2605_LOOP_GAIN, config->loop_gain);

	mask = DRV2605_N_ERM_LRA | DRV2605_FB_BRAKE_FACTOR | DRV2605_LOOP_GAIN;

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_FEEDBACK_CONTROL, mask, value);
	if (ret < 0) {
		return ret;
	}

	// 电压按式(4)-(9)换算；两寄存器是校准的输入，必须在校准前写好（8.5.2.1）
	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_RATED_VOLTAGE,
				    drv2605_calc_rated_reg(config->vib_rated_mv, &vp));
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_OVERDRIVE_CLAMP_VOLTAGE,
				    drv2605_calc_clamp_reg(config->vib_overdrive_mv, &vp));
	if (ret < 0) {
		return ret;
	}

	// CONTROL1：启动助推（芯片默认开）+ 驱动时间；AC_COUPLE 留给音频模式管理
	mask = DRV2605_STARTUP_BOOST | DRV2605_AC_COUPLE | DRV2605_DRIVE_TIME;
	value = DRV2605_STARTUP_BOOST | FIELD_PREP(DRV2605_DRIVE_TIME, drive_raw);

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL1, mask, value);
	if (ret < 0) {
		return ret;
	}

	// CONTROL4：只动 ZC_DET_TIME/AUTO_CAL_TIME 两字段，绝不触碰 OTP 位（一次性）
	mask = DRV2605_ZERO_CROSSING_TIME | DRV2605_AUTO_CAL_TIME;
	value = FIELD_PREP(DRV2605_AUTO_CAL_TIME, config->auto_cal_time);

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL4, mask, value);
	if (ret < 0) {
		return ret;
	}

	// CONTROL3：开环选择。LRA 默认闭环自动谐振（Library 6 即闭环库），与上游恒置开环不同
	mask = DRV2605_ERM_OPEN_LOOP | DRV2605_LRA_OPEN_LOOP;
	value = 0;
	if (vp.lra && config->lra_open_loop) {
		value = DRV2605_LRA_OPEN_LOOP;
	} else if (!vp.lra && config->erm_open_loop) {
		value = DRV2605_ERM_OPEN_LOOP;
	}

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL3, mask, value);
	if (ret < 0) {
		return ret;
	}

	if (vp.lra && config->vib_frequency_hz) {
		/* Also preload the period for effects that temporarily use open loop. */
		uint32_t period = DIV_ROUND_CLOSEST(10136U, (uint32_t)config->vib_frequency_hz);
		return i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_LRA_OPEN_LOOP_PERIOD,
					     (uint8_t)MIN(period, 127U));
	}
	if (vp.lra && config->lra_open_loop) {
		LOG_WRN("lra-open-loop without vib-frequency-hz, 0x20 left at default");
	}

	return 0;
}

static int drv2605_run_process(const struct device *dev, enum drv2605_mode mode)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;
	int ret;

	// 校准/诊断：退出待机 -> 进入模式 -> 触发 GO；GO 自清后结果才有效（Table 4）
	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE,
				     DRV2605_STANDBY | DRV2605_MODE, (uint8_t)mode);
	if (ret < 0) {
		return ret;
	}

	data->mode = mode;

	return i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_GO, DRV2605_GO, DRV2605_GO);
}

int drv2605_calibrate(const struct device *dev)
{
	return drv2605_run_process(dev, DRV2605_MODE_AUTO_CAL);
}

int drv2605_calibrate_and_wait(const struct device *dev)
{
	int ret = drv2605_calibrate(dev);

	if (ret < 0) {
		return ret;
	}

	/* The longest auto-calibration setting is 1200 ms. Allow for polling and I2C. */
	const int64_t deadline = k_uptime_get() + 1500;

	do {
		k_msleep(10);
		ret = drv2605_go_active(dev);
		if (ret < 0) {
			return ret;
		}
	} while (ret > 0 && k_uptime_get() < deadline);

	if (ret > 0) {
		return -ETIMEDOUT;
	}

	uint8_t status;
	ret = drv2605_read_status(dev, &status);
	if (ret < 0) {
		return ret;
	}
	if (status & (DRV2605_OVER_TEMP | DRV2605_OC_DETECT)) {
		LOG_ERR("DRV2605 auto-calibration protection fault, status 0x%02x", status);
		return -EIO;
	}

	return (status & DRV2605_DIAG_RESULT) ? -ERANGE : 0;
}

int drv2605_set_lra_drive(const struct device *dev, bool open_loop, uint16_t peak_mv,
			  bool half_cycle_updates)
{
	const struct drv2605_config *config = dev->config;

	if (config->actuator_mode != DRV2605_ACTUATOR_MODE_LRA ||
	    (open_loop && config->vib_frequency_hz == 0) ||
	    peak_mv == 0 || peak_mv > config->vib_overdrive_mv) {
		return -EINVAL;
	}

	/* Round down to keep the peak clamp at or below the requested voltage. */
	uint8_t clamp = MIN((uint32_t)peak_mv * 1000U / 21220U, 255U);
	int ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_OVERDRIVE_CLAMP_VOLTAGE,
					      UINT8_MAX, clamp);
	if (ret < 0) {
		return ret;
	}

	uint8_t mode = (open_loop ? DRV2605_LRA_OPEN_LOOP : 0) |
		       (half_cycle_updates ? DRV2605_LRA_DRIVE_MODE : 0);

	return i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_CONTROL3,
				      DRV2605_LRA_OPEN_LOOP | DRV2605_LRA_DRIVE_MODE, mode);
}

int drv2605_enable_lra_fallback(const struct device *dev, uint16_t peak_mv)
{
	int ret = drv2605_hw_setup(dev);

	return ret < 0 ? ret : drv2605_set_lra_drive(dev, true, peak_mv, false);
}

int drv2605_diagnose(const struct device *dev)
{
	return drv2605_run_process(dev, DRV2605_MODE_DIAGNOSTICS);
}

int drv2605_read_status(const struct device *dev, uint8_t *status)
{
	const struct drv2605_config *config = dev->config;

	return i2c_reg_read_byte_dt(&config->i2c, DRV2605_REG_STATUS, status);
}

int drv2605_restore_config(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;
	int ret;

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE,
				     DRV2605_STANDBY | DRV2605_MODE, 0);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_GO, 0);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&config->i2c, DRV2605_REG_RT_PLAYBACK_INPUT, 0);
	if (ret < 0) {
		return ret;
	}

	data->mode = DRV2605_MODE_INTERNAL_TRIGGER;

	return drv2605_hw_config(dev);
}

static int drv2605_reset(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	int retries = 5, ret;
	uint8_t value;

	i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_STANDBY, 0);

	ret = i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_DEV_RESET,
				     DRV2605_DEV_RESET);
	if (ret < 0) {
		return ret;
	}

	k_msleep(100);

	while (retries > 0) {
		retries--;

		ret = i2c_reg_read_byte_dt(&config->i2c, DRV2605_REG_MODE, &value);
		if (ret < 0) {
			k_usleep(10000);
			continue;
		}

		if ((value & DRV2605_DEV_RESET) == 0U) {
			i2c_reg_update_byte_dt(&config->i2c, DRV2605_REG_MODE, DRV2605_STANDBY, 0);
			return 0;
		}
	}

	return -ETIMEDOUT;
}

static int drv2605_check_devid(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	uint8_t value;
	int ret;

	ret = i2c_reg_read_byte_dt(&config->i2c, DRV2605_REG_STATUS, &value);
	if (ret < 0) {
		return ret;
	}

	value = FIELD_GET(DRV2605_DEVICE_ID, value);

	switch (value) {
	case DRV2605_DEVICE_ID_DRV2605:
	case DRV2605_DEVICE_ID_DRV2605L:
		break;
	default:
		LOG_ERR("Invalid device ID found");
		return -ENOTSUP;
	}

	LOG_DBG("Found DRV2605, DEVID: 0x%x", value);

	return 0;
}

static int drv2605_gpio_config(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	int ret;

	if (config->en_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->en_gpio)) {
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&config->en_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	if (config->in_trig_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->in_trig_gpio)) {
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&config->in_trig_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

int drv2605_hw_setup(const struct device *dev)
{
	struct drv2605_data *data = dev->data;
	int ret;

	atomic_clear(&data->configured);
	ret = drv2605_check_devid(dev);
	if (ret < 0) {
		return ret;
	}

	ret = drv2605_reset(dev);
	if (ret < 0) {
		LOG_DBG("Failed to reset device: %d", ret);
		return ret;
	}

	ret = drv2605_hw_config(dev);
	if (!ret) {
		atomic_clear(&data->standby);
		atomic_set(&data->configured, 1);
	}

	return ret;
}

// 设备 init 全程零 I2C：总线问题绝不允许拖住启动/USB，芯片初始化晚到 drv2605_hw_setup()
static int drv2605_init(const struct device *dev)
{
	const struct drv2605_config *config = dev->config;
	struct drv2605_data *data = dev->data;

	data->dev = dev;

	if (!i2c_is_ready_dt(&config->i2c)) {
		return -ENODEV;
	}

	k_work_init(&data->rtp_work, drv2605_rtp_work_handler);

	return drv2605_gpio_config(dev);
}

/*
 * Zephyr 3.5 移植点（唯一一处 4.x 专用语法）：
 *   DEVICE_API(haptics, x) 是 4.0 引入的宏，3.5 的 device.h 里没有。
 *   3.5 的写法就是直接声明 const struct <subsystem>_driver_api。
 * 其余 API（i2c_reg_*_dt / PM_DEVICE_DT_INST_DEFINE / DEVICE_DT_INST_DEFINE /
 * k_work_cancel / gpio_*_dt / FIELD_* ）3.5 全部具备，无需改动。
 */
static const struct haptics_driver_api drv2605_driver_api = {
	.start_output = &drv2605_start_output,
	.stop_output = &drv2605_stop_output,
};

#define HAPTICS_DRV2605_DEFINE(inst)                                                               \
                                                                                                   \
	static const struct drv2605_config drv2605_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.en_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, en_gpios, {}),                           \
		.in_trig_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, in_trig_gpios, {}),                 \
		.feedback_brake_factor = DT_INST_ENUM_IDX(inst, feedback_brake_factor),            \
		.loop_gain = DT_INST_ENUM_IDX(inst, loop_gain),                                    \
		.actuator_mode = DT_INST_ENUM_IDX(inst, actuator_mode),                            \
		.vib_rated_mv = DT_INST_PROP(inst, vib_rated_mv),                                  \
		.vib_overdrive_mv = DT_INST_PROP(inst, vib_overdrive_mv),                          \
		.vib_frequency_hz = DT_INST_PROP(inst, vib_frequency_hz),                          \
		.auto_cal_time = DT_INST_PROP(inst, auto_cal_time),                                \
		.drive_time = DT_INST_PROP(inst, drive_time),                                      \
		.blanking_time = DT_INST_PROP(inst, lra_blanking_time),                            \
		.idiss_time = DT_INST_PROP(inst, lra_idiss_time),                                  \
		.erm_open_loop = DT_INST_PROP(inst, erm_open_loop),                                \
		.lra_open_loop = DT_INST_PROP(inst, lra_open_loop),                                \
	};                                                                                         \
                                                                                                   \
	static struct drv2605_data drv2605_data_##inst = {                                         \
		.mode = DRV2605_MODE_INTERNAL_TRIGGER,                                             \
	};                                                                                         \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(inst, drv2605_pm_action);                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, drv2605_init, PM_DEVICE_DT_INST_GET(inst),                     \
			      &drv2605_data_##inst, &drv2605_config_##inst, POST_KERNEL,           \
			      CONFIG_HAPTICS_INIT_PRIORITY, &drv2605_driver_api);

DT_INST_FOREACH_STATUS_OKAY(HAPTICS_DRV2605_DEFINE)
