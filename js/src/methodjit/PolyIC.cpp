/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "PolyIC.h"
#include "StubCalls.h"
#include "CodeGenIncludes.h"
#include "StubCalls-inl.h"
#include "BaseCompiler.h"
#include "assembler/assembler/LinkBuffer.h"
#include "TypedArrayIC.h"
#include "jsscope.h"
#include "jsnum.h"
#include "jstypedarray.h"
#include "jsatominlines.h"
#include "jsobjinlines.h"
#include "jsscopeinlines.h"
#include "jsinterpinlines.h"
#include "jsautooplen.h"

#include "vm/ScopeObject-inl.h"
#include "vm/StringObject-inl.h"

#if defined JS_POLYIC

using namespace js;
using namespace js::mjit;
using namespace js::mjit::ic;

typedef JSC::FunctionPtr FunctionPtr;
typedef JSC::MacroAssembler::RegisterID RegisterID;
typedef JSC::MacroAssembler::Jump Jump;
typedef JSC::MacroAssembler::Imm32 Imm32;

/* Rough over-estimate of how much memory we need to unprotect. */
static const uint32_t INLINE_PATH_LENGTH = 64;

// Helper class to simplify LinkBuffer usage in PIC stub generators.
// This guarantees correct OOM and refcount handling for buffers while they
// are instantiated and rooted.
class PICLinker : public LinkerHelper
{
    ic::BasePolyIC &ic;

  public:
    PICLinker(Assembler &masm, ic::BasePolyIC &ic)
      : LinkerHelper(masm, JSC::METHOD_CODE), ic(ic)
    { }

    bool init(JSContext *cx) {
        JSC::ExecutablePool *pool = LinkerHelper::init(cx);
        if (!pool)
            return false;
        if (!ic.addPool(cx, pool)) {
            markVerified();
            pool->release();
            js_ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }
};

class PICStubCompiler : public BaseCompiler
{
  protected:
    const char *type;
    VMFrame &f;
    JSScript *script;
    ic::PICInfo &pic;
    void *stub;
    uint64_t gcNumber;

  public:
    bool canCallHook;

    PICStubCompiler(const char *type, VMFrame &f, JSScript *script, ic::PICInfo &pic, void *stub)
      : BaseCompiler(f.cx), type(type), f(f), script(script), pic(pic), stub(stub),
        gcNumber(f.cx->runtime->gcNumber), canCallHook(pic.canCallHook)
    { }

    LookupStatus error() {
        /*
         * N.B. Do not try to disable the IC, we do not want to guard on
         * whether the IC has been recompiled when propagating errors.
         */
        return Lookup_Error;
    }

    LookupStatus error(JSContext *cx) {
        return error();
    }

    LookupStatus disable(const char *reason) {
        return disable(f.cx, reason);
    }

    LookupStatus disable(JSContext *cx, const char *reason) {
        return pic.disable(f, reason, stub);
    }

    LookupStatus disable(VMFrame &f, const char *reason) {
        return pic.disable(f, reason, stub);
    }

    bool hadGC() {
        return gcNumber != f.cx->runtime->gcNumber;
    }

  protected:
    void spew(const char *event, const char *op) {
#ifdef JS_METHODJIT_SPEW
        JaegerSpew(JSpew_PICs, "%s %s: %s (%s: %d)\n",
                   type, event, op, script->filename, CurrentLine(cx));
#endif
    }
};

static bool
GeneratePrototypeGuards(JSContext *cx, Vector<JSC::MacroAssembler::Jump,8> &mismatches, Assembler &masm,
                        JSObject *obj, JSObject *holder,
                        JSC::MacroAssembler::RegisterID objReg,
                        JSC::MacroAssembler::RegisterID scratchReg)
{
    typedef JSC::MacroAssembler::Address Address;
    typedef JSC::MacroAssembler::AbsoluteAddress AbsoluteAddress;
    typedef JSC::MacroAssembler::ImmPtr ImmPtr;
    typedef JSC::MacroAssembler::Jump Jump;

    if (obj->hasUncacheableProto()) {
        masm.loadPtr(Address(objReg, JSObject::offsetOfType()), scratchReg);
        Jump j = masm.branchPtr(Assembler::NotEqual,
                                Address(scratchReg, offsetof(types::TypeObject, proto)),
                                ImmPtr(obj->getProto()));
        if (!mismatches.append(j))
            return false;
    }

    JSObject *pobj = obj->getProto();
    while (pobj != holder) {
        if (pobj->hasUncacheableProto()) {
            Jump j;
            if (pobj->hasSingletonType()) {
                types::TypeObject *type = pobj->getType(cx);
                j = masm.branchPtr(Assembler::NotEqual,
                                   AbsoluteAddress(&type->proto),
                                   ImmPtr(pobj->getProto()),
                                   scratchReg);
            } else {
                j = masm.branchPtr(Assembler::NotEqual,
                                   AbsoluteAddress(pobj->addressOfType()),
                                   ImmPtr(pobj->type()),
                                   scratchReg);
            }
            if (!mismatches.append(j))
                return false;
        }
        pobj = pobj->getProto();
    }

    return true;
}

class SetPropCompiler : public PICStubCompiler
{
    JSObject *obj;
    PropertyName *name;
    int lastStubSecondShapeGuard;

  public:
    SetPropCompiler(VMFrame &f, JSScript *script, JSObject *obj, ic::PICInfo &pic, PropertyName *name,
                    VoidStubPIC stub)
      : PICStubCompiler("setprop", f, script, pic, JS_FUNC_TO_DATA_PTR(void *, stub)),
        obj(obj), name(name), lastStubSecondShapeGuard(pic.secondShapeGuard)
    { }

    static void reset(Repatcher &repatcher, ic::PICInfo &pic)
    {
        SetPropLabels &labels = pic.setPropLabels();
        repatcher.repatchLEAToLoadPtr(labels.getDslotsLoad(pic.fastPathRejoin, pic.u.vr));
        repatcher.repatch(labels.getInlineShapeData(pic.fastPathStart, pic.shapeGuard),
                          NULL);
        repatcher.relink(labels.getInlineShapeJump(pic.fastPathStart.labelAtOffset(pic.shapeGuard)),
                         pic.slowPathStart);

        FunctionPtr target(JS_FUNC_TO_DATA_PTR(void *, ic::SetProp));
        repatcher.relink(pic.slowPathCall, target);
    }

    LookupStatus patchInline(const Shape *shape)
    {
        JS_ASSERT(!pic.inlinePathPatched);
        JaegerSpew(JSpew_PICs, "patch setprop inline at %p\n", pic.fastPathStart.executableAddress());

        Repatcher repatcher(f.chunk());
        SetPropLabels &labels = pic.setPropLabels();

        int32_t offset;
        if (obj->isFixedSlot(shape->slot())) {
            CodeLocationInstruction istr = labels.getDslotsLoad(pic.fastPathRejoin, pic.u.vr);
            repatcher.repatchLoadPtrToLEA(istr);

            //
            // We've patched | mov dslots, [obj + DSLOTS_OFFSET]
            // To:           | lea fslots, [obj + DSLOTS_OFFSET]
            //
            // Because the offset is wrong, it's necessary to correct it
            // below.
            //
            int32_t diff = int32_t(JSObject::getFixedSlotOffset(0)) -
                         int32_t(JSObject::offsetOfSlots());
            JS_ASSERT(diff != 0);
            offset = (int32_t(shape->slot()) * sizeof(Value)) + diff;
        } else {
            offset = obj->dynamicSlotIndex(shape->slot()) * sizeof(Value);
        }

        repatcher.repatch(labels.getInlineShapeData(pic.fastPathStart, pic.shapeGuard),
                          obj->lastProperty());
        repatcher.patchAddressOffsetForValueStore(labels.getInlineValueStore(pic.fastPathRejoin),
                                                  offset, pic.u.vr.isTypeKnown());

        pic.inlinePathPatched = true;

        return Lookup_Cacheable;
    }

    int getLastStubSecondShapeGuard() const {
        return lastStubSecondShapeGuard ? POST_INST_OFFSET(lastStubSecondShapeGuard) : 0;
    }

    void patchPreviousToHere(CodeLocationLabel cs)
    {
        Repatcher repatcher(pic.lastCodeBlock(f.chunk()));
        CodeLocationLabel label = pic.lastPathStart();

        // Patch either the inline fast path or a generated stub. The stub
        // omits the prefix of the inline fast path that loads the shape, so
        // the offsets are different.
        if (pic.stubsGenerated) {
            repatcher.relink(pic.setPropLabels().getStubShapeJump(label), cs);
        } else {
            CodeLocationLabel shapeGuard = label.labelAtOffset(pic.shapeGuard);
            repatcher.relink(pic.setPropLabels().getInlineShapeJump(shapeGuard), cs);
        }
        if (int secondGuardOffset = getLastStubSecondShapeGuard())
            repatcher.relink(label.jumpAtOffset(secondGuardOffset), cs);
    }

    LookupStatus generateStub(const Shape *initialShape, const Shape *shape, bool adding)
    {
        if (hadGC())
            return Lookup_Uncacheable;

        /* Exits to the slow path. */
        Vector<Jump, 8> slowExits(cx);
        Vector<Jump, 8> otherGuards(cx);

        Assembler masm;

        // Shape guard.
        if (pic.shapeNeedsRemat()) {
            masm.loadShape(pic.objReg, pic.shapeReg);
            pic.shapeRegHasBaseShape = true;
        }

        Label start = masm.label();
        Jump shapeGuard = masm.branchPtr(Assembler::NotEqual, pic.shapeReg,
                                         ImmPtr(initialShape));

        Label stubShapeJumpLabel = masm.label();

        pic.setPropLabels().setStubShapeJump(masm, start, stubShapeJumpLabel);

        if (pic.typeMonitored) {
            /*
             * Inference does not know the type of the object being updated,
             * and we need to make sure that the updateMonitoredTypes() call
             * covers this stub, i.e. we will be writing to an object with the
             * same type. Add a type guard in addition to the shape guard.
             * Note: it is possible that this test gets a spurious hit if the
             * object has a lazy type, but in such cases no analyzed scripts
             * depend on the object and we will reconstruct its type from the
             * value being written here.
             */
            Jump typeGuard = masm.branchPtr(Assembler::NotEqual,
                                            Address(pic.objReg, JSObject::offsetOfType()),
                                            ImmPtr(obj->getType(cx)));
            if (!otherGuards.append(typeGuard))
                return error();
        }

        JS_ASSERT_IF(!shape->hasDefaultSetter(), obj->isCall());

        if (adding) {
            JS_ASSERT(shape->hasSlot());
            pic.shapeRegHasBaseShape = false;

            if (!GeneratePrototypeGuards(cx, otherGuards, masm, obj, NULL,
                                         pic.objReg, pic.shapeReg)) {
                return error();
            }

            /* Emit shape guards for the object's prototype chain. */
            JSObject *proto = obj->getProto();
            RegisterID lastReg = pic.objReg;
            while (proto) {
                masm.loadPtr(Address(lastReg, JSObject::offsetOfType()), pic.shapeReg);
                masm.loadPtr(Address(pic.shapeReg, offsetof(types::TypeObject, proto)), pic.shapeReg);
                Jump protoGuard = masm.guardShape(pic.shapeReg, proto);
                if (!otherGuards.append(protoGuard))
                    return error();

                proto = proto->getProto();
                lastReg = pic.shapeReg;
            }

            if (obj->isFixedSlot(shape->slot())) {
                Address address(pic.objReg,
                                JSObject::getFixedSlotOffset(shape->slot()));
                masm.storeValue(pic.u.vr, address);
            } else {
                /*
                 * Note: the guard on the initial shape determines the object's
                 * number of fixed slots and slot span, which in turn determine
                 * the number of dynamic slots allocated for the object.
                 * We don't need to check capacity here.
                 */
                masm.loadPtr(Address(pic.objReg, JSObject::offsetOfSlots()), pic.shapeReg);
                Address address(pic.shapeReg, obj->dynamicSlotIndex(shape->slot()) * sizeof(Value));
                masm.storeValue(pic.u.vr, address);
            }

            JS_ASSERT(shape == obj->lastProperty());
            JS_ASSERT(shape != initialShape);

            /* Write the object's new shape. */
            masm.storePtr(ImmPtr(shape), Address(pic.objReg, JSObject::offsetOfShape()));
        } else if (shape->hasDefaultSetter()) {
            Address address = masm.objPropAddress(obj, pic.objReg, shape->slot());
            masm.storeValue(pic.u.vr, address);
        } else {
            //   \ /        In general, two function objects with different JSFunctions
            //    #         can have the same shape, thus we must not rely on the identity
            // >--+--<      of 'fun' remaining the same. However, since:
            //   |||         1. the shape includes all arguments and locals and their setters
            //    \\     V     and getters, and
            //      \===/    2. arguments and locals have different getters
            //              then we can rely on fun->nargs remaining invariant.
            JSFunction *fun = obj->asCall().getCalleeFunction();
            uint16_t slot = uint16_t(shape->shortid());
            if (shape->setterOp() == CallObject::setVarOp)
                slot += fun->nargs;
            slot += CallObject::RESERVED_SLOTS;
            Address address = masm.objPropAddress(obj, pic.objReg, slot);
            masm.storeValue(pic.u.vr, address);

            pic.shapeRegHasBaseShape = false;
        }

        Jump done = masm.jump();

        // Common all secondary guards into one big exit.
        MaybeJump slowExit;
        if (otherGuards.length()) {
            for (Jump *pj = otherGuards.begin(); pj != otherGuards.end(); ++pj)
                pj->linkTo(masm.label(), &masm);
            slowExit = masm.jump();
            pic.secondShapeGuard = masm.distanceOf(masm.label()) - masm.distanceOf(start);
        } else {
            pic.secondShapeGuard = 0;
        }

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        buffer.link(shapeGuard, pic.slowPathStart);
        if (slowExit.isSet())
            buffer.link(slowExit.get(), pic.slowPathStart);
        for (Jump *pj = slowExits.begin(); pj != slowExits.end(); ++pj)
            buffer.link(*pj, pic.slowPathStart);
        buffer.link(done, pic.fastPathRejoin);
        CodeLocationLabel cs = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generate setprop stub %p %p %d at %p\n",
                   (void*)&pic,
                   (void*)initialShape,
                   pic.stubsGenerated,
                   cs.executableAddress());

