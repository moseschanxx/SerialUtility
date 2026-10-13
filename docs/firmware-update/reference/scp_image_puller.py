#!/usr/bin/env python3
"""
SCP/SFTP Image Puller GUI

Reference copy for docs/firmware-update/REQUIREMENTS.md: the tool the Firmware tab replaces
(machine-specific default paths blanked).

Connects to a remote Ubuntu machine over SFTP (SSH) and downloads image files
(and other output files) from a remote directory tree into a local folder on
this Windows machine.

Requires: paramiko
    pip install paramiko

Run:
    python scp_image_puller.py
"""
import json
import os
import queue
import stat
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

try:
    import paramiko
except ImportError:
    paramiko = None

CONFIG_PATH = Path(__file__).with_name("scp_image_puller_config.json")

DEFAULT_CONFIG = {
    "host": "",
    "port": "22",
    "username": "",
    "remote_dir": "",   # e.g. <sdk>/output/image
    "local_dir": "",
    "extensions": ".img,.bin,.txt",
    "recursive": True,
    "overwrite": True,
}


def load_config():
    if CONFIG_PATH.exists():
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
            merged = dict(DEFAULT_CONFIG)
            merged.update(data)
            return merged
        except (OSError, json.JSONDecodeError):
            pass
    return dict(DEFAULT_CONFIG)


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
    except OSError:
        pass


