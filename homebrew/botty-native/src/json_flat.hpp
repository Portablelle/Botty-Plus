// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include <array>
namespace botty {
// Flat API contracts only. Reject escapes, duplicate keys, nesting, and excess data.
class FlatJSON {
    struct Pair {std::string_view key,value;bool number=false;};
    std::array<Pair,16> fields_{};unsigned count_=0;
public:
    bool parse(std::string_view text) noexcept {
        count_=0;if(text.size()>1024)return false;
        std::size_t p=0;
        const auto ws=[&]{while(p<text.size()&&(text[p]==' '||text[p]=='\r'||text[p]=='\n'||text[p]=='\t'))++p;};
        const auto take=[&](char c){ws();if(p==text.size()||text[p]!=c)return false;++p;return true;};
        const auto string=[&](std::string_view& out){
            if(!take('"'))return false;const auto start=p;
            while(p<text.size()&&text[p]!='"'){if(static_cast<unsigned char>(text[p])<32||text[p]=='\\')return false;++p;}
            if(p==text.size())return false;out=slice(text,start,p-start);++p;return true;
        };
        if(!take('{'))return false;
        if(take('}')){ws();return p==text.size();}
        do {
            if(count_==fields_.size())return false;
            auto& f=fields_[count_];
            if(!string(f.key)||!take(':'))return false;
            for(unsigned i=0;i<count_;++i)if(fields_[i].key==f.key)return false;
            ws();f.number=p<text.size()&&text[p]!='"';
            if(f.number){const auto start=p;while(p<text.size()&&text[p]>='0'&&text[p]<='9')++p;
                if(p==start||p-start>4||(p-start>1&&text[start]=='0'))return false;f.value=slice(text,start,p-start);
            }else if(!string(f.value))return false;
            ++count_;
        }while(take(','));
        if(!take('}'))return false;ws();return p==text.size();
    }
    std::string_view string(std::string_view key) const noexcept {
        for(unsigned i=0;i<count_;++i)if(fields_[i].key==key&&!fields_[i].number)return fields_[i].value;
        return {};
    }
    int number(std::string_view key) const noexcept {
        for(unsigned i=0;i<count_;++i)if(fields_[i].key==key&&fields_[i].number){int n=0;for(char c:fields_[i].value)n=n*10+c-'0';return n;}
        return -1;
    }
};
}
