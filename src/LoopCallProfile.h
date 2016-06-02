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
#include <fstream>
#include <climits>
#include <iostream>	// std::cout
#include <sstream>	// std::istringstream

#include "LoopName.h"

const std::string MetadataFileName = "loop-prof.flat.csv";
const std::string ProfileFileName  = "loop-prof.graph.data";

//===----------------------------------------------------------------------===//
// class LoopHeader:
// because basic blocks can be implicitly labelled,
// we will reference them (across program executions) by the
// order of default traversal. i.e. the first block encounter
// in `for (auto &BB : F)` has id 1;
//===----------------------------------------------------------------------===//

struct LoopHeader {
  std::string Function;
  unsigned HeaderId;

  LoopHeader(): Function(""), HeaderId(UINT_MAX) {}

  LoopHeader(const std::string& funcName, unsigned id):
    Function(funcName), HeaderId(id) {}

  LoopHeader(const LoopName& loopName):
    Function(loopName.getFuncName()), 
    HeaderId(loopName.getLoopId()) {}

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

  // Record the frequency for each edge and called loops/funcs for each loop
  std::vector<LoopHeader> CGNodes;
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
  std::vector<LoopHeader> GraphNodeMeta() { return CGNodes; }
  
  // Get the frequency for an edge from node X to node Y
  unsigned& getFreq(unsigned X, unsigned Y) { return M[Edge(X, Y)]; }
  
  // Get the loops and functions called from node X
  std::set<unsigned>& getNested(unsigned X) { return nested[X]; }

  // Does this index represent a function in the nested loop profile?
  bool isFunction(unsigned idx) { return idx == 0; }

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
};

void
LoopCallProfile::readGraphNodeMetaData(const std::string& MetaFileName)
{
  std::ifstream Fin(MetaFileName.c_str());
  std::string Line;
  std::vector<LoopHeader>& Nodes = this->CGNodes;

  // skip header
  std::getline(Fin, Line);
  while (std::getline(Fin, Line)) {
    LoopHeader Node;
    std::istringstream Fields(Line);
    std::getline(Fields, Node.Function, ',');
    Fields >> Node.HeaderId;

    Nodes.emplace_back(Node);
  }
}

void
LoopCallProfile::readProfileData(const std::string& ProfileFileName)
{
  std::ifstream In;
  In.open(ProfileFileName, std::ios::binary|std::ios::in);
  EdgeBuf Buf;
  while (Buf.readFrom(In)) {
    getFreq(Buf.From, Buf.To) = Buf.Freq;
    getNested(Buf.From).insert(Buf.To);
  }
  In.close();
}

void
LoopCallProfile::readProfiles()
{
  readGraphNodeMetaData(MetadataFileName);
  readProfileData(ProfileFileName);
}

#endif // ifndef _LOOP_CALL_RPFOFILE_H
