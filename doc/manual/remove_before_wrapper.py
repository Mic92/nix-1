#!/usr/bin/env python3

import argparse
import shutil
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory


def main() -> None:
    arg_parser = argparse.ArgumentParser(description="Remove before wrapper")
    arg_parser.add_argument("output", type=Path, help="Output file")
    arg_parser.add_argument("nix_command", nargs=argparse.REMAINDER, help="Nix command")
    args = arg_parser.parse_args()

    output = Path(args.output)
    with TemporaryDirectory(prefix=str(output.parent.resolve() / "tmp")) as temp:
        output_temp = Path(temp) / "output"

        # Remove the output output in case it exist
        shutil.rmtree(output, ignore_errors=True)

        # Execute nix command with `--write-to` tempary output
        subprocess.run([*args.nix_command, "--write-to", output_temp], check=True)

        # Move the temporary output to the intended location
        Path(output_temp).rename(output)


if __name__ == "__main__":
    main()
