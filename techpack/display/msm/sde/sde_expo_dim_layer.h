/*
 * A new exposure driver based on SDE dim layer for OLED devices
 *
 * Copyright (C) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019, Devries <therkduan@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef SDE_EXPO_DIM_LAYER_H
#define SDE_EXPO_DIM_LAYER_H

#define DIM_THRES_LEVEL 440
#define BACKLIGHT_DIM_SCALE 1

enum {
	BRIGHTNESS = 0,
	ALPHA = 1,
	LUT_MAX,
};

static const uint16_t brightness_alpha_lut[][LUT_MAX] = {
/* {brightness, alpha} */
	{0, 0xFF},
	{2, 0xD7},
	{20, 0xB9},
	{35, 0xAA},
	{50, 0x9B},
	{65, 0x8C},
	{80, 0x7D},
	{90, 0x78},
	{100, 0x73},
	{120, 0x6C},
	{140, 0x64},
	{160, 0x5A},
	{180, 0x50},
	{200, 0x46},
	{240, 0x32},
	{270, 0x28},
	{360, 0x1E},
	{440, 0x00}
};

uint32_t expo_map_dim_level(uint32_t level, struct dsi_display *display);

#endif /* SDE_EXPO_DIM_LAYER_H */
