#include "detection.hpp"

#include "builtin/mono_inline_cache.hpp"
#include "builtin/inline_cache.hpp"
#include "arguments.hpp"
#include "call_frame.hpp"
#include "global_cache.hpp"

#include "builtin/class.hpp"
#include "builtin/module.hpp"
#include "builtin/symbol.hpp"
#include "builtin/lookuptable.hpp"
#include "builtin/executable.hpp"
#include "builtin/methodtable.hpp"
#include "builtin/alias.hpp"
#include "builtin/call_unit.hpp"
#include "ontology.hpp"

namespace rubinius {

  void InlineCache::init(STATE) {
    GO(inline_cache).set(
        ontology::new_class(state, "InlineCache",
          G(call_site), G(rubinius)));
    G(inline_cache)->set_object_type(state, InlineCacheType);
  }

  InlineCache* InlineCache::create(STATE, MonoInlineCache* mono) {
    InlineCache* cache =
      state->vm()->new_object_mature<InlineCache>(G(inline_cache));
    cache->name_     = mono->name();
    cache->executable(state, mono->executable());
    cache->ip_       = mono->ip();
    cache->executor_ = check_cache;
    cache->fallback_ = mono->fallback_;
    cache->updater_  = inline_cache_updater;
    cache->seen_classes_overflow_ = 0;
    cache->clear();

    Dispatch dis(mono->name());
    dis.module = mono->stored_module();
    dis.method = mono->method();
    dis.method_missing = mono->method_missing();
    InlineCacheEntry* entry = InlineCacheEntry::create(state, mono->receiver_data(),
                                                       mono->receiver_class(), dis);
    cache->set_cache(state, entry);
    return cache;
  }

  InlineCacheEntry* InlineCacheEntry::create(STATE, ClassData data, Class* klass, Dispatch& dis) {
    InlineCacheEntry* cache = state->new_object_dirty<InlineCacheEntry>(G(object));

    cache->receiver_ = data;
    cache->receiver_class(state, klass);
    cache->stored_module(state, dis.module);
    cache->method(state, dis.method);
    cache->method_missing_ = dis.method_missing;

    return cache;
  }

  Object* InlineCache::check_cache(STATE, CallSite* call_site, CallFrame* call_frame,
                                   Arguments& args)
  {
    Class* const recv_class = args.recv()->lookup_begin(state);

    InlineCacheEntry* entry;
    InlineCache* cache = static_cast<InlineCache*>(call_site);
    InlineCacheHit* ic = cache->get_inline_cache(recv_class, entry);

    if(likely(ic)) {
      Executable* meth = entry->method();
      Module* mod = entry->stored_module();
      ic->hit();

      return meth->execute(state, call_frame, meth, mod, args);
    }

    return cache->fallback(state, call_frame, args);
  }

  Object* InlineCache::check_cache_mm(STATE, CallSite* call_site, CallFrame* call_frame,
                                      Arguments& args)
  {
    Class* const recv_class = args.recv()->lookup_begin(state);

    InlineCacheEntry* entry;
    InlineCache* cache = static_cast<InlineCache*>(call_site);
    InlineCacheHit* ic = cache->get_inline_cache(recv_class, entry);

    if(likely(ic)) {
      if(entry->method_missing() != eNone) {
        args.unshift(state, call_site->name_);
        state->vm()->set_method_missing_reason(entry->method_missing());
      }
      Executable* meth = entry->method();
      Module* mod = entry->stored_module();
      ic->hit();

      return meth->execute(state, call_frame, meth, mod, args);
    }

    return cache->fallback(state, call_frame, args);
  }

  void InlineCache::inline_cache_updater(STATE, CallSite* call_site, Class* klass, FallbackExecutor fallback, Dispatch& dispatch) {
    InlineCache* cache = reinterpret_cast<InlineCache*>(call_site);
    InlineCacheEntry* entry = InlineCacheEntry::create(state, klass->data(), klass, dispatch);
    cache->set_cache(state, entry);
  }

  void InlineCache::print(STATE, std::ostream& stream) {
    stream << "name: " << name_->debug_str(state) << "\n"
           << "seen classes: " << classes_seen() << "\n"
           << "overflows: " << seen_classes_overflow_ << "\n"
           << "classes:\n";

    for(int i = 0; i < cTrackedICHits; i++) {
      if(cache_[i].entry()) {
        Module* mod = cache_[i].entry()->receiver_class();
        if(mod) {
          if(SingletonClass* sc = try_as<SingletonClass>(mod)) {
            if(Module* inner = try_as<Module>(sc->attached_instance())) {
              stream << "  SingletonClass:" << inner->debug_str(state);
            } else {
              stream << "  SingletonClass:" << sc->attached_instance()->class_object(state)->debug_str(state);
            }
          } else {
            stream << "  " << mod->debug_str(state);
          }

          stream << "\n";
        }
      }
    }
  }

  void InlineCache::Info::mark(Object* obj, ObjectMark& mark) {
    auto_mark(obj, mark);
    InlineCache* cache = static_cast<InlineCache*>(obj);

    for(int j = 0; j < cTrackedICHits; ++j) {
      InlineCacheEntry* ice = cache->cache_[j].entry();
      if(ice) {
        InlineCacheEntry* updated = static_cast<InlineCacheEntry*>(mark.call(ice));
        if(updated) {
          cache->cache_[j].update(updated);
          mark.just_set(cache, updated);
        }
      }
    }
  }

}