﻿#pragma once
#ifndef KKLIB_H_
#define KKLIB_H_

/*---------------------------------------------------------------------------
  Copyright 2020 Daan Leijen, Microsoft Corporation.

  This is free software; you can redistribute it and/or modify it under the
  terms of the Apache License, Version 2.0. A copy of the License can be
  found in the file "license.txt" at the root of this distribution.
---------------------------------------------------------------------------*/
#include <assert.h>  // assert
#include <errno.h>   // ENOSYS, ...
#include <limits.h>  // LONG_MAX, ...
#include <stddef.h>  // ptrdiff_t
#include <stdint.h>  // int_t, ...
#include <stdbool.h> // bool
#include <stdio.h>   // FILE*, printf, ...
#include <string.h>  // strlen, memcpy, ...
#include <stdlib.h>  // malloc, abort, ...
#include <math.h>    // isnan, ...


#define MULTI_THREADED  1      // set to 0 to be used single threaded only

#include "kklib/platform.h"  // Platform abstractions and portability definitions
#include "kklib/atomic.h"    // Atomic operations



/*--------------------------------------------------------------------------------------
  Basic datatypes
--------------------------------------------------------------------------------------*/

// Tags for heap blocks
typedef enum tag_e {
  TAG_INVALID   = 0,
  TAG_MIN       = 1,
  TAG_MAX       = 65000,
  TAG_OPEN,        // open datatype, first field is a string tag
  TAG_BOX,         // boxed value type
  TAG_REF,         // mutable reference
  TAG_FUNCTION,    // function with free its free variables
  TAG_BIGINT,      // big integer (see `integer.c`)
  TAG_STRING_SMALL,// UTF8 encoded string of at most 7 bytes.
  TAG_STRING,      // UTF8 encoded string: valid (modified) UTF8 ending with a zero byte.
  TAG_BYTES,       // a vector of bytes
  TAG_VECTOR,      // a vector of (boxed) values
  TAG_INT64,       // boxed int64_t
  TAG_DOUBLE,      // boxed IEEE double (64-bit)
  TAG_INT32,       // boxed int32_t               (on 32-bit platforms)
  TAG_FLOAT,       // boxed IEEE float  (32-bit)  (on 32-bit platforms)
  TAG_CFUNPTR,     // C function pointer
  // raw tags have a free function together with a `void*` to the data
  TAG_CPTR_RAW,    // full void* (must be first, see tag_is_raw())
  TAG_STRING_RAW,  // pointer to a valid UTF8 string
  TAG_BYTES_RAW,   // pointer to bytes
  TAG_LAST
} tag_t;

static inline bool tag_is_raw(tag_t tag) {
  return (tag >= TAG_CPTR_RAW);
}

// Every heap block starts with a 64-bit header with a reference count, tag, and scan fields count.
// The reference count is 0 for a unique reference (for a faster free test in drop).
// Reference counts larger than 0x8000000 use atomic increment/decrement (for thread shared objects).
// (Reference counts are always 32-bit (even on 64-bit) platforms but get "sticky" if
//  they get too large (>0xC0000000) and in such case we never free the object, see `refcount.c`)
typedef struct header_s {
  uint8_t   scan_fsize;       // number of fields that should be scanned when releasing (`scan_fsize <= 0xFF`, if 0xFF, the full scan size is the first field)
  uint8_t   thread_shared : 1;
  uint16_t  tag;              // header tag
  uint32_t  refcount;         // reference count
} header_t;

#define SCAN_FSIZE_MAX (0xFF)
#define HEADER(scan_fsize,tag)         { scan_fsize, 0, tag, 0 }            // start with refcount of 0
#define HEADER_STATIC(scan_fsize,tag)  { scan_fsize, 0, tag, U32(0xFF00) }  // start with recognisable refcount (anything > 1 is ok)


// Polymorphic operations work on boxed values. (We use a struct for extra checks on accidental conversion)
// See `box.h` for definitions.
typedef struct box_s {
  uintptr_t box;          // We use unsigned representation to avoid UB on shift operations and overflow.
} box_t;

// An integer is either a small int or a pointer to a bigint_t. Identity with boxed values.
// See `integer.h` for definitions.
typedef struct integer_s {
  intptr_t value;
} integer_t;

// boxed forward declarations
static inline uintx_t   unbox_enum(box_t v);
static inline box_t     box_enum(uintx_t u);


