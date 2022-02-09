/*
 * Raw Depth Buffer Export Add-on for Reshade 5.0: https://github.com/crosire/reshade
 */

#define ImTextureID unsigned long long

#define STB_IMAGE_WRITE_IMPLEMENTATION

#define TINYEXR_IMPLEMENTATION

#include <imgui.h>
#include <reshade.hpp>
#include <vector>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include "FormatEnum.h"
#include <filesystem>
#include <stb_image_write.h>
#include "stb_image.h"

#include "tinyexr.h"
#include "miniz.c"
#include "miniz.h"

static bool undock_window_var = false;

using namespace reshade::api;

struct draw_stats
{
	uint32_t vertices = 0;
	uint32_t drawcalls = 0;
	uint32_t drawcalls_indirect = 0;
	viewport last_viewport = {};
};
struct clear_stats : public draw_stats
{
	bool rect = false;
};

struct depth_stencil_info
{
	draw_stats total_stats;
	draw_stats current_stats; // Stats since last clear operation
	std::vector<clear_stats> clears;
	bool copied_during_frame = false;
};

struct depth_stencil_hash
{
	inline size_t operator()(resource value) const
	{
		// Simply use the handle (which is usually a pointer) as hash value (with some bits shaved off due to pointer alignment)
		return static_cast<size_t>(value.handle >> 4);
	}
};

struct __declspec(uuid("7c6363c7-f94e-437a-9160-141782c44a98")) state_tracking_inst
{
	// The depth-stencil that is currently selected as being the main depth target
	resource selected_depth_stencil = { 0 };

	// Resource used to override automatic depth-stencil selection
	resource override_depth_stencil = { 0 };

	// The current shader resource view bound to shaders
	// This can be created from either the original depth-stencil of the application (if it supports shader access) or from a backup resource
	resource_view selected_shader_resource = { 0 };

	// True when the shader resource view was created from the backup resource, false when it was created from the original depth-stencil
	bool using_backup_texture = false;

	std::unordered_map<resource, unsigned int, depth_stencil_hash> display_count_per_depth_stencil;
};

struct __declspec(uuid("eadae23a-4009-4d32-8557-0af07e45f409")) depth_texture_inst
{
	resource depth_texture_r = { 0 };
	resource_desc depth_texture_rd;
	resource_view depth_texture_rv = { 0 };
	void update(resource sr, resource_desc srd, resource_view srv)
	{
		depth_texture_r = sr;
		depth_texture_rd = srd;
		depth_texture_rv = srv;
	}
	void reset()
	{
		depth_texture_r = { 0 };
		depth_texture_rv = { 0 };
	}
};

static void on_init_effect_runtime(effect_runtime *runtime)
{
	runtime->create_private_data<depth_texture_inst>();
}

static void on_destroy_effect_runtime(effect_runtime *runtime)
{
	device *const device = runtime->get_device();

	depth_texture_inst &dmi = runtime->get_private_data<depth_texture_inst>();

	if (dmi.depth_texture_rv != 0)
		device->destroy_resource_view(dmi.depth_texture_rv);

	runtime->destroy_private_data<depth_texture_inst>();
}

static void on_begin_render_effects(effect_runtime *runtime, command_list *cmd_list, resource_view, resource_view)
{
	depth_texture_inst &dmi = runtime->get_private_data<depth_texture_inst>();
	device *const device = runtime->get_device();

	runtime->enumerate_texture_variables(nullptr, [&dmi, &device](effect_runtime *runtime, auto variable) {
		char source[32] = "";
		runtime->get_texture_variable_name(variable, source);
		if (std::strcmp(source, "DepthToAddonTex") == 0) {
			resource_view srv = { 0 };
			runtime->get_texture_binding(variable, &srv);
			if (srv != 0) {
				dmi.update(device->get_resource_from_view(srv), device->get_resource_desc(device->get_resource_from_view(srv)), srv);
			}
			else
			{
				dmi.reset();
			}
		}
	});
}

