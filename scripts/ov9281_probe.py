"""
Probe an OV9281 UVC camera (or any USB webcam) through Windows' built-in
usbvideo/DirectShow driver stack - no vendor SDK, no driver replacement.

Two independent probes are performed:
  1. DirectShow camera controls (IAMCameraControl / IAMVideoProcAmp), reached
     directly via comtypes COM bindings - gives exposure, gain, brightness,
     focus, pan/tilt/zoom, white balance, etc. with min/max/step/default and
     whether each supports Auto and/or Manual mode.
  2. Video stream modes (resolution / FPS / pixel format) via OpenCV's
     DirectShow backend (cv2.CAP_DSHOW).

By default properties are actually set to their min/max and read back to
confirm they are really controllable (not just "range reported"), then
restored to their original value. Use --dry-run to only query, never write.

Usage:
    python ov9281_probe.py
    python ov9281_probe.py --vid 1BCF --pid 28C4 --json report.json
    python ov9281_probe.py --dry-run --skip-video
"""
import argparse
import ctypes
import json
import sys
import time
from ctypes import POINTER, byref, c_int, c_long, c_ulong, c_ulonglong, c_void_p, c_wchar_p, cast

import comtypes
import comtypes.client
from comtypes import COMMETHOD, GUID, HRESULT, IUnknown
from comtypes.automation import VARIANT

try:
    import cv2
except ImportError:
    cv2 = None


# --------------------------------------------------------------------------
# DirectShow COM interfaces (minimal hand-written bindings, no external
# typelib needed - comtypes builds the vtable from the _methods_ order, so
# base interfaces only need methods declared up to the last one we call).
# --------------------------------------------------------------------------

class IPersist(IUnknown):
    _iid_ = GUID('{0000010c-0000-0000-C000-000000000046}')
    _methods_ = [
        COMMETHOD([], HRESULT, 'GetClassID', (['out'], POINTER(GUID), 'pClassID')),
    ]


class IPersistStream(IPersist):
    _iid_ = GUID('{00000109-0000-0000-C000-000000000046}')
    _methods_ = [
        COMMETHOD([], HRESULT, 'IsDirty'),
        COMMETHOD([], HRESULT, 'Load', (['in'], POINTER(IUnknown), 'pStm')),
        COMMETHOD([], HRESULT, 'Save', (['in'], POINTER(IUnknown), 'pStm'), (['in'], c_int, 'fClearDirty')),
        COMMETHOD([], HRESULT, 'GetSizeMax', (['out'], POINTER(c_ulonglong), 'pcbSize')),
    ]


class IMoniker(IPersistStream):
    _iid_ = GUID('{0000000f-0000-0000-C000-000000000046}')
    _methods_ = [
        COMMETHOD([], HRESULT, 'BindToObject',
                   (['in'], POINTER(IUnknown), 'pbc'),
                   (['in'], POINTER(IUnknown), 'pmkToLeft'),
                   (['in'], POINTER(GUID), 'riidResult'),
                   (['out', 'iid_is', 'riidResult'], POINTER(POINTER(IUnknown)), 'ppvResult')),
        COMMETHOD([], HRESULT, 'BindToStorage',
                   (['in'], POINTER(IUnknown), 'pbc'),
                   (['in'], POINTER(IUnknown), 'pmkToLeft'),
                   (['in'], POINTER(GUID), 'riid'),
                   (['out', 'iid_is', 'riid'], POINTER(POINTER(IUnknown)), 'ppvObj')),
    ]


class IEnumMoniker(IUnknown):
    _iid_ = GUID('{00000102-0000-0000-C000-000000000046}')
    _methods_ = [
        COMMETHOD([], HRESULT, 'Next',
                   (['in'], c_ulong, 'celt'),
                   (['out'], POINTER(POINTER(IMoniker)), 'rgelt'),
                   (['out'], POINTER(c_ulong), 'pceltFetched')),
        COMMETHOD([], HRESULT, 'Skip', (['in'], c_ulong, 'celt')),
        COMMETHOD([], HRESULT, 'Reset'),
        COMMETHOD([], HRESULT, 'Clone', (['out'], POINTER(POINTER(IUnknown)), 'ppenum')),
    ]


