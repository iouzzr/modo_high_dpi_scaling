# python
# Copyright 2026 iouzzr
# SPDX-License-Identifier: Apache-2.0
#
# HighDPIScaling lxserv plugin for Foundry Modo (16.x & 17.x)
# 1. Pre-main-window bootstrapper with environment snapshot/restoration, zero-relaunch
#    short-circuit, transparent guardian bridge parent, Per-Monitor V2 dynamic cross-screen
#    scaling ("system"), and early OpenGL FBO/Viewport + 2D UI + IPR hook installer.
# 2. Native Modo Commands (high_dpi_scaling.mode, high_dpi_scaling.custom_scale),
#    Notifier (high_dpi_scaling.notifier), and official lx.service.Persistence()
#    storage (HighDPIScaling.cfg) honoring -noconfig / -dbon:noconfig / -config:<path>.
import os
import json
import ctypes
import subprocess
import xml.etree.ElementTree as ET
import lx
import lxifc
import lxu.command

KIT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL_PATH = os.path.join(KIT_DIR, "high_dpi_scaling.dll")
PERSIST_ATOM_NAME = "HighDPIScaling"

SCALE_OPTIONS = (
    ("100%", 1.0),
    ("125%", 1.25),
    ("150%", 1.5),
    ("200%", 2.0),
)
SCALE_BY_LABEL = {label: val for label, val in SCALE_OPTIONS}

NOTIFIER_NAME = "high_dpi_scaling.notifier"
CMD_MODE_NAME = "high_dpi_scaling.mode"
CMD_CUSTOM_SCALE_NAME = "high_dpi_scaling.custom_scale"
MODE_ARG_TYPE = "high_dpi_scaling-mode"
MODE_KEYS = ("system", "custom", "none")
NON_GUI_CLI_FLAGS = frozenset(("headless", "nowin", "slave", "worker", "telnet", "console"))

SENSITIVE_ENV_VARS = (
    "__COMPAT_LAYER",
    "QT_ENABLE_HIGHDPI_SCALING",
    "QT_AUTO_SCREEN_SCALE_FACTOR",
    "QT_FONT_DPI",
    "QT_SCALE_FACTOR",
    "QT_SCALE_FACTOR_ROUNDING_POLICY",
)
ENV_SNAPSHOT_VAR = "_MODO_HIDPI_ENV_SNAPSHOT"

_STD_INPUT_HANDLE = ctypes.c_uint32(-10 & 0xFFFFFFFF).value
_STD_OUTPUT_HANDLE = ctypes.c_uint32(-11 & 0xFFFFFFFF).value
_STD_ERROR_HANDLE = ctypes.c_uint32(-12 & 0xFFFFFFFF).value
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
_HANDLE_FLAG_INHERIT = 0x00000001
_FILE_TYPE_UNKNOWN = 0x0000
_SW_HIDE = 0
_INFINITE = 0xFFFFFFFF
_JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS = 9
_JOB_OBJECT_LIMIT_BREAKAWAY_OK = 0x00000800
_JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK = 0x00001000
_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
_MONITOR_DEFAULTTOPRIMARY = 1
_MONITOR_DEFAULTTONEAREST = 2
_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ctypes.c_void_p(-4)

_KEEPALIVE = []
_persist_data = None


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class _DpiPair(ctypes.Structure):
    _fields_ = [("first", ctypes.c_double), ("second", ctypes.c_double)]


class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("ReadOperationCount", ctypes.c_uint64),
        ("WriteOperationCount", ctypes.c_uint64),
        ("OtherOperationCount", ctypes.c_uint64),
        ("ReadTransferCount", ctypes.c_uint64),
        ("WriteTransferCount", ctypes.c_uint64),
        ("OtherTransferCount", ctypes.c_uint64),
    ]


class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64),
        ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", ctypes.c_uint32),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", ctypes.c_uint32),
        ("Affinity", ctypes.c_size_t),
        ("PriorityClass", ctypes.c_uint32),
        ("SchedulingClass", ctypes.c_uint32),
    ]


class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", _JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", _IO_COUNTERS),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


def _bind(dll, name, argtypes, restype):
    fn = getattr(dll, name)
    fn.argtypes = argtypes
    fn.restype = restype
    return fn


_KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
_SHELL32 = ctypes.WinDLL("shell32")
_USER32 = ctypes.WinDLL("user32")
_GDI32 = ctypes.WinDLL("gdi32")

