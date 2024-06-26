// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <vitfx/wrapper.hpp>

#include "effects/effect.hpp"

class Phaser final : public Effect {
  public:
    Phaser(FloeSmoothedValueSystem& s) : Effect(s, EffectType::Phaser), phaser(vitfx::phaser::Create()) {}
    ~Phaser() override { vitfx::phaser::Destroy(phaser); }

    void ResetInternal() override { vitfx::phaser::HardReset(*phaser); }

    virtual void PrepareToPlay(AudioProcessingContext const& context) override {
        vitfx::phaser::SetSampleRate(*phaser, (int)context.sample_rate);
    }

    bool ProcessBlock(Span<StereoAudioFrame> io_frames,
                      ScratchBuffers scratch_buffers,
                      AudioProcessingContext const&) override {
        if (!ShouldProcessBlock()) return false;

        auto wet = scratch_buffers.buf1.Interleaved();
        wet.size = io_frames.size;
        CopyMemory(wet.data, io_frames.data, io_frames.size * sizeof(StereoAudioFrame));

        auto num_frames = (u32)io_frames.size;
        u32 pos = 0;
        while (num_frames) {
            u32 const chunk_size = Min(num_frames, 64u);

            vitfx::phaser::ProcessPhaserArgs args {
                .num_frames = (int)chunk_size,
                .in_interleaved = (f32*)(io_frames.data + pos),
                .out_interleaved = (f32*)(wet.data + pos),
            };
            CopyMemory(args.params, params, sizeof(params));
            // TODO(1.0): use center_semitones_buffered

            vitfx::phaser::Process(*phaser, args);

            num_frames -= chunk_size;
            pos += chunk_size;
        }

        for (auto const frame_index : Range((u32)io_frames.size))
            io_frames[frame_index] = MixOnOffSmoothing(wet[frame_index], io_frames[frame_index], frame_index);

        return true;
    }

    void OnParamChangeInternal(ChangedParams changed_params, AudioProcessingContext const&) override {
        using namespace vitfx::phaser;

        if (auto p = changed_params.Param(ParamIndex::PhaserFeedback))
            params[ToInt(Params::FeedbackAmount)] = p->ProjectedValue();
        if (auto p = changed_params.Param(ParamIndex::PhaserModFreqHz))
            params[ToInt(Params::FrequencyHz)] = p->ProjectedValue();
        if (auto p = changed_params.Param(ParamIndex::PhaserCenterSemitones))
            params[ToInt(Params::CenterSemitones)] = p->ProjectedValue();
        if (auto p = changed_params.Param(ParamIndex::PhaserShape))
            params[ToInt(Params::Blend)] = p->ProjectedValue() * 2;
        if (auto p = changed_params.Param(ParamIndex::PhaserModDepth))
            params[ToInt(Params::ModDepthSemitones)] = p->ProjectedValue();
        if (auto p = changed_params.Param(ParamIndex::PhaserStereoAmount))
            params[ToInt(Params::PhaseOffset)] = p->ProjectedValue() / 2;
        if (auto p = changed_params.Param(ParamIndex::PhaserMix))
            params[ToInt(Params::Mix)] = p->ProjectedValue();
    }

    vitfx::phaser::Phaser* phaser {};
    f32 params[ToInt(vitfx::phaser::Params::Count)] {};
};
