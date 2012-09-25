#include <iostream>
#include <map>
#include <fstream>
#include <sstream>
#include <cstdlib>

//Toggles dynamic toss stats. We don't do dyntoss stuff yet, so disable it for now.
#define ENABLE_DYN_TOSS_STATS 0
#define NUM_MEMINTRINSICS 3

using namespace std;

//Arrays
static unsigned * fcnRunCounts;
static size_t * fcnMallocSize;
static unsigned * fcnDynTossCounts;
//Two levels. Basically, we want to know the distribution of how large dynamic tosses are across all
//calls to a function.
static map<unsigned, map<unsigned, size_t> > fcnDynTossSizes;
static map<unsigned, map<unsigned, unsigned> > fcnDynTossMallocCalls;
static unsigned * unfreedMallocs;
static unsigned numFunctions;

//An array maps that record the distribution of sizes for each memintrinsic type.
static map<size_t, unsigned> memIntrinsicSizes[NUM_MEMINTRINSICS];

extern "C" void heaptoss_print_result(void) __attribute__ ((destructor));

bool fexists(const char *filename)
{
  ifstream ifile(filename);
  return ifile;
}

extern "C" void heaptoss_print_result(void) {
  stringstream outputFileName;
  const char* filename;
  unsigned i = 0;
  do {
    outputFileName.str(std::string());
    outputFileName << "htstats_run_" << i++ << ".csv";
    filename = outputFileName.str().c_str();
  } while (fexists(filename));
  i--; //It was incremented one more than needed.
  ofstream outFile;
  outFile.open(filename, ios::out);

  unsigned long long totalMallocCalls = 0;

  //FUNCTIONS THAT RUN AND TOSS
  outFile << "ID,Execution Count,Malloc Size,Dynamic Toss Count,Unfreed Mallocs\n";
  for (unsigned i = 0; i < numFunctions; i++) {
    unsigned fcnId = i;
    unsigned unfreed = unfreedMallocs[fcnId];

    //Ignore functions that don't execute and don't toss.
    if (fcnRunCounts[fcnId] == 0 || fcnMallocSize[fcnId] == 0) continue;

    totalMallocCalls += fcnRunCounts[fcnId];

    //There's actually no malloc calls.
    if (fcnMallocSize[fcnId] == 0 && fcnRunCounts[fcnId] == unfreed)
      unfreed = 0;

    //Print out details.
    outFile << fcnId << "," << fcnRunCounts[fcnId] << "," << fcnMallocSize[fcnId] << "," << fcnDynTossCounts[fcnId] << "," << unfreed << "\n";
  }
  outFile.close();

  outputFileName.str(std::string());
  outputFileName << "htstats_run_" << i << "_no_locals.csv";
  filename = outputFileName.str().c_str();
  outFile.open(filename, ios::out);


  //FUNCTIONS THAT RUN AND DON'T TOSS
  outFile << "ID,Execution Count,Malloc Size,Dynamic Toss Count,Unfreed Mallocs\n";
  for (unsigned i = 0; i < numFunctions; i++) {
    unsigned fcnId = i;
    unsigned unfreed = unfreedMallocs[fcnId];

    //We only want functions that execute and don't toss.
    if (fcnRunCounts[fcnId] == 0 || fcnMallocSize[fcnId] != 0) continue;

    //There's actually no malloc calls.
    if (fcnMallocSize[fcnId] == 0 && fcnRunCounts[fcnId] == unfreed)
      unfreed = 0;

    //Print out details.
    outFile << fcnId << "," << fcnRunCounts[fcnId] << "," << fcnMallocSize[fcnId] << "," << fcnDynTossCounts[fcnId] << "," << unfreed << "\n";
  }

  outFile.close();

  outputFileName.str(std::string());
  outputFileName << "htstats_run_" << i << "_intrinsics.csv";
  filename = outputFileName.str().c_str();
  outFile.open(filename, ios::out);

  //INTRINSIC STATS
  outFile << "IntrinsicId,Size,Count\n";
  for (unsigned i = 0; i < NUM_MEMINTRINSICS; i++) {
    for (map<size_t, unsigned>::iterator j = memIntrinsicSizes[i].begin(); j != memIntrinsicSizes[i].end(); j++) {
      outFile << i << "," << j->first << "," << j->second << "\n";
    }
  }

  outFile.close();

  outputFileName.str(std::string());
  outputFileName << "htstats_run_" << i << "_general_stats.csv";
  filename = outputFileName.str().c_str();
  outFile.open(filename, ios::out);

  //GENERAL STATS
  outFile << "Total calls to malloc/free," << totalMallocCalls << "\n";


#if ENABLE_DYN_TOSS_STATS == 1
  outFile << "\n";
  outFile << "ID,Dynamic Toss Sizes...\n";
  for (map<unsigned, map<unsigned, size_t> >::iterator i = fcnDynTossSizes.begin(); i != fcnDynTossSizes.end(); i++) {
    outFile << i->first;
    map<unsigned, size_t> dynTossSizes;
    for (map<unsigned,size_t>::iterator j = dynTossSizes.begin(); j != dynTossSizes.end(); j++) {
      outFile << "," << j->second;
    }
    outFile << "\n";
  }

  outFile << "\n";
  outFile << "ID,# Malloc Calls...\n";
  for (map<unsigned, map<unsigned, unsigned> >::iterator i = fcnDynTossMallocCalls.begin(); i != fcnDynTossMallocCalls.end(); i++) {
    outFile << i->first;
    map<unsigned, unsigned> mallocCalls;
    for (map<unsigned,unsigned>::iterator j = mallocCalls.begin(); j != mallocCalls.end(); j++) {
      outFile << "," << j->second;
    }
    outFile << "\n";
  }
#endif

  outFile.close();

  //Free variables.
  free(fcnRunCounts);
  free(fcnMallocSize);
  free(fcnDynTossCounts);
  free(unfreedMallocs);
}

