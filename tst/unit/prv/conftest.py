"""
conftest.py — adds prv/ to sys.path so remsvc_provider and remsvc_proto
are importable when pytest is invoked from the repo root or tst/.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[3] / "prv"))
