#include "HandInput/Recognition/multi_hand_gesture_core.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace recognition = ryoiki::hand_input::recognition;
namespace measurements = ryoiki::hand_input::measurements;
namespace
{
void require(bool value, const char* message) { if (!value) throw std::runtime_error{message}; }
measurements::UnifiedFeatureObservation hand(std::uint64_t timestamp, float centerX, float handedness)
{
    measurements::UnifiedFeatureObservation value{}; value.timestampUs=timestamp; value.handedness=handedness;
    for(std::size_t i=0;i<value.imageLandmarks.size();++i) value.imageLandmarks[i]={centerX+static_cast<float>(i%4)*3.F,static_cast<float>(i/4)*3.F,0};
    value.imageLandmarks[0]={centerX,0,0}; value.imageLandmarks[9]={centerX+10,0,0}; return value;
}
std::vector<recognition::TwoHandFrameSetObservation> sequence(bool moving=true)
{
    std::vector<recognition::TwoHandFrameSetObservation> values;
    for(std::size_t i=0;i<40;++i){const auto ts=static_cast<std::uint64_t>(i)*50'000;recognition::TwoHandFrameSetObservation f{};f.timestampUs=ts;f.handCount=2;f.confidences={.9F,.8F};f.hands[0]=hand(ts,0,.9F);f.hands[1]=hand(ts,20+(moving?static_cast<float>(i):0),.1F);values.push_back(f);}return values;
}
void testCoverageOrderingAndCreation(){auto values=sequence();require(recognition::countUsableTwoHandFrames(values)==40,"usable pair count drifted");require(recognition::hasPredominantTwoHandCoverage(values,40),"coverage gate drifted");auto created=recognition::createTwoHandTemplate(values);require(created.value.valid,"moving pair was rejected");require(created.value.summary.meanRelativeDistance>=recognition::kTwoHandMinimumRelativeDistance,"relative distance normalization drifted");}
void testInputOrderDoesNotChangeHandOrder(){auto values=sequence();auto swapped=values;for(std::size_t i=0;i<swapped.size();i+=2){std::swap(swapped[i].hands[0],swapped[i].hands[1]);std::swap(swapped[i].confidences[0],swapped[i].confidences[1]);}const auto a=recognition::extractTwoHandSequence(values),b=recognition::extractTwoHandSequence(swapped);require(a.frames.size()==b.frames.size(),"input order changed usable count");for(std::size_t i=0;i<a.frames.size();++i)require(std::abs(a.frames[i].values[0]-b.frames[i].values[0])<.0001F,"handedness ordering changed with detector order");}
void testStableRejectedAndMatch(){auto stable=recognition::createTwoHandTemplate(sequence(false));require(!stable.value.valid,"stable pair was accepted");auto moving=recognition::createTwoHandTemplate(sequence());auto match=recognition::compareTwoHand(moving.value,moving.value);if(!match.eligible)std::cerr<<"score="<<match.score<<" confidence="<<match.confidence<<" reason="<<match.reason<<'\n';require(match.eligible&&match.confidence>.99F,"identical two-hand template did not match");}
void testBufferBounds(){recognition::TwoHandFrameSetBuffer b;for(std::uint64_t i=0;i<80;++i){recognition::TwoHandFrameSetObservation f{};f.timestampUs=i*50'000;b.push(f);}require(b.size()<=53,"2600 ms buffer was not bounded");require(b.window(3'950'000,1'400'000).size()<=29,"duration slice was not bounded");}
}
int main(){try{testCoverageOrderingAndCreation();testInputOrderDoesNotChangeHandOrder();testStableRejectedAndMatch();testBufferBounds();std::cout<<"Multi-hand gesture core tests passed.\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