        // This function can patch either the inline fast path for a generated
        // stub. The stub omits the prefix of the inline fast path that loads
        // the shape, so the offsets are different.
        patchPreviousToHere(cs);

        pic.stubsGenerated++;
        pic.updateLastPath(buffer, start);

        if (pic.stubsGenerated == MAX_PIC_STUBS)
            disable("max stubs reached");

        return Lookup_Cacheable;
    }

    bool updateMonitoredTypes()
    {
        JS_ASSERT(pic.typeMonitored);

        RecompilationMonitor monitor(cx);
        jsid id = NameToId(name);

        types::TypeObject *type = obj->getType(cx);
        if (monitor.recompiled())
            return false;

        if (!type->unknownProperties()) {
            types::AutoEnterTypeInference enter(cx);
            types::TypeSet *types = type->getProperty(cx, types::MakeTypeId(cx, id), true);
            if (!types)
                return false;
            pic.rhsTypes->addSubset(cx, types);
        }

        return !monitor.recompiled();
    }

    LookupStatus update()
    {
        JS_ASSERT(pic.hit);

        if (obj->isDenseArray())
            return disable("dense array");
        if (!obj->isNative())
            return disable("non-native");
        if (obj->watched())
            return disable("watchpoint");

        Class *clasp = obj->getClass();

        if (clasp->setProperty != JS_StrictPropertyStub)
            return disable("set property hook");
        if (clasp->ops.lookupProperty)
            return disable("ops lookup property hook");
        if (clasp->ops.setProperty)
            return disable("ops set property hook");

        JSObject *holder;
        JSProperty *prop = NULL;

        /* lookupProperty can trigger recompilations. */
        RecompilationMonitor monitor(cx);
        if (!obj->lookupProperty(cx, name, &holder, &prop))
            return error();
        if (monitor.recompiled())
            return Lookup_Uncacheable;

        /* If the property exists but is on a prototype, treat as addprop. */
        if (prop && holder != obj) {
            const Shape *shape = (const Shape *) prop;

            if (!holder->isNative())
                return disable("non-native holder");

            if (!shape->writable())
                return disable("readonly");
            if (!shape->hasDefaultSetter() || !shape->hasDefaultGetter())
                return disable("getter/setter in prototype");
            if (shape->hasShortID())
                return disable("short ID in prototype");
            if (!shape->hasSlot())
                return disable("missing slot");

            prop = NULL;
        }

        if (!prop) {
            /* Adding a property to the object. */
            if (obj->isDelegate())
                return disable("delegate");
            if (!obj->isExtensible())
                return disable("not extensible");

            if (clasp->addProperty != JS_PropertyStub)
                return disable("add property hook");
            if (clasp->ops.defineProperty)
                return disable("ops define property hook");

            /*
             * Don't add properties for SETNAME, which requires checks in
             * strict mode code.
             */
            if (JSOp(*f.pc()) == JSOP_SETNAME)
                return disable("add property under SETNAME");

            /*
             * When adding a property we need to check shapes along the entire
             * prototype chain to watch for an added setter.
             */
            JSObject *proto = obj;
            while (proto) {
                if (!proto->isNative())
                    return disable("non-native proto");
                proto = proto->getProto();
            }

            const Shape *initialShape = obj->lastProperty();
            uint32_t slots = obj->numDynamicSlots();

            unsigned flags = 0;
            PropertyOp getter = clasp->getProperty;

            /*
             * Define the property but do not set it yet. For setmethod,
             * populate the slot to satisfy the method invariant (in case we
             * hit an early return below).
             */
            const Shape *shape =
                obj->putProperty(cx, name, getter, clasp->setProperty,
                                 SHAPE_INVALID_SLOT, JSPROP_ENUMERATE, flags, 0);
            if (!shape)
                return error();

            if (monitor.recompiled())
                return Lookup_Uncacheable;

            /*
             * Test after calling putProperty since it can switch obj into
             * dictionary mode, specifically if the shape tree ancestor line
             * exceeds PropertyTree::MAX_HEIGHT.
             */
            if (obj->inDictionaryMode())
                return disable("dictionary");

            if (!shape->hasDefaultSetter())
                return disable("adding non-default setter");
            if (!shape->hasSlot())
                return disable("adding invalid slot");

            /*
             * Watch for cases where the object reallocated its slots when
             * adding the property, and disable the PIC.  Otherwise we will
             * keep generating identical PICs as side exits are taken on the
             * capacity checks.  Alternatively, we could avoid the disable
             * and just not generate a stub in case there are multiple shapes
             * that can flow here which don't all require reallocation.
             * Doing this would cause us to walk down this same update path
             * every time a reallocation is needed, however, which will
             * usually be a slowdown even if there *are* other shapes that
             * don't realloc.
             */
            if (obj->numDynamicSlots() != slots)
                return disable("insufficient slot capacity");

            if (pic.typeMonitored && !updateMonitoredTypes())
                return Lookup_Uncacheable;

            return generateStub(initialShape, shape, true);
        }

        const Shape *shape = (const Shape *) prop;
        if (!shape->writable())
            return disable("readonly");
        if (shape->hasDefaultSetter()) {
            if (!shape->hasSlot())
                return disable("invalid slot");
            if (pic.typeMonitored && !updateMonitoredTypes())
                return Lookup_Uncacheable;
        } else {
            if (shape->hasSetterValue())
                return disable("scripted setter");
            if (shape->setterOp() != CallObject::setArgOp &&
                shape->setterOp() != CallObject::setVarOp) {
                return disable("setter");
            }
            JS_ASSERT(obj->isCall());
            if (pic.typeMonitored) {
                /*
                 * Update the types of the locals/args in the script according
                 * to the possible RHS types of the assignment. Note that the
                 * shape guards we have performed do not by themselves
                 * guarantee that future call objects hit will be for the same
                 * script. We also depend on the fact that the scope chains hit
                 * at the same bytecode are all isomorphic: the same scripts,
                 * in the same order (though the properties on their call
                 * objects may differ due to eval(), DEFFUN, etc.).
                 */
                RecompilationMonitor monitor(cx);
                JSFunction *fun = obj->asCall().getCalleeFunction();
                JSScript *script = fun->script();
                uint16_t slot = uint16_t(shape->shortid());
                if (!script->ensureHasTypes(cx))
                    return error();
                {
                    types::AutoEnterTypeInference enter(cx);
                    if (shape->setterOp() == CallObject::setArgOp)
                        pic.rhsTypes->addSubset(cx, types::TypeScript::ArgTypes(script, slot));
                    else
                        pic.rhsTypes->addSubset(cx, types::TypeScript::LocalTypes(script, slot));
                }
                if (monitor.recompiled())
                    return Lookup_Uncacheable;
            }
        }

        JS_ASSERT(obj == holder);
        if (!pic.inlinePathPatched &&
            shape->hasDefaultSetter() &&
            !pic.typeMonitored &&
            !obj->isDenseArray()) {
            return patchInline(shape);
        }

        return generateStub(obj->lastProperty(), shape, false);
    }
};

static bool
IsCacheableProtoChain(JSObject *obj, JSObject *holder)
{
    while (obj != holder) {
        /*
         * We cannot assume that we find the holder object on the prototype
         * chain and must check for null proto. The prototype chain can be
         * altered during the lookupProperty call.
         */
        JSObject *proto = obj->getProto();
        if (!proto || !proto->isNative())
            return false;
        obj = proto;
    }
    return true;
}

template <typename IC>
struct GetPropHelper {
    // These fields are set in the constructor and describe a property lookup.
    JSContext   *cx;
    JSObject    *obj;
    RootedPropertyName name;
    IC          &ic;
    VMFrame     &f;

    // These fields are set by |bind| and |lookup|. After a call to either
    // function, these are set exactly as they are in JSOP_GETPROP or JSOP_NAME.
    JSObject    *holder;
    JSProperty  *prop;

    // This field is set by |bind| and |lookup| only if they returned
    // Lookup_Cacheable, otherwise it is NULL.
    const Shape *shape;

    GetPropHelper(JSContext *cx, JSObject *obj, PropertyName *name, IC &ic, VMFrame &f)
      : cx(cx), obj(obj), name(cx, name), ic(ic), f(f), holder(NULL), prop(NULL), shape(NULL)
    { }

  public:
    LookupStatus bind() {
        RecompilationMonitor monitor(cx);
        RootedObject scopeChain(cx, cx->stack.currentScriptedScopeChain());
        if (js_CodeSpec[*f.pc()].format & JOF_GNAME)
            scopeChain = &scopeChain->global();
        if (!FindProperty(cx, name, scopeChain, &obj, &holder, &prop))
            return ic.error(cx);
        if (monitor.recompiled())
            return Lookup_Uncacheable;
        if (!prop)
            return ic.disable(cx, "lookup failed");
        if (!obj->isNative())
            return ic.disable(cx, "non-native");
        if (!IsCacheableProtoChain(obj, holder))
            return ic.disable(cx, "non-native holder");
        shape = (const Shape *)prop;
        return Lookup_Cacheable;
    }

    LookupStatus lookup() {
        JSObject *aobj = obj;
        if (obj->isDenseArray())
            aobj = obj->getProto();
        if (!aobj->isNative())
            return ic.disable(f, "non-native");

        RecompilationMonitor monitor(cx);
        if (!aobj->lookupProperty(cx, name, &holder, &prop))
            return ic.error(cx);
        if (monitor.recompiled())
            return Lookup_Uncacheable;

        if (!prop)
            return ic.disable(f, "lookup failed");
        if (!IsCacheableProtoChain(obj, holder))
            return ic.disable(f, "non-native holder");
        shape = (const Shape *)prop;
        return Lookup_Cacheable;
    }

