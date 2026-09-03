//	pieFX — does findMenuCommandId speak English on a localised After Effects?
//
//	The whole ae-command half of the wheel is bound by NAME, because a name can
//	be validated at the moment the binding is made — app.findMenuCommandId
//	returns 0 for a spelling that does not exist — while an id cannot be, and
//	silently rots into some OTHER command when it drifts.
//
//	That design assumes AE will resolve the names pieFX ships. On a localised
//	install it may not, and if it does not, every binding falls back to its id
//	and the safety net is gone. This answers that, on this machine.
//
//	It tests BOTH English and Spanish spellings on purpose. All-zeros from an
//	English-only test would be ambiguous — a broken script looks exactly the
//	same — so the Spanish column is the control that makes a zero meaningful.
//
//	Run it: File > Scripts > Run Script File..., pick this file.
//
(function () {
	function tryName(n) {
		try {
			var id = app.findMenuCommandId(n);
			return id ? id : 0;
		} catch (e) {
			return -1;			// threw, which is not the same as "not found"
		}
	}

	//	Each row is one COMMAND, with the spellings worth trying for it. The
	//	Spanish ones are best guesses; a miss on all of them is informative
	//	only when the English one also misses, which is why both are shown.
	var rows = [
		["Add to Render Queue",
			["Add to Render Queue", "Add To Render Queue"],
			["Añadir a la cola de procesamiento", "Agregar a la cola de procesamiento"]],
		["Composition Settings...",
			["Composition Settings...", "Composition Settings"],
			["Ajustes de composición...", "Configuración de composición..."]],
		["Pre-compose...",
			["Pre-compose...", "Precompose...", "Precompose"],
			["Precomponer...", "Precomponer"]],
		["New Composition...",
			["New Composition...", "New Composition"],
			["Composición nueva...", "Nueva composición..."]],
		["Adjustment Layer",
			["Adjustment Layer"],
			["Capa de ajuste"]]
	];

	var out = [];

	out.push("pieFX locale probe");
	out.push("");
	try {
		out.push("app.language     : " + app.language);
	} catch (e) { out.push("app.language     : (unavailable)"); }
	try {
		out.push("app.isoLanguage  : " + app.isoLanguage);
	} catch (e) { out.push("app.isoLanguage  : (unavailable)"); }
	out.push("app.version      : " + app.version);
	out.push("");
	out.push("command                       english   spanish");
	out.push("--------------------------------------------------");

	var anyEnglish = 0, anySpanish = 0;

	for (var i = 0; i < rows.length; i++) {
		var label = rows[i][0], en = rows[i][1], es = rows[i][2];
		var enId = 0, esId = 0, j;

		for (j = 0; j < en.length && !enId; j++) { enId = tryName(en[j]); if (enId < 0) enId = 0; }
		for (j = 0; j < es.length && !esId; j++) { esId = tryName(es[j]); if (esId < 0) esId = 0; }

		if (enId) { anyEnglish++; }
		if (esId) { anySpanish++; }

		var pad = label;
		while (pad.length < 30) { pad += " "; }
		var e1 = enId ? String(enId) : "  --";
		while (e1.length < 10) { e1 += " "; }
		out.push(pad + e1 + (esId ? String(esId) : "  --"));
	}

	out.push("");
	if (anyEnglish && !anySpanish) {
		out.push("VERDICT: English names resolve. findMenuCommandId is NOT localised,");
		out.push("         so pieFX's name-first bindings work as they do on Windows.");
	} else if (!anyEnglish && anySpanish) {
		out.push("VERDICT: only the LOCALISED names resolve. findMenuCommandId follows");
		out.push("         the UI language, so every English binding pieFX ships");
		out.push("         resolves to 0 here and falls back to its id.");
	} else if (anyEnglish && anySpanish) {
		out.push("VERDICT: BOTH resolve. Compare the two columns: if the ids differ,");
		out.push("         they are different commands and one column is coincidence.");
	} else {
		out.push("VERDICT: neither resolved. That is not a result yet - the Spanish");
		out.push("         spellings here are guesses, so this may just mean none of");
		out.push("         them is right. Check one by hand against AE's own menus.");
	}

	var text = out.join("\n");

	//	Written as well as shown: a dialog cannot be copied out of, and this is
	//	the kind of output that wants pasting into an issue.
	try {
		var f = new File(Folder.temp.fsName + "/pieFX_locale_probe.txt");

		f.open("w");
		f.write(text);
		f.close();
		text += "\n\n(also written to " + f.fsName + ")";
	} catch (e) { /* the dialog is still the result */ }

	alert(text);
	return text;
})();