bool SaveEXR(const float *rgb, int width, int height, const char *outfilename) {

	EXRHeader header;
	InitEXRHeader(&header);

	EXRImage image;
	InitEXRImage(&image);

	image.num_channels = 3;

	// Must be BGR(A) order, since most of EXR viewers expect this channel order.

	std::vector<float> images[3];
	images[0].resize(width * height);
	images[1].resize(width * height);
	images[2].resize(width * height);

	for (int i = 0; i < width * height; i++) {
		images[0][i] = rgb[3 * i + 0];
		images[1][i] = rgb[3 * i + 1];
		images[2][i] = rgb[3 * i + 2];
	}

	float *image_ptr[3];
	image_ptr[0] = &(images[0].at(0)); // B
	image_ptr[1] = &(images[1].at(0)); // G
	image_ptr[2] = &(images[2].at(0)); // R

	image.images = (unsigned char **)image_ptr;
	image.width = width;
	image.height = height;

	header.num_channels = 3;
	header.channels = (EXRChannelInfo *)malloc(sizeof(EXRChannelInfo) * header.num_channels);
	// Must be BGR(A) order, since most of EXR viewers expect this channel order.
	strncpy(header.channels[0].name, "R", 255); header.channels[0].name[strlen("R")] = '\0';
	strncpy(header.channels[1].name, "G", 255); header.channels[1].name[strlen("G")] = '\0';
	strncpy(header.channels[2].name, "B", 255); header.channels[2].name[strlen("B")] = '\0';

	header.pixel_types = (int *)malloc(sizeof(int) * header.num_channels);
	header.requested_pixel_types = (int *)malloc(sizeof(int) * header.num_channels);
	for (int i = 0; i < header.num_channels; i++) {
		header.pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT; // pixel type of input image
		header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_HALF; // pixel type of output image to be stored in .EXR
	}

	const char *err;
	int ret = SaveEXRImageToFile(&image, &header, outfilename, &err);
	if (ret != TINYEXR_SUCCESS) {
		return false;
	}

	//free(&rgb);

	free(header.channels);
	free(header.pixel_types);
	free(header.requested_pixel_types);

	reshade::log_message(3, "EXR export done!");

	return true;
}

bool capture_image(const resource_desc &desc, const subresource_data &data, std::filesystem::path save_path)
{
	float *data_p = static_cast<float *>(data.data);
	std::vector<float> rgba_pixel_data(desc.texture.width * desc.texture.height * 3);

	uint32_t channels = 4;

	for (uint32_t y = 0; y < desc.texture.height; ++y, data_p += data.row_pitch / channels) //data.row_pitch
	{
		for (uint32_t x = 0; x < desc.texture.width; ++x)
		{
			const float *const src = data_p + x * 4;
			float *const dst = rgba_pixel_data.data() + (y * desc.texture.width + x) * 3;

			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
		}
	}

	return SaveEXR(rgba_pixel_data.data(), desc.texture.width, desc.texture.height, save_path.u8string().c_str());

	/*float unpack = (256.0 * 256.0 * 256.0) / (256.0 * 256.0 * 256.0 - 1.0);
	float unpack32 = (256.0 * 256.0) / (256.0 * 256.0 - 1.0);
	float bit_depth = bit_depth_setting;
	float bit_depth_restore32 = pow(2.0, bit_depth * 2.0);*/

	/*switch (desc.texture.format)
	{
	case format::r24_g8_typeless:
		for (uint32_t y = 0; y < desc.texture.height; ++y, data_p += data.row_pitch)
		{
			for (uint32_t x = 0; x < desc.texture.width; ++x)
			{
				const uint8_t *const src = data_p + x * 4;
				float *const dst = rgba_pixel_data.data() + (y * desc.texture.width + x) * 3;

				float R, G, B;

				B = (src[0] * (1.0 / (256.0 * 256.0 * 256.0)));
				G = (src[1] * (1.0 / (256.0 * 256.0)));
				R = (src[2] * (1.0 / (256.0)));

				dst[0] = (reverse_depth ? 1.0 - (R + G + B) * unpack : (R + G + B) * unpack);
				dst[1] = dst[0];
				dst[2] = dst[0];
			}
		}
		break;
	case format::r32_g8_typeless:
		for (uint32_t y = 0; y < desc.texture.height; ++y, data_p += data.row_pitch)
		{
			for (uint32_t x = 0; x < desc.texture.width; ++x)
			{
				const uint8_t *const src = data_p + x * 8;
				float *const dst = rgba_pixel_data.data() + (y * desc.texture.width + x) * 3;

				float R, G, B, A;
				B = (src[0] * (1.0 / (256.0 * 256.0 * 256.0 * 256.0)));
				G = (src[1] * (1.0 / (256.0 * 256.0 * 256.0)));
				R = (src[2] * (1.0 / (256.0 * 256.0)));
				A = (src[3] * (1.0 / (256.0)));


				if (high_precision_export)
					dst[0] = pow((A + R + G + B), bit_depth) * bit_depth_restore32 * unpack32; // B
				else
					dst[0] = ((A + R + G + B) * unpack32); // B
				dst[0] = (reverse_depth ? 1.0 - (dst[0]) : (dst[0]));
				dst[1] = dst[0];  //G
				dst[2] = dst[0];  //R
			}
		}
		break;
	}*/

	//return SaveEXR(rgba_pixel_data.data(), desc.texture.width, desc.texture.height, save_path.u8string().c_str());
}

