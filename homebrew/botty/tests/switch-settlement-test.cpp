#include "switch-source-settlement.hpp"
#include <cassert>
int main(){using namespace botty;using namespace std::chrono;SwitchSourceSettlement gate;auto now=SwitchSourceSettlement::Clock::time_point{};json old={{"size",10},{"mtime",1}},changed={{"size",10},{"mtime",2}};assert(!gate.observe("torrent",old,now));assert(!gate.observe("torrent",old,now+seconds(1)));assert(!gate.observe("torrent",changed,now+seconds(2)));assert(!gate.observe("torrent",changed,now+seconds(3)));assert(gate.observe("torrent",changed,now+seconds(4)));assert(!gate.observe("torrent",old,now+seconds(5)));assert(!gate.observe("another",old,now+seconds(50)));
 SwitchSourceSettlement queue;
 for(int i=0;i<128;++i)assert(!queue.observe(std::to_string(i),old,now));
 for(int i=0;i<64;++i)assert(queue.observe(std::to_string(i),old,now+seconds(3)));
 for(int i=64;i<128;++i)assert(!queue.observe(std::to_string(i),old,now+seconds(3)));
 for(int i=64;i<128;++i)assert(queue.observe(std::to_string(i),old,now+seconds(6)));
 for(int i=0;i<64;++i)assert(!queue.observe(std::to_string(i),old,now+seconds(10)));
 assert(!queue.observe("new after abandoned observations",old,now+minutes(6)));
 assert(queue.observe("new after abandoned observations",old,now+minutes(6)+seconds(3)));
}
