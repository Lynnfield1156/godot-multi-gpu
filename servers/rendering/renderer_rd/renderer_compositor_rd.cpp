/**************************************************************************/
/*  renderer_compositor_rd.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "renderer_compositor_rd.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"

#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/effects/ss_effects.h"
#include "servers/rendering/renderer_rd/environment/gi.h"

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.h"
#include "servers/rendering/renderer_rd/storage_rd/forward_id_storage.h"
#include "servers/rendering/rendering_server_globals.h"

void RendererCompositorRD::blit_render_targets_to_screen(DisplayServer::WindowID p_screen, const BlitToScreen *p_render_targets, int p_amount) {
	Error err = RD::get_singleton()->screen_prepare_for_drawing(p_screen);
	if (err != OK) {
		// Window is minimized and does not have valid swapchain, skip drawing without printing errors.
		return;
	}

	BlitPipelines blit_pipelines = _get_blit_pipelines_for_format(RD::get_singleton()->screen_get_framebuffer_format(p_screen));

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin_for_screen(p_screen);
	ERR_FAIL_COND(draw_list == RD::INVALID_ID);

	const RD::ColorSpace color_space = RD::get_singleton()->screen_get_color_space(p_screen);
	const float reference_luminance = RD::get_singleton()->get_context_driver()->window_get_hdr_output_reference_luminance(p_screen);
	const float linear_luminance_scale = RD::get_singleton()->get_context_driver()->window_get_hdr_output_linear_luminance_scale(p_screen);
	const float output_max_value = RD::get_singleton()->get_context_driver()->window_get_output_max_linear_value(p_screen);
	const float reference_multiplier = _compute_reference_multiplier(color_space, reference_luminance, linear_luminance_scale);

	for (int i = 0; i < p_amount; i++) {
		RID rd_texture = texture_storage->render_target_get_rd_texture(p_render_targets[i].render_target);
		ERR_CONTINUE(rd_texture.is_null());

		HashMap<RID, RID>::Iterator it = render_target_descriptors.find(rd_texture);
		if (it == render_target_descriptors.end() || !RD::get_singleton()->uniform_set_is_valid(it->value)) {
			Vector<RD::Uniform> uniforms;
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			u.binding = 0;
			u.append_id(blit.sampler);
			u.append_id(rd_texture);
			uniforms.push_back(u);
			RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, blit.shader.version_get_shader(blit.shader_version, BLIT_MODE_NORMAL), 0);

			it = render_target_descriptors.insert(rd_texture, uniform_set);
		}

		Size2 screen_size(RD::get_singleton()->screen_get_width(p_screen), RD::get_singleton()->screen_get_height(p_screen));
		BlitMode mode = p_render_targets[i].lens_distortion.apply ? BLIT_MODE_LENS : (p_render_targets[i].multi_view.use_layer ? BLIT_MODE_USE_LAYER : BLIT_MODE_NORMAL);

		RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, blit_pipelines.pipelines[mode]);
		RD::get_singleton()->draw_list_bind_index_array(draw_list, blit.array);
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, it->value, 0);

		// We need to invert the phone rotation.
		const int screen_rotation_degrees = -RD::get_singleton()->screen_get_pre_rotation_degrees(p_screen);
		float screen_rotation = Math::deg_to_rad((float)screen_rotation_degrees);

		blit.push_constant.rotation_cos = Math::cos(screen_rotation);
		blit.push_constant.rotation_sin = Math::sin(screen_rotation);
		// Swap width and height when the orientation is not the native one.
		if (screen_rotation_degrees % 180 != 0) {
			SWAP(screen_size.width, screen_size.height);
		}
		blit.push_constant.src_rect[0] = p_render_targets[i].src_rect.position.x;
		blit.push_constant.src_rect[1] = p_render_targets[i].src_rect.position.y;
		blit.push_constant.src_rect[2] = p_render_targets[i].src_rect.size.width;
		blit.push_constant.src_rect[3] = p_render_targets[i].src_rect.size.height;
		blit.push_constant.dst_rect[0] = p_render_targets[i].dst_rect.position.x / screen_size.width;
		blit.push_constant.dst_rect[1] = p_render_targets[i].dst_rect.position.y / screen_size.height;
		blit.push_constant.dst_rect[2] = p_render_targets[i].dst_rect.size.width / screen_size.width;
		blit.push_constant.dst_rect[3] = p_render_targets[i].dst_rect.size.height / screen_size.height;
		blit.push_constant.layer = p_render_targets[i].multi_view.layer;
		blit.push_constant.eye_center[0] = p_render_targets[i].lens_distortion.eye_center.x;
		blit.push_constant.eye_center[1] = p_render_targets[i].lens_distortion.eye_center.y;
		blit.push_constant.k1 = p_render_targets[i].lens_distortion.k1;
		blit.push_constant.k2 = p_render_targets[i].lens_distortion.k2;
		blit.push_constant.upscale = p_render_targets[i].lens_distortion.upscale;
		blit.push_constant.aspect_ratio = p_render_targets[i].lens_distortion.aspect_ratio;
		blit.push_constant.source_is_srgb = !texture_storage->render_target_is_using_hdr(p_render_targets[i].render_target);
		blit.push_constant.use_debanding = texture_storage->render_target_is_using_debanding(p_render_targets[i].render_target);
		blit.push_constant.target_color_space = color_space;
		blit.push_constant.reference_multiplier = reference_multiplier;
		blit.push_constant.output_max_value = output_max_value;

		RD::get_singleton()->draw_list_set_push_constant(draw_list, &blit.push_constant, sizeof(BlitPushConstant));
		RD::get_singleton()->draw_list_draw(draw_list, true);
	}

	RD::get_singleton()->draw_list_end();
}

void RendererCompositorRD::begin_frame(double frame_step) {
	frame++;
	delta = frame_step;
	time += frame_step;

	double time_roll_over = GLOBAL_GET_CACHED(double, "rendering/limits/time/time_rollover_secs");
	time = Math::fmod(time, time_roll_over);

	canvas->set_time(time);
	for (KeyValue<uint32_t, GPUContext> &E : gpu_contexts) {
		if (E.value.canvas) {
			E.value.canvas->set_time(time);
		}
	}
	scene->set_time(time, frame_step);
}

void RendererCompositorRD::end_frame(bool p_present) {
	RD::get_singleton()->swap_buffers(p_present);
}

void RendererCompositorRD::initialize() {
	{
		// Initialize blit
		Vector<String> blit_modes;
		blit_modes.push_back("\n");
		blit_modes.push_back("\n#define USE_LAYER\n");
		blit_modes.push_back("\n#define USE_LAYER\n#define APPLY_LENS_DISTORTION\n");
		blit_modes.push_back("\n");

		blit.shader.initialize(blit_modes);
		blit.shader_version = blit.shader.version_create();

		//create index array for copy shader
		Vector<uint8_t> pv;
		pv.resize(6 * 2);
		{
			uint8_t *w = pv.ptrw();
			uint16_t *p16 = (uint16_t *)w;
			p16[0] = 0;
			p16[1] = 1;
			p16[2] = 2;
			p16[3] = 0;
			p16[4] = 2;
			p16[5] = 3;
		}
		blit.index_buffer = RD::get_singleton()->index_buffer_create(6, RenderingDevice::INDEX_BUFFER_FORMAT_UINT16, pv);
		blit.array = RD::get_singleton()->index_array_create(blit.index_buffer, 0, 6);

		blit.sampler = RD::get_singleton()->sampler_create(RD::SamplerState());
	}
}

uint64_t RendererCompositorRD::frame = 1;

void RendererCompositorRD::finalize() {
	texture_storage->_tex_blit_shader_free();
	memdelete(scene);
	memdelete(canvas);
	memdelete(fog);
	memdelete(particles_storage);
	memdelete(light_storage);
	memdelete(mesh_storage);
	memdelete(material_storage);
	memdelete(texture_storage);
	memdelete(utilities);

	//only need to erase these, the rest are erased by cascade
	blit.shader.version_free(blit.shader_version);
	RD::get_singleton()->free_rid(blit.index_buffer);
	RD::get_singleton()->free_rid(blit.sampler);
}

RendererCompositorRD::BlitPipelines RendererCompositorRD::_get_blit_pipelines_for_format(RenderingDevice::FramebufferFormatID format) {
	HashMap<RenderingDevice::FramebufferFormatID, BlitPipelines>::Iterator it = blit.pipelines_by_format.find(format);
	if (it != blit.pipelines_by_format.end()) {
		return it->value;
	}

	BlitPipelines pipelines;
	for (int i = 0; i < BLIT_MODE_MAX; i++) {
		pipelines.pipelines[i] = RD::get_singleton()->render_pipeline_create(blit.shader.version_get_shader(blit.shader_version, i), format, RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), i == BLIT_MODE_NORMAL_ALPHA ? RenderingDevice::PipelineColorBlendState::create_blend() : RenderingDevice::PipelineColorBlendState::create_disabled(), 0);
	}
	blit.pipelines_by_format.insert(format, pipelines);
	return pipelines;
}

float RendererCompositorRD::_compute_reference_multiplier(RD::ColorSpace p_color_space, const float p_reference_luminance, const float p_linear_luminance_scale) {
	switch (p_color_space) {
		case RD::COLOR_SPACE_REC709_LINEAR:
			return p_reference_luminance / p_linear_luminance_scale;
		default:
			return 1.0f;
	}
}

void RendererCompositorRD::set_boot_image_with_stretch(const Ref<Image> &p_image, const Color &p_color, RenderingServer::SplashStretchMode p_stretch_mode, bool p_use_filter) {
	if (p_image.is_null() || p_image->is_empty()) {
		return;
	}

	Error err = RD::get_singleton()->screen_prepare_for_drawing(DisplayServer::MAIN_WINDOW_ID);
	if (err != OK) {
		// Window is minimized and does not have valid swapchain, skip drawing without printing errors.
		return;
	}

	BlitPipelines blit_pipelines = _get_blit_pipelines_for_format(RD::get_singleton()->screen_get_framebuffer_format(DisplayServer::MAIN_WINDOW_ID));

	RID texture = texture_storage->texture_allocate();
	texture_storage->texture_2d_initialize(texture, p_image);
	RID rd_texture = texture_storage->texture_get_rd_texture(texture, false);

	RD::SamplerState sampler_state;
	sampler_state.min_filter = p_use_filter ? RD::SAMPLER_FILTER_LINEAR : RD::SAMPLER_FILTER_NEAREST;
	sampler_state.mag_filter = p_use_filter ? RD::SAMPLER_FILTER_LINEAR : RD::SAMPLER_FILTER_NEAREST;
	sampler_state.max_lod = 0;
	RID sampler = RD::get_singleton()->sampler_create(sampler_state);

	RID uset;
	{
		Vector<RD::Uniform> uniforms;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		u.binding = 0;
		u.append_id(sampler);
		u.append_id(rd_texture);
		uniforms.push_back(u);
		uset = RD::get_singleton()->uniform_set_create(uniforms, blit.shader.version_get_shader(blit.shader_version, BLIT_MODE_NORMAL), 0);
	}

	Size2 window_size = DisplayServer::get_singleton()->window_get_size();

	Rect2 screenrect = RenderingServer::get_splash_stretched_screen_rect(p_image->get_size(), window_size, p_stretch_mode);
	screenrect.position /= window_size;
	screenrect.size /= window_size;

	const RD::ColorSpace color_space = RD::get_singleton()->screen_get_color_space(DisplayServer::MAIN_WINDOW_ID);
	const float reference_luminance = RD::get_singleton()->get_context_driver()->window_get_hdr_output_reference_luminance(DisplayServer::MAIN_WINDOW_ID);
	const float linear_luminance_scale = RD::get_singleton()->get_context_driver()->window_get_hdr_output_linear_luminance_scale(DisplayServer::MAIN_WINDOW_ID);
	const float output_max_value = RD::get_singleton()->get_context_driver()->window_get_output_max_linear_value(DisplayServer::MAIN_WINDOW_ID);
	const float reference_multiplier = _compute_reference_multiplier(color_space, reference_luminance, linear_luminance_scale);

	Color clear_color = p_color;
	if (color_space != RD::COLOR_SPACE_REC709_NONLINEAR_SRGB) {
		// draw_list_begin_for_screen requires linear-encoded Color when using an HDR buffer.
		clear_color = p_color.srgb_to_linear();

		clear_color.r *= reference_multiplier;
		clear_color.g *= reference_multiplier;
		clear_color.b *= reference_multiplier;
	}

	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin_for_screen(DisplayServer::MAIN_WINDOW_ID, clear_color);

	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, blit_pipelines.pipelines[BLIT_MODE_NORMAL_ALPHA]);
	RD::get_singleton()->draw_list_bind_index_array(draw_list, blit.array);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uset, 0);

	const int screen_rotation_degrees = -RD::get_singleton()->screen_get_pre_rotation_degrees(DisplayServer::MAIN_WINDOW_ID);
	float screen_rotation = Math::deg_to_rad((float)screen_rotation_degrees);
	blit.push_constant.rotation_cos = Math::cos(screen_rotation);
	blit.push_constant.rotation_sin = Math::sin(screen_rotation);
	blit.push_constant.src_rect[0] = 0.0;
	blit.push_constant.src_rect[1] = 0.0;
	blit.push_constant.src_rect[2] = 1.0;
	blit.push_constant.src_rect[3] = 1.0;
	blit.push_constant.dst_rect[0] = screenrect.position.x;
	blit.push_constant.dst_rect[1] = screenrect.position.y;
	blit.push_constant.dst_rect[2] = screenrect.size.width;
	blit.push_constant.dst_rect[3] = screenrect.size.height;
	blit.push_constant.layer = 0;
	blit.push_constant.eye_center[0] = 0;
	blit.push_constant.eye_center[1] = 0;
	blit.push_constant.k1 = 0;
	blit.push_constant.k2 = 0;
	blit.push_constant.upscale = 1.0;
	blit.push_constant.aspect_ratio = 1.0;
	blit.push_constant.source_is_srgb = true;
	blit.push_constant.use_debanding = false;
	blit.push_constant.target_color_space = color_space;
	blit.push_constant.reference_multiplier = reference_multiplier;
	blit.push_constant.output_max_value = output_max_value;

	RD::get_singleton()->draw_list_set_push_constant(draw_list, &blit.push_constant, sizeof(BlitPushConstant));
	RD::get_singleton()->draw_list_draw(draw_list, true);

	RD::get_singleton()->draw_list_end();

	RD::get_singleton()->swap_buffers(true);

	texture_storage->texture_free(texture);
	RD::get_singleton()->free_rid(sampler);
}

RendererCompositorRD *RendererCompositorRD::singleton = nullptr;

RendererCompositorRD::RendererCompositorRD() {
	uniform_set_cache = memnew(UniformSetCacheRD);
	framebuffer_cache = memnew(FramebufferCacheRD);

	bool shader_cache_enabled = GLOBAL_GET("rendering/shader_compiler/shader_cache/enabled");
	bool compress = GLOBAL_GET("rendering/shader_compiler/shader_cache/compress");
	bool use_zstd = GLOBAL_GET("rendering/shader_compiler/shader_cache/use_zstd_compression");
	bool strip_debug = GLOBAL_GET("rendering/shader_compiler/shader_cache/strip_debug");
	ShaderRD::set_shader_cache_save_compressed(compress);
	ShaderRD::set_shader_cache_save_compressed_zstd(use_zstd);
	ShaderRD::set_shader_cache_save_debug(!strip_debug);

	// Shader cache is forcefully enabled when running the editor.
	if (shader_cache_enabled || Engine::get_singleton()->is_editor_hint()) {
		// Attempt to create a folder for the shader cache that the user can write to. Shaders will only be attempted to be saved if this path exists.
		String shader_cache_user_dir = Engine::get_singleton()->get_shader_cache_path();
		if (shader_cache_user_dir.is_empty()) {
			shader_cache_user_dir = "user://";
		}

		Ref<DirAccess> user_da = DirAccess::open(shader_cache_user_dir);
		if (user_da.is_null()) {
			ERR_PRINT("Can't create shader cache folder, no shader caching will happen: " + shader_cache_user_dir);
		} else {
			Error err = user_da->change_dir("shader_cache");
			if (err != OK) {
				err = user_da->make_dir("shader_cache");
			}

			if (err != OK) {
				ERR_PRINT("Can't create shader cache folder, no shader caching will happen: " + shader_cache_user_dir);
			} else {
				shader_cache_user_dir = shader_cache_user_dir.path_join("shader_cache");
				ShaderRD::set_shader_cache_user_dir(shader_cache_user_dir);
			}
		}

		// Check if a directory exists for the shader cache to pull shaders from as read-only. This is used on exported projects with baked shaders.
		String shader_cache_res_dir = "res://.godot/shader_cache";
		Ref<DirAccess> res_da = DirAccess::open(shader_cache_res_dir);
		if (res_da.is_valid()) {
			ShaderRD::set_shader_cache_res_dir(shader_cache_res_dir);
		}
	}

	ERR_FAIL_COND_MSG(singleton != nullptr, "A RendererCompositorRD singleton already exists.");
	singleton = this;

	utilities = memnew(RendererRD::Utilities);
	texture_storage = memnew(RendererRD::TextureStorage);
	material_storage = memnew(RendererRD::MaterialStorage);
	mesh_storage = memnew(RendererRD::MeshStorage);
	light_storage = memnew(RendererRD::LightStorage);
	particles_storage = memnew(RendererRD::ParticlesStorage);
	fog = memnew(RendererRD::Fog);
	canvas = memnew(RendererCanvasRenderRD());
	texture_storage->_tex_blit_shader_initialize();

	String rendering_method = OS::get_singleton()->get_current_rendering_method();
	uint64_t textures_per_stage = RD::get_singleton()->limit_get(RD::LIMIT_MAX_TEXTURES_PER_SHADER_STAGE);

	if (rendering_method == "mobile" || textures_per_stage < 48) {
		if (rendering_method == "forward_plus") {
			WARN_PRINT_ONCE("Platform supports less than 48 textures per stage which is less than required by the Clustered renderer. Defaulting to Mobile renderer.");
		}
		scene = memnew(RendererSceneRenderImplementation::RenderForwardMobile());
	} else if (rendering_method == "forward_plus") {
		scene = memnew(RendererSceneRenderImplementation::RenderForwardClustered());
	} else {
		// Fall back to our high end renderer.
		ERR_PRINT(vformat("Cannot instantiate RenderingDevice-based renderer with renderer type '%s'. Defaulting to Forward+ renderer.", rendering_method));
		scene = memnew(RendererSceneRenderImplementation::RenderForwardClustered());
	}

	scene->init();
}

Error RendererCompositorRD::ensure_gpu_context(uint32_t p_gpu_index, RenderingDevice *p_device) {
	ERR_FAIL_COND_V(p_gpu_index == 0, ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(p_device, ERR_INVALID_PARAMETER);

	if (gpu_contexts.has(p_gpu_index)) {
		return OK;
	}

	GPUContext ctx;
	ctx.gpu_index = p_gpu_index;
	ctx.device = p_device;

	RenderingDevice *prev_device = RenderingDevice::get_current_device();
	RenderingDevice::set_current_device(p_device);

	ctx.uniform_set_cache = memnew(UniformSetCacheRD);
	ctx.framebuffer_cache = memnew(FramebufferCacheRD);

	ctx.utilities = memnew(RendererRD::Utilities);
	ctx.texture_storage = memnew(RendererRD::TextureStorage);
	ctx.material_storage = memnew(RendererRD::MaterialStorage);
	ctx.mesh_storage = memnew(RendererRD::MeshStorage);
	ctx.light_storage = memnew(RendererRD::LightStorage);
	ctx.particles_storage = memnew(RendererRD::ParticlesStorage);
	ctx.fog = memnew(RendererRD::Fog);

	RendererRD::Utilities::set_current(ctx.utilities);
	RendererRD::TextureStorage::set_current(ctx.texture_storage);
	RendererRD::MaterialStorage::set_current(ctx.material_storage);
	RendererRD::MeshStorage::set_current(ctx.mesh_storage);
	RendererRD::LightStorage::set_current(ctx.light_storage);
	RendererRD::ParticlesStorage::set_current(ctx.particles_storage);
	RendererRD::Fog::set_current(ctx.fog);
	UniformSetCacheRD::set_current(ctx.uniform_set_cache);
	FramebufferCacheRD::set_current(ctx.framebuffer_cache);

	// Initialize storage subsystems for this GPU context
	ctx.texture_storage->_tex_blit_shader_initialize();

	ctx.canvas = memnew(RendererCanvasRenderRD());
	RendererCanvasRender::set_current(ctx.canvas);


	String rendering_method = OS::get_singleton()->get_current_rendering_method();
	uint64_t textures_per_stage = p_device->limit_get(RD::LIMIT_MAX_TEXTURES_PER_SHADER_STAGE);

	if (rendering_method == "mobile" || textures_per_stage < 48) {
		ctx.scene = memnew(RendererSceneRenderImplementation::RenderForwardMobile());
	} else {
		ctx.scene = memnew(RendererSceneRenderImplementation::RenderForwardClustered());
	}

	RendererSceneRenderRD::set_current(ctx.scene);
	ctx.scene->init();

	RendererRD::Utilities::set_current(nullptr);
	RendererRD::TextureStorage::set_current(nullptr);
	RendererRD::MaterialStorage::set_current(nullptr);
	RendererRD::MeshStorage::set_current(nullptr);
	RendererRD::LightStorage::set_current(nullptr);
	RendererRD::ParticlesStorage::set_current(nullptr);
	RendererRD::Fog::set_current(nullptr);
	RendererCanvasRender::set_current(nullptr);
	RendererSceneRenderRD::set_current(nullptr);
	UniformSetCacheRD::set_current(nullptr);
	FramebufferCacheRD::set_current(nullptr);

	RenderingDevice::set_current_device(prev_device);

	gpu_contexts[p_gpu_index] = ctx;

	print_verbose(vformat("Multi-GPU: Created GPU context %d on %s", p_gpu_index, p_device->get_device_name()));

	return OK;
}

bool RendererCompositorRD::bind_gpu_context(uint32_t p_gpu_index) {
	if (p_gpu_index == 0) {
		unbind_gpu_context();
		return true;
	}

	const GPUContext *ctx = gpu_contexts.getptr(p_gpu_index);
	if (!ctx) {
		return false;
	}

	// Set TLS singletons for code using Class::get_singleton().
	RenderingDevice::set_current_device(ctx->device);
	RendererRD::Utilities::set_current(ctx->utilities);
	RendererRD::TextureStorage::set_current(ctx->texture_storage);
	RendererRD::MaterialStorage::set_current(ctx->material_storage);
	RendererRD::MeshStorage::set_current(ctx->mesh_storage);
	RendererRD::LightStorage::set_current(ctx->light_storage);
	RendererRD::ParticlesStorage::set_current(ctx->particles_storage);
	RendererRD::Fog::set_current(ctx->fog);
	RendererSceneRenderRD::set_current(ctx->scene);
	RendererSceneRender::set_current(ctx->scene);
	UniformSetCacheRD::set_current(ctx->uniform_set_cache);
	FramebufferCacheRD::set_current(ctx->framebuffer_cache);
	if (ctx->scene) {
		RendererRD::ForwardIDStorage::set_current(ctx->scene->get_forward_id_storage());
		RendererRD::CopyEffects::set_current(ctx->scene->get_copy_effects());
		RendererRD::GI::set_current(ctx->scene->get_gi());
	}

	String rendering_method = OS::get_singleton()->get_current_rendering_method();
	if (rendering_method == "forward_plus") {
		auto *fwd = static_cast<RendererSceneRenderImplementation::RenderForwardClustered *>(ctx->scene);
		RendererSceneRenderImplementation::SceneShaderForwardClustered::set_current(fwd->get_scene_shader());
		RendererRD::SSEffects::set_current(fwd->get_ss_effects());
	}

	// Swap RSG:: static globals so code using RSG::texture_storage->, RSG::light_storage->,
	// etc. (e.g. _draw_viewport) routes to the correct GPU's storage instances.
	// Rendering is single-threaded so this is safe.
	RSG::utilities = ctx->utilities;
	RSG::texture_storage = ctx->texture_storage;
	RSG::material_storage = ctx->material_storage;
	RSG::mesh_storage = ctx->mesh_storage;
	RSG::light_storage = ctx->light_storage;
	RSG::particles_storage = ctx->particles_storage;
	RSG::fog = ctx->fog;
	RSG::canvas_render = ctx->canvas;
	RendererCanvasRender::set_current(ctx->canvas);
	if (ctx->scene) {
		RSG::gi = ctx->scene->get_gi();
	}

	return true;
}

void RendererCompositorRD::unbind_gpu_context() {
	RenderingDevice::set_current_device(nullptr);
	RendererRD::Utilities::set_current(nullptr);
	RendererRD::TextureStorage::set_current(nullptr);
	RendererRD::MaterialStorage::set_current(nullptr);
	RendererRD::MeshStorage::set_current(nullptr);
	RendererRD::LightStorage::set_current(nullptr);
	RendererRD::ParticlesStorage::set_current(nullptr);
	RendererRD::Fog::set_current(nullptr);
	RendererSceneRenderRD::set_current(nullptr);
	RendererSceneRender::set_current(nullptr);
	RendererSceneRenderImplementation::SceneShaderForwardClustered::set_current(nullptr);
	UniformSetCacheRD::set_current(nullptr);
	FramebufferCacheRD::set_current(nullptr);
	RendererRD::ForwardIDStorage::set_current(nullptr);
	RendererRD::CopyEffects::set_current(nullptr);
	RendererRD::SSEffects::set_current(nullptr);
	RendererRD::GI::set_current(nullptr);

	// Restore RSG:: static globals to GPU 0's (primary) storage instances.
	RSG::utilities = utilities;
	RSG::texture_storage = texture_storage;
	RSG::material_storage = material_storage;
	RSG::mesh_storage = mesh_storage;
	RSG::light_storage = light_storage;
	RSG::particles_storage = particles_storage;
	RSG::fog = fog;
	RSG::canvas_render = canvas;
	RendererCanvasRender::set_current(nullptr);
	if (scene) {
		RSG::gi = scene->get_gi();
	}
}

const RendererCompositorRD::GPUContext *RendererCompositorRD::get_gpu_context(uint32_t p_gpu_index) const {
	return gpu_contexts.getptr(p_gpu_index);
}

Vector<uint32_t> RendererCompositorRD::get_gpu_context_indices() const {
	Vector<uint32_t> indices;
	for (const KeyValue<uint32_t, GPUContext> &E : gpu_contexts) {
		indices.push_back(E.key);
	}
	return indices;
}

RendererCompositorRD::~RendererCompositorRD() {
	for (KeyValue<uint32_t, GPUContext> &E : gpu_contexts) {
		GPUContext &ctx = E.value;
		bind_gpu_context(ctx.gpu_index);

		if (ctx.scene) {
			memdelete(ctx.scene);
		}
		if (ctx.fog) {
			memdelete(ctx.fog);
		}
		if (ctx.particles_storage) {
			memdelete(ctx.particles_storage);
		}
		if (ctx.light_storage) {
			memdelete(ctx.light_storage);
		}
		if (ctx.mesh_storage) {
			memdelete(ctx.mesh_storage);
		}
		if (ctx.material_storage) {
			memdelete(ctx.material_storage);
		}
		if (ctx.texture_storage) {
			memdelete(ctx.texture_storage);
		}
		if (ctx.utilities) {
			memdelete(ctx.utilities);
		}
		if (ctx.canvas) {
			memdelete(ctx.canvas);
		}
		if (ctx.framebuffer_cache) {
			memdelete(ctx.framebuffer_cache);
		}
		if (ctx.uniform_set_cache) {
			memdelete(ctx.uniform_set_cache);
		}

		unbind_gpu_context();
	}
	gpu_contexts.clear();

	singleton = nullptr;
	memdelete(uniform_set_cache);
	memdelete(framebuffer_cache);
	ShaderRD::set_shader_cache_user_dir(String());
	ShaderRD::set_shader_cache_res_dir(String());
}
