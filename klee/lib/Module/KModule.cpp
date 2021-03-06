//===-- KModule.cpp -------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// FIXME: This does not belong here.
#include "../Core/Common.h"

#include "klee/Internal/Module/KModule.h"

#include "Passes.h"

#include "klee/Config/Version.h"
#include "klee/Interpreter.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Support/ModuleUtil.h"

#include "llvm/Bitcode/ReaderWriter.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/DataLayout.h"
#else
#include "llvm/Instructions.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/ValueSymbolTable.h"
#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
#include "llvm/Target/TargetData.h"
#else
#include "llvm/DataLayout.h"
#endif

#endif

#include "llvm/PassManager.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/Path.h"
#include "llvm/Transforms/Scalar.h"

#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Support/InstIterator.h>
#include <llvm/Support/Debug.h>

#include <sstream>

#define XQX_DEBUG_PATCH_CRCERROR 


using namespace llvm;
using namespace klee;

extern llvm::Pass *createCallPathsPass();

static uint64_t ciIndex = 0;
static uint64_t bbIndex = 0;

namespace {
  enum SwitchImplType {
    eSwitchTypeSimple,
    eSwitchTypeLLVM,
    eSwitchTypeInternal
  };

  cl::list<std::string>
  MergeAtExit("merge-at-exit");
    
  cl::opt<bool>
  NoTruncateSourceLines("no-truncate-source-lines",
                        cl::desc("Don't truncate long lines in the output source"));

  cl::opt<bool>
  OutputSource("output-source",
               cl::desc("Write the assembly for the final transformed source"),
               cl::init(true));

  cl::opt<bool>
  OutputModule("output-module",
               cl::desc("Write the bitcode for the final transformed module"),
               cl::init(false));

  cl::opt<SwitchImplType>
  SwitchType("switch-type", cl::desc("Select the implementation of switch"),
             cl::values(clEnumValN(eSwitchTypeSimple, "simple", 
                                   "lower to ordered branches"),
                        clEnumValN(eSwitchTypeLLVM, "llvm", 
                                   "lower using LLVM"),
                        clEnumValN(eSwitchTypeInternal, "internal", 
                                   "execute switch internally"),
                        clEnumValEnd),
             cl::init(eSwitchTypeInternal));
  
  cl::opt<bool>
  DebugPrintEscapingFunctions("debug-print-escaping-functions", 
                              cl::desc("Print functions whose address is taken."));

  cl::opt<bool>
  OutputFuncID("output-func-id",
               cl::desc("Write the func id information to id-func.txt"),
               cl::init(false));

  cl::opt<bool>
  PatchCrc("patch-crc",
               cl::desc("patch crc check in libpng"),
               cl::init(true));


}

KModule::KModule(Module *_module) 
  : module(_module),
#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
    targetData(new TargetData(module)),
#else
    targetData(new DataLayout(module)),
#endif
    kleeMergeFn(0),
    infos(0),
    bbNum(0),
    constantTable(0) {
}

KModule::~KModule() {
  delete[] constantTable;
  delete infos;

  for (std::vector<KFunction*>::iterator it = functions.begin(), 
         ie = functions.end(); it != ie; ++it)
    delete *it;

  for (std::map<llvm::Constant*, KConstant*>::iterator it=constantMap.begin(),
      itE=constantMap.end(); it!=itE;++it)
    delete it->second;

  delete targetData;
  delete module;
}

/***/

namespace llvm {
extern void Optimize(Module*);
}

// what a hack
static Function *getStubFunctionForCtorList(Module *m,
                                            GlobalVariable *gv, 
                                            std::string name) {
  assert(!gv->isDeclaration() && !gv->hasInternalLinkage() &&
         "do not support old LLVM style constructor/destructor lists");
  
  std::vector<LLVM_TYPE_Q Type*> nullary;

  Function *fn = Function::Create(FunctionType::get(Type::getVoidTy(getGlobalContext()), 
						    nullary, false),
				  GlobalVariable::InternalLinkage, 
				  name,
                              m);
  BasicBlock *bb = BasicBlock::Create(getGlobalContext(), "entry", fn);
  
  // From lli:
  // Should be an array of '{ int, void ()* }' structs.  The first value is
  // the init priority, which we ignore.
  ConstantArray *arr = dyn_cast<ConstantArray>(gv->getInitializer());
  if (arr) {
    for (unsigned i=0; i<arr->getNumOperands(); i++) {
      ConstantStruct *cs = cast<ConstantStruct>(arr->getOperand(i));
      assert(cs->getNumOperands()==2 && "unexpected element in ctor initializer list");
      
      Constant *fp = cs->getOperand(1);      
      if (!fp->isNullValue()) {
        if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(fp))
          fp = ce->getOperand(0);

        if (Function *f = dyn_cast<Function>(fp)) {
	  CallInst::Create(f, "", bb);
        } else {
          assert(0 && "unable to get function pointer from ctor initializer list");
        }
      }
    }
  }
  
  ReturnInst::Create(getGlobalContext(), bb);

  return fn;
}

