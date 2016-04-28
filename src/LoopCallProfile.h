#ifndef _LOOP_CALL_RPFOFILE_H
#define _LOOP_CALL_RPFOFILE_H

#include <map>
#include <fstream>

class LoopCallProfile {
  // representing an edge in a "call graph", which is not really
  // a call graph because for every two nodes there can be only
  // one edge
  typedef std::pair<unsigned, unsigned> Edge;

  // mapping an edge to its frequency
  std::map<Edge, unsigned> M;

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
  unsigned &get(unsigned X, unsigned Y) { return M[Edge(X, Y)]; }

  // dump this profile to a file
  void dump(const std::string &OutFilename) { 
    std::ofstream Out;
    Out.open(OutFilename, std::ios::binary|std::ios::out);

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

  // load profile from a file
  void load(const std::string &InFilename) {
    std::ifstream In;
    In.open(InFilename, std::ios::binary|std::ios::in);
    EdgeBuf Buf;
    while (Buf.readFrom(In)) {
      get(Buf.From, Buf.To) = Buf.Freq;
    }
    In.close();
  }
};

#endif
