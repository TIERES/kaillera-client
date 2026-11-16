"""O aviso de ISO diferente com o RetroArch TIERES de verdade (o id calculado
pelo kailleraContentIdForGame do fork, a partir da ISO do Historico).

Um fake_room.exe cria a sala com outra ISO (--iso-host); o retroarch.exe da
pasta --ra (uma COPIA de instalacao, com o DLL novo, um content_history.lpl
com o jogo e um n02.ini apontando para o servidor local) abre o Kaillera pelo
menu, conecta e entra na sala. O host aperta Start: a popup dele lista o id que
o RetroArch mandou. Este script opera as janelas do RetroArch de fora (o
WM_NOTIFY e a selecao da lista vao pela memoria do outro processo).

    python run_real_retroarch.py --ra <pasta> --saida <pasta> --elk <pasta> --java <java.exe>
"""
import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

from run_room_test import AQUI, DLL, ini, subir_servidor

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.GetDlgItem.restype = wintypes.HWND
user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.VirtualAllocEx.restype = ctypes.c_void_p
kernel32.VirtualAllocEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
kernel32.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
kernel32.VirtualFreeEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD]

WM_COMMAND, WM_NOTIFY, WM_GETTEXT, WM_CLOSE = 0x111, 0x4E, 0x0D, 0x10
LVM_GETITEMCOUNT, LVM_SETITEMSTATE = 0x1004, 0x102B
BTN_CONNECT, LV_ULIST, IDC_CREATE, LV_GLIST, LV_GULIST, RE_GCHAT = 1036, 1029, 1006, 1046, 1054, 1052
MENU_KAILLERA = 40028


