/*
 * Copyright (c) 2017, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "code/relocInfo.hpp"
#include "code/nmethod.hpp"
#include "code/icBuffer.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zNMethod.hpp"
#include "gc/z/zNMethodData.hpp"
#include "gc/z/zNMethodTable.hpp"
#include "gc/z/zOopClosures.inline.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zWorkers.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/atomic.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/debug.hpp"

static ZNMethodData* gc_data(const nmethod* nm) {
  return nm->gc_data<ZNMethodData>();
}

static void set_gc_data(nmethod* nm, ZNMethodData* data) {
  return nm->set_gc_data<ZNMethodData>(data);
}

void ZNMethod::attach_gc_data(nmethod* nm) {
  GrowableArray<oop*> immediate_oops;
  bool non_immediate_oops = false;

  // Find all oops relocations
  RelocIterator iter(nm);
  while (iter.next()) {
    if (iter.type() != relocInfo::oop_type) {
      // Not an oop
      continue;
    }

    oop_Relocation* r = iter.oop_reloc();

    if (!r->oop_is_immediate()) {
      // Non-immediate oop found
      non_immediate_oops = true;
      continue;
    }

    if (r->oop_value() != NULL) {
      // Non-NULL immediate oop found. NULL oops can safely be
      // ignored since the method will be re-registered if they
      // are later patched to be non-NULL.
      immediate_oops.push(r->oop_addr());
    }
  }

  // Attach GC data to nmethod
  ZNMethodData* data = gc_data(nm);
  if (data == NULL) {
    data = ZNMethodData::create(nm);
    set_gc_data(nm, data);
  }

  // Attach oops in GC data
  ZNMethodDataOops* const new_oops = ZNMethodDataOops::create(immediate_oops, non_immediate_oops);
  ZNMethodDataOops* const old_oops = data->swap_oops(new_oops);
  ZNMethodDataOops::destroy(old_oops);
}

void ZNMethod::detach_gc_data(nmethod* nm) {
  // Destroy GC data
  ZNMethodData::destroy(gc_data(nm));
  set_gc_data(nm, NULL);
}

ZReentrantLock* ZNMethod::lock_for_nmethod(nmethod* nm) {
  ZNMethodData* const data = gc_data(nm);
  if (data == NULL) {
    return NULL;
  }
  return data->lock();
}

void ZNMethod::log_register(const nmethod* nm) {
  LogTarget(Trace, gc, nmethod) log;
  if (!log.is_enabled()) {
    return;
  }

  const ZNMethodDataOops* const oops = gc_data(nm)->oops();

  log.print("Register NMethod: %s.%s (" PTR_FORMAT "), "
            "Compiler: %s, Oops: %d, ImmediateOops: " SIZE_FORMAT ", NonImmediateOops: %s",
            nm->method()->method_holder()->external_name(),
            nm->method()->name()->as_C_string(),
            p2i(nm),
            nm->compiler_name(),
            nm->oops_count() - 1,
            oops->immediates_count(),
            oops->has_non_immediates() ? "Yes" : "No");

  LogTarget(Trace, gc, nmethod, oops) log_oops;
  if (!log_oops.is_enabled()) {
    return;
  }

  // Print nmethod oops table
  {
    oop* const begin = nm->oops_begin();
    oop* const end = nm->oops_end();
    for (oop* p = begin; p < end; p++) {
      log_oops.print("           Oop[" SIZE_FORMAT "] " PTR_FORMAT " (%s)",
                     (p - begin), p2i(*p), (*p)->klass()->external_name());
    }
  }

  // Print nmethod immediate oops
  {
    oop** const begin = oops->immediates_begin();
    oop** const end = oops->immediates_end();
    for (oop** p = begin; p < end; p++) {
      log_oops.print("  ImmediateOop[" SIZE_FORMAT "] " PTR_FORMAT " @ " PTR_FORMAT " (%s)",
                     (p - begin), p2i(**p), p2i(*p), (**p)->klass()->external_name());
    }
  }
}

void ZNMethod::log_unregister(const nmethod* nm) {
  LogTarget(Debug, gc, nmethod) log;
  if (!log.is_enabled()) {
    return;
  }

  log.print("Unregister NMethod: %s.%s (" PTR_FORMAT ")",
            nm->method()->method_holder()->external_name(),
            nm->method()->name()->as_C_string(),
            p2i(nm));
}

void ZNMethod::register_nmethod(nmethod* nm) {
  ResourceMark rm;

  // Create and attach gc data
  attach_gc_data(nm);

  log_register(nm);

  ZNMethodTable::register_nmethod(nm);

  // Disarm nmethod entry barrier
  disarm_nmethod(nm);
}

void ZNMethod::unregister_nmethod(nmethod* nm) {
  assert(CodeCache_lock->owned_by_self(), "Lock must be held");

  if (Thread::current()->is_Code_cache_sweeper_thread()) {
    // The sweeper must wait for any ongoing iteration to complete
    // before it can unregister an nmethod.
    ZNMethodTable::wait_until_iteration_done();
  }

  ResourceMark rm;

  log_unregister(nm);

  ZNMethodTable::unregister_nmethod(nm);

  // Destroy and detach gc data
  detach_gc_data(nm);
}

void ZNMethod::disarm_nmethod(nmethod* nm) {
  BarrierSetNMethod* const bs = BarrierSet::barrier_set()->barrier_set_nmethod();
  if (bs != NULL) {
    bs->disarm(nm);
  }
}

void ZNMethod::nmethod_oops_do(nmethod* nm, OopClosure* cl) {
  // Process oops table
  {
    oop* const begin = nm->oops_begin();
    oop* const end = nm->oops_end();
    for (oop* p = begin; p < end; p++) {
      if (*p != Universe::non_oop_word()) {
        cl->do_oop(p);
      }
    }
  }

  ZNMethodDataOops* const oops = gc_data(nm)->oops();

  // Process immediate oops
  {
    oop** const begin = oops->immediates_begin();
    oop** const end = oops->immediates_end();
    for (oop** p = begin; p < end; p++) {
      if (**p != Universe::non_oop_word()) {
        cl->do_oop(*p);
      }
    }
  }

  // Process non-immediate oops
  if (oops->has_non_immediates()) {
    nm->fix_oop_relocations();
  }
}

class ZNMethodToOopsDoClosure : public NMethodClosure {
private:
  OopClosure* _cl;

public:
  ZNMethodToOopsDoClosure(OopClosure* cl) :
      _cl(cl) {}

  virtual void do_nmethod(nmethod* nm) {
    ZNMethod::nmethod_oops_do(nm, _cl);
  }
};

void ZNMethod::oops_do_begin() {
  ZNMethodTable::nmethods_do_begin();
}

void ZNMethod::oops_do_end() {
  ZNMethodTable::nmethods_do_end();
}

void ZNMethod::oops_do(OopClosure* cl) {
  ZNMethodToOopsDoClosure nmethod_cl(cl);
  ZNMethodTable::nmethods_do(&nmethod_cl);
}

class ZNMethodUnlinkClosure : public NMethodClosure {
private:
  bool          _unloading_occurred;
  volatile bool _failed;

  void set_failed() {
    Atomic::store(true, &_failed);
  }

public:
  ZNMethodUnlinkClosure(bool unloading_occurred) :
      _unloading_occurred(unloading_occurred),
      _failed(false) {}

  virtual void do_nmethod(nmethod* nm) {
    if (failed()) {
      return;
    }

    if (!nm->is_alive()) {
      return;
    }

    ZLocker<ZReentrantLock> locker(ZNMethod::lock_for_nmethod(nm));

    if (nm->is_unloading()) {
      // Unlinking of the dependencies must happen before the
      // handshake separating unlink and purge.
      nm->flush_dependencies(false /* delete_immediately */);

      // We don't need to take the lock when unlinking nmethods from
      // the Method, because it is only concurrently unlinked by
      // the entry barrier, which acquires the per nmethod lock.
      nm->unlink_from_method(false /* acquire_lock */);
      return;
    }

    // Heal oops and disarm
    ZNMethodOopClosure cl;
    ZNMethod::nmethod_oops_do(nm, &cl);
    ZNMethod::disarm_nmethod(nm);

    // Clear compiled ICs and exception caches
    if (!nm->unload_nmethod_caches(_unloading_occurred)) {
      set_failed();
    }
  }

  bool failed() const {
    return Atomic::load(&_failed);
  }
};

