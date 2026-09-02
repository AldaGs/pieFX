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
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

//	Arm/disarm the whole thing.
#define PIEFX_MENU_NAME		"pieFX (Wheel: OFF/ON)"

//	Fires one of EVERY executor kind in sequence, so the paths that the wheel
//	can reach but nobody has yet exercised stop being guesses. Needs one
//	selected layer; everything it does is one undo away.
#define PIEFX_SELFTEST_NAME	"pieFX Self-Test (Executors)"

//	A stock effect present in every install, used to prove apply-by-match-name.
#define PIEFX_TEST_EFFECT	"ADBE Gaussian Blur 2"

//	Center In View. Chosen for the ae-command probe because it needs a selected
//	layer, is visible, and undoes cleanly. Its id comes from the hand-tested
//	command map, which is exactly the thing being checked.
#define PIEFX_TEST_COMMAND	3819

//	Layer > New commands, fired one after another by the command probe. They are
//	the two the wheel uses that reported success and then did nothing, plus the
//	map's OTHER AdjustmentLayer id - the name appears twice, at 2263 and 2279,
//	and only one of them can be right.
//
//	All three create a layer immediately with no dialog, so the probe is
//	countable by eye and undoes cleanly.
#define PIEFX_PROBE_NULL	2767
#define PIEFX_PROBE_ADJ_A	2279
#define PIEFX_PROBE_ADJ_B	2263

#define PIEFX_CMDPROBE_NAME	"pieFX Self-Test (AE Commands)"

//	Opens the settings window, which is a SECOND window inside the already
//	running overlay process (see overlay/src/settings.js). It is not on the
//	wheel: the centre hexagon is cancel, so settings living there would open a
//	window on every aborted gesture.
#define PIEFX_SETTINGS_NAME	"pieFX Settings"

//	Named pipes the overlay connects to. Native is the server on both.
//
//	ONE PIPE PER DIRECTION, deliberately. A single duplex pipe opened WITHOUT
//	FILE_FLAG_OVERLAPPED has its I/O serialised by Windows, so a reader thread
//	parked in ReadFile blocks a WriteFile issued from another thread. With the
//	write coming from AE's UI thread that is an instant freeze on the first
//	summon - which is exactly what a single duplex pipe did here. Splitting the
//	directions means no handle ever has both a read and a write outstanding, and
//	it keeps both sides on plain synchronous I/O.
#define PIEFX_PIPE_TX		"\\\\.\\pipe\\pieFX"		// native -> overlay (events)
#define PIEFX_PIPE_RX		"\\\\.\\pipe\\pieFX-cmd"	// overlay -> native (actions)

//	Overlay executable, looked for beside the .aex (best-effort launch; the
//	overlay also retries connecting, so during dev it can be run by hand).
#define PIEFX_OVERLAY_EXE	"pieFX-overlay.exe"

//	Hold threshold. Now the DEFAULT rather than the value: settings.json may
//	carry `gesture.holdMs`, which the plug-in reads at launch into S_hold_ms.
//	The overlay cannot own this one — the hold is detected in the mouse hook,
//	before the overlay is involved at all.
#define PIEFX_HOLD_MS		200

//	Where the settings the overlay writes are read back from. The plug-in reads
//	exactly two fields out of it (`armOnLaunch` and `holdMs`) because those are
//	the two that have to be honoured BEFORE the overlay exists. Everything else
//	in the file belongs to the overlay, which owns the slot tree.
#define PIEFX_SETTINGS_REL	"pieFX\\settings.json"

//	Wheel geometry, in screen px. Used only by the legacy 3x3 POC path; the
//	hexagon wheel does its hit-testing overlay-side (it owns the geometry it
//	draws) and sends back a finished action, so this duplication ends there.
#define PIEFX_CELL			76
#define PIEFX_GAP			8

//	Action plumbing. Free text (script source, paths, match names) crosses the
//	pipe base64-encoded - see SETTINGS.md. The native side hand-rolls its JSON,
//	and a correct unescaper for arbitrary user script is exactly the kind of
//	thing that works until someone puts a quote in a string literal.
#define PIEFX_LINE_MAX		8192
#define PIEFX_TEXT_MAX		4096
#define PIEFX_B64_MAX		6144
#define PIEFX_QUEUE_LEN		8

// Exported through the PiPL (.r file)
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;