class ICreateDevEnum(IUnknown):
    _iid_ = GUID('{29840822-5B84-11D0-BD3B-00A0C911CE86}')
    _methods_ = [
        COMMETHOD([], HRESULT, 'CreateClassEnumerator',
                   (['in'], POINTER(GUID), 'clsidDeviceClass'),
                   (['out'], POINTER(POINTER(IEnumMoniker)), 'ppEnumMoniker'),
                   (['in'], c_ulong, 'dwFlags')),
    ]


class IPropertyBag(IUnknown):
    _iid_ = GUID('{55272A00-42CB-11CE-8135-00AA004BB851}')
    _methods_ = [
        COMMETHOD([], HRESULT, 'Read',
                   (['in'], c_wchar_p, 'pszPropName'),
                   (['out'], POINTER(VARIANT), 'pVar'),
                   (['in'], c_void_p, 'pErrorLog')),
        COMMETHOD([], HRESULT, 'Write',
                   (['in'], c_wchar_p, 'pszPropName'),
                   (['in'], POINTER(VARIANT), 'pVar')),
    ]


def _control_methods():
    # IAMVideoProcAmp and IAMCameraControl are binary-identical (GetRange/Set/Get)
    return [
        COMMETHOD([], HRESULT, 'GetRange',
                   (['in'], c_long, 'Property'),
                   (['out'], POINTER(c_long), 'pMin'),
                   (['out'], POINTER(c_long), 'pMax'),
                   (['out'], POINTER(c_long), 'pSteppingDelta'),
                   (['out'], POINTER(c_long), 'pDefault'),
                   (['out'], POINTER(c_long), 'pCapsFlags')),
        COMMETHOD([], HRESULT, 'Set',
                   (['in'], c_long, 'Property'),
                   (['in'], c_long, 'lValue'),
                   (['in'], c_long, 'Flags')),
        COMMETHOD([], HRESULT, 'Get',
                   (['in'], c_long, 'Property'),
                   (['out'], POINTER(c_long), 'lValue'),
                   (['out'], POINTER(c_long), 'Flags')),
    ]


class IAMVideoProcAmp(IUnknown):
    _iid_ = GUID('{C6E13360-30AC-11d0-A18C-00A0C9118956}')
    _methods_ = _control_methods()


class IAMCameraControl(IUnknown):
    _iid_ = GUID('{C6E13370-30AC-11d0-A18C-00A0C9118956}')
    _methods_ = _control_methods()


CLSID_SystemDeviceEnum = GUID('{62BE5D10-60EB-11d0-BD3B-00A0C911CE86}')
CLSID_VideoInputDeviceCategory = GUID('{860BB310-5D01-11d0-BD3B-00A0C911CE86}')

FLAGS_AUTO = 0x0001
FLAGS_MANUAL = 0x0002

# VideoProcAmpProperty (strmif.h)
VIDEOPROCAMP_PROPS = {
    'Brightness': 0, 'Contrast': 1, 'Hue': 2, 'Saturation': 3, 'Sharpness': 4,
    'Gamma': 5, 'ColorEnable': 6, 'WhiteBalance': 7, 'BacklightCompensation': 8,
    'Gain': 9,
}

# CameraControlProperty (strmif.h)
CAMERACONTROL_PROPS = {
    'Pan': 0, 'Tilt': 1, 'Roll': 2, 'Zoom': 3, 'Exposure': 4, 'Iris': 5, 'Focus': 6,
}


# --------------------------------------------------------------------------
# Device enumeration
# --------------------------------------------------------------------------

def enumerate_devices():
    """Return [{'index', 'name', 'device_path', 'moniker'}] in DirectShow
    enumeration order (same order OpenCV's CAP_DSHOW backend uses)."""
    dev_enum = comtypes.client.CreateObject(CLSID_SystemDeviceEnum, interface=ICreateDevEnum)
    enum_moniker = dev_enum.CreateClassEnumerator(CLSID_VideoInputDeviceCategory, 0)
    devices = []
    if not enum_moniker:
        return devices
    index = 0
    while True:
        result = enum_moniker.Next(1)
        moniker, fetched = result if isinstance(result, tuple) else (result, 1 if result else 0)
        if not moniker or not fetched:
            break
        pbag = cast(moniker.BindToStorage(None, None, IPropertyBag._iid_), POINTER(IPropertyBag))
        name = pbag.Read('FriendlyName', None)
        try:
            device_path = pbag.Read('DevicePath', None)
        except Exception:
            device_path = ''
        devices.append({'index': index, 'name': name, 'device_path': device_path or '', 'moniker': moniker})
        index += 1
    return devices


