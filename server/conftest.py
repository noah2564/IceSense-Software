import os
import sys

# Make the server modules importable from tests without installing a package.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("ICE_NO_AUTOCREATE", "1")