static bool saveDS(effect_runtime *runtime, std::filesystem::path save_path)
{
	depth_texture_inst &dmi = runtime->get_private_data<depth_texture_inst>();

	if (dmi.depth_texture_r != 0)
	{
		device *const device = runtime->get_device();
		command_queue *const queue = runtime->get_command_queue();
		resource_desc resource_desc = dmi.depth_texture_rd;



		uint32_t row_pitch = format_row_pitch(resource_desc.texture.format, resource_desc.texture.width);
		if (device->get_api() == device_api::d3d12) // Align row pitch to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256)
			row_pitch = (row_pitch + 255) & ~255;
		const uint32_t slice_pitch = format_slice_pitch(resource_desc.texture.format, row_pitch, resource_desc.texture.height);



		resource intermediate;
		if (resource_desc.heap != memory_heap::gpu_only)
		{
			// Avoid copying to temporary system memory resource if texture is accessible directly
			intermediate = dmi.depth_texture_r;

		}
		else if (device->check_capability(device_caps::copy_buffer_to_texture))
		{
			if ((resource_desc.usage & resource_usage::copy_source) != resource_usage::copy_source)
			{
				return false;
			}


			if (!device->create_resource(reshade::api::resource_desc(slice_pitch, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr, resource_usage::copy_dest, &intermediate))
			{
				reshade::log_message(1, "Failed to create system memory buffer for texture dumping!");
				return false;
			}

			command_list *const cmd_list = queue->get_immediate_command_list();
			cmd_list->barrier(dmi.depth_texture_r, resource_usage::shader_resource, resource_usage::copy_source);
			cmd_list->copy_texture_to_buffer(dmi.depth_texture_r, 0, nullptr, intermediate, 0, resource_desc.texture.width, resource_desc.texture.height);
			cmd_list->barrier(dmi.depth_texture_r, resource_usage::copy_source, resource_usage::shader_resource);
		}
		else
		{
			if ((resource_desc.usage & resource_usage::copy_source) != resource_usage::copy_source)
				return false;

			if (!device->create_resource(reshade::api::resource_desc(resource_desc.texture.width, resource_desc.texture.height, 1, 1, format_to_default_typed(resource_desc.texture.format), 1, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr, resource_usage::copy_dest, &intermediate))
			{
				reshade::log_message(1, "Failed to create system memory texture for texture dumping!");
				return false;
			}

			command_list *const cmd_list = queue->get_immediate_command_list();
			cmd_list->barrier(dmi.depth_texture_r, resource_usage::shader_resource, resource_usage::copy_source);
			cmd_list->copy_texture_region(dmi.depth_texture_r, 0, nullptr, intermediate, 0, nullptr);
			cmd_list->barrier(dmi.depth_texture_r, resource_usage::copy_source, resource_usage::shader_resource);
		}

		queue->wait_idle();

		subresource_data mapped_data = {};
		if (resource_desc.heap == memory_heap::gpu_only &&
			device->check_capability(device_caps::copy_buffer_to_texture))
		{
			device->map_buffer_region(intermediate, 0, std::numeric_limits<uint64_t>::max(), map_access::read_only, &mapped_data.data);

			mapped_data.row_pitch = row_pitch;
			mapped_data.slice_pitch = slice_pitch;
		}
		else
		{
			device->map_texture_region(intermediate, 0, nullptr, map_access::read_only, &mapped_data);
		}

		if (mapped_data.data != nullptr)
		{
			if (!capture_image(resource_desc, mapped_data, save_path))
				return false;

			if (resource_desc.heap == memory_heap::gpu_only &&
				device->check_capability(device_caps::copy_buffer_to_texture))
				device->unmap_buffer_region(intermediate);
			else
				device->unmap_texture_region(intermediate, 0);
		}

		if (intermediate != dmi.depth_texture_r) {
			device->destroy_resource(intermediate);
		}

		return true;
	}
	else
	{
		return false;
	}
}

static void drawItem(effect_runtime *runtime, resource_view srv, resource_desc srd, bool ib, resource_desc ird, char *source)
{
	const auto total_width = ImGui::GetWindowContentRegionWidth();
	const auto num_columns = static_cast<unsigned int>(1);
	const auto single_image_max_size = (total_width / num_columns);

	uint32_t frame_width, frame_height;

	frame_width = srd.texture.width;
	frame_height = srd.texture.height;

	const float aspect_ratio = static_cast<float>(frame_width) / static_cast<float>(frame_height);
	const ImVec2 size = aspect_ratio > 1 ? ImVec2(single_image_max_size, single_image_max_size / aspect_ratio) : ImVec2(single_image_max_size * aspect_ratio, single_image_max_size);
	int dt_id = static_cast<int>(srd.texture.format);
	int ds_id = 0;
	if (ib)
		ds_id = static_cast<int>(ird.texture.format);

	ImGui::BeginGroup();
	ImGui::Image(srv.handle, size);
	ImGui::BeginGroup();
	ImGui::Text("Depth texture name:");
	ImGui::Text("Depth texture format:");
	ImGui::Text("Depth texture size:");
	ImGui::Text("Depth buffer format:");
	ImGui::EndGroup();
	ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::Text(source);
	ImGui::Text("%ix%i", static_cast<int>(frame_width), static_cast<int>(frame_height));
	ImGui::Text(texture_format[dt_id]);
	ImGui::Text(texture_format[ds_id]);
	ImGui::EndGroup();
	ImGui::EndGroup();
}

static void displayDS(effect_runtime *runtime)
{
	state_tracking_inst &instance = runtime->get_private_data<state_tracking_inst>();

	depth_texture_inst &dmi = runtime->get_private_data<depth_texture_inst>();

	device *const device = runtime->get_device();
	command_queue *const queue = runtime->get_command_queue();
	command_list *const cmd_list = queue->get_immediate_command_list();

	runtime->enumerate_texture_variables(nullptr, [&instance, &device, &dmi](effect_runtime *runtime, auto variable) {
		char source[32] = "";
		runtime->get_texture_variable_name(variable, source);
		if (std::strcmp(source, "DepthToAddonTex") == 0) {
			resource sr = { 0 };
			resource_view srv = { 0 };
			resource_desc ird;
			resource_desc srd;
			runtime->get_texture_binding(variable, &srv);
			if (srv != 0) {
				bool ib = false;
				if (instance.selected_depth_stencil != 0) {
					ird = device->get_resource_desc(instance.selected_depth_stencil);
					ib = true;
				}
				sr = device->get_resource_from_view(srv);
				srd = device->get_resource_desc(sr);
				drawItem(runtime, srv, srd, ib, ird, source);
			}
			else
			{
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(1.0, 0.2, 0.2, 1.0), "Depth texture not found. Is DepthToAddon.fx enabled?");
			}
		}
	});
}

static void on_reshade_present(effect_runtime *runtime)
{
	if (runtime->is_key_pressed(0x79))
	{
		uint32_t width, height;
		device *const device = runtime->get_device();
		resource backBuffer = runtime->get_current_back_buffer();
		resource_desc resource_desc = device->get_resource_desc(backBuffer);

		runtime->get_screenshot_width_and_height(&width, &height);
		std::vector<uint8_t> pixels(width * height * 4);

		runtime->capture_screenshot(pixels.data());

		WCHAR file_prefix[MAX_PATH] = L"";
		GetModuleFileNameW(nullptr, file_prefix, ARRAYSIZE(file_prefix));

		std::filesystem::path save_path = file_prefix;
		save_path += L'_';

		const auto now = std::chrono::system_clock::now();
		const auto now_seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);

		char timestamp[21];
		const std::time_t t = std::chrono::system_clock::to_time_t(now_seconds);
		tm tm; localtime_s(&tm, &t);
		sprintf_s(timestamp, "%.4d-%.2d-%.2d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
		save_path += timestamp;
		save_path += L' ';
		sprintf_s(timestamp, "%.2d-%.2d-%.2d", tm.tm_hour, tm.tm_min, tm.tm_sec);
		save_path += timestamp;
		save_path += L' ';
		sprintf_s(timestamp, "%.3lld", std::chrono::duration_cast<std::chrono::milliseconds>(now - now_seconds).count());
		save_path += timestamp;
		save_path += L' ';

		std::filesystem::path save_path_exr = save_path;

		save_path += L"BackBuffer";
		save_path += L".bmp";

		stbi_write_bmp(save_path.u8string().c_str(), width, height, 4, pixels.data());

		save_path_exr += L"DepthBuffer";
		save_path_exr += L".exr";

		saveDS(runtime, save_path_exr);
	}
}

static void draw_settings_overlay(effect_runtime *runtime)
{
	if (undock_window_var) {
		ImGui::Begin("Raw Depth Buffer Export", 0);
	}
	const bool undock_window = ImGui::Button("Toggle Docking", ImVec2(ImGui::GetWindowContentRegionWidth(), 0));
	if (undock_window)
		undock_window_var = !undock_window_var;
	displayDS(runtime);
	if (undock_window_var)
	{
		ImGui::End();
	}
}

void register_addon_RDBE()
{
	reshade::register_overlay(nullptr, draw_settings_overlay);

	reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);

	reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);

	reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_render_effects);
}
void unregister_addon_RDBE()
{
	reshade::unregister_overlay(nullptr, draw_settings_overlay);

	reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
	reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);

	reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
	reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin_render_effects);
}

extern "C" __declspec(dllexport) const char *NAME = "Raw Depth Buffer Export";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Add-on that allow to capture Depth Buffer to 32-bit .exr within the screenshot capture. Press F10 to capture.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		register_addon_RDBE();
		break;
	case DLL_PROCESS_DETACH:
		unregister_addon_RDBE();
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
