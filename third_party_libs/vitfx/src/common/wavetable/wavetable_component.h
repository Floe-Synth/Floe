// Copyright 2013-2019 Matt Tytel

//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "JuceHeader.h"
#include "wavetable_keyframe.h"
#include "wavetable_component_factory.h"
#include "json/json.h"

using json = nlohmann::json;

namespace vital {
  class WaveFrame;
} // namespace vital

class WavetableComponent {
  public:
    enum InterpolationStyle {
      kNone,
      kLinear,
      kCubic,
      kNumInterpolationStyles
    };

    WavetableComponent() : interpolation_style_(kLinear) { }
    virtual ~WavetableComponent() { }

    virtual WavetableKeyframe* createKeyframe(int position) = 0;
    virtual void render(vital::WaveFrame* wave_frame, float position) = 0;
    virtual WavetableComponentFactory::ComponentType getType() = 0;
    virtual json stateToJson();
    virtual void jsonToState(json data);
    virtual void prerender() { }
    virtual bool hasKeyframes() { return true; }

    void reset();
    void interpolate(WavetableKeyframe* dest, float position);
    WavetableKeyframe* insertNewKeyframe(int position);
    void reposition(WavetableKeyframe* keyframe);
    void remove(WavetableKeyframe* keyframe);

    inline int numFrames() const { return static_cast<int>(keyframes_.size()); }
    inline int indexOf(WavetableKeyframe* keyframe) const {
      for (int i = 0; i < keyframes_.size(); ++i) {
        if (keyframes_[i].get() == keyframe)
          return i;
      }
      return -1;
    }
    inline WavetableKeyframe* getFrameAt(int index) const { return keyframes_[index].get(); }
    int getIndexFromPosition(int position) const;
    WavetableKeyframe* getFrameAtPosition(int position);
    int getLastKeyframePosition();

    void setInterpolationStyle(InterpolationStyle type) { interpolation_style_ = type; }
    InterpolationStyle getInterpolationStyle() const { return interpolation_style_; }
  
  protected:
    std::vector<std::unique_ptr<WavetableKeyframe>> keyframes_;
    InterpolationStyle interpolation_style_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WavetableComponent)
};