_GetCommandLineW = _bind(_KERNEL32, "GetCommandLineW", [], ctypes.c_wchar_p)
_LocalFree = _bind(_KERNEL32, "LocalFree", [ctypes.c_void_p], ctypes.c_void_p)
_SetEnvironmentVariableW = _bind(
    _KERNEL32, "SetEnvironmentVariableW", [ctypes.c_wchar_p, ctypes.c_wchar_p], ctypes.c_int
)
_GetEnvironmentVariableW = _bind(
    _KERNEL32,
    "GetEnvironmentVariableW",
    [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32],
    ctypes.c_uint32,
)
_GetModuleFileNameW = _bind(
    _KERNEL32, "GetModuleFileNameW", [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32], ctypes.c_uint32
)
_GetModuleHandleW = _bind(_KERNEL32, "GetModuleHandleW", [ctypes.c_wchar_p], ctypes.c_void_p)
_GetProcAddress = _bind(_KERNEL32, "GetProcAddress", [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_void_p)
_GetCurrentThreadId = _bind(_KERNEL32, "GetCurrentThreadId", [], ctypes.c_uint32)
_GetStdHandle = _bind(_KERNEL32, "GetStdHandle", [ctypes.c_uint32], ctypes.c_void_p)
_GetFileType = _bind(_KERNEL32, "GetFileType", [ctypes.c_void_p], ctypes.c_uint32)
_SetHandleInformation = _bind(
    _KERNEL32, "SetHandleInformation", [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32], ctypes.c_int
)
_CreateJobObjectW = _bind(
    _KERNEL32, "CreateJobObjectW", [ctypes.c_void_p, ctypes.c_wchar_p], ctypes.c_void_p
)
_SetInformationJobObject = _bind(
    _KERNEL32,
    "SetInformationJobObject",
    [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32],
    ctypes.c_int,
)
_AssignProcessToJobObject = _bind(
    _KERNEL32, "AssignProcessToJobObject", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int
)
_WaitForSingleObject = _bind(
    _KERNEL32, "WaitForSingleObject", [ctypes.c_void_p, ctypes.c_uint32], ctypes.c_uint32
)
_GetExitCodeProcess = _bind(
    _KERNEL32, "GetExitCodeProcess", [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)], ctypes.c_int
)
_CloseHandle = _bind(_KERNEL32, "CloseHandle", [ctypes.c_void_p], ctypes.c_int)

_CommandLineToArgvW = _bind(
    _SHELL32, "CommandLineToArgvW", [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)], ctypes.POINTER(ctypes.c_wchar_p)
)
_GetDC = _bind(_USER32, "GetDC", [ctypes.c_void_p], ctypes.c_void_p)
_ReleaseDC = _bind(_USER32, "ReleaseDC", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int)
_GetCursorPos = _bind(_USER32, "GetCursorPos", [ctypes.POINTER(_POINT)], ctypes.c_int)
_GetForegroundWindow = _bind(_USER32, "GetForegroundWindow", [], ctypes.c_void_p)
_MonitorFromPoint = _bind(_USER32, "MonitorFromPoint", [_POINT, ctypes.c_uint32], ctypes.c_void_p)
_MonitorFromWindow = _bind(_USER32, "MonitorFromWindow", [ctypes.c_void_p, ctypes.c_uint32], ctypes.c_void_p)
_IsWindowVisible = _bind(_USER32, "IsWindowVisible", [ctypes.c_void_p], ctypes.c_int)
_ShowWindow = _bind(_USER32, "ShowWindow", [ctypes.c_void_p, ctypes.c_int], ctypes.c_int)
_WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t)
_EnumThreadWindows = _bind(_USER32, "EnumThreadWindows", [ctypes.c_uint32, _WNDENUMPROC, ctypes.c_ssize_t], ctypes.c_int)
_MONITORENUMPROC = ctypes.WINFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t
)
_EnumDisplayMonitors = _bind(
    _USER32,
    "EnumDisplayMonitors",
    [ctypes.c_void_p, ctypes.c_void_p, _MONITORENUMPROC, ctypes.c_ssize_t],
    ctypes.c_int,
)
_GetDeviceCaps = _bind(_GDI32, "GetDeviceCaps", [ctypes.c_void_p, ctypes.c_int], ctypes.c_int)

try:
    _UCRTBASE = ctypes.CDLL("ucrtbase")
    _wputenv_s = _bind(_UCRTBASE, "_wputenv_s", [ctypes.c_wchar_p, ctypes.c_wchar_p], ctypes.c_int)
except Exception:
    _wputenv_s = None

try:
    _GetThreadDpiAwarenessContext = _bind(_USER32, "GetThreadDpiAwarenessContext", [], ctypes.c_void_p)
    _SetThreadDpiAwarenessContext = _bind(
        _USER32, "SetThreadDpiAwarenessContext", [ctypes.c_void_p], ctypes.c_void_p
    )
    _GetAwarenessFromDpiAwarenessContext = _bind(
        _USER32, "GetAwarenessFromDpiAwarenessContext", [ctypes.c_void_p], ctypes.c_int
    )
    _GetDpiForMonitor = _bind(
        ctypes.WinDLL("shcore"),
        "GetDpiForMonitor",
        [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)],
        ctypes.c_long,
    )
except Exception:
    _GetThreadDpiAwarenessContext = None
    _SetThreadDpiAwarenessContext = None
    _GetAwarenessFromDpiAwarenessContext = None
    _GetDpiForMonitor = None


def get_command_line():
    try:
        return _GetCommandLineW() or ""
    except Exception:
        return ""


def get_cli_args(cmd_line=None):
    if cmd_line is None:
        cmd_line = get_command_line()
    if not cmd_line:
        return []
    try:
        argc = ctypes.c_int(0)
        argv = _CommandLineToArgvW(cmd_line, ctypes.byref(argc))
        if not argv:
            return []
        try:
            return [argv[i] for i in range(argc.value)]
        finally:
            _LocalFree(ctypes.cast(argv, ctypes.c_void_p))
    except Exception:
        return []


def set_or_clear_env_var(name, value=None):
    # Synchronize Python os.environ, UCRT (_wputenv_s), and Win32 PEB (SetEnvironmentVariableW).
    # Note: _wputenv_s(name, "") deletes the variable on Windows CRT, so when restoring an
    # originally empty string "", SetEnvironmentVariableW(name, "") MUST be called last.
    if value is None:
        os.environ.pop(name, None)
        if _wputenv_s is not None:
            try:
                _wputenv_s(name, "")
            except Exception:
                pass
        try:
            _SetEnvironmentVariableW(name, None)
        except Exception:
            pass
    else:
        val_str = str(value)
        os.environ[name] = val_str
        if _wputenv_s is not None:
            try:
                _wputenv_s(name, val_str)
            except Exception:
                pass
        try:
            _SetEnvironmentVariableW(name, val_str)
        except Exception:
            pass


