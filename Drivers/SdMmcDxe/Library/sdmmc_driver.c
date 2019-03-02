/*
* Copyright (c) 2018 naehrwert
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DevicePathLib.h>

#include "sdmmc.h"
#include "mmc.h"

#include <Foundation/Types.h>
#include <Library/EarlyTimerLib.h>
#include <Device/T210.h>
#include <Device/Pmc.h>
#include <Library/ClockLib.h>
#include <Library/Max7762xLib.h>
#include <Library/PinmuxLib.h>
#include <Library/GpioLib.h>

#define DPRINTF(...)

/*! SCMMC controller base addresses. */
static const UINTN _sdmmc_bases[4] = {
	0x700B0000,
	0x700B0200,
	0x700B0400,
	0x700B0600,
};

int sdmmc_get_voltage(sdmmc_t *sdmmc)
{
	u32 p = sdmmc->regs->pwrcon;
	if (!(p & TEGRA_MMC_PWRCTL_SD_BUS_POWER))
		return SDMMC_POWER_OFF;
	if (p & TEGRA_MMC_PWRCTL_SD_BUS_VOLTAGE_V1_8)
		return SDMMC_POWER_1_8;
	if (p & TEGRA_MMC_PWRCTL_SD_BUS_VOLTAGE_V3_3)
		return SDMMC_POWER_3_3;
	return -1;
}

static int _sdmmc_set_voltage(sdmmc_t *sdmmc, u32 power)
{
	u8 pwr = (power == SDMMC_POWER_1_8) ? TEGRA_MMC_PWRCTL_SD_BUS_VOLTAGE_V1_8 : TEGRA_MMC_PWRCTL_SD_BUS_VOLTAGE_V3_3;

	switch (power)
	{
	case SDMMC_POWER_OFF:
		sdmmc->regs->pwrcon &= ~TEGRA_MMC_PWRCTL_SD_BUS_POWER;
		break;
	case SDMMC_POWER_1_8:
	case SDMMC_POWER_3_3:
		sdmmc->regs->pwrcon = pwr;		
		break;
	default:
		return 0;
	}

	if (power != SDMMC_POWER_OFF)
	{
		pwr |= TEGRA_MMC_PWRCTL_SD_BUS_POWER;
		sdmmc->regs->pwrcon = pwr;
	}	

	return 1;
}

u32 sdmmc_get_bus_width(sdmmc_t *sdmmc)
{
	u32 h = sdmmc->regs->hostctl;
	if (h & TEGRA_MMC_HOSTCTL_8BIT)
		return SDMMC_BUS_WIDTH_8;
	if (h & TEGRA_MMC_HOSTCTL_4BIT)
		return SDMMC_BUS_WIDTH_4;
	return SDMMC_BUS_WIDTH_1;
}

void sdmmc_set_bus_width(sdmmc_t *sdmmc, u32 bus_width)
{
	if (bus_width == SDMMC_BUS_WIDTH_1)
		sdmmc->regs->hostctl &= ~(TEGRA_MMC_HOSTCTL_4BIT | TEGRA_MMC_HOSTCTL_8BIT);
	else if (bus_width == SDMMC_BUS_WIDTH_4)
	{
		sdmmc->regs->hostctl |= TEGRA_MMC_HOSTCTL_4BIT;
		sdmmc->regs->hostctl &= ~TEGRA_MMC_HOSTCTL_8BIT;
	}
	else if (bus_width == SDMMC_BUS_WIDTH_8)
		sdmmc->regs->hostctl |= TEGRA_MMC_HOSTCTL_8BIT;
}

void sdmmc_get_venclkctl(sdmmc_t *sdmmc)
{
	sdmmc->venclkctl_tap = sdmmc->regs->venclkctl >> 16;
	sdmmc->venclkctl_set = 1;
}

static int _sdmmc_config_ven_ceata_clk(sdmmc_t *sdmmc, u32 id)
{
	u32 tap_val = 0;

	if (id == 4)
		sdmmc->regs->venceatactl = (sdmmc->regs->venceatactl & 0xFFFFC0FF) | 0x2800;
	sdmmc->regs->field_1C0 &= 0xFFFDFFFF;
	if (id == 4)
	{
		if (!sdmmc->venclkctl_set)
			return 0;
		tap_val = sdmmc->venclkctl_tap;
	}
	else
	{
		static const u32 tap_values[] = { 4, 0, 3, 0 };
		tap_val = tap_values[sdmmc->id];
	}
	sdmmc->regs->venclkctl = (sdmmc->regs->venclkctl & 0xFF00FFFF) | (tap_val << 16);

	return 1;
}

static int _sdmmc_get_clkcon(sdmmc_t *sdmmc)
{
	return sdmmc->regs->clkcon;
}