    LookupStatus testForGet() {
        if (!shape->hasDefaultGetter()) {
            if (shape->hasGetterValue()) {
                JSObject *getterObj = shape->getterObject();
                if (!getterObj->isFunction() || !getterObj->toFunction()->isNative())
                    return ic.disable(f, "getter object not a native function");
            }
            if (shape->hasSlot() && holder != obj)
                return ic.disable(f, "slotful getter hook through prototype");
            if (!ic.canCallHook)
                return ic.disable(f, "can't call getter hook");
            if (f.regs.inlined()) {
                /*
                 * As with native stubs, getter hook stubs can't be
                 * generated for inline frames. Mark the inner function
                 * as uninlineable and recompile.
                 */
                f.script()->uninlineable = true;
                MarkTypeObjectFlags(cx, f.script()->function(),
                                    types::OBJECT_FLAG_UNINLINEABLE);
                return Lookup_Uncacheable;
            }
        } else if (!shape->hasSlot()) {
            return ic.disable(f, "no slot");
        }

        return Lookup_Cacheable;
    }

    LookupStatus lookupAndTest() {
        LookupStatus status = lookup();
        if (status != Lookup_Cacheable)
            return status;
        return testForGet();
    }
};

namespace js {
namespace mjit {

class GetPropCompiler : public PICStubCompiler
{
    JSObject    *obj;
    PropertyName *name;
    int         lastStubSecondShapeGuard;

  public:
    GetPropCompiler(VMFrame &f, JSScript *script, JSObject *obj, ic::PICInfo &pic, PropertyName *name,
                    VoidStubPIC stub)
      : PICStubCompiler("getprop", f, script, pic,
                        JS_FUNC_TO_DATA_PTR(void *, stub)),
        obj(obj),
        name(name),
        lastStubSecondShapeGuard(pic.secondShapeGuard)
    { }

    int getLastStubSecondShapeGuard() const {
        return lastStubSecondShapeGuard ? POST_INST_OFFSET(lastStubSecondShapeGuard) : 0;
    }

    static void reset(Repatcher &repatcher, ic::PICInfo &pic)
    {
        GetPropLabels &labels = pic.getPropLabels();
        repatcher.repatchLEAToLoadPtr(labels.getDslotsLoad(pic.fastPathRejoin));
        repatcher.repatch(labels.getInlineShapeData(pic.getFastShapeGuard()), NULL);
        repatcher.relink(labels.getInlineShapeJump(pic.getFastShapeGuard()), pic.slowPathStart);

        if (pic.hasTypeCheck()) {
            /* TODO: combine pic.u.get into ICLabels? */
            repatcher.relink(labels.getInlineTypeJump(pic.fastPathStart), pic.getSlowTypeCheck());
        }

        JS_ASSERT(pic.kind == ic::PICInfo::GET);

        FunctionPtr target(JS_FUNC_TO_DATA_PTR(void *, ic::GetProp));
        repatcher.relink(pic.slowPathCall, target);
    }

    LookupStatus generateArrayLengthStub()
    {
        Assembler masm;

        masm.loadObjClass(pic.objReg, pic.shapeReg);
        Jump isDense = masm.testClass(Assembler::Equal, pic.shapeReg, &ArrayClass);
        Jump notArray = masm.testClass(Assembler::NotEqual, pic.shapeReg, &SlowArrayClass);

        isDense.linkTo(masm.label(), &masm);
        masm.loadPtr(Address(pic.objReg, JSObject::offsetOfElements()), pic.objReg);
        masm.load32(Address(pic.objReg, ObjectElements::offsetOfLength()), pic.objReg);
        Jump oob = masm.branch32(Assembler::Above, pic.objReg, Imm32(JSVAL_INT_MAX));
        masm.move(ImmType(JSVAL_TYPE_INT32), pic.shapeReg);
        Jump done = masm.jump();

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        buffer.link(notArray, pic.slowPathStart);
        buffer.link(oob, pic.slowPathStart);
        buffer.link(done, pic.fastPathRejoin);

        CodeLocationLabel start = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generate array length stub at %p\n",
                   start.executableAddress());

        patchPreviousToHere(start);

        disable("array length done");

        return Lookup_Cacheable;
    }

    LookupStatus generateStringObjLengthStub()
    {
        Assembler masm;

        Jump notStringObj = masm.guardShape(pic.objReg, obj);

        masm.loadPayload(Address(pic.objReg, StringObject::getPrimitiveValueOffset()), pic.objReg);
        masm.loadPtr(Address(pic.objReg, JSString::offsetOfLengthAndFlags()), pic.objReg);
        masm.urshift32(Imm32(JSString::LENGTH_SHIFT), pic.objReg);
        masm.move(ImmType(JSVAL_TYPE_INT32), pic.shapeReg);
        Jump done = masm.jump();

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        buffer.link(notStringObj, pic.slowPathStart);
        buffer.link(done, pic.fastPathRejoin);

        CodeLocationLabel start = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generate string object length stub at %p\n",
                   start.executableAddress());

        patchPreviousToHere(start);

        disable("string object length done");

        return Lookup_Cacheable;
    }

    LookupStatus generateStringPropertyStub()
    {
        if (!f.fp()->script()->hasGlobal())
            return disable("String.prototype without compile-and-go global");

        RecompilationMonitor monitor(f.cx);

        JSObject *obj = f.fp()->global().getOrCreateStringPrototype(f.cx);
        if (!obj)
            return error();

        if (monitor.recompiled())
            return Lookup_Uncacheable;

        GetPropHelper<GetPropCompiler> getprop(cx, obj, name, *this, f);
        LookupStatus status = getprop.lookupAndTest();
        if (status != Lookup_Cacheable)
            return status;
        if (getprop.obj != getprop.holder)
            return disable("proto walk on String.prototype");
        if (!getprop.shape->hasDefaultGetter())
            return disable("getter hook on String.prototype");
        if (hadGC())
            return Lookup_Uncacheable;

        Assembler masm;

        /* Only strings are allowed. */
        Jump notString = masm.branchPtr(Assembler::NotEqual, pic.typeReg(),
                                        ImmType(JSVAL_TYPE_STRING));

        /*
         * Clobber objReg with String.prototype and do some PIC stuff. Well,
         * really this is now a MIC, except it won't ever be patched, so we
         * just disable the PIC at the end. :FIXME:? String.prototype probably
         * does not get random shape changes.
         */
        masm.move(ImmPtr(obj), pic.objReg);
        masm.loadShape(pic.objReg, pic.shapeReg);
        Jump shapeMismatch = masm.branchPtr(Assembler::NotEqual, pic.shapeReg,
                                            ImmPtr(obj->lastProperty()));
        masm.loadObjProp(obj, pic.objReg, getprop.shape, pic.shapeReg, pic.objReg);

        Jump done = masm.jump();

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        buffer.link(notString, pic.getSlowTypeCheck());
        buffer.link(shapeMismatch, pic.slowPathStart);
        buffer.link(done, pic.fastPathRejoin);

        CodeLocationLabel cs = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generate string call stub at %p\n",
                   cs.executableAddress());

        /* Patch the type check to jump here. */
        if (pic.hasTypeCheck()) {
            Repatcher repatcher(f.chunk());
            repatcher.relink(pic.getPropLabels().getInlineTypeJump(pic.fastPathStart), cs);
        }

