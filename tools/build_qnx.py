"""Build the CarLife module for the pinned upstream CIC HIGH HMI plugin."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

UPSTREAM = "6d8d5754e3e580e83143b94cac9a0b1096ef71a0"
ROOT = Path(__file__).resolve().parents[1]


def prepare_usb(source):
    # Manufacturer, model and version are Android accessory dispatch identifiers.
    # The last field identifies this laboratory accessory itself.
    identity = (
        '{"Baidu", "CarLife", "Baidu CarLife", "1.0.0", '
        '"http://carlife.baidu.com/", "CIC-CarLife-Lab"}'
    )
    patched, count = re.subn(
        r'(static const char \*ids\[6\] = )\{[^{}]+\}', lambda m: m[1] + identity, source
    )
    if count != 1:
        raise SystemExit("Could not locate the pinned USB accessory identity")
    return patched.replace('"AA: ', '"CarLife: ')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", required=True, type=Path)
    parser.add_argument("--out", default=ROOT / "out" / "qnx", type=Path)
    args = parser.parse_args()
    upstream, output = args.upstream.resolve(), args.out.resolve()

    env = os.environ.copy()
    host = Path(env.get("QNX_HOST", "C:/QNX630/host/win32/x86")).resolve()
    target = Path(env.get("QNX_TARGET", "C:/QNX630/target/qnx6")).resolve()
    suffix = ".exe" if os.name == "nt" else ""
    compiler = host / "usr" / "bin" / ("ntosh-gcc-3.3.5" + suffix)
    sdk_markers = [target / "shle/lib/libc.so.2", target / "usr/include/sys/usbdi.h"]
    if not compiler.is_file() or not all(marker.is_file() for marker in sdk_markers):
        raise SystemExit("QNX Momentics 6.3 SH4 compiler / libc / USB headers missing. Set QNX_HOST and QNX_TARGET.")
    env.update(QNX_HOST=str(host), QNX_TARGET=str(target))
    env["PATH"] = str(compiler.parent) + os.pathsep + env["PATH"]

    git = ["git", "-C", str(upstream)]
    actual = subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip()
    if actual != UPSTREAM:
        raise SystemExit(f"Expected CIC upstream commit {UPSTREAM}, found {actual}")
    subprocess.run(
        [*git, "diff", "--quiet", "HEAD", "--", "src/libcicaa", "src/common", "LICENSE.md", "NOTICE.md"],
        check=True,
    )
    lib = upstream / "src/libcicaa"
    common = upstream / "src/common"
    with tempfile.TemporaryDirectory(prefix="cic-carlife-") as temp:
        build = Path(temp)
        usb = build / "aa_usb_carlife.c"
        usb.write_text(prepare_usb((lib / "aa_usb_qnx.c").read_text(encoding="utf-8")), encoding="utf-8")
        sources = [
            ROOT / "src/carlife.c", ROOT / "src/avc.c", ROOT / "src/qnx_bridge.c",
            usb, lib / "aa_video.c", lib / "aa_video_native.c",
        ]
        flags = ["-m4", "-ml", "-std=gnu99", "-D_QNX_SOURCE", "-fPIC", "-Os", "-Wall"]
        flags.extend("-I" + str(p) for p in (lib, common))
        objects = []
        for source in sources:
            obj = build / (source.stem + ".o")
            subprocess.run([str(compiler), *flags, "-c", str(source), "-o", str(obj)], env=env, check=True)
            objects.append(str(obj))
        name = "libcicaa.so"
        binary = build / name
        subprocess.run(
            [str(compiler), "-m4", "-ml", "-shared", "-Wl,-z,defs", "-Wl,-soname," + name,
             "-o", str(binary), *objects, "-lusbdi", "-lc"],
            env=env, check=True,
        )
        header = binary.read_bytes()[:20]
        if header[:7] != b"\x7fELF\x01\x01\x01" or header[16:20] != b"\x03\x00\x2a\x00":
            raise SystemExit("Output is not an ELF32 little-endian SuperH shared object")
        output.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "LICENSE", output / "LICENSE")
        shutil.copyfile(ROOT / "NOTICE.md", output / "NOTICE.md")
        shutil.copyfile(upstream / "LICENSE.md", output / "UPSTREAM-LICENSE.md")
        shutil.copyfile(upstream / "NOTICE.md", output / "UPSTREAM-NOTICE.md")
        shutil.copyfile(binary, output / name)
    print(output / name)


if __name__ == "__main__":
    main()
