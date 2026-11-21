/*
obs-dlss5-nr — DLSS 5 Neural Rendering filter for OBS Studio
Copyright (C) 2026 Saganaki22

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#ifdef _WIN32
#include "dlss5-nr-filter.h"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
#ifdef _WIN32
	register_dlss5nr_filter();
#else
	obs_log(LOG_INFO, "obs-dlss5-nr requires Windows + NVIDIA RTX; this build is a no-op");
#endif
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
#ifdef _WIN32
	finalize_dlss5nr_filter();
#endif
	obs_log(LOG_INFO, "plugin unloaded");
}