static void _sdmmc_pad_config_fallback(sdmmc_t *sdmmc, u32 power)
{
	_sdmmc_get_clkcon(sdmmc);
	if (sdmmc->id == SDMMC_4)
		*(vu32 *)0x70000AB4 = ((*(vu32 *)0x70000AB4) & 0x3FFC) | 0x1040;
	// TODO: load standard values for other controllers, can depend on power.
}

static int _sdmmc_wait_type4(sdmmc_t *sdmmc)
{
	int res = 1, should_disable_sd_clock = 0;

	if (!(sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE))
	{
		should_disable_sd_clock = 1;
		sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
	}

	sdmmc->regs->field_1B0 |= 0x80000000;
	_sdmmc_get_clkcon(sdmmc);

	u32 timeout = get_tmr() + 5000;
	while (sdmmc->regs->field_1B0 & 0x80000000)
	{
		if (get_tmr() > timeout)
		{
			res = 0;
			goto out;
		}
	}

	timeout = get_tmr() + 10000;
	while (sdmmc->regs->field_1BC & 0x80000000)
	{
		if (get_tmr() > timeout)
		{
			res = 0;
			goto out;
		}
	}

out:;
	if (should_disable_sd_clock)
		sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
	return res;
}

int sdmmc_setup_clock(sdmmc_t *sdmmc, u32 type)
{
	//Disable the SD clock if it was enabled, and reenable it later.
	int should_enable_sd_clock = 0;
	if (sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE)
	{
		should_enable_sd_clock = 1;
		sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
	}

	_sdmmc_config_ven_ceata_clk(sdmmc, type);

	switch (type)
	{
	case 0:
	case 1:
	case 5:
	case 6:
		sdmmc->regs->hostctl &= 0xFB;
		sdmmc->regs->hostctl2 &= 0xFFF7;
		break;
	case 2:
	case 7:
		sdmmc->regs->hostctl |= 4;
		sdmmc->regs->hostctl2 &= 0xFFF7;
		break;
	case 3:
	case 11:
	case 13:
	case 14:
		sdmmc->regs->hostctl2 = (sdmmc->regs->hostctl2 & 0xFFF8) | 3;
		sdmmc->regs->hostctl2 |= 8;
		break;
	case 4:
		sdmmc->regs->hostctl2 = (sdmmc->regs->hostctl2 & 0xFFF8) | 5;
		sdmmc->regs->hostctl2 |= 8;
		break;
	case 8:
		sdmmc->regs->hostctl2 = sdmmc->regs->hostctl2 & 0xFFF8;
		sdmmc->regs->hostctl2 |= 8;
		break;
	case 10:
		sdmmc->regs->hostctl2 = (sdmmc->regs->hostctl2 & 0xFFF8) | 2;
		sdmmc->regs->hostctl2 |= 8;
		break;
	}

	_sdmmc_get_clkcon(sdmmc);

	u32 tmp;
	u16 divisor;
	clock_sdmmc_get_params(&tmp, &divisor, type);
	clock_sdmmc_config_clock_source(&tmp, sdmmc->id, tmp);
	sdmmc->divisor = (tmp + divisor - 1) / divisor;

	//if divisor != 1 && divisor << 31 -> error

	u16 div = divisor >> 1;
	divisor = 0;
	if (div > 0xFF)
		divisor = div >> 8;
	sdmmc->regs->clkcon = (sdmmc->regs->clkcon & 0x3F) | (div << 8) | (divisor << 6);

	//Enable the SD clock again.
	if (should_enable_sd_clock)
		sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;

	if (type == 4)
		return _sdmmc_wait_type4(sdmmc);
	return 1;
}

static void _sdmmc_sd_clock_enable(sdmmc_t *sdmmc)
{
	if (!sdmmc->no_sd)
	{
		if (!(sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE))
			sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
	}
	sdmmc->sd_clock_enabled = 1;
}

static void _sdmmc_sd_clock_disable(sdmmc_t *sdmmc)
{
	sdmmc->sd_clock_enabled = 0;
	sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
}

void sdmmc_sd_clock_ctrl(sdmmc_t *sdmmc, int no_sd)
{
	sdmmc->no_sd = no_sd;
	if (no_sd)
	{
		if (!(sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE))
			return;
		sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
		return;
	}
	if (sdmmc->sd_clock_enabled)
		if (!(sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE))
			sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
}

static int _sdmmc_cache_rsp(sdmmc_t *sdmmc, u32 *rsp, u32 size, u32 type)
{
	switch (type)
	{
	case SDMMC_RSP_TYPE_1:
	case SDMMC_RSP_TYPE_3:
	case SDMMC_RSP_TYPE_4:
	case SDMMC_RSP_TYPE_5:
		if (size < 4)
			return 0;
		rsp[0] = sdmmc->regs->rspreg0;
		break;
	case SDMMC_RSP_TYPE_2:
		if (size < 0x10)
			return 0;
		// CRC is stripped, so shifting is needed.
		u32 tempreg;
		for (int i = 0; i < 4; i++)
		{
			switch(i)
			{
			case 0:
				tempreg = sdmmc->regs->rspreg3;
				break;
			case 1:
				tempreg = sdmmc->regs->rspreg2;
				break;
			case 2:
				tempreg = sdmmc->regs->rspreg1;
				break;
			case 3:
				tempreg = sdmmc->regs->rspreg0;
				break;
			}
			rsp[i] = tempreg << 8;

			if (i != 0)
				rsp[i - 1] |= (tempreg >> 24) & 0xFF;
		}
		break;
	default:
		return 0;
		break;
	}

	return 1;
}

