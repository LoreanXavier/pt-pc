#pragma once
#include <cstdint>
struct Archive {const wchar_t* name;uint64_t size;const char* sha;};
inline constexpr Archive kArchives[]={
{L"chunk1.psarc",421978112ull,"f3cf67ef215065df01619fb0b3fd03fba7f465752ac4107e485dd0ea9dda41ef"},
{L"texture.qar",892291044ull,"436bb79d9d47df685423a9afaed89a8be5e88a938347dd18e63e94398c6ed9b0"},
{L"pathid_list_ps4.bin",93248ull,"6b4641bafe18f785229500454c5d44cba04cc1302fd8e187c53047a04d2633d0"},
};
