#pragma once
#include "drivers/vulkan/godot_vulkan.h"


//MAKE THIS A SINGLETON INSTANCED ON register_types.cpp
class VRAMBridge
{
	//TODO: MAKE THIS MANAGE VULKAN OBJECTS AND SUBMIT CALLS !!
public:

	//Create an external resource managed by this singleton
	RID create_external_resource();

	//Add methods to setup the queue for submitting. create a server pattern, each External Buffer should have an RID that can be used to acces the dispatching of copies

	// WE MAKE A SENDER AND A RECEIVER. RECEIVER WILL HAVE A STATE:
	// HERE THE RID OF THE RECEIVER CAN BE ASSOCIATED WITH THE RID OF THE COPIED BUFFER
	//
	/* SENDER STATES
	enum class State : int {
		IDLE = 0, // nothing happening -> Here RECEIVER is FINISHED, we can DISPATCH again !
		DISPATCHED = 1, // compute in DAG, waiting one frame before copy -> This should trigger the dispatch cycle on the next frame update
		COPY_PENDING = 2, // copy submitted -> semaphore signalled, RECEIVER reading
	};
	*/

	// Copies an godot-managed resource into the given external resource 
	//------------------------------- ext rest must be an external resource
	void export_resource(RID gd_resource, RID external_resource);

private:
	void _submit_copy_with_semaphores();
};
