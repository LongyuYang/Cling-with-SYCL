#ifndef CLING_CPPDUMPER_H
#define CLING_CPPDUMPER_H

#include <string>
#include <system_error>
#include <list>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>

namespace cling {
  class Transaction;
  class InputValidator;
  class Interpreter;
} // namespace cling


namespace cling {

  
  class Dumpcode_entry{
    public:
      //decl:0 stmt:1
      unsigned int declstmtflag;
      std::string code;
      Transaction * CurT;
      bool declSuccess;
      Dumpcode_entry(unsigned int dsflag,const std::string& input,Transaction * T, bool declSuc=false);
  };

  ///\brief The class is responsible for dump cpp code into dump.cpp
  /// and then to be compiled by syclcompiler
  class Cppdumper {
  private:
    Interpreter* m_Interpreter;
    std::list<Dumpcode_entry> myvector;
    std::ofstream dump_out;
    bool extract_decl_flag = false;
    Transaction* CurT;
    std::unique_ptr<InputValidator> m_InputValidator;
    ///\brief Transaction of the SYCL kernel head file
    ///
    Transaction** HeadTransaction = 0;

  public:
    Cppdumper(Interpreter* interp);
    ~Cppdumper();
    bool set_extract_decl_flag(const bool flag);
    bool set_curt(Transaction* curt);
    bool dump(const std::string& input,Transaction* T,unsigned int declstmtflag,size_t wrap_point, bool declSuccess=false);
    bool dump(const std::string& input,Transaction* T,unsigned int declstmtflag, bool declSuccess=false);
    bool submit();
    bool compile(const std::string& input);
    void setTransaction(Transaction* T);
    void removeCodeByTransaction(Transaction* T);
  };

} // namespace cling

#endif // CLING__CPPDUMPER_H