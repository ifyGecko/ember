#!/usr/bin/env bash
# install.sh - symlink ember's skills (the folders next to this script) into the skills
# directory of each agent harness. Idempotent: re-running updates the links, including links
# left by an earlier install from another checkout. Safe to run from anywhere.
#
# Usage: install.sh [--claude] [--qwen] [--agents] [--uninstall]
#   no flag     every harness found on this machine (Claude Code if ~/.claude or `claude`
#               exists, Qwen Code if its home dir or `qwen` exists); Claude Code if none is
#   --claude    Claude Code: $CLAUDE_SKILLS_DIR, default ~/.claude/skills
#   --qwen      Qwen Code:   $QWEN_SKILLS_DIR, default $QWEN_HOME/skills or ~/.qwen/skills
#               (Qwen reads SKILL.md skills from there and follows symlinked skill dirs)
#   --agents    the harness-neutral Agent Skills directory: $AGENTS_SKILLS_DIR, default
#               ~/.agents/skills (Qwen Code reads it too: use it or --qwen, not both)
#   --uninstall remove the links this script made (only links into this directory)
set -euo pipefail
SKILLS_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILLS=(ember-dev-recon ember-dev-build ember-fuzz)
CLAUDE_DEST="${CLAUDE_SKILLS_DIR:-$HOME/.claude/skills}"
QWEN_DEST="${QWEN_SKILLS_DIR:-${QWEN_HOME:-$HOME/.qwen}/skills}"
AGENTS_DEST="${AGENTS_SKILLS_DIR:-$HOME/.agents/skills}"

targets=(); uninstall=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --claude) targets+=(claude); shift;;
        --qwen) targets+=(qwen); shift;;
        --agents) targets+=(agents); shift;;
        --uninstall) uninstall=1; shift;;
        -h|--help) sed -n '2,15p' "$0"; exit 0;;
        *) echo "error: unknown argument: $1 (see --help)" >&2; exit 1;;
    esac
done
if [[ ${#targets[@]} -eq 0 ]]; then
    { [[ -d "$HOME/.claude" ]] || command -v claude >/dev/null 2>&1; } && targets+=(claude)
    { [[ -d "${QWEN_HOME:-$HOME/.qwen}" ]] || command -v qwen >/dev/null 2>&1; } && targets+=(qwen)
    [[ ${#targets[@]} -gt 0 ]] || targets=(claude)
fi

# Each skill must be a folder whose SKILL.md frontmatter has a name equal to the folder name
# and a description of 1-1024 characters (the Agent Skills format both harnesses read).
check_skill() {  # $1 = skill folder
    python3 -I - "$1" <<'EOF' || exit 1
import os, re, sys
d = sys.argv[1]; f = os.path.join(d, "SKILL.md")
try:
    t = open(f, encoding="utf-8").read()
except OSError as e:
    sys.exit(f"error: {f}: {e}")
m = re.match(r"^---\n(.*?)\n---(?:\n|$)", t.replace("\r\n", "\n"), re.S)
if not m:
    sys.exit(f"error: {f}: no YAML frontmatter")
fm = m.group(1)
def field(k):
    mm = re.search(rf"^{k}:[ \t]*(.*?)(?=^[A-Za-z][\w-]*:|\Z)", fm, re.S | re.M)
    v = mm.group(1).strip() if mm else ""
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        v = v[1:-1].replace('\\"', '"') if v[0] == '"' else v[1:-1].replace("''", "'")
    return v
name, desc = field("name"), field("description")
if name != os.path.basename(d.rstrip("/")) or not re.fullmatch(r"(?!.*--)[a-z0-9](?:[a-z0-9-]*[a-z0-9])?", name) or len(name) > 64:
    sys.exit(f"error: {f}: name '{name}' must equal the folder name, lowercase letters, digits and -")
if not 1 <= len(desc) <= 1024:
    sys.exit(f"error: {f}: description is {len(desc)} characters (1-1024)")
EOF
}

link_skills() {  # $1 = harness name, $2 = skills directory
    local dest="$2" skill src link
    mkdir -p "$dest"
    for skill in "${SKILLS[@]}"; do
        src="$SKILLS_SRC/$skill"
        link="$dest/$skill"
        [[ -d "$src" ]] || { echo "error: missing skill source: $src" >&2; exit 1; }
        check_skill "$src"
        # Replace an existing link (e.g. from an older checkout); leave a real dir alone.
        if [[ -L "$link" ]]; then
            rm -f "$link"
        elif [[ -e "$link" ]]; then
            echo "error: $link exists and is not a symlink; refusing to overwrite" >&2
            exit 1
        fi
        ln -s "$src" "$link"
        echo "linked $link -> $src"
    done
    echo "done ($1). Installed skills: ${SKILLS[*]}"
}

unlink_skills() {  # $1 = harness name, $2 = skills directory
    local dest="$2" skill link
    for skill in "${SKILLS[@]}"; do
        link="$dest/$skill"
        if [[ -L "$link" && "$(readlink "$link")" == "$SKILLS_SRC/$skill" ]]; then
            rm -f "$link"
            echo "removed $link"
        fi
    done
    echo "done ($1)."
}

for t in "${targets[@]}"; do
    case "$t" in
        claude) name="Claude Code"; dest="$CLAUDE_DEST";;
        qwen) name="Qwen Code"; dest="$QWEN_DEST";;
        agents) name="Agent Skills"; dest="$AGENTS_DEST";;
    esac
    if [[ $uninstall -eq 1 ]]; then unlink_skills "$name" "$dest"; else link_skills "$name" "$dest"; fi
done
