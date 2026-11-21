# SPDX-License-Identifier: Apache-2.0
# R19 application-owned Linux process guardian, carried by WSL/SSH argv without a project script.
# JSON control stdin is distinct from child stdin. Output and final cleanup are acknowledged facts.
import ctypes
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time

VERSION = 1
MAX_LINE = 1024 * 1024
LEASE_SECONDS = 10
state = {"stop": False, "lost": False, "error": "", "last": time.monotonic()}
output_lock = threading.Lock()
token = ""
child = None


def emit(event, **fields):
    message = {"version": VERSION, "token": token, "event": event}
    message.update(fields)
    try:
        with output_lock:
            sys.stdout.write(json.dumps(message, ensure_ascii=True, separators=(",", ":")) + "\n")
            sys.stdout.flush()
    except (BrokenPipeError, OSError):
        state["lost"] = True
        state["stop"] = True


def descendants():
    parents = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open("/proc/" + name + "/status", encoding="ascii", errors="replace") as handle:
                for line in handle:
                    if line.startswith("PPid:"):
                        parents[int(name)] = int(line.split()[1])
                        break
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    owned = {os.getpid()}
    changed = True
    while changed:
        changed = False
        for pid, parent in parents.items():
            if parent in owned and pid not in owned:
                owned.add(pid)
                changed = True
    owned.discard(os.getpid())
    return owned


def identity(pid):
    with open("/proc/" + str(pid) + "/stat", encoding="ascii", errors="replace") as handle:
        text = handle.read()
    fields = text[text.rfind(")") + 2:].split()
    return int(fields[1]), int(fields[19])  # Parent and Linux start-time ticks.


def bound_pid(fd):
    with open("/proc/self/fdinfo/" + str(fd), encoding="ascii") as handle:
        for line in handle:
            if line.startswith("Pid:"):
                return int(line.split()[1])
    return -1


def owned_pidfd(pid):
    # Bind every current ancestry edge before accepting ownership. The final signal uses the
    # pinned pidfd, never a recycled integer PID. Recheck all edges after opening the chain.
    opened = []
    observed = {}
    current = pid
    target_fd = None
    try:
        while current != os.getpid():
            if current <= 0 or current in observed or len(observed) >= 512:
                return None
            fd = os.pidfd_open(current, 0)
            opened.append(fd)
            row = identity(current)
            if bound_pid(fd) != current:
                return None
            observed[current] = (fd, row)
            if target_fd is None:
                target_fd = fd
            current = row[0]
        for current, (fd, row) in observed.items():
            if identity(current) != row or bound_pid(fd) != current:
                return None
        opened.remove(target_fd)
        return target_fd
    except OSError:
        return None
    finally:
        for fd in opened:
            os.close(fd)

def cleanup_tree():
    # Linux subreaper ownership retains orphaned/double-forked descendants in this guardian.
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        for pid in descendants():
            fd = owned_pidfd(pid)
            if fd is None:
                continue
            try:
                signal.pidfd_send_signal(fd, signal.SIGKILL, None, 0)
            except ProcessLookupError:
                pass
            except PermissionError:
                return False
            finally:
                os.close(fd)
        if child is not None:
            try:
                child.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                continue
        while True:
            try:
                pid, _ = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return not descendants()
            if pid == 0:
                break
        time.sleep(0.02)
    return False


def stop_signal(_number, _frame):
    state["stop"] = True


