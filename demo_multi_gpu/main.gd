extends Node

func _ready():
    var vp1 = $UI/HBoxContainer/VBoxContainer1/SubViewportContainer1/SubViewport1
    
    # Enable GPU 1 on the second viewport
    if vp1.has_method("set_gpu_index"):
        vp1.set_gpu_index(1)
        print("Set GPU 1 on SubViewport1")
    else:
        print("ERROR: set_gpu_index not available on SubViewport")
    
    if RenderingServer.has_method("set_active_gpu"):
        # Switch to GPU 1
        RenderingServer.set_active_gpu(1)
        
        # Create a new World3D for GPU 1 so its Scenario is created on GPU 1
        var world1 = World3D.new()
        vp1.world_3d = world1
        
        # Create a mesh for GPU 1
        var mesh1 = BoxMesh.new()
        var shader1 = Shader.new()
        shader1.code = """
        shader_type spatial;
        void fragment() {
            ALBEDO = vec3(0.0, 0.0, 1.0); // Blue
        }
        """
        var mat1 = ShaderMaterial.new()
        mat1.shader = shader1
        mesh1.material = mat1
        var instance1 = MeshInstance3D.new()
        instance1.mesh = mesh1
        vp1.add_child(instance1)

        # Create Canvas 2D for GPU 1 dynamically
        var rect1 = ColorRect.new()
        rect1.color = Color(0, 0, 1, 0.5)
        rect1.size = Vector2(100, 40)
        var label1 = Label.new()
        label1.text = "GPU 1 2D"
        rect1.add_child(label1)
        vp1.get_node("CanvasLayer").add_child(rect1)
        
        # Switch back to GPU 0
        RenderingServer.set_active_gpu(0)
        
        # Create a mesh for GPU 0
        var vp0 = $UI/HBoxContainer/VBoxContainer0/SubViewportContainer0/SubViewport0
        var mesh0 = BoxMesh.new()
        var shader0 = Shader.new()
        shader0.code = """
        shader_type spatial;
        void fragment() {
            ALBEDO = vec3(1.0, 0.0, 0.0); // Red
        }
        """
        var mat0 = ShaderMaterial.new()
        mat0.shader = shader0
        mesh0.material = mat0
        var instance0 = MeshInstance3D.new()
        instance0.mesh = mesh0
        vp0.add_child(instance0)

        # Create Canvas 2D for GPU 0 dynamically
        var rect0 = ColorRect.new()
        rect0.color = Color(1, 0, 0, 0.5)
        rect0.size = Vector2(100, 40)
        var label0 = Label.new()
        label0.text = "GPU 0 2D"
        rect0.add_child(label0)
        vp0.get_node("CanvasLayer").add_child(rect0)
        
        print("Meshes and CanvasItems created.")
    else:
        print("ERROR: set_active_gpu not available in RenderingServer")

func _process(delta):
    var vp0 = $UI/HBoxContainer/VBoxContainer0/SubViewportContainer0/SubViewport0
    var vp1 = $UI/HBoxContainer/VBoxContainer1/SubViewportContainer1/SubViewport1
    
    for child in vp0.get_children():
        if child is MeshInstance3D:
            child.rotate_y(delta)
            
    for child in vp1.get_children():
        if child is MeshInstance3D:
            child.rotate_y(-delta)