static void injectStaticConstructorsAndDestructors(Module *m) {
  GlobalVariable *ctors = m->getNamedGlobal("llvm.global_ctors");
  GlobalVariable *dtors = m->getNamedGlobal("llvm.global_dtors");
  
  if (ctors || dtors) {
    Function *mainFn = m->getFunction("main");
    if (!mainFn)
      klee_error("Could not find main() function.");

    if (ctors)
    CallInst::Create(getStubFunctionForCtorList(m, ctors, "klee.ctor_stub"),
		     "", mainFn->begin()->begin());
    if (dtors) {
      Function *dtorStub = getStubFunctionForCtorList(m, dtors, "klee.dtor_stub");
      for (Function::iterator it = mainFn->begin(), ie = mainFn->end();
           it != ie; ++it) {
        if (isa<ReturnInst>(it->getTerminator()))
	  CallInst::Create(dtorStub, "", it->getTerminator());
      }
    }
  }
}

#if LLVM_VERSION_CODE < LLVM_VERSION(3, 3)
static void forceImport(Module *m, const char *name, LLVM_TYPE_Q Type *retType,
                        ...) {
  // If module lacks an externally visible symbol for the name then we
  // need to create one. We have to look in the symbol table because
  // we want to check everything (global variables, functions, and
  // aliases).

  Value *v = m->getValueSymbolTable().lookup(name);
  GlobalValue *gv = dyn_cast_or_null<GlobalValue>(v);

  if (!gv || gv->hasInternalLinkage()) {
    va_list ap;

    va_start(ap, retType);
    std::vector<LLVM_TYPE_Q Type *> argTypes;
    while (LLVM_TYPE_Q Type *t = va_arg(ap, LLVM_TYPE_Q Type*))
      argTypes.push_back(t);
    va_end(ap);

    m->getOrInsertFunction(name, FunctionType::get(retType, argTypes, false));
  }
}
#endif

/// This function will take try to inline all calls to \p functionName
/// in the module \p module .
///
/// It is intended that this function be used for inling calls to
/// check functions like <tt>klee_div_zero_check()</tt>
static void inlineChecks(Module *module, const char * functionName) {
  std::vector<CallSite> checkCalls;
    Function* runtimeCheckCall = module->getFunction(functionName);
    if (runtimeCheckCall == 0)
    {
      DEBUG( klee_warning("Failed to inline %s because no calls were made to it in module", functionName) );
      return;
    }

    for (Value::use_iterator i = runtimeCheckCall->use_begin(),
        e = runtimeCheckCall->use_end(); i != e; ++i)
      if (isa<InvokeInst>(*i) || isa<CallInst>(*i)) {
        CallSite cs(*i);
        if (!cs.getCalledFunction())
          continue;
        checkCalls.push_back(cs);
      }

    unsigned int successCount=0;
    unsigned int failCount=0;
    InlineFunctionInfo IFI(0,0);
    for ( std::vector<CallSite>::iterator ci = checkCalls.begin(),
          cie = checkCalls.end();
          ci != cie; ++ci)
    {
      // Try to inline the function
      if (InlineFunction(*ci,IFI))
        ++successCount;
      else
      {
        ++failCount;
        klee_warning("Failed to inline function %s", functionName);
      }
    }

    DEBUG( klee_message("Tried to inline calls to %s. %u successes, %u failures",functionName, successCount, failCount) );
}

void KModule::addInternalFunction(const char* functionName){
  Function* internalFunction = module->getFunction(functionName);
  if (!internalFunction) {
      klee_warning("Failed to add internal function %s. Not found.", functionName);
    //DEBUG_WITH_TYPE("KModule", klee_warning(
        //"Failed to add internal function %s. Not found.", functionName));
    return ;
  }
  klee_message("Added function %s.",functionName);
  //DEBUG( klee_message("Added function %s.",functionName));
  internalFunctions.insert(internalFunction);
}

