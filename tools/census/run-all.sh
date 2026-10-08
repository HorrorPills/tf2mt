#!/bin/bash
# All census variants (one TF2 session each). ~15 min per variant.
cd "$(dirname "$0")"
./run-census.sh base
./run-census.sh msaa4  +mat_antialias 4 +mat_aaquality 0
./run-census.sh hdr2   +mat_hdr_level 2
./run-census.sh queue0 +mat_queue_mode 0