def find_device(devices, vid, pid):
    needle = (f'vid_{vid}'.lower(), f'pid_{pid}'.lower())
    for dev in devices:
        path = dev['device_path'].lower()
        if needle[0] in path and needle[1] in path:
            return dev
    return None


def bind_controls(moniker):
    """Return (IAMVideoProcAmp or None, IAMCameraControl or None) for a device moniker."""
    unk = moniker.BindToObject(None, None, IUnknown._iid_)
    vpa = cam = None
    try:
        vpa = unk.QueryInterface(IAMVideoProcAmp)
    except Exception:
        pass
    try:
        cam = unk.QueryInterface(IAMCameraControl)
    except Exception:
        pass
    return vpa, cam


# --------------------------------------------------------------------------
# Property probing
# --------------------------------------------------------------------------

def flags_to_str(flags):
    if flags == FLAGS_AUTO:
        return 'auto'
    if flags == FLAGS_MANUAL:
        return 'manual'
    if flags is None:
        return None
    return f'0x{flags:x}'


def probe_property(iface, name, prop_id, dry_run):
    result = {'name': name, 'id': prop_id, 'supported': False}
    try:
        mn, mx, step, default, caps = iface.GetRange(prop_id)
    except Exception as exc:
        result['error'] = str(exc)
        return result

    result.update({
        'supported': True,
        'min': mn, 'max': mx, 'step': step, 'default': default,
        'supports_auto': bool(caps & FLAGS_AUTO),
        'supports_manual': bool(caps & FLAGS_MANUAL),
    })

    if name == 'Exposure':
        result['default_seconds'] = 2.0 ** default

    try:
        orig_val, orig_flags = iface.Get(prop_id)
        result['current_value'] = orig_val
        result['current_flags'] = flags_to_str(orig_flags)
        if name == 'Exposure':
            result['current_seconds'] = 2.0 ** orig_val
    except Exception as exc:
        result['get_error'] = str(exc)
        return result

    if dry_run:
        result['write_tested'] = False
        return result

    write_flags = FLAGS_MANUAL if (caps & FLAGS_MANUAL) else (caps or FLAGS_MANUAL)
    probe_values = sorted({mn, mx})
    round_trip = []
    write_ok = True
    try:
        for test_val in probe_values:
            iface.Set(prop_id, test_val, write_flags)
            time.sleep(0.02)
            readback, _ = iface.Get(prop_id)
            round_trip.append({'set': test_val, 'readback': readback})
            if readback != test_val:
                write_ok = False
        # restore
        restore_flags = orig_flags if orig_flags in (FLAGS_AUTO, FLAGS_MANUAL) else write_flags
        iface.Set(prop_id, orig_val, restore_flags)
    except Exception as exc:
        write_ok = False
        result['write_error'] = str(exc)

    result['write_tested'] = True
    result['write_ok'] = write_ok
    result['round_trip'] = round_trip
    return result


def probe_all_controls(vpa, cam, dry_run):
    results = {'VideoProcAmp': [], 'CameraControl': []}
    if vpa is not None:
        for name, prop_id in VIDEOPROCAMP_PROPS.items():
            results['VideoProcAmp'].append(probe_property(vpa, name, prop_id, dry_run))
    if cam is not None:
        for name, prop_id in CAMERACONTROL_PROPS.items():
            results['CameraControl'].append(probe_property(cam, name, prop_id, dry_run))
    return results


# --------------------------------------------------------------------------
# Video stream modes (resolution / FPS / pixel format) via OpenCV + DirectShow
# --------------------------------------------------------------------------

CANDIDATE_RESOLUTIONS = [
    (320, 240), (640, 400), (640, 480), (800, 600),
    (1280, 720), (1280, 800), (1920, 1080),
]
CANDIDATE_FPS = [15, 30, 60, 90, 100, 120, 150, 160, 200, 210]
CANDIDATE_FOURCC = ['MJPG', 'YUY2', 'NV12', 'UYVY', 'GREY', 'Y800', 'BGR3']


