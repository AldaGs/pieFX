//	pieFX — legacy AE text into UTF-8.
//
//	AEGP_GetEffectName and AEGP_GetEffectCategory take `A_char *` and there is
//	no Unicode alternative: AEGP_EffectSuite5 is the newest suite and both
//	accessors still have the single-byte signature. Two earlier documents in
//	this repo claimed otherwise; they were wrong and have been corrected. Do
//	not go looking for a wide version.
//
//	So the conversion is ours, and it has to happen at the ONE point those
//	strings leave the process: effects.json, which the overlay reads with
//	JSON.parse. Invalid UTF-8 there is not a graceful failure — it is the whole
//	catalogue failing to load, reported to the user as "no effects installed".
//
//	This is NOT a macOS bug being fixed. The same defect is latent on Windows
//	under any non-Latin locale, where AEGP hands back CP_ACP bytes and the same
//	JSON.parse rejects them; a Spanish Mac merely got there first. The Windows
//	half of the same conversion lives beside the call site in pieFX.cpp.
#ifndef PIEFX_TEXT_H
#define PIEFX_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//	`in` as UTF-8 in `out`. Returns 1 when `out` holds a faithful conversion,
//	0 when it does not — and 0 means the caller should use the effect's MATCH
//	name instead, which the SDK header marks `UTF8!!` and which is a stable,
//	non-localised identifier. A wrong name is worse than an unlocalised one.
int PieFX_LegacyToUtf8(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif	// PIEFX_TEXT_H
