/*
 * nghttp2 - HTTP/2.0 C Library
 *
 * Copyright (c) 2013 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef NGHTTP2_HD_COMP_H
#define NGHTTP2_HD_COMP_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <nghttp2/nghttp2.h>

#define NGHTTP2_INITIAL_HD_TABLE_SIZE 128
#define NGHTTP2_INITIAL_EMIT_SET_SIZE 128

#define NGHTTP2_HD_MAX_BUFFER_SIZE 4096
#define NGHTTP2_HD_MAX_ENTRY_SIZE 3072
#define NGHTTP2_HD_ENTRY_OVERHEAD 32

/* This value is sensible to NGHTTP2_HD_MAX_BUFFER_SIZE. Currently,
   the index is at most 128, so 255 is good choice */
#define NGHTTP2_HD_INVALID_INDEX 255

typedef enum {
  NGHTTP2_HD_SIDE_CLIENT = 0,
  NGHTTP2_HD_SIDE_SERVER = 1
} nghttp2_hd_side;

typedef enum {
  NGHTTP2_HD_ROLE_DEFLATE,
  NGHTTP2_HD_ROLE_INFLATE
} nghttp2_hd_role;

typedef enum {
  NGHTTP2_HD_FLAG_NONE = 0,
  /* Indicates name was dynamically allocated and must be freed */
  NGHTTP2_HD_FLAG_NAME_ALLOC = 1,
  /* Indicates value was dynamically allocated and must be freed */
  NGHTTP2_HD_FLAG_VALUE_ALLOC = 1 << 1,
  /* Indicates that the entry is in the reference set */
  NGHTTP2_HD_FLAG_REFSET = 1 << 2,
  /* Indicates that the entry is emitted in the current header
     processing. */
  NGHTTP2_HD_FLAG_EMIT = 1 << 3,
  NGHTTP2_HD_FLAG_IMPLICIT_EMIT = 1 << 4
} nghttp2_hd_flags;

typedef struct {
  nghttp2_nv nv;
  /* Reference count */
  uint8_t ref;
  /* Index in the header table */
  uint8_t index;
  uint8_t flags;
} nghttp2_hd_entry;

typedef struct {
  /* Header table */
  nghttp2_hd_entry **hd_table;
  /* Holding emitted entry in deflating header block to retain
     reference count. */
  nghttp2_hd_entry **emit_set;
  /* The capacity of the |hd_table| */
  uint16_t hd_table_capacity;
  /* The number of entry the |hd_table| contains */
  uint16_t hd_tablelen;
  /* The capacity of the |emit_set| */
  uint16_t emit_set_capacity;
  /* The number of entry the |emit_set| contains */
  uint16_t emit_setlen;
  /* Abstract buffer size of hd_table as described in the spec. This
     is the sum of length of name/value in hd_table +
     NGHTTP2_HD_ENTRY_OVERHEAD bytes overhead per each entry. */
  uint16_t hd_table_bufsize;
  /* If inflate/deflate error occurred, this value is set to 1 and
     further invocation of inflate/deflate will fail with
     NGHTTP2_ERR_HEADER_COMP. */
  uint8_t bad;
  /* Role of this context; deflate or infalte */
  nghttp2_hd_role role;
} nghttp2_hd_context;

/*
 * Initializes the |ent| members. If NGHTTP2_HD_FLAG_NAME_ALLOC bit
 * set in the |flags|, the content pointed by the |name| with length
 * |namelen| is copied. Likewise, if NGHTTP2_HD_FLAG_VALUE_ALLOC bit
 * set in the |flags|, the content pointed by the |value| with length
 * |valuelen| is copied.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGHTTP2_ERR_NOMEM
 *     Out of memory.
 */
int nghttp2_hd_entry_init(nghttp2_hd_entry *ent, uint8_t index, uint8_t flags,
                          uint8_t *name, uint16_t namelen,
                          uint8_t *value, uint16_t valuelen);

void nghttp2_hd_entry_free(nghttp2_hd_entry *ent);

/*
 * Initializes |deflater| for deflating name/values pairs.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGHTTP2_ERR_NOMEM
 *     Out of memory.
 */
int nghttp2_hd_deflate_init(nghttp2_hd_context *deflater,
                            nghttp2_hd_side side);

