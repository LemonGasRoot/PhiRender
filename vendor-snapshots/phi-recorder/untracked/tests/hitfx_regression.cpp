#include "hitfx.h"
#include <iostream>
#include <stdexcept>
void near(float a,float b,float e){if(std::fabs(a-b)>e)throw std::runtime_error("hitfx formula mismatch");}
int main(){try{
 near(hitfx::width(237),364.98f,1e-4f);
 near(hitfx::particleSize(1920),35.2871287f,1e-4f);
 near(hitfx::particleTravel(0),0,1e-6f);near(hitfx::particleTravel(.5f),.9f,1e-6f);near(hitfx::particleTravel(1),1,1e-6f);
 if(hitfx::frame(0,30)!=0||hitfx::frame(.1f,30)!=6||hitfx::frame(.499f,30)!=29)throw std::runtime_error("atlas timing");
 for(unsigned i=0;i<2000;++i){float r=hitfx::random(185,265,i);if(r<185||r>265||r!=hitfx::random(185,265,i))throw std::runtime_error("particle radius/seek determinism");}
 std::cout<<"PASS: PhiVideo flash size, 0.5s atlas timing, particle size/trajectory, seeded radii\n";
 }catch(const std::exception&e){std::cerr<<e.what();return 1;}}
