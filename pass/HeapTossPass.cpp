#include "HeapTossStats.h"

// Are we running HeapToss through opt, or through clang? If it's through clang,
// all configuration must be done statically.
#define RUN_HT_THROUGH_OPT true

#if RUN_HT_THROUGH_OPT
  cl::opt<bool> TOSS_INDIVIDUALLY   ("ht-toss-individually", cl::init(false), cl::desc("Toss every stack variable individually, as opposed to tossing them all at once."));
  cl::opt<bool> TOSS_ALL ("ht-toss-all", cl::init(false), cl::desc("Do not use a tossing heuristic, and simply toss every stack variable into the heap."));
  cl::opt<bool> TOSS_NONE ("ht-toss-none", cl::init(false), cl::desc("Do not toss any stack variables into the heap. Primarily useful for viewing dynamic statistics on a program without tossing anything, or for viewing the impact of changing the alignment of MemIntrinsics to 1."));
  cl::opt<bool> GATHER_STATS ("ht-gather-stats", cl::init(false), cl::desc("Modify the program to gather statistics at runtime (function run count, etc). You must link the program against libHeapToss for this to work."));
  cl::opt<bool> MALLOC_NO_TOSS ("ht-malloc-no-toss", cl::init(false), cl::desc("(For RM) Call malloc/free to allocate/deallocate memory in the heap for stack variables, but do not actually toss any of the stack variables."));
  cl::opt<unsigned> RANDOM_TOSS ("ht-random-toss", cl::init(0), cl::desc("(For RM) Randomly toss stack variables in a deterministic fashion. Set to 0 to disable. Any other number will be used as the seed to the random number generator used to decide if a variable gets tossed. Note that this does not change the size arguments to malloc."));
  cl::opt<bool> REMOVE_RANDOM_TOSS_FROM_STRUCT ("ht-random-toss-change-malloc-size", cl::init(false), cl::desc("(For RM) [MUST BE USED WITH ht-random-toss!] Same as ht-random-toss, except the size argument to malloc is changed according to the size of the tossed variables. If no variables are tossed, we still call malloc with 0."));
#else
  const bool TOSS_INDIVIDUALLY = false;
  const bool TOSS_ALL = false;
  const bool TOSS_NONE = false;
  const bool GATHER_STATS = false;
  const bool MALLOC_NO_TOSS = false;
  const unsigned RANDOM_TOSS = 0;
  const bool REMOVE_RANDOM_TOSS_FROM_STRUCT = false;
#endif

/**
 * HeapTossPass
 *
 * Finds arguments to function calls that refer to an object on the stack via a pointer and
 * allocates the original objects ("tosses" them) in the heap instead.
 *
 * ISSUES:
 *  - Volatile variables -- tough.
 *  - Alloca alignment.
 */
struct HeapTossPass: public ModulePass {
  static char ID;

  IntegerType * ptrType;
  unsigned int ptrWidth;

  //Contains all variables that need to be tossed into the heap
  //for a function.
  //These can only be statically sized variables.
  set<AllocaInst *> toTossStatic;
  //Dynamic allocas to be tossed.
  set<AllocaInst *> toTossDynamic;

  //Contains all of the instructions that terminate the current function call.
  set<Instruction *> terminatorInsts;

  HeapTossStats * stats;

  //Used for handy debugging.
  Function * currentFunction;

  HeapTossPass() : ModulePass(ID) {

  }

  /**
   * Removes variables that do not escape from the input set.
   */
  void filterUnescapingVariables(set<AllocaInst*> & allocas) {
    set<AllocaInst *> filteredAllocas;
    for (set<AllocaInst *>::iterator a_iter = allocas.begin(); a_iter != allocas.end(); a_iter++)
    {
      AllocaInst * aInst = dyn_cast<AllocaInst>(*a_iter);

      if (canEscape(aInst)) {
        filteredAllocas.insert(aInst);
      }
    }

    //Replace the input set with the filtered set.
    allocas.clear();
    allocas.swap(filteredAllocas);
  }
    