class ZNMethodUnlinkTask : public ZTask {
private:
  ZNMethodUnlinkClosure _cl;
  ICRefillVerifier*     _verifier;

public:
  ZNMethodUnlinkTask(bool unloading_occurred, ICRefillVerifier* verifier) :
      ZTask("ZNMethodUnlinkTask"),
      _cl(unloading_occurred),
      _verifier(verifier) {
    ZNMethodTable::nmethods_do_begin();
  }

  ~ZNMethodUnlinkTask() {
    ZNMethodTable::nmethods_do_end();
  }

  virtual void work() {
    ICRefillVerifierMark mark(_verifier);
    ZNMethodTable::nmethods_do(&_cl);
  }

  bool success() const {
    return !_cl.failed();
  }
};

void ZNMethod::unlink(ZWorkers* workers, bool unloading_occurred) {
  for (;;) {
    ICRefillVerifier verifier;

    {
      ZNMethodUnlinkTask task(unloading_occurred, &verifier);
      workers->run_concurrent(&task);
      if (task.success()) {
        return;
      }
    }

    // Cleaning failed because we ran out of transitional IC stubs,
    // so we have to refill and try again. Refilling requires taking
    // a safepoint, so we temporarily leave the suspendible thread set.
    SuspendibleThreadSetLeaver sts;
    InlineCacheBuffer::refill_ic_stubs();
  }
}

class ZNMethodPurgeClosure : public NMethodClosure {
public:
  virtual void do_nmethod(nmethod* nm) {
    if (nm->is_alive() && nm->is_unloading()) {
      nm->make_unloaded();
    }
  }
};

class ZNMethodPurgeTask : public ZTask {
private:
  ZNMethodPurgeClosure _cl;

public:
  ZNMethodPurgeTask() :
      ZTask("ZNMethodPurgeTask"),
      _cl() {
    ZNMethodTable::nmethods_do_begin();
  }

  ~ZNMethodPurgeTask() {
    ZNMethodTable::nmethods_do_end();
  }

  virtual void work() {
    ZNMethodTable::nmethods_do(&_cl);
  }
};

void ZNMethod::purge(ZWorkers* workers) {
  ZNMethodPurgeTask task;
  workers->run_concurrent(&task);
}