def clear_env_var(name):
    set_or_clear_env_var(name, None)


def _read_win32_env_exact(name):
    # Distinguish undefined (None) vs empty string ("") vs non-empty string in Win32 PEB.
    if name in os.environ:
        val = os.environ[name]
        if val != "":
            return val
    try:
        ctypes.set_last_error(0)
        buf = ctypes.create_unicode_buffer(32768)
        n = _GetEnvironmentVariableW(name, buf, len(buf))
        err = ctypes.get_last_error()
        if n > 0:
            return buf.value
        # ERROR_ENVVAR_NOT_FOUND == 203; n == 0 with err != 203 means defined as "" in Win32 PEB.
        if err != 203:
            return ""
    except Exception:
        pass
    return os.environ.get(name, None)


def capture_env_snapshot(source_env=None):
    snap = {}
    for key in SENSITIVE_ENV_VARS:
        if source_env is not None:
            snap[key] = source_env.get(key, None)
        else:
            snap[key] = _read_win32_env_exact(key)
    return json.dumps(snap, separators=(",", ":"))


def lock_qt_override_dpi():
    # Invoke QPlatformScreen::overrideDpi once while QT_FONT_DPI is still in the child
    # environment so QtGui initializes its function-local static DPI cache before cleanup.
    if os.path.exists(DLL_PATH):
        try:
            dll = ctypes.CDLL(DLL_PATH)
            _KEEPALIVE.append(dll)
            if hasattr(dll, "LockQtOverrideDpi"):
                dll.LockQtOverrideDpi.argtypes = []
                dll.LockQtOverrideDpi.restype = None
                dll.LockQtOverrideDpi()
                return True
        except Exception:
            pass

    qt_targets = (
        ("Qt5Gui.dll", b"?overrideDpi@QPlatformScreen@@SA?AU?$QPair@NN@@AEBU2@@Z"),
        ("Qt6Gui.dll", b"?overrideDpi@QPlatformScreen@@SA?AU?$pair@NN@std@@AEBU23@@Z"),
    )
    fn_type = ctypes.CFUNCTYPE(ctypes.POINTER(_DpiPair), ctypes.POINTER(_DpiPair), ctypes.POINTER(_DpiPair))
    for dll_name, sym in qt_targets:
        try:
            hmod = _GetModuleHandleW(dll_name)
            if not hmod:
                continue
            addr = _GetProcAddress(hmod, sym)
            if not addr:
                continue
            fn = fn_type(addr)
            out_pair = _DpiPair(0.0, 0.0)
            in_pair = _DpiPair(96.0, 96.0)
            fn(ctypes.byref(out_pair), ctypes.byref(in_pair))
            return True
        except Exception:
            pass
    return False


def restore_env_from_snapshot():
    raw_snap = _read_win32_env_exact(ENV_SNAPSHOT_VAR)
    if raw_snap:
        try:
            parsed = json.loads(raw_snap)
            if isinstance(parsed, dict):
                for key in SENSITIVE_ENV_VARS:
                    val = parsed.get(key, None)
                    if val is not None and not isinstance(val, str):
                        val = str(val)
                    set_or_clear_env_var(key, val)
        except Exception:
            pass
    clear_env_var("MODO_HIDPI_CHILD")
    clear_env_var("MODO_HIDPI_PMV2")
    clear_env_var(ENV_SNAPSHOT_VAR)


def get_host_exe_path():
    try:
        buf = ctypes.create_unicode_buffer(32768)
        length = _GetModuleFileNameW(None, buf, len(buf))
        if 0 < length < len(buf):
            return buf.value
    except Exception:
        pass
    return ""


def get_host_exe_name(exe_path=None):
    if exe_path is None:
        exe_path = get_host_exe_path()
    return os.path.basename(exe_path).lower() if exe_path else ""


def has_non_gui_cli_flag(args):
    for raw_arg in args[1:]:
        if not raw_arg or raw_arg[0] not in ("-", "/"):
            continue
        opt = raw_arg.lstrip("-") if raw_arg[0] == "-" else raw_arg[1:]
        key = opt.split(":", 1)[0].split("=", 1)[0].lower()
        if key in NON_GUI_CLI_FLAGS:
            return True
    return False


def parse_config_cli_override(args=None):
    # Returns ("noconfig", None), ("custom", path_str), or ("default", None).
    if args is None:
        args = get_cli_args()
    custom_cfg = None
    for raw_arg in args[1:]:
        if not raw_arg or raw_arg[0] not in ("-", "/"):
            continue
        opt = raw_arg.lstrip("-") if raw_arg[0] == "-" else raw_arg[1:]
        low = opt.lower()
        if low == "noconfig":
            return ("noconfig", None)
        if low.startswith("dbon:") or low.startswith("dbon="):
            tokens = low[5:].replace(";", ",").split(",")
            if any(t.strip() == "noconfig" for t in tokens):
                return ("noconfig", None)
        if low.startswith("config:") or low.startswith("config="):
            val = opt[7:].strip().strip('"').strip("'")
            if val:
                custom_cfg = val
    if custom_cfg:
        return ("custom", custom_cfg)
    return ("default", None)