        /* Disable the PIC so we don't keep generating stubs on the above shape mismatch. */
        disable("generated string call stub");
        return Lookup_Cacheable;
    }

    LookupStatus generateStringLengthStub()
    {
        JS_ASSERT(pic.hasTypeCheck());

        Assembler masm;
        Jump notString = masm.branchPtr(Assembler::NotEqual, pic.typeReg(),
                                        ImmType(JSVAL_TYPE_STRING));
        masm.loadPtr(Address(pic.objReg, JSString::offsetOfLengthAndFlags()), pic.objReg);
        // String length is guaranteed to be no more than 2**28, so the 32-bit operation is OK.
        masm.urshift32(Imm32(JSString::LENGTH_SHIFT), pic.objReg);
        masm.move(ImmType(JSVAL_TYPE_INT32), pic.shapeReg);
        Jump done = masm.jump();

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        buffer.link(notString, pic.getSlowTypeCheck());
        buffer.link(done, pic.fastPathRejoin);

        CodeLocationLabel start = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generate string length stub at %p\n",
                   start.executableAddress());

        if (pic.hasTypeCheck()) {
            Repatcher repatcher(f.chunk());
            repatcher.relink(pic.getPropLabels().getInlineTypeJump(pic.fastPathStart), start);
        }

        disable("generated string length stub");

        return Lookup_Cacheable;
    }

    LookupStatus patchInline(JSObject *holder, const Shape *shape)
    {
        spew("patch", "inline");
        Repatcher repatcher(f.chunk());
        GetPropLabels &labels = pic.getPropLabels();

        int32_t offset;
        if (holder->isFixedSlot(shape->slot())) {
            CodeLocationInstruction istr = labels.getDslotsLoad(pic.fastPathRejoin);
            repatcher.repatchLoadPtrToLEA(istr);

            //
            // We've patched | mov dslots, [obj + DSLOTS_OFFSET]
            // To:           | lea fslots, [obj + DSLOTS_OFFSET]
            //
            // Because the offset is wrong, it's necessary to correct it
            // below.
            //
            int32_t diff = int32_t(JSObject::getFixedSlotOffset(0)) -
                         int32_t(JSObject::offsetOfSlots());
            JS_ASSERT(diff != 0);
            offset  = (int32_t(shape->slot()) * sizeof(Value)) + diff;
        } else {
            offset = holder->dynamicSlotIndex(shape->slot()) * sizeof(Value);
        }

        repatcher.repatch(labels.getInlineShapeData(pic.getFastShapeGuard()), obj->lastProperty());
        repatcher.patchAddressOffsetForValueLoad(labels.getValueLoad(pic.fastPathRejoin), offset);

        pic.inlinePathPatched = true;

        return Lookup_Cacheable;
    }

    /* For JSPropertyOp getters. */
    void generateGetterStub(Assembler &masm, const Shape *shape, jsid userid,
                            Label start, Vector<Jump, 8> &shapeMismatches)
    {
        /*
         * Getter hook needs to be called from the stub. The state is fully
         * synced and no registers are live except the result registers.
         */
        JS_ASSERT(pic.canCallHook);
        PropertyOp getter = shape->getterOp();

        masm.storePtr(ImmPtr((void *) REJOIN_NATIVE_GETTER),
                      FrameAddress(offsetof(VMFrame, stubRejoin)));

        Registers tempRegs = Registers::tempCallRegMask();
        if (tempRegs.hasReg(Registers::ClobberInCall))
            tempRegs.takeReg(Registers::ClobberInCall);

        /* Get a register to hold obj while we set up the rest of the frame. */
        RegisterID holdObjReg = pic.objReg;
        if (tempRegs.hasReg(pic.objReg)) {
            tempRegs.takeReg(pic.objReg);
        } else {
            holdObjReg = tempRegs.takeAnyReg().reg();
            masm.move(pic.objReg, holdObjReg);
        }

        RegisterID t0 = tempRegs.takeAnyReg().reg();
        masm.bumpStubCount(f.script(), f.pc(), t0);

        /*
         * Use three values above sp on the stack for use by the call to store
         * the object and id being passed into the call as handles and to store
         * the resulting value. Temporary slots are used by GETPROP for this,
         * plus there is extra room on the stack reserved for a callee frame.
         */
        int32_t initialFrameDepth = f.regs.sp - f.fp()->slots() + 3;
        int32_t vpOffset = (char *) f.regs.sp - (char *) f.fp();
        int32_t idHandleOffset = (char *) (f.regs.sp + 1) - (char *) f.fp();
        int32_t objHandleOffset = (char *) (f.regs.sp + 2) - (char *) f.fp();

        masm.storePtr(holdObjReg, Address(JSFrameReg, objHandleOffset));
        masm.storePtr(ImmPtr((void *) JSID_BITS(userid)), Address(JSFrameReg, idHandleOffset));

        /*
         * On 32 bit platforms zero the upper portion of the values so that
         * the GC does not see a corrupt value in the handle slots. The two
         * slots will look like doubles, so won't be traced, but the objects
         * will be held live by the object value still in place on the stack.
         * This will need to be addressed once a moving GC can relocate the
         * objects, as the created handles will need to be properly registered.
         */
#if JS_BITS_PER_WORD == 32
        masm.storePtr(ImmPtr(NULL), masm.tagOf(Address(JSFrameReg, objHandleOffset)));
        masm.storePtr(ImmPtr(NULL), masm.tagOf(Address(JSFrameReg, idHandleOffset)));
#endif

        if (shape->hasSlot()) {
            masm.loadObjProp(obj, holdObjReg, shape,
                             Registers::ClobberInCall, t0);
            masm.storeValueFromComponents(Registers::ClobberInCall, t0, Address(JSFrameReg, vpOffset));
        } else {
            masm.storeValue(UndefinedValue(), Address(JSFrameReg, vpOffset));
        }

        masm.setupFallibleABICall(cx->typeInferenceEnabled(), f.regs.pc, initialFrameDepth);

        /* Grab cx. */
#ifdef JS_CPU_X86
        RegisterID cxReg = tempRegs.takeAnyReg().reg();
#else
        RegisterID cxReg = Registers::ArgReg0;
#endif
        masm.loadPtr(FrameAddress(offsetof(VMFrame, cx)), cxReg);

        /* Use a temporary for parameters. */
        masm.addPtr(Imm32(vpOffset), JSFrameReg, t0);

        masm.restoreStackBase();
        masm.setupABICall(Registers::NormalCall, 4);
        masm.storeArg(3, t0);
        masm.addPtr(Imm32(idHandleOffset - vpOffset), t0);
        masm.storeArg(2, t0);
        masm.addPtr(Imm32(objHandleOffset - idHandleOffset), t0);
        masm.storeArg(1, t0);
        masm.storeArg(0, cxReg);

        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void *, getter), false);

        NativeStubLinker::FinalJump done;
        if (!NativeStubEpilogue(f, masm, &done, 0, vpOffset, pic.shapeReg, pic.objReg))
            return;
        NativeStubLinker linker(masm, f.chunk(), f.regs.pc, done);
        if (!linker.init(f.cx))
            THROW();

        if (!linker.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !linker.verifyRange(f.chunk())) {
            disable("code memory is out of range");
            return;
        }

        linker.patchJump(pic.fastPathRejoin);

        linkerEpilogue(linker, start, shapeMismatches);
    }

    /* For getters backed by a JSNative. */
    void generateNativeGetterStub(Assembler &masm, const Shape *shape,
                                  Label start, Vector<Jump, 8> &shapeMismatches)
    {
        /*
         * Getter hook needs to be called from the stub. The state is fully
         * synced and no registers are live except the result registers.
         */
        JS_ASSERT(pic.canCallHook);

        JSFunction *fun = shape->getterObject()->toFunction();
        Native native = fun->native();

        masm.storePtr(ImmPtr((void *) REJOIN_NATIVE_GETTER),
                      FrameAddress(offsetof(VMFrame, stubRejoin)));

        Registers tempRegs = Registers::tempCallRegMask();
        if (tempRegs.hasReg(Registers::ClobberInCall))
            tempRegs.takeReg(Registers::ClobberInCall);

        /* Get a register to hold obj while we set up the rest of the frame. */
        RegisterID holdObjReg = pic.objReg;
        if (tempRegs.hasReg(pic.objReg)) {
            tempRegs.takeReg(pic.objReg);
        } else {
            holdObjReg = tempRegs.takeAnyReg().reg();
            masm.move(pic.objReg, holdObjReg);
        }

        RegisterID t0 = tempRegs.takeAnyReg().reg();
        masm.bumpStubCount(f.script(), f.pc(), t0);

        /*
         * A JSNative has the following signature:
         *
         *   JSBool native(JSContext *cx, unsigned argc, Value *vp);
         *
         * Since we are calling a getter, argc is always 0. vp must point to two
         * values, the callee and the holder. We use vp == sp to avoid clobbering
         * stack values.
         */
        int32_t vpOffset = (char *) f.regs.sp - (char *) f.fp();

        masm.storeValue(ObjectValue(*fun), Address(JSFrameReg, vpOffset));
        masm.storeValueFromComponents(ImmType(JSVAL_TYPE_OBJECT), holdObjReg,
                                      Address(JSFrameReg, vpOffset + sizeof(js::Value)));

        /*
         * sp + 2 to avoid clobbering vp[0] and vp[1] if the getter calls
         * scripted functions.
         */
        int32_t initialFrameDepth = f.regs.sp + 2 - f.fp()->slots();
        masm.setupFallibleABICall(cx->typeInferenceEnabled(), f.regs.pc, initialFrameDepth);

        /* Grab cx. */
#ifdef JS_CPU_X86
        RegisterID cxReg = tempRegs.takeAnyReg().reg();
#else
        RegisterID cxReg = Registers::ArgReg0;
#endif
        masm.loadPtr(FrameAddress(offsetof(VMFrame, cx)), cxReg);

        /* Grab vp. */
        RegisterID vpReg = t0;
        masm.addPtr(Imm32(vpOffset), JSFrameReg, vpReg);

        masm.restoreStackBase();
        masm.setupABICall(Registers::NormalCall, 3);
        masm.storeArg(2, vpReg);
        masm.storeArg(1, Imm32(0)); // argc
        masm.storeArg(0, cxReg);

        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void *, native), false);

        NativeStubLinker::FinalJump done;
        if (!NativeStubEpilogue(f, masm, &done, 0, vpOffset, pic.shapeReg, pic.objReg))
            return;
        NativeStubLinker linker(masm, f.chunk(), f.regs.pc, done);
        if (!linker.init(f.cx))
            THROW();

        if (!linker.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !linker.verifyRange(f.chunk())) {
            disable("code memory is out of range");
            return;
        }

        linker.patchJump(pic.fastPathRejoin);

        linkerEpilogue(linker, start, shapeMismatches);
    }

    LookupStatus generateStub(JSObject *holder, const Shape *shape)
    {
        Vector<Jump, 8> shapeMismatches(cx);

        Assembler masm;

        Label start;
        Jump shapeGuardJump;
        Jump argsLenGuard;

        bool setStubShapeOffset = true;
        if (obj->isDenseArray()) {
            start = masm.label();
            shapeGuardJump = masm.branchPtr(Assembler::NotEqual,
                                            Address(pic.objReg, JSObject::offsetOfShape()),
                                            ImmPtr(obj->lastProperty()));

            /*
             * No need to assert validity of GETPROP_STUB_SHAPE_JUMP in this case:
             * the IC is disabled after a dense array hit, so no patching can occur.
             */
#ifndef JS_HAS_IC_LABELS
            setStubShapeOffset = false;
#endif
        } else {
            if (pic.shapeNeedsRemat()) {
                masm.loadShape(pic.objReg, pic.shapeReg);
                pic.shapeRegHasBaseShape = true;
            }

            start = masm.label();
            shapeGuardJump = masm.branchPtr(Assembler::NotEqual, pic.shapeReg,
                                            ImmPtr(obj->lastProperty()));
        }
        Label stubShapeJumpLabel = masm.label();

        if (!shapeMismatches.append(shapeGuardJump))
            return error();

        RegisterID holderReg = pic.objReg;
        if (obj != holder) {
            if (!GeneratePrototypeGuards(cx, shapeMismatches, masm, obj, holder,
                                         pic.objReg, pic.shapeReg)) {
                return error();
            }

            // Bake in the holder identity. Careful not to clobber |objReg|, since we can't remat it.
            holderReg = pic.shapeReg;
            masm.move(ImmPtr(holder), holderReg);
            pic.shapeRegHasBaseShape = false;

            // Guard on the holder's shape.
            Jump j = masm.guardShape(holderReg, holder);
            if (!shapeMismatches.append(j))
                return error();

            pic.secondShapeGuard = masm.distanceOf(masm.label()) - masm.distanceOf(start);
        } else {
            pic.secondShapeGuard = 0;
        }

        if (!shape->hasDefaultGetter()) {
            if (shape->hasGetterValue()) {
                generateNativeGetterStub(masm, shape, start, shapeMismatches);
            } else {
                jsid userid;
                if (!shape->getUserId(cx, &userid))
                    return error();
                generateGetterStub(masm, shape, userid, start, shapeMismatches);
            }
            if (setStubShapeOffset)
                pic.getPropLabels().setStubShapeJump(masm, start, stubShapeJumpLabel);
            return Lookup_Cacheable;
        }

        /* Load the value out of the object. */
        masm.loadObjProp(holder, holderReg, shape, pic.shapeReg, pic.objReg);
        Jump done = masm.jump();

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        // The final exit jumps to the store-back in the inline stub.
        buffer.link(done, pic.fastPathRejoin);

        linkerEpilogue(buffer, start, shapeMismatches);

        if (setStubShapeOffset)
            pic.getPropLabels().setStubShapeJump(masm, start, stubShapeJumpLabel);
        return Lookup_Cacheable;
    }

    void linkerEpilogue(LinkerHelper &buffer, Label start, Vector<Jump, 8> &shapeMismatches)
    {
        // The guard exit jumps to the original slow case.
        for (Jump *pj = shapeMismatches.begin(); pj != shapeMismatches.end(); ++pj)
            buffer.link(*pj, pic.slowPathStart);

        CodeLocationLabel cs = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generated %s stub at %p\n", type, cs.executableAddress());

        patchPreviousToHere(cs);

        pic.stubsGenerated++;
        pic.updateLastPath(buffer, start);

        if (pic.stubsGenerated == MAX_PIC_STUBS)
            disable("max stubs reached");
        if (obj->isDenseArray())
            disable("dense array");
    }

    void patchPreviousToHere(CodeLocationLabel cs)
    {
        Repatcher repatcher(pic.lastCodeBlock(f.chunk()));
        CodeLocationLabel label = pic.lastPathStart();

        // Patch either the inline fast path or a generated stub. The stub
        // omits the prefix of the inline fast path that loads the shape, so
        // the offsets are different.
        int shapeGuardJumpOffset;
        if (pic.stubsGenerated)
            shapeGuardJumpOffset = pic.getPropLabels().getStubShapeJumpOffset();
        else
            shapeGuardJumpOffset = pic.shapeGuard + pic.getPropLabels().getInlineShapeJumpOffset();
        int secondGuardOffset = getLastStubSecondShapeGuard();

        JaegerSpew(JSpew_PICs, "Patching previous (%d stubs) (start %p) (offset %d) (second %d)\n",
                   (int) pic.stubsGenerated, label.executableAddress(),
                   shapeGuardJumpOffset, secondGuardOffset);

        repatcher.relink(label.jumpAtOffset(shapeGuardJumpOffset), cs);
        if (secondGuardOffset)
            repatcher.relink(label.jumpAtOffset(secondGuardOffset), cs);
    }

    LookupStatus update()
    {
        JS_ASSERT(pic.hit);

        GetPropHelper<GetPropCompiler> getprop(cx, obj, name, *this, f);
        LookupStatus status = getprop.lookupAndTest();
        if (status != Lookup_Cacheable)
            return status;
        if (hadGC())
            return Lookup_Uncacheable;

        if (obj == getprop.holder &&
            getprop.shape->hasDefaultGetter() &&
            !pic.inlinePathPatched) {
            return patchInline(getprop.holder, getprop.shape);
        }

        return generateStub(getprop.holder, getprop.shape);
    }
};

}  // namespace mjit
}  // namespace js

class ScopeNameCompiler : public PICStubCompiler
{
  private:
    typedef Vector<Jump, 8> JumpList;

    RootedObject scopeChain;
    RootedPropertyName name;
    GetPropHelper<ScopeNameCompiler> getprop;
    ScopeNameCompiler *thisFromCtor() { return this; }

    void patchPreviousToHere(CodeLocationLabel cs)
    {
        ScopeNameLabels &       labels = pic.scopeNameLabels();
        Repatcher               repatcher(pic.lastCodeBlock(f.chunk()));
        CodeLocationLabel       start = pic.lastPathStart();
        JSC::CodeLocationJump   jump;

        // Patch either the inline fast path or a generated stub.
        if (pic.stubsGenerated)
            jump = labels.getStubJump(start);
        else
            jump = labels.getInlineJump(start);
        repatcher.relink(jump, cs);
    }