int sdmmc_get_rsp(sdmmc_t *sdmmc, u32 *rsp, u32 size, u32 type)
{
	if (!rsp || sdmmc->expected_rsp_type != type)
		return 0;

	switch (type)
	{
	case SDMMC_RSP_TYPE_1:
	case SDMMC_RSP_TYPE_3:
	case SDMMC_RSP_TYPE_4:
	case SDMMC_RSP_TYPE_5:
		if (size < 4)
			return 0;
		rsp[0] = sdmmc->rsp[0];
		break;
	case SDMMC_RSP_TYPE_2:
		if (size < 0x10)
			return 0;
		rsp[0] = sdmmc->rsp[0];
		rsp[1] = sdmmc->rsp[1];
		rsp[2] = sdmmc->rsp[2];
		rsp[3] = sdmmc->rsp[3];
		break;
	default:
		return 0;
		break;
	}

	return 1;
}

static void _sdmmc_reset(sdmmc_t *sdmmc)
{
	sdmmc->regs->swrst |= 
		TEGRA_MMC_SWRST_SW_RESET_FOR_CMD_LINE | TEGRA_MMC_SWRST_SW_RESET_FOR_DAT_LINE;
	_sdmmc_get_clkcon(sdmmc);
	u32 timeout = get_tmr() + 2000000;
	while (sdmmc->regs->swrst << 29 >> 30 && get_tmr() < timeout)
		;
}

static int _sdmmc_wait_prnsts_type0(sdmmc_t *sdmmc, u32 wait_dat)
{
	_sdmmc_get_clkcon(sdmmc);

	u32 timeout = get_tmr() + 2000000;
	while(sdmmc->regs->prnsts & 1) //CMD inhibit.
		if (get_tmr() > timeout)
		{
			_sdmmc_reset(sdmmc);
			return 0;
		}

	if (wait_dat)
	{
		timeout = get_tmr() + 2000000;
		while (sdmmc->regs->prnsts & 2) //DAT inhibit.
			if (get_tmr() > timeout)
			{
				_sdmmc_reset(sdmmc);
				return 0;
			}
	}

	return 1;
}

static int _sdmmc_wait_prnsts_type1(sdmmc_t *sdmmc)
{
	_sdmmc_get_clkcon(sdmmc);

	u32 timeout = get_tmr() + 2000000;
	while (!(sdmmc->regs->prnsts & 0x100000)) //DAT0 line level.
		if (get_tmr() > timeout)
		{
			_sdmmc_reset(sdmmc);
			return 0;
		}

	return 1;
}

static int _sdmmc_setup_read_small_block(sdmmc_t *sdmmc)
{
	switch (sdmmc_get_bus_width(sdmmc))
	{
	case SDMMC_BUS_WIDTH_1:
		return 0;
		break;
	case SDMMC_BUS_WIDTH_4:
		sdmmc->regs->blksize = 0x40;
		break;
	case SDMMC_BUS_WIDTH_8:
		sdmmc->regs->blksize = 0x80;
		break;
	}
	sdmmc->regs->blkcnt = 1;
	sdmmc->regs->trnmod = TEGRA_MMC_TRNMOD_DATA_XFER_DIR_SEL_READ;
	return 1;
}

static int _sdmmc_parse_cmdbuf(sdmmc_t *sdmmc, sdmmc_cmd_t *cmd, int is_data_present)
{
	u16 cmdflags = 0;
	
	switch (cmd->rsp_type)
	{
	case SDMMC_RSP_TYPE_0:
		break;
	case SDMMC_RSP_TYPE_1:
	case SDMMC_RSP_TYPE_4:
	case SDMMC_RSP_TYPE_5:
		if (cmd->check_busy)
			cmdflags = TEGRA_MMC_CMDREG_RESP_TYPE_SELECT_LENGTH_48_BUSY |
				TEGRA_MMC_TRNMOD_CMD_INDEX_CHECK |
				TEGRA_MMC_TRNMOD_CMD_CRC_CHECK;
		else
			cmdflags = TEGRA_MMC_CMDREG_RESP_TYPE_SELECT_LENGTH_48 |
				TEGRA_MMC_TRNMOD_CMD_INDEX_CHECK |
				TEGRA_MMC_TRNMOD_CMD_CRC_CHECK;
		break;
	case SDMMC_RSP_TYPE_2:
		cmdflags = TEGRA_MMC_CMDREG_RESP_TYPE_SELECT_LENGTH_136 |
			TEGRA_MMC_TRNMOD_CMD_CRC_CHECK;
		break;
	case SDMMC_RSP_TYPE_3:
		cmdflags = TEGRA_MMC_CMDREG_RESP_TYPE_SELECT_LENGTH_48;
		break;
	default:
		return 0;
		break;
	}

	if (is_data_present)
		cmdflags |= TEGRA_MMC_TRNMOD_DATA_PRESENT_SELECT_DATA_TRANSFER;
	sdmmc->regs->argument = cmd->arg;
	sdmmc->regs->cmdreg = (cmd->cmd << 8) | cmdflags;

	return 1;
}