extern "C" void heaptoss_memintrinsic_execution(unsigned intrinsicId, size_t size) {
  memIntrinsicSizes[intrinsicId][size]++;
}

extern "C" void heaptoss_dynamic_toss(unsigned fcnId, size_t size) {
  fcnDynTossCounts[fcnId]++;
  unsigned runNum = fcnRunCounts[fcnId];
  fcnDynTossSizes[fcnId][runNum] += size;
  fcnDynTossMallocCalls[fcnId][runNum]++;
}

extern "C" void heaptoss_malloc_size(unsigned fcnId, size_t size) {
  fcnMallocSize[fcnId] = size;
}

extern "C" void heaptoss_fcn_ret(unsigned fcnId) {
  unfreedMallocs[fcnId]--;
}

extern "C" void heaptoss_fcn_run(unsigned fcnId) {
  if (fcnRunCounts[fcnId] == NULL) {
#if ENABLE_DYN_TOSS_STATS == 1
    fcnDynTossSizes[fcnId] = map<unsigned, size_t>();
    fcnDynTossMallocCalls[fcnId] = map<unsigned, unsigned>();
#endif
  }
  fcnRunCounts[fcnId]++;
  unfreedMallocs[fcnId]++;
}

extern "C" void heaptoss_initialize(unsigned totalNumFunctions) {
  /*static map<unsigned, map<unsigned, size_t> > fcnDynTossSizes;
static map<unsigned, map<unsigned, unsigned> > fcnDynTossMallocCalls;
   */
  for (unsigned i = 0; i < NUM_MEMINTRINSICS; i++) {
    memIntrinsicSizes[i] = map<size_t, unsigned>();
  }

  //Initialize everything to 0 and allocate the memory.
  fcnRunCounts = (unsigned*) calloc(totalNumFunctions, sizeof(unsigned));
  fcnMallocSize = (size_t*) calloc(totalNumFunctions, sizeof(size_t));
  fcnDynTossCounts = (unsigned*) calloc(totalNumFunctions, sizeof(unsigned));
  unfreedMallocs = (unsigned*) calloc(totalNumFunctions, sizeof(unsigned));
  numFunctions = totalNumFunctions;
}