  /**
   * Check if destination BasicBlock is reachable from source BasicBlock.
   * Implemented as a DFS.
   *
   * Requires an input set to avoid infinite loops due to loops.
   *
   * TODO: Uh, this doesn't do what I wanted it to do. DO NOT USE.
   */
  bool isReachable(set<BasicBlock*> & searchedBlocks, BasicBlock * source, BasicBlock * dest) {
    return true;

    if (source == dest) return true;
    searchedBlocks.insert(source);

    unsigned successors = source->getTerminator()->getNumSuccessors();
    for (unsigned i = 0; i < successors; i++) {
      BasicBlock * successor = source->getTerminator()->getSuccessor(i);
      if (searchedBlocks.find(successor) != searchedBlocks.end()) {
        if (isReachable(searchedBlocks, successor, dest)) {
          return true;
        }
      }
    }
      
    return false;
  }

  /**
   * Wrapper function for the other isReachable.
   */
  bool isReachable(BasicBlock* source, BasicBlock* dest) {
    set<BasicBlock*> searchedBlocks;
    return isReachable(searchedBlocks, source, dest);
  }

  /**
   * Replace the given alloca instruction with another instruction. This is a good
   * point to insert other post-processing.
   */
  void replaceAlloca(AllocaInst * alloca, Instruction * replacement) {
    alloca->replaceAllUsesWith(replacement);
    alloca->eraseFromParent();
  }

  /**
   * Inserts a call to malloc before insertBefore with the given size argument.
   * Also calls free before all of the reachable terminators.
   */
  Instruction * callMalloc(Instruction* insertBefore, Type * type, Value * size, set<Instruction *> & terminators) {
    Instruction * call = CallInst::CreateMalloc(insertBefore, ptrType, type, size);

    BasicBlock * parentBlock = insertBefore->getParent();
    Function * parentFunction = parentBlock->getParent();
    //If we are inserting malloc in the first basic block, then all blocks in the function are reachable.
    //(Common case)
    bool isFirstBlock = &parentFunction->getEntryBlock() == parentBlock;

    for (set<Instruction *>::iterator i = terminators.begin(); i != terminators.end(); i++) {
      Instruction * terminator = dyn_cast<Instruction>(*i);
      //Note: isReachable does not work.
      if (isFirstBlock || isReachable(parentBlock, terminator->getParent())) {
        CallInst::CreateFree(call, terminator);
        stats->addTerminator(currentFunction, terminator);
      }
    }

    return call;
  }

  /**
   * Get a value representing the size of an alloca.
   * If the alloca is dynamically sized, then it inserts an instruction just before it to calculate
   * the size.
   */
  Value * getSize(AllocaInst * alloca) {
    Type * elementType = alloca->getAllocatedType();
    Value * arraySize = alloca->getArraySize();

    Constant * elementSize = ConstantExpr::getSizeOf(elementType);

    //If arraySize is a constant, then we can use a constant expression.
    if (isa<Constant>(arraySize)) {
      Constant * constantSize = dyn_cast<Constant>(arraySize);
      return ConstantExpr::getMul(constantSize, elementSize);
    }

    //Looks like we're going to calculate this dynamically!
    return BinaryOperator::Create(BinaryOperator::Mul, elementSize, arraySize, "", alloca);
  }

  /**
   * Tosses all of the variables in toToss with individual malloc/free calls.
   */
  void tossIndividually(set<AllocaInst*> & allocas, set<Instruction*> & terminators) {
    if (allocas.size() == 0) return;

    //Toss each variable in the input alloca set.
    for (set<AllocaInst *>::iterator a_iter = allocas.begin(); a_iter != allocas.end(); a_iter++)
    {
      AllocaInst * aInst = dyn_cast<AllocaInst>(*a_iter);
      Value * size = getSize(aInst);
      Instruction * call = callMalloc(aInst, aInst->getAllocatedType(), size, terminators);
      //TODO: Is this still needed now that I use CreateMalloc?
      BitCastInst * bitcast = new BitCastInst(call, aInst->getType(), "", aInst);

      //Stat collection for dynamic allocas.
      if (!aInst->isStaticAlloca()) {
        stats->addDynamicToss(aInst->getParent()->getParent(), size, bitcast);
      }
      replaceAlloca(aInst, bitcast);
    }
  }