/*
 * Initializes |inflater| for inflating name/values pairs.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGHTTP2_ERR_NOMEM
 *     Out of memory.
 */
int nghttp2_hd_inflate_init(nghttp2_hd_context *inflater,
                            nghttp2_hd_side side);

/*
 * Deallocates any resources allocated for |deflater|.
 */
void nghttp2_hd_deflate_free(nghttp2_hd_context *deflater);

/*
 * Deallocates any resources allocated for |inflater|.
 */
void nghttp2_hd_inflate_free(nghttp2_hd_context *inflater);

/*
 * Deflates the |nva|, which has the |nvlen| name/value pairs, into
 * the buffer pointed by the |*buf_ptr| with the length |*buflen_ptr|.
 * The output starts after |nv_offset| bytes from |*buf_ptr|.
 *
 * This function expands |*buf_ptr| as necessary to store the
 * result. When expansion occurred, memory previously pointed by
 * |*buf_ptr| may change.  |*buf_ptr| and |*buflen_ptr| are updated
 * accordingly.
 *
 * This function copies necessary data into |*buf_ptr|. After this
 * function returns, it is safe to delete the |nva|.
 *
 * TODO: The rest of the code call nghttp2_hd_end_headers() after this
 * call, but it is just a regacy of the first implementation. Now it
 * is not required to be called as of now.
 *
 * This function returns the number of bytes outputted if it succeeds,
 * or one of the following negative error codes:
 *
 * NGHTTP2_ERR_NOMEM
 *     Out of memory.
 * NGHTTP2_ERR_HEADER_COMP
 *     Deflation process has failed.
 */
ssize_t nghttp2_hd_deflate_hd(nghttp2_hd_context *deflater,
                              uint8_t **buf_ptr, size_t *buflen_ptr,
                              size_t nv_offset,
                              nghttp2_nv *nva, size_t nvlen);

/*
 * Inflates name/value block stored in |in| with length |inlen|. This
 * function performs decompression. The |*nva_ptr| points to the final
 * result on successful decompression. The caller must free |*nva_ptr|
 * using nghttp2_nv_array_del().
 *
 * The |*nva_ptr| includes pointers to the memory region in the
 * |in|. The caller must retain the |in| while the |*nva_ptr| is
 * used. After the use of |*nva_ptr| is over, if the caller intends to
 * inflate another set of headers, the caller must call
 * nghttp2_hd_end_headers().
 *
 * This function returns the number of name/value pairs in |*nva_ptr|
 * if it succeeds, or one of the following negative error codes:
 *
 * NGHTTP2_ERR_NOMEM
 *     Out of memory.
 * NGHTTP2_ERR_HEADER_COMP
 *     Inflation process has failed.
 */
ssize_t nghttp2_hd_inflate_hd(nghttp2_hd_context *inflater,
                              nghttp2_nv **nva_ptr,
                              uint8_t *in, size_t inlen);

/*
 * Signals the end of processing one header block.
 *
 * This function returns 0 if it succeeds. Currently this function
 * always succeeds.
 */
int nghttp2_hd_end_headers(nghttp2_hd_context *deflater_or_inflater);

/* For unittesting purpose */
int nghttp2_hd_emit_indname_block(uint8_t **buf_ptr, size_t *buflen_ptr,
                                  size_t *offset_ptr, size_t index,
                                  const uint8_t *value, size_t valuelen,
                                  int inc_indexing);

/* For unittesting purpose */
int nghttp2_hd_emit_newname_block(uint8_t **buf_ptr, size_t *buflen_ptr,
                                  size_t *offset_ptr, nghttp2_nv *nv,
                                  int inc_indexing);

/* For unittesting purpose */
int nghttp2_hd_emit_subst_indname_block(uint8_t **buf_ptr, size_t *buflen_ptr,
                                        size_t *offset_ptr, size_t index,
                                        const uint8_t *value, size_t valuelen,
                                        size_t subindex);

/* For unittesting purpose */
int nghttp2_hd_emit_subst_newname_block(uint8_t **buf_ptr, size_t *buflen_ptr,
                                        size_t *offset_ptr, nghttp2_nv *nv,
                                        size_t subindex);

#endif /* NGHTTP2_HD_COMP_H */
