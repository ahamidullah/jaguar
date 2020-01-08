// This file was auto-generated by material_compiler.c.

void Create_Material_Pipelines(Render_API_Context *context, GPU_Descriptor_Sets *descriptor_sets, GPU_Shader shaders[GPU_SHADER_COUNT], GPU_Render_Pass scene_render_pass, GPU_Pipeline *pipelines) {
	{
		GPU_Pipeline_Description pipeline_description = {
			.descriptor_set_layout_count = RUSTED_IRON_DESCRIPTOR_SET_LAYOUT_COUNT,
			.descriptor_set_layouts = descriptor_sets->layouts.rusted_iron,
			.push_constant_count = 0,
			.push_constant_descriptions = NULL,
			.topology = GPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			.viewport_width = window_width,
			.viewport_height = window_width,
			.scissor_width = window_width,
			.scissor_height = window_width,
			.depth_compare_operation = VK_COMPARE_OP_LESS,
			.framebuffer_attachment_color_blend_count = 1,
			.framebuffer_attachment_color_blend_descriptions = &(GPU_Framebuffer_Attachment_Color_Blend_Description){
				.color_write_mask = GPU_COLOR_COMPONENT_RED | GPU_COLOR_COMPONENT_GREEN | GPU_COLOR_COMPONENT_BLUE | GPU_COLOR_COMPONENT_ALPHA,
				.enable_blend = true,
				.source_color_blend_factor = GPU_BLEND_FACTOR_SRC_ALPHA,
				.destination_color_blend_factor = GPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
				//.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
				//.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
				.color_blend_operation = GPU_BLEND_OP_ADD ,
				.source_alpha_blend_factor = GPU_BLEND_FACTOR_ONE,
				.destination_alpha_blend_factor = GPU_BLEND_FACTOR_ZERO,
				.alpha_blend_operation = VK_BLEND_OP_ADD,
			},
			.vertex_input_attribute_count = 1,
			.vertex_input_attribute_descriptions = (GPU_Pipeline_Vertex_Input_Attribute_Description[]){
				{
					.binding = GPU_VERTEX_BUFFER_BIND_ID,
					.location = 0,
					.format = GPU_FORMAT_R32G32B32_SFLOAT,
					.offset = 0,
				},
			},
			.vertex_input_binding_count = 1,
			.vertex_input_binding_descriptions = (GPU_Pipeline_Vertex_Input_Binding_Description[]){
				{
					.binding = GPU_VERTEX_BUFFER_BIND_ID,
					.stride = sizeof(V3),
					.input_rate = GPU_VERTEX_INPUT_RATE_VERTEX,
				},
			},
			.dynamic_state_count = 2,
			.dynamic_states = (GPU_Dynamic_Pipeline_State[]){
				GPU_DYNAMIC_PIPELINE_STATE_VIEWPORT,
				GPU_DYNAMIC_PIPELINE_STATE_SCISSOR,
			},
			.shader = shaders[RUSTED_IRON_SHADER],
			.render_pass = scene_render_pass,
			.enable_depth_bias = false,
		};
		pipelines[RUSTED_IRON_SHADER] = Render_API_Create_Pipeline(context, &pipeline_description);
	}
}