  /**
   * Creates a struct to store all of the local variables, and then calls 'malloc' on it.
   */
  void tossTogetherElement(set<AllocaInst*> & allocas, set<Instruction *> & terminators) {
    if (allocas.size() == 0) return;

    Instruction * firstInst = dyn_cast<AllocaInst>(*(allocas.begin()))->getParent()->getFirstNonPHI();

    //Used by RANDOM_TOSS
    bool * shouldToss;
    unsigned numTossed = 0;
    unsigned int rindex = 0;

    if (RANDOM_TOSS > 0) {
      unsigned numAllocas = allocas.size();
      shouldToss = (bool *) malloc(sizeof(bool)*numAllocas);
      for (unsigned i = 0; i < numAllocas; i++) {
        bool randBool = rand()%2;
        if (randBool) numTossed++;
        shouldToss[i] = randBool;
      }

      stats->alterStaticNumTossed(firstInst->getParent()->getParent(), numTossed);
    }

    //Contains the type for each element in the struct.
    //TODO: Would it make any difference how these are ordered?
    std::vector<Type *> structElements;
    for (set<AllocaInst *>::iterator a_iter = allocas.begin(); a_iter != allocas.end(); a_iter++)
    {
      //shouldToss is only initialized if RANDOM_TOSS is set, which should be the case if
      //REMOVE_RANDOM_TOSS_FROM_STRUCT is set.
      if (!REMOVE_RANDOM_TOSS_FROM_STRUCT || shouldToss[rindex]) {
        AllocaInst * aInst = dyn_cast<AllocaInst>(*a_iter);

        if (aInst->isStaticAlloca()) {
          structElements.push_back(aInst->getAllocatedType());
        }
        else {
          errs() << "ERROR: TRYING TO TOSS NON STATIC ALLOCA IN A STRUCT\n";
          exit(1);
        }
      }

      //Used by REMOVE_RANDOM_TOSS_FROM_STRUCT
      rindex++;
    }

    Instruction * mallocCall;
    Type * structType;
    Constant * structSize;

    if (!REMOVE_RANDOM_TOSS_FROM_STRUCT || numTossed > 0) {
      //Create the struct type.
      structType = StructType::create(structElements, "locals", true);
      structSize = ConstantExpr::getSizeOf(structType);

      stats->setSize(firstInst->getParent()->getParent(), structSize);

      //Insert the malloc and free calls.
      mallocCall = callMalloc(firstInst, structType, structSize, terminators);
    }
    //If RANDOM_TOSS causes no stack slots to be tossed, then we call malloc with 0.
    else {
      structSize = ConstantInt::get(Type::getInt64Ty(firstInst->getContext()), 0, false);
      stats->setSize(firstInst->getParent()->getParent(), structSize);
      mallocCall = callMalloc(firstInst, Type::getInt8PtrTy(firstInst->getContext()), structSize, terminators);
    }


    if (!MALLOC_NO_TOSS) {
      //Replace all of the variables with getElementPtrs.
      unsigned int index = 0;
      //Used by REMOVE_RANDOM_TOSS_FROM_STRUCT
      unsigned int index2 = 0;

      for (set<AllocaInst *>::iterator a_iter = allocas.begin(); a_iter != allocas.end(); a_iter++)
      {
        AllocaInst * aInst = dyn_cast<AllocaInst>(*a_iter);
        std::vector<Value *> indices;

        if (RANDOM_TOSS == 0 || shouldToss[index2]) {
          //0 to dereference first pointer.
          //TODO: Should use 32 rather than ptrtype???
          //indices.push_back(Constant::getIntegerValue(ptrType, APInt(32, 0)));

          //Now the index of this variable.
          //indices.push_back(Constant::getIntegerValue(ptrType, APInt(32, index)));
          indices.push_back(Constant::getIntegerValue(Type::getInt32Ty(aInst->getContext()), APInt(32, 0)));
          indices.push_back(Constant::getIntegerValue(Type::getInt32Ty(aInst->getContext()), APInt(32, index)));

          GetElementPtrInst * elementPtrInst = GetElementPtrInst::Create(mallocCall, indices, "", aInst);

          replaceAlloca(aInst, elementPtrInst);

          index++;
          index2++;

          if (REMOVE_RANDOM_TOSS_FROM_STRUCT) {
            //Reverse index counting if we should not toss it.
            if (!shouldToss[index2]) index--;
          }
        }
      }
    }
  }