static void _sdmmc_parse_cmd_48(sdmmc_t *sdmmc, u32 cmd)
{
	sdmmc_cmd_t cmdbuf;
	cmdbuf.cmd = cmd;
	cmdbuf.arg = 0;
	cmdbuf.rsp_type = SDMMC_RSP_TYPE_1;
	cmdbuf.check_busy = 0;
	_sdmmc_parse_cmdbuf(sdmmc, &cmdbuf, 1);
}

static int _sdmmc_config_tuning_once(sdmmc_t *sdmmc, u32 cmd)
{
	if (sdmmc->no_sd)
		return 0;
	if (!_sdmmc_wait_prnsts_type0(sdmmc, 1))
		return 0;

	_sdmmc_setup_read_small_block(sdmmc);
	sdmmc->regs->norintstsen |= TEGRA_MMC_NORINTSTSEN_BUFFER_READ_READY;
	sdmmc->regs->norintsts = sdmmc->regs->norintsts;
	sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
	_sdmmc_parse_cmd_48(sdmmc, cmd);
	_sdmmc_get_clkcon(sdmmc);
	sleep(1);
	_sdmmc_reset(sdmmc);
	sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
	_sdmmc_get_clkcon(sdmmc);

	u32 timeout = get_tmr() + 5000;
	while (get_tmr() < timeout)
	{
		if (sdmmc->regs->norintsts & 0x20)
		{
			sdmmc->regs->norintsts = 0x20;
			sdmmc->regs->norintstsen &= 0xFFDF;
			_sdmmc_get_clkcon(sdmmc);
			sleep((1000 * 8 + sdmmc->divisor - 1) / sdmmc->divisor);
			return 1;
		}
	}
	_sdmmc_reset(sdmmc);
	sdmmc->regs->norintstsen &= 0xFFDF;
	_sdmmc_get_clkcon(sdmmc);
	sleep((1000 * 8 + sdmmc->divisor - 1) / sdmmc->divisor);
	return 0;
}

int sdmmc_config_tuning(sdmmc_t *sdmmc, u32 type, u32 cmd)
{
	u32 max = 0, flag = 0;

	sdmmc->regs->field_1C4 = 0;
	switch (type)
	{
	case 3:
	case 4:
	case 11:
		max = 0x80;
		flag = 0x4000;
		break;
	case 10:
	case 13:
	case 14:
		max = 0x100;
		flag = 0x8000;
		break;
	default:
		return 0;
	}

	sdmmc->regs->field_1C0 = (sdmmc->regs->field_1C0 & 0xFFFF1FFF) | flag;
	sdmmc->regs->field_1C0 = (sdmmc->regs->field_1C0 & 0xFFFFE03F) | 0x40;
	sdmmc->regs->field_1C0 |= 0x20000;
	sdmmc->regs->hostctl2 |= 0x40;

	for (u32 i = 0; i < max; i++)
	{
		_sdmmc_config_tuning_once(sdmmc, cmd);
		if (!(sdmmc->regs->hostctl2 & 0x40))
			break;
	}

	if (sdmmc->regs->hostctl2 & 0x80)
		return 1;
	return 0;
}

static int _sdmmc_enable_internal_clock(sdmmc_t *sdmmc)
{
	//Enable internal clock and wait till it is stable.
	sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_INTERNAL_CLOCK_ENABLE;
	_sdmmc_get_clkcon(sdmmc);
	u32 timeout = get_tmr() + 2000000;
	while (!(sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_INTERNAL_CLOCK_STABLE))
	{
		if (get_tmr() > timeout)
			return 0;
	}

	sdmmc->regs->hostctl2 &= 0x7FFF;
	sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_CLKGEN_SELECT;
	sdmmc->regs->hostctl2 |= 0x1000;

	if (!(sdmmc->regs->capareg & 0x10000000))
		return 0;

	sdmmc->regs->hostctl2 |= 0x2000;
	sdmmc->regs->hostctl &= 0xE7;
	sdmmc->regs->timeoutcon = (sdmmc->regs->timeoutcon & 0xF0) | 0xE;

	return 1;
}