void KModule::prepare(const Interpreter::ModuleOptions &opts,
                      InterpreterHandler *ih) {
  if (!MergeAtExit.empty()) {
    Function *mergeFn = module->getFunction("klee_merge");
    if (!mergeFn) {
      LLVM_TYPE_Q llvm::FunctionType *Ty = 
        FunctionType::get(Type::getVoidTy(getGlobalContext()), 
                          std::vector<LLVM_TYPE_Q Type*>(), false);
      mergeFn = Function::Create(Ty, GlobalVariable::ExternalLinkage,
				 "klee_merge",
				 module);
    }

    for (cl::list<std::string>::iterator it = MergeAtExit.begin(), 
           ie = MergeAtExit.end(); it != ie; ++it) {
      std::string &name = *it;
      Function *f = module->getFunction(name);
      if (!f) {
        klee_error("cannot insert merge-at-exit for: %s (cannot find)",
                   name.c_str());
      } else if (f->isDeclaration()) {
        klee_error("cannot insert merge-at-exit for: %s (external)",
                   name.c_str());
      }

      BasicBlock *exit = BasicBlock::Create(getGlobalContext(), "exit", f);
      PHINode *result = 0;
      if (f->getReturnType() != Type::getVoidTy(getGlobalContext()))
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 0)
        result = PHINode::Create(f->getReturnType(), 0, "retval", exit);
#else
		result = PHINode::Create(f->getReturnType(), "retval", exit);
#endif
      CallInst::Create(mergeFn, "", exit);
      ReturnInst::Create(getGlobalContext(), result, exit);

      llvm::errs() << "KLEE: adding klee_merge at exit of: " << name << "\n";
      for (llvm::Function::iterator bbit = f->begin(), bbie = f->end(); 
           bbit != bbie; ++bbit) {
        if (&*bbit != exit) {
          Instruction *i = bbit->getTerminator();
          if (i->getOpcode()==Instruction::Ret) {
            if (result) {
              result->addIncoming(i->getOperand(0), bbit);
            }
            i->eraseFromParent();
	    BranchInst::Create(exit, bbit);
          }
        }
      }
    }
  }

  // Inject checks prior to optimization... we also perform the
  // invariant transformations that we will end up doing later so that
  // optimize is seeing what is as close as possible to the final
  // module.
  PassManager pm;
  pm.add(new RaiseAsmPass());
  if (opts.CheckDivZero) pm.add(new DivCheckPass());
  if (opts.CheckOvershift) pm.add(new OvershiftCheckPass());

  //add a pass to get the path before run klee
  //pm.add(new CallPathsPass());
  //CallPathsPass *ps = (CallPathsPass*)createCallPathsPass();
  //if(ps != NULL) 
	  //ps->test();

  // FIXME: This false here is to work around a bug in
  // IntrinsicLowering which caches values which may eventually be
  // deleted (via RAUW). This can be removed once LLVM fixes this
  // issue.
  pm.add(new IntrinsicCleanerPass(*targetData, false));
  pm.run(*module);

  if (opts.Optimize)
    Optimize(module);
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 3)
  // Force importing functions required by intrinsic lowering. Kind of
  // unfortunate clutter when we don't need them but we won't know
  // that until after all linking and intrinsic lowering is
  // done. After linking and passes we just try to manually trim these
  // by name. We only add them if such a function doesn't exist to
  // avoid creating stale uses.

  LLVM_TYPE_Q llvm::Type *i8Ty = Type::getInt8Ty(getGlobalContext());
  forceImport(module, "memcpy", PointerType::getUnqual(i8Ty),
              PointerType::getUnqual(i8Ty),
              PointerType::getUnqual(i8Ty),
              targetData->getIntPtrType(getGlobalContext()), (Type*) 0);
  forceImport(module, "memmove", PointerType::getUnqual(i8Ty),
              PointerType::getUnqual(i8Ty),
              PointerType::getUnqual(i8Ty),
              targetData->getIntPtrType(getGlobalContext()), (Type*) 0);
  forceImport(module, "memset", PointerType::getUnqual(i8Ty),
              PointerType::getUnqual(i8Ty),
              Type::getInt32Ty(getGlobalContext()),
              targetData->getIntPtrType(getGlobalContext()), (Type*) 0);
