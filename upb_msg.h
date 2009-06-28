/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * Copyright (c) 2009 Joshua Haberman.  See LICENSE for details.
 *
 * upb_msg contains a full description of a message as defined in a .proto file.
 * This allows for run-time reflection over .proto types, but also defines an
 * in-memory byte-level format for storing protobufs.
 *
 * The in-memory format is very much like a C struct that you can define at
 * run-time, but also supports reflection.  Like C structs it supports
 * offset-based access, as opposed to the much slower name-based lookup.  The
 * format represents both the values themselves and bits describing whether each
 * field is set or not.
 *
 * The upb compiler emits C structs that mimic this definition exactly, so that
 * you can access the same hunk of memory using either this run-time
 * reflection-supporting interface or a C struct that was generated by the upb
 * compiler.
 *
 * Like C structs the format depends on the endianness of the host machine, so
 * it is not suitable for exchanging across machines of differing endianness.
 * But there is no reason to do that -- the protobuf serialization format is
 * designed already for serialization/deserialization, and is more compact than
 * this format.  This format is designed to allow the fastest possible random
 * access of individual fields.
 *
 * Note that no memory management is defined, which should make it easier to
 * integrate this format with existing memory-management schemes.  Any memory
 * management semantics can be used with the format as defined here.
 */

#ifndef UPB_MSG_H_
#define UPB_MSG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "upb.h"
#include "upb_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations from descriptor.h. */
struct google_protobuf_DescriptorProto;
struct google_protobuf_FieldDescriptorProto;

/* Structure definition. ******************************************************/

/* Structure that describes a single field in a message. */
struct upb_msg_field {
  struct google_protobuf_FieldDescriptorProto *descriptor;
  uint32_t byte_offset;     /* Where to find the data. */
  uint16_t field_index;     /* Indexes upb_msg.fields. Also indicates set bit */
  union upb_symbol_ref ref;
};

/* Structure that describes a single .proto message type. */
struct upb_msg {
  struct google_protobuf_DescriptorProto *descriptor;
  size_t size;
  uint32_t num_fields;
  uint32_t set_flags_bytes;
  uint32_t num_required_fields;  /* Required fields have the lowest set bytemasks. */
  struct upb_inttable fields_by_num;
  struct upb_strtable fields_by_name;
  struct upb_msg_field *fields;
};

/* The num->field and name->field maps in upb_msg allow fast lookup of fields
 * by number or name.  These lookups are in the critical path of parsing and
 * field lookup, so they must be as fast as possible.  To make these more
 * cache-friendly, we put the data in the table by value, but use only an
 * abbreviated set of data (ie. not all the data in upb_msg_field).  Notably,
 * we don't include the pointer to the field descriptor.  But the upb_msg_field
 * can be retrieved in its entirety using the function below.*/

struct upb_abbrev_msg_field {
  uint32_t byte_offset;     /* Where to find the data. */
  uint16_t field_index;     /* Indexes upb_msg.fields. Also indicates set bit */
  upb_field_type_t type;    /* Copied from descriptor for cache-friendliness. */
  union upb_symbol_ref ref;
};

struct upb_fieldsbynum_entry {
  struct upb_inttable_entry e;
  struct upb_abbrev_msg_field f;
};

struct upb_fieldsbyname_entry {
  struct upb_strtable_entry e;
  struct upb_abbrev_msg_field f;
};

struct upb_msg_field *upb_get_msg_field(
    struct upb_abbrev_msg_field *f, struct upb_msg *m) {
  return &m->fields[f->field_index];
}

/* Initialize and free a upb_msg.  Caller retains ownership of d, but the msg
 * will contain references to it, so it must outlive the msg.  Note that init
 * does not resolve upb_msg_field.ref -- that is left to the caller. */
bool upb_msg_init(struct upb_msg *m, struct google_protobuf_DescriptorProto *d);
void upb_msg_free(struct upb_msg *m);

/* While these are written to be as fast as possible, it will still be faster
 * to cache the results of this lookup if possible.  These return NULL if no
 * such field is found. */
INLINE struct upb_abbrev_msg_field *upb_msg_fieldbynum(struct upb_msg *m,
                                                       uint32_t number) {
  struct upb_fieldsbynum_entry *e = upb_inttable_lookup(
      &m->fields_by_num, number, sizeof(struct upb_fieldsbynum_entry));
  return e ? &e->f : NULL;
}
INLINE struct upb_abbrev_msg_field *upb_msg_fieldbyname(struct upb_msg *m,
                                                        struct upb_string *name) {
  struct upb_fieldsbyname_entry *e =
      upb_strtable_lookup(&m->fields_by_name, name);
  return e ? &e->f : NULL;
}

/* Variable-length data (strings and arrays).**********************************/

/* Represents an array (a repeated field) of any type.  The interpretation of
 * the data in the array depends on the type. */
struct upb_array {
  void *data;  /* Size of individual elements is based on type. */
  uint32_t len;     /* Measured in elements. */
};

/* A generic array of structs, using void* instead of specific types. */
struct upb_msg_array {
  void **elements;
  uint32_t len;
};

/* An array of strings. */
struct upb_string_array {
  struct upb_string **elements;
  uint32_t len;
};

/* Specific arrays of all the primitive types. */
#define UPB_DEFINE_PRIMITIVE_ARRAY(type, name) \
  struct upb_ ## name ## _array { \
    size_t len; \
    type *elements; \
  };

