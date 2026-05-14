#include "vram_bridge.h"
#include "core/object/object.h"

void VRAMBridge::export_resource(RID gd_resource, RID external_resource)
{

}

//
// submit_copy_with_semaphores
//
// Called from _render_callback when state == DISPATCHED (i.e. one frame after
// the compute dispatch was recorded).  Godot has already submitted frame N's
// command buffer.  Our submit lands after it on the same queue.
//
// Wait on:   vk_ready  (CUDA signals this when done reading — Vulkan waits
//                       before overwriting the external buffer)
// Signal:    vk_done   (tells CUDA the buffer has new data, safe to read)
//
void VRAMBridge::_submit_copy_with_semaphores() //ITS WAAAAY EASIER TO USE TIMELINE SEMAPHORES HERE. JUST DO IT !
{

}