  /**
   * Tosses all of the variables in toToss.
   * Also responsible for updating global tossing statistics.
   */
  void tossAll(Function * f) {
    unsigned stackSlots = toTossStatic.size();
    unsigned dynamicSlots = toTossDynamic.size();

    //Filter out variables that do not escape.
    filterUnescapingVariables(toTossStatic);
    filterUnescapingVariables(toTossDynamic);

    //Stop if there's nothing to toss.
    if (toTossStatic.size() + toTossDynamic.size() == 0) return;

    unsigned tossedStackSlots = toTossStatic.size();
    unsigned tossedDynamicSlots = toTossDynamic.size();

    stats->setStaticStats(f, tossedStackSlots, stackSlots, tossedDynamicSlots, dynamicSlots);

    //Call the appropriate function to toss the variables.
    if (tossedStackSlots > 0 && !TOSS_NONE) {
      //If the function returns, has stack slots to be tossed, and has no known function terminators... there's a problem.
      if (terminatorInsts.size() == 0 && !currentFunction->doesNotReturn()) {
        errs() << "That's weird. I'm trying to toss variables without knowing where I can free them. Memory leak!\n";
        currentFunction->print(errs()); errs() << "\n";
        exit(1);
      }

      if (TOSS_INDIVIDUALLY) {
        tossIndividually(toTossStatic, terminatorInsts);
      }
      else {
        tossTogetherElement(toTossStatic, terminatorInsts);
      }

      if (tossedDynamicSlots > 0) {
        //errs() << "TossDynamic\n";
        //Tosses the dynamic allocas. These usually cannot be grouped.
        //TODO: Reenable after fixing.
        //tossIndividually(toTossDynamic, terminatorInsts);
      }
    }
  }

  /**
   * Checks if a value's address can escape.
   */
  bool canEscape(Value * stackSlot)
  {
    if (TOSS_ALL) return true;

    //If the variable is only loaded/stored to, then it doesn't need to be tossed.
    //Iterate through its uses.
    for (Value::use_iterator uIter = stackSlot->use_begin(); uIter != stackSlot->use_end(); uIter++)
    {
      //Don't want its address leaking.
      Value * useVal = *uIter;
      //OK if just loaded.
      if (isa<LoadInst>(useVal))
      {
        LoadInst * load = dyn_cast<LoadInst>(useVal);
        //Are we loading FROM this stack slot?
        Value * val = load->getOperand(0);

        if (stackSlot == val)
          //This load is safe; continue on to the next use.
          continue;
      }
      //OK if stored into.
      else if (isa<StoreInst>(useVal))
      {
        StoreInst * store = dyn_cast<StoreInst>(useVal);
        //Are we storing TO this stack slot?
        Value * val = store->getOperand(0);

        if (stackSlot == val)
          //This store is safe.
          continue;
      }
      else if (isa<GetElementPtrInst>(useVal))
      {
        GetElementPtrInst * getEmPtr = dyn_cast<GetElementPtrInst>(useVal);

        //The stack slot should be the first argument.
        Value * arg0 = getEmPtr->getOperand(0);

        if (arg0 == stackSlot)
        {
          //It's only OK if the element ptr can't escape.
          if (!canEscape(useVal))
            continue;
        }
      }

      /**errs() << "Violater Instruction for variable ";
      stackSlot->print(errs());
      errs() << ":\n";
      useVal->print(errs());
      errs() << "\n";**/

      //Anything else is bad!
      return true;
    }

    //We can confidently say that all uses are store/loads, and this variable can not escape.
    return false;
  }

