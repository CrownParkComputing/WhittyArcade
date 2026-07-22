#pragma once

#include <string>
#include <vector>

std::vector<std::string> platform_file_selection(bool directory,
                                                 bool multiple,
                                                 const char* title);
