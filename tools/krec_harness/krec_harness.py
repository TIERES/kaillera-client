# Headless libretro harness for .krec replays / Watch Live streams.
#
# Drives pcsx_rearmed_libretro.dll exactly like retroarch-k3's Kaillera path
# does (runloop.c core_run() + input_driver.c): one .krec input record per
# retro_run(), 12 bytes per player (int16[6]: command, buttons bitmask, 4
# analog axes), an empty record = neutral input for everyone, core frame f
# (1-based, like current_core_frame) consumes record f-1, every port set to
# RETRO_DEVICE_JOYPAD, and the core options kaillera_sync.c forces on top of
# the package's PCSX-ReARMed.opt. The RAM digest is the anti-desync
# detector's own (FNV-1a over 64-bit words, 1/300 of system RAM per frame,
# one digest per 300-frame window), so it can be checked against the
# players' in-band "[SYNC] ... d=<window>:<hash>" lines.
#
# See README.md for the modes and what each one proves.
import argparse
import ctypes as C
import json
import os
import struct
import sys
import time
import zlib

from krec import Krec

DEFAULT_RA = r"D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.0.3"
DEFAULT_GAME = r"C:\Users\walti\Downloads\Telegram Desktop\MasterLeague12TemporadaLWS2F.bin"

# Keep in sync with ksync_forced_options[] in retroarch-k3's
# kaillera/kaillera_sync.c (memcard1/2 follow the replay's "Sem M. Card").
FORCED = {
    "pcsx_rearmed_memcard1": "none",
    "pcsx_rearmed_memcard2": "none",
    "pcsx_rearmed_bios": "auto",
    "pcsx_rearmed_show_bios_bootlogo": "disabled",
    "pcsx_rearmed_region": "auto",
    "pcsx_rearmed_drc": "enabled",
    "pcsx_rearmed_psxclock": "57",
    "pcsx_rearmed_nostalls": "disabled",
    "pcsx_rearmed_icache_emulation": "enabled",
    "pcsx_rearmed_exception_emulation": "disabled",
    "pcsx_rearmed_nosmccheck": "disabled",
    "pcsx_rearmed_gteregsunneeded": "disabled",
    "pcsx_rearmed_nogteflags": "disabled",
    "pcsx_rearmed_gpu_slow_llists": "auto",
    "pcsx_rearmed_gpu_thread_rendering": "auto",
    "pcsx_rearmed_cd_turbo": "disabled",
    "pcsx_rearmed_noxadecoding": "enabled",
    "pcsx_rearmed_nocdaudio": "enabled",
    "pcsx_rearmed_spu_reverb": "enabled",
    "pcsx_rearmed_frameskip_type": "disabled",
    "pcsx_rearmed_multitap": "disabled",
    "pcsx_rearmed_multitap1": "disabled",
    "pcsx_rearmed_multitap2": "disabled",
    "pcsx_rearmed_analog_axis_modifier": "square",
    "pcsx_rearmed_input_sensitivity": "1.00",
}

DIGEST_FRAMES = 300
MASK64 = (1 << 64) - 1
TRACE_FROM = 0  # --trace-from: log every frame from here on (to pin down a crash)


def fmt_time(frame):
    s = max(0, frame) // 60
    return "%d:%02d" % (s // 60, s % 60)


###############################################################################
# libretro core
###############################################################################

class retro_variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


class retro_game_info(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]


class retro_log_callback(C.Structure):
    _fields_ = [("log", C.c_void_p)]


ENV_T = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO_T = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO_T = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIOB_T = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)
POLL_T = C.CFUNCTYPE(None)
STATE_T = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
LOG_T = C.CFUNCTYPE(None, C.c_int, C.c_char_p)  # variadic core-side; the Win64 ABI lets us read the fixed args


