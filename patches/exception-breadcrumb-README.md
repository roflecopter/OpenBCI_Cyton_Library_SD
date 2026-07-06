# Forensic exception breadcrumb — WIP (deferred: flash-infeasible, 2026-07-06)

Captures a mid-recording MCU fault to LOCATE the hang: a strong `_general_exception_handler`
(`exception-breadcrumb-wip.c` → a sketch-dir `crash_handler.c`) stashes CP0 Cause/EPC to a `.noinit`
record + soft-resets; `setup()` reads it (see `exception-breadcrumb-defaultboard.diff`) and
SD_Card_Stuff.ino emits `exc=/epc=` in the next resumed `%BOOT`.

**Why it's not shipped:** it needs ~760 B — a strong `_general_exception_handler` defeats gc-sections'
stripping of the chipKIT core's exception-vector machinery. This 96%-full image has only ~492 B free
even after reclaiming every non-contract string → 268 B over. See `grill-breadcrumb-review-log.md`.

**To ship it later, first free ~300 B by one of:** (a) drop the accelerometer read/helpers; (b) reclaim
the reserved DEE/splitflash flash pages via linker-script surgery (BRICK RISK on this no-ICSP board —
needs its own investigation). Then restore `crash_handler.c` to the sketch dir, apply the DefaultBoard
diff, re-add the exc=/epc= %BOOT emit, and re-run the panel.