/*--------------------------------------------------------------------------------------
  Blocks
  A block is an object that starts with a header.

  heap allocated datatypes contain a first `_block` field.
  Their heap allocated constructors in turn contain a first `_base` field that is the datatype.
  This representation ensures correct behaviour under C alias rules and allow good optimization.
  e.g. :

    typedef struct tree_s {
      block_t _block;
    } * tree_t

    struct Bin {
      struct tree_s  _base;
      tree_t        left;
      tree_t        right;
    }

    struct Leaf {
      struct tree_s  _base;
      box_t          value;
    }

  Datatypes that have singletons encode singletons as constants with the lowest bit set to 1.
  We use the type `datatype_t` to represent them:

    struct list_s {
      block_t _block;
    };
    typedef datatype_t list_t;

    struct Cons {
      struct list_s  _base;
      box_t          head;
      list_t         tail;
    }

    // no type for the Nil constructor
--------------------------------------------------------------------------------------*/

// A heap block is a header followed by `scan_fsize` boxed fields and further raw bytes
// A `block_t*` is never NULL (to avoid testing for NULL for reference counts).
typedef struct block_s {
  header_t header;
} block_t;

// A large block has a (boxed) large scan size for vectors.
typedef struct block_large_s {
  block_t  _block;
  box_t    large_scan_fsize; // if `scan_fsize == 0xFF` there is a first field with the full scan size
                             // (the full scan size should include the `large_scan_fsize` field itself!)
} block_large_t;

// A pointer to a block. Cannot be NULL.
typedef block_t* ptr_t;

// A general datatype with constructors and singletons is eiter a pointer to a block or an enumeration

typedef union datatype_s {
  ptr_t      ptr;         // always lowest bit cleared
  uintptr_t  singleton;   // always lowest bit set as: 4*tag + 1
} datatype_t;




static inline decl_const tag_t block_tag(const block_t* b) {
  return (tag_t)(b->header.tag);
}

static inline decl_const bool block_has_tag(const block_t* b, tag_t t) {
  return (block_tag(b) == t);
}

static inline decl_pure size_t block_scan_fsize(const block_t* b) {
  const size_t sfsize = b->header.scan_fsize;
  if (likely(sfsize != SCAN_FSIZE_MAX)) return sfsize;
  const block_large_t* bl = (const block_large_t*)b;
  return unbox_enum(bl->large_scan_fsize);
}

static inline decl_pure uintptr_t block_refcount(const block_t* b) {
  return b->header.refcount;
}

static inline decl_pure bool block_is_unique(const block_t* b) {
  return (likely(b->header.refcount == 0));
}


/*--------------------------------------------------------------------------------------
  The thread local context as `context_t`
  This is passed by the code generator as an argument to every function so it can
  be (usually) accessed efficiently through a register.
--------------------------------------------------------------------------------------*/
typedef void*  heap_t;

// A function has as its first field a pointer to a C function that takes the
// `function_t` itself as a first argument. The following fields are the free variables.
typedef struct function_s {
  block_t  _block;
  box_t    fun;
  // followed by free variables
} *function_t;

// A vector is an array of boxed values.
typedef struct vector_s {
  block_t _block;
} *vector_t;

// Strong random number context (using chacha20)
struct random_ctx_s;

//A yield context allows up to 8 continuations to be stored in-place
#define YIELD_CONT_MAX (8)

typedef enum yield_kind_e {
  YIELD_NONE,
  YIELD_NORMAL,
  YIELD_FINAL
} yield_kind_t;

typedef struct yield_s {
  int32_t    marker;          // marker of the handler to yield to
  function_t clause;          // the operation clause to execute when the handler is found
  size_t     conts_count;     // number of continuations in `conts`
  function_t conts[YIELD_CONT_MAX];  // fixed array of continuations. The final continuation `k` is
                              // composed as `fN ○ ... ○ f2 ○ f1` if `conts = { f1, f2, ..., fN }`
                              // if the array becomes full, a fresh array is allocated and the first
                              // entry points to its composition.
} yield_t;

