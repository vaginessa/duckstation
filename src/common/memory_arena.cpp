#include "memory_arena.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
Log_SetChannel(Common::MemoryArena);

#if defined(WIN32)
#include "common/windows_headers.h"
#elif defined(__linux__) || defined(__ANDROID__)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Common {

MemoryArena::MemoryArena() = default;

MemoryArena::~MemoryArena()
{
#if defined(WIN32)
  if (m_file_handle)
    CloseHandle(m_file_handle);
#elif defined(__linux__)
  if (m_shmem_fd > 0)
    close(m_shmem_fd);
#endif
}

void* MemoryArena::FindBaseAddressForMapping(size_t size)
{
  void* base_address;
#if defined(WIN32)
  base_address = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
  if (base_address)
    VirtualFree(base_address, 0, MEM_RELEASE);
#elif defined(__linux__)
  base_address = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (base_address)
    munmap(base_address, size);
#elif defined(__ANDROID__)
  base_address = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_SHARED, -1, 0);
  if (base_address)
    munmap(base_address, size);
#else
  base_address = nullptr;
#endif

  if (!base_address)
  {
    Log_ErrorPrintf("Failed to get base address for memory mapping of size %zu", size);
    return nullptr;
  }

  return base_address;
}

bool MemoryArena::Create(size_t size, bool writable, bool executable)
{
#if defined(WIN32)
  const std::string file_mapping_name =
    StringUtil::StdStringFromFormat("common_memory_arena_%zu_%u", size, GetCurrentProcessId());

  const DWORD protect = (writable ? (executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) : PAGE_READONLY);
  m_file_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, protect, Truncate32(size >> 32), Truncate32(size),
                                     file_mapping_name.c_str());
  if (!m_file_handle)
  {
    Log_ErrorPrintf("CreateFileMapping failed: %u", GetLastError());
    return false;
  }

  return true;
#elif defined(__linux__)
  const std::string file_mapping_name =
    StringUtil::StdStringFromFormat("common_memory_arena_%zu_%u", size, static_cast<unsigned>(getpid()));
  m_shmem_fd = shm_open(file_mapping_name.c_str(), O_CREAT | O_EXCL | (writable ? O_RDWR : O_RDONLY), 0600);
  if (m_shmem_fd < 0)
  {
    Log_ErrorPrintf("shm_open failed: %d", errno);
    return false;
  }

  // we're not going to be opening this mapping in other processes, so remove the file
  shm_unlink(file_mapping_name.c_str());

  // ensure it's the correct size
  if (ftruncate64(m_shmem_fd, static_cast<off64_t>(size)) < 0)
  {
    Log_ErrorPrintf("ftruncate64(%zu) failed: %d", size, errno);
    return false;
  }

  return true;
#else
  return false;
#endif
}

std::optional<MemoryArena::View> MemoryArena::CreateView(size_t offset, size_t size, bool writable, bool executable,
                                                         void* fixed_address)
{
  void* base_pointer = CreateViewPtr(offset, size, writable, executable, fixed_address);
  if (!base_pointer)
    return std::nullopt;

  return View(this, base_pointer, offset, size, writable);
}

void* MemoryArena::CreateViewPtr(size_t offset, size_t size, bool writable, bool executable,
                                 void* fixed_address /*= nullptr*/)
{
  void* base_pointer;
#if defined(WIN32)
  const DWORD desired_access = FILE_MAP_READ | (writable ? FILE_MAP_WRITE : 0) | (executable ? FILE_MAP_EXECUTE : 0);
  base_pointer =
    MapViewOfFileEx(m_file_handle, desired_access, Truncate32(offset >> 32), Truncate32(offset), size, fixed_address);
  if (!base_pointer)
    return nullptr;
#elif defined(__linux__)
  const int flags = (fixed_address != nullptr) ? (MAP_SHARED | MAP_FIXED) : MAP_SHARED;
  const int prot = PROT_READ | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);
  base_pointer = mmap64(fixed_address, size, prot, flags, m_shmem_fd, static_cast<off64_t>(offset));
  if (base_pointer == reinterpret_cast<void*>(-1))
    return nullptr;
#else
  return nullptr;
#endif

  m_num_views.fetch_add(1);
  return base_pointer;
}

bool MemoryArena::FlushViewPtr(void* address, size_t size)
{
#if defined(WIN32)
  return FlushViewOfFile(address, size);
#elif defined(__linux__)
  return (msync(address, size, 0) >= 0);
#else
  return false;
#endif
}

bool MemoryArena::ReleaseViewPtr(void* address, size_t size)
{
  bool result;
#if defined(WIN32)
  result = static_cast<bool>(UnmapViewOfFile(address));
#elif defined(__linux__)
  result = (munmap(address, size) >= 0);
#else
  result = false;
#endif

  if (!result)
  {
    Log_ErrorPrintf("Failed to unmap previously-created view at %p", address);
    return false;
  }

  const size_t prev_count = m_num_views.fetch_sub(1);
  Assert(prev_count > 0);
  return true;
}

bool MemoryArena::SetPageProtection(void* address, size_t length, bool readable, bool writable, bool executable)
{
#if defined(WIN32)
  static constexpr DWORD protection_table[2][2][2] = {
    {{PAGE_NOACCESS, PAGE_EXECUTE}, {PAGE_WRITECOPY, PAGE_EXECUTE_WRITECOPY}},
    {{PAGE_READONLY, PAGE_EXECUTE_READ}, {PAGE_READWRITE, PAGE_EXECUTE_READWRITE}}};

  DWORD old_protect;
  return static_cast<bool>(
    VirtualProtect(address, length, protection_table[readable][writable][executable], &old_protect));
#elif defined(__linux__) || defined(__ANDROID__)
  const int prot = (readable ? PROT_READ : 0) | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);
  return (mprotect(address, length, prot) >= 0);
#else
  return false;
#endif
}

MemoryArena::View::View(MemoryArena* parent, void* base_pointer, size_t arena_offset, size_t mapping_size,
                        bool writable)
  : m_parent(parent), m_base_pointer(base_pointer), m_arena_offset(arena_offset), m_mapping_size(mapping_size),
    m_writable(writable)
{
}

MemoryArena::View::View(View&& view)
  : m_parent(view.m_parent), m_base_pointer(view.m_base_pointer), m_arena_offset(view.m_arena_offset),
    m_mapping_size(view.m_mapping_size)
{
  view.m_parent = nullptr;
  view.m_base_pointer = nullptr;
  view.m_arena_offset = 0;
  view.m_mapping_size = 0;
}

MemoryArena::View::~View()
{
  if (m_parent)
  {
    if (m_writable && !m_parent->FlushViewPtr(m_base_pointer, m_mapping_size))
      Panic("Failed to flush previously-created view");
    if (!m_parent->ReleaseViewPtr(m_base_pointer, m_mapping_size))
      Panic("Failed to unmap previously-created view");
  }
}
} // namespace Common