  /**
   * Finds all of the stack variables in the basic block, and tosses them in the
   * heap if need be.
   * It will continue on to all successor basic blocks.
   */
  void processBlock(BasicBlock * b) {
    Function * parent = b->getParent();
    bool isEntry = b == &parent->getEntryBlock();

    //Find all allocas.
    for (BasicBlock::iterator i = b->begin(); i != b->end(); i++) {
      if (isa<AllocaInst>(i))
      {
        AllocaInst * aInst = dyn_cast<AllocaInst>(i);
        //Even if it is a static alloca, we assume that static allocas
        //only occur in the first block. So, just add it to the dynamic
        //pile.
        if (!isEntry || !aInst->isStaticAlloca())
        {
          toTossDynamic.insert(aInst);
        }
        else
        {
          toTossStatic.insert(aInst);
        }
      }
      //Alignment causes problems...
      //This case must go BEFORE CallInst, or else it will never execute!
      //(MemIntrinsics are CallInsts)
      else if (isa<MemIntrinsic>(i)) {
        MemIntrinsic * memintrinsic = dyn_cast<MemIntrinsic>(i);
        //TODO: Maybe alias analysis can help me here.
        memintrinsic->setAlignment(ConstantInt::get(Type::getInt32Ty(i->getContext()), 1, false));
        stats->addMemIntrinsic(memintrinsic);
      }
      //For the case of calls like exit() that do not return.
      else if (isa<CallInst>(i)) {
        CallInst * call = dyn_cast<CallInst>(i);

        if (call->doesNotReturn()) {
          terminatorInsts.insert(call);
        }
      }
      //Invokes are like Calls, except they can unwind the stack in the case of an exception.
      //We need to handle noreturn invokes.
      else if (isa<InvokeInst>(i)) {
        InvokeInst * call = dyn_cast<InvokeInst>(i);

        //Same as DNR for regular calls.
        if (call->doesNotReturn()) {
          terminatorInsts.insert(call);
        }
      }
      //Instructions that end the current function call.
      //We will free all allocas from the entry block before this instruction executes.
      else if (isa<ReturnInst>(i))
      {
        terminatorInsts.insert(dyn_cast<TerminatorInst>(i));
      }
    }
  }


  /**
   * Iterates over every basic block in the module, and tosses stack variables into
   * the heap if needed.
   */
  bool runOnModule(Module & M) {
    //Seed the random number generator if we are randomly tossing.
    srand(RANDOM_TOSS);

    //We could be on a 32-bit or 64-bit machine, which affects the type of size_t, which
    //is needed to construct a function prototype for malloc.
    if (M.getPointerSize() == Module::Pointer64) {
      ptrType = Type::getInt64Ty(M.getContext());
      ptrWidth = 64;
    }
    else {
      ptrType = Type::getInt32Ty(M.getContext());
      ptrWidth = 32;
    }

    stats = new HeapTossStats(M, ptrType, GATHER_STATS);

    Module::FunctionListType & functions = M.getFunctionList();
    Function * mainFunc = NULL;

    for (iplist<Function>::iterator f_iter = functions.begin(); f_iter != functions.end(); f_iter++) {
      Function & f = *f_iter;
      currentFunction = &f;

      //We only want functions with a body.
      if(f.isDeclaration()) {
        continue;
      }

      stats->addFunction(&f);

      if (f.getName().compare("main") == 0) {
        mainFunc = &f;
      }

      for (iplist<BasicBlock>::iterator b_iter = f.begin(); b_iter != f.end(); b_iter++)
      {
        BasicBlock * b = b_iter;
        processBlock(b);
      }

      //Toss all of the variables in toToss.
      if (!TOSS_NONE) tossAll(&f);

      //Clear global state.
      toTossStatic.clear();
      toTossDynamic.clear();
      terminatorInsts.clear();
    }

    stats->insertInitialization(mainFunc);
    stats->outputStats(M);

    delete stats;
    return true;
  }

};

char HeapTossPass::ID = 0;

//Opt uses a switch to toggle plugin loading.
#if RUN_HT_THROUGH_OPT
  static RegisterPass<HeapTossPass> X("heaptoss", "Heap Toss Pass", false, false);
  //Clang just loads it as part of -O1
#else
  /*
   * Register this as a default pass attached to -O1. This is LLVM 3.0 bullshit.
   */
  void addHeapTossPass(const PassManagerBuilder &Builder, PassManagerBase &PM) {
    PM.add(new HeapTossPass());
  }

  RegisterStandardPasses S(PassManagerBuilder::EP_ScalarOptimizerLate,addHeapTossPass);
#endif