// The thread local context.
// The fields `yielding`, `heap` and `evv` should come first for efficiency
typedef struct context_s {
  uint8_t     yielding;         // are we yielding to a handler? 0:no, 1:yielding, 2:yielding_final (e.g. exception) // put first for efficiency
  heap_t      heap;             // the (thread-local) heap to allocate in; todo: put in a register?
  vector_t    evv;              // the current evidence vector for effect handling: vector for size 0 and N>1, direct evidence for one element vector
  yield_t     yield;            // inlined yield structure (for efficiency)
  int32_t     marker_unique;    // unique marker generation
  block_t*    delayed_free;     // list of blocks that still need to be freed
  integer_t   unique;           // thread local unique number generation
  uintptr_t   thread_id;        // unique thread id
  function_t  log;              // logging function
  function_t  out;              // std output
  struct random_ctx_s* srandom_ctx;    // secure random using chacha20, initialized on demand
} context_t;

// Get the current (thread local) runtime context (should always equal the `_ctx` parameter)
decl_export context_t* get_context(void);

// The current context is passed as a _ctx parameter in the generated code
#define current_context()   _ctx

// Is the execution yielding?
static inline decl_pure bool _yielding(const context_t* ctx) {
  return (ctx->yielding != YIELD_NONE);
}
#define yielding(ctx)   unlikely(_yielding(ctx))


static inline decl_pure bool yielding_non_final(const context_t* ctx) {
  return (ctx->yielding == YIELD_NORMAL);
}

static inline decl_pure bool yielding_final(const context_t* ctx) {
  return (ctx->yielding == YIELD_FINAL);
}

// Get a thread local marker unique number >= 1.
static inline int32_t marker_unique(context_t* ctx) {
  int32_t m = ++ctx->marker_unique;           // must return a marker >= 1 so increment first;
  if (m == INT32_MAX) ctx->marker_unique = 1; // controlled reset
  return m;
}



/*--------------------------------------------------------------------------------------
  Allocation
--------------------------------------------------------------------------------------*/

#ifdef KK_MIMALLOC
#ifdef KK_MIMALLOC_INLINE
  #ifdef KK_STATIC_LIB
  #include "../mimalloc/include/mimalloc-inline.h"
  #else
  #include "mimalloc-inline.h"
  #endif
  static inline void* runtime_malloc_small(size_t sz, context_t* ctx) {
    return mi_heap_malloc_small_inline(ctx->heap, sz);
  }
#else
  #ifdef KK_STATIC_LIB
  #include "../mimalloc/include/mimalloc.h"
  #else
  #include "mimalloc.h"
  #endif
  static inline void* runtime_malloc_small(size_t sz, context_t* ctx) {
    return mi_heap_malloc_small(ctx->heap, sz);
  } 
#endif

static inline void* runtime_malloc(size_t sz, context_t* ctx) {
  return mi_heap_malloc(ctx->heap, sz);
}

static inline void* runtime_zalloc(size_t sz, context_t* ctx) {
  UNUSED(ctx);
  return mi_heap_zalloc(ctx->heap, sz);
}

static inline void* runtime_realloc(void* p, size_t sz, context_t* ctx) {
  UNUSED(ctx);
  return mi_heap_realloc(ctx->heap, p, sz);
}

static inline void runtime_free(void* p) {
  UNUSED(p);
  mi_free(p);
}

static inline void runtime_free_local(void* p) {
  runtime_free(p);
}
#else
static inline void* runtime_malloc(size_t sz, context_t* ctx) {
  UNUSED(ctx);
  return malloc(sz);
}

static inline void* runtime_malloc_small(size_t sz, context_t* ctx) {
  return runtime_malloc(sz,ctx);
}

static inline void* runtime_zalloc(size_t sz, context_t* ctx) {
  UNUSED(ctx);
  return calloc(1, sz);
}

static inline void* runtime_realloc(void* p, size_t sz, context_t* ctx) {
  UNUSED(ctx);
  return realloc(p, sz);
}

static inline void runtime_free(void* p) {
  UNUSED(p);
  free(p);
}

static inline void runtime_free_local(void* p) {
  runtime_free(p);
}
#endif


decl_export void block_free(block_t* b, context_t* ctx);

static inline void block_init(block_t* b, size_t size, size_t scan_fsize, tag_t tag) {
  UNUSED(size);
  assert_internal(scan_fsize < SCAN_FSIZE_MAX);
#if (ARCH_LITTLE_ENDIAN)
  // explicit shifts lead to better codegen
  *((uint64_t*)b) = ((uint64_t)scan_fsize | ((uint64_t)tag << 16));  
#else
  header_t header = { (uint8_t)scan_fsize, 0, (uint16_t)tag, 0 };
  b->header = header;
#endif
}