    LookupStatus walkScopeChain(Assembler &masm, JumpList &fails)
    {
        /* Walk the scope chain. */
        JSObject *tobj = scopeChain;

        /* For GETXPROP, we'll never enter this loop. */
        JS_ASSERT_IF(pic.kind == ic::PICInfo::XNAME, tobj && tobj == getprop.holder);
        JS_ASSERT_IF(pic.kind == ic::PICInfo::XNAME, getprop.obj == tobj);

        while (tobj && tobj != getprop.holder) {
            if (!IsCacheableNonGlobalScope(tobj))
                return disable("non-cacheable scope chain object");
            JS_ASSERT(tobj->isNative());

            /* Guard on intervening shapes. */
            masm.loadShape(pic.objReg, pic.shapeReg);
            Jump j = masm.branchPtr(Assembler::NotEqual, pic.shapeReg,
                                    ImmPtr(tobj->lastProperty()));
            if (!fails.append(j))
                return error();

            /* Load the next link in the scope chain. */
            Address parent(pic.objReg, ScopeObject::offsetOfEnclosingScope());
            masm.loadPayload(parent, pic.objReg);

            tobj = &tobj->asScope().enclosingScope();
        }

        if (tobj != getprop.holder)
            return disable("scope chain walk terminated early");

        return Lookup_Cacheable;
    }

  public:
    ScopeNameCompiler(VMFrame &f, JSScript *script, JSObject *scopeChain, ic::PICInfo &pic,
                      PropertyName *name, VoidStubPIC stub)
      : PICStubCompiler("name", f, script, pic, JS_FUNC_TO_DATA_PTR(void *, stub)),
        scopeChain(f.cx, scopeChain), name(f.cx, name),
        getprop(f.cx, NULL, name, *thisFromCtor(), f)
    { }

    static void reset(Repatcher &repatcher, ic::PICInfo &pic)
    {
        ScopeNameLabels &labels = pic.scopeNameLabels();

        /* Link the inline path back to the slow path. */
        JSC::CodeLocationJump inlineJump = labels.getInlineJump(pic.fastPathStart);
        repatcher.relink(inlineJump, pic.slowPathStart);

        VoidStubPIC stub;
        switch (pic.kind) {
          case ic::PICInfo::NAME:
            stub = ic::Name;
            break;
          case ic::PICInfo::XNAME:
            stub = ic::XName;
            break;
          default:
            JS_NOT_REACHED("Invalid pic kind in ScopeNameCompiler::reset");
            return;
        }
        FunctionPtr target(JS_FUNC_TO_DATA_PTR(void *, stub));
        repatcher.relink(pic.slowPathCall, target);
    }

    LookupStatus generateGlobalStub(JSObject *obj)
    {
        Assembler masm;
        JumpList fails(cx);
        ScopeNameLabels &labels = pic.scopeNameLabels();

        /* For GETXPROP, the object is already in objReg. */
        if (pic.kind == ic::PICInfo::NAME)
            masm.loadPtr(Address(JSFrameReg, StackFrame::offsetOfScopeChain()), pic.objReg);

        JS_ASSERT(obj == getprop.holder);
        JS_ASSERT(getprop.holder == &scopeChain->global());

        LookupStatus status = walkScopeChain(masm, fails);
        if (status != Lookup_Cacheable)
            return status;

        /* If a scope chain walk was required, the final object needs a NULL test. */
        MaybeJump finalNull;
        if (pic.kind == ic::PICInfo::NAME)
            finalNull = masm.branchTestPtr(Assembler::Zero, pic.objReg, pic.objReg);
        masm.loadShape(pic.objReg, pic.shapeReg);
        Jump finalShape = masm.branchPtr(Assembler::NotEqual, pic.shapeReg,
                                         ImmPtr(getprop.holder->lastProperty()));

        masm.loadObjProp(obj, pic.objReg, getprop.shape, pic.shapeReg, pic.objReg);

        Jump done = masm.jump();

        /* All failures flow to here, so there is a common point to patch. */
        for (Jump *pj = fails.begin(); pj != fails.end(); ++pj)
            pj->linkTo(masm.label(), &masm);
        if (finalNull.isSet())
            finalNull.get().linkTo(masm.label(), &masm);
        finalShape.linkTo(masm.label(), &masm);
        Label failLabel = masm.label();
        Jump failJump = masm.jump();

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        buffer.link(failJump, pic.slowPathStart);
        buffer.link(done, pic.fastPathRejoin);
        CodeLocationLabel cs = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generated %s global stub at %p\n", type, cs.executableAddress());
        spew("NAME stub", "global");

        patchPreviousToHere(cs);

        pic.stubsGenerated++;
        pic.updateLastPath(buffer, failLabel);
        labels.setStubJump(masm, failLabel, failJump);

        if (pic.stubsGenerated == MAX_PIC_STUBS)
            disable("max stubs reached");

        return Lookup_Cacheable;
    }

    enum CallObjPropKind {
        ARG,
        VAR
    };

    LookupStatus generateCallStub(JSObject *obj)
    {
        Assembler masm;
        Vector<Jump, 8> fails(cx);
        ScopeNameLabels &labels = pic.scopeNameLabels();

        /* For GETXPROP, the object is already in objReg. */
        if (pic.kind == ic::PICInfo::NAME)
            masm.loadPtr(Address(JSFrameReg, StackFrame::offsetOfScopeChain()), pic.objReg);

        JS_ASSERT(obj == getprop.holder);
        JS_ASSERT(getprop.holder != &scopeChain->global());

        CallObjPropKind kind;
        const Shape *shape = getprop.shape;
        if (shape->setterOp() == CallObject::setArgOp) {
            kind = ARG;
        } else if (shape->setterOp() == CallObject::setVarOp) {
            kind = VAR;
        } else {
            return disable("unhandled callobj sprop getter");
        }

        LookupStatus status = walkScopeChain(masm, fails);
        if (status != Lookup_Cacheable)
            return status;

        /* If a scope chain walk was required, the final object needs a NULL test. */
        MaybeJump finalNull;
        if (pic.kind == ic::PICInfo::NAME)
            finalNull = masm.branchTestPtr(Assembler::Zero, pic.objReg, pic.objReg);
        masm.loadShape(pic.objReg, pic.shapeReg);
        Jump finalShape = masm.branchPtr(Assembler::NotEqual, pic.shapeReg,
                                         ImmPtr(getprop.holder->lastProperty()));

        JSFunction *fun = getprop.holder->asCall().getCalleeFunction();
        unsigned slot = shape->shortid();
        if (kind == VAR)
            slot += fun->nargs;
        slot += CallObject::RESERVED_SLOTS;
        Address address = masm.objPropAddress(obj, pic.objReg, slot);

        /* Safe because type is loaded first. */
        masm.loadValueAsComponents(address, pic.shapeReg, pic.objReg);

        Jump done = masm.jump();

        // All failures flow to here, so there is a common point to patch.
        for (Jump *pj = fails.begin(); pj != fails.end(); ++pj)
            pj->linkTo(masm.label(), &masm);
        if (finalNull.isSet())
            finalNull.get().linkTo(masm.label(), &masm);
        finalShape.linkTo(masm.label(), &masm);
        Label failLabel = masm.label();
        Jump failJump = masm.jump();

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        buffer.link(failJump, pic.slowPathStart);
        buffer.link(done, pic.fastPathRejoin);
        CodeLocationLabel cs = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generated %s call stub at %p\n", type, cs.executableAddress());

        patchPreviousToHere(cs);

        pic.stubsGenerated++;
        pic.updateLastPath(buffer, failLabel);
        labels.setStubJump(masm, failLabel, failJump);

        if (pic.stubsGenerated == MAX_PIC_STUBS)
            disable("max stubs reached");

        return Lookup_Cacheable;
    }

    LookupStatus updateForName()
    {
        // |getprop.obj| is filled by bind()
        LookupStatus status = getprop.bind();
        if (status != Lookup_Cacheable)
            return status;

        return update(getprop.obj);
    }

    LookupStatus updateForXName()
    {
        // |obj| and |getprop.obj| are NULL, but should be the given scopeChain.
        getprop.obj = scopeChain;
        LookupStatus status = getprop.lookup();
        if (status != Lookup_Cacheable)
            return status;

        return update(getprop.obj);
    }

    LookupStatus update(JSObject *obj)
    {
        if (obj != getprop.holder)
            return disable("property is on proto of a scope object");

        if (obj->isCall())
            return generateCallStub(obj);

        LookupStatus status = getprop.testForGet();
        if (status != Lookup_Cacheable)
            return status;

        if (obj->isGlobal())
            return generateGlobalStub(obj);

        return disable("scope object not handled yet");
    }

    bool retrieve(Value *vp, PICInfo::Kind kind)
    {
        JSObject *obj = getprop.obj;
        JSObject *holder = getprop.holder;
        const JSProperty *prop = getprop.prop;

        if (!prop) {
            /* Kludge to allow (typeof foo == "undefined") tests. */
            if (kind == ic::PICInfo::NAME) {
                JSOp op2 = JSOp(f.pc()[JSOP_NAME_LENGTH]);
                if (op2 == JSOP_TYPEOF) {
                    vp->setUndefined();
                    return true;
                }
            }
            ReportAtomNotDefined(cx, name);
            return false;
        }

        // If the property was found, but we decided not to cache it, then
        // take a slow path and do a full property fetch.
        if (!getprop.shape) {
            if (!obj->getProperty(cx, name, vp))
                return false;
            return true;
        }

        const Shape *shape = getprop.shape;
        JSObject *normalized = obj;
        if (obj->isWith() && !shape->hasDefaultGetter())
            normalized = &obj->asWith().object();
        NATIVE_GET(cx, normalized, holder, shape, 0, vp, return false);
        return true;
    }
};

class BindNameCompiler : public PICStubCompiler
{
    RootedObject scopeChain;
    RootedPropertyName name;

  public:
    BindNameCompiler(VMFrame &f, JSScript *script, JSObject *scopeChain, ic::PICInfo &pic,
                     PropertyName *name, VoidStubPIC stub)
      : PICStubCompiler("bind", f, script, pic, JS_FUNC_TO_DATA_PTR(void *, stub)),
        scopeChain(f.cx, scopeChain), name(f.cx, name)
    { }

    static void reset(Repatcher &repatcher, ic::PICInfo &pic)
    {
        BindNameLabels &labels = pic.bindNameLabels();

        /* Link the inline jump back to the slow path. */
        JSC::CodeLocationJump inlineJump = labels.getInlineJump(pic.getFastShapeGuard());
        repatcher.relink(inlineJump, pic.slowPathStart);

        /* Link the slow path to call the IC entry point. */
        FunctionPtr target(JS_FUNC_TO_DATA_PTR(void *, ic::BindName));
        repatcher.relink(pic.slowPathCall, target);
    }

    void patchPreviousToHere(CodeLocationLabel cs)
    {
        BindNameLabels &labels = pic.bindNameLabels();
        Repatcher repatcher(pic.lastCodeBlock(f.chunk()));
        JSC::CodeLocationJump jump;

        /* Patch either the inline fast path or a generated stub. */
        if (pic.stubsGenerated)
            jump = labels.getStubJump(pic.lastPathStart());
        else
            jump = labels.getInlineJump(pic.getFastShapeGuard());
        repatcher.relink(jump, cs);
    }

    LookupStatus generateStub(JSObject *obj)
    {
        Assembler masm;
        Vector<Jump, 8> fails(cx);

        BindNameLabels &labels = pic.bindNameLabels();

        if (!IsCacheableNonGlobalScope(scopeChain))
            return disable("non-cacheable obj at start of scope chain");

        /* Guard on the shape of the scope chain. */
        masm.loadPtr(Address(JSFrameReg, StackFrame::offsetOfScopeChain()), pic.objReg);
        masm.loadShape(pic.objReg, pic.shapeReg);
        Jump firstShape = masm.branchPtr(Assembler::NotEqual, pic.shapeReg,
                                         ImmPtr(scopeChain->lastProperty()));

        if (scopeChain != obj) {
            /* Walk up the scope chain. */
            JSObject *tobj = &scopeChain->asScope().enclosingScope();
            Address parent(pic.objReg, ScopeObject::offsetOfEnclosingScope());
            while (tobj) {
                if (!IsCacheableNonGlobalScope(tobj))
                    return disable("non-cacheable obj in scope chain");
                masm.loadPayload(parent, pic.objReg);
                masm.loadShape(pic.objReg, pic.shapeReg);
                Jump shapeTest = masm.branchPtr(Assembler::NotEqual, pic.shapeReg,
                                                ImmPtr(tobj->lastProperty()));
                if (!fails.append(shapeTest))
                    return error();
                if (tobj == obj)
                    break;
                tobj = &tobj->asScope().enclosingScope();
            }
            if (tobj != obj)
                return disable("indirect hit");
        }

        Jump done = masm.jump();

        // All failures flow to here, so there is a common point to patch.
        for (Jump *pj = fails.begin(); pj != fails.end(); ++pj)
            pj->linkTo(masm.label(), &masm);
        firstShape.linkTo(masm.label(), &masm);
        Label failLabel = masm.label();
        Jump failJump = masm.jump();

        pic.updatePCCounters(f, masm);

        PICLinker buffer(masm, pic);
        if (!buffer.init(cx))
            return error();

        if (!buffer.verifyRange(pic.lastCodeBlock(f.chunk())) ||
            !buffer.verifyRange(f.chunk())) {
            return disable("code memory is out of range");
        }

        buffer.link(failJump, pic.slowPathStart);
        buffer.link(done, pic.fastPathRejoin);
        CodeLocationLabel cs = buffer.finalize(f);
        JaegerSpew(JSpew_PICs, "generated %s stub at %p\n", type, cs.executableAddress());

        patchPreviousToHere(cs);

        pic.stubsGenerated++;
        pic.updateLastPath(buffer, failLabel);
        labels.setStubJump(masm, failLabel, failJump);

        if (pic.stubsGenerated == MAX_PIC_STUBS)
            disable("max stubs reached");

        return Lookup_Cacheable;
    }

