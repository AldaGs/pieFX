/*
	pieFX.h - POC: the Anchor wheel (Windows).

	First real product code. Wires three Phase-0 spikes into one live loop:
	  - S2D  right-hold gesture, taken by swallowing the DOWN and replaying a
	         short click (this file's MouseProc / HoldTimerProc / ReplayRightClick).
	  - S3   a transparent always-on-top overlay - now the Tauri app in
	         ../overlay, driven over a named pipe (this file is the pipe SERVER).
	  - S1   the anchor move with position compensation, generalised from
	         "centre" to any of the 3x3 grid cells (S_anchor_script_fmt).

	Scope (locked by the plan): one segment only - Anchor - Windows only,
	hardcoded layout, no settings, no persistence. macOS and everything else at MVP.

	The native side owns ALL input (it has the WH_MOUSE hook) and is the single
	source of truth for which cell is highlighted/fired. The overlay is a pure
	renderer made click-through, so it never fights for the mouse while the right
	button is held - the one S2+S3 combination Phase 0 never exercised.
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

//	The single POC menu command: arm/disarm the whole thing.
#define PIEFX_MENU_NAME		"pieFX POC (Anchor Wheel: OFF/ON)"

//	Named pipe the overlay connects to. Native is the server.
#define PIEFX_PIPE_NAME		"\\\\.\\pipe\\pieFX"

//	Overlay executable, looked for beside the .aex (best-effort launch; the
//	overlay also retries connecting, so during dev it can be run by hand).
#define PIEFX_OVERLAY_EXE	"pieFX-overlay.exe"

//	Hold threshold (roadmap's 200ms; a setting at MVP).
#define PIEFX_HOLD_MS		200

//	Wheel geometry, in screen px. MUST match the constants in ../overlay/src/main.js
//	(CELL, GAP) or the cell the native side fires will not be the one the overlay
//	highlights. POC contract: keep these two in lockstep.
#define PIEFX_CELL			76
#define PIEFX_GAP			8

// Exported through the PiPL (.r file)
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;
