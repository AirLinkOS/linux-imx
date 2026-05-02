#!/usr/bin/env python3
"""
replay_hunk.py — investigate past kernel merge conflict resolutions.

Given a merge JSON produced by kernel_merge.py, reconstruct everything you
need to evaluate a specific hunk's resolution: NXP's source file before and
after the merge, mainline's file before and after the merge, the conflict
itself, the prompt sent to the LLM, and the AI's response.

Workflow:
    # See all low-confidence hunks across all merges
    ./replay_hunk.py --list --max-confidence 6

    # Replay a specific hunk by global resolution index from --list output
    ./replay_hunk.py --merge 6.1.63 --resolution 2

    # Or replay by file + hunk index when you already know what you want
    ./replay_hunk.py --merge 6.1.63 --file drivers/media/i2c/ov5640.c --hunk 1

Output goes to <repo>/merge-replays/<version>_<file-slug>_hunk<N>/ by default,
or wherever --output-dir points.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Defaults — adjust if your layout differs
# ---------------------------------------------------------------------------

DEFAULT_REPO = Path.home() / "repos" / "linux-imx"
DEFAULT_LOG_DIR_NAME = "merge-logs"
DEFAULT_REPLAY_DIR_NAME = "merge-replays"


# ---------------------------------------------------------------------------
# JSON discovery and loading
# ---------------------------------------------------------------------------

def find_merge_json(log_dir: Path, version: str) -> Path:
    """Find the most recent merge_<version>_*.json file in log_dir.

    If multiple JSON files exist for the same version (e.g. the merge was
    re-run after a manual fix), picks the most recent by mtime.
    """
    pattern = f"merge_{version}_*.json"
    candidates = sorted(log_dir.glob(pattern),
                        key=lambda p: p.stat().st_mtime,
                        reverse=True)
    if not candidates:
        raise FileNotFoundError(
            f"No merge JSON for version {version} in {log_dir}\n"
            f"Looked for pattern: {pattern}"
        )
    if len(candidates) > 1:
        print(f"Note: {len(candidates)} JSONs for v{version}, using most "
              f"recent: {candidates[0].name}", file=sys.stderr)
    return candidates[0]


def load_json(path: Path) -> dict:
    with open(path) as f:
        return json.load(f)


def all_merge_jsons(log_dir: Path) -> list:
    """Return all merge_*.json files in log_dir, sorted by version."""
    files = list(log_dir.glob("merge_*.json"))

    def sort_key(p):
        m = re.match(r"merge_(\d+)\.(\d+)\.(\d+)_", p.name)
        if not m:
            return (0, 0, 0, p.name)
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)), p.name)

    return sorted(files, key=sort_key)


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------

def run_git(args: list, repo: Path) -> str:
    """Run git in repo and return stdout. Empty string on failure."""
    result = subprocess.run(
        ["git", "-C", str(repo)] + args,
        capture_output=True, text=True
    )
    if result.returncode != 0:
        return ""
    return result.stdout


def file_at_ref(repo: Path, ref: str, file_path: str) -> str:
    """Read a file at a specific git ref. Returns empty string if missing."""
    return run_git(["show", f"{ref}:{file_path}"], repo)


def heuristic_parent_nxp_sha(repo: Path, version: str) -> str:
    """Fallback: find the parent NXP SHA when JSON didn't store one.

    Looks for the merge commit message produced by kernel_merge.py and walks
    back to find the merge commit's first parent.
    """
    # Try the AI-assisted message first (conflicted merges)
    log = run_git([
        "log", "--all", "--oneline",
        f"--grep=Merge v{version} into",
    ], repo)
    if log:
        sha = log.split("\n")[0].split()[0]
        # First parent is what MAIN_BRANCH looked like before the merge
        parent = run_git(["rev-parse", f"{sha}^1"], repo).strip()
        if parent:
            return parent

    # Try the default git message (clean merges)
    log = run_git([
        "log", "--all", "--oneline",
        f"--grep=Merge tag 'v{version}'",
    ], repo)
    if log:
        sha = log.split("\n")[0].split()[0]
        parent = run_git(["rev-parse", f"{sha}^1"], repo).strip()
        if parent:
            return parent

    return ""


# ---------------------------------------------------------------------------
# Resolution lookup
# ---------------------------------------------------------------------------

def find_resolution(merge_data: dict, *, resolution_index: int = None,
                    file_path: str = None, hunk_index: int = None) -> dict:
    """Locate a specific resolution by either global index or (file, hunk)."""
    resolutions = merge_data.get("resolutions", [])

    if resolution_index is not None:
        if not 0 <= resolution_index < len(resolutions):
            raise IndexError(
                f"Resolution index {resolution_index} out of range "
                f"[0, {len(resolutions)})"
            )
        return resolutions[resolution_index]

    if file_path is None or hunk_index is None:
        raise ValueError("Must specify either --resolution or both --file and --hunk")

    for res in resolutions:
        if res["file"] == file_path and res["hunk_index"] == hunk_index:
            return res

    raise LookupError(
        f"No resolution for file={file_path}, hunk={hunk_index}.\n"
        f"Available resolutions in this merge:\n" +
        "\n".join(
            f"  [{i}] {r['file']} hunk {r['hunk_index']} (conf {r['confidence']})"
            for i, r in enumerate(resolutions)
        )
    )


# ---------------------------------------------------------------------------
# Filename helpers
# ---------------------------------------------------------------------------

def file_extension(file_path: str) -> str:
    """Get the file extension including the dot, e.g. '.c'. Empty if none."""
    name = Path(file_path).name
    if "." not in name:
        return ""
    # Use suffix from pathlib but handle edge cases like '.foo.bar'
    return Path(file_path).suffix


def file_slug(file_path: str) -> str:
    """Make a filesystem-safe slug from a file path."""
    # drivers/media/i2c/ov5640.c -> drivers-media-i2c-ov5640.c
    return file_path.replace("/", "-")


def previous_version(version: str) -> str:
    """v6.1.63 -> 6.1.62 (sublevel - 1)."""
    parts = version.split(".")
    parts[-1] = str(int(parts[-1]) - 1)
    return ".".join(parts)


# ---------------------------------------------------------------------------
# Replay output construction
# ---------------------------------------------------------------------------

def write_replay(repo: Path, output_dir: Path, merge_data: dict,
                 res: dict) -> None:
    """Materialize the replay artifacts for a single resolution into output_dir."""
    output_dir.mkdir(parents=True, exist_ok=True)

    version = merge_data["version"]
    prev = previous_version(version)
    file_path = res["file"]
    hunk_index = res["hunk_index"]
    ext = file_extension(file_path)

    # Resolve git refs
    parent_nxp_sha = merge_data.get("parent_nxp_sha")
    merge_commit_sha = merge_data.get("merge_commit_sha")

    if not parent_nxp_sha:
        print("Warning: JSON has no parent_nxp_sha (older log format). "
              "Falling back to heuristic SHA discovery — may be inaccurate.",
              file=sys.stderr)
        parent_nxp_sha = heuristic_parent_nxp_sha(repo, version)
        if not parent_nxp_sha:
            print(f"Warning: Could not heuristically find parent SHA for "
                  f"v{version}. linux-imx-{prev}{ext} will be empty.",
                  file=sys.stderr)

    if not merge_commit_sha:
        # Fallback: try to find the merge commit on MAIN_BRANCH
        # by looking for one whose first parent is parent_nxp_sha and
        # whose second parent is v{version}.
        if parent_nxp_sha:
            log = run_git([
                "log", "--all", "--format=%H",
                "--ancestry-path", f"{parent_nxp_sha}..v{version}",
            ], repo)
            # Not foolproof; user gets a warning if linux-imx-<version>
            # turns out empty.

    # Pull the four file snapshots
    nxp_before = file_at_ref(repo, parent_nxp_sha, file_path) if parent_nxp_sha else ""
    nxp_after = file_at_ref(repo, merge_commit_sha, file_path) if merge_commit_sha else ""
    mainline_before = file_at_ref(repo, f"v{prev}", file_path)
    mainline_after = file_at_ref(repo, f"v{version}", file_path)

    # Write the four snapshots
    snapshots = {
        f"linux-imx-{prev}{ext}": nxp_before,
        f"linux-imx-{version}{ext}": nxp_after,
        f"mainline-{prev}{ext}": mainline_before,
        f"mainline-{version}{ext}": mainline_after,
    }
    for name, content in snapshots.items():
        target = output_dir / name
        if content:
            target.write_text(content)
        else:
            target.write_text(f"# (empty — could not retrieve {name} from git)\n")

    # Conflict reconstruction. Some old JSONs may not have full_conflict /
    # context_before / context_after — synthesize from what we have.
    conflict_text = build_conflict_text(res, version, prev)
    (output_dir / "conflict.txt").write_text(conflict_text)

    # AI prompt and response
    (output_dir / "prompt.txt").write_text(build_prompt_text(res, version, prev))

    response = {
        "confidence": res.get("confidence"),
        "reasoning": res.get("reasoning"),
        "resolved_code": res.get("resolved_code"),
        "api_response_raw": res.get("api_response_raw"),
    }
    (output_dir / "response.json").write_text(
        json.dumps(response, indent=2)
    )

    # Verification (if any)
    if res.get("verification"):
        (output_dir / "verification.json").write_text(
            json.dumps(res["verification"], indent=2)
        )

    # Superseded attempts (if any) — these are previous resolutions that
    # were rejected by the verifier and replaced.
    superseded = res.get("superseded_attempts", [])
    if superseded:
        (output_dir / "superseded_attempts.json").write_text(
            json.dumps(superseded, indent=2)
        )

    # README pulling it all together
    readme = build_readme(merge_data, res, version, prev, ext, output_dir)
    (output_dir / "README.txt").write_text(readme)


def build_conflict_text(res: dict, version: str, prev: str) -> str:
    """Build a human-readable conflict reconstruction."""
    lines = [
        f"# Conflict in {res['file']}, hunk {res['hunk_index']}",
        f"# Approximate file location: lines {res.get('start_line', '?')}"
        f"-{res.get('end_line', '?')}",
        f"# Confidence: {res.get('confidence', '?')}/10",
        "",
    ]

    if res.get("context_before"):
        lines.append("# ---- CONTEXT BEFORE ----")
        lines.append(res["context_before"].rstrip("\n"))
        lines.append("")

    lines.append(f"<<<<<<< HEAD (linux-imx-{prev}: NXP's vendor side)")
    lines.append(res.get("ours", "").rstrip("\n"))
    lines.append("=======")
    lines.append(res.get("theirs", "").rstrip("\n"))
    lines.append(f">>>>>>> v{version} (mainline-{version}: stable side)")

    if res.get("context_after"):
        lines.append("")
        lines.append("# ---- CONTEXT AFTER ----")
        lines.append(res["context_after"].rstrip("\n"))

    return "\n".join(lines) + "\n"


def build_prompt_text(res: dict, version: str, prev: str) -> str:
    """Approximate the prompt that was sent to the LLM.

    The original prompt isn't stored verbatim in the JSON, so this is a
    reconstruction of what the prompt looked like based on the conflict
    content. Useful for understanding what the AI saw when making its
    decision.
    """
    return f"""# This is an APPROXIMATION of the prompt sent to the LLM.
