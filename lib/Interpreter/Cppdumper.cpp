#include "Cppdumper.h"
#include "cling/Interpreter/Transaction.h"

namespace cling {
    Dumpcode_entry::Dumpcode_entry(unsigned int dsflag,const std::string& input,Transaction * T){
        declstmtflag = dsflag;
        code.assign(input);
        CurT = T;
    }
    Cppdumper::Cppdumper(){
        dump_out.open("dump.cpp");
        dump_out << "int main(){\n" 
               << '}';
        dump_out.close();
        hppfile.open("st.h", std::ios::trunc);
        hppfile.close();
        spvfile.open("mk.spv", std::ios::trunc);
        spvfile.close();
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
        myvector.push_back(Dumpcode_entry(declstmtflag,input,T));
        return submit(*myvector.rbegin());
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
}