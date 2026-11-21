#pragma once

#include <cstdint>
#include <string_view>

namespace pt {

bool DataTraceOn();
void DataTraceFile(std::string_view path);
uint32_t DataTraceFileId(std::string_view file);
void DataTraceProperty(uint32_t file, std::string_view class_name, std::string_view property);

}
