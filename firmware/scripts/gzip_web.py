"""PlatformIO pre-script: gzip every web/*.html into data/<name>.html.gz (uploaded with `pio run -t uploadfs`).

Also runnable on its own:  python scripts/gzip_web.py
"""
import gzip
import os

HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
ROOT = os.path.dirname(HERE)


def build() -> list:
    src_dir = os.path.join(ROOT, "web")
    dst_dir = os.path.join(ROOT, "data")
    os.makedirs(dst_dir, exist_ok=True)
    written = []
    for name in sorted(os.listdir(src_dir)):
        if not name.endswith(".html"):
            continue
        with open(os.path.join(src_dir, name), "rb") as fh:
            raw = fh.read()
        dst = os.path.join(dst_dir, name + ".gz")
        with open(dst, "wb") as fh:
            fh.write(gzip.compress(raw, 9, mtime=0))   # mtime=0: reproducible output
        written.append(dst)
    return written


if __name__ == "__main__":
    print("wrote", ", ".join(build()))
else:  # executed by PlatformIO (SCons) where __file__ is not defined
    try:
        Import("env")  # noqa: F821
        ROOT = env.subst("$PROJECT_DIR")  # noqa: F821
    except NameError:
        pass
    build()