class Core:
    """One core per process - the DLL keeps global state, so a second
    instance means a second process (see the ref/spectator modes)."""

    def __init__(self, ra, game, overrides=None, verbose=False, workdir=".", core_path=None):
        self.opts = {}
        opt_file = os.path.join(ra, "config", "PCSX-ReARMed", "PCSX-ReARMed.opt")
        if os.path.exists(opt_file):
            for line in open(opt_file, encoding="latin-1"):
                if "=" in line:
                    k, v = line.split("=", 1)
                    self.opts[k.strip()] = v.strip().strip('"')
        self.opts.update(FORCED)
        if overrides:
            self.opts.update(overrides)
        self.sysdir = os.path.join(ra, "system")
        self.savedir = os.path.join(workdir, "harness_saves")
        os.makedirs(self.savedir, exist_ok=True)
        self.verbose = verbose
        self.av = 3
        self.joy = [[0] * 6 for _ in range(8)]
        self._strs = {}
        self.lib = C.CDLL(os.path.abspath(core_path) if core_path else os.path.join(ra, "cores", "pcsx_rearmed_libretro.dll"))
        L = self.lib
        L.retro_serialize_size.restype = C.c_size_t
        L.retro_serialize.argtypes = [C.c_void_p, C.c_size_t]
        L.retro_serialize.restype = C.c_bool
        L.retro_unserialize.argtypes = [C.c_void_p, C.c_size_t]
        L.retro_unserialize.restype = C.c_bool
        L.retro_get_memory_data.argtypes = [C.c_uint]
        L.retro_get_memory_data.restype = C.c_void_p
        L.retro_get_memory_size.argtypes = [C.c_uint]
        L.retro_get_memory_size.restype = C.c_size_t
        L.retro_load_game.argtypes = [C.POINTER(retro_game_info)]
        L.retro_load_game.restype = C.c_bool
        L.retro_set_controller_port_device.argtypes = [C.c_uint, C.c_uint]

        self._cbs = [ENV_T(self._env), VIDEO_T(lambda d, w, h, p: None), AUDIO_T(lambda l, r: None),
                     AUDIOB_T(lambda d, n: n), POLL_T(lambda: None), STATE_T(self._state), LOG_T(self._log)]
        env, video, audio, audiob, poll, state, self.log_cb = self._cbs
        L.retro_set_environment(env)
        L.retro_set_video_refresh(video)
        L.retro_set_audio_sample(audio)
        L.retro_set_audio_sample_batch(audiob)
        L.retro_set_input_poll(poll)
        L.retro_set_input_state(state)
        L.retro_init()
        gi = retro_game_info(game.encode("mbcs"), None, 0, None)
        if not L.retro_load_game(C.byref(gi)):
            raise SystemExit("retro_load_game falhou para %s" % game)
        for p in range(8):
            L.retro_set_controller_port_device(p, 1)  # RETRO_DEVICE_JOYPAD, like the fork forces
        self.ram_ptr = L.retro_get_memory_data(2)
        self.ram_size = L.retro_get_memory_size(2)
        self.ss = L.retro_serialize_size()

    def _cstr(self, s):
        b = self._strs.get(s)
        if b is None:
            b = C.create_string_buffer(s.encode("latin-1"))
            self._strs[s] = b
        return b

    def _log(self, level, fmt):
        if self.verbose or level >= 3:
            try:
                sys.stderr.write("[core] %s" % fmt.decode("latin-1", "replace"))
            except Exception:
                pass

    def _env(self, cmd, data):
        cmd &= 0xFFFF
        if cmd in (9, 31):  # GET_SYSTEM_DIRECTORY / GET_SAVE_DIRECTORY
            s = self._cstr(self.sysdir if cmd == 9 else self.savedir)
            C.cast(data, C.POINTER(C.c_void_p))[0] = C.cast(s, C.c_void_p).value
            return True
        if cmd == 15:  # GET_VARIABLE
            v = C.cast(data, C.POINTER(retro_variable))
            key = v[0].key.decode()
            if key in self.opts:
                v[0].value = C.cast(self._cstr(self.opts[key]), C.c_char_p)
                return True
            return False
        if cmd == 16:  # SET_VARIABLES - defaults for keys the .opt lacks
            p = C.cast(data, C.POINTER(retro_variable))
            i = 0
            while p[i].key:
                key = p[i].key.decode()
                desc = p[i].value.decode("latin-1")
                if key not in self.opts and ";" in desc:
                    self.opts[key] = desc.split(";", 1)[1].strip().split("|")[0]
                i += 1
            return True
        if cmd == 52:  # GET_CORE_OPTIONS_VERSION -> 0 = legacy SET_VARIABLES
            C.cast(data, C.POINTER(C.c_uint))[0] = 0
            return True
        if cmd == 17:  # GET_VARIABLE_UPDATE
            C.cast(data, C.POINTER(C.c_bool))[0] = False
            return True
        if cmd in (10, 11, 13, 32, 35, 36, 37, 51, 70):
            return True
        if cmd == 27:  # GET_LOG_INTERFACE
            C.cast(data, C.POINTER(retro_log_callback))[0].log = C.cast(self.log_cb, C.c_void_p).value
            return True
        if cmd == 3:  # GET_CAN_DUPE
            C.cast(data, C.POINTER(C.c_bool))[0] = True
            return True
        if cmd == 47:  # GET_AUDIO_VIDEO_ENABLE
            if data:
                C.cast(data, C.POINTER(C.c_int))[0] = self.av
            return True
        if cmd == 59:  # GET_INPUT_MAX_USERS
            C.cast(data, C.POINTER(C.c_uint))[0] = 8
            return True
        return False

    def _state(self, port, device, index, id_):
        if port >= 8:
            return 0
        device &= 0xFF
        j = self.joy[port]
        if device == 1:  # JOYPAD
            if id_ == 256:  # RETRO_DEVICE_ID_JOYPAD_MASK
                return j[1]
            return (j[1] >> (id_ & 0xF)) & 1
        if device == 5:  # ANALOG
            return j[2 + (id_ & 1)] if index == 0 else j[4 + (id_ & 1)]
        return 0

    # Kaillera slot whose input each core port reads with a PSX Multitap -
    # retroarch-k3's kailleraSyncSlotForPort(): Winning Eleven numbers its
    # controllers 1A, 2, 1B, 1C, 1D, so player 2 goes to the port-2 pad.
    MULTITAP_SLOT_OF_PORT = (0, 2, 3, 4, 1, 5, 6, 7)
    multitap = False

    def set_input(self, rec):
        if not rec:  # k_len == 0 in core_run(): neutral input for everyone
            for p in range(8):
                self.joy[p] = [0] * 6
            return
        slots = [[0] * 6 for _ in range(8)]
        for p in range(min(len(rec) // 12, 8)):
            slots[p] = list(struct.unpack_from("<6h", rec, p * 12))
        for port in range(8):
            self.joy[port] = slots[self.MULTITAP_SLOT_OF_PORT[port] if self.multitap else port]

    def run(self):
        self.lib.retro_run()

    def ram(self):
        return C.string_at(self.ram_ptr, self.ram_size)

    def serialize(self):
        buf = C.create_string_buffer(self.ss)
        if not self.lib.retro_serialize(buf, self.ss):
            raise SystemExit("retro_serialize falhou")
        return buf.raw

    def unserialize(self, blob):
        blob = blob[:self.ss]  # a Watch Live blob carries the fork's player-map trailer after the state
        buf = C.create_string_buffer(blob, len(blob))
        if not self.lib.retro_unserialize(buf, len(blob)):
            raise SystemExit("retro_unserialize falhou")


class Digester:
    """kailleraSyncAfterFrame()'s RAM digest. `valid_from` = first window
    fully hashed (a window in progress when a state was loaded is skipped)."""

    def __init__(self, core, valid_from=0):
        self.core = core
        self.slice = (core.ram_size + DIGEST_FRAMES - 1) // DIGEST_FRAMES
        self.window = -1
        self.acc = 0
        self.valid_from = valid_from
        self.done = {}

    def after_frame(self, frame):
        w, idx = divmod(frame, DIGEST_FRAMES)
        if w != self.window:
            self.window, self.acc = w, 0xCBF29CE484222325
        start = idx * self.slice
        if start < self.core.ram_size:
            b = C.string_at(self.core.ram_ptr + start, min(self.slice, self.core.ram_size - start))
            h = self.acc
            n8 = len(b) // 8
            for (x,) in struct.iter_unpack("<Q", b[:n8 * 8]):
                h = ((h ^ x) * 0x100000001B3) & MASK64
            for x in b[n8 * 8:]:
                h = ((h ^ x) * 0x100000001B3) & MASK64
            self.acc = h
        if idx == DIGEST_FRAMES - 1 and w >= self.valid_from:
            self.done[w] = "%016X" % self.acc
            return w
        return None


def open_core(a, k=None):
    overrides = dict(kv.split("=", 1) for kv in a.opt)
    if k is not None:
        mc = k.memcard_marker()
        if mc is False:
            print("ATENCAO: partida COM memory card - o harness roda sem cartao, pode divergir quando o jogo ler o cartao.", file=sys.stderr)
    multitap = k is not None and k.multitap_marker() and k.numplayers >= 3
    if multitap:  # what the fork forces (kaillera_sync.c, ksync_forced_value())
        both = k.numplayers > 4
        for key, value in (("pcsx_rearmed_multitap", "ports 1 and 2" if both else "port 1"),
                           ("pcsx_rearmed_multitap1", "enabled"),
                           ("pcsx_rearmed_multitap2", "enabled" if both else "disabled")):
            overrides.setdefault(key, value)
        print("MultiTap: %s, jogador 2 no controle da porta 2 (ordem do kailleraSyncSlotForPort)." % (
            "portas 1 e 2" if both else "porta 1"), file=sys.stderr)
    core = Core(a.ra, a.game, overrides, a.verbose, a.workdir, a.core)
    core.multitap = multitap
    core.av = a.av
    return core


###############################################################################
# audit / golive: replay against the players' own in-band RAM digests
###############################################################################

def check_windows(k, core, first_frame, last_frame, valid_from, label, max_mismatches):
    """Plays core frames first_frame..last_frame (record f-1 each) and
    compares every completed window with the players' epoch-0 digests."""
    recorded = k.digests()
    rollback = k.first_rollback_frame()
    if rollback is not None and rollback < last_frame:
        print("  rollback (BACKSPACE) na partida por volta de %s - o harness nao reproduz, auditoria vai ate ali." % fmt_time(rollback))
        last_frame = min(last_frame, rollback)
    dg = Digester(core, valid_from)
    stats = {"ok": 0, "bad": 0, "players_disagree": 0, "unrecorded": 0}
    first_bad = None
    t0 = time.time()
    for f in range(first_frame, last_frame + 1):
        core.set_input(k.frames[f - 1])
        core.run()
        # The core can end the whole process on its own (e.g. Lightrec's
        # "Exiting at cycle ..." after running out of code space) - this is
        # the only trace of where that happened.
        if f % 1000 == 0 or (TRACE_FROM and f >= TRACE_FROM):
            sys.stderr.write("[frame %d ~%s]\n" % (f, fmt_time(f)))
            sys.stderr.flush()
        w = dg.after_frame(f)
        if w is None:
            continue
        mine = dg.done[w]
        theirs = recorded.get((0, w))
        if not theirs:
            stats["unrecorded"] += 1
            continue
        if len(set(theirs.values())) > 1:
            stats["players_disagree"] += 1
        if mine in theirs.values():
            stats["ok"] += 1
        else:
            stats["bad"] += 1
            if first_bad is None:
                first_bad = w
                print("  DIVERGE na janela %d (frames %d-%d, ~%s): harness %s x jogadores %s" % (
                    w, w * DIGEST_FRAMES, w * DIGEST_FRAMES + DIGEST_FRAMES - 1, fmt_time(w * DIGEST_FRAMES), mine, theirs))
            if stats["bad"] >= max_mismatches:
                break
    secs = time.time() - t0
    verdict = "SEM DESYNC" if stats["bad"] == 0 else "DESYNC a partir da janela %d (~%s)" % (first_bad, fmt_time(first_bad * DIGEST_FRAMES))
    print("  %s: %d janelas iguais, %d diferentes, %d sem digest gravado, %d em que os proprios jogadores discordam (%.0fs)" % (
        label, stats["ok"], stats["bad"], stats["unrecorded"], stats["players_disagree"], secs))
    print("  RESULTADO: %s" % verdict)
    return stats, first_bad


def cmd_audit(a):
    k = Krec(a.krec)
    print("Auditoria de %s (%d frames, ~%s)" % (os.path.basename(a.krec), len(k.frames), fmt_time(len(k.frames))))
    if k.truncated is not None:
        print("  ATENCAO: gravacao corrompida/truncada no byte %d - auditoria so ate ali." % k.truncated)
    cmds = k.command_frames()
    if cmds:
        print("  %d frames com comando no input (swap/save/load/reset/rollback) - primeiro no frame %d (codigo 0x%X)" % (len(cmds), cmds[0][0] + 1, cmds[0][2]))
    core = open_core(a, k)
    last = len(k.frames) if not a.frames else min(len(k.frames), a.frames)
    check_windows(k, core, 1, last, 0, "Do frame 1 (como 'Acompanhar ao vivo!' desde o inicio)", a.max_mismatches)


def parse_dump_name(path):
    # <role>_<session>_f<frame>_o<offset>.state - see n02_watch.cpp / n02_stream.cpp
    base = os.path.basename(path)
    frame = offset = None
    for part in base.rsplit(".", 1)[0].split("_"):
        if part.startswith("f") and part[1:].isdigit():
            frame = int(part[1:])
        elif part.startswith("o") and part[1:].isdigit():
            offset = int(part[1:])
    return frame, offset


def cmd_golive(a):
    k = Krec(a.krec)
    frame, offset = parse_dump_name(a.state)
    frame = a.frame if a.frame is not None else frame
    offset = a.offset if a.offset is not None else offset
    if frame is None or offset is None:
        raise SystemExit("informe --frame e --offset (ou use o nome do dump: ..._f<frame>_o<offset>.state)")
    print("'Ir ao vivo!' com %s: frame do host %d (~%s), offset %d" % (os.path.basename(a.state), frame, fmt_time(frame), offset))
    before, boundary = k.frames_before_offset(offset)
    if not boundary:
        print("  ERRO: offset %d NAO cai no inicio de um registro do stream - state e stream desalinhados (lote perdido/duplicado?)" % offset)
    if before != frame - 1:
        print("  ERRO: antes do offset ha %d frames, esperado %d (frame %d do host) - input sairia deslocado %+d frames" % (before, frame - 1, frame, before - (frame - 1)))
    else:
        print("  pareamento state/offset OK: o proximo input do stream e o do frame %d" % frame)
    core = open_core(a, k)
    core.unserialize(open(a.state, "rb").read())
    valid_from = (frame + DIGEST_FRAMES - 1) // DIGEST_FRAMES  # first window that starts at/after the load
    last = min(len(k.frames), frame - 1 + (a.frames or 20 * DIGEST_FRAMES))
    check_windows(k, core, before + 1, last, valid_from, "Depois do load", a.max_mismatches)


###############################################################################
# compare: host's local .krec vs the server's copy of the same stream
###############################################################################

def cmd_compare(a):
    A, B = Krec(a.krec), Krec(a.other)
    print("A = %s (%d frames)\nB = %s (%d frames)" % (a.krec, len(A.frames), a.other, len(B.frames)))
    ra, rb = A.raw[A.header_size:], B.raw[B.header_size:]
    n = min(len(ra), len(rb))
    i = next((x for x in range(0, n, 4096) if ra[x:x + 4096] != rb[x:x + 4096]), None)
    if i is not None:
        while ra[i] == rb[i]:
            i += 1
    if i is None:
        if len(ra) == len(rb):
            print("IDENTICOS depois do cabecalho (%d bytes)." % len(ra))
        else:
            longer = "A" if len(ra) > len(rb) else "B"
            print("Iguais ate o fim do menor; %s tem %d bytes a mais (fim da partida/gravacao)." % (longer, abs(len(ra) - len(rb))))
        return
    pos = i + A.header_size
    fa, _ = A.frames_before_offset(pos)
    print("PRIMEIRA DIFERENCA no byte %d do stream (depois do frame %d, ~%s)" % (pos, fa, fmt_time(fa)))
    # B missing bytes: what B has at i shows up later in A; B has extra
    # (duplicated) bytes: what A has at i shows up later in B.
    j = ra.find(rb[i:i + 8192], i + 1, i + 262144)
    if j > i:
        print("  B tem %d bytes FALTANDO a partir dali (lote perdido) - realinha depois disso" % (j - i))
        return
    j = rb.find(ra[i:i + 8192], i + 1, i + 262144)
    if j > i:
        print("  B tem %d bytes A MAIS a partir dali (lote duplicado) - realinha depois disso" % (j - i))
        return
    print("  sem realinhamento simples em 256KB (conteudo diferente, nao so deslocado)")


###############################################################################
# determinism experiments (one process per run - see README)
###############################################################################

def cmd_ref(a):
    k = Krec(a.krec)
    core = open_core(a, k)
    res = {"F": a.F, "hash": {}}
    for f in range(1, a.F):
        core.set_input(k.frames[f - 1])
        core.run()
    blob = core.serialize()
    if a.mode == "roundtrip":
        core.unserialize(blob)
    else:
        open(a.state, "wb").write(blob)
    run_after(a, k, core, res)


def cmd_spectator(a):
    k = Krec(a.krec)
    core = open_core(a, k)
    hist = Krec(a.hist_krec).frames if a.hist_krec else k.frames
    for f in range(1, a.G + 1):
        core.set_input(hist[f - 1])
        core.run()
    core.unserialize(open(a.state, "rb").read())
    core.av = a.av_after
    run_after(a, k, core, {"F": a.F, "G": a.G, "hash": {}})


def run_after(a, k, core, res):
    end = min(a.F + a.K, len(k.frames))
    shift = (a.input_from - a.F) if a.input_from else 0  # simulate a refused jump: state of F, input of input_from
    for f in range(a.F, end + 1):
        core.set_input(k.frames[f - 1 + shift])
        core.run()
        res["hash"][f] = zlib.crc32(core.ram())
        if a.pace and f % a.burst == 0:
            time.sleep(a.pace * a.burst)
    json.dump(res, open(a.out, "w"))
    print("ok ->", a.out)


def cmd_diffhash(a):
    ref = json.load(open(a.krec))
    for other in a.others:
        o = json.load(open(other))
        common = sorted(int(f) for f in ref["hash"] if f in o["hash"])
        bad = [f for f in common if ref["hash"][str(f)] != o["hash"][str(f)]]
        if bad:
            print("%-28s DIVERGE no frame %d (%+d do load), %d/%d frames diferentes" % (other, bad[0], bad[0] - ref["F"], len(bad), len(common)))
        else:
            print("%-28s IGUAL em %d frames" % (other, len(common)))


###############################################################################

def main():
    ap = argparse.ArgumentParser(description="Harness libretro para .krec / Watch Live - ver README.md")
    ap.add_argument("mode", choices=["info", "audit", "golive", "compare", "ref", "roundtrip", "spectator", "diffhash"])
    ap.add_argument("krec", help=".krec (em diffhash: o json de referencia)")
    ap.add_argument("others", nargs="*", help="diffhash: jsons a comparar")
    ap.add_argument("--other", help="compare: segundo .krec")
    ap.add_argument("--state", help="golive/spectator: state; ref: onde salvar")
    ap.add_argument("--frame", type=int, help="golive: frame do host (current_core_frame) do state")
    ap.add_argument("--offset", type=int, help="golive: offset do stream do state")
    ap.add_argument("--frames", type=int, default=0, help="audit/golive: limite de frames")
    ap.add_argument("--max-mismatches", type=int, default=3)
    ap.add_argument("--trace-from", type=int, default=0, help="audit/golive: registra cada frame a partir deste (para achar onde o core fecha o processo)")
    ap.add_argument("--F", type=int, default=30000, help="ref/spectator: frame em cujo inicio o state e tirado")
    ap.add_argument("--K", type=int, default=6000, help="ref/spectator: frames depois de F")
    ap.add_argument("--G", type=int, default=0, help="spectator: frames de historico proprio antes do load")
    ap.add_argument("--hist-krec", help="spectator: historico vindo de outro .krec")
    ap.add_argument("--input-from", type=int, default=0, help="spectator: simula salto recusado (input desse frame)")
    ap.add_argument("--pace", type=float, default=0.0)
    ap.add_argument("--burst", type=int, default=1)
    ap.add_argument("--av", type=int, default=3, help="GET_AUDIO_VIDEO_ENABLE (bit0 video, bit1 audio)")
    ap.add_argument("--av-after", type=int, default=3)
    ap.add_argument("--out", default="run.json")
    ap.add_argument("--opt", action="append", default=[], help="chave=valor sobre as opcoes do core")
    ap.add_argument("--ra", default=DEFAULT_RA, help="pasta do RetroArch (core, system, .opt)")
    ap.add_argument("--game", default=DEFAULT_GAME, help="arquivo do jogo")
    ap.add_argument("--core", help="outro pcsx_rearmed_libretro.dll no lugar do da pasta do RetroArch")
    ap.add_argument("--workdir", default=".")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    global TRACE_FROM
    TRACE_FROM = a.trace_from

    if a.mode == "info":
        k = Krec(a.krec)
        import krec as _k
        _k.summary(a.krec)
        core = open_core(a, k)
        print("  core: state %d bytes, RAM %d bytes" % (core.ss, core.ram_size))
    elif a.mode == "audit":
        cmd_audit(a)
    elif a.mode == "golive":
        cmd_golive(a)
    elif a.mode == "compare":
        cmd_compare(a)
    elif a.mode in ("ref", "roundtrip"):
        cmd_ref(a)
    elif a.mode == "spectator":
        cmd_spectator(a)
    elif a.mode == "diffhash":
        cmd_diffhash(a)


if __name__ == "__main__":
    main()
