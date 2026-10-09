# mat_queue_mode on the Metal renderer (2026-10-08): no gain to ship

Question: does forcing Source's multithreaded rendering (`mat_queue_mode 2`) give tf2mt users a noticeable boost? tf2mt runs on the main game thread under Rosetta, which is the bottleneck.

Method: `tools/bench/queue-ab.sh` plays the benchmark demo on tf2mt, uncapped (`TF2MT_VSYNC=0`), once per mode, with a fresh process each time. `tools/bench/queue-summary.py` summarises it and skips the first 20 s.

| mode | fps | p50 | p99 | 1 % low | spikes > 20 ms/min |
|---|---|---|---|---|---|
| -1 auto (default; mastercomfig sets -1), run 1 | 255.8 | 3.53 ms | 9.34 ms | 26.4 | 2.0 |
| -1 auto, run 2 | 266.0 | 3.21 ms | 9.41 ms | 26.9 | 2.3 |
| 2 forced, run 1 | 270.4 | 3.10 ms | 8.80 ms | 22.6 | 10.7 (did not repeat) |
| 2 forced, run 2 | 268.0 | 3.21 ms | 9.29 ms | 27.0 | 2.0 |
| 0 single-threaded | 194.3 | 4.97 ms | 11.75 ms | 21.0 | 1.4 |

**Conclusion:** auto already resolves to queued rendering on these Macs (+~35 % over mode 0). Forcing 2 is within run-to-run noise of auto. Nothing to change.

Side note: run 1 of auto crashed at demo start with a null read in `engine.dll+0x960b3` (Steam dump `crash_tf_win64.exe_20261008233019_1.dmp`). This is TF2 code, not tf2mt, and three later runs were clean. Watch for repeats.
