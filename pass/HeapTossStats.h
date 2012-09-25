/*
 * HeapTossStats.h
 *
 *  Created on: Mar 20, 2012
 *      Author: jvilk
 */
#ifndef HEAPTOSSSTATS_H_
#define HEAPTOSSSTATS_H_
#include <iostream>
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <ctime>

#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Constants.h"
#include "llvm/Type.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Value.h"
#include "llvm/Target/TargetData.h"
#include "llvm/LLVMContext.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/PassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IntrinsicInst.h"

using namespace std;
using namespace llvm;

/**
 * Adds instrumentation to the program for stat collection, and collect static compile-time stats.
 */
class HeapTossStats {
private:
  map<Function*, unsigned> fcnIds;
  map<Function*, unsigned> fcnNumTossed;
  map<Function*, unsigned> fcnStackSlots;
  map<Function*, unsigned> fcnDynamicSlots;
  map<Function*, unsigned> fcnDynamicNumTossed;
  unsigned nextFcnId;
  Function * heaptoss_dynamic_toss;
  Function * heaptoss_malloc_size;
  Function * heaptoss_fcn_run;
  Function * heaptoss_fcn_ret;
  Function * heaptoss_memintrinsic_execution;
  Function * heaptoss_initialize;
  bool enabled;
  Type * ptrType;

public:
  HeapTossStats(Module &M, Type* ptrType, bool enabled) {
    this->ptrType = ptrType;
    this->enabled = enabled;
    this->nextFcnId = 0;
    //Grab the library functions.
    //TODO: If the # of functions grows significantly larger, may want to make a helper function.
    if (enabled)
    {
      Constant * heaptoss_fcn_run_c = M.getOrInsertFunction("heaptoss_fcn_run", FunctionType::getVoidTy(M.getContext()), ptrType, NULL);
      Constant * heaptoss_fcn_ret_c = M.getOrInsertFunction("heaptoss_fcn_ret", FunctionType::getVoidTy(M.getContext()), ptrType, NULL);
      Constant * heaptoss_malloc_size_c = M.getOrInsertFunction("heaptoss_malloc_size", FunctionType::getVoidTy(M.getContext()), ptrType, ptrType, NULL);
      Constant * heaptoss_dynamic_toss_c = M.getOrInsertFunction("heaptoss_dynamic_toss", FunctionType::getVoidTy(M.getContext()), ptrType, ptrType, NULL);
      Constant * heaptoss_memintrinsic_execution_c = M.getOrInsertFunction("heaptoss_memintrinsic_execution", FunctionType::getVoidTy(M.getContext()), ptrType, ptrType, NULL);
      Constant * heaptoss_initialize_c = M.getOrInsertFunction("heaptoss_initialize", FunctionType::getVoidTy(M.getContext()), ptrType, NULL);

      if (!isa<Function>(heaptoss_fcn_run_c) || !isa<Function>(heaptoss_fcn_ret_c)
          || !isa<Function>(heaptoss_malloc_size_c) || !isa<Function>(heaptoss_dynamic_toss_c)
          || !isa<Function>(heaptoss_memintrinsic_execution) || !isa<Function>(heaptoss_initialize))
      {
        errs() << "ERROR: Need to link heaptoss runtime library in order to enable dynamic statistic collecting.\n";
        exit(1);
      }
      heaptoss_fcn_run = dyn_cast<Function>(heaptoss_fcn_run_c);
      heaptoss_malloc_size = dyn_cast<Function>(heaptoss_malloc_size_c);
      heaptoss_dynamic_toss = dyn_cast<Function>(heaptoss_dynamic_toss_c);
      heaptoss_memintrinsic_execution = dyn_cast<Function>(heaptoss_memintrinsic_execution_c);
      heaptoss_fcn_ret = dyn_cast<Function>(heaptoss_fcn_ret_c);
      heaptoss_initialize = dyn_cast<Function>(heaptoss_initialize_c);
    }
  }

  void addMemIntrinsic(MemIntrinsic * memIntrinsic) {
    if (!enabled) return;

    unsigned miId;

    if (isa<MemSetInst>(memIntrinsic)) {
      miId = 0;
    }
    else if (isa<MemCpyInst>(memIntrinsic)) {
      miId = 1;
    }
    else if (isa<MemMoveInst>(memIntrinsic)) {
      miId = 2;
    }
    else {
      errs() << "ERROR: Unknown MemIntrinsic function!\n";
      memIntrinsic->print(errs()); errs() << "\n";
      exit(1);
    }

    std::vector<Value*> htMiArgs;
    htMiArgs.push_back(ConstantInt::get(ptrType, miId, false));
    htMiArgs.push_back(memIntrinsic->getLength());
    CallInst::Create(heaptoss_memintrinsic_execution, htMiArgs, "", memIntrinsic);
  }