    JSObject *update()
    {
        RecompilationMonitor monitor(cx);

        JSObject *obj = FindIdentifierBase(cx, scopeChain, name);
        if (!obj || monitor.recompiled())
            return obj;

        if (!pic.hit) {
            spew("first hit", "nop");
            pic.hit = true;
            return obj;
        }

        LookupStatus status = generateStub(obj);
        if (status == Lookup_Error)
            return NULL;

        return obj;
    }
};

static void JS_FASTCALL
DisabledGetPropIC(VMFrame &f, ic::PICInfo *pic)
{
    stubs::GetProp(f, pic->name);
}

static void JS_FASTCALL
DisabledGetPropNoCacheIC(VMFrame &f, ic::PICInfo *pic)
{
    stubs::GetPropNoCache(f, pic->name);
}

void JS_FASTCALL
ic::GetProp(VMFrame &f, ic::PICInfo *pic)
{
    bool cached = pic->cached;
    VoidStubPIC stub = cached ? DisabledGetPropIC : DisabledGetPropNoCacheIC;

    JSScript *script = f.fp()->script();

    PropertyName *name = pic->name;
    if (name == f.cx->runtime->atomState.lengthAtom) {
        if (f.regs.sp[-1].isMagic(JS_OPTIMIZED_ARGUMENTS)) {
            f.regs.sp[-1].setInt32(f.regs.fp()->numActualArgs());
            return;
        }
        if (!f.regs.sp[-1].isPrimitive()) {
            JSObject *obj = &f.regs.sp[-1].toObject();
            if (obj->isArray() || obj->isString()) {
                GetPropCompiler cc(f, script, obj, *pic, NULL, stub);
                if (obj->isArray()) {
                    LookupStatus status = cc.generateArrayLengthStub();
                    if (status == Lookup_Error)
                        THROW();
                    f.regs.sp[-1].setNumber(obj->getArrayLength());
                } else {
                    LookupStatus status = cc.generateStringObjLengthStub();
                    if (status == Lookup_Error)
                        THROW();
                    JSString *str = obj->asString().unbox();
                    f.regs.sp[-1].setInt32(str->length());
                }
                return;
            }
        }
    }

    if (f.regs.sp[-1].isString()) {
        GetPropCompiler cc(f, script, NULL, *pic, name, stub);
        if (name == f.cx->runtime->atomState.lengthAtom) {
            LookupStatus status = cc.generateStringLengthStub();
            if (status == Lookup_Error)
                THROW();
            JSString *str = f.regs.sp[-1].toString();
            f.regs.sp[-1].setInt32(str->length());
        } else {
            LookupStatus status = cc.generateStringPropertyStub();
            if (status == Lookup_Error)
                THROW();
            JSObject *obj = ValueToObject(f.cx, f.regs.sp[-1]);
            if (!obj)
                THROW();
            if (!obj->getProperty(f.cx, name, &f.regs.sp[-1]))
                THROW();
        }
        return;
    }

    RecompilationMonitor monitor(f.cx);

    JSObject *obj = ValueToObject(f.cx, f.regs.sp[-1]);
    if (!obj)
        THROW();

    if (!monitor.recompiled() && pic->shouldUpdate(f.cx)) {
        GetPropCompiler cc(f, script, obj, *pic, name, stub);
        if (!cc.update())
            THROW();
    }

    Value v;
    if (cached) {
        if (!GetPropertyOperation(f.cx, f.pc(), f.regs.sp[-1], &v))
            THROW();
    } else {
        if (!obj->getProperty(f.cx, name, &v))
            THROW();
    }

    f.regs.sp[-1] = v;
}

template <JSBool strict>
static void JS_FASTCALL
DisabledSetPropIC(VMFrame &f, ic::PICInfo *pic)
{
    stubs::SetName<strict>(f, pic->name);
}

void JS_FASTCALL
ic::SetProp(VMFrame &f, ic::PICInfo *pic)
{
    JSScript *script = f.fp()->script();
    JS_ASSERT(pic->isSet());

    VoidStubPIC stub = STRICT_VARIANT(DisabledSetPropIC);

    // Save this in case the compiler triggers a recompilation of this script.
    PropertyName *name = pic->name;
    VoidStubName nstub = STRICT_VARIANT(stubs::SetName);

    RecompilationMonitor monitor(f.cx);

    JSObject *obj = ValueToObject(f.cx, f.regs.sp[-2]);
    if (!obj)
        THROW();

    // Note, we can't use SetName for PROPINC PICs because the property
    // cache can't handle a GET and SET from the same scripted PC.
    if (!monitor.recompiled() && pic->shouldUpdate(f.cx)) {
        SetPropCompiler cc(f, script, obj, *pic, name, stub);
        LookupStatus status = cc.update();
        if (status == Lookup_Error)
            THROW();
    }

    nstub(f, name);
}

static void JS_FASTCALL
DisabledNameIC(VMFrame &f, ic::PICInfo *pic)
{
    stubs::Name(f);
}

static void JS_FASTCALL
DisabledXNameIC(VMFrame &f, ic::PICInfo *pic)
{
    stubs::GetProp(f, pic->name);
}

void JS_FASTCALL
ic::XName(VMFrame &f, ic::PICInfo *pic)
{
    JSScript *script = f.fp()->script();

    /* GETXPROP is guaranteed to have an object. */
    JSObject *obj = &f.regs.sp[-1].toObject();

    ScopeNameCompiler cc(f, script, obj, *pic, pic->name, DisabledXNameIC);

    LookupStatus status = cc.updateForXName();
    if (status == Lookup_Error)
        THROW();

    Value rval;
    if (!cc.retrieve(&rval, PICInfo::XNAME))
        THROW();
    f.regs.sp[-1] = rval;
}

void JS_FASTCALL
ic::Name(VMFrame &f, ic::PICInfo *pic)
{
    JSScript *script = f.fp()->script();

    ScopeNameCompiler cc(f, script, f.fp()->scopeChain(), *pic, pic->name, DisabledNameIC);

    LookupStatus status = cc.updateForName();
    if (status == Lookup_Error)
        THROW();

    Value rval;
    if (!cc.retrieve(&rval, PICInfo::NAME))
        THROW();
    f.regs.sp[0] = rval;
}

static void JS_FASTCALL
DisabledBindNameIC(VMFrame &f, ic::PICInfo *pic)
{
    stubs::BindName(f, pic->name);
}

void JS_FASTCALL
ic::BindName(VMFrame &f, ic::PICInfo *pic)
{
    JSScript *script = f.fp()->script();

    VoidStubPIC stub = DisabledBindNameIC;
    BindNameCompiler cc(f, script, f.fp()->scopeChain(), *pic, pic->name, stub);

    JSObject *obj = cc.update();
    if (!obj)
        THROW();

    f.regs.sp[0].setObject(*obj);
}

void
BaseIC::spew(JSContext *cx, const char *event, const char *message)
{
#ifdef JS_METHODJIT_SPEW
    JaegerSpew(JSpew_PICs, "%s %s: %s (%s: %d)\n",
               js_CodeName[op], event, message, cx->fp()->script()->filename, CurrentLine(cx));
#endif
}

/* Total length of scripts preceding a frame. */
inline uint32_t frameCountersOffset(VMFrame &f)
{
    JSContext *cx = f.cx;

    uint32_t offset = 0;
    if (cx->regs().inlined()) {
        offset += cx->fp()->script()->length;
        uint32_t index = cx->regs().inlined()->inlineIndex;
        InlineFrame *frames = f.chunk()->inlineFrames();
        for (unsigned i = 0; i < index; i++)
            offset += frames[i].fun->script()->length;
    }

    jsbytecode *pc;
    JSScript *script = cx->stack.currentScript(&pc);
    offset += pc - script->code;

    return offset;
}

LookupStatus
BaseIC::disable(VMFrame &f, const char *reason, void *stub)
{
    if (f.chunk()->pcLengths) {
        uint32_t offset = frameCountersOffset(f);
        f.chunk()->pcLengths[offset].picsLength = 0;
    }

    spew(f.cx, "disabled", reason);
    Repatcher repatcher(f.chunk());
    repatcher.relink(slowPathCall, FunctionPtr(stub));
    return Lookup_Uncacheable;
}

void
BaseIC::updatePCCounters(VMFrame &f, Assembler &masm)
{
    if (f.chunk()->pcLengths) {
        uint32_t offset = frameCountersOffset(f);
        f.chunk()->pcLengths[offset].picsLength += masm.size();
    }
}

bool
BaseIC::shouldUpdate(JSContext *cx)
{
    if (!hit) {
        hit = true;
        spew(cx, "ignored", "first hit");
        return false;
    }
    JS_ASSERT(stubsGenerated < MAX_PIC_STUBS);
    return true;
}

void
PICInfo::purge(Repatcher &repatcher)
{
    switch (kind) {
      case SET:
        SetPropCompiler::reset(repatcher, *this);
        break;
      case NAME:
      case XNAME:
        ScopeNameCompiler::reset(repatcher, *this);
        break;
      case BIND:
        BindNameCompiler::reset(repatcher, *this);
        break;
      case GET:
        GetPropCompiler::reset(repatcher, *this);
        break;
      default:
        JS_NOT_REACHED("Unhandled PIC kind");
        break;
    }
    reset();
}

static void JS_FASTCALL
DisabledGetElem(VMFrame &f, ic::GetElementIC *ic)
{
    stubs::GetElem(f);
}

bool
GetElementIC::shouldUpdate(JSContext *cx)
{
    if (!hit) {
        hit = true;
        spew(cx, "ignored", "first hit");
        return false;
    }
    JS_ASSERT(stubsGenerated < MAX_GETELEM_IC_STUBS);
    return true;
}

LookupStatus
GetElementIC::disable(VMFrame &f, const char *reason)
{
    slowCallPatched = true;
    void *stub = JS_FUNC_TO_DATA_PTR(void *, DisabledGetElem);
    BaseIC::disable(f, reason, stub);
    return Lookup_Uncacheable;
}

LookupStatus
GetElementIC::error(JSContext *cx)
{
    return Lookup_Error;
}

void
GetElementIC::purge(Repatcher &repatcher)
{
    // Repatch the inline jumps.
    if (inlineTypeGuardPatched)
        repatcher.relink(fastPathStart.jumpAtOffset(inlineTypeGuard), slowPathStart);
    if (inlineShapeGuardPatched)
        repatcher.relink(fastPathStart.jumpAtOffset(inlineShapeGuard), slowPathStart);

    if (slowCallPatched) {
        repatcher.relink(slowPathCall,
                         FunctionPtr(JS_FUNC_TO_DATA_PTR(void *, ic::GetElement)));
    }

    reset();
}

