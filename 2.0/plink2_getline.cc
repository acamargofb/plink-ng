// This library is part of PLINK 2.00, copyright (C) 2005-2019 Shaun Purcell,
// Christopher Chang.
//
// This library is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This library is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this library.  If not, see <http://www.gnu.org/licenses/>.

#include <errno.h>
#include "plink2_getline.h"

#ifdef __cplusplus
namespace plink2 {
#endif

PglErr GetFileType(const char* fname, FileCompressionType* ftype_ptr) {
  FILE* infile = fopen(fname, FOPEN_RB);
  if (unlikely(!infile)) {
    // Note that this does not print an error message (since it may be called
    // by a worker thread).
    return kPglRetOpenFail;
  }
  unsigned char buf[16];
  const uint32_t nbytes = fread_unlocked(buf, 1, 16, infile);
  if (unlikely(ferror_unlocked(infile) || fclose(infile))) {
    return kPglRetReadFail;
  }
  if (nbytes < 4) {
    *ftype_ptr = kFileUncompressed;
    return kPglRetSuccess;
  }
  uint32_t magic4;
  memcpy(&magic4, buf, 4);

  if (IsZstdFrame(magic4)) {
    *ftype_ptr = kFileZstd;
    return kPglRetSuccess;
  }
  if (S_CAST(uint16_t, magic4) != 0x8b1f) { // gzip ID1/ID2 bytes
    *ftype_ptr = kFileUncompressed;
    return kPglRetSuccess;
  }
  if ((nbytes == 16) && IsBgzfHeader(buf)) {
    *ftype_ptr = kFileBgzf;
  } else {
    *ftype_ptr = kFileGzip;
  }
  return kPglRetSuccess;
}

void EraseTextRfileBase(TextRfileBase* trbp) {
  trbp->consume_iter = nullptr;
  trbp->consume_stop = nullptr;
  trbp->errmsg = nullptr;
  trbp->reterr = kPglRetEof;
  trbp->ff = nullptr;
  trbp->dst = nullptr;
}

void PreinitTextRfile(textRFILE* trfp) {
  EraseTextRfileBase(&trfp->base);
}

BoolErr GzRawInit(const void* buf, uint32_t nbytes, GzRawDecompressStream* gzp) {
  gzp->ds_initialized = 0;
  gzp->in = S_CAST(unsigned char*, malloc(kDecompressChunkSizeX));
  if (!gzp->in) {
    return 1;
  }
  z_stream* dsp = &gzp->ds;
  memcpy(gzp->in, buf, nbytes);
  dsp->next_in = gzp->in;
  dsp->avail_in = nbytes;
  dsp->zalloc = nullptr;
  dsp->zfree = nullptr;
  dsp->opaque = nullptr;
  if (unlikely(inflateInit2(dsp, MAX_WBITS | 16) != Z_OK)) {
    return 1;
  }
  gzp->ds_initialized = 1;
  return 0;
}

BoolErr ZstRawInit(const void* buf, uint32_t nbytes, ZstRawDecompressStream* zstp) {
  zstp->ib.src = malloc(kDecompressChunkSizeX);
  if (unlikely(!zstp->ib.src)) {
    zstp->ds = nullptr;
    return 1;
  }
  zstp->ds = ZSTD_createDStream();
  if (unlikely(!zstp->ds)) {
    return 1;
  }
  memcpy(K_CAST(void*, zstp->ib.src), buf, nbytes);
  zstp->ib.size = nbytes;
  zstp->ib.pos = 0;
  return 0;
}

const char kShortErrRfileAlreadyOpen[] = "TextRfileOpenInternal can't be called on an already-open file";
const char kShortErrRfileEnforcedMaxBlenTooSmall[] = "TextRfileOpenInternal: enforced_max_line_blen too small (must be at least max(1 MiB, dst_capacity - 1 MiB))";
const char kShortErrRfileDstCapacityTooSmall[] = "TextRfileOpenInternal: dst_capacity too small (2 MiB minimum)";

PglErr TextRfileOpenInternal(const char* fname, uint32_t enforced_max_line_blen, uint32_t dst_capacity, char* dst, textRFILE* trfp, TextRstream* trsp) {
  PglErr reterr = kPglRetSuccess;
  TextRfileBase* trbp;
  if (trfp) {
    trbp = &trfp->base;
  } else {
    trbp = &trsp->base;
  }
  {
    // 1. Open file, get type.
    if (unlikely(trbp->ff)) {
      reterr = kPglRetImproperFunctionCall;
      trbp->errmsg = kShortErrRfileAlreadyOpen;
      goto TextRfileOpenInternal_ret_1;
    }
    if (enforced_max_line_blen || trfp) {
      if (unlikely(enforced_max_line_blen < kDecompressChunkSizeX)) {
        reterr = kPglRetImproperFunctionCall;
        trbp->errmsg = kShortErrRfileEnforcedMaxBlenTooSmall;
        goto TextRfileOpenInternal_ret_1;
      }
      if (dst) {
        if (unlikely(dst_capacity < 2 * kDecompressChunkSizeX)) {
          reterr = kPglRetImproperFunctionCall;
          trbp->errmsg = kShortErrRfileDstCapacityTooSmall;
          goto TextRfileOpenInternal_ret_1;
        }
        if (unlikely(enforced_max_line_blen + kDecompressChunkSizeX < dst_capacity)) {
          reterr = kPglRetImproperFunctionCall;
          trbp->errmsg = kShortErrRfileEnforcedMaxBlenTooSmall;
          goto TextRfileOpenInternal_ret_1;
        }
      }
    } else {
      // token-reading mode.  dst == nullptr not currently supported.
      assert(dst && (dst_capacity == kTokenRstreamBlen));
    }
    trbp->ff = fopen(fname, FOPEN_RB);
    if (unlikely(!trbp->ff)) {
      goto TextRfileOpenInternal_ret_OPEN_FAIL;
    }
    trbp->file_type = kFileUncompressed;
    if (dst) {
      trbp->dst_owned_by_consumer = 1;
      trbp->dst_capacity = dst_capacity;
    } else {
      dst = S_CAST(char*, malloc(2 * kDecompressChunkSizeX));
      if (unlikely(dst == nullptr)) {
        goto TextRfileOpenInternal_ret_NOMEM;
      }
      trbp->dst_owned_by_consumer = 0;
      trbp->dst_capacity = 2 * kDecompressChunkSizeX;
    }
    trbp->dst = dst;
    uint32_t nbytes = fread_unlocked(dst, 1, 16, trbp->ff);
    trbp->dst_len = nbytes;
    trbp->enforced_max_line_blen = enforced_max_line_blen;
    trbp->consume_iter = dst;
    trbp->consume_stop = dst;
    if (nbytes >= 4) {
      const uint32_t magic4 = *R_CAST(uint32_t*, dst);
      if (IsZstdFrame(magic4)) {
        trbp->dst_len = 0;
        trbp->file_type = kFileZstd;
        ZstRawDecompressStream* zstp;
        if (trfp) {
          zstp = &trfp->rds.zst;
        } else {
          zstp = &trsp->rds.zst;
        }
        if (unlikely(ZstRawInit(dst, nbytes, zstp))) {
          goto TextRfileOpenInternal_ret_NOMEM;
        }
      } else if ((magic4 << 8) == 0x088b1f00) {
        // gzip ID1/ID2 bytes, deflate compression method
        trbp->dst_len = 0;
        if ((nbytes == 16) && IsBgzfHeader(dst)) {
          trbp->file_type = kFileBgzf;
          if (trfp) {
            BgzfRawDecompressStream* bgzfp = &trfp->rds.bgzf;
            bgzfp->in = S_CAST(unsigned char*, malloc(kDecompressChunkSizeX));
            if (unlikely(!bgzfp->in)) {
              bgzfp->ldc = nullptr;
              goto TextRfileOpenInternal_ret_NOMEM;
            }
            bgzfp->ldc = libdeflate_alloc_decompressor();
            if (!bgzfp->ldc) {
              goto TextRfileOpenInternal_ret_NOMEM;
            }
            memcpy(bgzfp->in, dst, nbytes);
            bgzfp->in_size = nbytes;
            bgzfp->in_pos = 0;
          } else {
            reterr = BgzfRawMtStreamInit(dst, trsp->decompress_thread_ct, trbp->ff, nullptr, &trsp->rds.bgzf, &trbp->errmsg);
            if (unlikely(reterr)) {
              goto TextRfileOpenInternal_ret_1;
            }
          }
        } else {
          trbp->file_type = kFileGzip;
          GzRawDecompressStream* gzp;
          if (trfp) {
            gzp = &trfp->rds.gz;
          } else {
            gzp = &trsp->rds.gz;
          }
          if (unlikely(GzRawInit(dst, nbytes, gzp))) {
            goto TextRfileOpenInternal_ret_NOMEM;
          }
        }
      }
    } else if (!nbytes) {
      if (unlikely(!feof_unlocked(trbp->ff))) {
        goto TextRfileOpenInternal_ret_READ_FAIL;
      }
      // May as well accept this.
      // Don't jump to ret_1 since we're setting trfp->reterr to a different
      // value than we're returning.
      trbp->reterr = kPglRetEof;
      return kPglRetSuccess;
    }
  }
  while (0) {
  TextRfileOpenInternal_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  TextRfileOpenInternal_ret_OPEN_FAIL:
    reterr = kPglRetOpenFail;
    trbp->errmsg = strerror(errno);
    break;
  TextRfileOpenInternal_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    trbp->errmsg = strerror(errno);
    break;
  }
 TextRfileOpenInternal_ret_1:
  trbp->reterr = reterr;
  return reterr;
}

PglErr TextRfileOpenEx(const char* fname, uint32_t enforced_max_line_blen, uint32_t dst_capacity, char* dst, textRFILE* trfp) {
  return TextRfileOpenInternal(fname, enforced_max_line_blen, dst_capacity, dst, trfp, nullptr);
}

// Set enforced_max_line_blen == 0 in the token-reading case.
// trailing 'X' is temporary, to avoid duplicate-symbol error
BoolErr IsPathologicallyLongLineOrTokenX(const char* line_start, const char* load_start, const char* known_line_end, uint32_t enforced_max_line_blen) {
  if (enforced_max_line_blen) {
    // Preconditions:
    // * No \n in [line_start, load_start).
    // * (known_line_end - load_start) is usually <= enforced_max_line_blen,
    //   and never much larger.  Not a hard requirement, but it's better to
    //   enforce the line-length limit during line iteration outside this
    //   regime to avoid duplicating work.
    if (S_CAST(uintptr_t, known_line_end - line_start) <= enforced_max_line_blen) {
      return 0;
    }
    const uint32_t already_scanned_byte_ct = load_start - line_start;
    if (unlikely(already_scanned_byte_ct >= enforced_max_line_blen)) {
      return 1;
    }
    const char* memchr_result = S_CAST(const char*, memchr(load_start, '\n', enforced_max_line_blen - already_scanned_byte_ct));
    if (unlikely(!memchr_result)) {
      return 1;
    }
    // If we've found a line with terminal \n at or after this address, there
    // are <= enforced_max_line_blen bytes left, so no remaining line can be
    // longer.
    const char* memchr_result_thresh = known_line_end - (enforced_max_line_blen + 1);
    while (1) {
      if (memchr_result >= memchr_result_thresh) {
        return 0;
      }
      memchr_result = S_CAST(const char*, memchr(&(memchr_result[1]), '\n', enforced_max_line_blen));
      if (unlikely(!memchr_result)) {
        return 1;
      }
    }
  }
  if (S_CAST(uintptr_t, known_line_end - line_start) <= kMaxTokenBlenX) {
    return 0;
  }
  const uint32_t already_scanned_byte_ct = load_start - line_start;
  if (unlikely(already_scanned_byte_ct >= kMaxTokenBlenX)) {
    return 1;
  }
  // No loop needed for now, since token-scanning buffer sizes are hardcoded.
  //
  // Replace with a forward-scanning version of this functionality when
  // available ("FirstPostspaceBoundedFar"?)
  return (LastSpaceOrEoln(load_start, kMaxTokenBlenX - already_scanned_byte_ct) == nullptr);
}

const char kShortErrRfileTruncatedGz[] = "GzRawStreamRead: gzipped file appears to be truncated";

PglErr GzRawStreamRead(char* dst_end, FILE* ff, GzRawDecompressStream* gzp, char** dst_iterp, const char** errmsgp) {
  z_stream* dsp = &gzp->ds;
  if ((!dsp->avail_in) && feof_unlocked(ff)) {
    return kPglRetSuccess;
  }
  char* dst_iter = *dst_iterp;
  do {
    int zerr = Z_OK;
    if (dsp->avail_in) {  // can be zero after TextRewind()
      dsp->next_out = R_CAST(unsigned char*, dst_iter);
      dsp->avail_out = dst_end - dst_iter;
      zerr = inflate(dsp, Z_SYNC_FLUSH);
      if (unlikely((zerr < 0) || (zerr == Z_NEED_DICT))) {
        if (dsp->msg) {
          *errmsgp = dsp->msg;
        } else {
          *errmsgp = zError(zerr);
        }
        return kPglRetDecompressFail;
      }
      dst_iter = R_CAST(char*, dsp->next_out);
      if (dsp->avail_in) {
        assert(dst_iter == dst_end);
        break;
      }
    }
    const uint32_t nbytes = fread_unlocked(gzp->in, 1, kDecompressChunkSizeX, ff);
    dsp->next_in = gzp->in;
    dsp->avail_in = nbytes;
    if (!nbytes) {
      if (unlikely(!feof_unlocked(ff))) {
        *errmsgp = strerror(errno);
        return kPglRetReadFail;
      }
      if (unlikely(zerr == Z_OK)) {
        *errmsgp = kShortErrRfileTruncatedGz;
        return kPglRetDecompressFail;
      }
      // Normal EOF.
      break;
    }
  } while (dst_iter != dst_end);
  *dst_iterp = dst_iter;
  return kPglRetSuccess;
}

PglErr ZstRawStreamRead(char* dst_end, FILE* ff, ZstRawDecompressStream* zstp, char** dst_iterp, const char** errmsgp) {
  if ((!zstp->ib.size) && feof_unlocked(ff)) {
    return kPglRetSuccess;
  }
  // Sequentially dependent blocks limited to ~128 KiB.
  char* dst_iter = *dst_iterp;
  while (1) {
    ZSTD_outBuffer zob = {R_CAST(unsigned char*, dst_iter), S_CAST(size_t, dst_end - dst_iter), 0};
    // ib.size == 0 ok, no need to special-case rewind.
    const uintptr_t read_size_hint = ZSTD_decompressStream(zstp->ds, &zob, &zstp->ib);
    if (unlikely(ZSTD_isError(read_size_hint))) {
      *errmsgp = ZSTD_getErrorName(read_size_hint);
      return kPglRetDecompressFail;
    }
    dst_iter = &(dst_iter[zob.pos]);
    if (dst_iter == dst_end) {
      break;
    }
    // Decoder has flushed everything it could.  Either we're at EOF, or we
    // must load more.
    unsigned char* in = S_CAST(unsigned char*, K_CAST(void*, zstp->ib.src));
    const uint32_t n_inbytes = zstp->ib.size - zstp->ib.pos;
    memmove(in, &(in[zstp->ib.pos]), n_inbytes);
    unsigned char* load_start = &(in[n_inbytes]);
    const uint32_t nbytes = fread_unlocked(load_start, 1, kDecompressChunkSizeX - n_inbytes, ff);
    if (unlikely(ferror_unlocked(ff))) {
      *errmsgp = strerror(errno);
      return kPglRetReadFail;
    }
    zstp->ib.pos = 0;
    zstp->ib.size = nbytes + n_inbytes;
    if (!nbytes) {
      if (unlikely(n_inbytes)) {
        *errmsgp = kShortErrZstdPrefixUnknown;
        return kPglRetDecompressFail;
      }
      break;
    }
  }
  *dst_iterp = dst_iter;
  return kPglRetSuccess;
}

const char kShortErrLongLine[] = "Pathologically long line";

PglErr TextRfileAdvance(textRFILE* trfp) {
  if (trfp->base.reterr) {
    return trfp->base.reterr;
  }
  PglErr reterr = kPglRetSuccess;
  {
    char* orig_line_start = trfp->base.consume_stop;
    assert(trfp->base.consume_iter == orig_line_start);
    char* dst = trfp->base.dst;
    char* dst_load_start;
    while (1) {
      const uint32_t dst_offset = orig_line_start - dst;
      const uint32_t dst_rem = trfp->base.dst_len - dst_offset;
      // (dst_rem guaranteed to be < trfp->base.enforced_max_line_blen here,
      // since otherwise we error out earlier.)
      // Two cases:
      // 1. Move (possibly empty) unfinished line to the beginning of the
      //    buffer.
      // 2. Resize the buffer/report out-of-memory.
      if (dst_rem < trfp->base.dst_capacity - kDecompressChunkSizeX) {
        memmove(dst, orig_line_start, dst_rem);
      } else {
        if (unlikely(trfp->base.dst_owned_by_consumer)) {
          goto TextRfileAdvance_ret_NOMEM;
        }
        uint32_t next_dst_capacity = trfp->base.enforced_max_line_blen + kDecompressChunkSizeX;
        if ((next_dst_capacity / 2) > trfp->base.dst_capacity) {
          next_dst_capacity = trfp->base.dst_capacity * 2;
        }
#ifndef __LP64__
        if (next_dst_capacity >= 0x80000000U) {
          goto TextRfileAdvance_ret_NOMEM;
        }
#endif
        char* dst_next;
        if (!dst_offset) {
          dst_next = S_CAST(char*, realloc(dst, next_dst_capacity));
          if (unlikely(!dst_next)) {
            goto TextRfileAdvance_ret_NOMEM;
          }
        } else {
          dst_next = S_CAST(char*, malloc(next_dst_capacity));
          if (unlikely(!dst_next)) {
            goto TextRfileAdvance_ret_NOMEM;
          }
          memcpy(dst_next, orig_line_start, dst_rem);
        }
        trfp->base.dst = dst_next;
        dst = dst_next;
      }
      dst_load_start = &(dst[dst_rem]);
      FILE* ff = trfp->base.ff;
      char* dst_iter = dst_load_start;
      char* dst_end = &(dst[trfp->base.dst_capacity]);
      trfp->base.consume_iter = dst;
      switch (trfp->base.file_type) {
      case kFileUncompressed:
        {
          uint32_t rlen = dst_end - dst_iter;
          if (rlen > kMaxBytesPerIO) {
            // We need to know how many bytes were read, so fread_checked()
            // doesn't work.
            // This is an if-statement instead of a while loop since rlen can
            // never be larger than 2 * kMaxBytesPerIO.
            const uint32_t nbytes = fread_unlocked(dst_iter, 1, kMaxBytesPerIO, ff);
            if (nbytes < kMaxBytesPerIO) {
              if (unlikely(ferror_unlocked(ff))) {
                goto TextRfileAdvance_ret_READ_FAIL;
              }
              trfp->base.dst_len = nbytes + dst_rem;
              break;
            }
            rlen -= kMaxBytesPerIO;
            dst_iter = &(dst_iter[kMaxBytesPerIO]);
          }
          const uint32_t nbytes = fread_unlocked(dst_iter, 1, rlen, ff);
          if (unlikely(ferror_unlocked(ff))) {
            goto TextRfileAdvance_ret_READ_FAIL;
          }
          dst_iter = &(dst_iter[nbytes]);
          break;
        }
      case kFileGzip:
        {
          reterr = GzRawStreamRead(dst_end, ff, &trfp->rds.gz, &dst_iter, &trfp->base.errmsg);
          if (unlikely(reterr)) {
            goto TextRfileAdvance_ret_1;
          }
          break;
        }
      case kFileBgzf:
        {
          // Fully independent blocks limited to 64 KiB.
          // probable todo: move this to a BgzfRawStreamRead() function in
          // plink2_bgzf (and move ZstRawStreamRead() to plink2_zstfile).
          if ((!trfp->rds.bgzf.in_size) && feof_unlocked(ff)) {
            break;
          }
          struct libdeflate_decompressor* ldc = trfp->rds.bgzf.ldc;
          unsigned char* in = trfp->rds.bgzf.in;
          unsigned char* in_iter = &(in[trfp->rds.bgzf.in_pos]);
          unsigned char* in_end = &(in[trfp->rds.bgzf.in_size]);
          while (1) {
            uint32_t n_inbytes = in_end - in_iter;
            if (n_inbytes > 25) {
              if (unlikely(!IsBgzfHeader(in_iter))) {
                goto TextRfileAdvance_ret_INVALID_BGZF;
              }
#  ifdef __arm__
#    error "Unaligned accesses in TextRfileAdvance()."
#  endif
              const uint32_t bsize_minus1 = *R_CAST(uint16_t*, &(in_iter[16]));
              if (unlikely(bsize_minus1 < 25)) {
                goto TextRfileAdvance_ret_INVALID_BGZF;
              }
              if (bsize_minus1 < n_inbytes) {
                // We have at least one fully-loaded compressed block.
                // Decompress it if we have enough space.
                const uint32_t in_size = bsize_minus1 - 25;
                const uint32_t out_size = *R_CAST(uint32_t*, &(in_iter[in_size + 22]));
                if (unlikely(out_size > 65536)) {
                  goto TextRfileAdvance_ret_INVALID_BGZF;
                }
                if (out_size > S_CAST(uintptr_t, dst_end - dst_iter)) {
                  break;
                }
                if (unlikely(libdeflate_deflate_decompress(ldc, &(in_iter[18]), in_size, dst_iter, out_size, nullptr))) {
                  goto TextRfileAdvance_ret_INVALID_BGZF;
                }
                in_iter = &(in_iter[bsize_minus1 + 1]);
                dst_iter = &(dst_iter[out_size]);
                continue;
              }
            }
            // Either we're at EOF, or we must load more.
            memmove(in, in_iter, n_inbytes);
            unsigned char* load_start = &(in[n_inbytes]);
            const uint32_t nbytes = fread_unlocked(load_start, 1, kDecompressChunkSizeX - n_inbytes, ff);
            if (unlikely(ferror_unlocked(ff))) {
              goto TextRfileAdvance_ret_READ_FAIL;
            }
            in_iter = in;
            in_end = &(load_start[nbytes]);
            trfp->rds.bgzf.in_size = in_end - in;
            if (!nbytes) {
              if (unlikely(n_inbytes)) {
                goto TextRfileAdvance_ret_INVALID_BGZF;
              }
              break;
            }
          }
          trfp->rds.bgzf.in_pos = in_iter - in;
          dst_end = dst_iter;
          break;
        }
      case kFileZstd:
        {
          reterr = ZstRawStreamRead(dst_end, ff, &trfp->rds.zst, &dst_iter, &trfp->base.errmsg);
          if (unlikely(reterr)) {
            goto TextRfileAdvance_ret_1;
          }
          break;
        }
      }
      trfp->base.dst_len = dst_iter - dst;
      if (!trfp->base.dst_len) {
        goto TextRfileAdvance_ret_EOF;
      }
      if (dst_iter != dst_end) {
        // If last character of file isn't a newline, append one to simplify
        // downstream code.
        if (dst_iter[-1] != '\n') {
          *dst_iter++ = '\n';
          trfp->base.dst_len += 1;
        }
        trfp->base.consume_stop = dst_iter;
        break;
      }
      char* last_byte_ptr = Memrchr(dst_load_start, '\n', dst_iter - dst_load_start);
      if (last_byte_ptr) {
        trfp->base.consume_stop = &(last_byte_ptr[1]);
        break;
      }
      // Buffer is full, and no '\n' is present.  Restart the loop and try to
      // extend the buffer, if we aren't already at/past the line-length limit.
      if (trfp->base.dst_len >= trfp->base.enforced_max_line_blen) {
        goto TextRfileAdvance_ret_LONG_LINE;
      }
    }
    if (unlikely(IsPathologicallyLongLineOrTokenX(dst, dst_load_start, trfp->base.consume_stop, trfp->base.enforced_max_line_blen))) {
      goto TextRfileAdvance_ret_LONG_LINE;
    }
  }
  while (0) {
  TextRfileAdvance_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  TextRfileAdvance_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    trfp->base.errmsg = strerror(errno);
    break;
  TextRfileAdvance_ret_LONG_LINE:
    trfp->base.errmsg = kShortErrLongLine;
    reterr = kPglRetMalformedInput;
    break;
  TextRfileAdvance_ret_INVALID_BGZF:
    trfp->base.errmsg = kShortErrInvalidBgzf;
    reterr = kPglRetDecompressFail;
    break;
  TextRfileAdvance_ret_EOF:
    reterr = kPglRetEof;
    break;
  }
 TextRfileAdvance_ret_1:
  trfp->base.reterr = reterr;
  return reterr;
}

void TextRfileRewind(textRFILE* trfp) {
  if ((!trfp->base.ff) || ((trfp->base.reterr) && (trfp->base.reterr != kPglRetEof))) {
    return;
  }
  rewind(trfp->base.ff);
  trfp->base.reterr = kPglRetSuccess;
  trfp->base.dst_len = 0;
  trfp->base.consume_iter = trfp->base.dst;
  trfp->base.consume_stop = trfp->base.dst;
  if (trfp->base.file_type != kFileUncompressed) {
    if (trfp->base.file_type == kFileGzip) {
      trfp->rds.gz.ds.avail_in = 0;
#ifdef NDEBUG
      inflateReset(&trfp->rds.gz.ds);
#else
      const int errcode = inflateReset(&trfp->rds.gz.ds);
      assert(errcode == Z_OK);
#endif
    } else if (trfp->base.file_type == kFileBgzf) {
      trfp->rds.bgzf.in_size = 0;
      trfp->rds.bgzf.in_pos = 0;
    } else {
      // kFileZstd
      trfp->rds.zst.ib.size = 0;
      trfp->rds.zst.ib.pos = 0;
      ZSTD_DCtx_reset(trfp->rds.zst.ds, ZSTD_reset_session_only);
    }
  }
}

BoolErr CleanupTextRfile(textRFILE* trfp, PglErr* reterrp) {
  trfp->base.consume_iter = nullptr;
  trfp->base.consume_stop = nullptr;
  trfp->base.reterr = kPglRetEof;
  trfp->base.errmsg = nullptr;
  if (trfp->base.dst && (!trfp->base.dst_owned_by_consumer)) {
    free(trfp->base.dst);
    trfp->base.dst = nullptr;
  }
  if (trfp->base.ff) {
    if (trfp->base.file_type != kFileUncompressed) {
      if (trfp->base.file_type == kFileZstd) {
        if (trfp->rds.zst.ib.src) {
          free_const(trfp->rds.zst.ib.src);
          trfp->rds.zst.ib.src = nullptr;
        }
        if (trfp->rds.zst.ds) {
          ZSTD_freeDStream(trfp->rds.zst.ds);
          trfp->rds.zst.ds = nullptr;
        }
      } else if (trfp->base.file_type == kFileBgzf) {
        if (trfp->rds.bgzf.in) {
          free(trfp->rds.bgzf.in);
          trfp->rds.bgzf.in = nullptr;
        }
        if (trfp->rds.bgzf.ldc) {
          libdeflate_free_decompressor(trfp->rds.bgzf.ldc);
          trfp->rds.bgzf.ldc = nullptr;
        }
      } else {
        // plain gzip
        if (trfp->rds.gz.in) {
          free(trfp->rds.gz.in);
          trfp->rds.gz.in = nullptr;
        }
        if (trfp->rds.gz.ds_initialized) {
          inflateEnd(&trfp->rds.gz.ds);
        }
      }
    }
    if (unlikely(fclose_null(&trfp->base.ff))) {
      if (!reterrp) {
        return 1;
      }
      if (*reterrp == kPglRetSuccess) {
        *reterrp = kPglRetReadFail;
        return 1;
      }
    }
  }
  return 0;
}


void PreinitTextRstream(TextRstream* trsp) {
  EraseTextRfileBase(&trsp->base);
  trsp->syncp = nullptr;
}

// This type of code is especially bug-prone (ESR would call it a "defect
// attractor").  Goal is to get it right, and fast enough to be a major win
// over gzgets()... and then not worry about it again for years.
THREAD_FUNC_DECL TextRstreamThread(void* raw_arg) {
  TextRstream* context = S_CAST(TextRstream*, raw_arg);
  TextRstreamSync* syncp = context->syncp;
  FileCompressionType file_type = context->base.file_type;
  RawMtDecompressStream* rdsp = &context->rds;
  FILE* ff = context->base.ff;
  char* buf = context->base.dst;
  char* buf_end = &(buf[context->base.dst_capacity]);
  char* cur_block_start = context->base.consume_stop;
  char* read_head = &(buf[context->base.dst_len]);

  // We can either be reading/decompressing into memory past the bytes passed
  // to the consumer, or we can be doing it before those bytes.
  // In the first case, read_stop is buf_end, but it gets changed to the
  // latest value of consume_tail when we return to the front of the buffer.
  // In the second case, read_stop is the position of the first passed byte.
  char* read_stop = buf_end;
#ifdef _WIN32
  CRITICAL_SECTION* critical_sectionp = &syncp->critical_section;
  HANDLE reader_progress_event = syncp->reader_progress_event;
  HANDLE consumer_progress_event = syncp->consumer_progress_event;
#else
  pthread_mutex_t* sync_mutexp = &syncp->sync_mutex;
  pthread_cond_t* reader_progress_condvarp = &syncp->reader_progress_condvar;
  pthread_cond_t* consumer_progress_condvarp = &syncp->consumer_progress_condvar;
#endif
  const uint32_t enforced_max_line_blen = context->base.enforced_max_line_blen;
  const char* new_fname = nullptr;
  const uint32_t is_token_stream = (enforced_max_line_blen == 0);
  while (1) {
    TrsInterrupt interrupt = kTrsInterruptNone;
    PglErr reterr;
    TrsInterrupt min_interrupt;
    while (1) {
      uintptr_t read_attempt_size = read_stop - read_head;
      if (!read_attempt_size) {
        const uint32_t memmove_required = (read_stop == buf_end);
        if (unlikely((cur_block_start == buf) && memmove_required)) {
          // May need to modify this predicate if we ever allow is_token_stream
          // && !dst_owned_by_consumer.
          const uint32_t prev_capacity = buf_end - buf;
          if (context->base.dst_owned_by_consumer || (prev_capacity >= enforced_max_line_blen)) {
            goto TextRstreamThread_LONG_LINE;
          }
          // Try to expand buffer.
          uint32_t next_dst_capacity = enforced_max_line_blen + kDecompressChunkSizeX;
          if ((next_dst_capacity / 2) > context->base.dst_capacity) {
            next_dst_capacity = context->base.dst_capacity * 2;
          }
#ifndef __LP64__
          if (next_dst_capacity >= 0x80000000U) {
            goto TextRstreamThread_NOMEM;
          }
#endif
          char* dst_next = S_CAST(char*, realloc(buf, next_dst_capacity));
          if (unlikely(!dst_next)) {
            goto TextRstreamThread_NOMEM;
          }
#ifdef _WIN32
          EnterCriticalSection(critical_sectionp);
#else
          pthread_mutex_lock(sync_mutexp);
#endif
          context->base.dst = dst_next;
          context->base.dst_capacity = next_dst_capacity;
          syncp->consume_tail = dst_next;
          syncp->available_end = dst_next;
          syncp->dst_reallocated = 1;
#ifdef _WIN32
          LeaveCriticalSection(critical_sectionp);
#else
          pthread_mutex_unlock(sync_mutexp);
#endif
          buf = dst_next;
          buf_end = &(buf[next_dst_capacity]);
          cur_block_start = buf;
          read_head = &(buf[prev_capacity]);
          read_stop = &(buf[next_dst_capacity]);
          continue;
        }
        // We cannot continue reading forward.  Cases:
        // 1. read_stop == buf_end, cur_block_start != buf.  This means we're
        //    in the middle of reading/decompressing a long line, and want to
        //    wait for consume_tail == cur_block_start, so we can memmove all
        //    the bytes back and continue reading forward.  (Tried
        //    relaxing this to
        //      consume_tail >= (buf_end - cur_block_start) + margin
        //    for various values of margin, but that didn't make a meaningful
        //    difference.)
        // 2. read_stop == buf_end, cur_block_start == buf.  We failed with a
        //    long-line error here.
        // 3. read_stop < buf_end (usual case).  This means the consumer may
        //    not be done handling some bytes-in-front we handed off earlier.
        //    We are waiting for consume_tail <= cur_block_start, which means
        //    all bytes in front have been consumed and we're free to continue
        //    reading forward.
        char* latest_consume_tail;
#ifdef _WIN32
        // bugfix (7 May 2018): when consumer thread is waiting with
        // syncp->consume_tail == cur_block_start, read_stop is near but not at
        // buf_end, and there's no '\n' in the subsequent read, we can reach
        // here a second time without releasing the consumer, so we'd enter
        // deadlock if we unconditionally wait on consumer_progress_event (and
        // in the Linux/OS X case, we'd be waiting for a spurious wakeup to
        // save us).
        // However, if memmove_required isn't true, we have to wait first; see
        // the 21 Mar bugfix.
        if (!memmove_required) {
          goto TextRstreamThread_wait_first;
        }
        while (1) {
          EnterCriticalSection(critical_sectionp);
          interrupt = syncp->interrupt;
          if (interrupt != kTrsInterruptNone) {
            goto TextRstreamThread_INTERRUPT;
          }
          latest_consume_tail = syncp->consume_tail;
          if (memmove_required) {
            if (latest_consume_tail == cur_block_start) {
              syncp->consume_tail = buf;
              syncp->available_end = buf;
              break;
            }
          } else if (latest_consume_tail <= cur_block_start) {
            break;
          }
          LeaveCriticalSection(critical_sectionp);
        TextRstreamThread_wait_first:
          WaitForSingleObject(consumer_progress_event, INFINITE);
        }
        // bugfix (23 Mar 2018): didn't always leave the critical section
        LeaveCriticalSection(critical_sectionp);
#else
        pthread_mutex_lock(sync_mutexp);
        if (!memmove_required) {
          // Wait for all bytes in front of read_stop to be consumed.
          goto TextRstreamThread_wait_first;
        }
        while (1) {
          interrupt = syncp->interrupt;
          if (interrupt != kTrsInterruptNone) {
            goto TextRstreamThread_INTERRUPT;
          }
          latest_consume_tail = syncp->consume_tail;
          if (memmove_required) {
            if (latest_consume_tail == cur_block_start) {
              // All bytes have been consumed; memmove is now safe.
              // bugfix (2 Oct 2018): Previously, this just set
              // syncp->cur_circular_end = cur_block_start, but that created
              // TWO consume_iter == available_end == cur_circular_end cases,
              // one of which was handled incorrectly.
              syncp->consume_tail = buf;
              syncp->available_end = buf;
              break;
            }
            // There are bytes behind cur_block_start that haven't been
            // consumed yet.  This is possible on the first iteration through
            // the loop, since consumer_progress_state may have been set for a
            // reason we aren't interested in.

          } else if (latest_consume_tail <= cur_block_start) {
            // All bytes in front of read_stop have been consumed.
            break;
          }
        TextRstreamThread_wait_first:
          while (!syncp->consumer_progress_state) {
            pthread_cond_wait(consumer_progress_condvarp, sync_mutexp);
          }
          syncp->consumer_progress_state = 0;
        }
        pthread_mutex_unlock(sync_mutexp);
#endif
        if (read_stop == buf_end) {
          const uint32_t cur_memmove_len = buf_end - cur_block_start;
          memmove(buf, cur_block_start, cur_memmove_len);
          cur_block_start = buf;
          read_head = &(buf[cur_memmove_len]);
        } else {
          read_stop = buf_end;
        }
        continue;
      }
      if (read_attempt_size > kDecompressChunkSizeX) {
        read_attempt_size = kDecompressChunkSizeX;
      }
      char* cur_read_end = read_head;
      char* cur_read_stop = &(read_head[read_attempt_size]);
      switch (file_type) {
      case kFileUncompressed:
        {
          cur_read_end += fread_unlocked(read_head, 1, read_attempt_size, ff);
          if (unlikely(ferror_unlocked(ff))) {
            goto TextRstreamThread_READ_FAIL;
          }
          break;
        }
      case kFileGzip:
        {
          reterr = GzRawStreamRead(cur_read_stop, ff, &rdsp->gz, &cur_read_end, &syncp->errmsg);
          if (unlikely(reterr)) {
            goto TextRstreamThread_MISC_FAIL;
          }
          break;
        }
      case kFileBgzf:
        {
          ;;;
          reterr = BgzfRawMtStreamRead(R_CAST(unsigned char*, cur_read_stop), &rdsp->bgzf, R_CAST(unsigned char**, &cur_read_end), &syncp->errmsg);
          if (unlikely(reterr)) {
            goto TextRstreamThread_MISC_FAIL;
          }
          break;
        }
      case kFileZstd:
        {
          reterr = ZstRawStreamRead(cur_read_stop, ff, &rdsp->zst, &cur_read_end, &syncp->errmsg);
          if (unlikely(reterr)) {
            goto TextRstreamThread_MISC_FAIL;
          }
          break;
        }
      }
      if (cur_read_end < cur_read_stop) {
        char* final_read_head = cur_read_end;
        if (cur_block_start != final_read_head) {
          if (final_read_head[-1] != '\n') {
            // Append '\n' so consumer can always use rawmemchr(., '\n') to
            // find the end of the current line.
            *final_read_head++ = '\n';
          }
        }
        // Still want to consistently enforce max line/token length.
        if (unlikely(IsPathologicallyLongLineOrTokenX(cur_block_start, read_head, final_read_head, enforced_max_line_blen))) {
          goto TextRstreamThread_LONG_LINE;
        }
        read_head = final_read_head;
        goto TextRstreamThread_EOF;
      }
      char* last_byte_ptr;
      if (!is_token_stream) {
        last_byte_ptr = Memrchr(read_head, '\n', read_attempt_size);
      } else {
        last_byte_ptr = LastSpaceOrEoln(read_head, read_attempt_size);
      }
      if (last_byte_ptr) {
        char* next_available_end = &(last_byte_ptr[1]);
        if (unlikely(IsPathologicallyLongLineOrTokenX(cur_block_start, read_head, next_available_end, enforced_max_line_blen))) {
          goto TextRstreamThread_LONG_LINE;
        }
#ifdef _WIN32
        EnterCriticalSection(critical_sectionp);
#else
        pthread_mutex_lock(sync_mutexp);
#endif
        interrupt = syncp->interrupt;
        if (interrupt != kTrsInterruptNone) {
          goto TextRstreamThread_INTERRUPT;
        }
        char* latest_consume_tail = syncp->consume_tail;
        const uint32_t all_later_bytes_consumed = (latest_consume_tail <= cur_block_start);
        const uint32_t return_to_start = all_later_bytes_consumed && (latest_consume_tail >= &(buf[kDecompressChunkSizeX]));
        if (return_to_start) {
          // bugfix (2 Oct 2018): This was previously setting
          // syncp->available_end = next_available_end too, and that was being
          // handled as a special case which conflicted with a rare legitimate
          // case.
          syncp->cur_circular_end = next_available_end;
          syncp->available_end = buf;
        } else {
          syncp->available_end = next_available_end;
        }
#ifdef _WIN32
        // bugfix (23 Mar 2018): this needs to be in the critical section,
        // otherwise there's a danger of this resetting legitimate progress
        ResetEvent(consumer_progress_event);
        SetEvent(reader_progress_event);
        LeaveCriticalSection(critical_sectionp);
#else
        // bugfix (21 Mar 2018): must force consumer_progress_state to 0 (or
        // ResetEvent(consumer_progress_event); otherwise the other wait loop's
        // read_stop = buf_end assignment may occur before all later bytes are
        // actually consumed, in the next_available_end == latest_consume_tail
        // edge case.
        syncp->consumer_progress_state = 0;
        pthread_cond_signal(reader_progress_condvarp);
        pthread_mutex_unlock(sync_mutexp);
#endif
        if (return_to_start) {
          // Best to return to the beginning of the buffer.
          // (Note that read_attempt_size is guaranteed to be
          // <= kDecompressChunkSize.)
          const uintptr_t trailing_byte_ct = cur_read_end - next_available_end;
          memcpy(buf, next_available_end, trailing_byte_ct);
          cur_block_start = buf;
          read_head = &(buf[trailing_byte_ct]);
          // May as well reduce false sharing risk.
          read_stop = R_CAST(char*, RoundDownPow2(R_CAST(uintptr_t, latest_consume_tail), kCacheline));
          continue;
        }
        if (all_later_bytes_consumed) {
          read_stop = buf_end;
        } else {
          read_stop = R_CAST(char*, RoundDownPow2(R_CAST(uintptr_t, latest_consume_tail), kCacheline));
        }
        cur_block_start = next_available_end;
      }
      read_head = cur_read_end;
    }
    while (0) {
    TextRstreamThread_NOMEM:
      min_interrupt = kTrsInterruptShutdown;
      reterr = kPglRetNomem;
      break;
    TextRstreamThread_OPEN_FAIL:
      min_interrupt = kTrsInterruptShutdown;
      syncp->errmsg = strerror(errno);
      reterr = kPglRetOpenFail;
      break;
    TextRstreamThread_READ_FAIL:
      min_interrupt = kTrsInterruptShutdown;
      syncp->errmsg = strerror(errno);
      reterr = kPglRetReadFail;
      break;
    TextRstreamThread_LONG_LINE:
      min_interrupt = kTrsInterruptShutdown;
      syncp->errmsg = kShortErrLongLine;
      reterr = kPglRetMalformedInput;
      break;
    TextRstreamThread_EOF:
      min_interrupt = kTrsInterruptRetarget;
      reterr = kPglRetEof;
      break;
    TextRstreamThread_MISC_FAIL:
      min_interrupt = kTrsInterruptShutdown;
      break;
    }
    // We need to wait for a message from the consumer before we can usefully
    // proceed.
    // More precisely:
    // * In the eof subcase, we're waiting for either a rewind or shutdown
    //   request.
    // * In the error subcase, we're just waiting for the shutdown request.

    // Pass the error code back.
#ifdef _WIN32
    EnterCriticalSection(critical_sectionp);
#else
    pthread_mutex_lock(sync_mutexp);
#endif
    syncp->reterr = reterr;
    interrupt = syncp->interrupt;
    if (interrupt >= min_interrupt) {
      // It's our lucky day: we don't need to wait again.
      goto TextRstreamThread_INTERRUPT;
    }
    if (reterr == kPglRetEof) {
      syncp->available_end = read_head;
    }
#ifdef _WIN32
    SetEvent(reader_progress_event);
    LeaveCriticalSection(critical_sectionp);
    while (1) {
      WaitForSingleObject(consumer_progress_event, INFINITE);
      EnterCriticalSection(critical_sectionp);
      interrupt = syncp->interrupt;
      if (interrupt >= min_interrupt) {
        break;
      }
      LeaveCriticalSection(critical_sectionp);
    }
#else
    pthread_cond_signal(reader_progress_condvarp);
    do {
      while (!syncp->consumer_progress_state) {
        pthread_cond_wait(consumer_progress_condvarp, sync_mutexp);
      }
      syncp->consumer_progress_state = 0;
      interrupt = syncp->interrupt;
    } while (interrupt < min_interrupt);
#endif
  TextRstreamThread_INTERRUPT:
    // must be in critical section here, or be holding the mutex.
    if (interrupt == kTrsInterruptRetarget) {
      new_fname = syncp->new_fname;
      syncp->interrupt = kTrsInterruptNone;
      syncp->reterr = kPglRetSuccess;
    }
#ifdef _WIN32
    LeaveCriticalSection(critical_sectionp);
#else
    pthread_mutex_unlock(sync_mutexp);
#endif
    if (interrupt == kTrsInterruptShutdown) {
      // possible todo: close the file here
      THREAD_RETURN;
    }
    assert(interrupt == kTrsInterruptRetarget);
    read_head = buf;
    if (!new_fname) {
      if (file_type == kFileBgzf) {
        reterr = BgzfRawMtStreamRewind(&rdsp->bgzf, &syncp->errmsg);
        if (unlikely(reterr)) {
          goto TextRstreamThread_MISC_FAIL;
        }
      } else {
        // See TextRfileRewind().
        rewind(ff);
        if (file_type != kFileUncompressed) {
          if (file_type == kFileGzip) {
            rdsp->gz.ds.avail_in = 0;
#ifdef NDEBUG
            inflateReset(&rdsp->gz.ds);
#else
            const int errcode = inflateReset(&rdsp->gz.ds);
            assert(errcode == Z_OK);
#endif
          } else {
            // kFileZstd
            rdsp->zst.ib.size = 0;
            rdsp->zst.ib.pos = 0;
            ZSTD_DCtx_reset(rdsp->zst.ds, ZSTD_reset_session_only);
          }
        }
      }
    } else {
      // Switch to another file, with less creation/destruction of resources.
      FILE* next_ff = fopen(new_fname, FOPEN_RB);
      if (unlikely(!next_ff)) {
        goto TextRstreamThread_OPEN_FAIL;
      }
      // See TextRfileOpenInternal().
      uint32_t nbytes = fread_unlocked(buf, 1, 16, next_ff);
      FileCompressionType next_file_type = kFileUncompressed;
      if (nbytes >= 4) {
        const uint32_t magic4 = *R_CAST(uint32_t*, buf);
        if (IsZstdFrame(magic4)) {
          next_file_type = kFileZstd;
        } else if ((magic4 << 8) == 0x088b1f00) {
          if ((nbytes == 16) && IsBgzfHeader(buf)) {
            next_file_type = kFileBgzf;
          } else {
            next_file_type = kFileGzip;
          }
        }
      }
      if (file_type != next_file_type) {
        // Destroy old type-specific resources, and allocate new ones.
        if (file_type == kFileGzip) {
          free(rdsp->gz.in);
          inflateEnd(&rdsp->gz.ds);
        } else if (file_type == kFileBgzf) {
          CleanupBgzfRawMtStream(&rdsp->bgzf);
        } else if (file_type == kFileZstd) {
          free_const(rdsp->zst.ib.src);
          ZSTD_freeDStream(rdsp->zst.ds);
        }

        if (unlikely(fclose(ff))) {
          fclose(next_ff);
          goto TextRstreamThread_READ_FAIL;
        }
        ff = next_ff;
        context->base.ff = ff;
        file_type = next_file_type;
        context->base.file_type = file_type;
        switch (file_type) {
        case kFileUncompressed:
          read_head = &(read_head[nbytes]);
          break;
        case kFileGzip:
          if (unlikely(GzRawInit(buf, nbytes, &rdsp->gz))) {
            goto TextRstreamThread_NOMEM;
          }
          break;
        case kFileBgzf:
          {
            reterr = BgzfRawMtStreamInit(buf, context->decompress_thread_ct, ff, nullptr, &rdsp->bgzf, &syncp->errmsg);
            if (unlikely(reterr)) {
              goto TextRstreamThread_MISC_FAIL;
            }
          }
        case kFileZstd:
          if (unlikely(ZstRawInit(buf, nbytes, &rdsp->zst))) {
            goto TextRstreamThread_NOMEM;
          }
        }
      } else {
        switch (file_type) {
        case kFileUncompressed:
          read_head = &(read_head[nbytes]);
          break;
          // Rest of this is similar to rewind.
        case kFileGzip:
          {
            GzRawDecompressStream* gzp = &rdsp->gz;
            z_stream* dsp = &gzp->ds;
#ifdef NDEBUG
            inflateReset(dsp);
#else
            const int errcode = inflateReset(dsp);
            assert(errcode == Z_OK);
#endif
            memcpy(gzp->in, buf, nbytes);
            dsp->next_in = gzp->in;
            dsp->avail_in = nbytes;
            break;
          }
        case kFileBgzf:
          {
            reterr = BgzfRawMtStreamRetarget(&rdsp->bgzf, next_ff, &syncp->errmsg);
            if (unlikely(reterr)) {
              fclose(next_ff);
              goto TextRstreamThread_MISC_FAIL;
            }
            break;
          }
        case kFileZstd:
          {
            ZstRawDecompressStream* zstp = &rdsp->zst;
            ZSTD_DCtx_reset(zstp->ds, ZSTD_reset_session_only);
            memcpy(K_CAST(void*, zstp->ib.src), buf, nbytes);
            zstp->ib.size = nbytes;
            zstp->ib.pos = 0;
            break;
          }
        }
        if (unlikely(fclose(ff))) {
          fclose(next_ff);
          goto TextRstreamThread_READ_FAIL;
        }
        ff = next_ff;
        context->base.ff = ff;
      }
    }
    cur_block_start = buf;
    read_stop = buf_end;
  }
}

const char kShortErrRfileInvalid[] = "TextRstreamOpenEx can't be called with a closed or error-state textRFILE";

PglErr TextRstreamOpenEx(const char* fname, uint32_t enforced_max_line_blen, uint32_t dst_capacity, uint32_t decompress_thread_ct, textRFILE* trfp, char* dst, TextRstream* trsp) {
  PglErr reterr = kPglRetSuccess;
  {
    trsp->decompress_thread_ct = decompress_thread_ct;
    if (trfp) {
      // Move-construct (unless there was an error, or file is not opened)
      if (unlikely((!TextRfileIsOpen(trfp)) || TextRfileErrcode(trfp))) {
        reterr = kPglRetImproperFunctionCall;
        trsp->base.errmsg = kShortErrRfileInvalid;
        goto TextRstreamOpenEx_ret_1;
      }
      if (unlikely(TextRstreamIsOpen(trsp))) {
        reterr = kPglRetImproperFunctionCall;
        trsp->base.errmsg = kShortErrRfileAlreadyOpen;
        goto TextRstreamOpenEx_ret_1;
      }
      trsp->base = trfp->base;
      // Simplify TextRstreamThread() initialization.
      const uint32_t backfill_ct = trsp->base.consume_iter - trsp->base.dst;
      if (backfill_ct) {
        trsp->base.dst_len -= backfill_ct;
        memmove(trsp->base.dst, trsp->base.consume_iter, trsp->base.dst_len);
        trsp->base.consume_iter = trsp->base.dst;
        trsp->base.consume_stop -= backfill_ct;
      }
      trsp->base.enforced_max_line_blen = enforced_max_line_blen;
      assert(trsp->base.dst_len <= dst_capacity);
      trsp->base.dst_capacity = dst_capacity;
      reterr = trfp->base.reterr;
      const FileCompressionType file_type = trfp->base.file_type;
      if (file_type != kFileUncompressed) {
        if (file_type == kFileGzip) {
          trsp->rds.gz = trfp->rds.gz;
        } else if (file_type == kFileZstd) {
          trsp->rds.zst = trfp->rds.zst;
        } else {
          reterr = BgzfRawMtStreamInit(nullptr, decompress_thread_ct, trsp->base.ff, &trfp->rds.bgzf, &trsp->rds.bgzf, &trsp->base.errmsg);
          if (unlikely(reterr)) {
            EraseTextRfileBase(&trfp->base);
            goto TextRstreamOpenEx_ret_1;
          }
        }
      }
      EraseTextRfileBase(&trfp->base);
    } else {
      reterr = TextRfileOpenInternal(fname, enforced_max_line_blen, dst_capacity, dst, nullptr, trsp);
    }
    if (reterr) {
      if (reterr == kPglRetEof) {
        trsp->base.reterr = kPglRetEof;
        return kPglRetSuccess;
      }
      goto TextRstreamOpenEx_ret_1;
    }
    assert(!trsp->syncp);
    TextRstreamSync* syncp;
    if (unlikely(cachealigned_malloc(RoundUpPow2(sizeof(TextRstreamSync), kCacheline), &syncp))) {
      goto TextRstreamOpenEx_ret_NOMEM;
    }
    trsp->syncp = syncp;
    dst = trsp->base.dst;
    syncp->consume_tail = dst;
    syncp->cur_circular_end = nullptr;
    syncp->available_end = trsp->base.consume_stop;
    syncp->errmsg = nullptr;
    syncp->reterr = kPglRetSuccess;
    syncp->dst_reallocated = 0;
    syncp->interrupt = kTrsInterruptNone;
    syncp->new_fname = nullptr;
#ifdef _WIN32
    syncp->read_thread = nullptr;
    // apparently this can raise a low-memory exception in older Windows
    // versions, but that's not really our problem.
    InitializeCriticalSection(&syncp->critical_section);

    syncp->reader_progress_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (unlikely(!syncp->reader_progress_event)) {
      DeleteCriticalSection(&syncp->critical_section);
      goto TextRstreamOpenEx_ret_THREAD_CREATE_FAIL;
    }
    syncp->consumer_progress_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (unlikely(!syncp->consumer_progress_event)) {
      DeleteCriticalSection(&syncp->critical_section);
      CloseHandle(syncp->reader_progress_event);
      goto TextRstreamOpenEx_ret_THREAD_CREATE_FAIL;
    }
    syncp->read_thread = R_CAST(HANDLE, _beginthreadex(nullptr, kDefaultThreadStackX, TextRstreamThread, trsp, 0, nullptr));
    if (unlikely(!syncp->read_thread)) {
      DeleteCriticalSection(&syncp->critical_section);
      CloseHandle(syncp->consumer_progress_event);
      CloseHandle(syncp->reader_progress_event);
      goto TextRstreamOpenEx_ret_THREAD_CREATE_FAIL;
    }
#else
    syncp->sync_init_state = 0;
    if (unlikely(pthread_mutex_init(&syncp->sync_mutex, nullptr))) {
      goto TextRstreamOpenEx_ret_THREAD_CREATE_FAIL;
    }
    syncp->sync_init_state = 1;
    if (unlikely(pthread_cond_init(&syncp->reader_progress_condvar, nullptr))) {
      goto TextRstreamOpenEx_ret_THREAD_CREATE_FAIL;
    }
    syncp->sync_init_state = 2;
    syncp->consumer_progress_state = 0;
    if (unlikely(pthread_cond_init(&syncp->consumer_progress_condvar, nullptr))) {
      goto TextRstreamOpenEx_ret_THREAD_CREATE_FAIL;
    }
    syncp->sync_init_state = 3;
    pthread_attr_t smallstack_thread_attr;
    pthread_attr_init(&smallstack_thread_attr);
    pthread_attr_setstacksize(&smallstack_thread_attr, kDefaultThreadStackX);
    if (unlikely(pthread_create(&syncp->read_thread, &smallstack_thread_attr, TextRstreamThread, trsp))) {
      pthread_attr_destroy(&smallstack_thread_attr);
      goto TextRstreamOpenEx_ret_THREAD_CREATE_FAIL;
    }
    pthread_attr_destroy(&smallstack_thread_attr);
    syncp->sync_init_state = 4;
#endif
  }
  while (0) {
  TextRstreamOpenEx_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  TextRstreamOpenEx_ret_THREAD_CREATE_FAIL:
    reterr = kPglRetThreadCreateFail;
    break;
  }
 TextRstreamOpenEx_ret_1:
  trsp->base.reterr = reterr;
  return reterr;
}

uint32_t TextDecompressThreadCt(const TextRstream* trsp) {
  FileCompressionType file_type = trsp->base.file_type;
  if (file_type == kFileUncompressed) {
    return 0;
  }
  if (file_type != kFileBgzf) {
    return 1;
  }
  return GetThreadCt(&trsp->rds.bgzf.tg);
}

PglErr TextAdvance(TextRstream* trsp) {
  char* consume_iter = trsp->base.consume_iter;
  TextRstreamSync* syncp = trsp->syncp;
#ifdef _WIN32
  CRITICAL_SECTION* critical_sectionp = &syncp->critical_section;
  HANDLE consumer_progress_event = syncp->consumer_progress_event;
  while (1) {
    EnterCriticalSection(critical_sectionp);
    const PglErr reterr = syncp->reterr;
    if (unlikely((reterr != kPglRetSuccess) && (reterr != kPglRetEof))) {
      trsp->base.errmsg = syncp->errmsg;
      LeaveCriticalSection(critical_sectionp);
      trsp->base.reterr = reterr;
      // No need to set consumer_progress event here, just let the cleanup
      // routine take care of that.
      return reterr;
    }
    char* available_end = syncp->available_end;
    char* cur_circular_end = syncp->cur_circular_end;
    if (consume_iter == cur_circular_end) {
      char* buf = trsp->base.dst;
      consume_iter = buf;
      trsp->base.consume_iter = buf;
      cur_circular_end = nullptr;
      syncp->cur_circular_end = nullptr;
      if (consume_iter != available_end) {
        SetEvent(consumer_progress_event);
      }
    }
    if (syncp->dst_reallocated) {
      consume_iter = trsp->base.dst;
      syncp->dst_reallocated = 0;
    }
    syncp->consume_tail = consume_iter;
    if ((consume_iter != available_end) || cur_circular_end) {
      if (cur_circular_end) {
        trsp->base.consume_stop = cur_circular_end;
      } else {
        trsp->base.consume_stop = available_end;
      }
      LeaveCriticalSection(critical_sectionp);
      // We could set the consumer_progress event here, but it's not really
      // necessary?
      // SetEvent(consumer_progress_event);
      return kPglRetSuccess;
    }
    SetEvent(consumer_progress_event);
    LeaveCriticalSection(critical_sectionp);
    // We've processed all the consume-ready bytes...
    if (reterr != kPglRetSuccess) {
      // ...and we're at eof.  Don't set consumer_progress event here; let that
      // wait until cleanup or rewind/retarget.
      trsp->base.reterr = kPglRetEof;
      return kPglRetEof;
    }
    // ...and there's probably more.
    WaitForSingleObject(syncp->reader_progress_event, INFINITE);
    // bugfix (2 Oct 2018)
    consume_iter = syncp->consume_tail;
    trsp->base.consume_iter = consume_iter;
  }
#else
  pthread_mutex_t* sync_mutexp = &syncp->sync_mutex;
  pthread_cond_t* consumer_progress_condvarp = &syncp->consumer_progress_condvar;
  pthread_cond_t* reader_progress_condvarp = &syncp->reader_progress_condvar;
  pthread_mutex_lock(sync_mutexp);
  while (1) {
    const PglErr reterr = syncp->reterr;
    if (unlikely((reterr != kPglRetSuccess) && (reterr != kPglRetEof))) {
      trsp->base.errmsg = syncp->errmsg;
      pthread_mutex_unlock(sync_mutexp);
      trsp->base.reterr = reterr;
      return reterr;
    }
    char* available_end = syncp->available_end;
    // bugfix (2 Oct 2018): There were TWO consume_iter == available_end ==
    // cur_circular_end cases.
    // printf("checking for more to consume: %lx %lx %lx\n", (uintptr_t)consume_iter, (uintptr_t)syncp->cur_circular_end, (uintptr_t)available_end);
    if (consume_iter == syncp->cur_circular_end) {
      char* buf = trsp->base.dst;
      consume_iter = buf;
      trsp->base.consume_iter = buf;
      syncp->cur_circular_end = nullptr;
      // File-reader could be waiting on either "all bytes in front have been
      // consumed, some bytes behind may remain" or "all bytes have been
      // consumed".  Signal in case it's the first.
      if (consume_iter != available_end) {
        syncp->consumer_progress_state = 1;
        pthread_cond_signal(consumer_progress_condvarp);
      }
    }
    if (syncp->dst_reallocated) {
      consume_iter = trsp->base.dst;
      syncp->dst_reallocated = 0;
    }
    syncp->consume_tail = consume_iter;
    // If cur_circular_end is still non-null here, there must be bytes
    // available even when consume_iter == available_end.  (Is the latter
    // still possible?  Check this.)
    if ((consume_iter != available_end) || syncp->cur_circular_end) {
      if (syncp->cur_circular_end) {
        trsp->base.consume_stop = syncp->cur_circular_end;
      } else {
        trsp->base.consume_stop = available_end;
      }
      // pthread_cond_signal(consumer_progress_condvarp);
      pthread_mutex_unlock(sync_mutexp);
      // printf("consuming %lx..%lx\n", (uintptr_t)(*consume_iterp), (uintptr_t)rlsp->consume_stop);
      return kPglRetSuccess;
    }
    // We've processed all the consume-ready bytes...
    if (reterr != kPglRetSuccess) {
      // ...and we're at eof.
      pthread_mutex_unlock(sync_mutexp);
      trsp->base.reterr = kPglRetEof;
      return kPglRetEof;
    }
    // ...and there's probably more.
    syncp->consumer_progress_state = 1;
    pthread_cond_signal(consumer_progress_condvarp);
    // no need for an explicit spurious-wakeup check, we'll check the progress
    // condition (available_end has advanced, or we have a read error) anyway
    // and get back here if it isn't satisfied
    pthread_cond_wait(reader_progress_condvarp, sync_mutexp);
    // bugfix (2 Oct 2018)
    consume_iter = syncp->consume_tail;
    trsp->base.consume_iter = syncp->consume_tail;
  }
#endif
}

PglErr TextNextNonemptyLineLstrip(TextRstream* trsp, uintptr_t* line_idx_ptr, char** line_startp) {
  uintptr_t line_idx = *line_idx_ptr;
  while (1) {
    ++line_idx;
    if (trsp->base.consume_iter == trsp->base.consume_stop) {
      PglErr reterr = TextAdvance(trsp);
      // not unlikely() due to eof
      if (reterr) {
        return reterr;
      }
    }
    char* line_start = FirstNonTspace(trsp->base.consume_iter);
    trsp->base.consume_iter = AdvPastDelim(line_start, '\n');
    if (!IsEolnKns(*line_start)) {
      *line_idx_ptr = line_idx;
      *line_startp = line_start;
      return kPglRetSuccess;
    }
  }
}

PglErr TextSkipNz(uintptr_t skip_ct, TextRstream* trsp) {
#ifdef __LP64__
  char* consume_iter = trsp->base.consume_iter;
  // Minor extension of AdvToNthDelimChecked().
  const VecUc vvec_all_lf = vecuc_set1('\n');
  while (1) {
    uintptr_t starting_addr = R_CAST(uintptr_t, consume_iter);
    VecUc* consume_viter = R_CAST(VecUc*, RoundDownPow2(starting_addr, kBytesPerVec));
    uintptr_t ending_addr = R_CAST(uintptr_t, trsp->base.consume_stop);
    VecUc* consume_vstop = R_CAST(VecUc*, RoundDownPow2(ending_addr, kBytesPerVec));
    VecUc cur_vvec = *consume_viter;
    VecUc lf_vvec = (cur_vvec == vvec_all_lf);
    uint32_t lf_bytes = vecuc_movemask(lf_vvec);
    const uint32_t leading_byte_ct = starting_addr - R_CAST(uintptr_t, consume_viter);
    const uint32_t leading_mask = UINT32_MAX << leading_byte_ct;
    lf_bytes &= leading_mask;
    uint32_t cur_lf_ct;
    for (; consume_viter != consume_vstop; ) {
      cur_lf_ct = PopcountVec8thUint(lf_bytes);
      if (cur_lf_ct >= skip_ct) {
        goto TextSkipNz_finish;
      }
      skip_ct -= cur_lf_ct;
      // bugfix (28 Sep 2019): forgot to update cur_vvec/lf_vvec/lf_bytes?!
      ++consume_viter;
      cur_vvec = *consume_viter;
      lf_vvec = (cur_vvec == vvec_all_lf);
      lf_bytes = vecuc_movemask(lf_vvec);
    }
    lf_bytes &= (1U << (ending_addr % kBytesPerVec)) - 1;
    cur_lf_ct = PopcountVec8thUint(lf_bytes);
    if (cur_lf_ct >= skip_ct) {
    TextSkipNz_finish:
      lf_bytes = ClearBottomSetBits(skip_ct - 1, lf_bytes);
      const uint32_t byte_offset_in_vec = ctzu32(lf_bytes) + 1;
      const uintptr_t result_addr = R_CAST(uintptr_t, consume_viter) + byte_offset_in_vec;
      trsp->base.consume_iter = R_CAST(char*, result_addr);
      return kPglRetSuccess;
    }
    skip_ct -= cur_lf_ct;
    trsp->base.consume_iter = consume_iter;
    PglErr reterr = TextAdvance(trsp);
    // not unlikely() due to eof
    if (reterr) {
      return reterr;
    }
    consume_iter = trsp->base.consume_iter;
  }
#else
  char* consume_iter = trsp->base.consume_iter;
  char* consume_stop = trsp->base.consume_stop;
  for (uintptr_t ulii = 0; ulii != skip_ct; ++ulii) {
    if (consume_iter == consume_stop) {
      trsp->base.consume_iter = consume_iter;
      PglErr reterr = TextAdvance(trsp);
      if (reterr) {
        return reterr;
      }
      consume_iter = trsp->base.consume_iter;
      consume_stop = trsp->base.consume_stop;
    }
    consume_iter = AdvPastDelim(consume_iter, '\n');
  }
  trsp->base.consume_iter = consume_iter;
  return kPglRetSuccess;
#endif
}

PglErr TextRetarget(const char* new_fname, TextRstream* trsp) {
  TextRstreamSync* syncp = trsp->syncp;
#ifdef _WIN32
  CRITICAL_SECTION* critical_sectionp = &syncp->critical_section;
  EnterCriticalSection(critical_sectionp);
  const PglErr reterr = syncp->reterr;
  if (reterr != kPglRetSuccess) {
    if (unlikely(reterr != kPglRetEof)) {
      trsp->base.errmsg = syncp->errmsg;
      LeaveCriticalSection(critical_sectionp);
      trsp->base.reterr = reterr;
      return reterr;
    }
    // clear eof
    syncp->reterr = kPglRetSuccess;
  }
  // bugfix (5 Mar 2018): need to reset these here, can't wait for reader
  // thread to receive signal
  char* buf = trsp->base.dst;
  syncp->consume_tail = buf;
  syncp->cur_circular_end = nullptr;
  syncp->available_end = buf;
  syncp->dst_reallocated = 0;
  syncp->interrupt = kTrsInterruptRetarget;
  // Could also just open the file in this function (before acquiring the
  // mutex) and pass a gzFile.  Advantages: nothing bad happens if new_fname
  // is overwritten before it's read, RLstreamErrPrint() no longer has to deal
  // with OpenFail error.  Disadvantage: peak resource usage is a bit higher if
  // we open the second file before closing the first one.  Advantages probably
  // outweigh disadvantages, but I'll wait till --pmerge development to make a
  // decision since that's the main function that actually cares.
  syncp->new_fname = new_fname;
  SetEvent(syncp->consumer_progress_event);
  LeaveCriticalSection(critical_sectionp);
#else
  pthread_mutex_t* sync_mutexp = &syncp->sync_mutex;
  pthread_cond_t* consumer_progress_condvarp = &syncp->consumer_progress_condvar;
  pthread_mutex_lock(sync_mutexp);
  const PglErr reterr = syncp->reterr;
  if (reterr != kPglRetSuccess) {
    if (unlikely(reterr != kPglRetEof)) {
      trsp->base.errmsg = syncp->errmsg;
      pthread_mutex_unlock(sync_mutexp);
      trsp->base.reterr = reterr;
      return reterr;
    }
    // clear eof
    syncp->reterr = kPglRetSuccess;
  }
  char* buf = trsp->base.dst;
  syncp->consume_tail = buf;
  syncp->cur_circular_end = nullptr;
  syncp->available_end = buf;
  syncp->dst_reallocated = 0;
  syncp->interrupt = kTrsInterruptRetarget;
  syncp->new_fname = new_fname;
  syncp->consumer_progress_state = 1;
  pthread_cond_signal(consumer_progress_condvarp);
  pthread_mutex_unlock(sync_mutexp);
#endif
  trsp->base.consume_iter = buf;
  trsp->base.consume_stop = buf;
  return kPglRetSuccess;
}

BoolErr CleanupTextRstream(TextRstream* trsp, PglErr* reterrp) {
  TextRstreamSync* syncp = trsp->syncp;
  if (syncp) {
#ifdef _WIN32
    if (syncp->read_thread) {
      CRITICAL_SECTION* critical_sectionp = &syncp->critical_section;
      EnterCriticalSection(critical_sectionp);
      syncp->interrupt = kTrsInterruptShutdown;
      SetEvent(syncp->consumer_progress_event);
      LeaveCriticalSection(critical_sectionp);
      WaitForSingleObject(syncp->read_thread, INFINITE);
      DeleteCriticalSection(critical_sectionp);
      CloseHandle(syncp->consumer_progress_event);
      CloseHandle(syncp->reader_progress_event);
    }
#else
    const uint32_t sync_init_state = syncp->sync_init_state;
    if (sync_init_state) {
      pthread_mutex_t* sync_mutexp = &syncp->sync_mutex;
      pthread_cond_t* consumer_progress_condvarp = &syncp->consumer_progress_condvar;
      if (sync_init_state == 4) {
        pthread_mutex_lock(sync_mutexp);
        syncp->interrupt = kTrsInterruptShutdown;
        syncp->consumer_progress_state = 1;
        pthread_cond_signal(consumer_progress_condvarp);
        pthread_mutex_unlock(sync_mutexp);
        pthread_join(syncp->read_thread, nullptr);
      }
      pthread_mutex_destroy(sync_mutexp);
      if (sync_init_state > 1) {
        pthread_cond_destroy(&syncp->reader_progress_condvar);
        if (sync_init_state > 2) {
          pthread_cond_destroy(consumer_progress_condvarp);
        }
      }
    }
#endif
    aligned_free(trsp->syncp);
    trsp->syncp = nullptr;
  }
  trsp->base.consume_iter = nullptr;
  trsp->base.consume_stop = nullptr;
  trsp->base.reterr = kPglRetEof;
  trsp->base.errmsg = nullptr;
  if (trsp->base.dst && (!trsp->base.dst_owned_by_consumer)) {
    free(trsp->base.dst);
    trsp->base.dst = nullptr;
  }
  if (trsp->base.ff) {
    if (trsp->base.file_type != kFileUncompressed) {
      if (trsp->base.file_type == kFileZstd) {
        if (trsp->rds.zst.ib.src) {
          free_const(trsp->rds.zst.ib.src);
          trsp->rds.zst.ib.src = nullptr;
        }
        if (trsp->rds.zst.ds) {
          ZSTD_freeDStream(trsp->rds.zst.ds);
          trsp->rds.zst.ds = nullptr;
        }
      } else if (trsp->base.file_type == kFileBgzf) {
        CleanupBgzfRawMtStream(&trsp->rds.bgzf);
      } else {
        // plain gzip
        if (trsp->rds.gz.in) {
          free(trsp->rds.gz.in);
          trsp->rds.gz.in = nullptr;
        }
        if (trsp->rds.gz.ds_initialized) {
          inflateEnd(&trsp->rds.gz.ds);
        }
      }
      trsp->base.file_type = kFileUncompressed;
    }
    if (unlikely(fclose_null(&trsp->base.ff))) {
      if (!reterrp) {
        return 1;
      }
      if (*reterrp == kPglRetSuccess) {
        // Note that we don't set trsp->base.reterr or .errmsg here.
        *reterrp = kPglRetReadFail;
        return 1;
      }
    }
  }
  return 0;
}

#ifdef __cplusplus
}
#endif