static inline void block_large_init(block_large_t* b, size_t size, size_t scan_fsize, tag_t tag) {
  UNUSED(size);
  header_t header = { SCAN_FSIZE_MAX, 0, (uint16_t)tag, 0 };
  b->_block.header = header;
  b->large_scan_fsize = box_enum(scan_fsize);
}

typedef block_t* reuse_t;

#define reuse_null   ((reuse_t)NULL)

static inline block_t* block_alloc_at(reuse_t at, size_t size, size_t scan_fsize, tag_t tag, context_t* ctx) {
  assert_internal(scan_fsize < SCAN_FSIZE_MAX);
  block_t* b;
  if (at==reuse_null) {
    b = (block_t*)runtime_malloc_small(size, ctx);
  }
  else {
    assert_internal(block_is_unique(at)); // TODO: check usable size of `at`
    b = at;
  }
  block_init(b, size, scan_fsize, tag);
  return b;
}

static inline block_t* block_alloc(size_t size, size_t scan_fsize, tag_t tag, context_t* ctx) {
  assert_internal(scan_fsize < SCAN_FSIZE_MAX);
  block_t* b = (block_t*)runtime_malloc_small(size, ctx);
  block_init(b, size, scan_fsize, tag);
  return b;
}

static inline block_t* block_alloc_any(size_t size, size_t scan_fsize, tag_t tag, context_t* ctx) {
  assert_internal(scan_fsize < SCAN_FSIZE_MAX);
  block_t* b = (block_t*)runtime_malloc(size, ctx);
  block_init(b, size, scan_fsize, tag);
  return b;
}

static inline block_large_t* block_large_alloc(size_t size, size_t scan_fsize, tag_t tag, context_t* ctx) {
  block_large_t* b = (block_large_t*)runtime_malloc(size + 1 /* the scan_large_fsize field */, ctx);
  block_large_init(b, size, scan_fsize, tag);
  return b;
}

static inline block_t* block_realloc(block_t* b, size_t size, context_t* ctx) {
  assert_internal(block_is_unique(b));
  return (block_t*)runtime_realloc(b, size, ctx);
}

static inline char* _block_as_assert(block_t* b, tag_t tag) {
  UNUSED_RELEASE(tag);
  assert_internal(block_tag(b) == tag);
  return (char*)b;
}

#define block_alloc_as(struct_tp,scan_fsize,tag,ctx)        ((struct_tp*)block_alloc_at(reuse_null, sizeof(struct_tp),scan_fsize,tag,ctx))
#define block_alloc_at_as(struct_tp,at,scan_fsize,tag,ctx)  ((struct_tp*)block_alloc_at(at, sizeof(struct_tp),scan_fsize,tag,ctx))

#define block_as(tp,b)                                      ((tp)((void*)(b)))
#define block_as_assert(tp,b,tag)                           ((tp)_block_as_assert(b,tag))


/*--------------------------------------------------------------------------------------
  Reference counting
--------------------------------------------------------------------------------------*/

decl_export void     block_check_free(block_t* b, uint32_t rc, context_t* ctx);
decl_export block_t* dup_block_check(block_t* b, uint32_t rc);

static inline block_t* dup_block(block_t* b) {
  const uint32_t rc = b->header.refcount;
  if (likely((int32_t)rc >= 0)) {    // note: assume two's complement  (we can skip this check if we never overflow a reference count or use thread-shared objects.)
    b->header.refcount = rc+1;
    return b;
  }
  else {
    return dup_block_check(b, rc);   // thread-shared or sticky (overflow) ?
  }
}

static inline void drop_block(block_t* b, context_t* ctx) {
  const uint32_t rc = b->header.refcount;
  if ((int32_t)(rc > 0)) {          // note: assume two's complement
    b->header.refcount = rc-1;
  }
  else {
    block_check_free(b, rc, ctx);   // thread-shared, sticky (overflowed), or can be freed?
  }
}

static inline void block_decref(block_t* b, context_t* ctx) {
  const uint32_t rc = b->header.refcount;
  assert_internal(rc != 0);
  if (likely((int32_t)(rc > 0))) {     // note: assume two's complement
    b->header.refcount = rc - 1;
  }
  else {
    assert_internal(false);
    block_check_free(b, rc, ctx);      // thread-shared, sticky (overflowed), or can be freed? TODO: should just free; not drop recursively
  }
}



reuse_t block_check_reuse(block_t* b, uint32_t rc0, context_t* ctx);