LookupStatus
GetElementIC::attachGetProp(VMFrame &f, JSObject *obj, const Value &v, PropertyName *name,
                            Value *vp)
{
    JS_ASSERT(v.isString());
    JSContext *cx = f.cx;

    GetPropHelper<GetElementIC> getprop(cx, obj, name, *this, f);
    LookupStatus status = getprop.lookupAndTest();
    if (status != Lookup_Cacheable)
        return status;

    // With TI enabled, string property stubs can only be added to an opcode if
    // the value read will go through a type barrier afterwards. TI only
    // accounts for integer-valued properties accessed by GETELEM/CALLELEM.
    if (cx->typeInferenceEnabled() && !forcedTypeBarrier)
        return disable(f, "string element access may not have type barrier");

    Assembler masm;

    // Guard on the string's type and identity.
    MaybeJump atomTypeGuard;
    if (hasInlineTypeGuard() && !inlineTypeGuardPatched) {
        // We link all string-key dependent stubs together, and store the
        // first set of guards in the IC, separately, from int-key dependent
        // stubs. As long as we guarantee that the first string-key dependent
        // stub guards on the key type, then all other string-key stubs can
        // omit the guard.
        JS_ASSERT(!idRemat.isTypeKnown());
        atomTypeGuard = masm.testString(Assembler::NotEqual, typeReg);
    } else {
        // If there was no inline type guard, then a string type is guaranteed.
        // Otherwise, we are guaranteed the type has already been checked, via
        // the comment above.
        JS_ASSERT_IF(!hasInlineTypeGuard(), idRemat.knownType() == JSVAL_TYPE_STRING);
    }

    // Reify the shape before guards that could flow into shape guarding stubs.
    if (!obj->isDenseArray() && !typeRegHasBaseShape) {
        masm.loadShape(objReg, typeReg);
        typeRegHasBaseShape = true;
    }

    MaybeJump atomIdGuard;
    if (!idRemat.isConstant())
        atomIdGuard = masm.branchPtr(Assembler::NotEqual, idRemat.dataReg(), ImmPtr(v.toString()));

    // Guard on the base shape.
    Jump shapeGuard = masm.branchPtr(Assembler::NotEqual, typeReg, ImmPtr(obj->lastProperty()));

    Vector<Jump, 8> otherGuards(cx);

    // Guard on the prototype, if applicable.
    MaybeJump protoGuard;
    JSObject *holder = getprop.holder;
    RegisterID holderReg = objReg;
    if (obj != holder) {
        if (!GeneratePrototypeGuards(cx, otherGuards, masm, obj, holder, objReg, typeReg))
            return error(cx);

        // Bake in the holder identity. Careful not to clobber |objReg|, since we can't remat it.
        holderReg = typeReg;
        masm.move(ImmPtr(holder), holderReg);
        typeRegHasBaseShape = false;

        // Guard on the holder's shape.
        protoGuard = masm.guardShape(holderReg, holder);
    }

    if (op == JSOP_CALLELEM) {
        // Emit a write of |obj| to the top of the stack, before we lose it.
        Value *thisVp = &cx->regs().sp[-1];
        Address thisSlot(JSFrameReg, StackFrame::offsetOfFixed(thisVp - cx->fp()->slots()));
        masm.storeValueFromComponents(ImmType(JSVAL_TYPE_OBJECT), objReg, thisSlot);
    }

    // Load the value.
    const Shape *shape = getprop.shape;
    masm.loadObjProp(holder, holderReg, shape, typeReg, objReg);

    Jump done = masm.jump();

    updatePCCounters(f, masm);

    PICLinker buffer(masm, *this);
    if (!buffer.init(cx))
        return error(cx);

    if (hasLastStringStub && !buffer.verifyRange(lastStringStub))
        return disable(f, "code memory is out of range");
    if (!buffer.verifyRange(f.chunk()))
        return disable(f, "code memory is out of range");

    // Patch all guards.
    buffer.maybeLink(atomIdGuard, slowPathStart);
    buffer.maybeLink(atomTypeGuard, slowPathStart);
    buffer.link(shapeGuard, slowPathStart);
    buffer.maybeLink(protoGuard, slowPathStart);
    for (Jump *pj = otherGuards.begin(); pj != otherGuards.end(); ++pj)
        buffer.link(*pj, slowPathStart);
    buffer.link(done, fastPathRejoin);

    CodeLocationLabel cs = buffer.finalize(f);
#if DEBUG
    char *chars = DeflateString(cx, v.toString()->getChars(cx), v.toString()->length());
    JaegerSpew(JSpew_PICs, "generated %s stub at %p for atom %p (\"%s\") shape %p (%s: %d)\n",
               js_CodeName[op], cs.executableAddress(), (void*)name, chars,
               (void*)holder->lastProperty(), cx->fp()->script()->filename, CurrentLine(cx));
    cx->free_(chars);
#endif

    // Update the inline guards, if needed.
    if (shouldPatchInlineTypeGuard() || shouldPatchUnconditionalShapeGuard()) {
        Repatcher repatcher(f.chunk());

        if (shouldPatchInlineTypeGuard()) {
            // A type guard is present in the inline path, and this is the
            // first string stub, so patch it now.
            JS_ASSERT(!inlineTypeGuardPatched);
            JS_ASSERT(atomTypeGuard.isSet());

            repatcher.relink(fastPathStart.jumpAtOffset(inlineTypeGuard), cs);
            inlineTypeGuardPatched = true;
        }

        if (shouldPatchUnconditionalShapeGuard()) {
            // The shape guard is unconditional, meaning there is no type
            // check. This is the first stub, so it has to be patched. Note
            // that it is wrong to patch the inline shape guard otherwise,
            // because it follows an integer-id guard.
            JS_ASSERT(!hasInlineTypeGuard());

            repatcher.relink(fastPathStart.jumpAtOffset(inlineShapeGuard), cs);
            inlineShapeGuardPatched = true;
        }
    }

    // If there were previous stub guards, patch them now.
    if (hasLastStringStub) {
        Repatcher repatcher(lastStringStub);
        CodeLocationLabel stub(lastStringStub.start());
        if (atomGuard)
            repatcher.relink(stub.jumpAtOffset(atomGuard), cs);
        repatcher.relink(stub.jumpAtOffset(firstShapeGuard), cs);
        if (secondShapeGuard)
            repatcher.relink(stub.jumpAtOffset(secondShapeGuard), cs);
    }

    // Update state.
    hasLastStringStub = true;
    lastStringStub = JITCode(cs.executableAddress(), buffer.size());
    if (atomIdGuard.isSet()) {
        atomGuard = buffer.locationOf(atomIdGuard.get()) - cs;
        JS_ASSERT(atomGuard == buffer.locationOf(atomIdGuard.get()) - cs);
        JS_ASSERT(atomGuard);
    } else {
        atomGuard = 0;
    }
    if (protoGuard.isSet()) {
        secondShapeGuard = buffer.locationOf(protoGuard.get()) - cs;
        JS_ASSERT(secondShapeGuard == buffer.locationOf(protoGuard.get()) - cs);
        JS_ASSERT(secondShapeGuard);
    } else {
        secondShapeGuard = 0;
    }
    firstShapeGuard = buffer.locationOf(shapeGuard) - cs;
    JS_ASSERT(firstShapeGuard == buffer.locationOf(shapeGuard) - cs);
    JS_ASSERT(firstShapeGuard);

    stubsGenerated++;

    if (stubsGenerated == MAX_GETELEM_IC_STUBS)
        disable(f, "max stubs reached");

    // Finally, fetch the value to avoid redoing the property lookup.
    *vp = holder->getSlot(shape->slot());

    return Lookup_Cacheable;
}

#if defined JS_METHODJIT_TYPED_ARRAY
LookupStatus
GetElementIC::attachTypedArray(VMFrame &f, JSObject *obj, const Value &v, jsid id, Value *vp)
{
    JSContext *cx = f.cx;

    if (!v.isInt32())
        return disable(f, "typed array with string key");

    if (op == JSOP_CALLELEM)
        return disable(f, "typed array with call");

    // The fast-path guarantees that after the dense shape guard, the type is
    // known to be int32, either via type inference or the inline type check.
    JS_ASSERT(hasInlineTypeGuard() || idRemat.knownType() == JSVAL_TYPE_INT32);

    Assembler masm;

    // Guard on this typed array's shape/class.
    Jump shapeGuard = masm.guardShape(objReg, obj);

    // Bounds check.
    Jump outOfBounds;
    Address typedArrayLength = masm.payloadOf(Address(objReg, TypedArray::lengthOffset()));
    if (idRemat.isConstant()) {
        JS_ASSERT(idRemat.value().toInt32() == v.toInt32());
        outOfBounds = masm.branch32(Assembler::BelowOrEqual, typedArrayLength, Imm32(v.toInt32()));
    } else {
        outOfBounds = masm.branch32(Assembler::BelowOrEqual, typedArrayLength, idRemat.dataReg());
    }

    // Load the array's packed data vector.
    masm.loadPtr(Address(objReg, TypedArray::dataOffset()), objReg);

    Int32Key key = idRemat.isConstant()
                 ? Int32Key::FromConstant(v.toInt32())
                 : Int32Key::FromRegister(idRemat.dataReg());

    if (!masm.supportsFloatingPoint() &&
        (TypedArray::getType(obj) == TypedArray::TYPE_FLOAT32 ||
         TypedArray::getType(obj) == TypedArray::TYPE_FLOAT64 ||
         TypedArray::getType(obj) == TypedArray::TYPE_UINT32))
    {
        return disable(f, "fpu not supported");
    }

    MaybeRegisterID tempReg;
    masm.loadFromTypedArray(TypedArray::getType(obj), objReg, key, typeReg, objReg, tempReg);

    Jump done = masm.jump();

    updatePCCounters(f, masm);

    PICLinker buffer(masm, *this);
    if (!buffer.init(cx))
        return error(cx);

    if (!buffer.verifyRange(f.chunk()))
        return disable(f, "code memory is out of range");

    buffer.link(shapeGuard, slowPathStart);
    buffer.link(outOfBounds, slowPathStart);
    buffer.link(done, fastPathRejoin);

    CodeLocationLabel cs = buffer.finalizeCodeAddendum();
    JaegerSpew(JSpew_PICs, "generated getelem typed array stub at %p\n", cs.executableAddress());

    // If we can generate a typed array stub, the shape guard is conditional.
    // Also, we only support one typed array.
    JS_ASSERT(!shouldPatchUnconditionalShapeGuard());
    JS_ASSERT(!inlineShapeGuardPatched);

    Repatcher repatcher(f.chunk());
    repatcher.relink(fastPathStart.jumpAtOffset(inlineShapeGuard), cs);
    inlineShapeGuardPatched = true;

    stubsGenerated++;

    // In the future, it might make sense to attach multiple typed array stubs.
    // For simplicitly, they are currently monomorphic.
    if (stubsGenerated == MAX_GETELEM_IC_STUBS)
        disable(f, "max stubs reached");

    disable(f, "generated typed array stub");

    // Fetch the value as expected of Lookup_Cacheable for GetElement.
    if (!obj->getGeneric(cx, RootedId(cx, id), vp))
        return Lookup_Error;

    return Lookup_Cacheable;
}
#endif /* JS_METHODJIT_TYPED_ARRAY */

LookupStatus
GetElementIC::update(VMFrame &f, JSObject *obj, const Value &v, jsid id, Value *vp)
{
    /*
     * Only treat this as a GETPROP for non-numeric string identifiers. The
     * GETPROP IC assumes the id has already gone through filtering for string
     * indexes in the emitter.
     */
    uint32_t dummy;
    if (v.isString() && JSID_IS_ATOM(id) && !JSID_TO_ATOM(id)->isIndex(&dummy))
        return attachGetProp(f, obj, v, JSID_TO_ATOM(id)->asPropertyName(), vp);

#if defined JS_METHODJIT_TYPED_ARRAY
    /*
     * Typed array ICs can make stub calls, and need to know which registers
     * are in use and need to be restored after the call. If type inference is
     * enabled then we don't necessarily know the full set of such registers
     * when generating the IC (loop-carried registers may be allocated later),
     * and additionally the push/pop instructions used to save/restore in the
     * IC are not compatible with carrying entries in floating point registers.
     * Since we can use type information to generate inline paths for typed
     * arrays, just don't generate these ICs with inference enabled.
     */
    if (!f.cx->typeInferenceEnabled() && obj->isTypedArray())
        return attachTypedArray(f, obj, v, id, vp);
#endif

    return disable(f, "unhandled object and key type");
}

