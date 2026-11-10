"""PlatformIO pre-script: gzip web/index.html into data/index.html.gz (uploaded with `pio run -t uploadfs`).

Also runnable on its own:  python scripts/gzip_web.py
"""
import gzip
import os

HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
ROOT = os.path.dirname(HERE)


def build() -> str:
    src = os.path.join(ROOT, "web", "index.html")
    dst_dir = os.path.join(ROOT, "data")
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, "index.html.gz")
    with open(src, "rb") as fh:
        raw = fh.read()
    with open(dst, "wb") as fh:
        fh.write(gzip.compress(raw, 9, mtime=0))   # mtime=0: reproducible output
    return dst


if __name__ == "__main__":
    print("wrote", build())
else:  # executed by PlatformIO (SCons) where __file__ is not defined
    try:
        Import("env")  # noqa: F821
        ROOT = env.subst("$PROJECT_DIR")  # noqa: F821
    except NameError:
        pass
    build()
