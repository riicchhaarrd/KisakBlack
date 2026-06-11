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

    // Pick the Steam folder, choosing the best API for the page's context:
    //   * showDirectoryPicker() — needs a SECURE CONTEXT (https or localhost).
    //   * <input webkitdirectory> — works over plain HTTP/LAN (no secure context),
    //     so it's the fallback when the page is served from http://<lan-ip>.
    // Both build the same normalized-path index (handles vs File objects).
    async pickFolder() {
      // ?nofsapi=1 forces the <input webkitdirectory> path: automation (Playwright)
      // can intercept its file chooser, while showDirectoryPicker cannot be scripted.
      const noFsApi = new URLSearchParams(location.search).get("nofsapi") === "1";
      if (!noFsApi && window.isSecureContext && window.showDirectoryPicker) {
        try { return await this.pick(); }
        catch (e) { if (e && e.name === "AbortError") throw e;
                    console.warn("[KBFS] showDirectoryPicker failed; falling back to <input webkitdirectory>", e); }
      }
      return await this.pickViaInput();
    },

    // Let the user pick the Steam game folder and build the path index (secure ctx).
    async pick() {
      this.rootHandle = await window.showDirectoryPicker({ id: "kisak-blackops", mode: "read" });
      this.rootName = this.rootHandle.name;
      this.index.clear(); this.dirs.clear();
      await this._walk(this.rootHandle, "");
      this.ready = true;
      console.log(`[KBFS] indexed ${this.index.size} files under "${this.rootName}"`);
      return this.index.size;
    },

    // Insecure-context fallback (LAN/HTTP): a directory <input>. The browser hands
    // back a FileList whose .webkitRelativePath is "<picked-folder>/sub/file"; we
    // strip the leading folder segment so keys match _walk's relative convention.
    // File objects stream the same way (Blob.slice) — multi-GB is never resident.
    pickViaInput() {
      return new Promise((resolve, reject) => {
        const input = document.createElement("input");
        input.type = "file";
        input.webkitdirectory = true;
        input.multiple = true;
        input.style.display = "none";
        input.addEventListener("change", () => {
          const files = input.files;
          input.remove();
          if (!files || !files.length) { reject(new Error("no folder selected")); return; }
          this.index.clear(); this.dirs.clear(); this.rootName = "";
          for (const f of files) {
            const parts = (f.webkitRelativePath || f.name).split("/");
            if (!this.rootName && parts.length > 1) this.rootName = parts[0];
            const key = (parts.length > 1 ? parts.slice(1).join("/") : parts[0]).toLowerCase();
            this.index.set(key, f);
            const kp = key.split("/");
            for (let i = 1; i < kp.length; i++) this.dirs.add(kp.slice(0, i).join("/"));
          }
          this.ready = true;
          console.log(`[KBFS] indexed ${this.index.size} files (webkitdirectory) under "${this.rootName}"`);
          resolve(this.index.size);
        });
        document.body.appendChild(input);   // must run inside the pickFolder() user gesture
        input.click();
      });
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
      const entry = this._resolve(np);
      if (!entry) return 0;
      // entry is a FileSystemFileHandle (secure-ctx path) OR already a File
      // (webkitdirectory fallback). Both yield a File we can Blob.slice() lazily.
      const file = (typeof entry.getFile === "function") ? await entry.getFile() : entry;
      const id = this.nextId++;
      // Per-file 256KB block cache: the engine parses each .iwd's zip central
      // directory with thousands of tiny sequential reads; without caching that's
      // thousands of async round-trips. One block read serves them all.
      this.open_.set(id, { file, cache: new Map(), blockSize: 262144 });
      return id;
    },

    async size(id) {
      const e = this.open_.get(id);
      return e ? e.file.size : -1;
    },

    // Synchronous fast path: return the slice ONLY if every covering block is
    // already cached (and it's not a bulk read), else null. The C dispatcher
    // calls this first (no Asyncify), and only falls back to async pread on a
    // miss — so the engine's millions of tiny in-block reads cost ~nothing.
    preadCached(id, offset, len) {
      const e = this.open_.get(id);
      if (!e) return null;
      const size = e.file.size;
      const end = Math.min(offset + len, size);
      if (end <= offset) return new Uint8Array(0);
      const BS = e.blockSize;
      if (end - offset > BS) return null;                 // bulk -> async
      const out = new Uint8Array(end - offset);
      for (let pos = offset; pos < end; ) {
        const bi = Math.floor(pos / BS), bStart = bi * BS;
        const blk = e.cache.get(bi);
        if (!blk) return null;                            // miss -> async
        const from = pos - bStart, n = Math.min(blk.length - from, end - pos);
        out.set(blk.subarray(from, from + n), pos - offset);
        pos += n;
      }
      return out;
    },

    // Return a Uint8Array of the [offset, offset+len) slice (clamped to EOF).
    // Small/clustered reads go through the block cache; large bulk reads (a whole
    // fastfile region) bypass it and stream directly (caching wouldn't help).
    async pread(id, offset, len) {
      const e = this.open_.get(id);
      if (!e) return null;
      const size = e.file.size;
      const end = Math.min(offset + len, size);
      if (end <= offset) return new Uint8Array(0);

      if ((this._reads = (this._reads || 0) + 1) % 5000 === 0)
        console.log(`[KBFS] ${this._reads} reads served`);

      const BS = e.blockSize;
      if (end - offset > BS)
        return new Uint8Array(await e.file.slice(offset, end).arrayBuffer());

      const out = new Uint8Array(end - offset);
      for (let pos = offset; pos < end; ) {
        const bi = Math.floor(pos / BS), bStart = bi * BS;
        let blk = e.cache.get(bi);
        if (blk) { e.cache.delete(bi); e.cache.set(bi, blk); }   // LRU touch
        else {
          blk = new Uint8Array(await e.file.slice(bStart, Math.min(bStart + BS, size)).arrayBuffer());
          e.cache.set(bi, blk);
          if (e.cache.size > 8) e.cache.delete(e.cache.keys().next().value);
        }
        const from = pos - bStart, n = Math.min(blk.length - from, end - pos);
        out.set(blk.subarray(from, from + n), pos - offset);
        pos += n;
      }
      return out;
    },

    close(id) { this.open_.delete(id); },

    // Directory listing for Sys_ListFiles — synchronous against the prebuilt
    // index (the game data lives here, not in MEMFS). Mirrors the engine's
    // Sys_ListFiles semantics:
    //   * filter set    -> recursive; wildcard-match the path RELATIVE to dir;
    //                      return relative paths.
    //   * ext === "/"   -> return immediate subdirectory names.
    //   * else          -> immediate child files ending ".ext" (plus immediate
    //                      subdir names if wantsubs); return base names.
    listDir(dir, ext, filter, wantsubs) {
      let d = String(dir || "").replace(/\\/g, "/")
                .replace(/^\.?\/+/, "").replace(/\/+$/, "").toLowerCase();
      const prefix = d ? d + "/" : "";
      if (filter) {
        const rx = new RegExp("^" +
          String(filter).toLowerCase().replace(/[.+^${}()|[\]\\]/g, "\\$&")
            .replace(/\*/g, ".*").replace(/\?/g, ".") + "$");
        const out = [];
        for (const k of this.index.keys()) {
          if (prefix && !k.startsWith(prefix)) continue;
          const rel = prefix ? k.slice(prefix.length) : k;
          if (rx.test(rel)) out.push(rel);
        }
        return out;
      }
      const files = []; const dirs = new Set();
      for (const k of this.index.keys()) {
        if (prefix && !k.startsWith(prefix)) continue;
        const rel = prefix ? k.slice(prefix.length) : k;
        const slash = rel.indexOf("/");
        if (slash < 0) files.push(rel); else dirs.add(rel.slice(0, slash));
      }
      if (ext === "/") return [...dirs];
      const e = (ext || "").toLowerCase();
      let out = e ? files.filter(n => n.toLowerCase().endsWith("." + e)) : files.slice();
      if (wantsubs) out = out.concat([...dirs]);
      return out;
    },
  };

  // Expose on the Emscripten Module (created by blackops.js).
  if (typeof Module === "undefined") { window.Module = {}; }
  Module.KBFS = KBFS;
  window.KBFS = KBFS; // also reachable for the picker button
})();
