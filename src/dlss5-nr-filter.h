// obs-dlss5-nr — DLSS 5 Neural Rendering filter for OBS Studio
// Copyright (C) 2026 Saganaki22
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void register_dlss5nr_filter(void);
void finalize_dlss5nr_filter(void);

#ifdef __cplusplus
}
#endif
