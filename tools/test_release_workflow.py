"""Host test that pins the safety rules of .github/workflows/release.yml (#45).

actionlint and zizmor catch unpinned actions and template injection, but not a
widened job permission, persisted checkout credentials, a cache, a secrets
reference, a lost tag gate, a lost --draft or a weaker tag pattern. check()
reads the workflow and returns one finding per broken rule. main() proves the
real file is clean and that each mutation in MUTATIONS gives a finding, so a
rule that stops matching anything fails here too.

PyYAML comes with the pinned pre-commit in the Pixi environment.
"""

from __future__ import annotations

import fnmatch
from pathlib import Path
import re
import sys

import yaml

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"

TAG_PATTERN = "^v[0-9A-Za-z._-]+$"
RELEASE_IF = "github.event_name == 'push' && github.ref_type == 'tag'"
SHA_PIN = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")
PERMISSIONS = {
    "build": {"contents": "read"},
    "assemble": {},
    "release": {"contents": "write", "id-token": "write", "attestations": "write"},
}


def steps(job: dict) -> list[dict]:
    return job.get("steps") or []


def uses(step: dict) -> str:
    return str(step.get("uses", "")).split("@", 1)[0]


def check(text: str) -> list[str]:
    """Every broken rule in the workflow text, empty when all hold."""
    found = []
    if "secrets." in text or "secrets[" in text:
        found.append("the workflow references secrets, it must only use github.token")
    try:
        doc = yaml.safe_load(text)
    except yaml.YAMLError as error:
        return [*found, f"not valid YAML, {error}"]
    # PyYAML reads the bare key `on` as True.
    triggers = doc.get("on", doc.get(True))
    if not isinstance(triggers, dict) or set(triggers) != {"push", "workflow_dispatch"}:
        found.append("triggers must be exactly push and workflow_dispatch")
    elif triggers["push"] != {"tags": ["v*"]}:
        found.append("push must trigger on v* tags only")
    if doc.get("permissions") != {}:
        found.append("top level permissions must be {}")
    jobs = doc.get("jobs") or {}
    if set(jobs) != set(PERMISSIONS):
        found.append(f"jobs must be exactly {sorted(PERMISSIONS)}")
    for name, want in PERMISSIONS.items():
        job = jobs.get(name) or {}
        if job.get("permissions") != want:
            found.append(f"job {name} permissions must be {want}")
    for name, job in jobs.items():
        for step in steps(job):
            ref = str(step.get("uses", ""))
            if ref and not SHA_PIN.match(ref):
                found.append(f"job {name} uses {ref} without a full commit SHA")
            if uses(step) == "actions/checkout" and (step.get("with") or {}).get("persist-credentials") is not False:
                found.append(f"job {name} checkout must set persist-credentials false")
            if uses(step) == "prefix-dev/setup-pixi":
                options = step.get("with") or {}
                if options.get("cache") is not False or "cache-write" in options:
                    found.append(f"job {name} setup-pixi must set cache false")
            if uses(step).startswith("actions/cache"):
                found.append(f"job {name} uses {uses(step)}, release builds take no cache")
    release = jobs.get("release") or {}
    if release.get("if") != RELEASE_IF:
        found.append(f"job release must run only if {RELEASE_IF}")
    for name, job in jobs.items():
        if name != "release" and "if" in job:
            found.append(f"job {name} must not be skipped")

    run = {name: "\n".join(str(step.get("run", "")) for step in steps(job)) for name, job in jobs.items()}
    for name in ("build", "release"):
        patterns = re.findall(r'=~ (\S+) \]\]', run.get(name, ""))
        if patterns != [TAG_PATTERN]:
            found.append(f"job {name} must check the tag once against {TAG_PATTERN}, found {patterns}")
    create = [line for line in run.get("release", "").splitlines() if "gh release" in line]
    if len(create) != 1 or "gh release create" not in create[0]:
        found.append("job release must call gh release only to create")
    release_run = run.get("release", "")
    for flag in ("--draft", "--verify-tag"):
        if not re.search(rf"^\s*{flag} \\$", release_run, re.M):
            found.append(f"gh release create must pass {flag}")
    if "--draft=false" in text or "--latest" in text:
        found.append("the workflow must never publish")
    order = [step.get("name", uses(step)) for step in steps(release)]
    wanted = ["Refuse an existing release", "Attest build provenance", "Create draft release"]
    if [step for step in order if step in wanted] != wanted:
        found.append(f"job release must run {wanted} in that order")
    guard = next((step for step in steps(release) if step.get("name") == wanted[0]), {})
    if 'grep -Fxq -- "${TAG}"' not in str(guard.get("run", "")) or "exit 1" not in str(guard.get("run", "")):
        found.append("the existing release guard must fail when the tag has a release")

    # The board artifacts and the joined artifact must not share a pattern.
    uploaded = {name: [str((step.get("with") or {}).get("name", "")) for step in steps(job)
                       if uses(step) == "actions/upload-artifact"] for name, job in jobs.items()}
    pattern = next((str(step["with"].get("pattern")) for step in steps(jobs.get("assemble") or {})
                    if uses(step) == "actions/download-artifact" and "pattern" in (step.get("with") or {})), "")
    boards = [n.replace("${{ matrix.name }}", "cores3") for n in uploaded.get("build", [])]
    if not pattern or not boards or not all(fnmatch.fnmatchcase(n, pattern) for n in boards):
        found.append("the assemble download pattern must match every board artifact")
    if any(fnmatch.fnmatchcase(n, pattern) for n in uploaded.get("assemble", [])):
        found.append(f"the assemble download pattern {pattern} also matches the joined artifact")
    return found