# (The exact prompt isn't stored in the merge JSON.)
# It is reconstructed from the stored conflict content.

Resolve this merge conflict in the Linux kernel.

File: {res['file']}

We are merging mainline stable v{version} into NXP's vendor kernel.

Context BEFORE the conflict:
```
{res.get('context_before', '(not stored)')}
```

CONFLICT (hunk {res['hunk_index'] + 1}):
OUR side (NXP vendor kernel):
```
{res.get('ours', '')}
```

THEIR side (mainline stable):
```
{res.get('theirs', '')}
```

Context AFTER the conflict:
```
{res.get('context_after', '(not stored)')}
```

Resolve this conflict. Return ONLY the JSON object.
"""


def build_readme(merge_data: dict, res: dict, version: str, prev: str,
                 ext: str, output_dir: Path) -> str:
    """Build a README summarizing the replay and pointing at useful diffs."""
    file_path = res["file"]
    hunk_index = res["hunk_index"]
    confidence = res.get("confidence", "?")
    verdict = (res.get("verification") or {}).get("verdict", "(not verified)")
    summary = (res.get("verification") or {}).get("summary", "")

    n_superseded = len(res.get("superseded_attempts", []))
    superseded_note = (
        f"\nNote: This resolution had {n_superseded} previous attempt(s) "
        f"that the verifier rejected as critical. See superseded_attempts.json."
        if n_superseded else ""
    )

    return f"""# Merge replay for {file_path}, hunk {hunk_index}
