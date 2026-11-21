#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

void pc_options_boot(const std::filesystem::path& dir);
void pc_options_link(uint32_t function, const std::string& path);
uint32_t pc_options_call(uint32_t self, uint32_t function);
void pc_options_returned(uint32_t hook, uint32_t result);
bool pc_options_controller_screen();
uint32_t pc_options_stick_lit();
void pc_options_mouse_scale(float* yaw, float* pitch);