static int _sdmmc_autocal_config_offset(sdmmc_t *sdmmc, u32 power)
{
	u32 off_pd = 0;
	u32 off_pu = 0;

	switch (sdmmc->id)
	{
	case SDMMC_2:
	case SDMMC_4:
		if (power != SDMMC_POWER_1_8)
			return 0;
		off_pd = 5;
		off_pu = 5;
		break;
	case SDMMC_1:
	case SDMMC_3:
		if (power == SDMMC_POWER_1_8)
		{
			off_pd = 123;
			off_pu = 123;
		}
		else if (power == SDMMC_POWER_3_3)
		{
			off_pd = 125;
			off_pu = 0;
		}
		else
			return 0;
		break;
	}

	sdmmc->regs->autocalcfg = (((sdmmc->regs->autocalcfg & 0xFFFF80FF) | (off_pd << 8)) >> 7 << 7) | off_pu;
	return 1;
}

static void _sdmmc_autocal_execute(sdmmc_t *sdmmc, u32 power)
{
	int should_enable_sd_clock = 0;
	if (sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE)
	{
		should_enable_sd_clock = 1;
		sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
	}

	if (!(sdmmc->regs->sdmemcmppadctl & 0x80000000))
	{
		sdmmc->regs->sdmemcmppadctl |= 0x80000000;
		_sdmmc_get_clkcon(sdmmc);
		sleep(1);
	}

	sdmmc->regs->autocalcfg |= 0xA0000000;
	_sdmmc_get_clkcon(sdmmc);
	sleep(1);

	u32 timeout = get_tmr() + 10000;
	while (sdmmc->regs->autocalcfg & 0x80000000)
	{
		if (get_tmr() > timeout)
		{
			//In case autocalibration fails, we load suggested standard values.
			_sdmmc_pad_config_fallback(sdmmc, power);
			sdmmc->regs->autocalcfg &= 0xDFFFFFFF;
			break;
		}
	}

	sdmmc->regs->sdmemcmppadctl &= 0x7FFFFFFF;

	if(should_enable_sd_clock)
		sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
}

static void _sdmmc_enable_interrupts(sdmmc_t *sdmmc)
{
	sdmmc->regs->norintstsen |= 0xB;
	sdmmc->regs->errintstsen |= 0x17F;
	sdmmc->regs->norintsts = sdmmc->regs->norintsts;
	sdmmc->regs->errintsts = sdmmc->regs->errintsts;
}

static void _sdmmc_mask_interrupts(sdmmc_t *sdmmc)
{
	sdmmc->regs->errintstsen &= 0xFE80;
	sdmmc->regs->norintstsen &= 0xFFF4;
}

static int _sdmmc_check_mask_interrupt(sdmmc_t *sdmmc, u16 *pout, u16 mask)
{
	u16 norintsts = sdmmc->regs->norintsts;
	u16 errintsts = sdmmc->regs->errintsts;

	DPRINTF("norintsts %08X; errintsts %08X\n", norintsts, errintsts);

	if (pout)
		*pout = norintsts;

	//Check for error interrupt.
	if (norintsts & TEGRA_MMC_NORINTSTS_ERR_INTERRUPT)
	{
		sdmmc->regs->errintsts = errintsts;
		return SDMMC_MASKINT_ERROR;
	}
	else if (norintsts & mask)
	{
		sdmmc->regs->norintsts = norintsts & mask;
		return SDMMC_MASKINT_MASKED;
	}
	
	return SDMMC_MASKINT_NOERROR;
}

static int _sdmmc_wait_request(sdmmc_t *sdmmc)
{
	_sdmmc_get_clkcon(sdmmc);

	u32 timeout = get_tmr() + 2000000;
	while (1)
	{
		int res = _sdmmc_check_mask_interrupt(sdmmc, 0, TEGRA_MMC_NORINTSTS_CMD_COMPLETE);
		if (res == SDMMC_MASKINT_MASKED)
			break;
		if (res != SDMMC_MASKINT_NOERROR || get_tmr() > timeout)
		{
			_sdmmc_reset(sdmmc);
			return 0;
		}
	}

	return 1;
}

static int _sdmmc_stop_transmission_inner(sdmmc_t *sdmmc, u32 *rsp)
{
	sdmmc_cmd_t cmd;

	if (!_sdmmc_wait_prnsts_type0(sdmmc, 0))
		return 0;

	_sdmmc_enable_interrupts(sdmmc);
	cmd.cmd = MMC_STOP_TRANSMISSION;
	cmd.arg = 0;
	cmd.rsp_type = SDMMC_RSP_TYPE_1;
	cmd.check_busy = 1;
	_sdmmc_parse_cmdbuf(sdmmc, &cmd, 0);
	int res = _sdmmc_wait_request(sdmmc);
	_sdmmc_mask_interrupts(sdmmc);

	if (!res)
		return 0;
	
	_sdmmc_cache_rsp(sdmmc, rsp, 4, SDMMC_RSP_TYPE_1);
	return _sdmmc_wait_prnsts_type1(sdmmc);
}

