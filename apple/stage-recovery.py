#!/usr/bin/env python3

"""Stage read-only recovery baselines and writable test copies safely."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tempfile
from typing import Dict, Iterable, List, Tuple
import zipfile


ENTITY_RE = re.compile(rb"^\s*//\s*entity\s+\d+\s*$")
BRUSH_RE = re.compile(rb"^\s*//\s*brush\s+\d+\s*$")
PATCH_RE = re.compile(rb"^\s*patchDef2\s*$")
FACE_RE = re.compile(rb"^\s*\([^()]+\)\s+\([^()]+\)\s+\([^()]+\)\s+\S+")
PATCH_ROW_RE = re.compile(rb"^\s*\(\s*\(")
Q3PACK_MARKER = ("installs", "Q3Pack")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def parse_fixture(value: str) -> Tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("fixture must be NAME=/absolute/path.map")
    name, raw_path = value.split("=", 1)
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", name):
        raise argparse.ArgumentTypeError(
            "fixture name must use lowercase letters, numbers, '_' or '-'"
        )
    path = Path(raw_path).expanduser()
    if not path.is_absolute():
        raise argparse.ArgumentTypeError("fixture path must be absolute")
    return name, path.resolve()


def map_counts(path: Path) -> Dict[str, int]:
    counts = {
        "entities": 0,
        "brushes": 0,
        "patch_def2": 0,
        "brush_faces": 0,
        "patch_control_rows": 0,
    }
    with path.open("rb") as stream:
        for line in stream:
            counts["entities"] += bool(ENTITY_RE.match(line))
            counts["brushes"] += bool(BRUSH_RE.match(line))
            counts["patch_def2"] += bool(PATCH_RE.match(line))
            counts["brush_faces"] += bool(FACE_RE.match(line))
            counts["patch_control_rows"] += bool(PATCH_ROW_RE.match(line))
    return counts


def validate_destination(destination: Path, repo_root: Path) -> None:
    allowed_roots = {
        repo_root.resolve(),
        Path(tempfile.gettempdir()).resolve(),
        Path("/private/tmp").resolve(),
    }
    if not any(is_within(destination, root) for root in allowed_roots):
        allowed = ", ".join(str(path) for path in sorted(allowed_roots))
        raise ValueError(f"destination must be beneath one of: {allowed}")
    if destination.exists():
        raise ValueError(f"destination already exists: {destination}")


def validate_sources(
    fixtures: Iterable[Tuple[str, Path]], archive: Path, backup_root: Path
) -> None:
    if not backup_root.is_dir():
        raise ValueError(f"backup root is not a directory: {backup_root}")
    if not archive.is_file():
        raise ValueError(f"gamepack archive is not a file: {archive}")
    for name, path in fixtures:
        if not path.is_file():
            raise ValueError(f"fixture is not a file: {path}")
        if path.suffix.lower() != ".map":
            raise ValueError(f"fixture is not a .map file: {path}")
        if not is_within(path, backup_root):
            raise ValueError(f"fixture '{name}' is outside backup root: {path}")


def q3pack_members(archive: zipfile.ZipFile) -> List[Tuple[zipfile.ZipInfo, Path]]:
    selected: List[Tuple[zipfile.ZipInfo, Path]] = []
    for info in archive.infolist():
        member = PurePosixPath(info.filename)
        parts = member.parts
        marker_index = next(
            (
                index
                for index in range(len(parts) - 1)
                if tuple(parts[index : index + 2]) == Q3PACK_MARKER
            ),
            None,
        )
        if marker_index is None:
            continue
        relative_parts = parts[marker_index + 2 :]
        if not relative_parts:
            continue
        if member.is_absolute() or any(part in ("", ".", "..") for part in relative_parts):
            raise ValueError(f"unsafe ZIP member: {info.filename}")
        file_type = (info.external_attr >> 16) & 0o170000
        if file_type == stat.S_IFLNK:
            raise ValueError(f"Q3Pack contains an unsupported symlink: {info.filename}")
        selected.append((info, Path(*relative_parts)))
    if not selected:
        raise ValueError("archive does not contain installs/Q3Pack")
    return selected


def extract_q3pack(archive_path: Path, target: Path) -> Dict[str, int]:
    file_count = 0
    byte_count = 0
    with zipfile.ZipFile(archive_path) as archive:
        bad_member = archive.testzip()
        if bad_member is not None:
            raise ValueError(f"ZIP integrity check failed at: {bad_member}")
        members = q3pack_members(archive)
        for info, relative in members:
            output = target / relative
            if info.is_dir():
                output.mkdir(parents=True, exist_ok=True)
                continue
            output.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, output.open("xb") as destination:
                shutil.copyfileobj(source, destination)
            output.chmod(0o444)
            file_count += 1
            byte_count += info.file_size
    return {"files": file_count, "uncompressed_bytes": byte_count}


def stage_fixture(name: str, source: Path, root: Path) -> Dict[str, object]:
    baseline = root / "fixtures" / "baseline" / f"{name}.map"
    working = root / "fixtures" / "work" / f"{name}.map"
    baseline.parent.mkdir(parents=True, exist_ok=True)
    working.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, baseline)
    shutil.copyfile(source, working)
    baseline.chmod(0o444)
    working.chmod(0o644)

    source_hash = sha256(source)
    baseline_hash = sha256(baseline)
    working_hash = sha256(working)
    if len({source_hash, baseline_hash, working_hash}) != 1:
        raise RuntimeError(f"fixture copy verification failed: {source}")

    return {
        "name": name,
        "source": str(source),
        "bytes": source.stat().st_size,
        "sha256": source_hash,
        "structure": map_counts(source),
        "baseline": str(baseline.relative_to(root)),
        "working_copy": str(working.relative_to(root)),
    }


def compare_maps(baseline: Path, candidate: Path) -> int:
    if not baseline.is_file():
        raise ValueError(f"baseline is not a file: {baseline}")
    if not candidate.is_file():
        raise ValueError(f"candidate is not a file: {candidate}")
    baseline_structure = map_counts(baseline)
    candidate_structure = map_counts(candidate)
    baseline_hash = sha256(baseline)
    candidate_hash = sha256(candidate)
    report = {
        "baseline": {
            "path": str(baseline),
            "bytes": baseline.stat().st_size,
            "sha256": baseline_hash,
            "structure": baseline_structure,
        },
        "candidate": {
            "path": str(candidate),
            "bytes": candidate.stat().st_size,
            "sha256": candidate_hash,
            "structure": candidate_structure,
        },
        "byte_identical": baseline_hash == candidate_hash,
        "structure_identical": baseline_structure == candidate_structure,
    }
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0 if report["structure_identical"] else 1


def shader_names_from_file(path: Path) -> List[str]:
    names: List[str] = []
    with path.open("r", encoding="utf-8", errors="strict") as stream:
        for line in stream:
            value = line.split("//", 1)[0].strip()
            if value and re.fullmatch(r"[A-Za-z0-9_.-]+", value):
                names.append(value)
    return names


def prepare_vfs(workspace: Path, quake_base: Path, repo_root: Path) -> int:
    allowed_roots = {
        repo_root.resolve(),
        Path(tempfile.gettempdir()).resolve(),
        Path("/private/tmp").resolve(),
    }
    if not workspace.is_dir():
        raise ValueError(f"workspace is not a directory: {workspace}")
    if not any(is_within(workspace, root) for root in allowed_roots):
        raise ValueError("workspace must be inside the repository or a temporary directory")

    gamepack_base = workspace / "gamepack" / "Q3Pack" / "install"
    default_list = gamepack_base / "baseq3" / "scripts" / "default_shaderlist.txt"
    quake_game = quake_base / "baseq3"
    if not default_list.is_file():
        raise ValueError(f"staged Q3Pack shader list is missing: {default_list}")
    if not quake_game.is_dir():
        raise ValueError(f"Quake III game directory is missing: {quake_game}")

    shader_names = set(shader_names_from_file(default_list))
    pk3_sources: List[str] = []
    for pk3 in sorted(quake_game.glob("*.pk3")):
        if not pk3.is_file():
            continue
        pk3_sources.append(str(pk3))
        try:
            with zipfile.ZipFile(pk3) as archive:
                for member in archive.namelist():
                    match = re.fullmatch(r"scripts/([^/]+)\.shader", member, re.IGNORECASE)
                    if match:
                        shader_names.add(match.group(1))
        except zipfile.BadZipFile as error:
            raise ValueError(f"invalid PK3 archive: {pk3}") from error

    for shader in sorted((quake_game / "scripts").glob("*.shader")):
        shader_names.add(shader.stem)

    overlay = workspace / "test-vfs"
    scripts = overlay / "baseq3" / "scripts"
    scripts.mkdir(parents=True, exist_ok=True)
    shaderlist = scripts / "shaderlist.txt"
    content = "\n".join(sorted(shader_names, key=str.casefold)) + "\n"
    if shaderlist.exists():
        if shaderlist.read_text(encoding="utf-8") != content:
            raise ValueError(f"refusing to overwrite changed shader list: {shaderlist}")
    else:
        with shaderlist.open("x", encoding="utf-8") as stream:
            stream.write(content)

    sample_source = gamepack_base / "baseq3" / "maps" / "q3dm1sample.map"
    if not sample_source.is_file():
        raise ValueError(f"Q3Pack compiler sample is missing: {sample_source}")
    sample_baseline = workspace / "compiler-smoke" / "baseline" / "q3dm1sample.map"
    sample_working = workspace / "compiler-smoke" / "work" / "q3dm1sample.map"
    sample_hash = sha256(sample_source)
    for target, mode in ((sample_baseline, 0o444), (sample_working, 0o644)):
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            if sha256(target) != sample_hash:
                raise ValueError(f"refusing to overwrite changed compiler sample: {target}")
        else:
            shutil.copyfile(sample_source, target)
            target.chmod(mode)

    report = {
        "compiler_sample": {
            "baseline": str(sample_baseline),
            "bytes": sample_source.stat().st_size,
            "sha256": sample_hash,
            "source": str(sample_source),
            "working_copy": str(sample_working),
        },
        "format_version": 1,
        "overlay_basepath": str(overlay),
        "gamepack_basepath": str(gamepack_base),
        "quake_basepath": str(quake_base),
        "pk3_sources": pk3_sources,
        "shader_count": len(shader_names),
        "shaderlist": str(shaderlist),
    }
    manifest = overlay / "manifest.json"
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if manifest.exists():
        if manifest.read_text(encoding="utf-8") != rendered:
            raise ValueError(f"refusing to overwrite changed VFS manifest: {manifest}")
    else:
        with manifest.open("x", encoding="utf-8") as stream:
            stream.write(rendered)

    print(f"prepared test VFS overlay with {len(shader_names)} shader files")
    print(f"overlay base path: {overlay}")
    print(f"gamepack base path: {gamepack_base}")
    print(f"Quake base path: {quake_base}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    stage = commands.add_parser("stage", help="stage fixtures and Q3Pack data")
    stage.add_argument("--backup-root", required=True, type=Path)
    stage.add_argument("--gamepack-archive", required=True, type=Path)
    stage.add_argument("--destination", required=True, type=Path)
    stage.add_argument(
        "--fixture",
        required=True,
        action="append",
        type=parse_fixture,
        metavar="NAME=/absolute/path.map",
    )

    compare = commands.add_parser(
        "compare", help="compare a saved map with its read-only baseline"
    )
    compare.add_argument("--baseline", required=True, type=Path)
    compare.add_argument("--candidate", required=True, type=Path)

    vfs = commands.add_parser(
        "prepare-vfs", help="prepare a test-only shader-list overlay"
    )
    vfs.add_argument("--workspace", required=True, type=Path)
    vfs.add_argument("--quake-base", required=True, type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    repo_root = Path(__file__).resolve().parent.parent

    if args.command == "compare":
        try:
            return compare_maps(
                args.baseline.expanduser().resolve(),
                args.candidate.expanduser().resolve(),
            )
        except (OSError, ValueError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1

    if args.command == "prepare-vfs":
        try:
            return prepare_vfs(
                args.workspace.expanduser().resolve(),
                args.quake_base.expanduser().resolve(),
                repo_root,
            )
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            print(f"error: {error}", file=sys.stderr)
            return 1

    backup_root = args.backup_root.expanduser().resolve()
    archive = args.gamepack_archive.expanduser().resolve()
    destination = args.destination.expanduser().resolve()
    fixtures = args.fixture

    try:
        validate_destination(destination, repo_root)
        validate_sources(fixtures, archive, backup_root)
        destination.mkdir(parents=True)

        fixture_reports = [
            stage_fixture(name, source, destination) for name, source in fixtures
        ]
        gamepack_target = destination / "gamepack" / "Q3Pack"
        gamepack_report = extract_q3pack(archive, gamepack_target)
        archive_report = {
            "source": str(archive),
            "bytes": archive.stat().st_size,
            "sha256": sha256(archive),
            "q3pack": gamepack_report,
            "staged_at": str(gamepack_target.relative_to(destination)),
        }
        report = {
            "format_version": 1,
            "backup_root": str(backup_root),
            "destination": str(destination),
            "fixtures": fixture_reports,
            "gamepack_archive": archive_report,
        }
        manifest = destination / "manifest.json"
        with manifest.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
        print(f"staged {len(fixture_reports)} fixtures at {destination}")
        print(f"manifest: {manifest}")
        return 0
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