def is_platform_headless():
    try:
        return bool(lx.service.Platform().IsHeadless())
    except Exception:
        return False


def is_headless(exe_path=None, cmd_line=None):
    if is_platform_headless():
        return True
    if get_host_exe_name(exe_path).startswith("modo_cl"):
        return True
    if cmd_line is None:
        cmd_line = get_command_line()
    return has_non_gui_cli_flag(get_cli_args(cmd_line))


def _query_active_monitor_handle():
    pt = _POINT(0, 0)
    try:
        if _GetCursorPos(ctypes.byref(pt)):
            hmon = _MonitorFromPoint(pt, _MONITOR_DEFAULTTONEAREST)
            if hmon:
                return hmon
    except Exception:
        pass
    try:
        hwnd = _GetForegroundWindow()
        if hwnd:
            hmon = _MonitorFromWindow(hwnd, _MONITOR_DEFAULTTONEAREST)
            if hmon:
                return hmon
    except Exception:
        pass
    try:
        return _MonitorFromPoint(_POINT(0, 0), _MONITOR_DEFAULTTOPRIMARY)
    except Exception:
        return None


def get_system_ppi():
    try:
        # Temporarily enter Per-Monitor Aware V2 on the calling thread so GetCursorPos and
        # GetDpiForMonitor return unvirtualized per-monitor DPI even in the initial
        # <dpiAware>false</dpiAware> parent process, and query the cursor/active monitor.
        if _GetDpiForMonitor is not None:
            prev_ctx = None
            switched = False
            try:
                if _SetThreadDpiAwarenessContext is not None:
                    prev_ctx = _SetThreadDpiAwarenessContext(_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
                    switched = bool(prev_ctx)
                can_query = switched
                if not can_query and _GetAwarenessFromDpiAwarenessContext is not None and _GetThreadDpiAwarenessContext is not None:
                    can_query = _GetAwarenessFromDpiAwarenessContext(_GetThreadDpiAwarenessContext()) >= 2
                if can_query:
                    hmon = _query_active_monitor_handle()
                    if hmon:
                        dpi_x = ctypes.c_uint(0)
                        dpi_y = ctypes.c_uint(0)
                        if _GetDpiForMonitor(hmon, 0, ctypes.byref(dpi_x), ctypes.byref(dpi_y)) == 0:
                            if dpi_x.value > 0:
                                return int(dpi_x.value)
            except Exception:
                pass
            finally:
                if switched and prev_ctx and _SetThreadDpiAwarenessContext is not None:
                    try:
                        _SetThreadDpiAwarenessContext(prev_ctx)
                    except Exception:
                        pass

        # GDI fallback when Shcore is unavailable: HORZRES (8) is DWM-virtualized in a DPI-unaware
        # process while DESKTOPHORZRES (118) is physical, so LOGPIXELSX * DESKTOPHORZRES / HORZRES recovers PPI.
        hdc = _GetDC(None)
        if not hdc:
            return 96
        try:
            horz_res = _GetDeviceCaps(hdc, 8)        # HORZRES
            desktop_horz = _GetDeviceCaps(hdc, 118)  # DESKTOPHORZRES
            log_pixels = _GetDeviceCaps(hdc, 88)     # LOGPIXELSX
        finally:
            _ReleaseDC(None, hdc)
        if horz_res > 0 and desktop_horz > 0 and log_pixels > 0:
            return int(round(log_pixels * desktop_horz / horz_res))
    except Exception:
        pass
    return 96


def are_all_monitors_96_ppi():
    if _GetDpiForMonitor is None or _EnumDisplayMonitors is None:
        return get_system_ppi() == 96
    dpis = []

    @_MONITORENUMPROC
    def _cb(hmon, _hdc, _lprect, _lparam):
        dx = ctypes.c_uint(0)
        dy = ctypes.c_uint(0)
        if _GetDpiForMonitor(hmon, 0, ctypes.byref(dx), ctypes.byref(dy)) == 0 and dx.value > 0:
            dpis.append((int(dx.value), int(dy.value)))
        else:
            dpis.append((-1, -1))
        return 1

    prev_ctx = None
    switched = False
    try:
        if _SetThreadDpiAwarenessContext is not None:
            prev_ctx = _SetThreadDpiAwarenessContext(_DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
            switched = bool(prev_ctx)
        _EnumDisplayMonitors(None, None, _cb, 0)
    except Exception:
        return False
    finally:
        if switched and prev_ctx and _SetThreadDpiAwarenessContext is not None:
            try:
                _SetThreadDpiAwarenessContext(prev_ctx)
            except Exception:
                pass
    return bool(dpis) and all(dx == 96 and dy == 96 for dx, dy in dpis)


def build_highdpi_compat_layer(existing):
    # Strip conflicting user/parent DPI compatibility flags (e.g. ~ DPIUNAWARE, GDIDPISCALING)
    # while preserving unrelated __COMPAT_LAYER tokens before appending HighDpiAware.
    conflict_substrings = ("dpiunaware", "gdidpiscaling", "highdpiaware")
    kept = []
    for tok in (existing or "").split():
        if tok == "~":
            continue
        low = tok.lstrip("~").lower()
        if any(c in low for c in conflict_substrings):
            continue
        kept.append(tok)
    kept.append("HighDpiAware")
    return " ".join(kept)


class HighDPIScalingPersistData(object):
    # Parse HighDPIScaling.cfg directly via ElementTree in addition to lx.service.Persistence():
    # bootstrap() runs during lxserv import BEFORE Modo's ConfigMasterEnable() loads user .cfg
    # files into Persistence (so mode_val.GetString(0) raises LXe_NOTFOUND at bootstrap time).
    def __init__(self, cli_args=None):
        self.mode_acc = None
        self.mode_val = None
        self._fallback_mode = "system"

        self.scale_acc = None
        self.scale_val = None
        self._fallback_label = "100%"

        self._load_disk_fallback(cli_args=cli_args)

    def _parse_cfg_atoms(self, cfg_file):
        if not cfg_file or not os.path.isfile(cfg_file):
            return False
        try:
            root = ET.parse(cfg_file).getroot()
            containers = root.findall(f".//atom[@type='{PERSIST_ATOM_NAME}']")
            if not containers:
                return False
            found_value = False
            for container in containers:
                for atom in container.findall("atom"):
                    atype = atom.get("type")
                    text = (atom.text or "").strip()
                    if atype == "mode" and text in MODE_KEYS:
                        self._fallback_mode = text
                        found_value = True
                    elif atype == "custom_scale" and text in SCALE_BY_LABEL:
                        self._fallback_label = text
                        found_value = True
            return found_value
        except Exception:
            return False

    def _parse_cfg_spec(self, spec_path):
        # plat.Path("conf") (MODO*.CFG on Windows) is a directory containing HighDPIScaling.cfg,
        # whereas -config:<file.cfg> / NEXUS_CONFIG may be a file (check it first, then sibling HighDPIScaling.cfg).
        if not spec_path:
            return False
        if os.path.isdir(spec_path):
            return self._parse_cfg_atoms(os.path.join(spec_path, f"{PERSIST_ATOM_NAME}.cfg"))
        if os.path.isfile(spec_path):
            if self._parse_cfg_atoms(spec_path):
                return True
            sibling = os.path.join(os.path.dirname(spec_path), f"{PERSIST_ATOM_NAME}.cfg")
            if os.path.normcase(os.path.abspath(sibling)) != os.path.normcase(os.path.abspath(spec_path)):
                return self._parse_cfg_atoms(sibling)
        return False

    def _load_disk_fallback(self, cli_args=None):
        cfg_mode, custom_path = parse_config_cli_override(cli_args)
        if cfg_mode == "noconfig":
            return

        try:
            plat = lx.service.Platform()
        except Exception:
            plat = None

        # 1. Base import layer (Configs\HighDPIScaling.cfg via ImportPathByIndex) so partial
        #    overrides in conf / -config: merge cleanly rather than resetting unset atoms.
        if plat is not None:
            try:
                count = int(plat.ImportPathCount())
                for idx in range(count):
                    imp_dir = plat.ImportPathByIndex(idx)
                    if imp_dir and self._parse_cfg_atoms(os.path.join(imp_dir, f"{PERSIST_ATOM_NAME}.cfg")):
                        break
            except Exception:
                pass
        else:
            user_dir = (os.environ.get("NEXUS_USER") or "").strip().strip('"').strip("'")
            if not user_dir:
                appdata = (os.environ.get("APPDATA") or "").strip()
                if appdata:
                    user_dir = os.path.join(appdata, "Luxology")
            if user_dir:
                self._parse_cfg_atoms(os.path.join(user_dir, "Configs", f"{PERSIST_ATOM_NAME}.cfg"))

        # 2. Active user config override (-config:<path>, else plat.Path("conf") / NEXUS_CONFIG).
        if cfg_mode == "custom" and custom_path:
            self._parse_cfg_spec(custom_path)
        elif cfg_mode == "default":
            conf_path = None
            if plat is not None:
                try:
                    conf_path = plat.Path("conf")
                except Exception:
                    conf_path = None
            if not conf_path:
                conf_path = (os.environ.get("NEXUS_CONFIG") or "").strip().strip('"').strip("'")
            if conf_path:
                self._parse_cfg_spec(conf_path)

    def get_mode(self):
        if self.mode_val is not None:
            try:
                val = self.mode_val.GetString(0)
                if val in MODE_KEYS:
                    return val
            except Exception:
                pass
        return self._fallback_mode

    def set_mode(self, mode):
        self._fallback_mode = mode
        if self.mode_acc is not None and self.mode_val is not None:
            try:
                self.mode_acc.Append()
                self.mode_val.SetString(0, mode)
            except Exception:
                pass

    def get_custom_scale_label(self):
        if self.scale_val is not None:
            try:
                val = self.scale_val.GetString(0)
                if val in SCALE_BY_LABEL:
                    return val
            except Exception:
                pass
        return self._fallback_label

    def set_custom_scale_label(self, label):
        self._fallback_label = label
        if self.scale_acc is not None and self.scale_val is not None:
            try:
                self.scale_acc.Append()
                self.scale_val.SetString(0, label)
            except Exception:
                pass


class VisHighDPIScalingSettings(lxifc.Visitor):
    def vis_Evaluate(self):
        global _persist_data
        persist_svc = lx.service.Persistence()

        persist_svc.Start("mode", lx.symbol.i_PERSIST_ATOM)
        persist_svc.AddValue(lx.symbol.sTYPE_STRING)
        _persist_data.mode_acc = persist_svc.End()
        _persist_data.mode_val = lx.object.Attributes(_persist_data.mode_acc)

        persist_svc.Start("custom_scale", lx.symbol.i_PERSIST_ATOM)
        persist_svc.AddValue(lx.symbol.sTYPE_STRING)
        _persist_data.scale_acc = persist_svc.End()
        _persist_data.scale_val = lx.object.Attributes(_persist_data.scale_acc)

        return lx.symbol.e_OK


def persist_setup():
    global _persist_data
    if _persist_data is not None:
        return _persist_data
    _persist_data = HighDPIScalingPersistData()
    try:
        persist_svc = lx.service.Persistence()
        persist_vis = VisHighDPIScalingSettings()
        _KEEPALIVE.append(persist_vis)
        persist_svc.Configure(PERSIST_ATOM_NAME, persist_vis)
    except Exception:
        pass
    return _persist_data


def install_early_hooks(scale, dynamic_pmv2=False):
    if (not dynamic_pmv2 and scale <= 1.001) or not os.path.exists(DLL_PATH):
        return
    try:
        dll = ctypes.CDLL(DLL_PATH)
        _KEEPALIVE.append(dll)
        if hasattr(dll, "SetHiDPIConfig"):
            dll.SetHiDPIConfig.argtypes = [ctypes.c_double, ctypes.c_int]
            dll.SetHiDPIConfig.restype = None
            dll.SetHiDPIConfig(float(scale), 1 if dynamic_pmv2 else 0)
        else:
            dll.SetHiDPIScale.argtypes = [ctypes.c_double]
            dll.SetHiDPIScale.restype = None
            dll.SetHiDPIScale(float(scale))
    except Exception:
        pass


class HighDPIScalingNotifier(lxifc.Notifier):
    masterList = {}

    def noti_Name(self):
        return NOTIFIER_NAME

    def noti_AddClient(self, event):
        self.masterList[event.__peekobj__()] = event

    def noti_RemoveClient(self, event):
        self.masterList.pop(event.__peekobj__(), None)

    @classmethod
    def Notify(cls, flags):
        for event_obj in list(cls.masterList.values()):
            try:
                evt = lx.object.CommandEvent(event_obj)
                evt.Event(flags)
            except Exception:
                pass


class HighDPIScalingUIValueHints(lxifc.UIValueHints):
    def __init__(self, options):
        self._options = options

    def uiv_Flags(self):
        return lx.symbol.fVALHINT_POPUPS

    def uiv_PopCount(self):
        return len(self._options)

    def uiv_PopUserName(self, index):
        return self._options[index][1]

    def uiv_PopInternalName(self, index):
        return self._options[index][0]

    def uiv_PopToolTip(self, index):
        if len(self._options[index]) > 2 and self._options[index][2]:
            return self._options[index][2]
        lx.notimpl()


class HighDPIScalingModeCmd(lxu.command.BasicCommand):
    def __init__(self):
        lxu.command.BasicCommand.__init__(self)
        persist_setup()
        self._notifier = None
        self.dyna_Add("mode", lx.symbol.sTYPE_STRING)
        self.basic_SetFlags(0, lx.symbol.fCMDARG_QUERY | lx.symbol.fCMDARG_OPTIONAL)

    def cmd_Flags(self):
        return lx.symbol.fCMD_UI

    def cmd_Tooltip(self):
        msg_svc = lx.service.Message()
        lines = [msg_svc.ArgTypeDesc(MODE_ARG_TYPE)]
        for key in MODE_KEYS:
            name = msg_svc.ArgTypeOptionUserName(MODE_ARG_TYPE, key)
            desc = msg_svc.ArgTypeOptionDesc(MODE_ARG_TYPE, key)
            # \x03(...) sequences are Modo rich-text escapes for inline bullet icons and bold text.
            lines.append(
                f"\x03(i:MIMG_BULLET3,0)\x03(f:FONT_BOLD) {name}\x03(f:FONT_NORMAL):  {desc}"
            )
        return "\n".join(lines)

    def basic_Enable(self, msg):
        return True

    def cmd_NotifyAddClient(self, argument, object):
        if self._notifier is None:
            self._notifier = lx.service.NotifySys().Spawn(NOTIFIER_NAME, "")
        self._notifier.AddClient(object)

    def cmd_NotifyRemoveClient(self, object):
        if self._notifier is not None:
            self._notifier.RemoveClient(object)

    def arg_UIValueHints(self, index):
        if index == 0:
            msg_svc = lx.service.Message()
            ppi = get_system_ppi()
            options = []
            for key in MODE_KEYS:
                name = msg_svc.ArgTypeOptionUserName(MODE_ARG_TYPE, key)
                desc = msg_svc.ArgTypeOptionDesc(MODE_ARG_TYPE, key)
                label = f"{name}: [{ppi} ppi]" if key == "system" else name
                options.append((key, label, desc))
            return HighDPIScalingUIValueHints(options)
        return None

    def cmd_Query(self, index, vaQuery):
        if index == 0:
            pdata = persist_setup()
            va = lx.object.ValueArray()
            va.set(vaQuery)
            va.AddString(pdata.get_mode())
        return lx.result.OK

    def basic_Execute(self, msg, flags):
        pdata = persist_setup()
        if self.dyna_IsSet(0):
            val = self.dyna_String(0, pdata.get_mode())
            if val in MODE_KEYS:
                pdata.set_mode(val)
        HighDPIScalingNotifier.Notify(
            lx.symbol.fCMDNOTIFY_VALUE | lx.symbol.fCMDNOTIFY_DISABLE
        )


class HighDPIScalingCustomScaleCmd(lxu.command.BasicCommand):
    def __init__(self):
        lxu.command.BasicCommand.__init__(self)
        persist_setup()
        self._notifier = None
        self.dyna_Add("scale", lx.symbol.sTYPE_STRING)
        self.basic_SetFlags(
            0,
            lx.symbol.fCMDARG_QUERY
            | lx.symbol.fCMDARG_OPTIONAL
            | lx.symbol.fCMDARG_CAN_QUERY_WHEN_DISABLED,
        )

    def cmd_Flags(self):
        return lx.symbol.fCMD_UI

    def basic_Enable(self, msg):
        pdata = persist_setup()
        if pdata.get_mode() != "custom":
            try:
                msg = lx.object.Message(msg)
                msg.SetMessage(CMD_CUSTOM_SCALE_NAME, "DisabledReason", 0)
            except Exception:
                pass
            return False
        return True

    def cmd_NotifyAddClient(self, argument, object):
        if self._notifier is None:
            self._notifier = lx.service.NotifySys().Spawn(NOTIFIER_NAME, "")
        self._notifier.AddClient(object)

    def cmd_NotifyRemoveClient(self, object):
        if self._notifier is not None:
            self._notifier.RemoveClient(object)

    def arg_UIValueHints(self, index):
        if index == 0:
            return HighDPIScalingUIValueHints([(lbl, lbl) for lbl, _ in SCALE_OPTIONS])
        return None

    def cmd_Query(self, index, vaQuery):
        if index == 0:
            pdata = persist_setup()
            va = lx.object.ValueArray()
            va.set(vaQuery)
            va.AddString(pdata.get_custom_scale_label())
        return lx.result.OK

    def basic_Execute(self, msg, flags):
        pdata = persist_setup()
        if self.dyna_IsSet(0):
            label = self.dyna_String(0, pdata.get_custom_scale_label())
            if label in SCALE_BY_LABEL:
                pdata.set_custom_scale_label(label)
        HighDPIScalingNotifier.Notify(lx.symbol.fCMDNOTIFY_VALUE)


def compute_effective_scale():
    pdata = persist_setup()
    mode = pdata.get_mode()
    if mode == "none":
        return 1.0
    if mode == "system":
        ratio = float(get_system_ppi()) / 96.0
        return max(1.0, min(4.0, ratio))
    return SCALE_BY_LABEL.get(pdata.get_custom_scale_label(), 1.0)


def hide_parent_splash_windows():
    # Hide any early top-level QSplashScreen created on the current thread (Modo 17)
    # so the guardian parent stays invisible while waiting on the child process.
    @_WNDENUMPROC
    def _cb(hwnd, _lparam):
        try:
            if _IsWindowVisible(hwnd):
                _ShowWindow(hwnd, _SW_HIDE)
        except Exception:
            pass
        return 1

    try:
        _EnumThreadWindows(_GetCurrentThreadId(), _cb, 0)
    except Exception:
        pass


def _prepare_stdio_startupinfo():
    # Whitelist valid stdio handles via PROC_THREAD_ATTRIBUTE_HANDLE_LIST ("handle_list")
    # so child Modo preserves stdout/stderr redirection across close_fds=True.
    valid_handles = []
    resolved = []
    for std_id in (_STD_INPUT_HANDLE, _STD_OUTPUT_HANDLE, _STD_ERROR_HANDLE):
        h = None
        try:
            cand = _GetStdHandle(std_id)
            if cand and cand != _INVALID_HANDLE_VALUE and _GetFileType(cand) != _FILE_TYPE_UNKNOWN:
                if _SetHandleInformation(cand, _HANDLE_FLAG_INHERIT, _HANDLE_FLAG_INHERIT):
                    h = int(cand)
                    if h not in valid_handles:
                        valid_handles.append(h)
        except Exception:
            h = None
        resolved.append(h or 0)

    if not valid_handles:
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESTDHANDLES
    si.hStdInput = resolved[0]
    si.hStdOutput = resolved[1]
    si.hStdError = resolved[2]
    si.lpAttributeList = {"handle_list": valid_handles}
    return si


def _create_kill_on_close_job():
    try:
        hjob = _CreateJobObjectW(None, None)
        if not hjob:
            return None
        # SILENT_BREAKAWAY_OK ensures grandchildren spawned by the child Modo process
        # (such as app.restart, Kit restart="YES", or detached external DCC tools) break
        # away from this bridge job instead of being terminated when the child exits.
        for limit_flags in (
            _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            | _JOB_OBJECT_LIMIT_BREAKAWAY_OK
            | _JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK,
            _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | _JOB_OBJECT_LIMIT_BREAKAWAY_OK,
        ):
            info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
            info.BasicLimitInformation.LimitFlags = limit_flags
            if _SetInformationJobObject(
                hjob,
                _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS,
                ctypes.byref(info),
                ctypes.sizeof(info),
            ):
                return hjob
        _CloseHandle(hjob)
        return None
    except Exception:
        return None


def run_guardian_bridge(cmd_line, exe_path, child_env):
    # Keep parent alive waiting on child to preserve PID, exit code, and Job lifetime for
    # launchers/scripts; retry without startupinfo on invalid/ConPTY handles, and os._exit
    # directly so the parent never resumes Modo GUI initialization.
    hide_parent_splash_windows()
    startupinfo = _prepare_stdio_startupinfo()
    hjob = _create_kill_on_close_job()
    exit_code = 0
    try:
        try:
            proc = subprocess.Popen(
                cmd_line,
                executable=exe_path or None,
                env=child_env,
                startupinfo=startupinfo,
                close_fds=True,
            )
        except OSError:
            if startupinfo is None:
                raise
            proc = subprocess.Popen(
                cmd_line,
                executable=exe_path or None,
                env=child_env,
                close_fds=True,
            )
        proc_handle = int(proc._handle)
        if hjob:
            try:
                _AssignProcessToJobObject(hjob, proc_handle)
            except Exception:
                pass
        _WaitForSingleObject(proc_handle, _INFINITE)
        raw_code = ctypes.c_uint32(0)
        if _GetExitCodeProcess(proc_handle, ctypes.byref(raw_code)):
            exit_code = int(raw_code.value)
        else:
            exit_code = int(proc.wait())
    finally:
        if hjob:
            try:
                _CloseHandle(hjob)
            except Exception:
                pass
    os._exit(exit_code)


def build_child_environment(mode, effective_scale, base_env=None):
    child_env = dict(os.environ if base_env is None else base_env)
    if ENV_SNAPSHOT_VAR not in child_env:
        child_env[ENV_SNAPSHOT_VAR] = capture_env_snapshot(
            source_env=None if base_env is None else child_env
        )
    child_env["__COMPAT_LAYER"] = build_highdpi_compat_layer(child_env.get("__COMPAT_LAYER", ""))

    if mode == "system":
        # Per-Monitor V2 dynamic cross-screen mode: Qt tracks each monitor's live DPI via
        # PassThrough without global QT_SCALE_FACTOR or QT_FONT_DPI overrides.
        child_env["QT_ENABLE_HIGHDPI_SCALING"] = "1"
        child_env["QT_SCALE_FACTOR_ROUNDING_POLICY"] = "PassThrough"
        child_env.pop("QT_AUTO_SCREEN_SCALE_FACTOR", None)
        child_env.pop("QT_FONT_DPI", None)
        child_env.pop("QT_SCALE_FACTOR", None)
        child_env["MODO_HIDPI_PMV2"] = "1"
    else:
        # Fixed-scale ("custom") and 1.0x ("none") modes: HighDpiAware + QT_ENABLE_HIGHDPI_SCALING=0
        # + QT_FONT_DPI=96 + explicit QT_SCALE_FACTOR lock deterministic scaling and prevent Qt6 in
        # Modo 17 from auto-scaling when "Do Not Scale" is selected.
        child_env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
        child_env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
        child_env["QT_FONT_DPI"] = "96"
        child_env.pop("QT_SCALE_FACTOR_ROUNDING_POLICY", None)
        child_env.pop("MODO_HIDPI_PMV2", None)
        if effective_scale > 1.001:
            scale_str = (
                str(int(effective_scale))
                if effective_scale == int(effective_scale)
                else str(effective_scale)
            )
        else:
            scale_str = "1"
        child_env["QT_SCALE_FACTOR"] = scale_str

    child_env["MODO_HIDPI_CHILD"] = "1"
    return child_env


def bootstrap():
    # Never relaunch or install GUI/OpenGL hooks in headless / non-GUI CLI mode
    # (modo_cl.exe, -headless, -nowin, -slave, -worker, -telnet, -console).
    exe_path = ""
    cmd_line = ""
    if not is_platform_headless():
        exe_path = get_host_exe_path()
        cmd_line = get_command_line()
    if is_headless(exe_path=exe_path, cmd_line=cmd_line):
        if "MODO_HIDPI_CHILD" in os.environ or ENV_SNAPSHOT_VAR in os.environ:
            restore_env_from_snapshot()
        return
    host_name = get_host_exe_name(exe_path)
    if not host_name.startswith("modo"):
        return

    # Child process:
    # 1. Lock QT_FONT_DPI into QtGui's static cache via QPlatformScreen::overrideDpi.
    # 2. Restore UCRT + Win32 PEB + Python os.environ from _MODO_HIDPI_ENV_SNAPSHOT so
    #    licensingapp.exe, external DCC subprocesses, and app.restart stay unpolluted.
    # 3. Install OpenGL/2D UI/IPR hooks before the main window or any 3D FBO is created.
    if os.environ.get("MODO_HIDPI_CHILD") == "1":
        pmv2_mode = os.environ.get("MODO_HIDPI_PMV2") == "1"
        locked_str = os.environ.get("QT_SCALE_FACTOR")
        lock_qt_override_dpi()
        restore_env_from_snapshot()
        child_scale = None
        if locked_str:
            try:
                parsed = float(locked_str)
                if 1.0 <= parsed <= 4.0:
                    child_scale = parsed
            except (TypeError, ValueError):
                pass
        if child_scale is None:
            child_scale = compute_effective_scale()
        install_early_hooks(child_scale, dynamic_pmv2=pmv2_mode)
        return

    pdata = persist_setup()
    mode = pdata.get_mode()
    effective_scale = compute_effective_scale()

    # Zero-restart short-circuit when target scale is 1.0x and every attached monitor is 96 PPI.
    if effective_scale <= 1.001 and are_all_monitors_96_ppi():
        return

    if not cmd_line:
        return

    child_env = build_child_environment(mode, effective_scale)
    run_guardian_bridge(cmd_line, exe_path, child_env)


try:
    bootstrap()
except Exception:
    pass

try:
    persist_setup()
    lx.bless(HighDPIScalingNotifier, NOTIFIER_NAME)
    lx.bless(HighDPIScalingModeCmd, CMD_MODE_NAME)
    lx.bless(HighDPIScalingCustomScaleCmd, CMD_CUSTOM_SCALE_NAME)
except Exception:
    pass
