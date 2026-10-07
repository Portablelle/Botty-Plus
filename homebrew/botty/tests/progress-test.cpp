#include "progress.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace botty;
int main(){
 const auto start=ExtractionEstimate::Clock::time_point{};
 const auto at=[&](int seconds){return start+std::chrono::seconds(seconds);};
 ExtractionEstimate estimate;
 estimate.update(at(0),50000,200000);
 estimate.update(at(1),51000,200000);assert(estimate.eta==-1);
 estimate.update(at(5),55000,200000);assert(estimate.eta==145);
 for(int second=6;second<=60;++second)estimate.update(at(second),50000+second*1000,200000);
 assert(estimate.eta==90);
 estimate.update(at(61),115000,200000);
 assert(std::abs(estimate.eta-85000.0*60/64000)<0.001);
 for(int second=62;second<=121;++second)estimate.update(at(second),115000+(second-61)*100,200000);
 assert(std::abs(estimate.eta-790)<0.001);
 estimate.update(at(136),121000,200000);assert(estimate.eta==-1);
 estimate.update(at(137),121100,200000);assert(estimate.eta>790);
 estimate.update(at(138),10000,200000);assert(estimate.eta==-1&&estimate.rate==0);
 estimate.update(at(143),15000,200000);assert(estimate.eta==185);
 estimate.update(at(144),15000,300000);assert(estimate.eta==-1);
 estimate.update(at(149),20000,300000);assert(estimate.eta==280);
 estimate.update(at(150),300000,300000);assert(estimate.eta==0);
 ExtractionEstimate complete;complete.update(at(0),100,100);assert(complete.eta==0);
 ExtractionEstimate irregular;irregular.update(at(0),0,1000000);
 irregular.update(at(17),17000,1000000);irregular.update(at(42),42000,1000000);
 irregular.update(at(75),75000,1000000);assert(irregular.eta==925);
 irregular.update(at(75),75000,1000000);assert(irregular.eta==925);
 irregular.update(at(90),75000,1000000);assert(irregular.eta==-1);
 ExtractionEstimate idle;idle.update(at(0),0,0);idle.update(at(30),0,0);assert(idle.eta==-1);
 std::cout<<"Progress ETA tests passed\n";
}
