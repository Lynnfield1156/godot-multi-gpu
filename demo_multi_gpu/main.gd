extends Node3D

## Multi-GPU Demo Scene
## Shows GPU 0 rendering on left, GPU 1 rendering on right

@onready var display_quad: MeshInstance3D = $DisplayQuad
@onready var fps_label: Label = $UI/Panel/FPSLabel
@onready var gpu_label: Label = $UI/Panel/GPULabel

var secondary_rd: RenderingDevice = null
var secondary_color_texture: RID = RID()
var secondary_depth_texture: RID = RID()
var secondary_framebuffer: RID = RID()
var secondary_pipeline: RID = RID()
var secondary_shader: RID = RID()
var pos_buffer: RID = RID()
var color_buffer: RID = RID()
var vertex_array: RID = RID()

var display_texture: ImageTexture
var is_initialized: bool = false

const TEXTURE_SIZE := 512

func _ready() -> void:
	print("=== Multi-GPU Demo Scene ===")
	
	if not _init_secondary_gpu():
		push_error("Failed to initialize secondary GPU")
		return
	
	display_texture = ImageTexture.create_from_image(
		Image.create(TEXTURE_SIZE, TEXTURE_SIZE, false, Image.FORMAT_RGBA8)
	)
	
	var material := StandardMaterial3D.new()
	material.albedo_texture = display_texture
	material.unshaded = true
	display_quad.material_override = material
	
	_update_gpu_info()
	
	is_initialized = true
	print("Multi-GPU Demo initialized successfully!")

func _init_secondary_gpu() -> bool:
	var main_rd := RenderingServer.get_rendering_device()
	if main_rd == null:
		push_error("No main RenderingDevice")
		return false
	
	print("Main GPU: %s" % main_rd.get_device_name())
	
	secondary_rd = main_rd.create_local_device(1)
	
	if secondary_rd == null:
		push_warning("Secondary GPU not available, using main GPU")
		secondary_rd = main_rd
	else:
		print("Secondary GPU: %s" % secondary_rd.get_device_name())
	
	return _setup_secondary_resources()

func _setup_secondary_resources() -> bool:
	var format := RDTextureFormat.new()
	format.width = TEXTURE_SIZE
	format.height = TEXTURE_SIZE
	format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	format.usage_bits = (
		RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
		RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT |
		RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT |
		RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT
	)
	
	var view := RDTextureView.new()
	secondary_color_texture = secondary_rd.texture_create(format, view, [])
	if not secondary_color_texture.is_valid():
		push_error("Failed to create color texture")
		return false
	print("Color texture created: %s" % secondary_color_texture)
	
	var depth_format := RDTextureFormat.new()
	depth_format.width = TEXTURE_SIZE
	depth_format.height = TEXTURE_SIZE
	depth_format.format = RenderingDevice.DATA_FORMAT_D32_SFLOAT
	depth_format.usage_bits = (
		RenderingDevice.TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	)
	
	secondary_depth_texture = secondary_rd.texture_create(depth_format, view, [])
	if not secondary_depth_texture.is_valid():
		push_error("Failed to create depth texture")
		return false
	print("Depth texture created: %s" % secondary_depth_texture)
	
	secondary_framebuffer = secondary_rd.framebuffer_create([secondary_color_texture, secondary_depth_texture])
	if not secondary_framebuffer.is_valid():
		push_error("Failed to create framebuffer")
		return false
	print("Framebuffer created: %s" % secondary_framebuffer)
	
	if not _create_shader():
		return false
	
	if not _create_pipeline():
		return false
	
	return true

func _create_shader() -> bool:
	var shader_source := RDShaderSource.new()
	shader_source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	
	shader_source.set_stage_source(RenderingDevice.SHADER_STAGE_VERTEX, """
#version 450
layout(location = 0) in vec3 vertex;
layout(location = 1) in vec3 color;
layout(location = 0) out vec3 v_color;
void main() {
	gl_Position = vec4(vertex.xy, 0.0, 1.0);
	v_color = color;
}
""")
	
	shader_source.set_stage_source(RenderingDevice.SHADER_STAGE_FRAGMENT, """
#version 450
layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 frag_color;
void main() {
	frag_color = vec4(v_color, 1.0);
}
""")
	
	print("Compiling shader...")
	var shader_spirv: RDShaderSPIRV = secondary_rd.shader_compile_spirv_from_source(shader_source)
	
	var vertex_error: String = shader_spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_VERTEX)
	var fragment_error: String = shader_spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_FRAGMENT)
	
	if not vertex_error.is_empty():
		push_error("Vertex shader error: " + vertex_error)
		return false
	if not fragment_error.is_empty():
		push_error("Fragment shader error: " + fragment_error)
		return false
	
	print("Creating shader from SPIRV...")
	secondary_shader = secondary_rd.shader_create_from_spirv(shader_spirv)
	if not secondary_shader.is_valid():
		push_error("Failed to create shader")
		return false
	print("Shader created: %s" % secondary_shader)
	
	return true

