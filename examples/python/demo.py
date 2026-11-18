#!/usr/bin/env python3
"""Example Python script using published libyui bindings."""

import os
import sys

# Ensure bindings package is discoverable
sys.path.insert(
    0,
    os.path.abspath(
        os.path.join(os.path.dirname(__file__), "../../bindings/python")
    ),
)

import yui


def main():
  print(f"YuiVieOS Python Demo — v{yui.version()} ({yui.arch()})")

  telex = yui.IME.convert("telex", "Tieengs Vieetj")
  print(f"  Telex: 'Tieengs Vieetj' -> '{telex}'")
  assert telex == "Tiếng Việt", f"Unexpected telex output: {telex}"

  vni = yui.IME.convert("vni", "Vie65t Nam")
  print(f"  VNI  : 'Vie65t Nam' -> '{vni}'")
  assert vni == "Việt Nam", f"Unexpected vni output: {vni}"

  print("OK: Python demo thành công")


if __name__ == "__main__":
  main()