// Decrement the reference count, and return the memory for reuse if it drops to zero
static inline reuse_t drop_reuse_blockx(block_t* b, context_t* ctx) {
  const uint32_t rc = b->header.refcount;
  if ((int32_t)rc <= 0) {                 // note: assume two's complement
    return block_check_reuse(b, rc, ctx); // thread-shared, sticky (overflowed), or can be reused?
  }
  else {
    b->header.refcount = rc-1;
    return reuse_null;
  }
}

typedef struct block_fields_s {
  block_t _block;
  box_t   fields[1];
} block_fields_t;

static inline box_t block_field(block_t* b, size_t index) {
  block_fields_t* bf = (block_fields_t*)b;  // must overlap with datatypes with scanned fields.
  return bf->fields[index];
}

static inline void drop_box_t(box_t b, context_t* ctx);

// Decrement the reference count, and return the memory for reuse if it drops to zero
static inline reuse_t drop_reuse_block(block_t* b, context_t* ctx) {
  const uint32_t rc = b->header.refcount;
  if ((int32_t)rc == 0) {                 // note: assume two's complement
    size_t scan_fsize = block_scan_fsize(b);
    for (size_t i = 0; i < scan_fsize; i++) {
      drop_box_t(block_field(b, i), ctx);
    }
    return b;
  }
  else {
    drop_block(b, ctx);
    return reuse_null;
  }
}

// Decrement the reference count, and return the memory for reuse if it drops to zero
static inline reuse_t drop_reuse_blockn(block_t* b, size_t scan_fsize, context_t* ctx) {
  const uint32_t rc = b->header.refcount;
  if (rc == 0) {                 
    assert_internal(block_scan_fsize(b) == scan_fsize);
    for (size_t i = 0; i < scan_fsize; i++) {
      drop_box_t(block_field(b, i), ctx);
    }
    return b;
  }
  else if ((int32_t)rc < 0) {     // note: assume two's complement
    block_check_free(b, rc, ctx); // thread-shared or sticky (overflowed)?
    return reuse_null;
  }
  else {
    b->header.refcount = rc-1;
    return reuse_null;
  }
}

static inline void drop_blockn(block_t* b, size_t scan_fsize,  context_t* ctx) {
  const uint32_t rc = b->header.refcount;
  if (rc == 0) {                 // note: assume two's complement
    assert_internal(scan_fsize == block_scan_fsize(b));
    for (size_t i = 0; i < scan_fsize; i++) {
      drop_box_t(block_field(b, i), ctx);
    }
    runtime_free(b);
  }
  else if ((int32_t)rc < 0) {
    block_check_free(b, rc, ctx); // thread-shared, sticky (overflowed)?
  }
  else {
    b->header.refcount = rc-1;
  }
}



static inline void drop_block_assert(block_t* b, tag_t tag, context_t* ctx) {
  UNUSED_RELEASE(tag);
  assert_internal(block_tag(b) == tag);
  drop_block(b,ctx);
}

static inline block_t* dup_block_assert(block_t* b, tag_t tag) {
  UNUSED_RELEASE(tag);
  assert_internal(block_tag(b) == tag);
  return dup_block(b);
}

static inline void drop_reuse_t(reuse_t r, context_t* ctx) {
  UNUSED(ctx);
  if (r != NULL) {
    assert_internal(block_is_unique(r));
    runtime_free(r);
  }
}


/*--------------------------------------------------------------------------------------
  Datatype and Constructor macros
  We use:
  - basetype      For a pointer to the base type of a heap allocated constructor.
                  Datatypes without singletons are always a datatypex
  - datatype      For a regular datatype_t
  - constructor   For a pointer to a heap allocated constructor (whose first field
                  is `_base` and points to the base type as a `basetype`
--------------------------------------------------------------------------------------*/

// #define basetype_tag(v)                     (block_tag(&((v)->_block)))
#define basetype_has_tag(v,t)               (block_has_tag(&((v)->_block),t))
#define basetype_is_unique(v)               (block_is_unique(&((v)->_block)))
#define basetype_as(tp,v)                   (block_as(tp,&((v)->_block)))
#define basetype_free(v)                    (runtime_free(v))
#define basetype_decref(v,ctx)              (block_decref(&((v)->_block),ctx))
#define dup_basetype_as(tp,v)               ((tp)dup_block(&((v)->_block)))
#define drop_basetype(v,ctx)                (drop_block(&((v)->_block),ctx))
#define drop_reuse_basetype(v,n,ctx)        (drop_reuse_blockn(&((v)->_block),n,ctx))

