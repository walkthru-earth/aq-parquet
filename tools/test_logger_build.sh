#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixture="${repo_root}/firmware/common/logger/tests/compile"
"${repo_root}/tools/idf.sh" -C "${fixture}" -B "${fixture}/build" build
# The fixture has no board target, so check only the per-file C++ dialect.
python "${repo_root}/tools/check_idf_target.py" --compile-dialect "${fixture}/build"
python - "${fixture}/build/project_description.json" <<'CHECK'
import json, sys
components = json.load(open(sys.argv[1]))["build_components"]
assert not any("arduino" in name.lower() or "m5" in name.lower() for name in components), components
print("Native generic fixture has no Arduino or M5 dependency")
CHECK
