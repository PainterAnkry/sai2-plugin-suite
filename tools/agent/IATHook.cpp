#include "IATHook.h"

#include <cstring>

namespace sai2agent {

static bool NameEqualsAscii(const char* wide, const char* ascii) {
    for (const char* p = ascii; *p; ++p, ++wide) {
        // 导入表里的 DLL/函数名大小写随意，按不区分大小写比较
        if (*wide == '\0') return false;
        const char a = *wide;
        const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
        const char lb = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p + 32) : *p;
        if (la != lb) return false;
    }
    return *wide == '\0';
}

void** IATHook::FindIATSlot(const char* dllName, const char* funcName) {
    const HMODULE mainModule = GetModuleHandleW(nullptr);
    if (!mainModule) return nullptr;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(mainModule);
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return nullptr;

    const IMAGE_IMPORT_DESCRIPTOR* desc =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base +
                                                         dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        // 导入表的 DLL 名是 ASCII；按不区分大小写比较
        {
            const char* narrowName =
                reinterpret_cast<const char*>(base + desc->Name);
            size_t i = 0;
            bool eq = true;
            for (const char* p = dllName; *p; ++p, ++i) {
                const char a = narrowName[i];
                if (a == '\0') { eq = false; break; }
                const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
                const char lb = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p + 32) : *p;
                if (la != lb) { eq = false; break; }
            }
            if (!eq || narrowName[i] != '\0') continue;
        }

        const IMAGE_THUNK_DATA* nameThunk =
            reinterpret_cast<const IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addrThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            const_cast<uint8_t*>(base) + desc->FirstThunk);
        if (!desc->OriginalFirstThunk) continue; // bound imports：跳过（罕见）

        for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++addrThunk) {
            if (nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            const IMAGE_IMPORT_BY_NAME* byName =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + static_cast<uintptr_t>(nameThunk->u1.AddressOfData));
            if (NameEqualsAscii(byName->Name, funcName)) {
                return reinterpret_cast<void**>(&addrThunk->u1.Function);
            }
        }
    }
    return nullptr;
}

void* IATHook::Install(const char* dllName, const char* funcName,
                       void* replacement) {
    void** slot = FindIATSlot(dllName, funcName);
    if (!slot) return nullptr;

    void* original = *slot;
    if (original == replacement) return nullptr;

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return nullptr;
    // 8 字节对齐指针的写放在 x64 上是原子的：并发调用只会看到旧或新
    *slot = replacement;
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &tmp);

    replToOriginal_[replacement] = original;
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return original;
}

void* IATHook::Original(void* replacement) const {
    const auto it = replToOriginal_.find(replacement);
    return it == replToOriginal_.end() ? nullptr : it->second;
}

int IATHook::RestoreAll() {
    int restored = 0;
    const HMODULE mainModule = GetModuleHandleW(nullptr);
    if (!mainModule) return 0;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(mainModule);
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return 0;
    const IMAGE_IMPORT_DESCRIPTOR* desc =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base +
                                                         dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        if (!desc->OriginalFirstThunk) continue;
        const IMAGE_THUNK_DATA* nameThunk =
            reinterpret_cast<const IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addrThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            const_cast<uint8_t*>(base) + desc->FirstThunk);
        for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++addrThunk) {
            if (nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            void** slot =
                reinterpret_cast<void**>(&addrThunk->u1.Function);
            // 线性查这个表项当前值是否是我们的 replacement
            for (const auto& kv : replToOriginal_) {
                if (*slot == kv.first) {
                    DWORD oldProtect = 0;
                    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE,
                                       &oldProtect)) {
                        *slot = kv.second;
                        DWORD tmp = 0;
                        VirtualProtect(slot, sizeof(void*), oldProtect, &tmp);
                        FlushInstructionCache(GetCurrentProcess(), slot,
                                              sizeof(void*));
                        ++restored;
                    }
                    break;
                }
            }
        }
    }
    return restored;
}

} // namespace sai2agent
