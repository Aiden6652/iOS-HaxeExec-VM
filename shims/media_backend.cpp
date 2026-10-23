// SPDX-License-Identifier: MIT
#include "shims/media_backend.hpp"

namespace hxvm {
namespace {

MediaBackend g_media;
RenderBackend g_render;

}  // namespace

void SetMediaBackend(const MediaBackend &b) { g_media = b; }
const MediaBackend &GetMediaBackend() { return g_media; }

void SetRenderBackend(const RenderBackend &b) { g_render = b; }
const RenderBackend &GetRenderBackend() { return g_render; }

bool HasMediaBackend() { return g_media.open != nullptr; }
bool HasRenderBackend() { return g_render.create_window != nullptr; }

}  // namespace hxvm