int sdmmc_stop_transmission(sdmmc_t *sdmmc, u32 *rsp)
{
	if (!sdmmc->sd_clock_enabled)
		return 0;

	int should_disable_sd_clock = 0;
	if (!(sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE))
	{
		should_disable_sd_clock = 1;
		sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
		_sdmmc_get_clkcon(sdmmc);
		sleep((8000 + sdmmc->divisor - 1) / sdmmc->divisor);
	}

	int res = _sdmmc_stop_transmission_inner(sdmmc, rsp);
	sleep((8000 + sdmmc->divisor - 1) / sdmmc->divisor);
	if (should_disable_sd_clock)
		sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;

	return res;
}

static int _sdmmc_config_dma(sdmmc_t *sdmmc, u32 *blkcnt_out, sdmmc_req_t *req)
{
	if (!req->blksize || !req->num_sectors)
		return 0;

	u32 blkcnt = req->num_sectors;
	if (blkcnt >= 0xFFFF) blkcnt = 0xFFFF;
	u64 admaaddr = (UINTN) req->buf;

	// Check alignment. (This for 32-bit, need propre value for 64-bit)
	// if (admaaddr << 29) return 0;

	sdmmc->regs->admaaddr = admaaddr;
	sdmmc->dma_addr_next = (admaaddr + 0x80000) & 0xFFF80000;
	sdmmc->regs->blksize = req->blksize | 0x7000;
	sdmmc->regs->blkcnt = blkcnt;

	if (blkcnt_out)
		*blkcnt_out = blkcnt;

	u32 trnmode = TEGRA_MMC_TRNMOD_DMA_ENABLE;
	if (req->is_multi_block)
	{
		trnmode = TEGRA_MMC_TRNMOD_MULTI_BLOCK_SELECT |
			TEGRA_MMC_TRNMOD_BLOCK_COUNT_ENABLE |
			TEGRA_MMC_TRNMOD_DMA_ENABLE;
	}
	if (!req->is_write)
	{
		trnmode |= TEGRA_MMC_TRNMOD_DATA_XFER_DIR_SEL_READ;
	}
	if (req->is_auto_cmd12)
	{
		trnmode = (trnmode & 0xFFF3) | TEGRA_MMC_TRNMOD_AUTO_CMD12;
	}

	sdmmc->regs->trnmod = trnmode;

	return 1;
}

static int _sdmmc_update_dma(sdmmc_t *sdmmc)
{
	u16 blkcnt = 0;
	do
	{
		blkcnt = sdmmc->regs->blkcnt;
		u32 timeout = get_tmr() + 1500000;
		do
		{
			int res = 0;
			while (1)
			{
				u16 intr = 0;
				res = _sdmmc_check_mask_interrupt(sdmmc, &intr, 
					TEGRA_MMC_NORINTSTS_XFER_COMPLETE | TEGRA_MMC_NORINTSTS_DMA_INTERRUPT);
				if (res < 0)
					break;
				if (intr & TEGRA_MMC_NORINTSTS_XFER_COMPLETE)
					return 1; //Transfer complete.
				if (intr & TEGRA_MMC_NORINTSTS_DMA_INTERRUPT)
				{
					//Update DMA.
					sdmmc->regs->admaaddr = sdmmc->dma_addr_next;
					sdmmc->dma_addr_next += 0x80000;
				}
			}
			if (res != SDMMC_MASKINT_NOERROR)
			{
				_sdmmc_reset(sdmmc);
				return 0;
			}
		} while (get_tmr() < timeout);
	} while (sdmmc->regs->blkcnt != blkcnt);

	_sdmmc_reset(sdmmc);
	return 0;
}

static int _sdmmc_execute_cmd_inner(sdmmc_t *sdmmc, sdmmc_cmd_t *cmd, sdmmc_req_t *req, u32 *blkcnt_out)
{
	int has_req_or_check_busy = req || cmd->check_busy;
	if (!_sdmmc_wait_prnsts_type0(sdmmc, has_req_or_check_busy))
		return 0;

	u32 blkcnt = 0;
	int is_data_present = 0;
	if (req)
	{
		_sdmmc_config_dma(sdmmc, &blkcnt, req);
		_sdmmc_enable_interrupts(sdmmc);
		is_data_present = 1;
	}
	else
	{
		_sdmmc_enable_interrupts(sdmmc);
		is_data_present = 0;
	}

	_sdmmc_parse_cmdbuf(sdmmc, cmd, is_data_present);

	int res = _sdmmc_wait_request(sdmmc);
	DPRINTF("rsp(%d): %08X, %08X, %08X, %08X\n", res, 
		sdmmc->regs->rspreg0, sdmmc->regs->rspreg1, sdmmc->regs->rspreg2, sdmmc->regs->rspreg3);
	if (res)
	{
		if (cmd->rsp_type)
		{
			sdmmc->expected_rsp_type = cmd->rsp_type;
			_sdmmc_cache_rsp(sdmmc, sdmmc->rsp, 0x10, cmd->rsp_type);
		}
		if (req)
			_sdmmc_update_dma(sdmmc);
	}

	_sdmmc_mask_interrupts(sdmmc);

	if (res)
	{
		if (req)
		{
			if (blkcnt_out)
				*blkcnt_out = blkcnt;
			if (req->is_auto_cmd12)
				sdmmc->rsp3 = sdmmc->regs->rspreg3;
		}

		if (cmd->check_busy || req)
			return _sdmmc_wait_prnsts_type1(sdmmc);
	}

	return res;
}

