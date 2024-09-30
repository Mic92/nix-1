#!/usr/bin/env nix
#! nix shell --inputs-from .# nixpkgs#bashInteractive nixpkgs#python3 nixpkgs#awscli2 nixpkgs#nix nixpkgs#podman --command python3
import argparse
import json
import logging
import os
import shlex
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent

logger = logging.getLogger("upload-release")


@dataclass
class Options:
    eval_id: int
    aws_region: str
    aws_endpoint: str
    release_bucket: str
    channels_bucket: str
    is_latest: bool
    docker_owner: str
    docker_authfile: Path
    dry_run: bool
    self_test: bool
    project_root: Path
    self_test_registry_port: int = 5000
    self_test_minio_port: int = 9000
    eval_url: str = field(init=False)

    def __post_init__(self) -> None:
        self.eval_url = f"https://hydra.nixos.org/eval/{self.eval_id}"


@dataclass
class Platform:
    job_name: str
    can_fail: bool = False


class Error(Exception):
    pass


def fetch_json(url: str) -> Any:
    request = urllib.request.Request(url)
    request.add_header("Accept", "application/json")
    logger.info(f"fetching {url}")
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def get_store_path(eval_url: str, job_name: str, output: str = "out") -> str:
    build_info = fetch_json(f"{eval_url}/job/{job_name}")
    path = build_info["buildoutputs"].get(output, {}).get("path", None)
    if not path:
        msg = f"job '{job_name}' lacks output '{output}'"
        raise Error(msg)
    return path


def run(
    command: list[str],
    check: bool = True,
    dry_run: bool = False,
    **kwargs: Any,
) -> subprocess.CompletedProcess:
    logger.info(f"run {shlex.join(command)}")
    if dry_run:
        return subprocess.CompletedProcess(args=command, returncode=0, stdout=b"", stderr=b"")
    return subprocess.run(command, check=check, text=True, **kwargs)


def copy_manual(
    options: Options,
    tmp_dir: Path,
    release_name: str,
    binary_cache: str,
    release_dir: str,
) -> None:
    try:
        manual = get_store_path(options.eval_url, "build.nix.x86_64-linux", "doc")
    except Exception:
        logger.exception("Failed to get manual path")
        return

    logger.info(f"Manual: {manual}")

    manual_nar = tmp_dir / f"{release_name}-manual.nar.xz"
    logger.info(manual_nar)

    if not manual_nar.exists():
        tmp = manual_nar.with_suffix(".xz.tmp")
        env = os.environ.copy()
        env["NIX_REMOTE"] = binary_cache
        run(["bash", "-c", "nix store dump-path $1 | xz > $2", "", manual, str(tmp)], env=env)
        tmp.rename(manual_nar)

    manual_dir = tmp_dir / "manual"
    if not manual_dir.exists():
        tmp = manual_dir.with_suffix(".tmp")
        run(["bash", "-c", "xz -d < $1 | nix-store --restore $2", "", str(manual_nar), str(tmp)])
        (tmp / "share" / "doc" / "nix" / "manual").rename(manual_dir)
        shutil.rmtree(manual_dir.parent / "manual.tmp")

    run(
        [
            "aws",
            "--endpoint",
            options.aws_endpoint,
            "s3",
            "sync",
            str(manual_dir),
            f"s3://{options.release_bucket}/{release_dir}/manual",
        ],
    )