def janelas(pid):
    out = []

    def cb(h, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            cls = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(h, cls, 64)
            out.append((h, cls.value, texto(h)))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return out


def texto(h, n=32768):
    buf = ctypes.create_unicode_buffer(n)
    user32.SendMessageW(h, WM_GETTEXT, n, ctypes.addressof(buf))
    return buf.value


def esperar(f, segundos, passo=0.2):
    fim = time.time() + segundos
    while time.time() < fim:
        r = f()
        if r:
            return r
        time.sleep(passo)
    return None


def dialogo(pid, ctrl, outro, extra=None):
    for h, cls, _ in janelas(pid):
        if cls == "#32770" and user32.GetDlgItem(h, ctrl) and user32.GetDlgItem(h, outro) \
                and (extra is None or user32.GetDlgItem(h, extra)):
            return h
    return None


def remoto(pid, hwnd, msg, wparam, dados):
    """SendMessage com um ponteiro para `dados` dentro do outro processo."""
    proc = kernel32.OpenProcess(0x0008 | 0x0010 | 0x0020, False, pid)
    mem = kernel32.VirtualAllocEx(proc, None, len(dados), 0x3000, 0x04)
    kernel32.WriteProcessMemory(proc, mem, dados, len(dados), None)
    r = user32.SendMessageW(hwnd, msg, wparam, mem)
    kernel32.VirtualFreeEx(proc, mem, 0, 0x8000)
    kernel32.CloseHandle(proc)
    return r


def selecionar(pid, lv, linha):
    item = bytearray(88)
    item[12:16] = (0x3).to_bytes(4, "little")   # state = LVIS_SELECTED | LVIS_FOCUSED
    item[16:20] = (0x3).to_bytes(4, "little")   # stateMask
    remoto(pid, lv, LVM_SETITEMSTATE, linha, bytes(item))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ra", required=True)
    ap.add_argument("--saida", required=True)
    ap.add_argument("--elk", required=True)
    ap.add_argument("--java", required=True)
    ap.add_argument("--iso-host", default="73E91CFE:1C473F60")
    ap.add_argument("--jogadores", default="2", help="o host so aperta Start com esse numero na sala (3 = nunca)")
    ap.add_argument("--espera", type=int, default=45, help="segundos esperando a popup depois de entrar")
    a = ap.parse_args()

    base = os.path.join(a.saida, "real")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    servidor = subir_servidor(a.elk, a.java, os.path.join(base, "emulinker.log"))
    host = os.path.join(base, "hostFake")
    os.makedirs(host)
    shutil.copy(os.path.join(AQUI, "fake_room.exe"), host)
    shutil.copy(DLL, host)
    with open(os.path.join(host, "n02.ini"), "w", newline="") as f:
        f.write(ini("hostFake", 27888))
    env = dict(os.environ, FAKE_ROLE="host", FAKE_ISO=a.iso_host, FAKE_DELAY_MS="500", FAKE_PLAYERS=a.jogadores,
               FAKE_SETTLE_MS="8000", FAKE_ANSWERS="nao", FAKE_RUN_MS="75000")
    fake = subprocess.Popen([os.path.join(host, "fake_room.exe")], cwd=host, env=env)
    time.sleep(6)

    ra = subprocess.Popen([os.path.join(a.ra, "retroarch.exe")], cwd=a.ra)
    log = []

    def anota(s):
        log.append("%6.1f %s" % (time.time() - t0, s))
    t0 = time.time()
    try:
        janela = esperar(lambda: next((h for h, c, _ in janelas(ra.pid) if c == "RetroArch"), None), 30)
        anota("janela do RetroArch: %s" % bool(janela))
        time.sleep(3)
        user32.PostMessageW(janela, WM_COMMAND, MENU_KAILLERA, 0)
        ss = esperar(lambda: dialogo(ra.pid, BTN_CONNECT, LV_ULIST), 30)
        anota("lista de servidores: %s" % bool(ss))
        selecionar(ra.pid, user32.GetDlgItem(ss, LV_ULIST), 0)
        time.sleep(0.5)
        user32.PostMessageW(ss, WM_COMMAND, BTN_CONNECT, user32.GetDlgItem(ss, BTN_CONNECT))
        sdlg = esperar(lambda: dialogo(ra.pid, IDC_CREATE, RE_GCHAT, LV_GLIST), 30)
        glist = user32.GetDlgItem(sdlg, LV_GLIST)
        esperar(lambda: user32.SendMessageW(glist, LVM_GETITEMCOUNT, 0, 0) > 0, 30)
        time.sleep(1)
        selecionar(ra.pid, glist, 0)
        anota("entrando na sala")
        # A double click on the room's row (the list sends NM_DBLCLK itself;
        # a WM_NOTIFY posted from here is refused). The row's rectangle comes
        # through the other process's memory.
        rect = bytearray(16)
        rect[0:4] = (0).to_bytes(4, "little")  # LVIR_BOUNDS
        proc = kernel32.OpenProcess(0x0008 | 0x0010 | 0x0020, False, ra.pid)
        mem = kernel32.VirtualAllocEx(proc, None, 16, 0x3000, 0x04)
        kernel32.WriteProcessMemory(proc, mem, bytes(rect), 16, None)
        user32.SendMessageW(glist, 0x100E, 0, mem)  # LVM_GETITEMRECT
        out = ctypes.create_string_buffer(16)
        kernel32.ReadProcessMemory(proc, ctypes.c_void_p(mem), out, 16, None)
        kernel32.VirtualFreeEx(proc, mem, 0, 0x8000)
        left, top, right, bottom = [int.from_bytes(out.raw[i:i + 4], "little", signed=True) for i in (0, 4, 8, 12)]
        x, y = left + 10, (top + bottom) // 2
        pos = (y << 16) | x
        for msg in (0x201, 0x202, 0x203, 0x202):  # down, up, double click, up
            user32.PostMessageW(glist, msg, 1 if msg in (0x201, 0x203) else 0, pos)
            time.sleep(0.05)
        anota("duplo clique na sala em (%d, %d)" % (x, y))
        caixa = esperar(lambda: next((h for h, c, t in janelas(ra.pid) if c == "#32770" and t == "Error"), None), 5)
        if caixa:
            anota("caixa do DLL: " + texto(user32.GetDlgItem(caixa, 0xFFFF)).replace("\n", " ") + " -> Sim")
            user32.PostMessageW(caixa, WM_COMMAND, 6, 0)  # IDYES
        # The host presses Start after the room has 2 players; our popup comes.
        fim = time.time() + a.espera
        vistas = 0
        while time.time() < fim:
            for h, cls, titulo in janelas(ra.pid):
                if titulo == "Kaillera - ISO diferente":
                    vistas += 1
                    anota("popup no RetroArch:\n" + texto(user32.GetDlgItem(h, 0xFFFF)))
                    user32.PostMessageW(h, WM_COMMAND, 2, 0)  # IDCANCEL = the OK of a MB_OK box
                    time.sleep(1)
            if vistas and fake.poll() is not None:
                break
            time.sleep(0.3)
        anota("chat da sala no RetroArch:\n" + texto(user32.GetDlgItem(sdlg, RE_GCHAT)))
        user32.PostMessageW(sdlg, WM_CLOSE, 0, 0)
        time.sleep(2)
    finally:
        ra.kill()
        try:
            fake.wait(timeout=60)
        except subprocess.TimeoutExpired:
            fake.kill()
        servidor.kill()
    print("\n".join(log))
    print("=" * 20, "hostFake")
    with open(os.path.join(host, "fake.log"), encoding="utf-8", errors="replace") as f:
        print(f.read())


if __name__ == "__main__":
    sys.exit(main())
