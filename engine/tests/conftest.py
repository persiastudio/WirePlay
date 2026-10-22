"""Put the engine package on sys.path once, for every test module.

Several test modules imported `wireplay_engine` after doing their own
`sys.path.insert`, and `test_wire_protocol.py` did not do it at all -- it worked
only because pytest happened to import another module first. Running that file
on its own raised ModuleNotFoundError. Collection order is not a dependency a
test suite should have.
"""

import sys
from pathlib import Path

ENGINE_DIR = str(Path(__file__).resolve().parents[1])
if ENGINE_DIR not in sys.path:
    sys.path.insert(0, ENGINE_DIR)
