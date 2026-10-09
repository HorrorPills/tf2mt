#!/bin/bash
# Per-module fps cost on the benchmark demo (uncapped): Medium preset with one module group set back to Low each run.
cd "$(dirname "$0")/../.."; . scripts/_env.sh
OV="$TF2DIR/tf/cfg/overrides/modules.cfg"; mkdir -p "$(dirname "$OV")"
[ -f "$OV" ] && cp "$OV" /tmp/tf2mt-modules.cfg.orig
orig=$(scripts/mastercomfig.sh status | sed -E 's/.*preset=([a-z]+).*/\1/')
cleanup() { rm -f "$OV"; [ -f /tmp/tf2mt-modules.cfg.orig ] && cp /tmp/tf2mt-modules.cfg.orig "$OV"; scripts/mastercomfig.sh set "$orig" >/dev/null 2>&1; echo "restored preset $orig, overrides removed"; }
trap cleanup EXIT
run() { name=$1; shift; printf '%s\n' "$@" > "$OV"; [ $# = 0 ] && rm -f "$OV"; tools/bench/night-run.sh "$name" VSYNC=0 2>&1 | tail -1; }
scripts/mastercomfig.sh set low >/dev/null 2>&1; run M-low
scripts/mastercomfig.sh set medium >/dev/null 2>&1; run M-medium
run M-med-shadows shadows=low
run M-med-lod lod=low characters=very_low
run M-med-lighting lighting=low shading=low phong=off
run M-med-effects effects=low tracers=low
run M-med-aa anti_aliasing=off
run M-med-physics ragdolls=off gibs=off jigglebones=off
run M-med-decals decals=off decals_models=off decals_art=off
run M-med-water water=low post_processing=off
run M-med-sound sound=low
run M-med-misc hud_player_model=off outlines=off ropes=off texture_filter=trilinear
