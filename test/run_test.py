#! /usr/bin/env python

import array
import datetime
import io
import json
import os
import os.path
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

cmd = "ocijail/ocijail"

def recv_fds(sock, msglen, maxfds):
    fds = array.array("i")   # Array of ints
    msg, ancdata, flags, addr = sock.recvmsg(msglen, socket.CMSG_LEN(maxfds * fds.itemsize))
    for cmsg_level, cmsg_type, cmsg_data in ancdata:
        if cmsg_level == socket.SOL_SOCKET and cmsg_type == socket.SCM_RIGHTS:
            # Append data, ignoring any truncated integers at the end.
            fds.frombytes(cmsg_data[:len(cmsg_data) - (len(cmsg_data) % fds.itemsize)])
    return msg, list(fds)

class test_run(unittest.TestCase):
    "End-to-end tests for running containers"

    count = 1

    def setUp(self):
        self.container_id = f"ocijail_test_{os.getpid()}_{test_run.count}"
        test_run.count += 1

    def tearDown(self):
        self.delete(False)

    def config(self):
        return {
            "ociVersion": "1.0.2",
            "process": {
                "cwd": "/",
                "env": [
                    "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
                ],
            },
            "root": {
                "path": "/"
            }
        }

    def create(self, bundle_dir, terminal=False, expected_ret=0):
        pid_file = os.path.join(bundle_dir, "pid")
        args = [
            cmd,
            "create",
            "--pid-file", pid_file,
            "--bundle", bundle_dir,
        ]
        if terminal:
            console_socket = os.path.join(bundle_dir, "sock")
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.bind(console_socket)
            sock.listen()
            args += ["--console-socket", console_socket]
        args.append(self.container_id)
        with subprocess.Popen(
            args=args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        ) as proc:
            if terminal:
                conn, _ = sock.accept()
                msg, fds = recv_fds(conn, 512, 10)
                conn.close()
                sock.close()
                proc.wait()
                self.assertEqual(len(fds), 1)
                stdout = io.FileIO(fds[0], "w+")
                stderr = io.FileIO(os.dup(fds[0]), "w+")
            else:
                # Steal the pipes so that they don't get closed during
                # the call to proc.wait()
                stdout = proc.stdout
                stderr = proc.stderr
                proc.stdout = None
                proc.stderr = None
            ret = proc.wait()
            if ret != 0:
                out = stderr.read().decode("utf-8", errors="replace")
                print(f"failed with stderr: {out}")
                self.assertTrue(ret == expected_ret)
                return -1, stdout, stderr
        with open(pid_file, "r") as f:
            pid = int(f.read())
        return pid, stdout, stderr

    def start(self):
        args = [cmd, "start", self.container_id]
        ret = subprocess.run(args=args)
        self.assertTrue(ret.returncode == 0)

    def delete(self, check_returncode=True):
        args = [cmd, "delete", self.container_id]
        ret = subprocess.run(args=args)
        if check_returncode:
            self.assertTrue(ret.returncode == 0)

    def run_with_config(self, c, expected_ret=0):
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            with open(os.path.join("/tmp", "config.json"), "w") as f:
                json.dump(c, f)
            if "terminal" in c["process"]:
                terminal = c["process"]["terminal"]
            else:
                terminal = False
            pid, stdout, stderr = self.create(bundle_dir, terminal=terminal, expected_ret=expected_ret)
            if pid == -1:
                stdout.close()
                stderr.close()
                return 1, "", ""
            self.start()
            out = stdout.read().decode("utf-8")
            err = stderr.read().decode("utf-8")
            stdout.close()
            stderr.close()
            pid2, status = os.waitpid(pid, os.WEXITED)
            self.assertTrue(pid == pid2)
            self.assertEqual(status & 0xff, 0)
            return status >> 8, out, err

    def test_exit_code(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "exit 42"]
        ret, _, _ = self.run_with_config(c)
        self.assertEqual(ret, 42)

    def test_stdout(self):
        c = self.config()
        c["process"]["args"] = ["echo", "Hello", "World"]
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "Hello World\n")

    def test_stdout_tty(self):
        c = self.config()
        c["process"]["args"] = ["echo", "Hello", "World"]
        c["process"]["terminal"] = True
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "Hello World\r\n")

    def test_list_quiet(self):
        # Use a private state root so that only our container is listed.
        with tempfile.TemporaryDirectory() as state_root, \
             tempfile.TemporaryDirectory() as bundle_dir:
            c = self.config()
            c["process"]["args"] = ["true"]
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            root_args = [cmd, "--root", state_root]
            subprocess.run(
                args=root_args + [
                    "create", "--bundle", bundle_dir, self.container_id],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                check=True)
            try:
                for format_args in [[], ["--format", "json"]]:
                    ret = subprocess.run(
                        args=root_args + ["list", "-q"] + format_args,
                        capture_output=True,
                        check=True)
                    self.assertEqual(
                        ret.stdout.decode("utf-8"), f"{self.container_id}\n")
            finally:
                subprocess.run(
                    args=root_args + ["delete", self.container_id],
                    check=True)

    def test_hostname(self):
        c = self.config()
        c["hostname"] = "myhost"
        c["domainname"] = "example.com"
        c["process"]["args"] = ["sh", "-c", "hostname; domainname"]
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out.split(), ["myhost", "example.com"])

    def test_console_size(self):
        c = self.config()
        c["process"]["args"] = ["stty", "size"]
        c["process"]["terminal"] = True
        c["process"]["consoleSize"] = {"height": 42, "width": 123}
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "42 123\r\n")

    def test_scheduler(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "rtprio 2>&1"]
        c["process"]["scheduler"] = {"policy": "SCHED_FIFO", "priority": 10}
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        # rtprio reports on stderr. POSIX priorities count up, rtprio
        # priorities down.
        self.assertEqual(out, "rtprio: realtime priority 21\n")
        self.delete()

        c["process"]["args"] = ["sh", "-c", "idprio 2>&1"]
        c["process"]["scheduler"] = {"policy": "SCHED_IDLE"}
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "idprio: idle priority 31\n")
        self.delete()

        c["process"]["args"] = ["sh", "-c", "ps -o nice= -p $$; exit 0"]
        c["process"]["scheduler"] = {"policy": "SCHED_OTHER", "nice": 7}
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out.strip(), "7")

    def test_chroot(self):
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["sh", "-c", "echo Hello World > /file"]
            c["process"]["env"] = ["PATH=/rescue"]
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            with open(os.path.join(root_dir, "file"), "r") as f:
                self.assertEqual(f.read(), "Hello World\n")

    def test_parent_jail_children_max(self):
        # A parent jail which allows no children has its limit raised to
        # make room for the container. The parent must allow what the
        # container's jail is given.
        parent = f"{self.container_id}_parent"
        subprocess.run(
            ["jail", "-c", f"name={parent}", "path=/", "persist",
             "enforce_statfs=1", "allow.raw_sockets", "children.max=0"],
            check=True)
        try:
            c = self.config()
            c["annotations"] = {"org.freebsd.parentJail": parent}
            c["process"]["args"] = ["sh", "-c", "exit 0"]
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            self.delete()
        finally:
            subprocess.run(["jail", "-r", parent])

    def test_exec_detach_error_logged(self):
        # A detached exec of a missing command reports the error in the
        # runtime's log, where podman reads it from
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "exit 0"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            stdout.close()
            stderr.close()
            process_path = os.path.join(bundle_dir, "process.json")
            with open(process_path, "w") as f:
                json.dump({"args": ["no_such_command"], "cwd": "/"}, f)
            log_path = os.path.join(bundle_dir, "log")
            ret = subprocess.run(
                args=[cmd, "--log", log_path, "--log-format", "json",
                      "exec", "--detach", "--process", process_path,
                      self.container_id],
                stderr=subprocess.DEVNULL)
            self.assertNotEqual(ret.returncode, 0)
            with open(log_path, "r") as f:
                log = f.read()
            self.assertIn("'no_such_command' not found in $PATH", log)
            self.start()
            os.waitpid(pid, os.WEXITED)

    def test_process_user_umask(self):
        c = self.config()
        # If process.user.umask is set, this should be the value of umask inside
        # the container
        c["process"]["user"] = {"uid": 0, "gid": 0}
        c["process"]["user"]["umask"] = 0o022
        c["process"]["args"] = ["umask"]
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out.strip(), "0022")

    def test_propagate_umask(self):
        c = self.config()
        # If process.user.umask is not set, the caller's umask should be
        # propagated to the container
        old_umask = os.umask(0o011)
        c["process"]["args"] = ["umask"]
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out.strip(), "0011")
        os.umask(old_umask)

    def test_state_fields(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "exit 0"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            before = datetime.datetime.now(datetime.timezone.utc)
            pid, stdout, stderr = self.create(bundle_dir)
            after = datetime.datetime.now(datetime.timezone.utc)
            try:
                ret = subprocess.run(
                    args=[cmd, "state", self.container_id],
                    capture_output=True)
                self.assertEqual(ret.returncode, 0, ret.stderr)
                state = json.loads(ret.stdout)
                self.assertEqual(state["rootfs"], "/")
                self.assertEqual(state["owner"], "root")
                self.assertRegex(
                    state["created"],
                    r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z$")
                created = datetime.datetime.strptime(
                    state["created"], "%Y-%m-%dT%H:%M:%S.%fZ").replace(
                        tzinfo=datetime.timezone.utc)
                self.assertTrue(before <= created <= after)
            finally:
                os.kill(pid, 9)
                stdout.close()
                stderr.close()
                os.waitpid(pid, os.WEXITED)

    def run_command(self, bundle_dir, *args, **kwargs):
        return subprocess.run(
            args=[cmd, "run", "--bundle", bundle_dir, *args,
                  self.container_id],
            capture_output=True, **kwargs)

    def container_state(self):
        ret = subprocess.run(
            args=[cmd, "state", self.container_id], capture_output=True)
        if ret.returncode != 0:
            return None
        return json.loads(ret.stdout)

    def test_run(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "echo Hello World; exit 42"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)

            # run waits for the container, returns its exit status and
            # deletes it
            ret = self.run_command(bundle_dir)
            self.assertEqual(ret.returncode, 42, ret.stderr)
            self.assertEqual(ret.stdout, b"Hello World\n")
            self.assertIsNone(self.container_state())

            # With --keep, the stopped container is left behind
            ret = self.run_command(bundle_dir, "--keep")
            self.assertEqual(ret.returncode, 42, ret.stderr)
            self.assertEqual(self.container_state()["status"], "stopped")
            self.delete()

    def test_run_signal(self):
        c = self.config()
        c["process"]["args"] = ["sleep", "60"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)

            # Signals sent to run are forwarded to the container, and a
            # container killed by a signal exits with 128 + the signal
            with subprocess.Popen(
                args=[cmd, "run", "--bundle", bundle_dir, self.container_id],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            ) as proc:
                for _ in range(100):
                    state = self.container_state()
                    if state and state["status"] == "running":
                        break
                    time.sleep(0.1)
                self.assertEqual(state["status"], "running")
                proc.send_signal(15)
                self.assertEqual(proc.wait(timeout=10), 128 + 15)
            self.assertIsNone(self.container_state())

    def test_run_detach(self):
        c = self.config()
        c["process"]["args"] = ["sleep", "60"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid_file = os.path.join(bundle_dir, "pid")
            # The container keeps run's stdout and stderr open, so don't
            # wait for them to close.
            with tempfile.TemporaryFile() as err:
                ret = subprocess.run(
                    args=[cmd, "run", "--bundle", bundle_dir, "--detach",
                          "--pid-file", pid_file, self.container_id],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=err)
                err.seek(0)
                self.assertEqual(ret.returncode, 0, err.read())
            with open(pid_file) as f:
                pid = int(f.read())
            state = self.container_state()
            self.assertEqual(state["status"], "running")
            self.assertEqual(state["pid"], pid)
            os.kill(pid, 9)
            try:
                # Reap the container, which may be our child via the
                # subreaper
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
            ret = subprocess.run(args=[cmd, "delete", "--force", self.container_id])
            self.assertEqual(ret.returncode, 0)

    def test_stale_state(self):
        # Simulate a container whose jail went away, e.g. in a reboot, after
        # which its jid and pid were reused by something else.
        c = self.config()
        c["process"]["args"] = ["sleep", "60"]
        other_jail = self.container_id + "_other"
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            stdout.close()
            stderr.close()
            subprocess.run(args=["jail", "-r", self.container_id], check=True)
            os.waitpid(pid, os.WEXITED)

            subprocess.run(
                args=["jail", "-c", f"name={other_jail}", "persist", "path=/"],
                check=True)
            other = subprocess.Popen(args=["sleep", "60"])
            try:
                jid = int(subprocess.run(
                    args=["jls", "-j", other_jail, "jid"],
                    capture_output=True, check=True).stdout)
                state_path = os.path.join(
                    "/var/run/ocijail", self.container_id, "state.json")
                with open(state_path) as f:
                    state = json.load(f)
                state["jid"] = jid
                state["pid"] = other.pid
                with open(state_path, "w") as f:
                    json.dump(state, f)

                ret = subprocess.run(
                    args=[cmd, "state", self.container_id],
                    capture_output=True)
                self.assertEqual(json.loads(ret.stdout)["status"], "stopped")
                ret = subprocess.run(
                    args=[cmd, "exec", self.container_id, "sh"],
                    capture_output=True)
                self.assertNotEqual(ret.returncode, 0)
                subprocess.run(args=[cmd, "kill", self.container_id, "KILL"])
                self.delete()
                self.assertIsNone(other.poll())
                ret = subprocess.run(args=["jls", "-j", other_jail])
                self.assertEqual(ret.returncode, 0)
            finally:
                other.kill()
                other.wait()
                subprocess.run(args=["jail", "-r", other_jail])

    def test_failed_create_cleanup(self):
        # If create fails after mounting the volumes, here because the
        # hostname is too long for the jail, it unmounts them and removes
        # the state.
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["root"]["readonly"] = True
            c["hostname"] = "x" * 300
            c["process"]["args"] = ["sh", "-c", "exit 0"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["mounts"] = [{"type": "tmpfs", "destination": "/tmpdir"}]
            ret, _, _ = self.run_with_config(c, expected_ret=1)
            self.assertEqual(ret, 1)
            state_dir = os.path.join("/var/run/ocijail", self.container_id)
            self.assertFalse(os.path.exists(state_dir))
            mounts = subprocess.run(
                args=["mount"], capture_output=True).stdout.decode("utf-8")
            self.assertNotIn(root_dir, mounts)
            self.assertNotIn(state_dir, mounts)

    def test_killed_create(self):
        # A container left in the creating state by a create which was
        # killed is reported as stopped and can be deleted.
        state_dir = os.path.join("/var/run/ocijail", self.container_id)
        os.makedirs(state_dir)
        with open(os.path.join(state_dir, "state.json"), "w") as f:
            json.dump({
                "status": "creating",
                "bundle": "/nonexistent",
                "root_path": "/nonexistent",
                "root_readonly": False,
                "volumes_mounted": False,
                "config": {"mounts": [{"destination": "/tmpdir"}]},
            }, f)
        ret = subprocess.run(
            args=[cmd, "state", self.container_id], capture_output=True)
        self.assertEqual(ret.returncode, 0, ret.stderr)
        self.assertEqual(json.loads(ret.stdout)["status"], "stopped")
        self.delete()
        self.assertFalse(os.path.exists(state_dir))

    def test_jail_error(self):
        # The kernel gives no message for a hostname which is too long, so
        # we report just the error.
        c = self.config()
        c["hostname"] = "x" * 300
        c["process"]["args"] = ["sh", "-c", "exit 0"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            ret = subprocess.run(
                args=[cmd, "create", "--bundle", bundle_dir,
                      self.container_id],
                capture_output=True)
            self.assertNotEqual(ret.returncode, 0)
            self.assertTrue(ret.stderr.decode("utf-8").endswith(
                "error calling jail_set: File name too long\n"))

    def jail_params(self, c, *params):
        "Create a container with config c and return its jail parameters"
        c["process"]["args"] = ["sleep", "60"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            try:
                out = subprocess.run(
                    args=["jls", "-j", self.container_id, "-n", *params],
                    capture_output=True, check=True).stdout.decode("utf-8")
            finally:
                os.kill(pid, 9)
                stdout.close()
                stderr.close()
                os.waitpid(pid, os.WEXITED)
                self.delete()
        res = {}
        for param in out.split():
            if "=" in param:
                key, value = param.split("=", 1)
                res[key] = value
            elif ".no" in param:
                res[param.replace(".no", ".", 1)] = False
            else:
                res[param] = True
        return res

    def test_freebsd_jail(self):
        allow = ["allow.raw_sockets", "allow.chflags", "allow.set_hostname",
                 "allow.suser", "allow.mlock", "allow.mount",
                 "allow.mount.tmpfs"]

        # Without a freebsd section, the defaults are as before
        p = self.jail_params(self.config(), "enforce_statfs", *allow)
        self.assertEqual(p["enforce_statfs"], "1")
        self.assertTrue(p["allow.raw_sockets"])
        self.assertTrue(p["allow.chflags"])

        # With one, they are those of the specification
        c = self.config()
        c["freebsd"] = {"jail": {}}
        p = self.jail_params(c, "enforce_statfs", "host", *allow)
        self.assertEqual(p["enforce_statfs"], "2")
        self.assertEqual(p["host"], "inherit")
        for param in allow:
            self.assertEqual(p[param], param == "allow.suser", param)

        # Explicit settings
        c = self.config()
        c["hostname"] = "myhost"
        c["freebsd"] = {"jail": {
            "host": "new",
            "sysvshm": "new",
            "enforceStatfs": 1,
            "allow": {
                "rawSockets": True,
                "setHostname": True,
                "suser": False,
                "mount": ["tmpfs"],
            },
        }}
        p = self.jail_params(c, "enforce_statfs", "host", "host.hostname",
                             "sysvshm", *allow)
        self.assertEqual(p["enforce_statfs"], "1")
        self.assertEqual(p["host"], "new")
        self.assertEqual(p["host.hostname"], "myhost")
        self.assertEqual(p["sysvshm"], "new")
        self.assertTrue(p["allow.raw_sockets"])
        self.assertTrue(p["allow.set_hostname"])
        self.assertFalse(p["allow.suser"])
        self.assertTrue(p["allow.mount"])
        self.assertTrue(p["allow.mount.tmpfs"])
        self.assertFalse(p["allow.chflags"])

        # The freebsd section takes precedence over annotations, which still
        # apply where it says nothing
        c = self.config()
        c["annotations"] = {
            "org.freebsd.jail.allow.raw_sockets": "true",
            "org.freebsd.jail.allow.mlock": "true",
            "org.freebsd.jail.sysvshm": "disable",
        }
        c["freebsd"] = {"jail": {
            "sysvshm": "new",
            "allow": {"rawSockets": False},
        }}
        p = self.jail_params(c, "sysvshm", *allow)
        self.assertEqual(p["sysvshm"], "new")
        self.assertFalse(p["allow.raw_sockets"])
        self.assertTrue(p["allow.mlock"])

        # Addresses
        c = self.config()
        c["freebsd"] = {"jail": {"ip4Addr": ["127.0.0.1"], "ip6": "disable"}}
        p = self.jail_params(c, "ip4.addr", "ip6")
        self.assertEqual(p["ip4.addr"], "127.0.0.1")
        self.assertEqual(p["ip6"], "disable")

    def test_freebsd_interface(self):
        # The container's addresses are added to the interface while it
        # exists, except for those which were already there
        def lo0():
            return subprocess.run(
                args=["ifconfig", "lo0"],
                capture_output=True, check=True).stdout.decode("utf-8")
        c = self.config()
        c["process"]["args"] = ["sleep", "60"]
        c["freebsd"] = {"jail": {
            "interface": "lo0",
            "ip4Addr": ["127.0.0.42", "127.0.0.1"],
            "ip6Addr": ["fd00::42"],
        }}
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            try:
                self.assertIn("inet 127.0.0.42 ", lo0())
                self.assertIn("inet6 fd00::42 ", lo0())
                out = subprocess.run(
                    args=["jls", "-j", self.container_id, "ip4.addr"],
                    capture_output=True, check=True).stdout.decode("utf-8")
                self.assertEqual(out.strip(), "127.0.0.42,127.0.0.1")
            finally:
                os.kill(pid, 9)
                stdout.close()
                stderr.close()
                os.waitpid(pid, os.WEXITED)
                self.delete()
        self.assertNotIn("inet 127.0.0.42 ", lo0())
        self.assertNotIn("inet6 fd00::42 ", lo0())
        self.assertIn("inet 127.0.0.1 ", lo0())

    def test_freebsd_vnet_interfaces(self):
        # The interfaces are moved into the container's vnet while it exists
        epair = subprocess.run(
            args=["ifconfig", "epair", "create"],
            capture_output=True, check=True).stdout.decode("utf-8").strip()
        other = epair[:-1] + "b"
        def host_interfaces():
            return subprocess.run(
                args=["ifconfig", "-l"],
                capture_output=True, check=True).stdout.decode("utf-8").split()
        try:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", "ifconfig -l; exit 0"]
            c["freebsd"] = {"jail": {"vnet": "new", "vnetInterfaces": [epair]}}
            with tempfile.TemporaryDirectory() as bundle_dir:
                with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                    json.dump(c, f)
                pid, stdout, stderr = self.create(bundle_dir)
                try:
                    self.assertNotIn(epair, host_interfaces())
                    self.assertIn(other, host_interfaces())
                    self.start()
                    out = stdout.read().decode("utf-8")
                    self.assertIn(epair, out.split())
                finally:
                    try:
                        os.kill(pid, 9)
                    except ProcessLookupError:
                        pass
                    stdout.close()
                    stderr.close()
                    os.waitpid(pid, os.WEXITED)
                    self.delete()
            self.assertIn(epair, host_interfaces())
        finally:
            subprocess.run(args=["ifconfig", epair, "destroy"])

    def test_freebsd_devices(self):
        # Devices hidden by the devfs ruleset can be exposed, with a mode
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["env"] = ["PATH=/rescue"]
            c["process"]["args"] = ["sh", "-c", "ls -l /dev/mem; exit 0"]
            c["mounts"] = [{"destination": "/dev", "type": "devfs",
                            "source": "devfs", "options": ["ruleset=4"]}]
            try:
                ret, out, _ = self.run_with_config(c)
                self.assertEqual(ret, 0)
                self.assertEqual(out, "")
            finally:
                # Unmount /dev before root_dir is removed
                self.delete()

            c["freebsd"] = {"devices": [{"path": "mem", "mode": 0o600}]}
            try:
                ret, out, _ = self.run_with_config(c)
                self.assertEqual(ret, 0)
                self.assertTrue(out.startswith("crw------- "), out)
            finally:
                self.delete()

    def test_rlimits(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "ulimit -n; ulimit -H -n"]
        c["process"]["rlimits"] = [
            {"type": "RLIMIT_NOFILE", "soft": 1000, "hard": 2000},
        ]
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "1000\n2000\n")
        self.delete()

        # podman's defaults are above the system's limits, which apply
        # instead.
        maxfiles = int(subprocess.run(
            args=["sysctl", "-n", "kern.maxfilesperproc"],
            capture_output=True).stdout)
        c["process"]["rlimits"] = [
            {"type": "RLIMIT_NOFILE", "soft": 1048576, "hard": 1048576},
            {"type": "RLIMIT_NPROC", "soft": 1048576, "hard": 1048576},
        ]
        c["process"]["args"] = ["sh", "-c", "ulimit -n"]
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(int(out), maxfiles)
        self.delete()

        # Linux limits which FreeBSD lacks are rejected.
        c["process"]["rlimits"] = [
            {"type": "RLIMIT_RTTIME", "soft": 1, "hard": 1},
        ]
        ret, _, _ = self.run_with_config(c, expected_ret=1)
        self.assertEqual(ret, 1)

    def test_no_new_privileges(self):
        c = self.config()
        c["process"]["args"] = [
            "sh", "-c", "proccontrol -m nonewprivs -q -p $$; exit 0"]
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "disabled\n")
        self.delete()

        c["process"]["noNewPrivileges"] = True
        ret, out, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.assertEqual(out, "enabled\n")

    def jail_process_states(self):
        out = subprocess.run(
            args=["ps", "-J", self.container_id, "-o", "pid=", "-o", "state="],
            capture_output=True).stdout.decode("utf-8")
        return [line.split()[1] for line in out.splitlines() if line.strip()]

    def test_pause_resume(self):
        # The container forks continuously, so pause has to catch the new
        # processes too
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "while :; do /bin/sh -c true; done"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            try:
                self.start()
                time.sleep(0.5)
                ret = subprocess.run(args=[cmd, "pause", self.container_id])
                self.assertEqual(ret.returncode, 0)
                self.assertEqual(self.container_state()["status"], "paused")
                states = self.jail_process_states()
                self.assertTrue(states)
                self.assertTrue(
                    all(s.startswith(("T", "Z")) for s in states), states)
                # Nothing runs while paused
                time.sleep(0.5)
                self.assertEqual(len(self.jail_process_states()), len(states))

                # Pausing twice fails, as does exec or delete without --force
                ret = subprocess.run(args=[cmd, "pause", self.container_id])
                self.assertNotEqual(ret.returncode, 0)
                ret = subprocess.run(
                    args=[cmd, "exec", self.container_id, "true"],
                    capture_output=True)
                self.assertNotEqual(ret.returncode, 0)
                ret = subprocess.run(args=[cmd, "delete", self.container_id])
                self.assertNotEqual(ret.returncode, 0)

                ret = subprocess.run(args=[cmd, "resume", self.container_id])
                self.assertEqual(ret.returncode, 0)
                self.assertEqual(self.container_state()["status"], "running")
                time.sleep(0.2)
                self.assertFalse(
                    all(s.startswith("T") for s in self.jail_process_states()))
                ret = subprocess.run(args=[cmd, "resume", self.container_id])
                self.assertNotEqual(ret.returncode, 0)

                # A paused container can be deleted with --force
                ret = subprocess.run(args=[cmd, "pause", self.container_id])
                self.assertEqual(ret.returncode, 0)
                ret = subprocess.run(
                    args=[cmd, "delete", "--force", self.container_id])
                self.assertEqual(ret.returncode, 0)
            finally:
                try:
                    os.kill(pid, 9)
                except ProcessLookupError:
                    pass
                stdout.close()
                stderr.close()
                os.waitpid(pid, os.WEXITED)

    def rctl_rules(self):
        out = subprocess.run(
            args=["rctl", f"jail:{self.container_id}"],
            capture_output=True).stdout.decode("utf-8")
        prefix = f"jail:{self.container_id}:"
        return sorted(r[len(prefix):] for r in out.split() if r.startswith(prefix))

    def test_resource_limits(self):
        # Too many processes fail to fork
        c = self.config()
        c["process"]["args"] = [
            "sh", "-c", "for i in 1 2 3 4 5 6 7 8; do sleep 1 & done; wait"]
        c["linux"] = {"resources": {"pids": {"limit": 4}}}
        ret, _, err = self.run_with_config(c)
        self.assertIn("Cannot fork", err)
        self.delete()

        # A process which takes the container over its memory limit is
        # killed
        c = self.config()
        c["process"]["args"] = [
            sys.executable, "-c",
            "import time; a = bytearray(200 * 1024 * 1024); time.sleep(3)"]
        c["linux"] = {"resources": {"memory": {"limit": 64 * 1024 * 1024}}}
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            self.start()
            stdout.close()
            stderr.close()
            _, status = os.waitpid(pid, os.WEXITED)
            self.assertTrue(os.WIFSIGNALED(status))
            self.assertEqual(os.WTERMSIG(status), 9)
        self.delete()
        # but can use less
        c["process"]["args"] = [
            sys.executable, "-c",
            "import time; a = bytearray(16 * 1024 * 1024); time.sleep(3)"]
        ret, _, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)
        self.delete()

        # The other limits become rules, which are removed with the container
        c = self.config()
        c["process"]["args"] = ["sleep", "60"]
        c["linux"] = {"resources": {
            "memory": {"limit": 64 * 1024 * 1024, "swap": 96 * 1024 * 1024},
            "cpu": {"quota": 50000, "period": 100000},
            "blockIO": {
                "throttleReadBpsDevice": [
                    {"major": 0, "minor": 1, "rate": 2000000},
                    {"major": 0, "minor": 2, "rate": 1000000},
                ],
                "throttleWriteIOPSDevice": [
                    {"major": 0, "minor": 1, "rate": 100},
                ],
            },
        }}
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            try:
                self.assertEqual(self.rctl_rules(), [
                    "memoryuse:sigkill=67108864",
                    "pcpu:deny=50",
                    "readbps:throttle=1000000",
                    "swapuse:deny=33554432",
                    "writeiops:throttle=100",
                ])
            finally:
                os.kill(pid, 9)
                stdout.close()
                stderr.close()
                os.waitpid(pid, os.WEXITED)
                self.delete()
        self.assertEqual(self.rctl_rules(), [])

    def test_update(self):
        c = self.config()
        c["process"]["args"] = ["sleep", "60"]
        c["linux"] = {"resources": {"pids": {"limit": 100}}}
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            def update(*args):
                return subprocess.run(
                    args=[cmd, "update", *args, self.container_id],
                    capture_output=True)
            try:
                self.assertEqual(self.rctl_rules(), ["maxproc:deny=100"])

                # Options change part of the resources
                ret = update("--memory", "64m", "--cpu-quota", "25000")
                self.assertEqual(ret.returncode, 0, ret.stderr)
                self.assertEqual(self.rctl_rules(), [
                    "maxproc:deny=100",
                    "memoryuse:sigkill=67108864",
                    "pcpu:deny=25",
                ])

                # A resources file replaces them, before applying options
                resources = os.path.join(bundle_dir, "resources.json")
                with open(resources, "w") as f:
                    json.dump({"pids": {"limit": 10}}, f)
                ret = update("-r", resources, "--memory-swap", "1.5G")
                self.assertEqual(ret.returncode, 0, ret.stderr)
                self.assertEqual(self.rctl_rules(), [
                    "maxproc:deny=10",
                    "swapuse:deny=1610612736",
                ])

                # The update is saved, so later updates build on it
                ret = update("--pids-limit", "20")
                self.assertEqual(ret.returncode, 0, ret.stderr)
                self.assertEqual(self.rctl_rules(), [
                    "maxproc:deny=20",
                    "swapuse:deny=1610612736",
                ])

                for args in [["--memory", "lots"], ["--pids-limit", "1.5"],
                             ["--cpu-share", "100"]]:
                    ret = update(*args)
                    self.assertNotEqual(ret.returncode, 0, args)
            finally:
                os.kill(pid, 9)
                stdout.close()
                stderr.close()
                os.waitpid(pid, os.WEXITED)
        # A stopped container can't be updated
        ret = update("--pids-limit", "5")
        self.assertNotEqual(ret.returncode, 0)
        self.delete()
        self.assertEqual(self.rctl_rules(), [])

    def test_events(self):
        c = self.config()
        c["process"]["args"] = [
            "sh", "-c", "sleep 30 & sleep 30 & while :; do :; done"]
        c["linux"] = {"resources": {
            "memory": {"limit": 256 * 1024 * 1024},
            "pids": {"limit": 50},
        }}
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            try:
                self.start()
                # racct updates CPU time once a second
                time.sleep(2.5)
                ret = subprocess.run(
                    args=[cmd, "events", "--stats", self.container_id],
                    capture_output=True)
                self.assertEqual(ret.returncode, 0, ret.stderr)
                event = json.loads(ret.stdout)
                self.assertEqual(event["type"], "stats")
                self.assertEqual(event["id"], self.container_id)
                data = event["data"]
                self.assertGreater(data["cpu"]["usage"]["total"], 0)
                self.assertGreater(data["memory"]["usage"]["usage"], 0)
                self.assertEqual(
                    data["memory"]["usage"]["limit"], 256 * 1024 * 1024)
                self.assertEqual(data["pids"]["current"], 3)
                self.assertEqual(data["pids"]["limit"], 50)

                # Without --stats, events reports until the container stops
                with subprocess.Popen(
                    args=[cmd, "events", "--interval", "200ms",
                          self.container_id],
                    stdout=subprocess.PIPE
                ) as proc:
                    first = json.loads(proc.stdout.readline())
                    self.assertEqual(first["type"], "stats")
                    second = json.loads(proc.stdout.readline())
                    self.assertEqual(second["type"], "stats")
                    os.kill(pid, 9)
                    os.waitpid(pid, os.WEXITED)
                    pid = None
                    proc.stdout.read()
                    self.assertEqual(proc.wait(timeout=10), 0)
            finally:
                if pid:
                    os.kill(pid, 9)
                    os.waitpid(pid, os.WEXITED)
                stdout.close()
                stderr.close()
        ret = subprocess.run(
            args=[cmd, "events", "--stats", self.container_id],
            capture_output=True)
        self.assertNotEqual(ret.returncode, 0)

    def test_hook_create_runtme(self):
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", f"exit $(cat {scratch}/file)"]
            c["hooks"] = {
                "createRuntime": [
                    {
                        "path": "/bin/sh",
                        "env": [
                            "foo=99"
                        ],
                        "args": [
                            "sh",
                            "-c",
                            f"echo $foo > {scratch}/file"
                        ]
                    }
                ]
            }
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 99)

    def test_state_oci_version(self):
        # state and features should agree on the runtime spec version
        with tempfile.TemporaryDirectory() as bundle_dir:
            c = self.config()
            c["process"]["args"] = ["true"]
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            _, stdout, stderr = self.create(bundle_dir)
            stdout.close()
            stderr.close()
            state = json.loads(subprocess.run(
                args=[cmd, "state", self.container_id],
                capture_output=True,
                check=True).stdout)
            features = json.loads(subprocess.run(
                args=[cmd, "features"],
                capture_output=True,
                check=True).stdout)
            self.assertEqual(state["ociVersion"], "1.3.0")
            self.assertEqual(features["ociVersionMax"], "1.3.0")

    def test_stopped_pid_reuse(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "exit 0"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            stdout.close()
            stderr.close()
            os.kill(pid, 9)
            os.waitpid(pid, os.WEXITED)

            # Simulate the container's pid being reused by an unrelated
            # process.
            with subprocess.Popen(["sleep", "60"]) as decoy:
                try:
                    state_json = os.path.join(
                        "/var/run/ocijail", self.container_id, "state.json")
                    with open(state_json) as f:
                        state = json.load(f)
                    state["pid"] = decoy.pid
                    with open(state_json, "w") as f:
                        json.dump(state, f)
                    ret = subprocess.run(
                        args=[cmd, "state", self.container_id],
                        capture_output=True)
                    self.assertEqual(ret.returncode, 0)
                    self.assertEqual(json.loads(ret.stdout)["status"], "stopped")
                finally:
                    decoy.kill()

    def test_kill_pid(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "sleep 60 & echo $!; wait $!"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            self.start()
            sleep_pid = int(stdout.readline())

            # A process outside the container must not be signalled
            with subprocess.Popen(["sleep", "60"]) as decoy:
                try:
                    ret = subprocess.run(
                        args=[cmd, "kill", "--pid", str(decoy.pid),
                              self.container_id, "KILL"],
                        stderr=subprocess.DEVNULL)
                    self.assertNotEqual(ret.returncode, 0)
                    self.assertIsNone(decoy.poll())
                finally:
                    decoy.kill()

            # Only the given process is signalled, not the container process
            ret = subprocess.run(
                args=[cmd, "kill", "--pid", str(sleep_pid),
                      self.container_id, "KILL"])
            self.assertEqual(ret.returncode, 0)
            stdout.close()
            stderr.close()
            pid2, status = os.waitpid(pid, os.WEXITED)
            self.assertEqual(status & 0xff, 0)
            self.assertEqual(status >> 8, 128 + 9)

    def test_hook_prestart(self):
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", f"exit $(cat {scratch}/file)"]
            c["hooks"] = {
                "prestart": [
                    {
                        "path": "/bin/sh",
                        "args": [
                            "sh",
                            "-c",
                            f"echo 99 > {scratch}/file"
                        ]
                    }
                ]
            }
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 99)

    def test_hook_prestart_in_create(self):
        # prestart hooks run during create, before createRuntime
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sleep", "60"]
            c["hooks"] = {
                "prestart": [
                    {
                        "path": "/bin/sh",
                        "args": ["sh", "-c", f"echo prestart >> {scratch}/file"]
                    }
                ],
                "createRuntime": [
                    {
                        "path": "/bin/sh",
                        "args": ["sh", "-c", f"echo createRuntime >> {scratch}/file"]
                    }
                ]
            }
            with tempfile.TemporaryDirectory() as bundle_dir:
                with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                    json.dump(c, f)
                pid, stdout, stderr = self.create(bundle_dir)
                try:
                    with open(f"{scratch}/file") as f:
                        self.assertEqual(f.read(), "prestart\ncreateRuntime\n")
                finally:
                    os.kill(pid, 9)
                    stdout.close()
                    stderr.close()
                    os.waitpid(pid, os.WEXITED)

    def test_start_waits_for_exec(self):
        with tempfile.TemporaryDirectory() as scratch:
            # poststart hooks run once the container has executed its
            # command
            script = (
                "import json, subprocess, sys\n"
                "pid = json.load(sys.stdin)['pid']\n"
                "out = subprocess.run(['ps', '-o', 'comm=', '-p', str(pid)],"
                " capture_output=True).stdout\n"
                f"open('{scratch}/comm', 'wb').write(out)\n")
            c = self.config()
            c["process"]["args"] = ["sleep", "60"]
            c["hooks"] = {
                "poststart": [
                    {
                        "path": sys.executable,
                        "args": ["python3", "-c", script]
                    }
                ]
            }
            with tempfile.TemporaryDirectory() as bundle_dir:
                with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                    json.dump(c, f)
                pid, stdout, stderr = self.create(bundle_dir)
                try:
                    self.start()
                    with open(f"{scratch}/comm") as f:
                        self.assertEqual(f.read(), "sleep\n")
                finally:
                    os.kill(pid, 9)
                    stdout.close()
                    stderr.close()
                    os.waitpid(pid, os.WEXITED)
                    self.delete()

        # If the container can't execute its command, start fails
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "exit 0"]
        c["process"]["cwd"] = "/nonexistent"
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            ret = subprocess.run(
                args=[cmd, "start", self.container_id], capture_output=True)
            self.assertNotEqual(ret.returncode, 0)
            self.assertIn(b"/nonexistent", ret.stderr)
            stdout.close()
            stderr.close()
            os.waitpid(pid, os.WEXITED)

    def test_hook_create_container(self):
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["sh", "-c", "exit $(cat /file)"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["hooks"] = {
                "createContainer": [
                    {
                        "path": "/rescue/sh",
                        "args": [
                            "sh",
                            "-c",
                            "echo 42 > file"
                        ]
                    }
                ]
            }
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 42)

    def test_hook_start_container(self):
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["sh", "-c", "exit $(cat /file)"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["hooks"] = {
                "startContainer": [
                    {
                        "path": "/rescue/sh",
                        "args": [
                            "sh",
                            "-c",
                            "echo 42 > /file"
                        ]
                    }
                ]
            }
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 42)

    def hook(self, *args, timeout=None):
        h = {"path": "/bin/sh", "args": ["sh", "-c", *args]}
        if timeout:
            h["timeout"] = timeout
        return h

    def check_no_container(self):
        ret = subprocess.run(
            args=[cmd, "state", self.container_id], capture_output=True)
        self.assertNotEqual(ret.returncode, 0)
        ret = subprocess.run(
            args=["jls", "-j", self.container_id], capture_output=True)
        self.assertNotEqual(ret.returncode, 0)

    def test_hook_failure(self):
        # A failing createRuntime or createContainer hook fails create and
        # removes the container
        for phase in ["createRuntime", "createContainer"]:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", "exit 0"]
            c["hooks"] = {phase: [self.hook("exit 3")]}
            ret, _, _ = self.run_with_config(c, expected_ret=1)
            self.assertEqual(ret, 1)
            self.check_no_container()

        # A failing startContainer hook stops the container before its
        # process runs, and start reports it
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", f"touch {scratch}/ran"]
            c["hooks"] = {"startContainer": [self.hook("exit 3")]}
            with tempfile.TemporaryDirectory() as bundle_dir:
                with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                    json.dump(c, f)
                pid, stdout, stderr = self.create(bundle_dir)
                ret = subprocess.run(
                    args=[cmd, "start", self.container_id],
                    capture_output=True)
                self.assertNotEqual(ret.returncode, 0)
                self.assertIn(b"startContainer hook", ret.stderr)
                stdout.close()
                stderr.close()
                _, status = os.waitpid(pid, os.WEXITED)
                self.assertNotEqual(status, 0)
            self.assertFalse(os.path.exists(f"{scratch}/ran"))
            self.delete()

        # Failing poststart and poststop hooks are only warnings
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "exit 42"]
        c["hooks"] = {
            "poststart": [self.hook("exit 3")],
            "poststop": [self.hook("exit 3")],
        }
        ret, _, _ = self.run_with_config(c)
        self.assertEqual(ret, 42)
        self.delete()
        self.check_no_container()

    def test_hook_timeout(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "exit 0"]
        c["hooks"] = {"createRuntime": [self.hook("sleep 30", timeout=1)]}
        start = time.monotonic()
        ret, _, _ = self.run_with_config(c, expected_ret=1)
        self.assertEqual(ret, 1)
        self.assertLess(time.monotonic() - start, 10)
        self.check_no_container()

        # A hook which finishes in time succeeds
        c["hooks"] = {"createRuntime": [self.hook("exit 0", timeout=10)]}
        ret, _, _ = self.run_with_config(c)
        self.assertEqual(ret, 0)

    def test_hook_argv(self):
        # The hook's args are its argv, including argv[0]
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", "exit 0"]
            c["hooks"] = {
                "createRuntime": [
                    {
                        "path": "/bin/sh",
                        "args": [
                            "myhook",
                            "-c",
                            f"ps -o args= -p $$ > {scratch}/file; exit 0",
                        ]
                    }
                ]
            }
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            with open(f"{scratch}/file") as f:
                self.assertTrue(f.read().startswith("myhook -c "))

    def test_hook_poststop(self):
        with tempfile.TemporaryDirectory() as scratch:
            c = self.config()
            c["process"]["args"] = ["sh", "-c", f"exit $(cat {scratch}/file)"]
            c["hooks"] = {
                "createRuntime": [
                    {
                        "path": "/bin/sh",
                        "env": [
                            "foo=123"
                        ],
                        "args": [
                            "sh",
                            "-c",
                            f"echo $foo > {scratch}/file"
                        ]
                    }
                ],
                "poststop": [
                    {
                        "path": "/bin/rm",
                        "args": [
                            "rm",
                            f"{scratch}/file"
                        ]
                    }
                ]
            }
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 123)
            self.assertTrue(os.path.exists(f"{scratch}/file"))
            self.delete()
            self.assertFalse(os.path.exists(f"{scratch}/file"))

    def test_validate_command_path(self):
        with tempfile.TemporaryDirectory() as root_dir:
            random_dir = secrets.token_urlsafe(8)
            shutil.copytree("/rescue", os.path.join(root_dir, random_dir))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["echo", "Hello World"]
            c["process"]["env"] = [f"PATH=/{random_dir}"]
            ret, out, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            self.assertEqual(out, "Hello World\n")

    def test_validate_command_abs(self):
        with tempfile.TemporaryDirectory() as root_dir:
            random_dir = secrets.token_urlsafe(8)
            shutil.copytree("/rescue", os.path.join(root_dir, random_dir))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = [f"/{random_dir}/echo", "Hello World"]
            ret, out, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            self.assertEqual(out, "Hello World\n")

    def test_ps(self):
        c = self.config()
        c["process"]["args"] = ["sh", "-c", "sleep 60 & echo $!; wait $!"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            self.start()
            sleep_pid = int(stdout.readline())
            expected = sorted([pid, sleep_pid])

            ret = subprocess.run(
                args=[cmd, "ps", "--format", "json", self.container_id],
                capture_output=True)
            self.assertEqual(ret.returncode, 0)
            self.assertEqual(json.loads(ret.stdout), expected)

            ret = subprocess.run(
                args=[cmd, "ps", self.container_id], capture_output=True)
            self.assertEqual(ret.returncode, 0)
            self.assertEqual(
                ret.stdout.decode("utf-8").split(),
                ["PID"] + [str(p) for p in expected])

            os.kill(sleep_pid, 9)
            stdout.close()
            stderr.close()
            os.waitpid(pid, os.WEXITED)

    def test_exec_args(self):
        c = self.config()
        c["process"]["args"] = ["sleep", "60"]
        c["process"]["cwd"] = "/tmp"
        c["process"]["env"].append("FOO=bar")
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            self.start()
            try:
                # Without --process, the container's process is used with
                # the given command, which may have option-like arguments
                ret = subprocess.run(
                    args=[cmd, "exec", self.container_id,
                          "sh", "-c", "echo $FOO; pwd"],
                    capture_output=True)
                self.assertEqual(ret.returncode, 0, ret.stderr)
                self.assertEqual(ret.stdout.decode("utf-8"), "bar\n/tmp\n")

                ret = subprocess.run(
                    args=[cmd, "exec", self.container_id],
                    capture_output=True)
                self.assertNotEqual(ret.returncode, 0)

                # Unknown options are still reported
                ret = subprocess.run(
                    args=[cmd, "exec", "--bogus", self.container_id,
                          "sh", "-c", "exit 0"],
                    capture_output=True)
                self.assertNotEqual(ret.returncode, 0)
                self.assertIn(b"unknown option --bogus", ret.stderr)

                # --cwd, --env and --user override the container's process
                ret = subprocess.run(
                    args=[cmd, "exec", "--cwd", "/", "-e", "FOO=baz",
                          "-e", "BAR=qux", "-u", "123:456", self.container_id,
                          "sh", "-c", "echo $FOO $BAR; pwd; id -u; id -g"],
                    capture_output=True)
                self.assertEqual(ret.returncode, 0, ret.stderr)
                self.assertEqual(
                    ret.stdout.decode("utf-8").split(),
                    ["baz", "qux", "/", "123", "456"])
                ret = subprocess.run(
                    args=[cmd, "exec", "--no-new-privs", self.container_id,
                          "sh", "-c", "proccontrol -m nonewprivs -q -p $$; exit 0"],
                    capture_output=True)
                self.assertEqual(ret.returncode, 0, ret.stderr)
                self.assertEqual(ret.stdout, b"enabled\n")
                ret = subprocess.run(
                    args=[cmd, "exec", "-u", "nobody", self.container_id,
                          "true"],
                    capture_output=True)
                self.assertNotEqual(ret.returncode, 0)
            finally:
                os.kill(pid, 9)
                stdout.close()
                stderr.close()
                os.waitpid(pid, os.WEXITED)

    def test_tmpcopyup(self):
        # Running the container should copy the contents of /tmpdir to a
        # tmpfs which is mounted over it
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["cat", "/tmpdir/file"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["mounts"] = [
                {
                    "type": "tmpfs",
                    "destination": "/tmpdir",
                    "options": ["tmpcopyup"],
                },
            ]
            os.mkdir(os.path.join(root_dir, "tmpdir"))
            with open(os.path.join(root_dir, "tmpdir", "file"), "w") as f:
                f.write("Hello World\n")
            ret, out, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            self.assertEqual(out, "Hello World\n")
            # Delete the container so that the tmpfs is unmounted
            # before we delete root_dir
            self.delete()

    def test_cleanup_mounts(self):
        # Verify that /foo is removed strictly after /foo/dir1 and /foo/dir2
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["process"]["args"] = ["sh", "-c", "exit 0"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["mounts"] = [
                {
                    "type": "tmpfs",
                    "destination": "/foo/dir1",
                },
                {
                    "type": "tmpfs",
                    "destination": "/foo/dir2",
                },
            ]
            ret, out, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            # Delete the container so that the tmpfs is unmounted
            # before we delete root_dir
            self.delete()

    def test_readonly_root(self):
        # Running the container should not modify the root
        with tempfile.TemporaryDirectory() as root_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["root"]["readonly"] = True
            c["process"]["args"] = ["sh", "-c", "echo world > /hello; exit 0"]
            c["process"]["env"] = ["PATH=/rescue"]
            c["mounts"] = [
                {
                    "type": "tmpfs",
                    "destination": "/tmpdir",
                },
            ]
            ret, out, err = self.run_with_config(c)
            self.assertEqual(err.count("Read-only file system"), 1)
            self.assertFalse(os.path.exists(os.path.join(root_dir, "hello")))
            self.assertEqual(ret, 0)
            # Delete the container so that the tmpfs is unmounted
            # before we delete root_dir
            self.delete()

    def test_readonly_root_unmount_busy(self):
        # If the read-only alias of the root cannot be unmounted, delete
        # should fail and leave the container in place so that it can be
        # retried.
        with tempfile.TemporaryDirectory() as root_dir, \
             tempfile.TemporaryDirectory() as busy_dir:
            shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
            c = self.config()
            c["root"]["path"] = root_dir
            c["root"]["readonly"] = True
            c["process"]["args"] = ["sh", "-c", "exit 0"]
            c["process"]["env"] = ["PATH=/rescue"]
            ret, _, _ = self.run_with_config(c)
            self.assertEqual(ret, 0)
            # Keep the alias busy by using it as the lower layer of another
            # nullfs mount.
            alias = os.path.join(
                "/var/run/ocijail", self.container_id, "readonly_root")
            subprocess.run(
                args=["mount", "-t", "nullfs", alias, busy_dir], check=True)
            try:
                ret = subprocess.run(
                    args=[cmd, "delete", self.container_id],
                    capture_output=True)
                self.assertNotEqual(ret.returncode, 0)
                self.assertIn("unmounting", ret.stderr.decode("utf-8"))
            finally:
                subprocess.run(args=["umount", busy_dir], check=True)
            self.delete()

    # setup is a function which is called to initialise the root, destination is
    # the path inside the root for our mount and real_destination is the path
    # inside the root after resolving symlinks
    def symlink_test_helper(self, setup, destination, real_destination, expected_ret=0):
        # Test mounting to a path which goes via a symbolic link
        with tempfile.TemporaryDirectory() as root_dir:
            with tempfile.NamedTemporaryFile(mode="wb", buffering=0) as file_to_mount:
                shutil.copytree("/rescue", os.path.join(root_dir, "rescue"))
                file_to_mount.write(b"Hello World\n")
                setup(root_dir)

                c = self.config()
                c["root"]["path"] = root_dir
                c["process"]["args"] = ["cat", real_destination]
                c["process"]["env"] = ["PATH=/rescue"]
                c["mounts"] = [
                    {
                        "type": "nullfs",
                        "destination": destination,
                        "source": file_to_mount.name,
                    },
                ]
                ret, out, _ = self.run_with_config(c, expected_ret)
                self.assertEqual(ret, expected_ret)
                if ret == 0:
                    self.assertEqual(out, "Hello World\n")
                # Delete the container so that the file mount is unmounted
                # before we delete root_dir
                self.delete()

    def test_mount_absolute_symlink(self):
        def setup(root_dir):
            # If mounting to /var/run/foo and /var/run is a symlink to /run, the
            # mount should target {root_dir}/run/foo
            os.mkdir(os.path.join(root_dir, "run"))
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("/run", os.path.join(root_dir, "var/run"))
        self.symlink_test_helper(setup, "/var/run/foo", "/run/foo")

    def test_mount_relative_symlink(self):
        def setup(root_dir):
            # If mounting to /var/run/foo and /var/run is a symlink to somedir, the
            # mount should target {root_dir}/var/somedir/foo
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("somedir", os.path.join(root_dir, "var/run"))
        self.symlink_test_helper(setup, "/var/run/foo", "/var/somedir/foo")

    def test_mount_symlink_parent(self):
        def setup(root_dir):
            # If mounting to /var/run/foo and /var/run is a symlink to "../somedir", the
            # mount should target {root_dir}/somedir/foo
            # 
            os.mkdir(os.path.join(root_dir, "somedir"))
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("../somedir", os.path.join(root_dir, "var/run"))
        self.symlink_test_helper(setup, "/var/run/foo", "/somedir/foo")

    def test_mount_symlink_root_escape(self):
        def setup(root_dir):
            # Verify that its not possible for a mount to escape above the root
            # directory using ".."
            # 
            os.mkdir(os.path.join(root_dir, "somedir"))
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("../../somedir", os.path.join(root_dir, "var/run"))
        self.symlink_test_helper(setup, "/var/run/foo", "/somedir/foo")

    def test_mount_recursive_symlink(self):
        def setup(root_dir):
            # Make sure that symlinks to symlinks are resolve correctly
            #
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("somelink", os.path.join(root_dir, "var/run"))
            os.symlink("somedir", os.path.join(root_dir, "var/somelink"))
        self.symlink_test_helper(setup, "/var/run/foo", "/var/somedir/foo")

    def test_mount_symlink_loop(self):
        def setup(root_dir):
            # Symlink loops should cause an error
            #
            os.mkdir(os.path.join(root_dir, "var"))
            os.symlink("somelink", os.path.join(root_dir, "var/run"))
            os.symlink("somelink", os.path.join(root_dir, "var/somelink"))
        self.symlink_test_helper(setup, "/var/run/foo", "/var/somedir/foo", expected_ret=1)

    def jail_pids(self):
        args = [cmd, "state", self.container_id]
        ret = subprocess.run(args=args, capture_output=True, check=True)
        jid = json.loads(ret.stdout)["annotations"]["org.freebsd.jail.jid"]
        # Ignore zombies - we are the reaper for orphaned container
        # processes and they will stay in the jail until reaped.
        ret = subprocess.run(
            args=["ps", "-x", "-J", jid, "-o", "pid=", "-o", "state="],
            capture_output=True)
        pids = set()
        for line in ret.stdout.decode("utf-8").splitlines():
            pid, state = line.split()
            if not state.startswith("Z"):
                pids.add(int(pid))
        return pids

    def wait_for_jail_pids(self, count):
        for _ in range(100):
            pids = self.jail_pids()
            if len(pids) == count:
                return pids
            time.sleep(0.1)
        self.fail(f"expected {count} processes in jail, found {pids}")

    def kill_helper(self, all):
        # The container process starts two children which stay in the
        # jail if only the container process is signalled.
        c = self.config()
        c["process"]["args"] = [
            "sh", "-c", "sleep 1000 & sleep 1000 & wait"]
        with tempfile.TemporaryDirectory() as bundle_dir:
            with open(os.path.join(bundle_dir, "config.json"), "w") as f:
                json.dump(c, f)
            pid, stdout, stderr = self.create(bundle_dir)
            stdout.close()
            stderr.close()
            self.start()
            self.wait_for_jail_pids(3)
            args = [cmd, "kill"]
            if all:
                args.append("--all")
            args += [self.container_id, "TERM"]
            ret = subprocess.run(args=args)
            self.assertEqual(ret.returncode, 0)
            _, status = os.waitpid(pid, os.WEXITED)
            self.assertEqual(status & 0x7f, signal.SIGTERM)

    def test_kill(self):
        self.kill_helper(False)
        self.assertEqual(len(self.jail_pids()), 2)

    def test_kill_all(self):
        self.kill_helper(True)
        self.wait_for_jail_pids(0)

if __name__ == "__main__":
    if os.getenv("OCIJAIL_PATH"):
        cmd = os.getenv("OCIJAIL_PATH")
    unittest.main()