def download_file(
    eval_url: str,
    binary_cache: str,
    tmp_dir: Path,
    job_name: str,
    dst_name: str | None = None,
) -> str | None:
    build_info = fetch_json(f"{eval_url}/job/{job_name}")
    src_file = build_info["buildproducts"]["1"].get("path", None)
    if not src_file:
        msg = f"job '{job_name}' lacks product '1'"
        raise Error(msg)
    dst_name = dst_name or Path(src_file).name
    tmp_file = tmp_dir / dst_name

    if not tmp_file.exists():
        logger.info(f"downloading {src_file} to {tmp_file}...")
        file_info = json.loads(
            run(
                ["nix", "store", "ls", "--json", src_file],
                env={"NIX_REMOTE": binary_cache},
                stdout=subprocess.PIPE,
            ).stdout,
        )
        if file_info["type"] == "symlink":
            src_file = file_info["target"]
        with tmp_file.with_suffix(".tmp").open("wb") as f:
            run(
                ["nix", "store", "cat", src_file],
                env={"NIX_REMOTE": binary_cache},
                stdout=f,
            )
        tmp_file.with_suffix(".tmp").rename(tmp_file)

    sha256_expected = build_info["buildproducts"]["1"]["sha256hash"]
    sha256_actual = run(
        ["nix", "hash", "file", "--base16", "--type", "sha256", str(tmp_file)],
        stdout=subprocess.PIPE,
    ).stdout.strip()
    if sha256_expected and sha256_expected != sha256_actual:
        msg = f"file {tmp_file} is corrupt, got {sha256_actual}, expected {sha256_expected}"
        raise Error(msg)

    tmp_file.with_suffix(".sha256").write_text(sha256_actual)
    return sha256_expected


def update_container_images(
    options: Options,
    binary_cache: str,
    tmp_dir: Path,
    release_dir: Path,
    version: str,
) -> None:
    docker_manifest = []
    docker_manifest_latest = []
    have_docker = False
    podman_home = tmp_dir / "podman"
    auth_file = podman_home / ".config" / "containers" / "auth.json"
    auth_file.parent.mkdir(parents=True, exist_ok=True)
    if options.docker_authfile.exists():
        shutil.copy(options.docker_authfile, auth_file)
    policy = podman_home / ".config" / "containers" / "policy.json"
    policy.parent.mkdir(parents=True, exist_ok=True)
    policy.write_text(
        json.dumps(
            {
                "default": [
                    {"type": "insecureAcceptAnything"},
                ],
            },
        ),
    )
    if options.self_test:
        registry = podman_home / ".config" / "containers" / "registries.conf"
        registry.parent.mkdir(parents=True, exist_ok=True)
        registry.write_text(
            f"""
            [[registry]]
            location = "localhost:{options.self_test_registry_port}"
            insecure = true
            """,
        )

    docker_platforms = [
        ("x86_64-linux", "amd64"),
        ("aarch64-linux", "arm64"),
    ]

    def podman(command: list[str], dry_run: bool = False) -> subprocess.CompletedProcess:
        env = os.environ.copy()
        env["HOME"] = str(podman_home)
        return run(["podman", *command], env=env, dry_run=dry_run)

    for system, docker_platform in docker_platforms:
        file_name = f"nix-{version}-docker-image-{docker_platform}.tar.gz"
        try:
            download_file(options.eval_url, binary_cache, release_dir, f"dockerImage.{system}", file_name)
        except subprocess.CalledProcessError:
            logger.exception(f"Failed to build for {docker_platform}")
            continue

        have_docker = True
        logger.info(f"loading docker image for {docker_platform}...")
        podman(["load", "-i", str(release_dir / file_name)])

        tag = f"{options.docker_owner}/nix:{version}-{docker_platform}"
        latest_tag = f"{options.docker_owner}/nix:latest-{docker_platform}"

        logger.info(f"tagging {version} docker image for {docker_platform}...")
        podman(["tag", f"nix:{version}", tag])
        logger.info(f"pushing {version} docker image for {docker_platform}...")
        podman(["push", "-q", tag], dry_run=options.dry_run)

        if options.is_latest:
            logger.info(f"tagging latest docker image for {docker_platform}...")
            podman(["tag", f"nix:{version}", latest_tag])
            logger.info(f"pushing latest docker image for {docker_platform}...")
            podman(["push", "-q", latest_tag], dry_run=options.dry_run)

        docker_manifest += ["--amend", tag]
        docker_manifest_latest += ["--amend", latest_tag]

    if not have_docker:
        return

    logger.info("creating multi-platform docker manifest...")
    podman(["manifest", "create", f"{options.docker_owner}/nix:{version}", *docker_manifest])
    if options.is_latest:
        logger.info("creating latest multi-platform docker manifest...")
        podman(["manifest", "create", f"{options.docker_owner}/nix:latest", *docker_manifest_latest])

    logger.info("pushing multi-platform docker manifest...")
    podman(["manifest", "push", f"{options.docker_owner}/nix:{version}"], dry_run=options.dry_run)

    if options.is_latest:
        logger.info("pushing latest multi-platform docker manifest...")
        podman(["manifest", "push", f"{options.docker_owner}/nix:latest"], dry_run=options.dry_run)