#endif
  // FIXME: Missing force import for various math functions.

  // FIXME: Find a way that we can test programs without requiring
  // this to be linked in, it makes low level debugging much more
  // annoying.
  llvm::sys::Path path(opts.LibraryDir);
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
  path.appendComponent("kleeRuntimeIntrinsic.bc");
#else
  path.appendComponent("libkleeRuntimeIntrinsic.bca");
#endif
  module = linkWithLibrary(module, path.c_str());

  // Add internal functions which are not used to check if instructions
  // have been already visited
  if (opts.CheckDivZero)
    addInternalFunction("klee_div_zero_check");
  if (opts.CheckOvershift)
    addInternalFunction("klee_overshift_check");


  // Needs to happen after linking (since ctors/dtors can be modified)
  // and optimization (since global optimization can rewrite lists).
  injectStaticConstructorsAndDestructors(module);

  // Finally, run the passes that maintain invariants we expect during
  // interpretation. We run the intrinsic cleaner just in case we
  // linked in something with intrinsics but any external calls are
  // going to be unresolved. We really need to handle the intrinsics
  // directly I think?
  PassManager pm3;
  pm3.add(createCFGSimplificationPass());
  switch(SwitchType) {
  case eSwitchTypeInternal: break;
  case eSwitchTypeSimple: pm3.add(new LowerSwitchPass()); break;
  case eSwitchTypeLLVM:  pm3.add(createLowerSwitchPass()); break;
  default: klee_error("invalid --switch-type");
  }
  pm3.add(new IntrinsicCleanerPass(*targetData));
  pm3.add(new PhiCleanerPass());
  pm3.run(*module);
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 3)
  // For cleanliness see if we can discard any of the functions we
  // forced to import.
  Function *f;
  f = module->getFunction("memcpy");
  if (f && f->use_empty()) f->eraseFromParent();
  f = module->getFunction("memmove");
  if (f && f->use_empty()) f->eraseFromParent();
  f = module->getFunction("memset");
  if (f && f->use_empty()) f->eraseFromParent();