def run():
    global token, child
    if sys.platform != "linux":
        raise RuntimeError("remote guardian supports Linux/WSL targets only")
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        raise RuntimeError("Linux pidfd process-identity ownership requires Python >=3.9 and kernel pidfd support")
    probe = os.pidfd_open(os.getpid(), 0)
    try:
        signal.pidfd_send_signal(probe, 0, None, 0)
        if bound_pid(probe) != os.getpid():
            raise RuntimeError("Linux pidfd identity binding is unavailable")
    finally:
        os.close(probe)
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER, Linux prctl(2).
        raise RuntimeError("Linux child-subreaper ownership unavailable")
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, stop_signal)
    line = sys.stdin.buffer.readline(MAX_LINE + 1)
    if len(line) > MAX_LINE or not line.endswith(b"\n"):
        raise RuntimeError("remote launch request is missing or oversized")
    request = json.loads(line)
    token = request.get("token", "")
    argv = request.get("argv")
    cwd = request.get("cwd")
    changes = request.get("environment")
    if (request.get("version") != VERSION or not isinstance(token, str) or not token or
            not isinstance(argv, list) or not argv or len(argv) > 4096 or
            not all(isinstance(arg, str) and "\0" not in arg for arg in argv) or
            not os.path.isabs(argv[0]) or not isinstance(cwd, str) or not os.path.isabs(cwd) or
            "\0" in cwd or not isinstance(changes, list) or not isinstance(request.get("stdin"), bool)):
        raise RuntimeError("malformed remote launch request")
    if not isinstance(request.get("cleanEnvironment", False), bool) or not isinstance(request.get("lowPriority", False), bool):
        raise RuntimeError("malformed remote environment/priority policy")
    environment = {} if request.get("cleanEnvironment", False) else os.environ.copy()
    for change in changes:
        name, value = change.get("name"), change.get("value")
        if (not isinstance(name, str) or not name or "=" in name or "\0" in name or
                (value is not None and (not isinstance(value, str) or "\0" in value))):
            raise RuntimeError("malformed remote environment")
        if value is None:
            environment.pop(name, None)
        else:
            environment[name] = value
    child = subprocess.Popen(argv, cwd=cwd, env=environment, start_new_session=True,
                             stdin=subprocess.PIPE if request["stdin"] else subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, close_fds=True,
                             preexec_fn=(lambda: os.nice(10)) if request.get("lowPriority", False) else None)
    emit("started", pid=child.pid)
    incoming = queue.Queue(maxsize=256)  # At most 1 MiB pending child stdin.

    def input_writer():
        while True:
            item = incoming.get()
            try:
                if item is None:
                    if child.stdin is not None:
                        child.stdin.close()
                    return
                child.stdin.write(item)
                child.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                return

    def control_reader():
        while True:
            try:
                line = sys.stdin.buffer.readline(MAX_LINE + 1)
                if not line:
                    state["lost"] = True
                    state["stop"] = True
                    return
                if len(line) > MAX_LINE or not line.endswith(b"\n"):
                    raise RuntimeError("oversized remote control packet")
                message = json.loads(line)
                if message.get("token") != token or message.get("version") != VERSION:
                    raise RuntimeError("remote control identity mismatch")
                state["last"] = time.monotonic()
                if message.get("stop") is True:
                    state["stop"] = True
                elif message.get("ping") is True:
                    pass
                elif message.get("close") is True:
                    if request["stdin"]:
                        incoming.put_nowait(None)
                elif isinstance(message.get("input"), str) and request["stdin"]:
                    if len(message["input"]) > 8192:
                        raise RuntimeError("remote input packet exceeds 4096 bytes")
                    incoming.put_nowait(bytes.fromhex(message["input"]))
                else:
                    raise RuntimeError("unknown remote control packet")
            except (Exception, queue.Full) as error:
                state["error"] = str(error) or "remote stdin backlog exceeded"
                state["stop"] = True
                return

    def pump(stream, name):
        while True:
            data = stream.read1(8192)
            if not data:
                return
            emit(name, data=data.hex())

    readers = [threading.Thread(target=pump, args=(child.stdout, "stdout"), daemon=True),
               threading.Thread(target=pump, args=(child.stderr, "stderr"), daemon=True)]
    for reader in readers:
        reader.start()
    if request["stdin"]:
        threading.Thread(target=input_writer, daemon=True).start()
    threading.Thread(target=control_reader, daemon=True).start()
    stopped = False
    while child.poll() is None:
        if state["stop"] or time.monotonic() - state["last"] > LEASE_SECONDS:
            stopped = True
            break
        time.sleep(0.05)
    clean = cleanup_tree()
    for reader in readers:
        reader.join(timeout=2)
        if reader.is_alive():
            clean = False
    if state["error"]:
        emit("error", message=state["error"])
    code = 137 if stopped else child.returncode
    if code is None:
        code = 125
        clean = False
    elif code < 0:
        code = 128 - code
    emit("exited", exitCode=code, stopped=stopped, cleanup=clean)
    return 0 if clean else 125


try:
    result = run()
except BaseException as error:
    clean = cleanup_tree() if child is not None else True
    emit("error", message=str(error), cleanup=clean)
    result = 125
# The child tree has been retired and terminal frames flushed. The control thread may still
# block on the IDE-owned stdin; Python finalization would try to lock that buffered stream.
os._exit(result)