#define basetype_as_assert(tp,v,tag)        (block_as_assert(tp,&((v)->_block),tag))
#define drop_basetype_assert(v,tag,ctx)     (drop_block_assert(&((v)->_block),tag,ctx))
#define dup_basetype_as_assert(tp,v,tag)    ((tp)dup_block_assert(&((v)->_block),tag))

#define constructor_tag(v)                  (basetype_tag(&((v)->_base)))
#define constructor_is_unique(v)            (basetype_is_unique(&((v)->_base)))
#define dup_constructor_as(tp,v)            (dup_basetype_as(tp, &((v)->_base)))
#define drop_constructor(v,ctx)             (drop_basetype(&((v)->_base),ctx))
#define drop_reuse_constructor(v,n,ctx)     (drop_reuse_basetype(&((v)->_base),n,ctx))

#define dup_value(v)                        (v)
#define drop_value(v,ctx)                   (void)
#define drop_reuse_value(v,ctx)             (reuse_null)


/*----------------------------------------------------------------------
  Datatypes
----------------------------------------------------------------------*/

// create a singleton
static inline datatype_t datatype_from_tag(tag_t t) {
  datatype_t d;
  d.singleton = (((uintptr_t)t)<<2 | 1);
  return d;
}

static inline datatype_t datatype_from_ptr(ptr_t p) {
  datatype_t d;
  d.ptr = p;
  return d;
}

static inline bool datatype_is_ptr(datatype_t d) {
  return ((d.singleton&1) == 0);
}

static inline bool datatype_is_singleton(datatype_t d) {
  return ((d.singleton&1) == 1);
}

static inline bool datatype_has_tag(datatype_t d, tag_t t) {
  if (datatype_is_ptr(d)) {
    return (block_tag(d.ptr) == t); 
  }
  else {
    return (d.singleton == datatype_from_tag(t).singleton);
  }
}

static inline block_t* datatype_as_ptr(datatype_t d) {
  assert_internal(datatype_is_ptr(d));
  return d.ptr;
}

static inline bool datatype_is_unique(datatype_t d) {
  return (datatype_is_ptr(d) && block_is_unique(d.ptr));
}

static inline datatype_t dup_datatype(datatype_t d) {
  if (datatype_is_ptr(d)) { dup_block(d.ptr); }
  return d;
}

static inline void drop_datatype(datatype_t d, context_t* ctx) {
  if (datatype_is_ptr(d)) { drop_block(d.ptr,ctx); }
}

static inline datatype_t dup_datatype_assert(datatype_t d, tag_t t) {
  UNUSED_RELEASE(t);
  assert_internal(datatype_has_tag(d, t));
  return dup_datatype(d);
}

static inline void drop_datatype_assert(datatype_t d, tag_t t, context_t* ctx) {
  UNUSED_RELEASE(t);
  assert_internal(datatype_has_tag(d, t));
  drop_datatype(d, ctx);
}

static inline reuse_t drop_reuse_datatype(datatype_t d, size_t scan_fsize, context_t* ctx) {
  if (datatype_is_singleton(d)) {
    return reuse_null;
  }
  else {
    return drop_reuse_blockn(d.ptr, scan_fsize, ctx);
  }
}

static inline void datatype_free(datatype_t d) {
  if (datatype_is_ptr(d)) {
    runtime_free(d.ptr);
  }
}

static inline void datatype_decref(datatype_t d, context_t* ctx) {
  if (datatype_is_ptr(d)) {
    block_decref(d.ptr,ctx);
  }
}

#define datatype_from_base(b)               (datatype_from_ptr(&(b)->_block))
#define datatype_as(tp,v)                   (block_as(tp,datatype_as_ptr(v)))
#define datatype_as_assert(tp,v,tag)        (block_as_assert(tp,datatype_as_ptr(v),tag))


#define define_static_datatype(decl,struct_tp,name,tag) \
  static struct_tp _static_##name = { { HEADER_STATIC(0,tag) } }; \
  decl struct_tp* name = &_static_##name;

#define define_static_open_datatype(decl,struct_tp,name,otag) /* ignore otag as it is initialized dynamically */ \
  static struct_tp _static_##name = { { HEADER_STATIC(0,TAG_OPEN) }, &_static_string_empty._base }; \
  decl struct_tp* name = &_static_##name;


