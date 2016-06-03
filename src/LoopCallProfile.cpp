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

#include "LoopCallProfile.h"
#include "LoopName.h"


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
    IdToLoopNameMap[++nodeNum] = LoopName(Node.ModuleName, 
					  Node.Function, Node.HeaderId);
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
    LoopName& fromLN = IdToLoopNameMap[Buf.From];
    LoopName& toLN = IdToLoopNameMap[Buf.To];
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

