#include "guest_flat_memory.h"

#include <mach/mach.h>
#if defined(MKW_PLATFORM_VISIONOS)
// The iOS family's SDK refuses <mach/mach_vm.h>; the vm_map.h entry points
// <mach/mach.h> brings in are the same calls with vm_address_t (64-bit here).
#include <mach/vm_map.h>
#else
#include <mach/mach_vm.h>
#endif
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace GuestFlat {
bool g_requiresCheckedAccess = false;
namespace {
struct Mapping { uint32_t base; uint64_t size; uint8_t* host; };
std::mutex g_mutex;
std::vector<Mapping> g_mappings;
std::vector<RegionRequest> g_layout;
uint8_t* g_base = nullptr;
bool g_active = false;

uint64_t Offset(const RegionRequest& r) {
    if (r.backing == Backing::Mem1) return r.base & 0x1fffffffu;
    if (r.backing == Backing::Mem2) return (r.base & 0x1fffffffu) - 0x10000000u;
    return 0;
}
bool Same(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.base == y.base && x.size == y.size && x.backing == y.backing; });
}
// The store every alias of a region maps: an unlinked temporary file. /tmp is the only writable
// scratch directory on macOS that needs no lookup; the iOS family (visionOS) sandboxes it away
// and names the app's own temporary directory in TMPDIR instead.
int BackingFile(size_t size) {
    std::string name;
#if defined(MKW_PLATFORM_VISIONOS)
    if (const char* tmp = std::getenv("TMPDIR"); tmp && *tmp) {
        name = tmp;
        if (name.back() != '/') name += '/';
    }
    if (name.empty()) name = "./";
#else
    name = "/tmp/";
#endif
    name += "wiicompiled-guest-XXXXXX";
    const int fd = mkstemp(name.data());
    if (fd >= 0) { unlink(name.c_str()); if (ftruncate(fd, static_cast<off_t>(size)) != 0) { close(fd); return -1; } }
    return fd;
}

#if defined(MKW_PLATFORM_VISIONOS)
// What sits in the way of the fixed reservation, and which other bases the
// process could have taken: the region the kernel reports at the fixed base
// and a probe of a few candidate bases (reserved and released at once). For
// the log when the reservation fails, so a change in the system's layout (as
// visionOS 27 brought) can be read off the crash instead of guessed at.
std::string ReserveDiagnostics(kern_return_t failure) {
    std::string out = " [vm_allocate=" + std::to_string(failure);
    vm_address_t probe = kFixedFlatGuestBase;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &probe, &size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "; region at/after base: 0x%llx+0x%llx prot=%d/%d shared=%d",
                      static_cast<unsigned long long>(probe), static_cast<unsigned long long>(size),
                      static_cast<int>(info.protection), static_cast<int>(info.max_protection),
                      static_cast<int>(info.shared));
        out += buffer;
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    } else {
        out += "; no region at or after the base";
    }
    vm_address_t anywhere = 0;
    if (vm_allocate(mach_task_self(), &anywhere, kGuestSpaceSize, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "; anywhere -> 0x%llx", static_cast<unsigned long long>(anywhere));
        out += buffer;
        vm_deallocate(mach_task_self(), anywhere, kGuestSpaceSize);
    }
    // Every region of the map, so the gaps show.
    out += "; map:";
    vm_address_t cursor = 0;
    for (int i = 0; i < 400; ++i) {
        vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t regionInfo{};
        mach_msg_type_number_t regionCount = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t regionObject = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &cursor, &regionSize, VM_REGION_BASIC_INFO_64,
                         reinterpret_cast<vm_region_info_t>(&regionInfo), &regionCount, &regionObject) != KERN_SUCCESS)
            break;
        if (regionObject != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), regionObject);
        // Only what matters for a 4 GiB reservation: regions from 256 MiB up.
        if (regionSize >= 0x1000'0000ull) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), " %llx+%llx/%d", static_cast<unsigned long long>(cursor),
                          static_cast<unsigned long long>(regionSize), static_cast<int>(regionInfo.protection));
            out += buffer;
        }
        cursor += regionSize;
    }
    out += "; free bases:";
    for (uint64_t candidate = 0x1'0000'0000ull; candidate <= 0x200'0000'0000ull; candidate += 0x4'0000'0000ull) {
        vm_address_t address = candidate;
        if (vm_allocate(mach_task_self(), &address, kGuestSpaceSize, VM_FLAGS_FIXED) == KERN_SUCCESS &&
            address == candidate) {
            vm_deallocate(mach_task_self(), address, kGuestSpaceSize);
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), " %lluG", static_cast<unsigned long long>(candidate >> 30));
            out += buffer;
        }
    }
    return out + "]";
}
#endif

