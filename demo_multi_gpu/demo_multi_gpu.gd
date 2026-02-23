@tool
extends EditorScript
class_name DemoMultiGPU

## Multi-GPU Rendering Demo
## GPU 0: Main rendering (left half of screen)
## GPU 1: Secondary rendering (right half of screen)

var secondary_rd: RenderingDevice
var secondary_framebuffer: RID
var secondary_color_texture: RID
var secondary_depth_texture: RID
var secondary_shader: RID
var secondary_pipeline: RID
var secondary_vertex_buffer: RID
var secondary_vertex_array: RID
var secondary_uniform_set: RID

var main_rd: RenderingDevice
var display_texture: RID
var display_shader_rid: RID
var display_pipeline: RID

var is_initialized: bool = false
var texture_size: Vector2i = Vector2i(512, 512)

func _run() -> void:
	print("=== Multi-GPU Demo ===")
	print("Initializing...")
	
	if not _init_multi_gpu():
		print("Failed to initialize multi-GPU")
		return
	
	print("Multi-GPU initialized successfully!")
	print("GPU 0: Main rendering")
	print("GPU 1: Secondary rendering (if available)")
	
	# Render one frame
	_render_frame()

func _init_multi_gpu() -> bool:
	main_rd = RenderingServer.get_rendering_device()
	if main_rd == null:
		push_error("Failed to get main RenderingDevice")
		return false
	
	print("Main GPU: %s" % main_rd.get_device_name())
	
	# Try to create secondary GPU device
	secondary_rd = main_rd.create_local_device(1)
	if secondary_rd == null:
		push_warning("Secondary GPU not available, using main GPU for both")
		secondary_rd = main_rd
	
	print("Secondary GPU: %s" % secondary_rd.get_device_name())
	
	# Setup secondary GPU resources
	if not _setup_secondary_gpu():
		return false
	
	# Setup display resources on main GPU
	if not _setup_display():
		return false
	
	is_initialized = true
	return true

func _setup_secondary_gpu() -> bool:
	# Create color texture
	var format := RDTextureFormat.new()
	format.width = texture_size.x
	format.height = texture_size.y
	format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	
	var view := RDTextureView.new()
	secondary_color_texture = secondary_rd.texture_create(format, view, [])
	if not secondary_color_texture.is_valid():
		push_error("Failed to create secondary color texture")
		return false
	
	# Create depth texture
	var depth_format := RDTextureFormat.new()
	depth_format.width = texture_size.x
	depth_format.height = texture_size.y
	depth_format.format = RenderingDevice.DATA_FORMAT_D32_SFLOAT
	depth_format.usage_bits = RenderingDevice.TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	
	secondary_depth_texture = secondary_rd.texture_create(depth_format, view, [])
	if not secondary_depth_texture.is_valid():
		push_error("Failed to create secondary depth texture")
		return false
	
	# Create framebuffer
	var fb_format_id := secondary_rd.framebuffer_format_create([
		_render_target_format(RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM)
	])
	secondary_framebuffer = secondary_rd.framebuffer_create([secondary_color_texture], fb_format_id)
	if not secondary_framebuffer.is_valid():
		push_error("Failed to create secondary framebuffer")
		return false
	
	# Create simple triangle shader
	var shader_source := RDShaderSource.new()
	shader_source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	
	# Vertex shader
	shader_source.set_stage_source(RenderingDevice.SHADER_STAGE_VERTEX, """
#version 450
layout(location = 0) in vec3 vertex;
layout(location = 1) in vec3 color;
layout(location = 0) out vec3 v_color;
void main() {
	gl_Position = vec4(vertex, 1.0);
	v_color = color;
}
""")
	
	# Fragment shader
	shader_source.set_stage_source(RenderingDevice.SHADER_STAGE_FRAGMENT, """
#version 450
layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = vec4(v_color, 1.0);
}
""")
	
	var shader_spirv: RDShaderSPIRV = secondary_rd.shader_compile_spirv_from_source(shader_source)
	if shader_spirv.compile_error != "":
		push_error("Shader compile error: " + shader_spirv.compile_error)
		return false
	
	secondary_shader = secondary_rd.shader_create_from_spirv(shader_spirv)
	if not secondary_shader.is_valid():
		push_error("Failed to create shader")
		return false
	
	# Create vertex buffer (colorful triangle)
	var vertices := PackedFloat32Array([
		# Position (x, y, z), Color (r, g, b)
		-0.8, -0.8, 0.0,  1.0, 0.0, 0.0,  # Red
		 0.8, -0.8, 0.0,  0.0, 1.0, 0.0,  # Green
		 0.0,  0.8, 0.0,  0.0, 0.0, 1.0,  # Blue
	])
	var vertex_data := vertices.to_byte_array()
	secondary_vertex_buffer = secondary_rd.vertex_buffer_create(vertex_data.size(), vertex_data)
	if not secondary_vertex_buffer.is_valid():
		push_error("Failed to create vertex buffer")
		return false
	
	# Create vertex format
	var vertex_format := RDVertexFormat.new()
	var attr := RDVertexAttribute.new()
	attr.location = 0
	attr.format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	attr.stride = 24  # 6 floats = 24 bytes
	vertex_format.attributes.push_back(attr)
	
	var color_attr := RDVertexAttribute.new()
	color_attr.location = 1
	color_attr.format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	color_attr.stride = 24
	color_attr.offset = 12  # After position
	vertex_format.attributes.push_back(color_attr)
	
	var vertex_format_id := secondary_rd.vertex_format_create(vertex_format)
	secondary_vertex_array = secondary_rd.vertex_array_create(3, vertex_format_id, [secondary_vertex_buffer])
	if not secondary_vertex_array.is_valid():
		push_error("Failed to create vertex array")
		return false
	
	# Create pipeline
	var raster_state := RDPipelineRasterizationState.new()
	raster_state.cull_mode = RenderingDevice.POLYGON_CULL_NONE
	
	var multisample := RDPipelineMultisampleState.new()
	
	var depth_stencil := RDPipelineDepthStencilState.new()
	depth_stencil.enable_depth_test = true
	depth_stencil.enable_depth_write = true
	depth_stencil.depth_compare_operator = RenderingDevice.COMPARE_OP_LESS
	
	var blend_state := RDPipelineColorBlendState.new()
	var attachment := RDPipelineColorBlendStateAttachment.new()
	blend_state.attachments.push_back(attachment)
	
	secondary_pipeline = secondary_rd.render_pipeline_create(
		secondary_shader,
		secondary_rd.framebuffer_get_format(secondary_framebuffer),
		vertex_format_id,
		RenderingDevice.RENDER_PRIMITIVE_TRIANGLES,
		raster_state,
		multisample,
		depth_stencil,
		blend_state
	)
	if not secondary_pipeline.is_valid():
		push_error("Failed to create render pipeline")
		return false
	
	return true

