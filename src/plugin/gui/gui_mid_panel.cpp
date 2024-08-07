// Copyright 2018-2024 Sam Windell
// SPDX-License-Identifier: GPL-3.0-or-later

#include <IconsFontAwesome5.h>

#include "framework/gui_live_edit.hpp"
#include "gui.hpp"
#include "gui_effects.hpp"
#include "gui_widget_helpers.hpp"
#include "gui_window.hpp"

void MidPanel(Gui* g) {
    auto& imgui = g->imgui;
    auto& lay = g->layout;
    auto& plugin = g->plugin;

    auto const layer_width = LiveSize(imgui, UiSizeId::LayerWidth);
    auto const total_layer_width = layer_width * k_num_layers;
    auto const mid_panel_title_height = LiveSize(imgui, UiSizeId::MidPanelTitleHeight);
    auto const mid_panel_size = imgui.Size();

    auto const get_background_uvs =
        [&](LibraryImages const& imgs, Rect r, imgui::Window* window, f32x2& out_min_uv, f32x2& out_max_uv) {
            auto const whole_uv = GetMaxUVToMaintainAspectRatio(*imgs.background, mid_panel_size);
            auto const left_margin = r.x - window->parent_window->bounds.x;
            auto const top_margin = r.y - window->parent_window->bounds.y;

            out_min_uv = {whole_uv.x * (left_margin / mid_panel_size.x),
                          whole_uv.y * (top_margin / mid_panel_size.y)};
            out_max_uv = {whole_uv.x * (r.w + left_margin) / mid_panel_size.x,
                          whole_uv.y * (r.h + top_margin) / mid_panel_size.y};
        };

    auto const panel_rounding = LiveSize(imgui, UiSizeId::BlurredPanelRounding);

    auto do_randomise_button = [&](String tooltip) {
        auto const margin = LiveSize(imgui, UiSizeId::MidPanelTitleMarginLeft);
        auto const size = LiveSize(imgui, UiSizeId::LayerSelectorButtonW);
        Rect const btn_r {imgui.Width() - (size + margin), 0, size, mid_panel_title_height};
        auto const id = imgui.GetID("rand");
        if (buttons::Button(g,
                            id,
                            btn_r,
                            ICON_FA_RANDOM,
                            buttons::IconButton(imgui).WithRandomiseIconScaling()))
            return true;
        Tooltip(g, id, btn_r, tooltip);
        return false;
    };

    {
        auto settings = FloeWindowSettings(imgui, [&](IMGUI_DRAW_WINDOW_BG_ARGS) {
            auto const first_lib_name = g->plugin.Layer(0).LibId();
            if (first_lib_name) {
                auto const& r = window->bounds;

                auto background_lib =
                    sample_lib_server::FindLibraryRetained(g->plugin.shared_data.sample_library_server,
                                                           *first_lib_name);
                DEFER { background_lib.Release(); };

                if (background_lib && !g->settings.settings.gui.high_contrast_gui) {
                    auto imgs = LoadLibraryBackgroundAndIconIfNeeded(g, *background_lib);
                    if (imgs.blurred_background) {

                        if (auto tex =
                                g->frame_input.graphics_ctx->GetTextureFromImage(imgs.blurred_background);
                            tex && !g->settings.settings.gui.high_contrast_gui) {
                            f32x2 min_uv;
                            f32x2 max_uv;
                            get_background_uvs(imgs, r, window, min_uv, max_uv);
                            imgui.graphics->AddImageRounded(*tex,
                                                            r.Min(),
                                                            r.Max(),
                                                            min_uv,
                                                            max_uv,
                                                            LiveCol(imgui, UiColMap::BlurredImageDrawColour),
                                                            panel_rounding);
                        } else {
                            imgui.graphics->AddRectFilled(r.Min(),
                                                          r.Max(),
                                                          LiveCol(imgui, UiColMap::BlurredImageFallback),
                                                          panel_rounding);
                        }

                        {
                            int const vtx_idx_0 = imgui.graphics->vtx_buffer.size;
                            auto const pos = r.Min() + f32x2 {1, 1};
                            auto const size = f32x2 {r.w, r.h / 2} - f32x2 {2, 2};
                            imgui.graphics->AddRectFilled(pos, pos + size, 0xffffffff, panel_rounding);
                            int const vtx_idx_1 = imgui.graphics->vtx_buffer.size;
                            imgui.graphics->AddRectFilled(pos, pos + size, 0xffffffff, panel_rounding);
                            int const vtx_idx_2 = imgui.graphics->vtx_buffer.size;

                            graphics::DrawList::ShadeVertsLinearColorGradientSetAlpha(
                                imgui.graphics,
                                vtx_idx_0,
                                vtx_idx_1,
                                pos,
                                pos + f32x2 {0, size.y},
                                LiveCol(imgui, UiColMap::BlurredImageGradientOverlay),
                                0);
                            graphics::DrawList::ShadeVertsLinearColorGradientSetAlpha(
                                imgui.graphics,
                                vtx_idx_1,
                                vtx_idx_2,
                                pos + f32x2 {size.x, 0},
                                pos + f32x2 {size.x, size.y},
                                LiveCol(imgui, UiColMap::BlurredImageGradientOverlay),
                                0);
                        }

                        imgui.graphics->AddRect(r.Min(),
                                                r.Max(),
                                                LiveCol(imgui, UiColMap::BlurredImageBorder),
                                                panel_rounding);

                        imgui.graphics->AddLine({r.x, r.y + mid_panel_title_height},
                                                {r.Right(), r.y + mid_panel_title_height},
                                                LiveCol(imgui, UiColMap::LayerDividerLine));
                        for (u32 i = 1; i < k_num_layers; ++i) {
                            auto const x_pos = r.x + (f32)i * (r.w / k_num_layers);
                            imgui.graphics->AddLine({x_pos, r.y + mid_panel_title_height},
                                                    {x_pos, r.Bottom()},
                                                    LiveCol(imgui, UiColMap::LayerDividerLine));
                        }
                    }
                }
            }
        });

        settings.pad_top_left.x = LiveSize(imgui, UiSizeId::LayersBoxMarginL);
        settings.pad_top_left.y = LiveSize(imgui, UiSizeId::LayersBoxMarginT);
        settings.pad_bottom_right.x = LiveSize(imgui, UiSizeId::LayersBoxMarginR);
        settings.pad_bottom_right.y = LiveSize(imgui, UiSizeId::LayersBoxMarginB);
        imgui.BeginWindow(settings, {0, 0, total_layer_width, imgui.Height()}, "Layers");

        // do the title
        {
            Rect title_r {LiveSize(imgui, UiSizeId::MidPanelTitleMarginLeft),
                          0,
                          imgui.Width(),
                          mid_panel_title_height};
            imgui.RegisterAndConvertRect(&title_r);
            imgui.graphics->AddTextJustified(title_r,
                                             "Layers",
                                             LiveCol(imgui, UiColMap::MidPanelTitleText),
                                             TextJustification::CentredLeft);
        }

        // randomise button
        if (do_randomise_button("Load random instruments for all 3 layers")) RandomiseAllLayerInsts(plugin);

        // do the 3 panels
        auto const layer_width_minus_pad = imgui.Width() / 3;
        auto const layer_height = imgui.Height() - mid_panel_title_height;
        for (auto const i : Range(k_num_layers)) {
            layer_gui::LayerLayoutTempIDs ids {};
            layer_gui::Layout(g,
                              &plugin.Layer(i),
                              ids,
                              &g->layer_gui[i],
                              layer_width_minus_pad,
                              layer_height);
            lay.PerformLayout();

            layer_gui::Draw(
                g,
                &plugin,
                {(f32)i * layer_width_minus_pad, mid_panel_title_height, layer_width_minus_pad, layer_height},
                &plugin.Layer(i),
                ids,
                &g->layer_gui[i]);
            lay.Reset();
        }

        imgui.EndWindow();
    }

    {
        auto settings = FloeWindowSettings(imgui, [&](IMGUI_DRAW_WINDOW_BG_ARGS) {
            auto const first_lib_name = g->plugin.Layer(0).LibId();
            if (first_lib_name) {
                auto const& r = window->bounds;

                auto background_lib =
                    sample_lib_server::FindLibraryRetained(g->plugin.shared_data.sample_library_server,
                                                           *first_lib_name);
                DEFER { background_lib.Release(); };

                if (background_lib && !g->settings.settings.gui.high_contrast_gui) {
                    auto imgs = LoadLibraryBackgroundAndIconIfNeeded(g, *background_lib);
                    if (imgs.blurred_background) {
                        if (auto tex =
                                g->frame_input.graphics_ctx->GetTextureFromImage(*imgs.blurred_background)) {
                            f32x2 min_uv;
                            f32x2 max_uv;
                            get_background_uvs(imgs, r, window, min_uv, max_uv);
                            imgui.graphics->AddImageRounded(*tex,
                                                            r.Min(),
                                                            r.Max(),
                                                            min_uv,
                                                            max_uv,
                                                            LiveCol(imgui, UiColMap::BlurredImageDrawColour),
                                                            panel_rounding);
                        } else {
                            imgui.graphics->AddRectFilled(r.Min(),
                                                          r.Max(),
                                                          LiveCol(imgui, UiColMap::BlurredImageFallback),
                                                          panel_rounding);
                        }

                        {
                            int const vtx_idx_0 = imgui.graphics->vtx_buffer.size;
                            auto const pos = r.Min() + f32x2 {1, 1};
                            auto const size = f32x2 {r.w, r.h / 2} - f32x2 {2, 2};
                            imgui.graphics->AddRectFilled(pos, pos + size, 0xffffffff, panel_rounding);
                            int const vtx_idx_1 = imgui.graphics->vtx_buffer.size;
                            imgui.graphics->AddRectFilled(pos, pos + size, 0xffffffff, panel_rounding);
                            int const vtx_idx_2 = imgui.graphics->vtx_buffer.size;

                            graphics::DrawList::ShadeVertsLinearColorGradientSetAlpha(
                                imgui.graphics,
                                vtx_idx_0,
                                vtx_idx_1,
                                pos,
                                pos + f32x2 {0, size.y},
                                LiveCol(imgui, UiColMap::BlurredImageGradientOverlay),
                                0);
                            graphics::DrawList::ShadeVertsLinearColorGradientSetAlpha(
                                imgui.graphics,
                                vtx_idx_1,
                                vtx_idx_2,
                                pos + f32x2 {size.x, 0},
                                pos + f32x2 {size.x, size.y},
                                LiveCol(imgui, UiColMap::BlurredImageGradientOverlay),
                                0);
                        }

                        imgui.graphics->AddRect(r.Min(),
                                                r.Max(),
                                                LiveCol(imgui, UiColMap::BlurredImageBorder),
                                                panel_rounding);

                        imgui.graphics->AddLine({r.x, r.y + mid_panel_title_height},
                                                {r.Right(), r.y + mid_panel_title_height},
                                                LiveCol(imgui, UiColMap::LayerDividerLine));
                    }
                }
            }
        });
        settings.pad_top_left.x = LiveSize(imgui, UiSizeId::FXListMarginL);
        settings.pad_top_left.y = LiveSize(imgui, UiSizeId::FXListMarginT);
        settings.pad_bottom_right.x = LiveSize(imgui, UiSizeId::FXListMarginR);
        settings.pad_bottom_right.y = LiveSize(imgui, UiSizeId::FXListMarginB);

        imgui.BeginWindow(settings,
                          {total_layer_width, 0, imgui.Width() - total_layer_width, imgui.Height()},
                          "EffectsContainer");

        // do the title
        {
            Rect title_r {LiveSize(imgui, UiSizeId::MidPanelTitleMarginLeft),
                          0,
                          imgui.Width(),
                          mid_panel_title_height};
            imgui.RegisterAndConvertRect(&title_r);
            imgui.graphics->AddTextJustified(title_r,
                                             "Effects",
                                             LiveCol(imgui, UiColMap::MidPanelTitleText),
                                             TextJustification::CentredLeft);
        }

        // randomise button
        if (do_randomise_button("Randomise all of the effects"))
            RandomiseAllEffectParameterValues(plugin.processor);

        DoEffectsWindow(g,
                        {0, mid_panel_title_height, imgui.Width(), imgui.Height() - mid_panel_title_height});

        imgui.EndWindow();
    }
}