std::string ReserveFailureMessage(kern_return_t failure) {
#if defined(MKW_PLATFORM_VISIONOS)
    return "unable to reserve the fixed 4 GiB guest address space at " +
           std::to_string(kFixedFlatGuestBase >> 30) + " GiB; the visionOS app needs the "
           "com.apple.developer.kernel.extended-virtual-addressing entitlement (see visionos/), and the "
           "system's layout may have moved (guest_flat_memory.h)" + ReserveDiagnostics(failure);
#else
    (void)failure;
    return "unable to reserve fixed 4 GiB macOS guest address space";
#endif
}
} // namespace

bool IsActive() { return g_active; }
void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard lock(g_mutex);
    g_requiresCheckedAccess = static_cast<size_t>(getpagesize()) > kGuestPageSize;
    if (g_active) { if (!Same(g_layout, regions)) throw std::runtime_error("flat guest layout cannot be remapped"); return; }
#if defined(MKW_PLATFORM_VISIONOS)
    vm_address_t address = kFixedFlatGuestBase;
    if (const kern_return_t result = vm_allocate(mach_task_self(), &address, kGuestSpaceSize, VM_FLAGS_FIXED);
        result != KERN_SUCCESS || address != kFixedFlatGuestBase)
        throw std::runtime_error(ReserveFailureMessage(result));
#else
    mach_vm_address_t address = kFixedFlatGuestBase;
    if (const kern_return_t result = mach_vm_allocate(mach_task_self(), &address, kGuestSpaceSize, VM_FLAGS_FIXED);
        result != KERN_SUCCESS || address != kFixedFlatGuestBase)
        throw std::runtime_error(ReserveFailureMessage(result));
#endif
    g_base = reinterpret_cast<uint8_t*>(address);
    struct Store { Backing kind; uint32_t owned; uint64_t size; int fd; };
    std::vector<Store> stores;
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto it = std::find_if(stores.begin(), stores.end(), [&](const Store& s) { return s.kind == r.backing && s.owned == owned; });
        const uint64_t need = Offset(r) + r.size;
        if (it == stores.end()) stores.push_back({r.backing, owned, need, -1}); else it->size = std::max(it->size, need);
    }
    for (auto& s : stores) { s.fd = BackingFile(s.size); if (s.fd < 0) throw std::runtime_error("unable to create macOS guest backing store"); }
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        const auto& s = *std::find_if(stores.begin(), stores.end(), [&](const Store& x) { return x.kind == r.backing && x.owned == owned; });
        auto* host = static_cast<uint8_t*>(mmap(nullptr, r.size, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, Offset(r)));
        auto* guest = mmap(g_base + r.base, r.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s.fd, Offset(r));
        if (host == MAP_FAILED || guest != g_base + r.base) throw std::runtime_error("unable to map macOS guest alias");
        g_mappings.push_back({r.base, r.size, host});
    }
    for (auto& s : stores) close(s.fd);
    g_layout = regions; g_active = true;
}
uint8_t* HostPointer(uint32_t a) { for (const auto& m : g_mappings) if (a >= m.base && uint64_t(a - m.base) < m.size) return m.host + (a - m.base); return nullptr; }
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }
} // namespace GuestFlat