# Generated by replay_hunk.py

Merge step:        v{prev} -> v{version}
File:              {file_path}
Hunk index:        {hunk_index}
File location:     lines {res.get('start_line', '?')}-{res.get('end_line', '?')}
AI confidence:     {confidence}/10
Verifier verdict:  {verdict}
Verifier summary:  {summary}{superseded_note}

Files in this directory:

  linux-imx-{prev}{ext}      NXP's vendor file BEFORE the merge ('ours' side)
  linux-imx-{version}{ext}      NXP's vendor file AFTER the merge (post-resolution)
  mainline-{prev}{ext}      Mainline stable file BEFORE this stable update
  mainline-{version}{ext}      Mainline stable file AFTER this stable update ('theirs' side)
  conflict.txt              The conflict markers + surrounding context
  prompt.txt                Approximate prompt sent to the LLM
  response.json             AI's resolution: confidence, reasoning, resolved code
  verification.json         Verifier's verdict (if verification ran)
  superseded_attempts.json  Previous rejected resolutions, if any

Useful diffs (run from this directory):

  # What did mainline change in this file across the version step?
  diff -u mainline-{prev}{ext} mainline-{version}{ext}

  # How much had NXP diverged from mainline at the FROM version?
  diff -u mainline-{prev}{ext} linux-imx-{prev}{ext}

  # What did the merge actually do to NXP's tree?
  diff -u linux-imx-{prev}{ext} linux-imx-{version}{ext}

  # Did the merged result match mainline's intent?
  diff -u mainline-{version}{ext} linux-imx-{version}{ext}

  # Just look at the conflict region:
  cat conflict.txt
  cat response.json
