// ---- ProRes core host: instantiates fp.wasm with a minimal WASI shim ----
// readSync(offset, length) -> Uint8Array (synchronous; in the browser a worker using FileReaderSync)
function createCore(wasmModule, readSync, log) {
  let inst, mem;
  const u8 = () => new Uint8Array(mem.buffer), dv = () => new DataView(mem.buffer);
  const EBADF = 8, ENOSYS = 52;
  const wasi = {
    environ_sizes_get: (c, s) => { dv().setUint32(c, 0, true); dv().setUint32(s, 0, true); return 0; },
    environ_get: () => 0,
    clock_time_get: (id, prec, out) => { dv().setBigUint64(out, BigInt(Math.round(performance.now() * 1e6)), true); return 0; },
    fd_write: (fd, iovs, n, nw) => { let t = 0, s = ''; const d = dv(); for (let i = 0; i < n; i++) { const p = d.getUint32(iovs + i * 8, true), l = d.getUint32(iovs + i * 8 + 4, true); s += new TextDecoder().decode(u8().subarray(p, p + l)); t += l; } d.setUint32(nw, t, true); if (log && s.trim()) log(s.trim()); return 0; },
    fd_close: () => EBADF, fd_fdstat_get: () => EBADF, fd_fdstat_set_flags: () => EBADF, fd_prestat_get: () => EBADF, fd_prestat_dir_name: () => EBADF,
    fd_read: () => EBADF, fd_seek: () => EBADF, path_open: () => ENOSYS, poll_oneoff: () => ENOSYS,
    proc_exit: c => { throw new Error('wasm exit ' + c); },
  };
  const env = { js_read: (off, ptr, len) => { const b = readSync(off, len); u8().set(b, ptr); return b.length; } };
  inst = new WebAssembly.Instance(wasmModule, { wasi_snapshot_preview1: wasi, env });
  mem = inst.exports.memory; inst.exports._initialize();
  const X = inst.exports;
  let lutPtr = X.fp_malloc(4096), outPtr = 0, outCap = 0;
  const infoArr = () => new Int32Array(mem.buffer, X.fp_info(), 32);
  return {
    open(size) { const r = X.fp_open(size); if (r < 0) return { error: r }; const i = infoArr(); return { width: i[0], height: i[1], frames: i[2], tbNum: i[3], tbDen: i[4], fpsNum: i[5], fpsDen: i[6], primaries: i[7], trc: i[8], matrix: i[9], range: i[10], tag: i[11] }; },
    decode(idx) { const r = X.fp_decode(idx); if (r < 0) return { error: r }; const i = infoArr(); return { pixfmt: i[12], depth: i[13], range: i[16], colorspace: i[17], width: i[18], height: i[19] }; },
    intra() { return new Int32Array(mem.buffer, X.fp_info(), 32)[20]; },
    trim() { if (outPtr) { X.fp_free(outPtr); outPtr = 0; outCap = 0; } },
    // lut: Uint8Array(4096) for preview curves, or null for exports (pass-through + TPDF dither). Returns RGBA (copy).
    render(x, y, w, h, step, matrix, full, lut) {
      const ow = Math.floor(w / step), oh = Math.floor(h / step), need = ow * oh * 4;
      if (need > outCap) { if (outPtr) X.fp_free(outPtr); outPtr = X.fp_malloc(need); outCap = need; if (!outPtr) throw new Error('out of memory'); }
      if (lut) u8().set(lut, lutPtr);
      const n = X.fp_render(x, y, w, h, step, matrix, full, lut ? lutPtr : 0, outPtr);
      if (n < 0) throw new Error('render failed');
      return { w: ow, h: oh, data: new Uint8ClampedArray(mem.buffer.slice(outPtr, outPtr + need)) };
    },
    extradata() { const p = X.fp_extradata(), i = infoArr(); return { codec: i[22], data: i[21] ? new Uint8Array(mem.buffer.slice(p, p + i[21])) : null }; },
    // packets in decode order from the keyframe at/before frame n, until `count` packets past n (covers B-frame reordering)
    packets(n, after) {
      if (X.fp_pkt_seek(n) < 0) throw new Error('seek failed');
      const out = []; let past = 0;
      for (let g = 0; g < 2000; g++) {
        const p = X.fp_pkt_next(); if (!p) break;
        const i = infoArr();
        out.push({ data: new Uint8Array(mem.buffer.slice(p, p + i[25])), key: !!i[26], idx: i[27], dts: i[28] });
        if (i[28] > n || (i[28] < 0 && i[27] > n)) { if (++past > after) break; }
      }
      return out;
    },
    // render the rect straight into a JPEG inside wasm memory (no 35 MB round trip through JS)
    renderJpeg(x, y, w, h, matrix, full, quality) {
      const need = w * h * 4;
      if (need > outCap) { if (outPtr) X.fp_free(outPtr); outPtr = X.fp_malloc(need); outCap = need; if (!outPtr) throw new Error('out of memory'); }
      if (X.fp_render(x, y, w, h, 1, matrix, full, 0, outPtr) < 0) throw new Error('render failed');
      const p = X.fp_jpeg(outPtr, w, h, quality);
      if (!p) throw new Error('JPEG encode failed');
      const out = new Uint8Array(mem.buffer.slice(p, p + infoArr()[29])); X.fp_jpeg_free(); this.trim(); return out;
    },
    png16(x, y, w, h, matrix, full) {
      const p = X.fp_png16(x, y, w, h, matrix, full);
      if (!p) throw new Error('16-bit PNG encode failed (out of memory?)');
      const out = new Uint8Array(mem.buffer.slice(p, p + infoArr()[29])); X.fp_png16_free(); return out;
    },
    jpeg(rgba, w, h, quality) {
      const ip = X.fp_malloc(rgba.length); if (!ip) throw new Error('out of memory');
      u8().set(rgba, ip);
      const p = X.fp_jpeg(ip, w, h, quality); X.fp_free(ip);
      if (!p) throw new Error('JPEG encode failed');
      const out = new Uint8Array(mem.buffer.slice(p, p + infoArr()[29])); X.fp_jpeg_free(); return out;
    },
  };
}
// ---- end core ----
if (typeof module !== 'undefined') module.exports = { createCore };