func _setup_display() -> bool:
	# Create texture to hold secondary GPU output (on main GPU)
	var format := RDTextureFormat.new()
	format.width = texture_size.x
	format.height = texture_size.y
	format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_FROM_BIT
	
	var view := RDTextureView.new()
	display_texture = main_rd.texture_create(format, view, [])
	if not display_texture.is_valid():
		push_error("Failed to create display texture")
		return false
	
	return true

func _render_target_format(format: int) -> RDAttachmentFormat:
	var af := RDAttachmentFormat.new()
	af.format = format
	af.samples = RenderingDevice.TEXTURE_SAMPLES_1
	return af

func _render_frame() -> void:
	if not is_initialized:
		return
	
	# === Render on Secondary GPU ===
	_render_on_secondary()
	
	# === Transfer to Main GPU ===
	_transfer_to_main_gpu()
	
	# === Display on Main GPU ===
	# (This would be done by a regular Godot node in a real scenario)
	print("Frame rendered!")

func _render_on_secondary() -> void:
	var draw_list := secondary_rd.draw_list_begin(secondary_framebuffer, 
		RenderingDevice.DRAW_CLEAR_COLOR_ALL | RenderingDevice.DRAW_CLEAR_DEPTH,
		[Color(0.1, 0.1, 0.2, 1.0)], 1.0, 0)
	
	secondary_rd.draw_list_bind_render_pipeline(draw_list, secondary_pipeline)
	secondary_rd.draw_list_bind_vertex_array(draw_list, secondary_vertex_array)
	secondary_rd.draw_list_draw(draw_list, false, 1)
	secondary_rd.draw_list_end()
	
	# Submit and sync
	secondary_rd.submit()
	secondary_rd.sync()

func _transfer_to_main_gpu() -> void:
	# Read texture from secondary GPU
	var texture_data: PackedByteArray = secondary_rd.texture_get_data(secondary_color_texture, 0)
	
	# Update texture on main GPU
	if main_rd != secondary_rd:
		main_rd.texture_update(display_texture, 0, texture_data)
	
	print("Transferred %d bytes from secondary GPU to main GPU" % texture_data.size())

func _cleanup() -> void:
	if secondary_rd and secondary_rd != main_rd:
		if secondary_framebuffer.is_valid():
			secondary_rd.free_rid(secondary_framebuffer)
		if secondary_color_texture.is_valid():
			secondary_rd.free_rid(secondary_color_texture)
		if secondary_depth_texture.is_valid():
			secondary_rd.free_rid(secondary_depth_texture)
		if secondary_shader.is_valid():
			secondary_rd.free_rid(secondary_shader)
		if secondary_pipeline.is_valid():
			secondary_rd.free_rid(secondary_pipeline)
		if secondary_vertex_buffer.is_valid():
			secondary_rd.free_rid(secondary_vertex_buffer)
		if secondary_vertex_array.is_valid():
			secondary_rd.free_rid(secondary_vertex_array)
		# Note: secondary_rd should be freed manually when done
	
	if main_rd:
		if display_texture.is_valid():
			main_rd.free_rid(display_texture)
		if display_shader_rid.is_valid():
			main_rd.free_rid(display_shader_rid)
		if display_pipeline.is_valid():
			main_rd.free_rid(display_pipeline)