static void _sdmmc1_config_pads(u32 padMode) //0 for disabled, 1 for 3.3v, higher for 1.8v (schmitt on)
{
	/*
	* Pinmux config:
	*  DRV_TYPE = DRIVE_2X
	*  E_SCHMT = ENABLE (for 1.8V),  DISABLE (for 3.3V)
	*  E_INPUT = ENABLE
	*  TRISTATE = PASSTHROUGH
	*  APB_MISC_GP_SDMMCx_CLK_LPBK_CONTROL = SDMMCx_CLK_PAD_E_LPBK for CLK
	*/

	APB_MISC(APB_MISC_GP_SDMMC1_CLK_LPBK_CONTROL) = padMode ? 1 : 0;
	u32 config = PINMUX_DRIVE_2X | PINMUX_PARKED;
	if (padMode)
		config |= PINMUX_INPUT_ENABLE;
	else
		config |= PINMUX_TRISTATE;

	if (padMode > 1)
		config |= PINMUX_SCHMT;

	pinmux_set_config(PINMUX_SDMMC1_CLK_INDEX, config | PINMUX_SDMMC1_CLK_FUNC_SDMMC1);
	if (padMode)
		config |= PINMUX_PULL_UP; //needed for all except CLK
	
	pinmux_set_config(PINMUX_SDMMC1_CMD_INDEX, config | PINMUX_SDMMC1_CMD_FUNC_SDMMC1);
	pinmux_set_config(PINMUX_SDMMC1_DAT3_INDEX, config | PINMUX_SDMMC1_DAT3_FUNC_SDMMC1);
	pinmux_set_config(PINMUX_SDMMC1_DAT2_INDEX, config | PINMUX_SDMMC1_DAT2_FUNC_SDMMC1);
	pinmux_set_config(PINMUX_SDMMC1_DAT1_INDEX, config | PINMUX_SDMMC1_DAT1_FUNC_SDMMC1);
	pinmux_set_config(PINMUX_SDMMC1_DAT0_INDEX, config | PINMUX_SDMMC1_DAT0_FUNC_SDMMC1);
}

static int _sdmmc_config_sdmmc1()
{
	sleep(100); // let the card detect stabilize
	if(!!gpio_read(GPIO_DECOMPOSE(GPIO_Z1_INDEX)))
		return 0;

	//Set SDMMC1 IO clamps to default value before changing voltage
	PMC(APBDEV_PMC_PWR_DET_VAL) |= (1 << 12);

	//Reset the SDMMC1 IO voltage back to normal
	max77620_regulator_set_voltage(REGULATOR_LDO2, 3300000);

	//Configure SDMMC1 pinmux to enabled, 3.3v mode
	_sdmmc1_config_pads(1);

	//Let the power to the SD card flow
	gpio_write(GPIO_BY_NAME(DMIC3_CLK), GPIO_HIGH);
	sleep(1000);

	//For good measure.
	APB_MISC(APB_MISC_GP_SDMMC1_PAD_CFGPADCTRL) = 0x10000000;

	sleep(1000);

	return 1;
}