#endif

  // Write out the .ll assembly file. We truncate long lines to work
  // around a kcachegrind parsing bug (it puts them on new lines), so
  // that source browsing works.
  if (OutputSource) {
    std::ostream *os = ih->openOutputFile("assembly.ll");
    assert(os && os->good() && "unable to open source output");

    llvm::raw_os_ostream *ros = new llvm::raw_os_ostream(*os);

    // We have an option for this in case the user wants a .ll they
    // can compile.
    if (NoTruncateSourceLines) {
      *ros << *module;
    } else {
      std::string string;
      llvm::raw_string_ostream rss(string);
      rss << *module;
      rss.flush();
      const char *position = string.c_str();

      for (;;) {
        const char *end = index(position, '\n');
        if (!end) {
          *ros << position;
          break;
        } else {
          unsigned count = (end - position) + 1;
          if (count<255) {
            ros->write(position, count);
          } else {
            ros->write(position, 254);
            *ros << "\n";
          }
          position = end+1;
        }
      }
    }
    delete ros;

    delete os;
  }

  if (OutputModule) {
    std::ostream *f = ih->openOutputFile("final.bc");
    llvm::raw_os_ostream* rfs = new llvm::raw_os_ostream(*f);
    WriteBitcodeToFile(module, *rfs);
    delete rfs;
    delete f;
  }

  kleeMergeFn = module->getFunction("klee_merge");

  /* Build shadow structures */

  infos = new InstructionInfoTable(module);  
  
  unsigned funcID = 1;
  std::ostream *fidFile;
  std::ostream *ciidFile;
  std::ostream *bbidFile;
  std::ostream *srcInfoFile;
  typedef std::map<unsigned, std::set<unsigned> > BBSrcLineMapTy;
  typedef std::map<llvm::Function*, BBSrcLineMapTy> FuncBBMapTy;
  typedef std::map<std::string, FuncBBMapTy> srcBcMapTy;
  srcBcMapTy srcBcMap;
  if( OutputFuncID ) {
      fidFile = ih->openOutputFile("id-funcs.txt");
      ciidFile = ih->openOutputFile("id-callinst.txt");
      bbidFile = ih->openOutputFile("id-bbs.txt");
      srcInfoFile = ih->openOutputFile("src-bc.txt");
  }

  //sort the functions by name, for a fixed order everytime
  std::map<std::string, Function *> functionNames;
  for (Module::iterator fnIt = module->begin(), fn_ie = module->end(); 
          fnIt!=fn_ie; fnIt++) {
      functionNames.insert(std::make_pair(fnIt->getName().str(), fnIt));
  }

  for (std::map<std::string,Function*>::iterator it = functionNames.begin(),
          ie=functionNames.end(); it!=ie; it++) {
    llvm::Function* f = it->second;
    if (f->isDeclaration())
      continue;

    KFunction *kf = new KFunction(f, this);
    kf->fid = funcID++;

    std::string srcFilePath = infos->getFunctionInfo(f).file;
    if( srcFilePath == "") {
        for (unsigned i=0; i<kf->numInstructions; ++i) {
            KInstruction *ki = kf->instructions[i];
            ki->info = &infos->getInfo(ki->inst);
            if( ki->info->file != "" ) {
                srcFilePath  = ki->info->file;
                break;
            }
        }
        if( srcFilePath == "") 
            srcFilePath = "nofilefind";
    }
    FuncBBMapTy  &funcBBMap = srcBcMap[srcFilePath];
    BBSrcLineMapTy &bbSrcLineMap = funcBBMap[f];
    
    for (unsigned i=0; i<kf->numInstructions; ++i) {
      KInstruction *ki = kf->instructions[i];
      ki->info = &infos->getInfo(ki->inst);
      if( OutputFuncID && (isa<CallInst>(ki->inst) || isa<InvokeInst>(ki->inst)) ) {
      //if( isa<KCallInstruction>(ki) ) {
          KCallInstruction *kci = static_cast<KCallInstruction*>(ki);
          const std::string &pathFile = kci->info->file;
          //klee_xqx_debug("%s\n", pathFile.c_str() );
          *ciidFile << kci->kiid << "\t\t" << kf->function->getNameStr().c_str() 
             << "\t\t\t\t" << pathFile.substr(pathFile.find_last_of("/")+1)
             << ":" << kci->info->line << ":" << kci->info->assemblyLine << "\n";
      }
      unsigned bbID = kf->basicBlockID[ki->inst->getParent()];
      unsigned srcLineNum = ki->info->line;
      bbSrcLineMap[bbID].insert(srcLineNum);

    }

    functions.push_back(kf);
    functionMap.insert(std::make_pair(f, kf));

    if( OutputFuncID ) {
        *fidFile << kf->fid  << "   " << f->getNameStr().c_str() << "\n";
#if 1
        Function::iterator fit = f->begin(), fie=f->end();
        for( ; fit!=fie; fit++) {
          const InstructionInfo  &ii = infos->getInfo(&fit->front());
          const std::string &pathFile = ii.file;
          *bbidFile << kf->basicBlockID[fit] << "\t\t" << kf->function->getNameStr().c_str() 
             << "\t\t\t\t" << pathFile.substr(pathFile.find_last_of("/")+1)
             << ":" << ii.line << ":" << ii.assemblyLine << "\n";
        }
#endif
    }
  } //for(std::map<std::string

  
  if( OutputFuncID ) {
      srcBcMapTy::iterator sit = srcBcMap.begin(), sie = srcBcMap.end();
      for( ; sit!=sie; sit++) {
          *srcInfoFile << "File: " << sit->first << "\n";
          FuncBBMapTy::iterator fit = sit->second.begin(), fie = sit->second.end();
          for(; fit!=fie; fit++) {
              llvm::Function* f =  fit->first;
              *srcInfoFile << "Func: " << f->getNameStr() << ":\n";
              BBSrcLineMapTy::iterator bit=fit->second.begin(), bie=fit->second.end();
              for(; bit!=bie; bit++) {
                  std::set<unsigned>::iterator it=bit->second.begin(), ie=bit->second.end();
                  for(; it!=ie; it++) {
                      if( *it == 0 ) continue;
                      *srcInfoFile << bit->first << "  " << *it << "\n";
                  }
              }
          }
      }
      delete srcInfoFile;
      delete fidFile;
      delete ciidFile;
      delete bbidFile;
  }

  /* Compute various interesting properties */

  for (std::vector<KFunction*>::iterator it = functions.begin(), 
         ie = functions.end(); it != ie; ++it) {
    KFunction *kf = *it;
    if (functionEscapes(kf->function))
      escapingFunctions.insert(kf->function);
  }

  if (DebugPrintEscapingFunctions && !escapingFunctions.empty()) {
    llvm::errs() << "KLEE: escaping functions: [";
    for (std::set<Function*>::iterator it = escapingFunctions.begin(), 
         ie = escapingFunctions.end(); it != ie; ++it) {
      llvm::errs() << (*it)->getName() << ", ";
    }
    llvm::errs() << "]\n";
  }
  