  void addFunction(Function* f) {
    if (!enabled) return;

    fcnNumTossed[f] = 0;
    fcnStackSlots[f] = 0;
    fcnIds[f] = nextFcnId;
    nextFcnId++;

    //Insert call to heaptoss_fcn_run so we can record the number of times
    //this function is run.
    BasicBlock * first = &f->getEntryBlock();
    Instruction * firstInst = first->getFirstNonPHI();
    std::vector<Value*> htFcnRunArgs;
    Constant * fcnIdConst = ConstantInt::get(ptrType, fcnIds[f], false);
    htFcnRunArgs.push_back(fcnIdConst);
    CallInst::Create(heaptoss_fcn_run, htFcnRunArgs, "", firstInst);
  }

  void addTerminator(Function* f, Instruction* terminator) {
    if (!enabled) return;

    std::vector<Value*> htFcnRetArgs;
    Constant * fcnIdConst = ConstantInt::get(ptrType, fcnIds[f], false);
    htFcnRetArgs.push_back(fcnIdConst);
    CallInst::Create(heaptoss_fcn_ret, htFcnRetArgs, "", terminator);
  }

  void addDynamicToss(Function* f, Value* size, Instruction* tossInstruction) {
    if (!enabled) return;

    //Insert instruction to call dynamic toss thing.
    std::vector<Value*> htDynamicTossArgs;
    htDynamicTossArgs.push_back(ConstantInt::get(ptrType, fcnIds[f], false));

    // Sizes are 64-bit. Need to bitcast on 32-bit platforms.
    if (size->getType() != ptrType) {
      size = BitCastInst::CreateIntegerCast(size, ptrType, false, "", tossInstruction);
    }
    htDynamicTossArgs.push_back(size);
    CallInst::Create(heaptoss_dynamic_toss, htDynamicTossArgs, "", tossInstruction);
  }

  void setSize(Function *f, Value* size) {
    if (!enabled) return;

    Instruction * insertBefore = f->getEntryBlock().getFirstNonPHI();

    std::vector<Value*> htMallocSizeArgs;
    htMallocSizeArgs.push_back(ConstantInt::get(ptrType, fcnIds[f], false));

    // Sizes are 64-bit. Need to bitcast on 32-bit platforms.
    if (size->getType() != ptrType) {
      size = BitCastInst::CreateIntegerCast(size, ptrType, false, "", insertBefore);
    }
    htMallocSizeArgs.push_back(size);
    CallInst::Create(heaptoss_malloc_size, htMallocSizeArgs, "", insertBefore);
  }

  void setStaticStats(Function *f,
      unsigned staticNumTossed, unsigned totalStackSlots,
      unsigned dynamicNumTossed, unsigned totalDynamicSlots) {
    if (!enabled) return;
    fcnNumTossed[f] = staticNumTossed;
    fcnStackSlots[f] = totalStackSlots;
    fcnDynamicNumTossed[f] = dynamicNumTossed;
    fcnDynamicSlots[f] = totalDynamicSlots;
  }

  void alterStaticNumTossed(Function *f, unsigned staticNumTossed) {
    fcnNumTossed[f] = staticNumTossed;
  }

  bool fexists(const char *filename)
  {
    ifstream ifile(filename);
    return ifile;
  }

  void outputStats(Module &M) {
    if (!enabled) return;
    stringstream outputFileName;
    const char* filename;
    unsigned i = 0;
    do {
      outputFileName.str(std::string());
      outputFileName << "htstats_compile_" << i++ << ".csv";
      filename = outputFileName.str().c_str();
    } while (fexists(filename));

    errs() << "Outputting static statistics to " << filename  << "...\n";

    ofstream outFile;
    outFile.open(filename, ios::out);

    outFile << "Function ID,Function Name,Static Tosses,Stack Slots,Dynamic Tosses,Dynamic Slots\n";
    for (map<Function*, unsigned>::iterator i = fcnIds.begin(); i != fcnIds.end(); i++) {
      unsigned fcnId = i->second;
      Function * f = i->first;
      outFile << fcnId << "," << f->getName().data() << "," << fcnNumTossed[f] << ","
          << fcnStackSlots[f] << "," << fcnDynamicNumTossed[f] << ","
          << fcnDynamicSlots[f] << "\n";
    }

    outFile.close();
  }

  void insertInitialization(Function * main) {
    if (!enabled) return;

    if (main == NULL) {
      errs() << "ERROR: Main function not found!\n";
      exit(1);
    }

    Instruction * firstInst = main->getEntryBlock().getFirstNonPHI();
    std::vector<Value*> htInitArgs;
    htInitArgs.push_back(ConstantInt::get(ptrType, nextFcnId, false));
    CallInst::Create(heaptoss_initialize, htInitArgs, "", firstInst);
  }
};

#endif /* HEAPTOSSSTATS_H_ */