BACKENDS = {}
if cv2 is not None:
    BACKENDS = {'dshow': cv2.CAP_DSHOW, 'msmf': cv2.CAP_MSMF}


def probe_video_modes(cv_index, backend_name):
    if cv2 is None:
        return {'error': 'opencv-python not installed (pip install opencv-python)'}

    modes = {'backend': backend_name, 'resolutions': [], 'fourcc': []}

    cap = cv2.VideoCapture(cv_index, BACKENDS[backend_name])
    if not cap.isOpened():
        return {'error': f'could not open camera at index {cv_index} via {backend_name}'}

    try:
        native_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        native_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        native_fps = cap.get(cv2.CAP_PROP_FPS)
        modes['native'] = {'width': native_w, 'height': native_h, 'fps': native_fps}

        for w, h in CANDIDATE_RESOLUTIONS:
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, w)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, h)
            actual_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            actual_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            ok, frame = cap.read()
            best_fps = None
            if ok:
                for fps in CANDIDATE_FPS:
                    cap.set(cv2.CAP_PROP_FPS, fps)
                    got_fps = cap.get(cv2.CAP_PROP_FPS)
                    if got_fps and abs(got_fps - fps) < 0.5:
                        best_fps = got_fps
            modes['resolutions'].append({
                'requested': f'{w}x{h}', 'applied': f'{actual_w}x{actual_h}',
                'frame_delivered': bool(ok),
                'frame_shape': list(frame.shape) if ok and frame is not None else None,
                'max_matched_fps': best_fps,
            })

        # restore native size before fourcc tests
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, native_w)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, native_h)

        for fourcc_str in CANDIDATE_FOURCC:
            fourcc = cv2.VideoWriter_fourcc(*fourcc_str)
            cap.set(cv2.CAP_PROP_FOURCC, fourcc)
            ok, frame = cap.read()
            applied = int(cap.get(cv2.CAP_PROP_FOURCC))
            applied_str = ''.join([chr((applied >> (8 * i)) & 0xFF) for i in range(4)]) if applied > 0 else ''
            modes['fourcc'].append({
                'requested': fourcc_str, 'applied': applied_str,
                'frame_delivered': bool(ok),
                'frame_shape': list(frame.shape) if ok and frame is not None else None,
            })
    finally:
        cap.release()

    return modes


PREVIEW_WIDTH, PREVIEW_HEIGHT = 1280, 800


def show_live_feed(cv_index, backend_name, width, height):
    if cv2 is None:
        print('Cannot show live feed: opencv-python not installed.')
        return

    cap = cv2.VideoCapture(cv_index, BACKENDS[backend_name])
    if not cap.isOpened():
        print(f'Cannot show live feed: could not open camera at index {cv_index} via {backend_name}.')
        return

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
    actual_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    window = f'OV9281 Live Feed - {actual_w}x{actual_h} ({backend_name})'
    cv2.namedWindow(window, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window, actual_w, actual_h)
    print(f'\nLive feed at {actual_w}x{actual_h} via {backend_name}. Press "q" or ESC to close.')

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                print('Frame grab failed, stopping live feed.')
                break
            cv2.imshow(window, frame)
            key = cv2.waitKey(1) & 0xFF
            if key in (ord('q'), 27):
                break
            if cv2.getWindowProperty(window, cv2.WND_PROP_VISIBLE) < 1:
                break
    finally:
        cap.release()
        cv2.destroyWindow(window)


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------

def print_prop_table(title, props):
    print(f'\n--- {title} ---')
    if not props:
        print('  (interface not available on this device)')
        return
    for p in props:
        if not p['supported']:
            print(f"  {p['name']:22s} NOT SUPPORTED ({p.get('error', '?')})")
            continue
        auto_str = 'auto' if p['supports_auto'] else '-'
        manual_str = 'manual' if p['supports_manual'] else '-'
        line = (f"  {p['name']:22s} range=[{p['min']:>6},{p['max']:>6}] step={p['step']:<4} "
                f"default={p['default']:<6} caps=({auto_str}/{manual_str}) "
                f"current={p.get('current_value')} ({p.get('current_flags')})")
        if p.get('write_tested'):
            status = 'OK' if p.get('write_ok') else 'FAILED'
            line += f'  write-test={status}'
        if 'default_seconds' in p:
            line += f"  [{p['default_seconds']:.6g}s default, {p.get('current_seconds', 0):.6g}s current]"
        print(line)


