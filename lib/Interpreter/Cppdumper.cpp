#include "Cppdumper.h"

#include "cling/Interpreter/Transaction.h"
#include "cling/Interpreter/Interpreter.h"
#include "cling/MetaProcessor/InputValidator.h"
#include "cling/Utils/SourceNormalization.h"

#include "clang/Frontend/CompilerInstance.h"

namespace cling {
    Dumpcode_entry::Dumpcode_entry(unsigned int dsflag,const std::string& input,Transaction * T){
        declstmtflag = dsflag;
        code.assign(input);
        CurT = T;
    }
    Cppdumper::Cppdumper(Interpreter* interp) : m_Interpreter(interp) {
        dump_out.open("dump.cpp");
        dump_out << "int main(){\n" 
               << '}';
        dump_out.close();
        m_InputValidator.reset(new InputValidator());
    }
    Cppdumper::~Cppdumper(){ 
        delete HeadTransaction;
    }
    bool Cppdumper::set_curt(Transaction* curt){
        CurT = curt;
        return true;
    }
    bool Cppdumper::set_extract_decl_flag(const bool flag){
        extract_decl_flag = flag;
        return true;
    }
    bool Cppdumper::dump(const std::string& input,Transaction* T,unsigned int declstmtflag){
        return dump(input,T,declstmtflag,std::string::npos);
    }
    bool Cppdumper::dump(const std::string& input,Transaction* T,unsigned int declstmtflag,size_t wrap_point){
        if((declstmtflag == 0)&&(!extract_decl_flag))
            return true;      
        if(declstmtflag == 0){
            myvector.push_back(Dumpcode_entry(declstmtflag,input,T));
            return submit(*myvector.rbegin());
        }
        std::istringstream input_holder(input);
        std::string line; 
        std::string complete_input;
        while(getline(input_holder,line)){
            if (line.empty() || (line.size() == 1 && line.front() == '\n')) {
                continue;
            }
            if (m_InputValidator->validate(line) == InputValidator::kIncomplete) {
                continue;
            }
            complete_input.clear();
            m_InputValidator->reset(&complete_input);
            size_t wrapPoint = std::string::npos;
            wrapPoint = utils::getWrapPoint(complete_input, m_Interpreter->getCI()->getLangOpts());
            if(wrapPoint == std::string::npos)
                myvector.push_back(Dumpcode_entry(0,complete_input,T));
            else
                myvector.push_back(Dumpcode_entry(1,complete_input,T));
            submit(*myvector.rbegin());
        }
        return true;
    }
    bool Cppdumper::submit(Dumpcode_entry & de){
        std::string input(de.code);
        unsigned declstmtflag = de.declstmtflag;
        extract_decl_flag = false;
        if(declstmtflag == 0){
            std::ifstream dump_in;
            dump_in.open("dump.cpp");
            std::ostringstream tmp;
            tmp << dump_in.rdbuf();
            std::string head = tmp.str();
            std::string::size_type position;
            position = head.find("int main()");
            if(position != std::string::npos){
                std::string m_insert(input);
                m_insert.append("\n");
                head.insert(position,m_insert);
                dump_out.open("dump.cpp", std::ios::in | std::ios::out);
                dump_out.seekp(0,std::ios::beg);
                dump_out<< head;
                dump_out.close();
            }else{
                assert(0);
            }
            dump_in.close();
        }else{
            dump_out.open("dump.cpp", std::ios::in | std::ios::out | std::ios::ate);
            dump_out.seekp(-1, std::ios::end);
            for(auto sit = input.rbegin();sit!=input.rend();sit++){
                if(*sit!=' '){
                    if(*sit == ';')
                        dump_out<< input<< '\n'<< '}';
                    else
                        dump_out<< input<<';'<< '\n'<< '}';
                    break;
                }
            }
            dump_out.close();
        }
        return true;
    }

    bool Cppdumper::compile(const std::string& input){
        std::string _hsinput(input);
        bool head_spv_flg = utils::generate_hppandspv(_hsinput,m_Interpreter->getCI()->getLangOpts());
        if (head_spv_flg) {
            int sysReturn = std::system("clang++ --sycl -fno-sycl-use-bitcode -Xclang -fsycl-int-header=st.h -c dump.cpp -o mk.spv");
            if (sysReturn != 0)
                return false;
            if (HeadTransaction)
                m_Interpreter->unload(HeadTransaction[0][0]);
            if (!HeadTransaction)
                HeadTransaction = new Transaction *;
            if (m_Interpreter->loadHeader("st.h", HeadTransaction) != Interpreter::kSuccess) {
                std::cout << "=======> error: fail to load SYCL kernel head file" << std::endl;
                return false;
            }
        }
        return true;
    }
}