int sdmmc_init(sdmmc_t *sdmmc, u32 id, u32 power, u32 bus_width, u32 type, int no_sd)
{
	if (id > SDMMC_4)
		return 0;

	if (id == SDMMC_1)
		if (!_sdmmc_config_sdmmc1())
			return 0;

	ZeroMem(sdmmc, sizeof(sdmmc_t));

	sdmmc->regs = (t210_sdmmc_t *)_sdmmc_bases[id];
	sdmmc->id = id;
	sdmmc->clock_stopped = 1;

	if (clock_sdmmc_is_not_reset_and_enabled(id))
	{
		_sdmmc_sd_clock_disable(sdmmc);
		_sdmmc_get_clkcon(sdmmc);
	}

	u32 clock;
	u16 divisor;
	clock_sdmmc_get_params(&clock, &divisor, type);
	clock_sdmmc_enable(id, clock);

	sdmmc->clock_stopped = 0;

	//TODO: make this skip-able.
	sdmmc->regs->field_1F0 |= 0x80000;
	sdmmc->regs->field_1AC &= 0xFFFFFFFB;
	static const u32 trim_values[] = { 2, 8, 3, 8 };
	sdmmc->regs->venclkctl = (sdmmc->regs->venclkctl & 0xE0FFFFFF) | (trim_values[sdmmc->id] << 24);
	sdmmc->regs->sdmemcmppadctl = (sdmmc->regs->sdmemcmppadctl & 0xF) | 7;
	
	if (!_sdmmc_autocal_config_offset(sdmmc, power))
		return 0;
	_sdmmc_autocal_execute(sdmmc, power);
	if (_sdmmc_enable_internal_clock(sdmmc))
	{
		sdmmc_set_bus_width(sdmmc, bus_width);
		_sdmmc_set_voltage(sdmmc, power);
		if (sdmmc_setup_clock(sdmmc, type))
		{
			sdmmc_sd_clock_ctrl(sdmmc, no_sd);
			_sdmmc_sd_clock_enable(sdmmc);
			_sdmmc_get_clkcon(sdmmc);
			return 1;
		}
		return 0;
	}
	return 0;
}

void sdmmc_end(sdmmc_t *sdmmc, u32 powerOff)
{
	if (!sdmmc->clock_stopped)
	{
		_sdmmc_sd_clock_disable(sdmmc);
		_sdmmc_set_voltage(sdmmc, SDMMC_POWER_OFF);
		_sdmmc_get_clkcon(sdmmc);
		clock_sdmmc_disable(sdmmc->id);
		sdmmc->clock_stopped = 1;
	}

	//turn off the power completely if applicable
	if (powerOff && sdmmc->id == SDMMC_1) 
	{
		//Turn off the pads
		_sdmmc1_config_pads(0);

		//Cut the card's power
		gpio_write(GPIO_BY_NAME(DMIC3_CLK), GPIO_LOW);

		//Put the clamps back to the safe value before changing voltage
		PMC(APBDEV_PMC_PWR_DET_VAL) |= (1 << 12); 
		//Set the SDMMC1 IO rail back to 3.3v
		max77620_regulator_set_voltage(REGULATOR_LDO2, 3300000);
	}
}

void sdmmc_init_cmd(sdmmc_cmd_t *cmdbuf, u16 cmd, u32 arg, u32 rsp_type, u32 check_busy)
{
	cmdbuf->cmd = cmd;
	cmdbuf->arg = arg;
	cmdbuf->rsp_type = rsp_type;
	cmdbuf->check_busy = check_busy;
}

int sdmmc_execute_cmd(sdmmc_t *sdmmc, sdmmc_cmd_t *cmd, sdmmc_req_t *req, u32 *blkcnt_out)
{
	if (!sdmmc->sd_clock_enabled)
		return 0;

	//Recalibrate periodically for SDMMC1.
	if (sdmmc->id == SDMMC_1 && sdmmc->no_sd)
		_sdmmc_autocal_execute(sdmmc, sdmmc_get_voltage(sdmmc));

	int should_disable_sd_clock = 0;
	if (!(sdmmc->regs->clkcon & TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE))
	{
		should_disable_sd_clock = 1;
		sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
		_sdmmc_get_clkcon(sdmmc);
		sleep((8000 + sdmmc->divisor - 1) / sdmmc->divisor);
	}

	int res = _sdmmc_execute_cmd_inner(sdmmc, cmd, req, blkcnt_out);
	sleep((8000 + sdmmc->divisor - 1) / sdmmc->divisor);
	if (should_disable_sd_clock)
		sdmmc->regs->clkcon &= ~TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;

	return res;
}

int sdmmc_enable_low_voltage(sdmmc_t *sdmmc)
{
	if(sdmmc->id != SDMMC_1)
		return 0;

	if (!sdmmc_setup_clock(sdmmc, 8))
		return 0;

	_sdmmc_get_clkcon(sdmmc);

	max77620_regulator_set_voltage(REGULATOR_LDO2, 1800000);
	sleep(1000); //wait for regulator to change voltage
	PMC(APBDEV_PMC_PWR_DET_VAL) &= ~(1 << 12); //re-adjust the clamps for 1.8v operation
	_sdmmc1_config_pads(2); //enable schmitt on inputs

	_sdmmc_autocal_config_offset(sdmmc, SDMMC_POWER_1_8);
	_sdmmc_autocal_execute(sdmmc, SDMMC_POWER_1_8);
	_sdmmc_set_voltage(sdmmc, SDMMC_POWER_1_8);
	_sdmmc_get_clkcon(sdmmc);
	sleep(5000);
	
	if (sdmmc->regs->hostctl2 & 8)
	{
		sdmmc->regs->clkcon |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
		_sdmmc_get_clkcon(sdmmc);
		sleep(1000u);
		if ((sdmmc->regs->prnsts & 0xF00000) == 0xF00000)
			return 1;
	}

	return 0;
}
