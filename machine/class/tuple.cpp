#include "alloc.hpp"
#include "memory.hpp"
#include "object_utils.hpp"

#include "class/class.hpp"
#include "class/exception.hpp"
#include "class/fixnum.hpp"
#include "class/tuple.hpp"

#include <stdarg.h>
#include <sstream>

namespace rubinius {

  uintptr_t Tuple::fields_offset;

  void Tuple::bootstrap(STATE) {
    GO(tuple).set(Class::bootstrap_class(state, G(object), TupleType));

    Tuple* tup = ALLOCA(Tuple);
    fields_offset = (uintptr_t)&(tup->field) - (uintptr_t)tup;
  }

  void Tuple::write_barrier(STATE, Tuple* tuple, Object* val) {
    state->memory()->write_barrier(tuple, val);
  }

  Tuple* Tuple::bounds_exceeded_error(STATE, const char* method, int index) {
    std::ostringstream msg;

    msg << method;
    msg << ": index " << index << " out of bounds for size " << num_fields();

    Exception::raise_object_bounds_exceeded_error(state, msg.str().c_str());
    return 0;
  }

  /* The Tuple#at primitive. */
  Object* Tuple::at_prim(STATE, Fixnum* index_obj) {
    native_int index = index_obj->to_native();

    if(index < 0 || num_fields() <= index) {
      return bounds_exceeded_error(state, "Tuple::at_prim", index);
    }

    return field[index];
  }

  /* The Tuple#put primitive. */
  Object* Tuple::put_prim(STATE, Fixnum* index, Object* val) {
    native_int idx = index->to_native();

    if(idx < 0 || num_fields() <= idx) {
      return bounds_exceeded_error(state, "Tuple::put_prim", idx);
    }

    field[idx] = val;
    state->memory()->write_barrier(this, val);
    return val;
  }

  /* The Tuple#fields primitive. */
  Object* Tuple::fields_prim(STATE) {
    return Integer::from(state, num_fields());
  }

  Tuple* Tuple::create(STATE, native_int fields) {
    if(fields < 0) {
      Exception::raise_argument_error(state, "negative tuple size");
    }

    Tuple* tup = state->memory()->new_fields<Tuple>(state, G(tuple), fields);
    Tuple::initialize(state, tup);

    return tup;
  }

  Tuple* Tuple::allocate(STATE, Object* self, Fixnum* fields) {
    native_int size = fields->to_native();

    if(size < 0) {
      Exception::raise_argument_error(state, "negative tuple size");
    }

    Tuple* tuple = create(state, fields->to_native());
    tuple->klass(state, as<Class>(self));
    return tuple;
  }

  Tuple* Tuple::from(STATE, native_int fields, ...) {
    Tuple* tup = Tuple::create(state, fields);
    va_list ar;

    va_start(ar, fields);
    for(native_int i = 0; i < fields; i++) {
      Object *obj = va_arg(ar, Object*);
      // fields equals size so bounds checking is unecessary
      tup->put(state, i, obj);
    }
    va_end(ar);

    return tup;
  }

  Tuple* Tuple::copy_from(STATE, Tuple* other, Fixnum* start, Fixnum *length, Fixnum* dest) {
    native_int osize = other->num_fields();
    native_int size = this->num_fields();

    native_int src_start = start->to_native();
    native_int dst_start = dest->to_native();
    native_int len = length->to_native();

    // left end should be within range
    if(src_start < 0 || src_start > osize) {
      return other->bounds_exceeded_error(state, "Tuple::copy_from", src_start);
    }

    if(dst_start < 0 || dst_start > size) {
      return bounds_exceeded_error(state, "Tuple::copy_from", dst_start);
    }

    // length can not be negative and must fit in src/dest
    if(len < 0) {
      return other->bounds_exceeded_error(state, "Tuple::copy_from", len);
    }

    if((src_start + len) > osize) {
      return other->bounds_exceeded_error(state, "Tuple::copy_from", src_start + len);
    }

    if(len > (size - dst_start)) {
      return bounds_exceeded_error(state, "Tuple::copy_from", len);
    }

    // A memmove within the tuple
    if(other == this) {
      // No movement, no work!
      if(src_start == dst_start) return this;
      // right shift
      if(src_start < dst_start) {
        for(native_int dest_idx = dst_start + len - 1,
                       src_idx  = src_start + len - 1;
            src_idx >= src_start;
            src_idx--, dest_idx--) {
          this->field[dest_idx] = this->field[src_idx];
        }
      } else {
        // left shift
        for(native_int dest_idx = dst_start,
                       src_idx  = src_start;
            src_idx < src_start + len;
            src_idx++, dest_idx++) {
          this->field[dest_idx] = this->field[src_idx];
        }
      }

    } else {
      for(native_int src = src_start, dst = dst_start;
          src < (src_start + len);
          ++src, ++dst) {
        // Since we have carefully checked the bounds we don't need
        // to do it in at/put
        Object *obj = other->field[src];
        // but this is necessary to keep the GC happy
        field[dst] = obj;
        state->memory()->write_barrier(this, obj);
      }
    }

    return this;
  }

