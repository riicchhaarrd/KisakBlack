// web_fs.js — Chrome File System Access bridge for KisakBlack (the JS half of
// src/platform/sdl/web_fs.cpp). Installed on Module.KBFS before the wasm runs.
//
// Responsibilities:
//   * showDirectoryPicker() on the user's local Steam "Call of Duty Black Ops"
//     folder, then recursively index every file path -> FileSystemFileHandle.
//   * Resolve engine paths case-insensitively (the game ships Windows-cased
//     paths on a case-sensitive index) and strip any base prefix.
//   * Stream reads: pread() materializes ONLY the requested [offset,len) slice
//     via Blob.slice(...).arrayBuffer() — a multi-GB .ff is never fully resident.
//   * A tiny LRU caches the File object per open id (File handles are cheap; the
//     bytes are read per-slice on demand).
//
// The C side calls open/size/pread/close via Asyncify (EM_ASYNC_JS), so these
// async methods present as blocking to the engine's synchronous file I/O.
(function () {
  const KBFS = {
    ready: false,
    rootHandle: null,
    // Normalized-path (lowercase, '/'-separated, no leading slash) -> handle.
    index: new Map(),
    dirs: new Set(),       // normalized directory paths (for GetFileAttributes)
    open_: new Map(),      // id -> { handle, file }
    nextId: 1,
    rootName: "",

    // Normalize an engine path for lookup: backslashes->slash, collapse, lowercase.
    norm(p) {
      if (!p) return "";
      let s = String(p).replace(/\\/g, "/");
      // Drop a leading "./" and any leading slash.
      s = s.replace(/^\.?\//, "");
      // The engine often prefixes the install dir / "main"/"zone" roots; we index
      // relative to the picked folder, so try the path as-is and a few tails.
      return s.toLowerCase();
    },

    // Let the user pick the Steam game folder and build the path index.
    async pick() {
      this.rootHandle = await window.showDirectoryPicker({ id: "kisak-blackops", mode: "read" });
      this.rootName = this.rootHandle.name;
      this.index.clear(); this.dirs.clear();
      await this._walk(this.rootHandle, "");
      this.ready = true;
      console.log(`[KBFS] indexed ${this.index.size} files under "${this.rootName}"`);
      return this.index.size;
    },

    async _walk(dirHandle, prefix) {
      for await (const [name, handle] of dirHandle.entries()) {
        const path = prefix ? prefix + "/" + name : name;
        if (handle.kind === "file") {
          this.index.set(path.toLowerCase(), handle);
        } else if (handle.kind === "directory") {
          this.dirs.add(path.toLowerCase());
          await this._walk(handle, path);
        }
      }
    },

    // Resolve a normalized path to a file handle, trying progressively shorter
    // tails so an absolute/prefixed engine path still matches the relative index.
    _resolve(np) {
      if (this.index.has(np)) return this.index.get(np);
      const parts = np.split("/");
      for (let i = 1; i < parts.length; i++) {
        const tail = parts.slice(i).join("/");
        if (this.index.has(tail)) return this.index.get(tail);
      }
      return null;
    },

    exists(p) {
      const np = this.norm(p);
      if (this._resolve(np)) return 1;
      if (this.dirs.has(np)) return 2;
      // directory tail match
      const parts = np.split("/");
      for (let i = 1; i < parts.length; i++)
        if (this.dirs.has(parts.slice(i).join("/"))) return 2;
      return 0;
    },

    async open(p) {
      const np = this.norm(p);
      const handle = this._resolve(np);
      if (!handle) return 0;
      const file = await handle.getFile();
      const id = this.nextId++;
      this.open_.set(id, { handle, file });
      return id;
    },

    async size(id) {
      const e = this.open_.get(id);
      return e ? e.file.size : -1;
    },

    // Return a Uint8Array of the [offset, offset+len) slice (clamped to EOF).
    async pread(id, offset, len) {
      const e = this.open_.get(id);
      if (!e) return null;
      const end = Math.min(offset + len, e.file.size);
      if (end <= offset) return new Uint8Array(0);
      const blob = e.file.slice(offset, end);
      const buf = await blob.arrayBuffer();
      return new Uint8Array(buf);
    },

    close(id) { this.open_.delete(id); },
  };

  // Expose on the Emscripten Module (created by blackops.js).
  if (typeof Module === "undefined") { window.Module = {}; }
  Module.KBFS = KBFS;
  window.KBFS = KBFS; // also reachable for the picker button
})();
