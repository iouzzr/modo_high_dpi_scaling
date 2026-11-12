# python
# Copyright 2026 iouzzr
# SPDX-License-Identifier: Apache-2.0
#
# HighDPIScaling lxserv plugin for Foundry Modo (16.x & 17.x)
# 1. Pre-window bootstrapper (__COMPAT_LAYER=HighDpiAware, QT_SCALE_FACTOR, QT_FONT_DPI)
#    and early OpenGL FBO/Viewport + 2D UI hook installer.
# 2. Native Modo Commands (high_dpi_scaling.mode, high_dpi_scaling.custom_scale),
#    Notifier (high_dpi_scaling.notifier), and official lx.service.Persistence()
#    storage (HighDPIScaling.cfg).
import os
import ctypes
import subprocess
import xml.etree.ElementTree as ET
import lx
import lxifc
import lxu.command

KIT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL_PATH = os.path.join(KIT_DIR, "high_dpi_scaling.dll")
PERSIST_ATOM_NAME = "HighDPIScaling"

ALLOWED_SCALES = (1.0, 1.25, 1.5, 2.0)
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

_KEEPALIVE = []
_persist_data = None


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def _bind(dll, name, argtypes, restype):
    fn = getattr(dll, name)
    fn.argtypes = argtypes
    fn.restype = restype
    return fn


_KERNEL32 = ctypes.WinDLL("kernel32")
_SHELL32 = ctypes.WinDLL("shell32")
_USER32 = ctypes.WinDLL("user32")
_GDI32 = ctypes.WinDLL("gdi32")

_GetCommandLineW = _bind(_KERNEL32, "GetCommandLineW", [], ctypes.c_wchar_p)
_LocalFree = _bind(_KERNEL32, "LocalFree", [ctypes.c_void_p], ctypes.c_void_p)
_SetEnvironmentVariableW = _bind(
    _KERNEL32, "SetEnvironmentVariableW", [ctypes.c_wchar_p, ctypes.c_wchar_p], ctypes.c_int
)
_GetModuleFileNameW = _bind(
    _KERNEL32, "GetModuleFileNameW", [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32], ctypes.c_uint32
)
_CommandLineToArgvW = _bind(
    _SHELL32, "CommandLineToArgvW", [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)], ctypes.POINTER(ctypes.c_wchar_p)
)
_GetDC = _bind(_USER32, "GetDC", [ctypes.c_void_p], ctypes.c_void_p)
_ReleaseDC = _bind(_USER32, "ReleaseDC", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int)
_MonitorFromPoint = _bind(_USER32, "MonitorFromPoint", [_POINT, ctypes.c_uint32], ctypes.c_void_p)
_GetDeviceCaps = _bind(_GDI32, "GetDeviceCaps", [ctypes.c_void_p, ctypes.c_int], ctypes.c_int)

try:
    _GetThreadDpiAwarenessContext = _bind(_USER32, "GetThreadDpiAwarenessContext", [], ctypes.c_void_p)
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


def clear_env_var(name):
    os.environ.pop(name, None)
    try:
        _SetEnvironmentVariableW(name, None)
    except Exception:
        pass


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


def get_system_ppi():
    try:
        # When running Per-Monitor DPI Aware (e.g. under __COMPAT_LAYER=HighDpiAware),
        # query live effective monitor DPI via Shcore!GetDpiForMonitor so dynamic
        # display scale changes or hot-plugged monitors without Windows sign-out are
        # accurately detected instead of reading a stale logon GDI LOGPIXELSX value.
        if _GetDpiForMonitor is not None:
            try:
                awareness = _GetAwarenessFromDpiAwarenessContext(_GetThreadDpiAwarenessContext())
                if awareness >= 2:
                    hmon = _MonitorFromPoint(_POINT(0, 0), 1)  # MONITOR_DEFAULTTOPRIMARY
                    if hmon:
                        dpi_x = ctypes.c_uint(0)
                        dpi_y = ctypes.c_uint(0)
                        if _GetDpiForMonitor(hmon, 0, ctypes.byref(dpi_x), ctypes.byref(dpi_y)) == 0:
                            if dpi_x.value > 0:
                                return int(dpi_x.value)
            except Exception:
                pass

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