  native_int Tuple::delete_inplace(native_int lend, native_int len, Object* obj) {
    native_int rend = lend + len;
    native_int i = lend;
    while(i < rend) {
      if(this->at(i) == obj) {
        native_int j = i;
        ++i;
        while(i < rend) {
          Object *val = field[i];
          if(val != obj) {
            // no need to set write_barrier since it's already
            // referenced to this object
            field[j] = val;
            ++j;
          }
          ++i;
        }
        // cleanup all the bins after
        i = j;
        while(i < rend) {
          this->field[i] = cNil;
          ++i;
        }
        return rend-j;
      }
      ++i;
    }
    return 0;
  }

  Fixnum* Tuple::delete_inplace_prim(STATE, Fixnum *start, Fixnum *length, Object *obj) {
    native_int size = this->num_fields();
    native_int len  = length->to_native();
    native_int lend = start->to_native();
    native_int rend = lend + len;

    if(size == 0 || len == 0) return Fixnum::from(0);
    if(lend < 0 || lend >= size) {
      return force_as<Fixnum>(bounds_exceeded_error(state, "Tuple::delete_inplace", lend));
    }

    if(rend < 0 || rend > size) {
      return force_as<Fixnum>(bounds_exceeded_error(state, "Tuple::delete_inplace", rend));
    }

    return Fixnum::from(delete_inplace(lend, len, obj));
  }

  /** @todo Add some error checking/handling and
   *  evaluate corner cases, and add tests... --rue
   */
  Tuple* Tuple::lshift_inplace(STATE, Fixnum* shift) {
    native_int size = this->num_fields();
    const native_int start = shift->to_native();

    assert(start >= 0);

    if(start > 0) {
      native_int i = 0;
      native_int j = start;

      while(j < size) {
        this->field[i++] = this->field[j++];
      }

      while(i < size) {
        this->field[i++] = cNil;
      }
    }

    return this;
  }

  Object* Tuple::reverse(STATE, Fixnum* o_start, Fixnum* o_total) {
    native_int start = o_start->to_native();
    native_int total = o_total->to_native();

    if(total <= 0 || start < 0 || start >= num_fields()) return this;

    native_int end = start + total - 1;
    if(end >= num_fields()) end = num_fields() - 1;

    Object** pos1 = field + start;
    Object** pos2 = field + end;

    Object* tmp;
    while(pos1 < pos2) {
      tmp = *pos1;
      *pos1++ = *pos2;
      *pos2-- = tmp;
    }

    return this;
  }

  // @todo performance primitive; could be replaced with Ruby
  Tuple* Tuple::pattern(STATE, Fixnum* size, Object* val) {
    native_int cnt = size->to_native();

    if(cnt < 0) {
      Exception::raise_argument_error(state, "negative tuple size");
    }

    Tuple* tuple = state->memory()->new_fields<Tuple>(state, G(tuple), cnt);

    for(native_int i = 0; i < cnt; i++) {
      tuple->field[i] = val;
    }

    state->memory()->write_barrier(tuple, val);

    return tuple;
  }

  Tuple* Tuple::tuple_dup(STATE) {
    native_int fields = num_fields();

    Tuple* tup = Tuple::create(state, fields);

    for(native_int i = 0; i < fields; i++) {
      tup->put(state, i, field[i]);
    }

    return tup;
  }

  size_t Tuple::Info::object_size(const ObjectHeader* obj) {
    return force_as<Tuple>(obj)->full_size();
  }

  void Tuple::Info::mark(Object* obj, memory::ObjectMark& mark) {
    Tuple* tup = as<Tuple>(obj);

    for(native_int i = 0; i < tup->num_fields(); i++) {
      if(Object* tmp = mark.call(tup->field[i])) {
        mark.set(obj, &tup->field[i], tmp);
      }
    }
  }

  void Tuple::Info::show(STATE, Object* self, int level) {
    Tuple* tup = as<Tuple>(self);
    native_int size = tup->num_fields();
    native_int stop = size < 6 ? size : 6;

    if(size == 0) {
      class_info(state, self, true);
      return;
    }

    class_info(state, self);
    std::cout << ": " << size << std::endl;
    ++level;
    for(native_int i = 0; i < stop; i++) {
      indent(level);
      Object* obj = tup->at(state, i);
      if(obj == tup) {
        class_info(state, self, true);
      } else {
        obj->show(state, level);
      }
    }
    if(tup->num_fields() > stop) ellipsis(level);
    close_body(level);
  }

  void Tuple::Info::show_simple(STATE, Object* self, int level) {
    Tuple* tup = as<Tuple>(self);
    native_int size = tup->num_fields();
    native_int stop = size < 6 ? size : 6;

    if(size == 0) {
      class_info(state, self, true);
      return;
    }

    class_info(state, self);
    std::cout << ": " << size << std::endl;
    ++level;
    for(native_int i = 0; i < stop; i++) {
      indent(level);
      Object* obj = tup->at(state, i);
      if(Tuple* t = try_as<Tuple>(obj)) {
        class_info(state, self);
        std::cout << ": " << t->num_fields() << ">" << std::endl;
      } else {
        obj->show_simple(state, level);
      }
    }
    if(tup->num_fields() > stop) ellipsis(level);
    close_body(level);
  }
}
