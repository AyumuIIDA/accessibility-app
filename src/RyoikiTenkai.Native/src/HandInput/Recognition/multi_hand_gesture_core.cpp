#include "HandInput/Recognition/multi_hand_gesture_core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ryoiki::hand_input::recognition
{
namespace
{
constexpr std::uint64_t kWindowDurationUs = 2'600'000;
constexpr std::size_t kMinimumUsableFrames = 32;
constexpr double kMinimumDurationMs = 1900.0;
constexpr double kMinimumFps = 16.0;
constexpr float kMinimumTopology = 0.32F;
constexpr float kConfidenceThreshold = 0.80F;
constexpr float kReverseMargin = 0.94F;

struct OrderedPair { measurements::UnifiedFeatureObservation low; measurements::UnifiedFeatureObservation high; };

float distance(const measurements::UnifiedFeatureObservation& hand)
{
    const auto& a = hand.imageLandmarks[0]; const auto& b = hand.imageLandmarks[9];
    return std::hypot(a.x - b.x, a.y - b.y);
}
std::array<float, 2> center(const measurements::UnifiedFeatureObservation& hand)
{
    return {(hand.imageLandmarks[0].x + hand.imageLandmarks[9].x) * .5F,
        (hand.imageLandmarks[0].y + hand.imageLandmarks[9].y) * .5F};
}
bool orderedPair(const TwoHandFrameSetObservation& frame, OrderedPair& out)
{
    if (frame.handCount < 2 || frame.confidences[0] < kTwoHandMinimumConfidence
        || frame.confidences[1] < kTwoHandMinimumConfidence) return false;
    std::size_t low = 0, high = 1;
    if (std::abs(frame.hands[0].handedness - frame.hands[1].handedness) >= .20F)
    {
        if (frame.hands[0].handedness > frame.hands[1].handedness) std::swap(low, high);
    }
    else if (center(frame.hands[0])[0] > center(frame.hands[1])[0]) std::swap(low, high);
    const auto lc = center(frame.hands[low]); const auto hc = center(frame.hands[high]);
    const float scale = (std::max)(1.0F, (distance(frame.hands[low]) + distance(frame.hands[high])) * .5F);
    if (std::hypot(hc[0] - lc[0], hc[1] - lc[1]) / scale < kTwoHandMinimumRelativeDistance) return false;
    out = {frame.hands[low], frame.hands[high]}; return true;
}

std::vector<float> unwrap(const std::vector<float>& angles)
{
    if (angles.empty()) return {};
    std::vector<float> result{angles[0]}; float previous = angles[0], offset = 0;
    for (std::size_t i=1;i<angles.size();++i) { const float delta=angles[i]-previous;
        if(delta>std::numbers::pi_v<float>) offset-=2*std::numbers::pi_v<float>;
        else if(delta<-std::numbers::pi_v<float>) offset+=2*std::numbers::pi_v<float>;
        result.push_back(angles[i]+offset); previous=angles[i]; }
    return result;
}

TwoHandFeatureFrame interpolate(const TwoHandFeatureFrame&a,const TwoHandFeatureFrame&b,float t,double time)
{ TwoHandFeatureFrame r{}; r.timeOffsetMs=time; r.centerX=std::lerp(a.centerX,b.centerX,t); r.centerY=std::lerp(a.centerY,b.centerY,t);
 r.relativeAngleRadians=std::lerp(a.relativeAngleRadians,b.relativeAngleRadians,t); r.palmVelocity=std::lerp(a.palmVelocity,b.palmVelocity,t);
 for(std::size_t i=0;i<r.values.size();++i)r.values[i]=std::lerp(a.values[i],b.values[i],t); return r; }

std::array<TwoHandFeatureFrame,measurements::kUnifiedSequenceLength> resample(const std::vector<TwoHandFeatureFrame>& f)
{ std::array<TwoHandFeatureFrame,measurements::kUnifiedSequenceLength> out{}; if(f.empty())return out;
 const double duration=(std::max)(1.0,f.back().timeOffsetMs); std::size_t right=1;
 for(std::size_t i=0;i<out.size();++i){const double target=duration*i/(out.size()-1); while(right<f.size()&&f[right].timeOffsetMs<target)++right;
 if(right>=f.size()){out[i]=f.back();out[i].timeOffsetMs=target;continue;} const auto left=right-1; const double span=(std::max)(1.0,f[right].timeOffsetMs-f[left].timeOffsetMs);
 out[i]=interpolate(f[left],f[right],static_cast<float>((target-f[left].timeOffsetMs)/span),target);} return out; }

struct Dtw { float score; std::size_t path; };
Dtw dtw(const auto& a,const auto& b,bool reverse)
{ constexpr std::size_t n=measurements::kUnifiedSequenceLength; const std::size_t width=n+1; std::array<float,(n+1)*(n+1)> costs; costs.fill(std::numeric_limits<float>::infinity()); costs[0]=0;
 const std::size_t radius=(std::max)(std::size_t{5},n/4); for(std::size_t i=1;i<=n;++i)for(std::size_t j=i>radius?i-radius:1;j<=(std::min)(n,i+radius);++j){const auto&x=a[i-1];const auto&y=b[reverse?n-j:j-1];float d=0;for(std::size_t k=0;k<kTwoHandFeatureLength;++k)d+=std::abs(x.values[k]-y.values[k]);d/=kTwoHandFeatureLength; costs[i*width+j]=d+(std::min)({costs[(i-1)*width+j],costs[i*width+j-1],costs[(i-1)*width+j-1]});}
 std::size_t i=n,j=n,path=0;while(i&&j){++path;const float diag=costs[(i-1)*width+j-1],up=costs[(i-1)*width+j],left=costs[i*width+j-1];if(diag<=up&&diag<=left){--i;--j;}else if(up<=left)--i;else --j;}return{costs[n*width+n]/(std::max)(std::size_t{1},path),path}; }
}

void TwoHandFrameSetBuffer::push(TwoHandFrameSetObservation observation){observations_.push_back(std::move(observation));const auto newest=observations_.back().timestampUs;while(!observations_.empty()&&newest>observations_.front().timestampUs&&newest-observations_.front().timestampUs>kWindowDurationUs)observations_.pop_front();}
void TwoHandFrameSetBuffer::reset() noexcept{observations_.clear();}
std::vector<TwoHandFrameSetObservation> TwoHandFrameSetBuffer::window(std::uint64_t end,std::uint64_t duration)const{std::vector<TwoHandFrameSetObservation> r;const auto start=end>duration?end-duration:0;for(const auto&x:observations_)if(x.timestampUs>=start&&x.timestampUs<=end)r.push_back(x);return r;}
std::size_t countUsableTwoHandFrames(const std::vector<TwoHandFrameSetObservation>& s) noexcept{std::size_t n=0;OrderedPair p{};for(const auto&x:s)n+=orderedPair(x,p)?1:0;return n;}
bool hasPredominantTwoHandCoverage(const std::vector<TwoHandFrameSetObservation>&s,std::size_t captured) noexcept{const auto n=countUsableTwoHandFrames(s);return n>=kMinimumUsableFrames&&static_cast<float>(n)/(std::max)(std::size_t{1},(std::max)(captured,s.size()))>=kTwoHandMinimumCoverageRatio;}

TwoHandSequence extractTwoHandSequence(const std::vector<TwoHandFrameSetObservation>& source)
{ std::vector<OrderedPair> pairs;for(const auto&x:source){OrderedPair p{};if(orderedPair(x,p))pairs.push_back(p);} TwoHandSequence result{};if(pairs.empty())return result;
 std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.low.timestampUs < b.low.timestampUs; });
 std::vector<measurements::UnifiedFeatureObservation> lows,highs;for(auto&p:pairs){lows.push_back(p.low);highs.push_back(p.high);}const auto lf=measurements::buildFeatureSequence(lows),hf=measurements::buildFeatureSequence(highs);const auto count=(std::min)(lf.size(),hf.size());const auto firstL=center(pairs[0].low),firstH=center(pairs[0].high);const float firstDistance=(std::max)(.001F,std::hypot(firstH[0]-firstL[0],firstH[1]-firstL[1]));std::vector<float> ratios,angles,relativeDistances;
 for(std::size_t i=0;i<count;++i){const auto lc=center(pairs[i].low),hc=center(pairs[i].high);const float scale=(std::max)(1.F,(distance(pairs[i].low)+distance(pairs[i].high))*.5F);const float rx=(hc[0]-lc[0])/scale,ry=(hc[1]-lc[1])/scale,ratio=std::hypot(hc[0]-lc[0],hc[1]-lc[1])/firstDistance;TwoHandFeatureFrame f{};f.timeOffsetMs=lf[i].timeOffsetMs;f.centerX=(lf[i].centerX+hf[i].centerX)*.5F;f.centerY=(lf[i].centerY+hf[i].centerY)*.5F;f.relativeAngleRadians=std::atan2(ry,rx);f.palmVelocity=(std::max)(lf[i].palmVelocity,hf[i].palmVelocity);std::copy(lf[i].values.begin(),lf[i].values.end(),f.values.begin());std::copy(hf[i].values.begin(),hf[i].values.end(),f.values.begin()+measurements::kUnifiedFeatureVectorLength);auto o=measurements::kUnifiedFeatureVectorLength*2;f.values[o++]=rx;f.values[o++]=ry;f.values[o++]=ratio;f.values[o]=f.relativeAngleRadians;result.frames.push_back(f);ratios.push_back(ratio);angles.push_back(f.relativeAngleRadians);relativeDistances.push_back(std::hypot(rx,ry));}
 if(result.frames.empty())return result;const auto ua=unwrap(angles);const auto&first=result.frames.front();const auto&last=result.frames.back();const auto off=measurements::kUnifiedFeatureVectorLength*2;result.summary.relativeTranslationDistance=std::hypot(last.values[off]-first.values[off],last.values[off+1]-first.values[off+1]);result.summary.relativeDistanceRange=*std::max_element(ratios.begin(),ratios.end())-*std::min_element(ratios.begin(),ratios.end());result.summary.relativeDistanceDelta=ratios.back()-ratios.front();result.summary.relativeAngleRangeRadians=*std::max_element(ua.begin(),ua.end())-*std::min_element(ua.begin(),ua.end());for(const auto&f:result.frames){result.summary.lowHandednessMean+=f.values[measurements::kHandednessOffset];result.summary.highHandednessMean+=f.values[measurements::kUnifiedFeatureVectorLength+measurements::kHandednessOffset];}result.summary.lowHandednessMean/=result.frames.size();result.summary.highHandednessMean/=result.frames.size();result.summary.topologyChangeScore=result.summary.relativeTranslationDistance+result.summary.relativeDistanceRange*.75F+std::abs(result.summary.relativeDistanceDelta)*.55F+result.summary.relativeAngleRangeRadians*.35F;for(float v:relativeDistances)result.summary.meanRelativeDistance+=v;result.summary.meanRelativeDistance/=relativeDistances.size();float path=0,velocitySum=0;for(std::size_t i=1;i<result.frames.size();++i){const float d=std::hypot(result.frames[i].centerX-result.frames[i-1].centerX,result.frames[i].centerY-result.frames[i-1].centerY);path+=d;const float dt=(std::max)(.001F,static_cast<float>((result.frames[i].timeOffsetMs-result.frames[i-1].timeOffsetMs)/1000));const float v=d/dt;velocitySum+=v;result.peakVelocity=(std::max)(result.peakVelocity,v);}const float seconds=(std::max)(.001F,static_cast<float>(result.frames.back().timeOffsetMs/1000));result.averageVelocity=result.frames.size()<2?0:velocitySum/(result.frames.size()-1);result.motionScore=(std::max)(path/seconds,result.averageVelocity)+result.summary.relativeDistanceRange;return result; }