/*----------------------------------------------------------------------
  Reference counting of pattern matches
----------------------------------------------------------------------*/

// The constructor that is matched on is still used; only duplicate the used fields
#define keep_match(con,dups) \
  do dups while(0)

// The constructor that is matched on is dropped:
// 1. if unique, drop the unused fields and free just the constructor block
// 2. otherwise, duplicate the used fields, and drop the constructor
#define drop_match(con,dups,drops,ctx) \
  if (constructor_is_unique(con)) { \
    do drops while(0); runtime_free(con); \
  } else { \
    do dups while(0); drop_constructor(con,ctx); \
  }

// The constructor that is matched on may be reused:
// 1. if unique, drop the unused fields and make the constructor block available for reuse
// 2. otherwise, duplicate the used fields, drop the constructor, and don't reuse
#define reuse_match(reuseid,con,dups,drops,ctx) \
  if (constructor_is_unique(con)) { \
    do drops while(0); reuseid = drop_reuse_constructor(conid,ctx); \
  } else { \
    do dups while(0); drop_constructor(con,ctx); reuseid = NULL; \
  }


/*----------------------------------------------------------------------
  Further includes
----------------------------------------------------------------------*/

// The unit type
typedef enum unit_e {
  Unit = 0
} unit_t;



#include "kklib/bits.h"
#include "kklib/box.h"
#include "kklib/integer.h"
#include "kklib/string.h"
#include "kklib/random.h"

/*----------------------------------------------------------------------
  TLD operations
----------------------------------------------------------------------*/

// Get a thread local unique number.
static inline integer_t gen_unique(context_t* ctx) {
  integer_t u = ctx->unique;
  ctx->unique = integer_inc(dup_integer_t(u),ctx);
  return u;
}



/*--------------------------------------------------------------------------------------
  Value tags
--------------------------------------------------------------------------------------*/

// Tag for value types is always an integer
typedef integer_t value_tag_t;

// Use inlined #define to enable constant initializer expression
/*
static inline value_tag_t value_tag(uintx_t tag) {
  return integer_from_small((intx_t)tag);
}
*/
#define value_tag(tag) (integer_from_small(tag))   

/*--------------------------------------------------------------------------------------
  Functions
--------------------------------------------------------------------------------------*/

#define function_as(tp,fun)                     basetype_as_assert(tp,fun,TAG_FUNCTION)
#define function_alloc_as(tp,scan_fsize,ctx)    block_alloc_as(tp,scan_fsize,TAG_FUNCTION,ctx)
#define function_call(restp,argtps,f,args)      ((restp(*)argtps)(unbox_cfun_ptr(f->fun)))args
#define define_static_function(name,cfun,ctx) \
  static struct function_s _static_##name = { { HEADER_STATIC(0,TAG_FUNCTION) }, { ~UP(0) } }; /* must be box_null */ \
  function_t name = &_static_##name; \
  if (box_eq(name->fun,box_null)) { name->fun = box_cfun_ptr((cfun_ptr_t)&cfun,ctx); }  // initialize on demand so it can be boxed properly



function_t function_id(context_t* ctx);
function_t function_null(context_t* ctx);

static inline function_t unbox_function_t(box_t v) {
  return unbox_basetype_as_assert(function_t, v, TAG_FUNCTION);
}

static inline box_t box_function_t(function_t d) {
  return box_basetype(d);
}

static inline bool function_is_unique(function_t f) {
  return block_is_unique(&f->_block);
}

static inline void drop_function_t(function_t f, context_t* ctx) {
  drop_basetype_assert(f, TAG_FUNCTION, ctx);
}

static inline function_t dup_function_t(function_t f) {
  return dup_basetype_as_assert(function_t, f, TAG_FUNCTION);
}


/*--------------------------------------------------------------------------------------
  References
--------------------------------------------------------------------------------------*/
typedef struct ref_s {
  block_t _block;
  box_t   value;
} *ref_t;

static inline ref_t ref_alloc(box_t value, context_t* ctx) {
  ref_t r = block_alloc_as(struct ref_s, 1, TAG_REF, ctx);
  r->value = value;
  return r;
}

static inline box_t ref_get(ref_t r) {
  box_t b = dup_box_t(r->value);
  // TODO: drop_box_t(r,_ctx)
  return b;
}

