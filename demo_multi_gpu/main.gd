extends Node

func _ready():
    var vp1 = $UI/HBoxContainer/VBoxContainer1/SubViewportContainer1/SubViewport1
    
    # Enable GPU 1 on the second viewport
    if vp1.has_method("set_gpu_index"):
        vp1.set_gpu_index(1)
        print("Set GPU 1 on SubViewport1")
    else:
        print("ERROR: set_gpu_index not available on SubViewport")
    
    # We must explicitly create GPU 1 resources right now since _enter_tree
    # creates them on GPU 0 before the instance knows its viewport GPU index.
    if RenderingServer.has_method("set_active_gpu"):
        # Switch to GPU 1
        RenderingServer.set_active_gpu(1)
        print("Creating mesh on GPU 1...")
        
        # Create a mesh for GPU 1
        var mesh1 = BoxMesh.new()
        var mat1 = StandardMaterial3D.new()
        mat1.albedo_color = Color(0, 0, 1) # Blue box for GPU 1
        mesh1.material = mat1
        
        var instance1 = MeshInstance3D.new()
        instance1.mesh = mesh1
        vp1.add_child(instance1)
        
        # Switch back to GPU 0
        RenderingServer.set_active_gpu(0)
        
        # Create a mesh for GPU 0
        var vp0 = $UI/HBoxContainer/VBoxContainer0/SubViewportContainer0/SubViewport0
        var mesh0 = BoxMesh.new()
        var mat0 = StandardMaterial3D.new()
        mat0.albedo_color = Color(1, 0, 0) # Red box for GPU 0
        mesh0.material = mat0
        
        var instance0 = MeshInstance3D.new()
        instance0.mesh = mesh0
        vp0.add_child(instance0)
        
        print("Meshes created.")
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

