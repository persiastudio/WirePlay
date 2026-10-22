#!/usr/bin/env python3
"""Single-variant QuickTime Docking probe.

    python tools/qt_probe.py A

ONE variant per process, always. This tool used to run four variants
back-to-back in a single process; that version blue-screened the
development machine (WDF_VIOLATION, bugcheck 0x10D) three times. The
working hypothesis, consistent with every crash and with the app never
crashing: repeatedly cycling enable/disable while accumulating pyusb
device objects in one process eventually pushes an invalid handle
through the libusb0/usbccgp stack. The app -- one process, one session,
one opened handle, disposed once -- has never crashed the machine.

So this probe now mirrors the app's lifecycle exactly: it reuses
engine/quicktime_backend.py (no divergent copy of the sequence), opens
one session, listens, closes it, exits. To test another variant, run
the tool again: a new process, like the app.

Cleanup rule, learned the hard way: only a device this process opened
(via a control transfer or set_configuration) may be disposed.
Enumeration results that were only looked at are dropped, never
disposed -- disposing a never-opened device is regression-tested as
banned in the backend.
"""
import sys
import time

sys.path.insert(0, "engine")

import usb.core  # noqa: E402
import usb.util  # noqa: E402
import quicktime_backend as qt  # noqa: E402

LISTEN_SECONDS = 8.0


def log(m):
    print("[probe] %s" % m, flush=True)


def _select_capture_config(device):
    """(config_index, in_ep, out_ep) for the config carrying ff/2a."""
    for i in range(device.bNumConfigurations):
        cfg = usb.core.Configuration(device, i)
        for intf in cfg:
            if intf.bInterfaceSubClass == qt.USB_INTERFACE_SUBCLASS_AV:
                in_ep = out_ep = None
                for ep in intf:
                    if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
                        in_ep = ep
                    else:
                        out_ep = ep
                return i + 1, in_ep, out_ep
    return None, None, None


def _listen(device, in_ep) -> bool:
    """True if a device ping shows up within LISTEN_SECONDS."""
    splitter = qt.FrameSplitter()
    deadline = time.monotonic() + LISTEN_SECONDS
    while time.monotonic() < deadline:
        try:
            raw = device.read(in_ep, 1024 * 1024, 1000)
        except usb.core.USBTimeoutError:
            continue
        except usb.core.USBError as exc:
            if "timeout" in str(exc).lower():
                continue
            log("USB error while listening: %s" % exc)
            return False
        for body in splitter.feed(bytes(raw)):
            magic, _clock, _type, _payload = qt.parse_header(body)
            if magic == qt.PING_MAGIC:
                return True
    return False


def _cleanup(device) -> None:
    """Disable the capture config, then dispose -- on the current device.

    The control transfer is what opens a pyusb device, so issuing it
    first makes the dispose below legitimate even for a handle that was
    only enumerated. Disposing without it is the banned pattern.
    """
    if device is None:
        return
    try:
        device.ctrl_transfer(*qt.CTRL_DISABLE_AV_CONFIG, b"", 1500)
    except Exception:
        pass
    try:
        usb.util.dispose_resources(device)
    except Exception:
        pass


def variant_a() -> bool:
    """The app's exact sequence, via the app's own transport code."""
    transport = qt.QuickTimeTransport(logger=log)
    transport.open()
    splitter = qt.FrameSplitter()
    heard = {"ping": False}
    original_handle = transport._handle

    def spy(body):
        magic, _c, _t, _p = qt.parse_header(body)
        if magic == qt.PING_MAGIC:
            heard["ping"] = True
        return original_handle(body)

    transport._handle = spy
    deadline = time.monotonic() + LISTEN_SECONDS
    while time.monotonic() < deadline and not heard["ping"]:
        transport.pump()
    transport.close_session()
    return heard["ping"]


def _manual(variant: str) -> bool:
    device = qt._find_device(usb, None, log)
    current = device
    try:
        if variant == "C":
            device.set_configuration()
        if variant in ("B", "C"):
            result = device.ctrl_transfer(*qt.CTRL_ENABLE_AV_CONFIG, b"")
            if result:
                log("enable rejected: %s" % result)
                return False
        time.sleep(1.5)  # the phone's settle time; proven, do not shorten
        current = qt._find_device(usb, None, log)
        config_index, in_ep, out_ep = _select_capture_config(current)
        if config_index is None:
            log("capture interface never appeared")
            return False
        if variant == "D":
            # No set_configuration after re-enumeration: listen on the
            # config the phone selected by itself, if any.
            return _listen(current, in_ep)
        current.set_configuration(config_index)
        current.ctrl_transfer(*qt.CTRL_SET_INTERFACE_HI, b"")
        current.ctrl_transfer(*qt.CTRL_SET_INTERFACE_LO, b"")
        return _listen(current, in_ep)
    finally:
        _cleanup(current)


_VARIANTS = {
    "A": "disable+default+enable (the app's sequence, via its own code)",
    "B": "enable only",
    "D": "enable, no set_configuration after re-enumeration",
    # "C" (default+enable, no disable first) is deliberately NOT offered:
    # an instrumented run on 2026-10-20 traced a WDF_VIOLATION bugcheck
    # to the opening set_configuration() of variant C after A and B had
    # cycled the device in the same process. Isolated C might behave
    # differently, but re-running the documented trigger to find out
    # buys no information the app needs.
}


def main(argv) -> int:
    if len(argv) != 1 or argv[0].upper() not in _VARIANTS:
        print(__doc__)
        print("usage: python tools/qt_probe.py <A|B|D>   (one variant per run)")
        return 2
    variant = argv[0].upper()
    log("variant %s: %s" % (variant, _VARIANTS[variant]))
    log("keep the phone awake and unlocked; listening %.0f s" % LISTEN_SECONDS)
    try:
        heard = variant_a() if variant == "A" else _manual(variant)
    except Exception as exc:
        log("variant %s failed: %s: %s" % (variant, type(exc).__name__, exc))
        return 1
    log("=== variant %s -> ping heard: %s ===" % (variant, heard))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