func _create_pipeline() -> bool:
	# Position buffer (x, y, z per vertex)
	var positions := PackedFloat32Array([
		-0.8, -0.8, 0.0,
		 0.8, -0.8, 0.0,
		-0.8,  0.8, 0.0,
		 0.8, -0.8, 0.0,
		 0.8,  0.8, 0.0,
		-0.8,  0.8, 0.0,
	])
	pos_buffer = secondary_rd.vertex_buffer_create(positions.to_byte_array().size(), positions.to_byte_array())
	if not pos_buffer.is_valid():
		push_error("Failed to create position buffer")
		return false
	print("Position buffer created: %s" % pos_buffer)
	
	# Color buffer (r, g, b per vertex)
	var colors := PackedFloat32Array([
		1.0, 0.2, 0.2,
		0.2, 1.0, 0.2,
		0.2, 0.2, 1.0,
		0.2, 1.0, 0.2,
		1.0, 1.0, 0.2,
		0.2, 0.2, 1.0,
	])
	color_buffer = secondary_rd.vertex_buffer_create(colors.to_byte_array().size(), colors.to_byte_array())
	if not color_buffer.is_valid():
		push_error("Failed to create color buffer")
		return false
	print("Color buffer created: %s" % color_buffer)
	
	# Create vertex attributes
	var pos_attr := RDVertexAttribute.new()
	pos_attr.location = 0
	pos_attr.format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	pos_attr.stride = 12
	
	var color_attr := RDVertexAttribute.new()
	color_attr.location = 1
	color_attr.format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	color_attr.stride = 12
	
	var vertex_format_id := secondary_rd.vertex_format_create([pos_attr, color_attr])
	print("Vertex format ID: %d" % vertex_format_id)
	
	# Two buffers: [position, color]
	vertex_array = secondary_rd.vertex_array_create(6, vertex_format_id, [pos_buffer, color_buffer])
	if not vertex_array.is_valid():
		push_error("Failed to create vertex array")
		return false
	print("Vertex array created: %s" % vertex_array)
	
	var raster_state := RDPipelineRasterizationState.new()
	raster_state.cull_mode = RenderingDevice.POLYGON_CULL_DISABLED
	
	var multisample := RDPipelineMultisampleState.new()
	
	var depth_stencil := RDPipelineDepthStencilState.new()
	depth_stencil.enable_depth_test = true
	depth_stencil.enable_depth_write = true
	depth_stencil.depth_compare_operator = RenderingDevice.COMPARE_OP_LESS
	
	var blend_state := RDPipelineColorBlendState.new()
	var attachment := RDPipelineColorBlendStateAttachment.new()
	blend_state.attachments.push_back(attachment)
	
	var fb_format := secondary_rd.framebuffer_get_format(secondary_framebuffer)
	print("Framebuffer format: %d" % fb_format)
	
	print("Creating render pipeline...")
	secondary_pipeline = secondary_rd.render_pipeline_create(
		secondary_shader,
		fb_format,
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
	print("Render pipeline created: %s" % secondary_pipeline)
	
	return true

func _process(_delta: float) -> void:
	if not is_initialized:
		return
	
	_render_on_secondary()
	_transfer_and_display()
	
	if fps_label:
		fps_label.text = "FPS: %d" % Engine.get_frames_per_second()

func _render_on_secondary() -> void:
	if not secondary_framebuffer.is_valid():
		return
	if not secondary_pipeline.is_valid():
		return
	if not vertex_array.is_valid():
		return
	
	var clear_color := Color(0.05, 0.05, 0.15, 1.0)
	var draw_list := secondary_rd.draw_list_begin(
		secondary_framebuffer,
		RenderingDevice.DRAW_CLEAR_COLOR_ALL | RenderingDevice.DRAW_CLEAR_DEPTH,
		[clear_color],
		1.0, 0
	)
	
	secondary_rd.draw_list_bind_render_pipeline(draw_list, secondary_pipeline)
	secondary_rd.draw_list_bind_vertex_array(draw_list, vertex_array)
	secondary_rd.draw_list_draw(draw_list, false, 1)
	secondary_rd.draw_list_end()
	
	secondary_rd.submit()
	secondary_rd.sync()

func _transfer_and_display() -> void:
	if not secondary_color_texture.is_valid():
		return
	if display_texture == null:
		return
	
	var texture_data: PackedByteArray = secondary_rd.texture_get_data(secondary_color_texture, 0)
	if texture_data.is_empty():
		return
	
	var image := Image.create_from_data(
		TEXTURE_SIZE, TEXTURE_SIZE,
		false, Image.FORMAT_RGBA8,
		texture_data
	)
	
	display_texture.update(image)

func _update_gpu_info() -> void:
	var main_rd := RenderingServer.get_rendering_device()
	if main_rd == null:
		return
	
	var info := "Main GPU: %s\n" % main_rd.get_device_name()
	
	if secondary_rd and secondary_rd != main_rd:
		info += "Secondary GPU: %s" % secondary_rd.get_device_name()
	else:
		info += "Secondary GPU: Same as main"
	
	if gpu_label:
		gpu_label.text = info

func _exit_tree() -> void:
	if secondary_rd:
		if pos_buffer.is_valid():
			secondary_rd.free_rid(pos_buffer)
		if color_buffer.is_valid():
			secondary_rd.free_rid(color_buffer)
		if vertex_array.is_valid():
			secondary_rd.free_rid(vertex_array)
		if secondary_color_texture.is_valid():
			secondary_rd.free_rid(secondary_color_texture)
		if secondary_depth_texture.is_valid():
			secondary_rd.free_rid(secondary_depth_texture)
		if secondary_framebuffer.is_valid():
			secondary_rd.free_rid(secondary_framebuffer)
		if secondary_shader.is_valid():
			secondary_rd.free_rid(secondary_shader)
		if secondary_pipeline.is_valid():
			secondary_rd.free_rid(secondary_pipeline)