void JS_FASTCALL
ic::GetElement(VMFrame &f, ic::GetElementIC *ic)
{
    JSContext *cx = f.cx;

    // Right now, we don't optimize for strings or lazy arguments.
    if (!f.regs.sp[-2].isObject()) {
        ic->disable(f, "non-object");
        stubs::GetElem(f);
        return;
    }

    Value idval = f.regs.sp[-1];

    RecompilationMonitor monitor(cx);

    RootedObject obj(cx, ValueToObject(cx, f.regs.sp[-2]));
    if (!obj)
        THROW();

#if JS_HAS_XML_SUPPORT
    // Some XML properties behave differently when accessed in a call vs. normal
    // context, so we fall back to stubs::GetElem.
    if (obj->isXML()) {
        ic->disable(f, "XML object");
        stubs::GetElem(f);
        return;
    }
#endif

    jsid id;
    if (idval.isInt32() && INT_FITS_IN_JSID(idval.toInt32())) {
        id = INT_TO_JSID(idval.toInt32());
    } else {
        if (!InternNonIntElementId(cx, obj, idval, &id))
            THROW();
    }

    if (!monitor.recompiled() && ic->shouldUpdate(cx)) {
#ifdef DEBUG
        f.regs.sp[-2] = MagicValue(JS_GENERIC_MAGIC);
#endif
        LookupStatus status = ic->update(f, obj, idval, id, &f.regs.sp[-2]);
        if (status != Lookup_Uncacheable) {
            if (status == Lookup_Error)
                THROW();

            // If the result can be cached, the value was already retrieved.
            JS_ASSERT(!f.regs.sp[-2].isMagic());
            return;
        }
    }

    if (!obj->getGeneric(cx, RootedId(cx, id), &f.regs.sp[-2]))
        THROW();

#if JS_HAS_NO_SUCH_METHOD
    if (*f.pc() == JSOP_CALLELEM && JS_UNLIKELY(f.regs.sp[-2].isPrimitive())) {
        if (!OnUnknownMethod(cx, obj, idval, &f.regs.sp[-2]))
            THROW();
    }
#endif
}

#define APPLY_STRICTNESS(f, s)                          \
    (FunctionTemplateConditional(s, f<true>, f<false>))

LookupStatus
SetElementIC::disable(VMFrame &f, const char *reason)
{
    slowCallPatched = true;
    VoidStub stub = APPLY_STRICTNESS(stubs::SetElem, strictMode);
    BaseIC::disable(f, reason, JS_FUNC_TO_DATA_PTR(void *, stub));
    return Lookup_Uncacheable;
}

LookupStatus
SetElementIC::error(JSContext *cx)
{
    return Lookup_Error;
}

void
SetElementIC::purge(Repatcher &repatcher)
{
    // Repatch the inline jumps.
    if (inlineShapeGuardPatched)
        repatcher.relink(fastPathStart.jumpAtOffset(inlineShapeGuard), slowPathStart);
    if (inlineHoleGuardPatched)
        repatcher.relink(fastPathStart.jumpAtOffset(inlineHoleGuard), slowPathStart);

    if (slowCallPatched) {
        void *stub = JS_FUNC_TO_DATA_PTR(void *, APPLY_STRICTNESS(ic::SetElement, strictMode));
        repatcher.relink(slowPathCall, FunctionPtr(stub));
    }

    reset();
}

LookupStatus
SetElementIC::attachHoleStub(VMFrame &f, JSObject *obj, int32_t keyval)
{
    JSContext *cx = f.cx;

    if (keyval < 0)
        return disable(f, "negative key index");

    // We may have failed a capacity check instead of a dense array check.
    // However we should still build the IC in this case, since it could
    // be in a loop that is filling in the array.

    if (js_PrototypeHasIndexedProperties(cx, obj))
        return disable(f, "prototype has indexed properties");

    Assembler masm;

    Vector<Jump, 8> fails(cx);

    if (!GeneratePrototypeGuards(cx, fails, masm, obj, NULL, objReg, objReg))
        return error(cx);

    // Test for indexed properties in Array.prototype. We test each shape
    // along the proto chain. This affords us two optimizations:
    //  1) Loading the prototype can be avoided because the shape would change;
    //     instead we can bake in their identities.
    //  2) We only have to test the shape, rather than INDEXED.
    for (JSObject *pobj = obj->getProto(); pobj; pobj = pobj->getProto()) {
        if (!pobj->isNative())
            return disable(f, "non-native array prototype");
        masm.move(ImmPtr(pobj), objReg);
        Jump j = masm.guardShape(objReg, pobj);
        if (!fails.append(j))
            return error(cx);
    }

    // Restore |obj|.
    masm.rematPayload(StateRemat::FromInt32(objRemat), objReg);

    // Load the elements.
    masm.loadPtr(Address(objReg, JSObject::offsetOfElements()), objReg);

    Int32Key key = hasConstantKey ? Int32Key::FromConstant(keyValue) : Int32Key::FromRegister(keyReg);

    // Guard that the initialized length is being updated exactly.
    fails.append(masm.guardArrayExtent(ObjectElements::offsetOfInitializedLength(),
                                       objReg, key, Assembler::NotEqual));

    // Check the array capacity.
    fails.append(masm.guardArrayExtent(ObjectElements::offsetOfCapacity(),
                                       objReg, key, Assembler::BelowOrEqual));

    masm.bumpKey(key, 1);

    // Update the length and initialized length.
    masm.storeKey(key, Address(objReg, ObjectElements::offsetOfInitializedLength()));
    Jump lengthGuard = masm.guardArrayExtent(ObjectElements::offsetOfLength(),
                                             objReg, key, Assembler::AboveOrEqual);
    masm.storeKey(key, Address(objReg, ObjectElements::offsetOfLength()));
    lengthGuard.linkTo(masm.label(), &masm);

    masm.bumpKey(key, -1);

    // Store the value back.
    if (hasConstantKey) {
        Address slot(objReg, keyValue * sizeof(Value));
        masm.storeValue(vr, slot);
    } else {
        BaseIndex slot(objReg, keyReg, Assembler::JSVAL_SCALE);
        masm.storeValue(vr, slot);
    }

    Jump done = masm.jump();

    JS_ASSERT(!execPool);
    JS_ASSERT(!inlineHoleGuardPatched);

    LinkerHelper buffer(masm, JSC::METHOD_CODE);
    execPool = buffer.init(cx);
    if (!execPool)
        return error(cx);

    if (!buffer.verifyRange(f.chunk()))
        return disable(f, "code memory is out of range");

    // Patch all guards.
    for (size_t i = 0; i < fails.length(); i++)
        buffer.link(fails[i], slowPathStart);
    buffer.link(done, fastPathRejoin);

    CodeLocationLabel cs = buffer.finalize(f);
    JaegerSpew(JSpew_PICs, "generated dense array hole stub at %p\n", cs.executableAddress());

    Repatcher repatcher(f.chunk());
    repatcher.relink(fastPathStart.jumpAtOffset(inlineHoleGuard), cs);
    inlineHoleGuardPatched = true;

    disable(f, "generated dense array hole stub");

    return Lookup_Cacheable;
}

#if defined JS_METHODJIT_TYPED_ARRAY
LookupStatus
SetElementIC::attachTypedArray(VMFrame &f, JSObject *obj, int32_t key)
{
    // Right now, only one shape guard extension is supported.
    JS_ASSERT(!inlineShapeGuardPatched);

    Assembler masm;
    JSContext *cx = f.cx;

    // Restore |obj|.
    masm.rematPayload(StateRemat::FromInt32(objRemat), objReg);

    // Guard on this typed array's shape.
    Jump shapeGuard = masm.guardShape(objReg, obj);

    // Bounds check.
    Jump outOfBounds;
    Address typedArrayLength = masm.payloadOf(Address(objReg, TypedArray::lengthOffset()));
    if (hasConstantKey)
        outOfBounds = masm.branch32(Assembler::BelowOrEqual, typedArrayLength, Imm32(keyValue));
    else
        outOfBounds = masm.branch32(Assembler::BelowOrEqual, typedArrayLength, keyReg);

    // Load the array's packed data vector.
    masm.loadPtr(Address(objReg, TypedArray::dataOffset()), objReg);

    if (!masm.supportsFloatingPoint() &&
        (TypedArray::getType(obj) == TypedArray::TYPE_FLOAT32 ||
         TypedArray::getType(obj) == TypedArray::TYPE_FLOAT64))
    {
        return disable(f, "fpu not supported");
    }

    int shift = TypedArray::slotWidth(obj);
    if (hasConstantKey) {
        Address addr(objReg, keyValue * shift);
        if (!StoreToTypedArray(cx, masm, obj, addr, vr, volatileMask))
            return error(cx);
    } else {
        Assembler::Scale scale = Assembler::TimesOne;
        switch (shift) {
          case 2:
            scale = Assembler::TimesTwo;
            break;
          case 4:
            scale = Assembler::TimesFour;
            break;
          case 8:
            scale = Assembler::TimesEight;
            break;
        }
        BaseIndex addr(objReg, keyReg, scale);
        if (!StoreToTypedArray(cx, masm, obj, addr, vr, volatileMask))
            return error(cx);
    }

    Jump done = masm.jump();

    // The stub does not rely on any pointers or numbers that could be ruined
    // by a GC or shape regenerated GC. We let this stub live for the lifetime
    // of the script.
    JS_ASSERT(!execPool);
    LinkerHelper buffer(masm, JSC::METHOD_CODE);
    execPool = buffer.init(cx);
    if (!execPool)
        return error(cx);

    if (!buffer.verifyRange(f.chunk()))
        return disable(f, "code memory is out of range");

    // Note that the out-of-bounds path simply does nothing.
    buffer.link(shapeGuard, slowPathStart);
    buffer.link(outOfBounds, fastPathRejoin);
    buffer.link(done, fastPathRejoin);
    masm.finalize(buffer);

    CodeLocationLabel cs = buffer.finalizeCodeAddendum();
    JaegerSpew(JSpew_PICs, "generated setelem typed array stub at %p\n", cs.executableAddress());

    Repatcher repatcher(f.chunk());
    repatcher.relink(fastPathStart.jumpAtOffset(inlineShapeGuard), cs);
    inlineShapeGuardPatched = true;

    stubsGenerated++;

    // In the future, it might make sense to attach multiple typed array stubs.
    // For simplicitly, they are currently monomorphic.
    if (stubsGenerated == MAX_GETELEM_IC_STUBS)
        disable(f, "max stubs reached");

    disable(f, "generated typed array stub");

    return Lookup_Cacheable;
}
#endif /* JS_METHODJIT_TYPED_ARRAY */

LookupStatus
SetElementIC::update(VMFrame &f, const Value &objval, const Value &idval)
{
    if (!objval.isObject())
        return disable(f, "primitive lval");
    if (!idval.isInt32())
        return disable(f, "non-int32 key");

    JSObject *obj = &objval.toObject();
    int32_t key = idval.toInt32();

    if (obj->isDenseArray())
        return attachHoleStub(f, obj, key);

#if defined JS_METHODJIT_TYPED_ARRAY
    /* Not attaching typed array stubs with linear scan allocator, see GetElementIC. */
    if (!f.cx->typeInferenceEnabled() && obj->isTypedArray())
        return attachTypedArray(f, obj, key);
#endif

    return disable(f, "unsupported object type");
}

bool
SetElementIC::shouldUpdate(JSContext *cx)
{
    if (!hit) {
        hit = true;
        spew(cx, "ignored", "first hit");
        return false;
    }
#ifdef JSGC_INCREMENTAL_MJ
    JS_ASSERT(!cx->compartment->needsBarrier());
#endif
    JS_ASSERT(stubsGenerated < MAX_PIC_STUBS);
    return true;
}

template<JSBool strict>
void JS_FASTCALL
ic::SetElement(VMFrame &f, ic::SetElementIC *ic)
{
    JSContext *cx = f.cx;

    if (ic->shouldUpdate(cx)) {
        LookupStatus status = ic->update(f, f.regs.sp[-3], f.regs.sp[-2]);
        if (status == Lookup_Error)
            THROW();
    }

    stubs::SetElem<strict>(f);
}

template void JS_FASTCALL ic::SetElement<true>(VMFrame &f, SetElementIC *ic);
template void JS_FASTCALL ic::SetElement<false>(VMFrame &f, SetElementIC *ic);

#endif /* JS_POLYIC */

