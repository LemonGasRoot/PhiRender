#include "chart.h"
#include <iostream>
#include <stdexcept>
void near(float a,float b,float eps,const char* tag){if(std::fabs(a-b)>eps)throw std::runtime_error(std::string(tag)+": "+std::to_string(a)+" != "+std::to_string(b));}
int main(int argc,char**argv){try{
 Chart a,b;std::string err;
 if(!a.loadChart(argv[1],&err)||!b.loadChart(argv[2],&err))throw std::runtime_error(err);
 if(a.lines.size()!=b.lines.size()||a.noteCount!=b.noteCount)throw std::runtime_error("counts");
 float maxFp=0; int checks=0;
 for(size_t i=0;i<a.lines.size();++i){auto& x=a.lines[i];auto& y=b.lines[i];
  const std::vector<Note>* xs[]={&x.notesAbove,&x.notesBelow};
  const std::vector<Note>* ys[]={&y.notesAbove,&y.notesBelow};
  for(int side=0;side<2;++side){
    if(xs[side]->size()!=ys[side]->size())throw std::runtime_error("note side counts");
    for(size_t ni=0;ni<xs[side]->size();++ni){auto& an=(*xs[side])[ni];auto& bn=(*ys[side])[ni];
      if(an.type!=bn.type)throw std::runtime_error("note types");
      near(an.secTime,bn.secTime,0.0001f,"note timestamp");
      near(an.secHoldEndTime,bn.secHoldEndTime,0.0001f,"hold tail timestamp");
      near(an.positionX*(75.f/(1350.f*PGRW)),bn.positionX,0.0001f,"RPE canvas note position");
      if(an.type==3){
        if(!bn.officialHold)throw std::runtime_error("official hold metadata missing");
        near(an.officialHoldSpeed,bn.officialHoldSpeed,1e-5f,"original hold speed");
        size_t ticks=0;
        for(float beat=an.time;beat<an.time+an.holdTime;beat+=16.f){
          if(ticks>=bn.holdEffectTimes.size())throw std::runtime_error("missing hold effect tick");
          near(x.beat2sec(beat,a.offset),bn.holdEffectTimes[ticks++],0.0001f,"half-beat effect timing");
        }
        if(ticks!=bn.holdEffectTimes.size())throw std::runtime_error("extra hold effect tick");
        for(float phase:{-1.f,0.f,0.25f,0.5f,0.75f,1.f,2.f}){
          float at=an.secTime+(an.secHoldEndTime-an.secTime)*phase;
          float expected=std::max(0.f,an.secHoldEndTime-std::max(at,an.secTime))*an.speed*PGRH;
          near(bn.officialHoldBody(at),expected,0.001f,"PhiVideo official body distance");
        }
      }
    }
  }
  for(float t=0.1f;t<150;t+=0.137f){float bt=x.sec2beat(t,a.offset);
   near(x.moveX(bt),y.moveX(t),0.0001f,"line X");near(x.moveY(bt),y.moveY(t),0.0001f,"line Y");
   near(x.rotate(bt),y.rotate(t),0.02f,"rotation");near(x.speedAt(bt),y.speedAt(t),0.001f,"speed");
   maxFp=std::max(maxFp,std::fabs(x.fpAt(bt)-y.fpAt(t))); ++checks;
  }
 }
 std::cout<<"real chart: "<<checks<<" line samples passed; max floor delta="<<maxFp<<"\n";
 near(maxFp,0,0.005f,"floor integral");
 Note durationHold;durationHold.officialHold=true;durationHold.officialHoldSpeed=5;
 durationHold.secTime=10;durationHold.secHoldEndTime=12;
 near(durationHold.officialHoldBody(9),6,1e-6f,"duration body before hit");
 near(durationHold.officialHoldBody(11),3,1e-6f,"duration body while held");
 near(durationHold.officialHoldBody(13),0,1e-6f,"duration body after tail");
 Note ordinaryRpe;if(ordinaryRpe.officialHold)throw std::runtime_error("plain RPE mode changed");
 BlockArea z;z.rpe=true;z.rpeCenterPx=Vec2(0,0);z.rpeHalfPx=Vec2(675,450);
 auto tr=z.transform(0,16.f/9);Vec2 c[4];blockCorners(tr,c);
 near(c[0].x,-1,1e-6f,"noise full width");near(c[0].y,-9.f/16,1e-6f,"noise full height");
 BlockArea::RpeBlockEvent r;r.time=0;r.endTime=1;r.value=0;r.endValue=90;r.transformStart=0;r.transformEnd=90;r.anchorPx=Vec2(100,0);
 z.rpeRot.push_back(r);tr=z.transform(1,16.f/9);
 near(tr.center.x,100*2.f/1350,1e-6f,"anchored rotation X");near(tr.center.y,100*2.f/900/(16.f/9),1e-6f,"anchored rotation Y");
 near(tr.rotation,-90,1e-6f,"noise direction");
 z.rpeActStart.clear();if(z.phase(0)!=BlockArea::Phase::Hidden)throw std::runtime_error("lifetime");
 z.appearTime=0;z.disappearTime=2;if(z.phase(1)!=BlockArea::Phase::Disabled)throw std::runtime_error("empty intervals");
 // Changing the transform reference between anchored events must preserve
 // the prefix adjustment, including explicit transformStart/End metadata.
 z.rpeRot.clear(); r.endValue=0;r.transformEnd=90;z.rpeRot.push_back(r);
 auto n=r;n.time=1;n.endTime=2;n.value=90;n.endValue=180;n.transformStart=90;n.transformEnd=180;
 z.rpeRot.push_back(n);tr=z.transform(1,16.f/9);
 near(tr.center.x,100*2.f/1350,1e-6f,"prefix metadata X");
 near(tr.center.y,100*2.f/900/(16.f/9),1e-6f,"prefix metadata Y");
 JudgeLine ramp;ramp.rpe=true;ramp.rpeSpeed.push_back(ValueSegment{0,2,0,2,1});
 near(ramp.rpeFpAt(1),0.5,1e-6f,"partial speed ramp");near(ramp.rpeFpAt(3),4,1e-6f,"speed ramp tail");
 std::cout<<"noise dimensions, projection, anchor, rotation, empty intervals passed\n";
 return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}