"""


# ---------------------------------------------------------------------------
# --list mode
# ---------------------------------------------------------------------------

def list_resolutions(log_dir: Path, max_confidence: int = None,
                     verifier_only: bool = False) -> None:
    """Print all resolutions across all merges, sorted by confidence asc."""
    rows = []
    for json_path in all_merge_jsons(log_dir):
        try:
            data = load_json(json_path)
        except Exception as e:
            print(f"Warning: could not parse {json_path.name}: {e}",
                  file=sys.stderr)
            continue

        version = data.get("version", "?")
        for idx, res in enumerate(data.get("resolutions", [])):
            conf = res.get("confidence", 0)
            if max_confidence is not None and conf > max_confidence:
                continue
            verdict = (res.get("verification") or {}).get("verdict")
            if verifier_only and verdict not in ("critical", "concerns"):
                continue
            rows.append({
                "version": version,
                "resolution_index": idx,
                "file": res["file"],
                "hunk": res["hunk_index"],
                "confidence": conf,
                "verdict": verdict or "",
            })

    rows.sort(key=lambda r: (r["confidence"], r["version"], r["resolution_index"]))

    if not rows:
        print("No resolutions match the filter.")
        return

    # Pretty-print
    max_file_len = max(len(r["file"]) for r in rows)
    max_file_len = min(max_file_len, 60)  # cap for readability
    for r in rows:
        verdict_str = ""
        if r["verdict"] == "critical":
            verdict_str = " 🔴 critical"
        elif r["verdict"] == "concerns":
            verdict_str = " 🟡 concerns"
        elif r["verdict"]:
            verdict_str = f" ({r['verdict']})"

        file_display = r["file"]
        if len(file_display) > max_file_len:
            file_display = "..." + file_display[-(max_file_len - 3):]

        print(
            f"[{r['version']:<8} #{r['resolution_index']:>2}]  "
            f"conf={r['confidence']:>2}  "
            f"{file_display:<{max_file_len}}  "
            f"hunk {r['hunk']}{verdict_str}"
        )

    print(f"\n{len(rows)} resolution(s) shown. To replay any:")
    print(f"  ./replay_hunk.py --merge <version> --resolution <index>")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Replay a kernel merge conflict resolution for review",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Workflow:", 1)[1] if "Workflow:" in __doc__ else ""
    )
    parser.add_argument("--repo", type=Path, default=DEFAULT_REPO,
                        help=f"Path to linux-imx repo (default: {DEFAULT_REPO})")
    parser.add_argument("--log-dir", type=Path, default=None,
                        help="Path to merge-logs directory "
                             "(default: <repo>/merge-logs)")
    parser.add_argument("--output-dir", type=Path, default=None,
                        help="Where to write replay artifacts "
                             "(default: <repo>/merge-replays/<auto>)")

    # List mode
    parser.add_argument("--list", action="store_true",
                        help="List all resolutions across all merges")
    parser.add_argument("--max-confidence", type=int, default=None,
                        help="With --list: only show resolutions with "
                             "confidence <= this value")
    parser.add_argument("--verifier-only", action="store_true",
                        help="With --list: only show resolutions the verifier "
                             "flagged as concerns or critical")

    # Replay mode — version always required, then either resolution index or
    # file+hunk
    parser.add_argument("--merge", type=str,
                        help="Merge version to replay from, e.g. 6.1.63")
    parser.add_argument("--resolution", type=int, default=None,
                        help="Global resolution index in the merge "
                             "(0-based; see --list output)")
    parser.add_argument("--file", type=str, default=None,
                        help="File path of the conflicted file "
                             "(use with --hunk)")
    parser.add_argument("--hunk", type=int, default=None,
                        help="Hunk index within the file (0-based)")

    args = parser.parse_args()

    repo = args.repo.resolve()
    log_dir = args.log_dir or (repo / DEFAULT_LOG_DIR_NAME)

    if not repo.exists():
        print(f"Error: repo not found: {repo}", file=sys.stderr)
        sys.exit(1)
    if not log_dir.exists():
        print(f"Error: log dir not found: {log_dir}", file=sys.stderr)
        sys.exit(1)

    # --list mode
    if args.list:
        list_resolutions(log_dir, max_confidence=args.max_confidence,
                         verifier_only=args.verifier_only)
        return

    # Replay mode
    if not args.merge:
        parser.error("Either --list or --merge is required")

    if args.resolution is None and (args.file is None or args.hunk is None):
        parser.error("With --merge, specify either --resolution or both "
                     "--file and --hunk")

    json_path = find_merge_json(log_dir, args.merge)
    merge_data = load_json(json_path)

    res = find_resolution(
        merge_data,
        resolution_index=args.resolution,
        file_path=args.file,
        hunk_index=args.hunk,
    )

    # Build output directory
    if args.output_dir:
        output_dir = args.output_dir.resolve()
    else:
        slug = file_slug(res["file"])
        dirname = f"{args.merge}_{slug}_hunk{res['hunk_index']}"
        output_dir = repo / DEFAULT_REPLAY_DIR_NAME / dirname

    # Wipe existing output dir to keep things idempotent
    if output_dir.exists():
        shutil.rmtree(output_dir)

    write_replay(repo, output_dir, merge_data, res)

    print(f"\nReplay written to: {output_dir}\n")
    print(f"  Open the README:    cat {output_dir}/README.txt")
    print(f"  See the conflict:   cat {output_dir}/conflict.txt")
    print(f"  See AI's response:  cat {output_dir}/response.json")


if __name__ == "__main__":
    main()