#ifdef XQX_DEBUG_PATCH_CRCERROR
  //addbyxqx
  if(PatchCrc){
  if (OutputSource) {
    std::ostream *os = ih->openOutputFile("patched.ll");
    assert(os && os->good() && "unable to open source output");

    llvm::raw_os_ostream *ros = new llvm::raw_os_ostream(*os);

    // We have an option for this in case the user wants a .ll they
    // can compile.
    if (NoTruncateSourceLines) {
      *ros << *module;
    } else {
      std::string string;
      llvm::raw_string_ostream rss(string);
      rss << *module;
      rss.flush();
      const char *position = string.c_str();

      for (;;) {
        const char *end = index(position, '\n');
        if (!end) {
          *ros << position;
          break;
        } else {
          unsigned count = (end - position) + 1;
          if (count<255) {
            ros->write(position, count);
          } else {
            ros->write(position, 254);
            *ros << "\n";
          }
          position = end+1;
        }
      }
    }
    delete ros;

    delete os;
  }
  }
#endif
}

KConstant* KModule::getKConstant(Constant *c) {
  std::map<llvm::Constant*, KConstant*>::iterator it = constantMap.find(c);
  if (it != constantMap.end())
    return it->second;
  return NULL;
}

unsigned KModule::getConstantID(Constant *c, KInstruction* ki) {
  KConstant *kc = getKConstant(c);
  if (kc)
    return kc->id;  

  unsigned id = constants.size();
  kc = new KConstant(c, id, ki);
  constantMap.insert(std::make_pair(c, kc));
  constants.push_back(c);
#ifdef XQX_DEBUG_KMOUDLE
  klee_xqx_debug("insert constant+++++++++++id=%d",id);
  c->dump();
#endif
  return id;
}

/***/

KConstant::KConstant(llvm::Constant* _ct, unsigned _id, KInstruction* _ki) {
  ct = _ct;
  id = _id;
  ki = _ki;
}

/***/

static int getOperandNum(Value *v,
                         std::map<Instruction*, unsigned> &registerMap,
                         KModule *km,
                         KInstruction *ki) {
  if (Instruction *inst = dyn_cast<Instruction>(v)) {
    return registerMap[inst];
  } else if (Argument *a = dyn_cast<Argument>(v)) {
    return a->getArgNo();
  } else if (isa<BasicBlock>(v) || isa<InlineAsm>(v) ||
             isa<MDNode>(v)) {
    return -1;
  } else {
    assert(isa<Constant>(v));
    Constant *c = cast<Constant>(v);
    return -(km->getConstantID(c, ki) + 2);
  }
}

