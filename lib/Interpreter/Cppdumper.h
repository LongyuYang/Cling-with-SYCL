#ifndef CLING_CPPDUMPER_H
#define CLING_CPPDUMPER_H

#include <string>
#include <system_error>
#include <vector>
#include <fstream>
#include <sstream>

namespace cling {
  class Transaction;
} // namespace cling


namespace cling {

  
  class Dumpcode_entry{
    public:
      //decl:0 stmt:1
      unsigned int declstmtflag;
      std::string code;
      Transaction * CurT;
      Dumpcode_entry(unsigned int dsflag,const std::string& input,Transaction * T);
  };

  ///\brief The class is responsible for dump cpp code into dump.cpp
  /// and then to be compiled by syclcompiler
  class Cppdumper {
  private:
    std::vector<Dumpcode_entry> myvector;
    std::ofstream dump_out;
    std::ofstream hppfile;
    std::ofstream spvfile;
    bool extract_decl_flag = false;
    Transaction* CurT;

  public:
    Cppdumper();
    bool set_extract_decl_flag(const bool flag);
    bool set_curt(Transaction* curt);
    bool dump(const std::string& input,Transaction* T,unsigned int declstmtflag,size_t wrap_point);
    bool dump(const std::string& input,Transaction* T,unsigned int declstmtflag);
    bool submit(Dumpcode_entry & de);

  };

} // namespace cling

#endif // CLING__CPPDUMPER_H