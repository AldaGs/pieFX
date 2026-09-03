# pieFX docs

**Paths in these files are relative to the repo root**, one level up. When a
document says `poc/native/pieFX.cpp` it means `../poc/native/pieFX.cpp` from
here; when it names another `.md` with no path, that file is in this folder.

Read in this order if you are picking the project up cold:

| | |
|---|---|
| [HANDOFF.md](HANDOFF.md) | **Start here.** The product handoff: what is proven, what is not, the bugs that cost real sessions, the settled decisions, and how to build, package and install on Windows. |
| [../poc/README.md](../poc/README.md) | Build, run and verify the two halves by hand. |
| [../poc/SETTINGS.md](../poc/SETTINGS.md) | The action model and the settings file format — what a slot can be bound to. |
| [ARCHITECTURE.md](ARCHITECTURE.md) | The locked two-process design, and why it is two processes. |
| [HANDOFF_MAC.md](HANDOFF_MAC.md) | The macOS handoff. Read it before touching either platform: the shared bodies mean a Mac change is often a Windows change. |
| [SPIKES.md](SPIKES.md) | The Phase 0 log. Still the record of which obvious-looking approaches are known **not** to work, which is why it has not been deleted. |

The macOS port, in the order it happened:

| | |
|---|---|
| [MAC_PORT.md](MAC_PORT.md) | What porting the product was going to take, written before it was done. |
| [MAC_SESSION.md](MAC_SESSION.md) | The checklist for the first macOS session, written on Windows and executed once. |
| [MAC_RESULTS.md](MAC_RESULTS.md) | What the bench actually found — the measurements, in the order they were made. This is the one with the surprises in it. |

Also worth knowing about, kept next to the code they describe:

| | |
|---|---|
| [../_archive/phase0-spike-win/README.md](../_archive/phase0-spike-win/README.md) | The retired Phase 0 spike plug-in: what it is still good for, and why every name in it had to change. |
