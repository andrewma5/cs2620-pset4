#!/usr/bin/env python3
"""
Deploy a tm-server swarm test instance.

Creates:
  <target>/repo.git/                         (bare git repo, shared)
  <target>/agent-N/CLAUDE.md                 (env vars + run instructions)
  <target>/agent-N/.claude/skills/*.md       (task-loop, task-planning, ...)
  <target>/agent-N/.claude/settings.json     (default model: sonnet)

The skill files and CLAUDE.md template are copied from the swarm-deploy
skill's own assets/ directory (resolved relative to this script).

Usage:
    deploy.py <target-dir> [--agents N]

The target directory is created if it does not exist. If repo.git or
any agent-N directory already exists, deploy.py refuses to overwrite —
delete them by hand first.
"""

import argparse
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path


def _force_writable_then_retry(func, path, exc_info):
    """rmtree onerror: clear read-only bit (git pack files on Windows) and retry."""
    os.chmod(path, stat.S_IWRITE)
    func(path)


def rmtree_force(path: Path) -> None:
    shutil.rmtree(path, onerror=_force_writable_then_retry)


def find_assets_dir() -> Path:
    """Resolve the skill's assets/ directory from this script's location."""
    here = Path(__file__).resolve().parent
    assets = here.parent / "assets"
    if not assets.is_dir():
        sys.exit(f"error: cannot find assets/ next to scripts/ (looked in {assets})")
    return assets


def init_bare_repo(repo_dir: Path) -> None:
    """git init --bare <repo_dir>; seed an initial commit on main so agents can branch."""
    subprocess.run(["git", "init", "--bare", str(repo_dir)], check=True)
    seed = repo_dir.parent / "_seed_tmp"
    if seed.exists():
        rmtree_force(seed)
    subprocess.run(["git", "clone", str(repo_dir), str(seed)], check=True)
    subprocess.run(
        ["git", "-c", "user.email=human@swarm", "-c", "user.name=human",
         "commit", "--allow-empty", "-m", "initial commit"],
        cwd=seed, check=True,
    )
    # Push to main regardless of whether default is master or main.
    subprocess.run(["git", "branch", "-M", "main"], cwd=seed, check=True)
    subprocess.run(["git", "push", "origin", "main"], cwd=seed, check=True)
    rmtree_force(seed)


def to_posix(p: Path) -> str:
    """Path string with forward slashes — the agents' env vars are read by bash."""
    return str(p.resolve()).replace("\\", "/")


def write_agent(agent_dir: Path, agent_id: str, n_agents: int,
                repo_dir: Path, assets: Path, tm_url_list: str) -> None:
    agent_dir.mkdir(parents=True, exist_ok=False)

    # Both CLAUDE.md.template and tm.env.template use the same set of
    # {PLACEHOLDER} substitutions, so render them with one helper.
    def render(template_name: str) -> str:
        text = (assets / template_name).read_text(encoding="utf-8")
        return (text
                .replace("{AGENT_ID}", agent_id)
                .replace("{N_AGENTS}", str(n_agents))
                .replace("{TM_REPO}", to_posix(repo_dir))
                .replace("{TM_WORK}", to_posix(agent_dir))
                .replace("{TM_URL_LIST}", tm_url_list))

    (agent_dir / "CLAUDE.md").write_text(render("CLAUDE.md.template"),
                                         encoding="utf-8")
    (agent_dir / "tm.env").write_text(render("tm.env.template"),
                                      encoding="utf-8")

    dot_claude = agent_dir / ".claude"
    skills_dir = dot_claude / "skills"
    skills_dir.mkdir(parents=True)
    # Each bundled skill is a folder containing SKILL.md (and optionally
    # other files). Copy the whole folder so the deployed layout is
    # <agent>/.claude/skills/<skill-name>/SKILL.md — the format the
    # Claude CLI expects.
    src_skills = assets / "agent-skills"
    for skill_subdir in sorted(p for p in src_skills.iterdir() if p.is_dir()):
        shutil.copytree(skill_subdir, skills_dir / skill_subdir.name)

    # The bundled tm CLI + bash wrappers must be executable on Unix.
    # `shutil.copytree` preserves mode bits, but if a user added a file
    # from Windows it may lack +x. Force it here.
    task_loop_dir = skills_dir / "task-loop"
    for name in ("tm", "tm-hb.sh", "tm-wait.sh"):
        p = task_loop_dir / name
        if p.is_file():
            mode = p.stat().st_mode
            p.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    shutil.copy2(assets / "settings.json", dot_claude / "settings.json")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="directory to deploy the swarm into")
    ap.add_argument("--agents", "-n", type=int, default=3,
                    help="number of agent folders to create (default: 3)")
    ap.add_argument("--force", "-f", action="store_true",
                    help="if set, wipe an existing repo.git/agent-N before "
                         "redeploying. Use this to pick up updated skill files.")
    ap.add_argument("--tm-urls",
                    default="http://localhost:8081,http://localhost:8082,"
                            "http://localhost:8083",
                    help="comma-separated http URLs of tm-server replicas, "
                         "tried in order on transport failure. Default "
                         "matches the docker-compose 3-replica layout. For "
                         "single-replica testing, pass e.g. "
                         "--tm-urls http://localhost:8080.")
    args = ap.parse_args()

    if args.agents < 1:
        sys.exit("error: --agents must be >= 1")

    target = Path(args.target).resolve()
    target.mkdir(parents=True, exist_ok=True)

    repo_dir = target / "repo.git"
    existing = [repo_dir] + [target / f"agent-{i}" for i in range(1, args.agents + 1)]
    existing = [p for p in existing if p.exists()]
    if existing and not args.force:
        sys.exit("error: the following paths already exist; pass --force to wipe + redeploy:\n  "
                 + "\n  ".join(str(p) for p in existing))
    for p in existing:
        print(f"--force: removing existing {p}...")
        rmtree_force(p)

    assets = find_assets_dir()

    # Sanity-check the URL list: strip empties, ensure each starts with http.
    tm_urls = [u.strip().rstrip("/") for u in args.tm_urls.split(",") if u.strip()]
    if not tm_urls:
        sys.exit("error: --tm-urls cannot be empty")
    for u in tm_urls:
        if not (u.startswith("http://") or u.startswith("https://")):
            sys.exit(f"error: --tm-urls entries must start with http:// or "
                     f"https:// (got {u!r})")
    tm_url_list = ",".join(tm_urls)

    print(f"deploying swarm into {target}")
    print(f"  agents: {args.agents}")
    print(f"  repo:   {repo_dir}")
    print(f"  tm urls: {tm_url_list}")

    print("initializing bare repo...")
    init_bare_repo(repo_dir)

    for i in range(1, args.agents + 1):
        agent_id = f"agent-{i}"
        agent_dir = target / agent_id
        print(f"writing {agent_dir}...")
        write_agent(agent_dir, agent_id, args.agents, repo_dir, assets,
                    tm_url_list)

    summary = {
        "target": to_posix(target),
        "agents": args.agents,
        "repo": to_posix(repo_dir),
        "agent_dirs": [to_posix(target / f"agent-{i}") for i in range(1, args.agents + 1)],
    }
    print("\nready. summary:")
    print(json.dumps(summary, indent=2))
    print("\nnext steps:")
    print("  1. start tm-server (e.g. ./build/tm-server -V -p 8080) in another terminal")
    print("  2. POST a seed plan task to tm-server (see pset4/demo/seed-task.json)")
    print(f"  3. for each agent: cd {to_posix(target)}/agent-N && claude")
    print("     then inside Claude: /task-loop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
