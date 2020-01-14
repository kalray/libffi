/* Copyright (c) 2020 Kalray

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
``Software''), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED ``AS IS'', WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#if defined(__k1c__)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fficonfig.h>
#include <ffi.h>
#include "ffi_common.h"
#include "asm.h"

#define ALIGN(x, a) ALIGN_MASK(x, (typeof(x))(a) - 1)
#define ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define K1C_ABI_STACK_ALIGNMENT (8)
#define max(a,b) ((a) > (b) ? (a) : (b))

#ifdef FFI_DEBUG
#define DEBUG_PRINT(...) do{ fprintf( stderr, __VA_ARGS__ ); } while(0)
#else
#define DEBUG_PRINT(...)
#endif

struct ret_value {
	unsigned long int r0;
	unsigned long int r1;
	unsigned long int r2;
	unsigned long int r3;
};

extern struct ret_value ffi_call_SYSV(unsigned total_size,
                                      unsigned size,
                                      extended_cif *ecif,
                                      unsigned *rvalue_addr,
                                      void *fn);

/* Perform machine dependent cif processing */
ffi_status ffi_prep_cif_machdep(ffi_cif *cif)
{
  cif->flags = cif->rtype->size;
  return FFI_OK;
}

/* ffi_prep_args is called by the assembly routine once stack space
   has been allocated for the function's arguments */

void *ffi_prep_args(char *stack, unsigned int arg_slots_size, extended_cif *ecif)
{
  char *stacktemp = stack;
  char *current_arg_passed_by_value = stack + arg_slots_size;
  int i, s;
  ffi_type **arg;
  int count = 0;
  ffi_cif *cif = ecif->cif;
  void **argv = ecif->avalue;

  arg = cif->arg_types;

  DEBUG_PRINT("stack: %p\n", stack);
  DEBUG_PRINT("arg_slots_size: %u\n", arg_slots_size);
  DEBUG_PRINT("current_arg_passed_by_value: %p\n", current_arg_passed_by_value);
  DEBUG_PRINT("ecif: %p\n", ecif);
  DEBUG_PRINT("ecif->avalue: %p\n", ecif->avalue);

  for (i = 0; i < cif->nargs; i++) {

    s = K1C_ABI_SLOT_SIZE;
    switch((*arg)->type) {
      case FFI_TYPE_SINT8:
      case FFI_TYPE_UINT8:
      case FFI_TYPE_SINT16:
      case FFI_TYPE_UINT16:
      case FFI_TYPE_SINT32:
      case FFI_TYPE_UINT32:
      case FFI_TYPE_FLOAT:
      case FFI_TYPE_DOUBLE:
      case FFI_TYPE_UINT64:
      case FFI_TYPE_SINT64:
      case FFI_TYPE_POINTER:
        DEBUG_PRINT("INT64/32/16/8/FLOAT/DOUBLE or POINTER @%p\n", stack);
        *(uint64_t *) stack = *(uint64_t *)(* argv);
        break;

      case FFI_TYPE_STRUCT: {
        char *value;
        unsigned int written_size = 0;
        DEBUG_PRINT("struct by value @%p\n", stack);
        if ((*arg)->size > K1C_ABI_MAX_AGGREGATE_IN_REG_SIZE) {
          DEBUG_PRINT("big struct\n");
          *(uint64_t *) stack = (uintptr_t)current_arg_passed_by_value;
          value = current_arg_passed_by_value;
          current_arg_passed_by_value += (*arg)->size;
          written_size = K1C_ABI_SLOT_SIZE;
        } else {
          value = stack;
          written_size = (*arg)->size;
        }
        memcpy(value, *argv, (*arg)->size);
        s = ALIGN(written_size, K1C_ABI_STACK_ALIGNMENT);
        break;
      }
      default:
        abort();
        break;

    }
    stack += s;
    count += s;
    argv++;
    arg++;
  }
  return stacktemp + REG_ARGS_SIZE;
}

/* Perform machine dependent cif processing when we have a variadic function */

ffi_status ffi_prep_cif_machdep_var(ffi_cif *cif, unsigned int nfixedargs,
                                    unsigned int ntotalargs)
{
  cif->flags = cif->rtype->size;
  return FFI_OK;
}

void ffi_call(ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue)
{
  int i;
  unsigned long int slot_fitting_args_size = 0;
  unsigned long int total_size = 0;
  unsigned long int big_struct_size = 0;
  ffi_type **arg;
  struct ret_value local_rvalue;


  /* Calculate size to allocate on stack */
  for (i = 0, arg = cif->arg_types; i < cif->nargs; i++, arg++) {
    DEBUG_PRINT("argument %d, type %d, size %lu\n", i, (*arg)->type, (*arg)->size);
    if ((*arg)->type == FFI_TYPE_STRUCT) {
      if ((*arg)->size <= K1C_ABI_MAX_AGGREGATE_IN_REG_SIZE) {
        slot_fitting_args_size += ALIGN((*arg)->size, K1C_ABI_SLOT_SIZE);
      } else {
        slot_fitting_args_size += K1C_ABI_SLOT_SIZE; /* aggregate passed by reference */
        big_struct_size += ALIGN((*arg)->size, K1C_ABI_SLOT_SIZE);
      }
    } else if ((*arg)->size <= K1C_ABI_SLOT_SIZE) {
      slot_fitting_args_size += K1C_ABI_SLOT_SIZE;
    } else {
      abort(); /* should never happen? */
    }
  }

  extended_cif ecif;
  ecif.cif = cif;
  ecif.avalue = avalue;
  ecif.rvalue = rvalue;

  /* This implementation allocates anyway for all register based args */
  slot_fitting_args_size = max(slot_fitting_args_size, REG_ARGS_SIZE);
  total_size = slot_fitting_args_size + big_struct_size;

  switch (cif->abi) {
    case FFI_SYSV:
      DEBUG_PRINT("total_size: %lu\n", total_size);
      DEBUG_PRINT("slot fitting args size: %lu\n", slot_fitting_args_size);
      DEBUG_PRINT("rvalue: %p\n", rvalue);
      DEBUG_PRINT("fn: %p\n", fn);
      DEBUG_PRINT("rsize: %u\n", cif->flags);
      local_rvalue = ffi_call_SYSV(total_size, slot_fitting_args_size, &ecif, rvalue, fn);
      if (cif->flags <= K1C_ABI_MAX_AGGREGATE_IN_REG_SIZE)
        memcpy(rvalue, &local_rvalue, cif->flags);
      break;
    default:
      abort();
      break;
  }
}

/* Closures not supported yet */
ffi_status
ffi_prep_closure_loc (ffi_closure* closure,
                      ffi_cif* cif,
                      void (*fun)(ffi_cif*,void*,void**,void*),
                      void *user_data,
                      void *codeloc)
{
  return FFI_BAD_ABI;
}

#endif /* (__k1c__) */