def print_video_modes(modes):
    print(f"\n--- Video stream modes (OpenCV / backend={modes.get('backend', '?')}) ---")
    if 'error' in modes:
        print(f"  {modes['error']}")
        return
    native = modes.get('native', {})
    print(f"  Native/current: {native.get('width')}x{native.get('height')} @ {native.get('fps')} fps")
    print('  Resolution tests:')
    for r in modes['resolutions']:
        status = 'OK' if r['frame_delivered'] else 'no frame'
        print(f"    requested={r['requested']:<10} applied={r['applied']:<10} "
              f"frame={status:<8} matched_fps={r['max_matched_fps']}")
    print('  Pixel format (FOURCC) tests:')
    for f in modes['fourcc']:
        status = 'OK' if f['frame_delivered'] else 'no frame'
        print(f"    requested={f['requested']:<6} applied={f['applied'] or '?':<6} frame={status}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--vid', default='1BCF', help='USB vendor ID (hex, default 1BCF)')
    parser.add_argument('--pid', default='28C4', help='USB product ID (hex, default 28C4)')
    parser.add_argument('--dry-run', action='store_true', help='Only query ranges/current values, never write')
    parser.add_argument('--skip-video', action='store_true', help='Skip OpenCV resolution/FPS/FOURCC probing')
    parser.add_argument('--backend', choices=['dshow', 'msmf', 'both'], default='both',
                         help='OpenCV capture backend(s) to test video modes with (default: both)')
    parser.add_argument('--json', metavar='PATH', help='Write full report as JSON to PATH')
    parser.add_argument('--preview', action='store_true',
                         help=f'Open a live video window fixed at {PREVIEW_WIDTH}x{PREVIEW_HEIGHT} after probing')
    parser.add_argument('--preview-backend', choices=['dshow', 'msmf'],
                         help='Backend for --preview (default: dshow, or --backend if a single one was given)')
    args = parser.parse_args()

    comtypes.CoInitialize()

    print(f'Looking for VID_{args.vid.upper()}&PID_{args.pid.upper()} via DirectShow ...')
    devices = enumerate_devices()
    if not devices:
        print('No DirectShow video input devices found at all.')
        sys.exit(1)

    print(f'Found {len(devices)} video input device(s):')
    for d in devices:
        marker = ''
        print(f"  [{d['index']}] {d['name']}  ({d['device_path'] or 'no device path'})")

    dev = find_device(devices, args.vid, args.pid)
    if dev is None:
        print(f'\nNo device matching VID_{args.vid.upper()}&PID_{args.pid.upper()} found.')
        sys.exit(1)

    print(f"\nMatched device: [{dev['index']}] {dev['name']}")

    vpa, cam = bind_controls(dev['moniker'])
    control_results = probe_all_controls(vpa, cam, args.dry_run)
    print_prop_table('IAMVideoProcAmp (brightness/contrast/gain/...)', control_results['VideoProcAmp'])
    print_prop_table('IAMCameraControl (exposure/focus/pan-tilt-zoom/...)', control_results['CameraControl'])

    video_modes = {}
    if not args.skip_video:
        backend_names = ['dshow', 'msmf'] if args.backend == 'both' else [args.backend]
        for backend_name in backend_names:
            video_modes[backend_name] = probe_video_modes(dev['index'], backend_name)
            print_video_modes(video_modes[backend_name])

    if args.json:
        report = {
            'device': {'name': dev['name'], 'device_path': dev['device_path'], 'index': dev['index']},
            'controls': control_results,
            'video_modes': video_modes,
        }
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2)
        print(f'\nFull report written to {args.json}')

    if args.preview:
        preview_backend = args.preview_backend or (args.backend if args.backend != 'both' else 'dshow')
        show_live_feed(dev['index'], preview_backend, PREVIEW_WIDTH, PREVIEW_HEIGHT)


if __name__ == '__main__':
    main()