def upload_release(options: Options, tmp_dir: Path) -> None:
    # check if git diff is clean
    if run(["git", "diff", "--quiet"]).returncode != 0:
        msg = "Git diff is not clean. Please commit or stash your changes or set --project-root to a clean directory."
        raise Error(msg)

    nar_cache = tmp_dir / "nar-cache"
    nar_cache.mkdir(parents=True, exist_ok=True)
    binary_cache = f"https://cache.nixos.org/?local-nar-cache={nar_cache}"

    try:
        run(["aws", "--endpoint", options.aws_endpoint, "s3", "ls", f"s3://{options.release_bucket}"])
    except subprocess.CalledProcessError as e:
        msg = "Cannot access release buckets. Check your AWS credentials."
        raise Error(msg) from e

    eval_info = fetch_json(options.eval_url)
    flake_url = eval_info.get("flake")
    flake_info = json.loads(run(["nix", "flake", "metadata", "--json", flake_url], stdout=subprocess.PIPE).stdout)
    nix_rev = flake_info["locked"]["rev"]

    build_info = fetch_json(f"{options.eval_url}/job/build.nix.x86_64-linux")
    release_name = build_info["nixname"]
    release_dir = tmp_dir / "nix" / release_name
    release_dir.mkdir(parents=True, exist_ok=True)
    version = release_name.split("-")[-1]

    logger.info(
        f"Flake URL is {flake_url}, Nix revision is {nix_rev}, version is {version}",
    )
    update_container_images(options, binary_cache, tmp_dir, release_dir, version)

    release_location = f"nix/{release_name}"
    copy_manual(options, tmp_dir, release_name, binary_cache, release_location)

    platforms = [
        Platform("binaryTarball.i686-linux"),
        Platform("binaryTarball.x86_64-linux"),
        Platform("binaryTarball.aarch64-linux"),
        Platform("binaryTarball.x86_64-darwin"),
        Platform("binaryTarball.aarch64-darwin"),
        Platform("binaryTarballCross.x86_64-linux.armv6l-unknown-linux-gnueabihf", can_fail=True),
        Platform("binaryTarballCross.x86_64-linux.armv7l-unknown-linux-gnueabihf", can_fail=True),
        Platform("binaryTarballCross.x86_64-linux.riscv64-unknown-linux-gnu"),
        Platform("installerScript"),
    ]

    for platform in platforms:
        try:
            download_file(options.eval_url, binary_cache, release_dir, platform.job_name)
        except subprocess.CalledProcessError:
            if platform.can_fail:
                logger.exception(f"Failed to build {platform.job_name}")
                continue
            raise

    fallback_paths = {
        "x86_64-linux": get_store_path(options.eval_url, "build.nix.x86_64-linux"),
        "i686-linux": get_store_path(options.eval_url, "build.nix.i686-linux"),
        "aarch64-linux": get_store_path(options.eval_url, "build.nix.aarch64-linux"),
        "riscv64-linux": get_store_path(options.eval_url, "buildCross.nix.riscv64-unknown-linux-gnu.x86_64-linux"),
        "x86_64-darwin": get_store_path(options.eval_url, "build.nix.x86_64-darwin"),
        "aarch64-darwin": get_store_path(options.eval_url, "build.nix.aarch64-darwin"),
    }

    (release_dir / "fallback-paths.nix").write_text(json.dumps(fallback_paths, indent=2))

    for file_name in release_dir.glob("*"):
        name = file_name.name
        dst_key = f"{release_location}/{name}"
        has_object = (
            subprocess.run(
                ["aws", "--endpoint", options.aws_endpoint, "s3", "ls", f"s3://{options.release_bucket}/{dst_key}"],
                stdout=subprocess.PIPE,
                check=False,
            ).returncode
            == 0
        )
        if not has_object:
            logger.info(f"uploading {file_name} to s3://{options.release_bucket}/{dst_key}...")
            content_type = "application/octet-stream"
            if file_name.suffix in [".sha256", ".install", ".nix"]:
                content_type = "text/plain"
            run(
                [
                    "aws",
                    "--endpoint",
                    options.aws_endpoint,
                    "s3",
                    "cp",
                    str(file_name),
                    f"s3://{options.release_bucket}/{dst_key}",
                    "--content-type",
                    content_type,
                ],
                dry_run=options.dry_run,
            )

    if options.is_latest:
        run(
            [
                "aws",
                "--endpoint",
                options.aws_endpoint,
                "s3api",
                "put-object",
                "--bucket",
                options.channels_bucket,
                "--key",
                "nix-latest/install",
                "--website-redirect-location",
                f"https://releases.nixos.org/{release_location}/install",
            ],
            dry_run=options.dry_run,
        )

    run(["git", "remote", "update", "origin"])
    run(
        [
            "git",
            "tag",
            "--force",
            "--sign",
            version,
            nix_rev,
            "-m",
            f"Tagging release {version}",
        ],
    )
    run(["git", "push", "--tags"], dry_run=options.dry_run or options.self_test)
    if options.is_latest:
        run(
            [
                "git",
                "push",
                "--force-with-lease",
                "origin",
                f"{nix_rev}:refs/heads/latest-release",
            ],
            dry_run=options.dry_run or options.self_test,
        )


