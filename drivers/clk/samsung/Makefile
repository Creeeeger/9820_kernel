# SPDX-License-Identifier: GPL-2.0
#
# Samsung Clock specific Makefile
#

obj-$(CONFIG_EXYNOS_AUDSS_CLK_CON) += clk-exynos-audss.o

ifeq ($(CONFIG_COMPOSITE_CLK_SAMSUNG), y)
	obj-y                           += composite.o
ifeq ($(CONFIG_SOC_EXYNOS9820_EVT0), y)
	obj-$(CONFIG_SOC_EXYNOS9820_EVT0) += clk-exynos9820_evt0.o
else
	obj-$(CONFIG_SOC_EXYNOS9820)	+= clk-exynos9820.o
endif

else
	obj-$(CONFIG_COMMON_CLK)        += clk.o clk-pll.o clk-cpu.o
endif
obj-$(CONFIG_ARCH_EXYNOS)	+= clk-exynos-clkout.o
