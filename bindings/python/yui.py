"""YuiVieOS C-ABI Python bindings (libyui)."""

import ctypes
import os

_SEARCH_PATHS = [
    os.path.join(os.path.dirname(__file__), "../../build/bin/libyui.so.1"),
    os.path.join(os.path.dirname(__file__), "../../build/bin/libyui.so"),
    "/usr/local/lib/libyui.so.1",
    "/usr/local/lib/libyui.so",
    "/usr/lib/libyui.so.1",
    "/usr/lib/libyui.so",
]


def _find_lib():
  for p in _SEARCH_PATHS:
    abs_p = os.path.abspath(p)
    if os.path.exists(abs_p):
      return ctypes.CDLL(abs_p)
  try:
    return ctypes.CDLL("libyui.so.1")
  except OSError:
    try:
      return ctypes.CDLL("libyui.so")
    except OSError as e:
      raise RuntimeError(f"Cannot find libyui: {e}") from e


_lib = None


def get_lib():
  global _lib
  if _lib is None:
    _lib = _find_lib()
    _lib.yui_version.restype = ctypes.c_char_p
    _lib.yui_version.argtypes = []
    _lib.yui_arch.restype = ctypes.c_char_p
    _lib.yui_arch.argtypes = []
    _lib.yui_ime_convert.restype = ctypes.c_int
    _lib.yui_ime_convert.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
  return _lib


class IME:

  @staticmethod
  def convert(method: str, keys: str) -> str:
    lib = get_lib()
    buf = ctypes.create_string_buffer(512)
    rc = lib.yui_ime_convert(
        method.encode("utf-8"), keys.encode("utf-8"), buf, len(buf)
    )
    if rc <= 0:
      raise ValueError(f"yui_ime_convert failed with code {rc}")
    return buf.value.decode("utf-8")


def version() -> str:
  return get_lib().yui_version().decode("utf-8")


def arch() -> str:
  return get_lib().yui_arch().decode("utf-8")