def parse_args() -> Options:
    parser = argparse.ArgumentParser(description="Upload a release to S3")
    parser.add_argument("evalid", type=int, help="The evaluation ID to upload")
    parser.add_argument(
        "--aws-endpoint",
        type=str,
        default="https://s3-eu-west-1.amazonaws.com",
        help="The AWS endpoint to use",
    )
    parser.add_argument(
        "--aws-region",
        type=str,
        default="s3-eu-west-1.amazonaws.com",
        help="The AWS region to use",
    )
    parser.add_argument(
        "--release-bucket",
        type=str,
        default="nix-releases",
        help="The S3 bucket to upload releases to",
    )
    parser.add_argument(
        "--channels-bucket",
        type=str,
        default="nix-channels",
        help="The S3 bucket to upload channels to",
    )
    parser.add_argument("--is-latest", action="store_true", help="Whether this is the latest release")
    parser.add_argument("--dry-run", action="store_true", help="Don't actually upload anything")
    parser.add_argument("--self-test", action="store_true", help="Don't actually upload anything")
    parser.add_argument(
        "--self-test-registry-port",
        type=int,
        default=5000,
        help="The port to run the Docker registry on",
    )
    parser.add_argument("--self-test-minio-port", type=int, default=9000, help="The port to run Minio on")
    parser.add_argument("--docker-owner", type=str, default="docker.io/nixos", help="The owner of the Docker images")
    parser.add_argument(
        "--docker-authfile",
        type=Path,
        help="The path to the Docker authfile",
        default=Path.home() / ".docker" / "config.json",
    )
    parser.add_argument(
        "--project-root",
        type=Path,
        default=SCRIPT_DIR.parent,
        help="The root of the project (default: the directory of this script)",
    )
    args = parser.parse_args()
    return Options(
        eval_id=args.evalid,
        aws_region=args.aws_region,
        aws_endpoint=args.aws_endpoint,
        release_bucket=args.release_bucket,
        channels_bucket=args.channels_bucket,
        is_latest=args.is_latest,
        docker_owner=args.docker_owner,
        docker_authfile=args.docker_authfile,
        dry_run=args.dry_run,
        project_root=args.project_root,
        self_test=args.self_test,
        self_test_registry_port=args.self_test_registry_port,
        self_test_minio_port=args.self_test_minio_port,
    )