def build_highdpi_compat_layer(existing):
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


def nearest_scale_for_ppi(ppi):
    ratio = float(ppi) / 96.0
    return min(ALLOWED_SCALES, key=lambda s: abs(s - ratio))


class HighDPIScalingPersistData(object):
    def __init__(self):
        self.mode_acc = None
        self.mode_val = None
        self._fallback_mode = "system"

        self.scale_acc = None
        self.scale_val = None
        self._fallback_label = "100%"

        try:
            conf_path = lx.service.Platform().Path("conf")
            cfg_file = os.path.join(conf_path, f"{PERSIST_ATOM_NAME}.cfg")
            if os.path.isfile(cfg_file):
                root = ET.parse(cfg_file).getroot()
                for atom in root.findall(f".//atom[@type='{PERSIST_ATOM_NAME}']/atom"):
                    atype = atom.get("type")
                    text = (atom.text or "").strip()
                    if atype == "mode" and text in MODE_KEYS:
                        self._fallback_mode = text
                    elif atype == "custom_scale" and text in SCALE_BY_LABEL:
                        self._fallback_label = text
        except Exception:
            pass

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


def install_early_hooks(scale):
    if scale <= 1.001 or not os.path.exists(DLL_PATH):
        return
    try:
        dll = ctypes.CDLL(DLL_PATH)
        _KEEPALIVE.append(dll)
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
        return nearest_scale_for_ppi(get_system_ppi())
    return SCALE_BY_LABEL.get(pdata.get_custom_scale_label(), 1.0)


def bootstrap():
    # Never relaunch or install GUI/OpenGL hooks in headless / non-GUI CLI mode
    # (modo_cl.exe, -headless, -nowin, -slave, -worker, -telnet, -console).
    exe_path = ""
    cmd_line = ""
    if not is_platform_headless():
        exe_path = get_host_exe_path()
        cmd_line = get_command_line()
    if is_headless(exe_path=exe_path, cmd_line=cmd_line):
        if "MODO_HIDPI_CHILD" in os.environ:
            clear_env_var("MODO_HIDPI_CHILD")
        return

    # If this IS already the child process, clear MODO_HIDPI_CHILD from the
    # environment block (so future app.restart spawns a fresh bootstrap) and
    # install OpenGL/2D UI IAT hooks using the exact scale locked into QT_SCALE_FACTOR
    # by the parent BEFORE any Modo window or FBO is created!
    if os.environ.get("MODO_HIDPI_CHILD") == "1":
        locked_str = os.environ.get("QT_SCALE_FACTOR")
        clear_env_var("MODO_HIDPI_CHILD")
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
        install_early_hooks(child_scale)
        return

    effective_scale = compute_effective_scale()

    # Prepare purely in-memory process environment variables (ZERO registry access)
    child_env = os.environ.copy()
    child_env["__COMPAT_LAYER"] = build_highdpi_compat_layer(child_env.get("__COMPAT_LAYER", ""))
    child_env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    child_env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"
    child_env["QT_FONT_DPI"] = "96"

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

    if not cmd_line:
        return
    subprocess.Popen(cmd_line, executable=exe_path or None, env=child_env)
    os._exit(0)


try:
    bootstrap()
    persist_setup()
    lx.bless(HighDPIScalingNotifier, NOTIFIER_NAME)
    lx.bless(HighDPIScalingModeCmd, CMD_MODE_NAME)
    lx.bless(HighDPIScalingCustomScaleCmd, CMD_CUSTOM_SCALE_NAME)
except Exception:
    pass
