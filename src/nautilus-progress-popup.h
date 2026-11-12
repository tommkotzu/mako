/*
 * nautilus-progress-popup.h: running file operations, in a card in the corner
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define NAUTILUS_TYPE_PROGRESS_POPUP (nautilus_progress_popup_get_type ())

G_DECLARE_FINAL_TYPE (NautilusProgressPopup, nautilus_progress_popup, NAUTILUS, PROGRESS_POPUP, AdwBin)

G_END_DECLS