def wait_tcp_server(port: int, process: subprocess.Popen) -> None:
    while True:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(("localhost", port))
        except OSError as e:
            logger.info(f"Wait for port: {port}")
            if res := process.poll() is not None:
                msg = f"Process {process} exited with code {res}"
                raise Error(msg) from e
        else:
            return
        time.sleep(0.1)


@contextmanager
def setup_self_test(options: Options, tmp_dir: Path) -> Iterator[None]:
    if not options.self_test:
        yield
        return
    registry_dir = tmp_dir / "docker-registry"
    registry_dir.mkdir()
    registry_config = registry_dir / "config.yml"
    registry_config.write_text(
        json.dumps(
            {
                "version": "0.1",
                "http": {
                    "addr": f"localhost:{options.self_test_registry_port}",
                },
                "storage": {
                    "filesystem": {"rootdirectory": str(registry_dir)},
                },
            },
        ),
    )

    registry_command = [
        "nix",
        "shell",
        "--inputs-from",
        options.project_root,
        "nixpkgs#docker-distribution",
        "-c",
        "registry",
        "serve",
        registry_config,
    ]
    minio_command = ["nix", "run", "--inputs-from", options.project_root, "nixpkgs#minio", "--", "server", tmp_dir / "minio"]

    os.environ["MINIO_ROOT_USER"] = "minioadmin"
    os.environ["MINIO_ROOT_PASSWORD"] = "minioadmin"
    os.environ["AWS_ACCESS_KEY_ID"] = "minioadmin"
    os.environ["AWS_SECRET_ACCESS_KEY"] = "minioadmin"
    options.aws_endpoint = f"http://localhost:{options.self_test_minio_port}"
    options.docker_owner = f"localhost:{options.self_test_registry_port}"

    with (
        subprocess.Popen(
            registry_command,
            cwd=registry_dir,
        ) as registry,
        subprocess.Popen(minio_command) as minio,
    ):
        try:
            wait_tcp_server(options.self_test_registry_port, registry)
            wait_tcp_server(options.self_test_minio_port, minio)
            run(["aws", "--endpoint", options.aws_endpoint, "s3", "mb", f"s3://{options.release_bucket}"])
            run(["aws", "--endpoint", options.aws_endpoint, "s3", "mb", f"s3://{options.channels_bucket}"])
            yield
            logger.info("############################### Finished self-test ###############################")
            logger.info(
                f"You can inspect the release at http://localhost:{options.self_test_minio_port}/{options.release_bucket}",
            )
            logger.info("Username/password: minioadmin/minioadmin")
            logger.info(
                f"You can inspect the registry at http://localhost:{options.self_test_registry_port}/v2/_catalog",
            )
            logger.info("Type 'exit' to stop the self-test")
            subprocess.run(["bash", "--login"], check=False)
        finally:
            try:
                registry.kill()
            finally:
                minio.kill()


def main() -> None:
    options = parse_args()
    logging.basicConfig(level=logging.INFO)
    try:
        with TemporaryDirectory() as _tmp_dir:
            tmp_dir = Path(_tmp_dir)
            os.chdir(options.project_root)

            with setup_self_test(options, tmp_dir):
                upload_release(options, tmp_dir)
    except Error as e:
        print(e, file=sys.stderr)  # noqa: T201
        sys.exit(1)


if __name__ == "__main__":
    main()