UPB_DEFINE_PRIMITIVE_ARRAY(double,   double)
UPB_DEFINE_PRIMITIVE_ARRAY(float,    float)
UPB_DEFINE_PRIMITIVE_ARRAY(int32_t,  int32)
UPB_DEFINE_PRIMITIVE_ARRAY(int64_t,  int64)
UPB_DEFINE_PRIMITIVE_ARRAY(uint32_t, uint32)
UPB_DEFINE_PRIMITIVE_ARRAY(uint64_t, uint64)
UPB_DEFINE_PRIMITIVE_ARRAY(bool,     bool)
#undef UPB_DEFINE_PRMITIVE_ARRAY

#define UPB_STRUCT_ARRAY(struct_type) struct struct_type ## _array

#define UPB_DEFINE_STRUCT_ARRAY(struct_type) \
  UPB_STRUCT_ARRAY(struct_type) { \
    size_t len; \
    struct_type **elements; \
  };

/* Accessors for primitive types.  ********************************************/

/* For each primitive type we define a set of six functions:
 *
 *  // For fetching out of a struct (s points to the raw struct data).
 *  int32_t *upb_msg_get_int32_ptr(void *s, struct upb_msg_field *f);
 *  int32_t upb_msg_get_int32(void *s, struct upb_msg_field *f);
 *  void upb_msg_set_int32(void *s, struct upb_msg_field *f, int32_t val);
 *
 *  // For fetching out of an array.
 *  int32_t *upb_array_get_int32_ptr(struct upb_array *a, int n);
 *  int32_t upb_array_get_int32(struct upb_array *a, int n);
 *  void upb_array_set_int32(struct upb_array *a, int n, ctype val);
 *
 * For arrays we provide only the first three because protobufs do not support
 * arrays of arrays.
 *
 * These do no existence checks, bounds checks, or type checks. */

#define UPB_DEFINE_ACCESSORS(ctype, name, INLINE) \
  INLINE ctype *upb_msg_get_ ## name ## _ptr( \
      void *s, struct upb_msg_field *f) { \
    return (ctype*)((char*)s + f->byte_offset); \
  } \
  INLINE ctype upb_msg_get_ ## name( \
      void *s, struct upb_msg_field *f) { \
    return *upb_msg_get_ ## name ## _ptr(s, f); \
  } \
  INLINE void upb_msg_set_ ## name( \
      void *s, struct upb_msg_field *f, ctype val) { \
    *upb_msg_get_ ## name ## _ptr(s, f) = val; \
  }

#define UPB_DEFINE_ARRAY_ACCESSORS(ctype, name, INLINE) \
  INLINE ctype *upb_array_get_ ## name ## _ptr(struct upb_array *a, int n) { \
    return ((ctype*)a->data) + n; \
  } \
  INLINE ctype upb_array_get_ ## name(struct upb_array *a, int n) { \
    return *upb_array_get_ ## name ## _ptr(a, n); \
  } \
  INLINE void upb_array_set_ ## name(struct upb_array *a, int n, ctype val) { \
    *upb_array_get_ ## name ## _ptr(a, n) = val; \
  }

#define UPB_DEFINE_ALL_ACCESSORS(ctype, name, INLINE) \
  UPB_DEFINE_ACCESSORS(ctype, name, INLINE) \
  UPB_DEFINE_ARRAY_ACCESSORS(ctype, name, INLINE)

UPB_DEFINE_ALL_ACCESSORS(double,   double, INLINE)
UPB_DEFINE_ALL_ACCESSORS(float,    float,  INLINE)
UPB_DEFINE_ALL_ACCESSORS(int32_t,  int32,  INLINE)
UPB_DEFINE_ALL_ACCESSORS(int64_t,  int64,  INLINE)
UPB_DEFINE_ALL_ACCESSORS(uint32_t, uint32, INLINE)
UPB_DEFINE_ALL_ACCESSORS(uint64_t, uint64, INLINE)
UPB_DEFINE_ALL_ACCESSORS(bool,     bool,   INLINE)
UPB_DEFINE_ALL_ACCESSORS(struct upb_string*, bytes, INLINE)
UPB_DEFINE_ALL_ACCESSORS(struct upb_string*, string, INLINE)
UPB_DEFINE_ALL_ACCESSORS(void*, substruct, INLINE)
UPB_DEFINE_ACCESSORS(struct upb_array*, array, INLINE)

INLINE size_t upb_isset_offset(uint32_t field_index) {
  return field_index / 8;
}

INLINE uint8_t upb_isset_mask(uint32_t field_index) {
  return 1 << (field_index % 8);
}

/* Functions for reading and writing the "set" flags in the pbstruct.  Note
 * that these do not perform any memory management associated with any dynamic
 * memory these fields may be referencing; that is the client's responsibility.
 * These *only* set and test the flags. */
INLINE void upb_msg_set(void *s, struct upb_msg_field *f)
{
  ((char*)s)[upb_isset_offset(f->field_index)] |= upb_isset_mask(f->field_index);
}

INLINE void upb_msg_unset(void *s, struct upb_msg_field *f)
{
  ((char*)s)[upb_isset_offset(f->field_index)] &= ~upb_isset_mask(f->field_index);
}

INLINE bool upb_msg_is_set(void *s, struct upb_msg_field *f)
{
  return ((char*)s)[upb_isset_offset(f->field_index)] & upb_isset_mask(f->field_index);
}

INLINE bool upb_msg_all_required_fields_set(void *s, struct upb_msg *m)
{
  int num_fields = m->num_required_fields;
  int i = 0;
  while(num_fields > 8) {
    if(((uint8_t*)s)[i++] != 0xFF) return false;
    num_fields -= 8;
  }
  if(((uint8_t*)s)[i] != (1 << num_fields) - 1) return false;
  return true;
}

INLINE void upb_msg_clear(void *s, struct upb_msg *m)
{
  memset(s, 0, m->set_flags_bytes);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* UPB_MSG_H_ */
