# python
# -*- coding: utf-8 -*-
# HighDPIScaling lxserv plugin for Foundry Modo
# 1. Pre-window bootstrapper (__COMPAT_LAYER=HighDpiAware, QT_SCALE_FACTOR, QT_FONT_DPI)
#    and early OpenGL FBO/Viewport IAT hook installer.
# 2. Native Modo Commands (high_dpi_scaling.mode, high_dpi_scaling.custom_scale),
#    Notifier (high_dpi_scaling.notifier), and official lx.service.Persistence()
#    storage (MODO16.1.CFG\HighDPIScaling.cfg).
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
LABEL_BY_SCALE = {val: label for label, val in SCALE_OPTIONS}

NOTIFIER_NAME = "high_dpi_scaling.notifier"
CMD_MODE_NAME = "high_dpi_scaling.mode"
CMD_CUSTOM_SCALE_NAME = "high_dpi_scaling.custom_scale"
MODE_ARG_TYPE = "high_dpi_scaling-mode"
MODE_KEYS = ("system", "custom", "none")

_KEEPALIVE = []
_persist_data = None


def get_command_line():
    kernel32 = ctypes.windll.kernel32
    kernel32.GetCommandLineW.restype = ctypes.c_wchar_p
    return kernel32.GetCommandLineW()


def get_system_ppi():
    try:
        user32 = ctypes.windll.user32
        gdi32 = ctypes.windll.gdi32
        hdc = user32.GetDC(0)
        horz_res = gdi32.GetDeviceCaps(hdc, 8)        # HORZRES
        desktop_horz = gdi32.GetDeviceCaps(hdc, 118)  # DESKTOPHORZRES
        log_pixels = gdi32.GetDeviceCaps(hdc, 88)     # LOGPIXELSX
        user32.ReleaseDC(0, hdc)
        if horz_res > 0 and desktop_horz > 0 and log_pixels > 0:
            return int(round(log_pixels * desktop_horz / horz_res))
    except Exception:
        pass
    return 96


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
                    if atype == "mode" and text in ("system", "custom", "none"):
                        self._fallback_mode = text
                    elif atype == "custom_scale" and text in SCALE_BY_LABEL:
                        self._fallback_label = text
        except Exception:
            pass

    def get_mode(self):
        if self.mode_val is not None:
            try:
                val = self.mode_val.GetString(0)
                if val in ("system", "custom", "none"):
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
            if val in ("system", "custom", "none"):
                pdata.set_mode(val)
        HighDPIScalingNotifier.Notify(lx.symbol.fCMDNOTIFY_CHANGE_ALL)


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
        HighDPIScalingNotifier.Notify(lx.symbol.fCMDNOTIFY_CHANGE_ALL)


def bootstrap():
    ppi = get_system_ppi()
    pdata = persist_setup()
    mode = pdata.get_mode()
    custom_label = pdata.get_custom_scale_label()
    custom_scale = SCALE_BY_LABEL.get(custom_label, 1.0)

    if mode == "none":
        effective_scale = 1.0
    elif mode == "system":
        effective_scale = nearest_scale_for_ppi(ppi)
    else:
        effective_scale = custom_scale

    # If this IS already the child process, install OpenGL Viewport IAT hooks
    # BEFORE any Modo window or FBO is created!
    if os.environ.get("MODO_HIDPI_CHILD") == "1":
        install_early_hooks(effective_scale)
        return

    # Prepare purely in-memory process environment variables (ZERO registry access)
    child_env = os.environ.copy()
    existing_compat = child_env.get("__COMPAT_LAYER", "").strip()
    if "HIGHDPIAWARE" not in existing_compat.upper():
        child_env["__COMPAT_LAYER"] = f"{existing_compat} HighDpiAware".strip()

    child_env["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    child_env["QT_AUTO_SCREEN_SCALE_FACTOR"] = "0"

    if effective_scale > 1.001:
        scale_str = str(int(effective_scale)) if effective_scale == int(effective_scale) else str(effective_scale)
        child_env["QT_SCALE_FACTOR"] = scale_str
        child_env["QT_FONT_DPI"] = "96"
    else:
        child_env["QT_SCALE_FACTOR"] = "1"
        child_env.pop("QT_FONT_DPI", None)

    child_env["MODO_HIDPI_CHILD"] = "1"

    cmd_line = get_command_line()
    subprocess.Popen(cmd_line, env=child_env)
    os._exit(0)


try:
    bootstrap()
    persist_setup()
    lx.bless(HighDPIScalingNotifier, NOTIFIER_NAME)
    lx.bless(HighDPIScalingModeCmd, CMD_MODE_NAME)
    lx.bless(HighDPIScalingCustomScaleCmd, CMD_CUSTOM_SCALE_NAME)
except Exception:
    pass
