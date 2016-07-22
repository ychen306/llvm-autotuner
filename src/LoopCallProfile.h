//===- llvmtuner/src/LoopCallProfile.h: create extraction policy-*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the following classes:
// + LoopHeader: representing a single loop and its enclosing function
// + LoopCallProfile: describing a loop->loop and loop->function call profile
// 
//===----------------------------------------------------------------------===//

#ifndef _LOOP_CALL_PROFILE_H
#define _LOOP_CALL_PROFILE_H

#include <map>
#include <set>
#include <vector>
#include <fstream>	// std::ofstream
#include <climits>	// UINT_MAX

#include <llvm/Support/CommandLine.h>
using namespace llvm;

#include "LoopName.h"

//===----------------------------------------------------------------------===//
// Profile file names.
// FIXME: Make these configurable.
//===----------------------------------------------------------------------===//

const char* const MetadataFileName = "loop-prof.flat.csv";
const char* const ProfileFileName  = "loop-prof.graph.data";
const char* const ProfileDumpFileName = "loop_prof.out";

//===----------------------------------------------------------------------===//
// Command line flag to control debugging info for profiles
//===----------------------------------------------------------------------===//

enum ProfileDebugOptions { NoDebug=0x0, Meta=0x1, Pretty=0x2 };

extern cl::opt<ProfileDebugOptions> ProfileDebugLevel;

//===----------------------------------------------------------------------===//
// class LoopHeader:
// because basic blocks can be implicitly labelled,
// we will reference them (across program executions) by the
// order of default traversal. i.e. the first block encounter
// in `for (auto &BB : F)` has id 1;
//===----------------------------------------------------------------------===//

// Does this index represent a function in the nested loop profile?
static bool isFunction(unsigned id) { return id == 0; }

struct LoopHeader {
  std::string ModuleName;
  std::string Function;
  unsigned HeaderId;

  LoopHeader(): Function(""), HeaderId(UINT_MAX) {}

  LoopHeader(const std::string& funcName, unsigned id):
    Function(funcName), HeaderId(id) {}

  LoopHeader(const LoopName& loopName):
    Function(loopName.getFuncName()), 
    HeaderId(loopName.getLoopId()) {}

  // Does this index represent a function in the nested loop profile?
  bool isFunction() { return ::isFunction(HeaderId); }

  bool operator==(LoopHeader &other) const {
    return Function == other.Function && HeaderId == other.HeaderId;
  }
};


//===----------------------------------------------------------------------===//
// class LoopCallProfile: 
// Describes a loop->loop and loop->function call profile.
// Uses LoopHeader to "name" loops uniquely within functions.
//===----------------------------------------------------------------------===//

class LoopCallProfile {
  // representing an edge in a "call graph": which shows which functions
  // are called directly or indirectly from which (top-level) loops
  typedef std::pair<unsigned, unsigned> Edge;

  // Record the loops/funcs called by each loop and frequency for each edge
  std::vector<LoopHeader> CGNodes;
  std::map<unsigned, LoopName*> IdToLoopNameMap;
  std::map<std::string, unsigned> FuncNameToIdMap;
  std::map<Edge, unsigned> M;		   // mapping an edge to its frequency
  std::map<unsigned, std::set<unsigned>> nested;	// inner loops & funcs

  // Helper functions to read the two policy files
  void readGraphNodeMetaData(const std::string& MetaFileName);
  void readProfileData(const std::string& ProfileFileName);

  // helper struct used for serialization
  struct EdgeBuf {
    unsigned From, To, Freq;

    char *getaddr() {
      return (char *)this;
    }

    // write buffer to `Out`
    void writeTo(std::ostream &Out) {
      Out.write(getaddr(), sizeof(EdgeBuf));
    }

    // read from `In`, return if success
    bool readFrom(std::istream &In) {
      In.read(getaddr(), sizeof(EdgeBuf));
      return !In.fail();
    }
  };

public:
  // Get the metadata describing the nodes of the profiled "call graph"
  const std::vector<LoopHeader>& GraphNodeMeta() const { return CGNodes; }
  
  // Get the frequency for an edge from node X to node Y
  unsigned& getFreq(unsigned X, unsigned Y) { return M[Edge(X, Y)]; }
  
  // Get the loops and functions called from node X
  std::set<unsigned>& getNested(unsigned X) { return nested[X]; }

  // Does this index represent a function in the nested loop profile?
  bool isFunction(unsigned idx) { return idx == 0; }

  // Access the maps storing id <-> func/loop information
  const LoopName& getLoopNameForId(unsigned X) { return *IdToLoopNameMap[X]; }
  unsigned getFuncIdForFuncName(const std::string& funcName)
					{ return FuncNameToIdMap[funcName]; }

  // begin(), end() member function used for range-based enumeration
  unsigned begin() { return nested.begin()->first; }
  unsigned end()   { return nested.end()->first; }

  // dump this profile to a file
  void dump(const std::string &OutFileName) { 
    std::ofstream Out;
    Out.open(OutFileName, std::ios::binary|std::ios::out);

    EdgeBuf Buf;
    for (const auto &Pair : M) {
      const auto &Edge = Pair.first;
      const auto &Freq = Pair.second; 
      Buf.From = Edge.first;
      Buf.To = Edge.second;
      Buf.Freq = Freq;
      Buf.writeTo(Out);
    }

    Out.close();
  }

  // Read metadata and profiles for loops and functions from policy files.
  void readProfiles();

  // Formatted printout of metadata and profiles
  void prettyPrint(std::ostream& os);
  void prettyPrintProfiles(std::ostream& os);
};

#endif // ifndef _LOOP_CALL_RPFOFILE_H
