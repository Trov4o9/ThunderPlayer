/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 */

/** \file ghost/intern/GHOST_DisplayManagerWin32.cpp
 *  \ingroup GHOST
 *  \author	Maarten Gribnau
 *  \date	September 21, 2001
 */

#include "GHOST_DisplayManagerWin32.h"
#include "GHOST_Debug.h"

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x501 // require Windows XP or newer
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// We do not support multiple monitors at the moment
#define COMPILE_MULTIMON_STUBS
#include <multimon.h>


GHOST_DisplayManagerWin32::GHOST_DisplayManagerWin32(void)
{
}


GHOST_TSuccess GHOST_DisplayManagerWin32::getNumDisplays(GHOST_TUns8& numDisplays) const
{
	numDisplays = ::GetSystemMetrics(SM_CMONITORS);
	return numDisplays > 0 ? GHOST_kSuccess : GHOST_kFailure;
}

static BOOL get_dd(DWORD d, DISPLAY_DEVICE *dd)
{
	dd->cb = sizeof(DISPLAY_DEVICE);
	return ::EnumDisplayDevices(NULL, d, dd, 0);
}

/*
 * When you call EnumDisplaySettings with iModeNum set to zero, the operating system
 * initializes and caches information about the display device. When you call
 * EnumDisplaySettings with iModeNum set to a non-zero value, the function returns
 * the information that was cached the last time the function was called with iModeNum
 * set to zero.
 */
GHOST_TSuccess GHOST_DisplayManagerWin32::getNumDisplaySettings(GHOST_TUns8 display, GHOST_TInt32& numSettings) const
{
	DISPLAY_DEVICE display_device;
	if (!get_dd(display, &display_device)) return GHOST_kFailure;

	numSettings = 0;
	DEVMODE dm;
	while (::EnumDisplaySettings(display_device.DeviceName, numSettings, &dm)) {
		numSettings++;
	}
	return GHOST_kSuccess;
}


GHOST_TSuccess GHOST_DisplayManagerWin32::getDisplaySetting(GHOST_TUns8 display, GHOST_TInt32 index, GHOST_DisplaySetting& setting) const
{
	DISPLAY_DEVICE display_device;
	if (!get_dd(display, &display_device)) return GHOST_kFailure;

	GHOST_TSuccess success;
	DEVMODE dm;
	if (::EnumDisplaySettings(display_device.DeviceName, index, &dm)) {
#ifdef GHOST_DEBUG
		printf("display mode: width=%d, height=%d, bpp=%d, frequency=%d\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);
#endif // GHOST_DEBUG
		setting.xPixels     = dm.dmPelsWidth;
		setting.yPixels     = dm.dmPelsHeight;
		setting.bpp         = dm.dmBitsPerPel;
		/* When you call the EnumDisplaySettings function, the dmDisplayFrequency member
		 * may return with the value 0 or 1. These values represent the display hardware's
		 * default refresh rate. This default rate is typically set by switches on a display
		 * card or computer motherboard, or by a configuration program that does not use
		 * Win32 display functions such as ChangeDisplaySettings.
		 */
		/* First, we tried to explicitly set the frequency to 60 if EnumDisplaySettings
		 * returned 0 or 1 but this doesn't work since later on an exact match will
		 * be searched. And this will never happen if we change it to 60. Now we rely
		 * on the default h/w setting.
		 */
		setting.frequency = dm.dmDisplayFrequency;
		success = GHOST_kSuccess;
	}
	else {
		success = GHOST_kFailure;
	}
	return success;
}


GHOST_TSuccess GHOST_DisplayManagerWin32::getCurrentDisplaySetting(GHOST_TUns8 display, GHOST_DisplaySetting& setting) const
{
	return getDisplaySetting(display, ENUM_CURRENT_SETTINGS, setting);
}
GHOST_TSuccess GHOST_DisplayManagerWin32::setCurrentDisplaySetting(
        GHOST_TUns8 display,
        const GHOST_DisplaySetting& setting)
{
    DISPLAY_DEVICE dd;
    if (!get_dd(display, &dd)) {
        printf("Erro: display %d inválido.\n", display);
        return GHOST_kFailure;
    }

    GHOST_DisplaySetting best;
    findMatch(display, setting, best);

    DEVMODE dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmFields =
          DM_PELSWIDTH
        | DM_PELSHEIGHT
        | DM_BITSPERPEL
        | DM_DISPLAYFREQUENCY;

    dm.dmPelsWidth        = best.xPixels;
    dm.dmPelsHeight       = best.yPixels;
    dm.dmBitsPerPel       = best.bpp;
    dm.dmDisplayFrequency = best.frequency;

#ifdef GHOST_DEBUG
    printf("Aplicando modo REAL ao sistema:\n");
    printf("  Width=%d\n", dm.dmPelsWidth);
    printf("  Height=%d\n", dm.dmPelsHeight);
    printf("  BPP=%d\n", dm.dmBitsPerPel);
    printf("  Hz=%d\n", dm.dmDisplayFrequency);
    printf("Monitor: %s\n", dd.DeviceName);
#endif

    LONG result = ChangeDisplaySettingsEx(
        dd.DeviceName,
        &dm,
        NULL,
        CDS_FULLSCREEN,
        NULL);

    if (result != DISP_CHANGE_SUCCESSFUL)
    {
        printf("Erro: ChangeDisplaySettingsEx falhou (%ld). Tentando modo padrão...\n", result);

        ChangeDisplaySettingsEx(dd.DeviceName, NULL, NULL, 0, NULL);
        return GHOST_kFailure;
    }

    return GHOST_kSuccess;
}
