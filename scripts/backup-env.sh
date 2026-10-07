#!/bin/bash
# instant APFS copy-on-write backup of the whole game environment
. "$(dirname "$0")/_env.sh"; "$WINESERVER" -k 2>/dev/null
D="$TF2_HOME.backup-$(date +%Y%m%d-%H%M%S)"; cp -cR "$TF2_HOME" "$D" && echo "backup: $D"
