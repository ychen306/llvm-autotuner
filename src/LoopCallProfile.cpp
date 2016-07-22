//===- llvmtuner/src/LoopCallProfile.cpp: create extraction policy-*- C++ -*-=//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the following classes:
// + LoopHeader: representing a single loop and its enclosing function
// + LoopCallProfile: describing a loop->loop and loop->function call profile
// 
//===----------------------------------------------------------------------===//

#include <map>
#include <set>
#include <fstream>	// std::ofstream
#include <iostream>	// std::cout
#include <sstream>	// std::istringstream
#include <assert.h>

#include "LoopCallProfile.h"
#include "LoopName.h"

//===----------------------------------------------------------------------===//
// Command line flag to control debugging info for profiles
//===----------------------------------------------------------------------===//

cl::opt<ProfileDebugOptions>
ProfileDebugLevel("prof-debug",
                  cl::desc("Set debugging level for profiling"),
                  cl::values(
			     clEnumVal(NoDebug,  "disable debug information"),
			     clEnumVal(Meta,     "print profile metadata"),
			     clEnumVal(Pretty,   "pretty-print profile data"),
			     clEnumValEnd));

//===----------------------------------------------------------------------===//

void
LoopCallProfile::readGraphNodeMetaData(const std::string& MetaFileName)
{
  std::ifstream Fin(MetaFileName.c_str());
  std::string Line;
  std::vector<LoopHeader>& Nodes = this->CGNodes;

  // skip header
  std::getline(Fin, Line);
  unsigned nodeNum = 0;
  while (std::getline(Fin, Line)) {
    LoopHeader Node;
    std::istringstream Fields(Line);
    std::getline(Fields, Node.ModuleName, ',');
    std::getline(Fields, Node.Function, ',');
    Fields >> Node.HeaderId;
    
    Nodes.emplace_back(Node);
    
    // Record LoopName info for each entry in the file and map func name to idx
    IdToLoopNameMap.emplace(++nodeNum, 
	      new LoopName(Node.ModuleName, Node.Function, Node.HeaderId));
    if (isFunction(Node.HeaderId))
      FuncNameToIdMap[Node.Function] = nodeNum;
  }
}

void
LoopCallProfile::readProfileData(const std::string& ProfileFileName)
{
  std::ifstream In;
  In.open(ProfileFileName, std::ios::binary|std::ios::in);
  EdgeBuf Buf;
  while (Buf.readFrom(In)) {
    if (IdToLoopNameMap.find(Buf.From) == IdToLoopNameMap.end()) {
#ifndef NDEBUG
      std::cerr << "LoopName not found for From-loop " << Buf.From << std::endl;
#endif
      continue;
    }
    if (IdToLoopNameMap.find(Buf.To) == IdToLoopNameMap.end()) {
#ifndef NDEBUG
      std::cerr << "LoopName not found for To-loop " << Buf.To << std::endl;
#endif
      continue;
    }

    getFreq(Buf.From, Buf.To) = Buf.Freq;
    getNested(Buf.From).emplace(Buf.To);

#undef  DEBUG_PROFILE_DATA
#ifdef  DEBUG_PROFILE_DATA
    LoopName& fromLN = IdToLoopNameMap.getLoopNameForId(Buf.From);
    LoopName& toLN = IdToLoopNameMap.getLoopNameForId(Buf.To);
    std::cout << "From("
              << fromLN.getModule() << ", " << fromLN.getFuncName() 
              << " : Loop " << fromLN.getLoopId() << ") " << std::endl 
              << "  To("
              << toLN.getModule() << ", " << toLN.getFuncName() 
              << " : Loop " << toLN.getLoopId() << ") ----> "
              << Buf.Freq
              << std::endl << std::endl;
#endif
  }
  In.close();
}

void
LoopCallProfile::readProfiles()
{
  readGraphNodeMetaData(MetadataFileName);
  readProfileData(ProfileFileName);
}

void LoopCallProfile::prettyPrint(std::ostream& os)
{
  prettyPrintProfiles(os);
}

void LoopCallProfile::prettyPrintProfiles(std::ostream& os)
{
  os << "----------PROFILES----------" << std::endl << std::endl;
  for (auto& nestedIter: nested) {
    std::set<unsigned>& inners = nestedIter.second;
    unsigned loopId = nestedIter.first;
    const LoopName& outerLN = getLoopNameForId(loopId);
    os << (isFunction(outerLN.getLoopId())? "Function " : "Loop ") << outerLN;
    os << ": SELF = " << getFreq(loopId, loopId) << std::endl;

    unsigned i=1;
    for (auto& innerId: inners) {
      const LoopName& innerLN = getLoopNameForId(innerId);
      //assert(innerLN.getLoopId() == innerId && "What BS is this?!");
      os << "    [" << i++ << "] "
	 << (isFunction(innerLN.getLoopId())? "Function " : "Loop ")
	 << innerLN << ": ADDS " << getFreq(loopId, innerId) << std::endl;
    }

    os << std::endl;
  }
}