KFunction::KFunction(llvm::Function *_function,
                     KModule *km) 
  : function(_function),
    numArgs(function->arg_size()),
    numInstructions(0),
    isFocusedFunc(false),
    trackCoverage(true) {
  for (llvm::Function::iterator bbit = function->begin(), 
         bbie = function->end(); bbit != bbie; ++bbit) {
    BasicBlock *bb = bbit;
    basicBlockEntry[bb] = numInstructions;
    //basicBlockID[bb] = ++bbIndex;
    basicBlockID[bb] = ++km->bbNum;
    numInstructions += bb->size();
  }
  //klee_xqx_debug("bbidnum = %d\n", basicBlockID.size());


  instructions = new KInstruction*[numInstructions];

  std::map<Instruction*, unsigned> registerMap;

  // The first arg_size() registers are reserved for formals.
  unsigned rnum = numArgs;
  for (llvm::Function::iterator bbit = function->begin(), 
         bbie = function->end(); bbit != bbie; ++bbit) {
    for (llvm::BasicBlock::iterator it = bbit->begin(), ie = bbit->end();
         it != ie; ++it)
      registerMap[it] = rnum++;
  }
  numRegisters = rnum;
  
#ifdef XQX_DEBUG_PATCH_CRCERROR
  bool in_crcerror = false;
  bool in_png_calc_crc = false;
  if(PatchCrc){
  if( function->getNameStr() == "png_crc_error" ) {
	  klee_xqx_debug("kfunction----%s", function->getName());
	  in_crcerror = true;
  }
  if( function->getNameStr() == "png_calculate_crc" ) {
	  klee_xqx_debug("kfunction----%s", function->getName());
	  in_png_calc_crc = true;
  }
  }
#endif

  unsigned i = 0;
  for (llvm::Function::iterator bbit = function->begin(), 
         bbie = function->end(); bbit != bbie; ++bbit) {
    for (llvm::BasicBlock::iterator it = bbit->begin(), ie = bbit->end();
         it != ie; ++it) {
      KInstruction *ki;

      switch(it->getOpcode()) {
      case Instruction::GetElementPtr:
      case Instruction::InsertValue:
      case Instruction::ExtractValue:
        ki = new KGEPInstruction(); break;
      case Instruction::Call:
      case Instruction::Invoke:
        ki = new KCallInstruction() ; 
        static_cast<KCallInstruction*>(ki)->kiid = ++ciIndex; break;
      default:
        ki = new KInstruction(); break;
      }

      ki->inst = it;      
      ki->dest = registerMap[it];
#ifdef XQX_DEBUG_PATCH_CRCERROR
  if(PatchCrc){
	  if( in_crcerror ) {
		  if( PHINode *phi = dyn_cast<PHINode>(it) ){
			  it->dump();
			  //unsigned numOperands = it->getNumOperands();
			  //for (unsigned j=0; j<numOperands; j++) {
				  //Value *v = it->getOperand(j);
				  //v->dump();
			  //}
			  it->setOperand(0,it->getOperand(2));
			  it->dump();
		  }
		  // if add "-g" 
		  if( StoreInst *SI = dyn_cast<StoreInst>(it) ) {
			  //klee_message("store in %s by ", SI->getOperand(1)->getName().str().c_str());
			  if( SI->getOperand(1)->getName().str() == "need_crc") {
				  if( ConstantInt *CI = dyn_cast<ConstantInt>(SI->getOperand(0)) ) {
					  if( CI->isOne() ) 
						  SI->setOperand(0,llvm::ConstantInt::get(SI->getOperand(0)->getType(), 0) );
				  }
			  }
		  }

	  }
	  if( in_png_calc_crc ) {
		  if( BranchInst *BrI = dyn_cast<BranchInst>(it) ) {
			  //klee_message("br: succ0:%s", BrI->getSuccessor(0)->getName().str().c_str());
			  //klee_message("br: succ1:%s", BrI->getSuccessor(1)->getName().str().c_str());
			  if ( BrI->getParent()->getNameStr() == "bb" ) {
				  BrI->setCondition(llvm::ConstantInt::get(BrI->getCondition()->getType(),1) );

			  }
			  //1.4.2 :0   1.7.0 :1
			  if ( BrI->getParent()->getNameStr() == "bb2" ) 
				  BrI->setCondition(llvm::ConstantInt::get(BrI->getCondition()->getType(),1) );
		  }
	  }
  }
#endif


      if (isa<CallInst>(it) || isa<InvokeInst>(it)) {
        CallSite cs(it);
        unsigned numArgs = cs.arg_size();
        ki->operands = new int[numArgs+1];
#ifdef XQX_DEBUG_KMOUDLE
		klee_xqx_debug("kfunction----%s",cs.getCalledValue()->getName());
		klee_xqx_debug("kfunction---cs dump-");
#endif
        ki->operands[0] = getOperandNum(cs.getCalledValue(), registerMap, km,
                                        ki);
        for (unsigned j=0; j<numArgs; j++) {
          Value *v = cs.getArgument(j);
          ki->operands[j+1] = getOperandNum(v, registerMap, km, ki);
        }
      } 
	  else {
        unsigned numOperands = it->getNumOperands();
        ki->operands = new int[numOperands];
        for (unsigned j=0; j<numOperands; j++) {
          Value *v = it->getOperand(j);
          ki->operands[j] = getOperandNum(v, registerMap, km, ki);
        }
      }

      instructions[i++] = ki;
    }
  }
}

KFunction::~KFunction() {
  for (unsigned i=0; i<numInstructions; ++i)
    delete instructions[i];
  delete[] instructions;
}
