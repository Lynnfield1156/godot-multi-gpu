extends Node3D

## Multi-GPU Viewport Demo
##
## Left viewport: GPU 0 with standard scene tree content (cube + light).
## Right viewport: GPU 1 with camera only (renders clear color).
##
## GPU 1 scene content must be created via RenderingServer API with
## set_active_gpu(1) active. Scene tree nodes create their GPU resources
## (meshes, materials, lights) on whichever GPU context is active during
## construction — which is GPU 0 by default. The shared RendererSceneCull
## runs update_dirty_instances() on GPU 0, so GPU 1 RIDs would be
## unreachable. Separate Scenarios (own_world_3d=true) isolate the
## instance lists per viewport.

@onready var viewport1: SubViewport = $ViewportContainer1/SubViewport
@onready var viewport2: SubViewport = $ViewportContainer2/SubViewport
@onready var fps_label: Label = $UI/Panel/FPSLabel
@onready var gpu_label: Label = $UI/Panel/GPULabel

func _ready() -> void:
	print("=== Multi-GPU Viewport Demo ===")

	var main_rd := RenderingServer.get_rendering_device()
	if main_rd == null:
		push_error("No RenderingDevice available")
		return

	print("Main GPU: %s" % main_rd.get_device_name())

	viewport1.set_gpu_index(0)
	viewport2.set_gpu_index(1)

	_update_gpu_info()

	print("Left: GPU 0 (scene tree) | Right: GPU 1 (clear color)")

func _process(_delta: float) -> void:
	if fps_label:
		fps_label.text = "FPS: %d" % Engine.get_frames_per_second()

func _update_gpu_info() -> void:
	var main_rd := RenderingServer.get_rendering_device()
	if main_rd == null:
		return

	var info := "GPU 0 (Left): %s\n" % main_rd.get_device_name()
	info += "GPU 1 (Right): "

	var secondary_rd := main_rd.create_local_device(1)
	if secondary_rd:
		info += secondary_rd.get_device_name()
	else:
		info += "Not available (using GPU 0)"

	if gpu_label:
		gpu_label.text = info