TwoHandTemplateCreationResult createTwoHandTemplate(const std::vector<TwoHandFrameSetObservation>& source){TwoHandTemplateCreationResult r{};const auto usable=countUsableTwoHandFrames(source);auto ordered=source;std::sort(ordered.begin(),ordered.end(),[](const auto&a,const auto&b){return a.timestampUs<b.timestampUs;});const double duration=ordered.size()<2?0:(ordered.back().timestampUs-ordered.front().timestampUs)/1000.0;const double fps=duration<=0?0:usable*1000.0/duration;if(usable<kMinimumUsableFrames||duration<kMinimumDurationMs||fps<kMinimumFps){r.rejectionReason="Need two visible hands for the whole capture window.";return r;}auto seq=extractTwoHandSequence(ordered);if(seq.summary.topologyChangeScore<kMinimumTopology){r.rejectionReason="Captured stable two-hand pose, not a two-hand gesture.";return r;}r.value.frames=resample(seq.frames);r.value.summary=seq.summary;r.value.sourceFrameCount=usable;r.value.durationMs=duration;r.value.valid=true;return r;}
TwoHandTemplateCreationResult createTwoHandCandidate(const std::vector<TwoHandFrameSetObservation>& source){TwoHandTemplateCreationResult r{};const auto usable=countUsableTwoHandFrames(source);auto ordered=source;std::sort(ordered.begin(),ordered.end(),[](const auto&a,const auto&b){return a.timestampUs<b.timestampUs;});const double duration=ordered.size()<2?0:(ordered.back().timestampUs-ordered.front().timestampUs)/1000.0;if(usable<25||duration<1400.0){r.rejectionReason="Waiting for usable two-hand rolling window.";return r;}auto seq=extractTwoHandSequence(ordered);if(seq.frames.size()<3){r.rejectionReason="Too few two-hand feature frames.";return r;}r.value.frames=resample(seq.frames);r.value.summary=seq.summary;r.value.sourceFrameCount=usable;r.value.durationMs=duration;r.value.valid=true;return r;}
TwoHandComparisonResult compareTwoHand(const TwoHandTemplate&c,const TwoHandTemplate&t) noexcept{TwoHandComparisonResult r{};if(!c.valid||!t.valid){r.reason="Invalid two-hand template";return r;}if(t.summary.relativeTranslationDistance>=.22F&&c.summary.relativeTranslationDistance<t.summary.relativeTranslationDistance*.45F){r.reason="Two-hand relative translation too small";return r;}if(t.summary.relativeDistanceRange>=.22F&&c.summary.relativeDistanceRange<t.summary.relativeDistanceRange*.55F){r.reason="Two-hand distance change too small";return r;}if(std::abs(t.summary.relativeDistanceDelta)>=.22F&&std::signbit(c.summary.relativeDistanceDelta)!=std::signbit(t.summary.relativeDistanceDelta)){r.reason="Two-hand distance moved opposite direction";return r;}const auto forward=dtw(c.frames,t.frames,false),reverse=dtw(c.frames,t.frames,true);r.score=forward.score;r.confidence=std::clamp(1-r.score/.9F,0.F,1.F);r.warpRatio=static_cast<float>(forward.path)/measurements::kUnifiedSequenceLength;if(reverse.score<=forward.score*kReverseMargin){r.reason="Reversed two-hand time direction fits better";return r;}r.eligible=r.confidence>=kConfidenceThreshold;r.reason=r.eligible?"Eligible two-hand":"Below two-hand threshold";return r;}
}
