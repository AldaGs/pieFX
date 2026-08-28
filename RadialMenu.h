/*
	RadialMenu.h - Phase 0 capability spikes.

	Throwaway spike host for Phase 0. Currently carries:
	   S1  - AEGP loads, menu command registers, ExtendScript dispatch works.
	   S2A - probe: what window actually sits under the cursor over a comp viewer?
	   S2B - can we see right-button-held-past-200ms without breaking right-click?
	   S2C - can we TAKE the gesture: suppress the context menu on a hold only?
	   S3  - can a transparent always-on-top window sit over AE, and does a
	         focus round-trip cost us the layer selection?
*/

#pragma once

#include "AEConfig.h"

#ifdef AE_OS_WIN
	#define VC_EXTRALEAN
	#include <windows.h>
#endif

#include "entry.h"
#include "AE_GeneralPlug.h"
#include "AEGP_SuiteHandler.h"
#include "AE_Macros.h"

#include <stdio.h>
#include <stdarg.h>

#define RM_S1_MENU_NAME		"Radial Menu S1 (Anchor to Center)"
#define RM_S2A_MENU_NAME	"Radial Menu S2A (Probe Window Under Cursor)"
#define RM_S2B_MENU_NAME	"Radial Menu S2B (Watch Right-Hold: OFF/ON)"
#define RM_S2C_MENU_NAME	"Radial Menu S2C (Swallow Hold: OFF/ON)"
#define RM_S3_MENU_NAME		"Radial Menu S3A (Overlay Test, in-process)"
#define RM_S3B_MENU_NAME	"Radial Menu S3B (Overlay Test, separate process)"

//	Name of the throwaway .exe that draws the out-of-process ring. It is
//	expected to sit next to the .aex.
#define RM_S3B_EXE_NAME		"RadialMenu_S3B.exe"

#define RM_S5A_MENU_NAME	"Radial Menu S5A (Dump Effects Catalogue)"
#define RM_S5B_MENU_NAME	"Radial Menu S5B (Apply Gaussian Blur by Match Name)"

//	A stock effect present in every AE install, used to prove apply-by-match-name.
#define RM_S5B_MATCH_NAME	"ADBE Gaussian Blur 2"

//	Overlay diameter for the S3 spike.
#define RM_OVERLAY_SIZE		320

//	How long the S3 overlay stays up before closing itself.
#define RM_OVERLAY_MS		3000

//	Grace period after the menu command, so the cursor can be moved to the panel
//	being tested. Without it the overlay always appears over the Window menu,
//	which is the one screen position we do not care about.
#define RM_OVERLAY_ARM_MS	4000

//	How long the S2A probe samples the cursor for, once armed.
#define RM_PROBE_SECONDS	8

//	The hold threshold from the roadmap. Fixed here so the spike measures one
//	thing; it becomes a setting at MVP.
#define RM_HOLD_MS			200

// Exported through the PiPL (.r file)
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;