class ImagePullerApp:
    def __init__(self, root):
        self.root = root
        self.root.title("RV1106B Image Puller (SFTP)")
        self.root.geometry("720x560")
        self.root.minsize(640, 480)

        self.cfg = load_config()
        self.msg_queue = queue.Queue()
        self.worker_thread = None
        self.cancel_flag = threading.Event()

        self._build_ui()
        self._poll_queue()

    # ---------------------------------------------------------- UI setup
    def _build_ui(self):
        pad = {"padx": 6, "pady": 4}

        conn_frame = ttk.LabelFrame(self.root, text="Connection")
        conn_frame.pack(fill="x", **pad)

        ttk.Label(conn_frame, text="Host / IP:").grid(row=0, column=0, sticky="w", **pad)
        self.host_var = tk.StringVar(value=self.cfg["host"])
        ttk.Entry(conn_frame, textvariable=self.host_var, width=30).grid(row=0, column=1, sticky="w", **pad)

        ttk.Label(conn_frame, text="Port:").grid(row=0, column=2, sticky="w", **pad)
        self.port_var = tk.StringVar(value=self.cfg["port"])
        ttk.Entry(conn_frame, textvariable=self.port_var, width=6).grid(row=0, column=3, sticky="w", **pad)

        ttk.Label(conn_frame, text="Username:").grid(row=1, column=0, sticky="w", **pad)
        self.user_var = tk.StringVar(value=self.cfg["username"])
        ttk.Entry(conn_frame, textvariable=self.user_var, width=30).grid(row=1, column=1, sticky="w", **pad)

        ttk.Label(conn_frame, text="Password:").grid(row=1, column=2, sticky="w", **pad)
        self.pass_var = tk.StringVar(value="")
        ttk.Entry(conn_frame, textvariable=self.pass_var, width=20, show="*").grid(row=1, column=3, sticky="w", **pad)

        paths_frame = ttk.LabelFrame(self.root, text="Paths")
        paths_frame.pack(fill="x", **pad)

        ttk.Label(paths_frame, text="Remote directory:").grid(row=0, column=0, sticky="w", **pad)
        self.remote_var = tk.StringVar(value=self.cfg["remote_dir"])
        ttk.Entry(paths_frame, textvariable=self.remote_var, width=60).grid(row=0, column=1, columnspan=2, sticky="we", **pad)

        ttk.Label(paths_frame, text="Local directory:").grid(row=1, column=0, sticky="w", **pad)
        self.local_var = tk.StringVar(value=self.cfg["local_dir"])
        ttk.Entry(paths_frame, textvariable=self.local_var, width=50).grid(row=1, column=1, sticky="we", **pad)
        ttk.Button(paths_frame, text="Browse...", command=self._browse_local).grid(row=1, column=2, sticky="w", **pad)

        paths_frame.columnconfigure(1, weight=1)

        opts_frame = ttk.LabelFrame(self.root, text="Options")
        opts_frame.pack(fill="x", **pad)

        ttk.Label(opts_frame, text="File extensions (comma-separated):").grid(row=0, column=0, sticky="w", **pad)
        self.ext_var = tk.StringVar(value=self.cfg["extensions"])
        ttk.Entry(opts_frame, textvariable=self.ext_var, width=30).grid(row=0, column=1, sticky="w", **pad)

        self.recursive_var = tk.BooleanVar(value=self.cfg["recursive"])
        ttk.Checkbutton(opts_frame, text="Search subfolders recursively", variable=self.recursive_var).grid(
            row=0, column=2, sticky="w", **pad
        )

        self.overwrite_var = tk.BooleanVar(value=self.cfg["overwrite"])
        ttk.Checkbutton(
            opts_frame,
            text="Overwrite existing local files (uncheck to skip files that already match in size)",
            variable=self.overwrite_var,
        ).grid(row=1, column=0, columnspan=3, sticky="w", **pad)

        btn_frame = ttk.Frame(self.root)
        btn_frame.pack(fill="x", **pad)

        self.pull_btn = ttk.Button(btn_frame, text="Pull Images", command=self._start_pull)
        self.pull_btn.pack(side="left", padx=6)

        self.cancel_btn = ttk.Button(btn_frame, text="Cancel", command=self._cancel_pull, state="disabled")
        self.cancel_btn.pack(side="left", padx=6)

        self.open_local_btn = ttk.Button(btn_frame, text="Open Local Folder", command=self._open_local_folder)
        self.open_local_btn.pack(side="left", padx=6)

        self.progress = ttk.Progressbar(self.root, mode="determinate")
        self.progress.pack(fill="x", **pad)

        self.status_var = tk.StringVar(value="Ready.")
        ttk.Label(self.root, textvariable=self.status_var, anchor="w").pack(fill="x", padx=6)

        log_frame = ttk.LabelFrame(self.root, text="Log")
        log_frame.pack(fill="both", expand=True, **pad)

        self.log_text = tk.Text(log_frame, height=14, wrap="word", state="disabled")
        self.log_text.pack(side="left", fill="both", expand=True)
        scroll = ttk.Scrollbar(log_frame, command=self.log_text.yview)
        scroll.pack(side="right", fill="y")
        self.log_text.configure(yscrollcommand=scroll.set)

        if paramiko is None:
            self._log("ERROR: paramiko is not installed. Run: pip install paramiko")
            self.pull_btn.configure(state="disabled")

    # ---------------------------------------------------------- helpers
    def _browse_local(self):
        chosen = filedialog.askdirectory(initialdir=self.local_var.get() or str(Path.home()))
        if chosen:
            self.local_var.set(chosen)

    def _open_local_folder(self):
        path = self.local_var.get().strip()
        if path and Path(path).exists():
            os.startfile(path)  # noqa: S606 (Windows-only helper, user-initiated)
        else:
            messagebox.showinfo("Not found", "Local folder does not exist yet.")

    def _log(self, message):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", message + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _current_config(self):
        return {
            "host": self.host_var.get().strip(),
            "port": self.port_var.get().strip() or "22",
            "username": self.user_var.get().strip(),
            "remote_dir": self.remote_var.get().strip(),
            "local_dir": self.local_var.get().strip(),
            "extensions": self.ext_var.get().strip(),
            "recursive": self.recursive_var.get(),
            "overwrite": self.overwrite_var.get(),
        }

    # ---------------------------------------------------------- actions
    def _start_pull(self):
        if paramiko is None:
            messagebox.showerror("Missing dependency", "Install paramiko first: pip install paramiko")
            return

        cfg = self._current_config()
        if not cfg["host"] or not cfg["username"]:
            messagebox.showerror("Missing info", "Host and username are required.")
            return
        if not cfg["remote_dir"]:
            messagebox.showerror("Missing info", "Remote directory is required.")
            return
        if not cfg["local_dir"]:
            messagebox.showerror("Missing info", "Local directory is required.")
            return
        try:
            port = int(cfg["port"])
        except ValueError:
            messagebox.showerror("Invalid port", "Port must be a number.")
            return

        password = self.pass_var.get()
        if not password:
            if not messagebox.askyesno("No password", "No password entered. Continue anyway (e.g. agent/keyless auth)?"):
                return

        save_config(cfg)  # never persist the password

        Path(cfg["local_dir"]).mkdir(parents=True, exist_ok=True)

        extensions = tuple(
            e.strip().lower() if e.strip().startswith(".") else "." + e.strip().lower()
            for e in cfg["extensions"].split(",")
            if e.strip()
        )

        self.cancel_flag.clear()
        self.pull_btn.configure(state="disabled")
        self.cancel_btn.configure(state="normal")
        self.progress.configure(value=0, mode="indeterminate")
        self.progress.start(10)
        self.status_var.set("Connecting...")

        self.worker_thread = threading.Thread(
            target=self._worker,
            args=(cfg, port, password, extensions),
            daemon=True,
        )
        self.worker_thread.start()

    def _cancel_pull(self):
        self.cancel_flag.set()
        self._log("Cancel requested...")

    def _worker(self, cfg, port, password, extensions):
        client = None
        try:
            client = paramiko.SSHClient()
            client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            self.msg_queue.put(("status", f"Connecting to {cfg['host']}:{port} as {cfg['username']}..."))
            client.connect(
                hostname=cfg["host"],
                port=port,
                username=cfg["username"],
                password=password if password else None,
                timeout=15,
            )
            sftp = client.open_sftp()
            self.msg_queue.put(("log", "Connected. Scanning remote directory..."))

            remote_files = self._collect_remote_files(sftp, cfg["remote_dir"], cfg["recursive"], extensions)
            total = len(remote_files)
            self.msg_queue.put(("log", f"Found {total} matching file(s)."))
            self.msg_queue.put(("progress_mode", total))

            downloaded, skipped, failed = 0, 0, 0
            local_root = Path(cfg["local_dir"])

            for idx, remote_path in enumerate(remote_files, start=1):
                if self.cancel_flag.is_set():
                    self.msg_queue.put(("log", "Cancelled by user."))
                    break

                rel = os.path.relpath(remote_path, cfg["remote_dir"])
                local_path = local_root / rel
                local_path.parent.mkdir(parents=True, exist_ok=True)

                try:
                    remote_stat = sftp.stat(remote_path)
                    already_matches = local_path.exists() and local_path.stat().st_size == remote_stat.st_size
                    if not cfg["overwrite"] and already_matches:
                        skipped += 1
                        self.msg_queue.put(("log", f"[skip] {rel}"))
                    else:
                        sftp.get(remote_path, str(local_path))
                        downloaded += 1
                        self.msg_queue.put(("log", f"[ok]   {rel}"))
                except Exception as exc:  # noqa: BLE001 - report and continue
                    failed += 1
                    self.msg_queue.put(("log", f"[fail] {rel}: {exc}"))

                self.msg_queue.put(("progress_value", idx))
                self.msg_queue.put(("status", f"{idx}/{total} processed — {downloaded} downloaded, {skipped} skipped, {failed} failed"))

            sftp.close()
            self.msg_queue.put(("log", f"Done. Downloaded {downloaded}, skipped {skipped}, failed {failed}."))
            self.msg_queue.put(("status", "Done."))
        except Exception as exc:  # noqa: BLE001 - surfaced to the user
            self.msg_queue.put(("log", f"ERROR: {exc}"))
            self.msg_queue.put(("status", "Failed."))
        finally:
            if client is not None:
                client.close()
            self.msg_queue.put(("finished", None))

    def _collect_remote_files(self, sftp, remote_dir, recursive, extensions):
        matches = []
        stack = [remote_dir]
        while stack:
            current = stack.pop()
            try:
                entries = sftp.listdir_attr(current)
            except IOError as exc:
                self.msg_queue.put(("log", f"Cannot list {current}: {exc}"))
                continue
            for entry in entries:
                full_path = current.rstrip("/") + "/" + entry.filename
                if stat.S_ISDIR(entry.st_mode):
                    if recursive:
                        stack.append(full_path)
                    continue
                if not extensions or full_path.lower().endswith(extensions):
                    matches.append(full_path)
        return sorted(matches)

    # ---------------------------------------------------------- queue polling
    def _poll_queue(self):
        try:
            while True:
                kind, payload = self.msg_queue.get_nowait()
                if kind == "log":
                    self._log(payload)
                elif kind == "status":
                    self.status_var.set(payload)
                elif kind == "progress_mode":
                    self.progress.stop()
                    self.progress.configure(mode="determinate", maximum=max(payload, 1), value=0)
                elif kind == "progress_value":
                    self.progress.configure(value=payload)
                elif kind == "finished":
                    self.pull_btn.configure(state="normal")
                    self.cancel_btn.configure(state="disabled")
                    self.progress.stop()
        except queue.Empty:
            pass
        self.root.after(100, self._poll_queue)


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    ImagePullerApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