static inline unit_t ref_set(ref_t r, box_t value, context_t* ctx) {
  box_t b = r->value;
  drop_box_t(b, ctx);
  r->value = value;
  return Unit;
}

static inline box_t ref_swap(ref_t r, box_t value) {
  box_t b = r->value;
  r->value = value;
  return b;
}

static inline box_t box_ref_t(ref_t r, context_t* ctx) {
  UNUSED(ctx);
  return box_basetype(r);
}

static inline ref_t unbox_ref_t(box_t b, context_t* ctx) {
  UNUSED(ctx);
  return unbox_basetype_as_assert(ref_t, b, TAG_REF);
}

static inline void drop_ref_t(ref_t r, context_t* ctx) {
  drop_basetype_assert(r, TAG_REF, ctx);
}

static inline ref_t dup_ref_t(ref_t r) {
  return dup_basetype_as_assert(ref_t, r, TAG_REF);
}


decl_export void fatal_error(int err, const char* msg, ...);
decl_export void warning_message(const char* msg, ...);

static inline void unsupported_external(const char* msg) {
  fatal_error(ENOSYS,msg);
}


/*--------------------------------------------------------------------------------------
  Unit
--------------------------------------------------------------------------------------*/

static inline box_t box_unit_t(unit_t u) {
  return box_enum((uintx_t)u);
}

static inline unit_t unbox_unit_t(box_t u) {
  UNUSED_RELEASE(u);
  assert_internal( unbox_enum(u) == (uintx_t)Unit || is_box_any(u));
  return Unit; // (unit_t)unbox_enum(u);
}

/*--------------------------------------------------------------------------------------
  Vector
--------------------------------------------------------------------------------------*/
extern vector_t vector_empty;

static inline void drop_vector_t(vector_t v, context_t* ctx) {
  drop_basetype(v, ctx);
}

static inline vector_t dup_vector_t(vector_t v) {
  return dup_basetype_as(vector_t, v);
}

typedef struct vector_large_s {  // always use a large block for a vector so the offset to the elements is fixed
  struct block_large_s _block;
  box_t    vec[1];               // vec[(large_)scan_fsize]
} *vector_large_t;

static inline vector_t vector_alloc(size_t length, box_t def, context_t* ctx) {
  if (length==0) {
    return dup_vector_t(vector_empty);
  }
  else {
    vector_large_t v = (vector_large_t)block_large_alloc(sizeof(struct vector_large_s) + (length-1)*sizeof(box_t), length + 1 /* large_scan_fsize */, TAG_VECTOR, ctx);
    if (def.box != box_null.box) {
      for (size_t i = 0; i < length; i++) {
        v->vec[i] = def;
      }
    }
    return (vector_t)(&v->_block._block);
  }
}

static inline size_t vector_len(const vector_t v) {
  size_t len = unbox_enum( basetype_as_assert(vector_large_t, v, TAG_VECTOR)->_block.large_scan_fsize ) - 1;
  assert_internal(len + 1 == block_scan_fsize(&v->_block));
  assert_internal(len + 1 != 0);
  return len;
}

static inline box_t* vector_buf(vector_t v, size_t* len) {
  if (len != NULL) *len = vector_len(v);
  return &(basetype_as_assert(vector_large_t, v, TAG_VECTOR)->vec[0]);
}

static inline box_t vector_at(const vector_t v, size_t i) {
  assert(i < vector_len(v));
  return dup_box_t(vector_buf(v,NULL)[i]);
}

static inline box_t box_vector_t(vector_t v, context_t* ctx) {
  UNUSED(ctx);
  return box_ptr(&v->_block);
}

static inline vector_t unbox_vector_t(box_t v, context_t* ctx) {
  UNUSED(ctx);
  block_t* b = unbox_ptr(v);
  assert_internal(block_tag(b) == TAG_VECTOR);
  return (vector_t)b;
}

/*--------------------------------------------------------------------------------------
  Bytes
--------------------------------------------------------------------------------------*/

typedef struct bytes_s {
  block_t  _block;               // TAG_BYTES or TAG_BYTES_RAW
}* bytes_t;

struct bytes_vector_s {          // in-place bytes
  struct bytes_s  _base;
  size_t          length;
  char            buf[1];
};

struct bytes_raw_s {             // pointer to bytes with free function
  struct bytes_s _base;
  free_fun_t*    free;
  uint8_t*       data;
  size_t         length;
};



decl_export string_t  get_host(context_t* ctx);


#endif // include guard
