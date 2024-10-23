#!/usr/bin/env python3

import subprocess
import sys
import shutil
from pathlib import Path
from tempfile import TemporaryDirectory


def main() -> None:
    if len(sys.argv) < 4 or "--" not in sys.argv:
        print("Usage: remove-before-wrapper <output> -- <nix command...>", file=sys.stderr)
        sys.exit(1)

    # Extract the parts
    output = Path(sys.argv[1])
    nix_command_idx = sys.argv.index("--") + 1
    nix_command = sys.argv[nix_command_idx:]

    with TemporaryDirectory(prefix=str(output.parent.resolve() / "tmp")) as temp:
        output_temp = Path(temp) / "output"

        # Remove the output and temp output in case they exist
        shutil.rmtree(output, ignore_errors=True)

        # Execute nix command with `--write-to` tempary output
        subprocess.run([*nix_command, "--write-to", str(output_temp)], check=True)

        # Move the temporary output to the intended location
        Path(output_temp).rename(output)


if __name__ == "__main__":
    main()