def check_tag_pattern() -> None:
    tag = re.compile(TAG_PATTERN)
    for good in ("v1.2.0", "v0.1.0-rc.1", "v2_beta"):
        assert tag.match(good), good
    for bad in ("v", "1.2.0", "v1/2", "v1 2", "v1;rm", "v1$(x)", "../v1", "v1\n"):
        assert not tag.fullmatch(bad), bad


# Each break must change the file and give at least one finding.
MUTATIONS = [
    ("build gets contents write", "    permissions:\n      contents: read\n",
     "    permissions:\n      contents: write\n"),
    ("checkout keeps credentials", "persist-credentials: false", "persist-credentials: true"),
    ("setup-pixi cache on", "cache: false", "cache: true"),
    ("secrets token", "GH_TOKEN: ${{ github.token }}\n          TAG",
     "GH_TOKEN: ${{ secrets.RELEASE_PAT }}\n          TAG"),
    ("release gate removed", f"    if: {RELEASE_IF}\n", ""),
    ("release gate widened", f"    if: {RELEASE_IF}\n", "    if: github.ref_type == 'tag'\n"),
    ("draft removed", "            --draft \\\n", ""),
    ("verify-tag removed", "            --verify-tag \\\n", ""),
    ("tag regex weakened", '=~ ^v[0-9A-Za-z._-]+$ ]]', '=~ ^v ]]'),
    ("publish added", '--notes "${notes}"', '--notes "${notes}"\n          gh release edit "${TAG}" --draft=false'),
    ("guard removed", 'if grep -Fxq -- "${TAG}" <<<"${tags}"; then', 'if false; then'),
    ("artifact pattern overlaps", "pattern: board-*", "pattern: '*'"),
    ("action unpinned", "actions/attest-build-provenance@4d101475d8b20a2381f78447822ac1eab6504dd8",
     "actions/attest-build-provenance@v4"),
    ("pull_request_target", "  workflow_dispatch:\n", "  workflow_dispatch:\n  pull_request_target:\n"),
    ("top level write-all", "\npermissions: {}\n", "\npermissions: write-all\n"),
    ("cache action", "      - name: Install pinned ESP-IDF\n",
     "      - uses: actions/cache/restore@55cc8345863c7cc4c66a329aec7e433d2d1c52a9\n"
     "        with:\n          path: ~/.espressif\n          key: k\n"
     "      - name: Install pinned ESP-IDF\n"),
]


def main() -> int:
    text = WORKFLOW.read_text(encoding="utf-8")
    clean = check(text)
    assert not clean, clean
    check_tag_pattern()
    for name, old, new in MUTATIONS:
        assert old in text, f"mutation {name} no longer applies, update it"
        found = check(text.replace(old, new))
        assert found, f"mutation {name} was not caught"
    print(f"release workflow tests passed, {len(MUTATIONS)} mutations caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
